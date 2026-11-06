/* Included by sweet_rpmh.c. Read-only inventory, not a resource vote readback:
 * completed votes may persist in hardware after a TCS slot is cleared.
 * The lock excludes local writers, not firmware/other execution environments.
 * A stable sample is not proof that XNU owns a BCM or may replace its vote. */
int sweet_rpmh_tcs_snapshot(void);
int sweet_rpmh_tcs_snapshot(void)
{
    static unsigned attempted;
    static struct {
        uint32_t control, status, enable, wait;
        uint32_t command[16][3]; /* message ID, address, data */
    } bank[8];
    uint32_t irq_enable, irq_status;
    bool stable = true;
    int error;
    if (__atomic_exchange_n(&attempted, 1, __ATOMIC_ACQ_REL)) return EALREADY;
    if (__atomic_exchange_n(&rh_lock, 1, __ATOMIC_ACQUIRE)) return EBUSY;
    if (rh_poisoned) { error = EIO; goto out; }
    error = rh_map();
    if (error) goto out;
    if (rh_ncpt != 16 || ((rh_read(rh_rsc, 0x0c) >> (6u * RH_DRV)) & 0x3fu) != 8) {
        error = ENODEV; goto out;
    }
    irq_enable = rh_read(rh_tcs, RH_IRQ_EN);
    irq_status = rh_read(rh_tcs, RH_IRQ_STATUS);
    for (unsigned t = 0; t < 8; ++t) {
        uint32_t b = t * RH_TCS_STRIDE;
        bank[t].control = rh_read(rh_tcs, b + RH_CONTROL);
        bank[t].status = rh_read(rh_tcs, b + RH_STATUS);
        bank[t].enable = rh_read(rh_tcs, b + RH_ENABLE);
        bank[t].wait = rh_read(rh_tcs, b + RH_WAIT);
        for (unsigned c = 0; c < 16; ++c) {
            uint32_t off = b + c * RH_CMD_STRIDE;
            bank[t].command[c][0] = rh_read(rh_tcs, off + RH_MSGID);
            bank[t].command[c][1] = rh_read(rh_tcs, off + RH_ADDR);
            bank[t].command[c][2] = rh_read(rh_tcs, off + RH_DATA);
        }
        if (bank[t].control == 0xffffffffu || bank[t].status == 0xffffffffu ||
            (bank[t].enable & ~0xffffu) || (bank[t].wait & ~0xffffu)) stable = false;
    }
    /* Compare every sampled field again before logging; no command slot,
     * status, IRQ enable or W1C register is written. */
    for (unsigned t = 0; t < 8; ++t) {
        uint32_t b = t * RH_TCS_STRIDE;
        if (bank[t].control != rh_read(rh_tcs, b + RH_CONTROL) ||
            bank[t].status != rh_read(rh_tcs, b + RH_STATUS) ||
            bank[t].enable != rh_read(rh_tcs, b + RH_ENABLE) ||
            bank[t].wait != rh_read(rh_tcs, b + RH_WAIT)) stable = false;
        for (unsigned c = 0; c < 16; ++c) {
            uint32_t off = b + c * RH_CMD_STRIDE;
            if (bank[t].command[c][0] != rh_read(rh_tcs, off + RH_MSGID) ||
                bank[t].command[c][1] != rh_read(rh_tcs, off + RH_ADDR) ||
                bank[t].command[c][2] != rh_read(rh_tcs, off + RH_DATA)) stable = false;
        }
    }
    if (irq_enable == 0xffffffffu || irq_status == 0xffffffffu ||
        irq_enable != rh_read(rh_tcs, RH_IRQ_EN) ||
        irq_status != rh_read(rh_tcs, RH_IRQ_STATUS)) stable = false;
    printf("SWEET TCS: begin banks=8 slots=16 irq-enable=%08x irq-status=%08x\n", irq_enable, irq_status);
    for (unsigned t = 0; t < 8; ++t) {
        printf("SWEET TCS: bank=%u kind=%s control=%08x status=%08x enable=%08x wait=%08x\n",
            t, t < 2 ? "active" : t < 5 ? "sleep" : "wake",
            bank[t].control, bank[t].status, bank[t].enable, bank[t].wait);
        for (unsigned c = 0; c < 16; ++c)
            printf("SWEET TCS: slot=%u:%u msg=%08x addr=%08x data=%08x enabled=%u\n",
                t, c, bank[t].command[c][0], bank[t].command[c][1], bank[t].command[c][2],
                (bank[t].enable >> c) & 1u);
    }
    printf("SWEET TCS: end stable=%u; slots do not prove current votes or ownership\n", stable ? 1u : 0u);
    error = stable ? 0 : EBUSY;
out:
    __atomic_store_n(&rh_lock, 0, __ATOMIC_RELEASE);
    return error;
}
