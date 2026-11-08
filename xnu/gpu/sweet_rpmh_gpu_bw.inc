/* First-start APPS-DRV handoff, not a shared-client interconnect driver.
 * Establishes a new stock-derived bandwidth floor and can replace an unknown
 * bootloader vote. It does NOT read/preserve that vote or claim an increase.
 * No withdrawal/retry this boot. Caller keeps CM3 reset and disables idle.
 * Pinned Xiaomi 10c33d40: DDR usecase4, sorted by VCD.
 * CNOC is supplied through the HFI table later, as in stock gmu_start. */
struct sweet_bcm_boot_command {
    const char *name;
    uint32_t address, data, unit, metadata;
};
static const struct sweet_bcm_boot_command sweet_bcm_boot[3] = {
    {"MC0",  0x50000, 0x60000fec, 1770000, 0x00000010},
    {"SH0",  0x5003c, 0x600006be, 2090000, 0x00010020},
    {"ACV",  0x5000c, 0x60000008,       1, 0x000b0000},
};
/* Combined cold boot: floor for GPU DDR and the stock PAS crypto path.
 * Keep the known ABL history floor on SN0/SN11. Slot history is not a
 * live vote readback, so this retains the cold handoff's ownership limit. */
static const struct sweet_bcm_boot_command sweet_bcm_zap[6] = {
    {"MC0",  0x50000, 0x606f22be, 1770000, 0x00000010},
    {"SH0",  0x5003c, 0x602f0eb6, 2090000, 0x00010020},
    {"SN0",  0x50078, 0x213c5d02,  530000, 0x00030010},
    {"SN11", 0x5006c, 0x613c5d02,  530000, 0x00030010},
    {"CE0",  0x500a0, 0x6fffffff,    6802, 0x00060008},
    {"ACV",  0x5000c, 0x60000008,       1, 0x000b0000},
};
static int sweet_bcm_boot_bindings(const struct sweet_bcm_boot_command *commands, unsigned count)
{
    unsigned matches[6] = {0};
    for (unsigned i = 0; i < 8; ++i) {
        uint32_t h = 8 + 16*i;
        unsigned slave = rh_db16(h);
        if (!slave) break;
        uint32_t entries = RH_DB_HEADER + rh_db16(h+2), aux = RH_DB_HEADER + rh_db16(h+4);
        unsigned count = rh_db16(h+6);
        if (slave < 3 || slave > 5 || (entries & 3) ||
            !rh_range(entries, count*RH_DB_ENTRY) || !rh_range(aux,0)) return EINVAL;
        for (unsigned j = 0; j < count; ++j) {
            uint32_t e = entries + j*RH_DB_ENTRY;
            for (unsigned k = 0; k < count; ++k) {
                const struct sweet_bcm_boot_command *cmd = &commands[k];
                uint32_t key = 0;
                for (unsigned n = 0; cmd->name[n]; ++n)
                    key |= (uint32_t)(uint8_t)cmd->name[n] << (8*n);
                if (rh_read(rh_db,e) != key || rh_read(rh_db,e+4)) continue;
                uint32_t a = aux + rh_db16(e+22);
                if (slave != 5 || rh_db16(h+8) != 0x1000 ||
                    rh_read(rh_db,e+16) != cmd->address ||
                    rh_db16(e+20) != 8 || (a & 3) || !rh_range(a,8) ||
                    rh_read(rh_db,a) != cmd->unit ||
                    rh_read(rh_db,a+4) != cmd->metadata) return ENODEV;
                ++matches[k];
            }
        }
    }
    for (unsigned k = 0; k < count; ++k) if (matches[k] != 1) return ENODEV;
    return 0;
}
static bool sweet_bcm_banks_idle(void)
{
    for (unsigned t = 0; t < 8; ++t) if (!rh_free(t)) return false;
    return true;
}

static bool sweet_bcm_zap_history_ok(void)
{
    for (unsigned bank = 0; bank < 8; ++bank) {
        for (unsigned slot = 0; slot < 16; ++slot) {
            uint32_t off = bank * RH_TCS_STRIDE + slot * RH_CMD_STRIDE;
            uint32_t addr = rh_read(rh_tcs, off + RH_ADDR);
            for (unsigned i = 0; i < 6; ++i) {
                if (addr != sweet_bcm_zap[i].address) continue;
                uint32_t old = rh_read(rh_tcs, off + RH_DATA), floor = sweet_bcm_zap[i].data;
                if (!(old & (1u << 29))) break;
                bool decreases = i == 5 ? ((old & 0x0fffffffu) & ~floor) != 0 :
                    ((old >> 14) & 0x3fffu) > ((floor >> 14) & 0x3fffu) ||
                    (old & 0x3fffu) > (floor & 0x3fffu);
                if (decreases) {
                    printf("SWEET A618 FILL: BCM history above floor addr=%08x data=%08x\n", addr, old);
                    return false;
                }
                break;
            }
        }
    }
    return true;
}
int sweet_rpmh_gpu_bw_bootstrap(void);
int sweet_rpmh_gpu_zap_bw_bootstrap(void);
static int sweet_bcm_bootstrap(bool zap)
{
    extern boolean_t idle_enable;
    static unsigned attempted;
    const struct sweet_bcm_boot_command *commands = zap ? sweet_bcm_zap : sweet_bcm_boot;
    const unsigned count = zap ? 6u : 3u;
    const uint32_t mask = zap ? 0x3fu : 0x7u, complete_mask = zap ? 0x3bu : 0x7u, bit = 1u;
    uint32_t irq_enable, status[6] = {0}, irq = 0, idle = 0;
    bool progress = false, done = false;
    int error;
    if (__atomic_exchange_n(&attempted,1,__ATOMIC_ACQ_REL)) return EALREADY;
    if (__atomic_exchange_n(&rh_lock,1,__ATOMIC_ACQUIRE)) return EBUSY;
    if (rh_poisoned || idle_enable) { error = EBUSY; goto out; }
    error = rh_map();
    if (error) goto out;
    if (rh_ncpt != 16 || ((rh_read(rh_rsc,0x0c) >> (6*RH_DRV)) & 0x3f) != 8) {
        error = ENODEV; goto out;
    }
    error = sweet_bcm_boot_bindings(commands, count);
    if (error) goto out;
    /* More restrictive than normal runtime: all active/sleep/wake banks
     * must be empty and idle. No masked address comparisons or BCM reads. */
    irq_enable = rh_read(rh_tcs,RH_IRQ_EN);
    if (irq_enable == 0xffffffffu || !sweet_bcm_banks_idle() || idle_enable ||
        (zap && !sweet_bcm_zap_history_ok())) {
        error = EBUSY; goto out;
    }
    if (!rh_sync(RH_IRQ_EN,irq_enable & ~bit)) {
        rh_poisoned = true; error = EIO; goto out;
    }
    if (!sweet_bcm_banks_idle() || idle_enable) {
        if (!rh_sync(RH_IRQ_EN,(rh_read(rh_tcs,RH_IRQ_EN) & ~bit) | (irq_enable & bit)))
            rh_poisoned = true;
        error = EBUSY; goto out;
    }
    /* TCS0 is reserved under the sole local RPMh writer lock. Every uncertain
     * write/timeout quarantines it, including after successful command ACKs. */
    rh_poisoned = true;
    error = EIO;
    printf("SWEET BCM: begin APPS handoff DDR4; old votes unknown, hold until reboot\n");
    if (zap) printf("SWEET A618 FILL: boot bandwidth commands=%u\n", count);
    for (unsigned c = 0; c < count; ++c) {
        uint32_t off = c*RH_CMD_STRIDE;
        uint32_t msg = 8u | (1u<<16) | ((complete_mask & (1u<<c)) ? (1u<<8) : 0u);
        printf("SWEET BCM: %s addr=%08x data=%08x complete=%u\n",
            commands[c].name, commands[c].address, commands[c].data,
            (complete_mask>>c)&1u);
        if (!rh_sync(off+RH_MSGID,msg) ||
            !rh_sync(off+RH_ADDR,commands[c].address) ||
            !rh_sync(off+RH_DATA,commands[c].data)) goto out;
    }
    if (!rh_sync(RH_WAIT,complete_mask) || !rh_sync(RH_ENABLE,mask) ||
        !rh_sync(RH_CONTROL,0) || !rh_sync(RH_CONTROL,RH_AMC_EN)) goto out;
    rh_write(rh_tcs,RH_CONTROL,RH_AMC_EN|RH_TRIGGER);
    for (unsigned us = 0; us < RH_TIMEOUT_US; ++us) {
        irq = rh_read(rh_tcs,RH_IRQ_STATUS); idle = rh_read(rh_tcs,RH_STATUS);
        bool complete = true;
        for (unsigned c = 0; c < count; ++c) {
            uint32_t needed = RH_ISSUED | ((complete_mask & (1u<<c)) ? RH_COMPLETE : 0u);
            status[c] = rh_read(rh_tcs,RH_CMD_STATUS+c*RH_CMD_STRIDE);
            complete &= status[c] != 0xffffffffu && (status[c] & needed) == needed;
        }
        progress |= idle == 0 || !complete;
        if (idle && idle != 0xffffffffu && irq != 0xffffffffu &&
            ((irq & bit) || progress) && complete) { done = true; break; }
        delay(1);
    }
    printf("SWEET BCM: done=%u irq=%08x idle=%08x status=%08x/%08x/%08x\n",
        done ? 1u : 0u,irq,idle,status[0],status[1],status[2]);
    if (!done) { error = ETIMEDOUT; goto out; }
    if (!rh_sync(RH_CONTROL,RH_AMC_EN) || !rh_sync(RH_CONTROL,0) ||
        !rh_sync(RH_ENABLE,0) || !rh_sync(RH_WAIT,0)) goto out;
    rh_write(rh_tcs,RH_IRQ_CLEAR,bit);
    if ((rh_read(rh_tcs,RH_IRQ_STATUS)&bit) ||
        !rh_sync(RH_IRQ_EN,(rh_read(rh_tcs,RH_IRQ_EN)&~bit)|(irq_enable&bit))) goto out;
    if (!sweet_bcm_banks_idle()) goto out;
    rh_poisoned = false;
    error = 0;
    printf("SWEET BCM: ACK complete; bandwidth/frequency and GMU execution not proven\n");
out:
    printf("SWEET BCM: end rc=%d poisoned=%u\n",error,rh_poisoned ? 1u : 0u);
    __atomic_store_n(&rh_lock,0,__ATOMIC_RELEASE);
    return error;
}

int sweet_rpmh_gpu_bw_bootstrap(void) { return sweet_bcm_bootstrap(false); }
int sweet_rpmh_gpu_zap_bw_bootstrap(void) { return sweet_bcm_bootstrap(true); }
