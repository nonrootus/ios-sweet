/* First submission on sweet_k6a/A618 only. GPU-relative offsets are DWORDs
 * from Xiaomi 10c33d40 a6xx_reg.h. Keep the dedicated GPU OOB vote through
 * this function. The caller must retain it and all memory if SQE is released. */
struct a618_cp_run {
    uintptr_t extended;
    volatile uint32_t *shadow;
    bool sqe_attempted;
    bool tsb_preconfigured;
    unsigned writes, waited_us;
    uint32_t rptr, wptr, shadow_rptr, busy, fence;
};

static uintptr_t a618_cp_register(const a618_cp_run &run, unsigned dword)
{
    unsigned bytes = dword * 4;
    return bytes < a618_register_size ? a618_preparation.gpu + bytes :
        run.extended + bytes - a618_register_size;
}

static uint32_t a618_cp_read(const a618_cp_run &run, unsigned dword)
{
    return mmio_read(a618_cp_register(run, dword));
}

static int a618_cp_guard(persistent_gmu *s, const a618_cp_run &run)
{
    if (a618_gpu_oob_health(s, 0, nullptr)) return -3;
    /* The regular status may be masked. The GMU mirror exposes raw GPU IRQs.
     * Only stock A6xx error bits are fatal; idle/CP completion are events. */
    constexpr uint32_t error_irq = (1u<<1)|(1u<<6)|(1u<<7)|(1u<<9)|
        (1u<<22)|(1u<<23)|(1u<<24)|(1u<<25)|(1u<<28);
    constexpr uint32_t cp_error = (1u<<0)|(1u<<1)|(1u<<2)|
        (1u<<4)|(1u<<5)|(1u<<6)|(1u<<7);
    uint32_t irq = a618_cp_read(run, 0x201);     // RBBM_INT_0_STATUS
    uint32_t fault = a618_cp_read(run, 0x821);   // CP_HW_FAULT
    uint32_t protect = a618_cp_read(run, 0x824); // CP_PROTECT_STATUS
    uint32_t cp_irq = a618_cp_read(run, 0x823);  // CP_INTERRUPT_STATUS
    uint32_t unmasked = 0;
    constexpr uint32_t raw_irq_off = 0x23b15u*4-0x6a000u;
    if (post_read(s, raw_irq_off, &unmasked)) return -2;
    if (irq == UINT32_MAX || fault == UINT32_MAX || protect == UINT32_MAX ||
        cp_irq == UINT32_MAX || unmasked == UINT32_MAX ||
        ((irq | unmasked) & error_irq) || (cp_irq & cp_error) || fault || protect) {
        IOLog("SWEET A618 CP: fault IRQ=%08x RAW=%08x CPIRQ=%08x HW=%08x PROTECT=%08x\n",
            irq, unmasked, cp_irq, fault, protect);
        return -3;
    }
    return 0;
}

static int a618_cp_set(persistent_gmu *s, a618_cp_run &run, unsigned dword, uint32_t value)
{
    if (a618_cp_guard(s, run)) return -3;
    ++run.writes;
    mmio_write32(a618_cp_register(run, dword), value);
    if (complete(s)) return -2;
    uint32_t readback = a618_cp_read(run, dword);
    if (readback == value) return 0;
    IOLog("SWEET A618 CP: readback DWORD=%05x expected=%08x actual=%08x\n", dword, value, readback);
    return -5;
}

static int a618_cp_preflight(persistent_gmu *s, a618_cp_run &run)
{
    constexpr unsigned gbif_halt = 0x3c45, gbif_ack = 0x3c46;
    constexpr uint32_t cx_ack_mask = 3;
    constexpr uint32_t unavailable = 0xdeafbead;
    if (a618_cp_guard(s, run)) return -3;
    uint32_t halt = a618_cp_read(run, gbif_halt), halt_ack = a618_cp_read(run, gbif_ack);
    /* Stock adreno_clear_pending_transactions uses GPR0_CNTL and
     * VBIF_GX_RESET_STATUS for the A615 family, which includes A618. */
    uint32_t gx_halt = a618_cp_read(run, 0x18), gx_ack = a618_cp_read(run, 0x215);
    uint32_t trust = a618_cp_read(run, 0xf400), tsb_lo = a618_cp_read(run, 0xf800);
    uint32_t tsb_hi = a618_cp_read(run, 0xf801), tsb_size = a618_cp_read(run, 0xf802);
    uint32_t tsb_ctl = a618_cp_read(run, 0xf803);
    IOLog("SWEET A618 CP: preflight GBIF=%08x/%08x GX=%08x/%08x TRUST=%08x TSB=%08x%08x/%08x/%08x\n",
        halt, halt_ack, gx_halt, gx_ack, trust, tsb_hi, tsb_lo, tsb_size, tsb_ctl);
    if (halt || (halt_ack & ~cx_ack_mask) || gx_halt == unavailable || gx_ack == unavailable ||
        (gx_halt & 0x1e0u) || (gx_ack & 0xf0u) || trust == UINT32_MAX || trust == unavailable ||
        tsb_ctl || tsb_hi)
        return -3;
    /* Stock KGSL 64-bit profile: secure end 0xfc000000, default size 256MiB
     * (no qcom,secure-size in captured FDT). Only empty or exact stock TSB
     * is accepted. Never write TRUST_CNTL or move an existing window. */
    if ((tsb_lo || tsb_size) && (tsb_lo != 0xec000000u || tsb_size != 0x10000000u))
        return -3;
    run.tsb_preconfigured = tsb_lo == 0xec000000u && tsb_size == 0x10000000u;
    static const unsigned reset_registers[] = {0x800,0x801,0x802,0x804,0x805,0x806,0x807,0x808,0x830,0x831,0x842,0x84f};
    for (unsigned reg : reset_registers)
        if (a618_cp_read(run, reg)) return -3;
    for (unsigned slot = SWEET_A618_PROTECT_COUNT; slot < SWEET_A618_PROTECT_SLOTS; ++slot)
        if (a618_cp_read(run, SWEET_A618_PROTECT_FIRST_SLOT+slot)) return -3;
    if (halt_ack) {
        /* Stock startup deasserts CX GBIF_HALT. When a request is already
         * clear but its acknowledgement remains, issue only that zero write
         * and require deassertion before any CP configuration. Stock does not
         * poll the ACK. Here a persistent ACK leaves bus readiness unproven. */
        if (a618_cp_guard(s, run) || a618_cp_read(run, gbif_halt)) return -3;
        int rc = a618_cp_set(s, run, gbif_halt, 0);
        if (rc) return rc;
        for (unsigned waited = 0;; waited += 100) {
            if (a618_cp_guard(s, run) || a618_cp_read(run, gbif_halt)) return -3;
            uint32_t ack = a618_cp_read(run, gbif_ack);
            if (ack & ~cx_ack_mask) return -3;
            if (!ack || waited == 100000) {
                IOLog("SWEET A618 BUS: CX halt ack %08x -> %08x in %uus\n", halt_ack, ack, waited);
                return ack ? -4 : 0;
            }
            IODelay(100);
        }
    }
    return 0;
}

static int a618_cp_configure(persistent_gmu *s, a618_cp_run &run)
{
    /* _set_secvid, a6xx_enable_64bit, and the A618 paths of a6xx_start.
     * FDT HBB=14, MAL=32, UBWC=2, no macrotiling or two-pass WFI quirk.
     * Stock GMEM size 512KiB, with a 1MiB aligned UCHE GMEM base. */
    static const struct { unsigned reg; uint32_t value; } setup[] = {
        {0x842,1}, {0xc01,1}, {0x8601,1}, {0x8e05,1}, {0x9e01,1}, {0xbe05,1},
        {0xa601,1}, {0x9601,1}, {0xe00,1}, {0xae01,1}, {0xb601,1}, {0xf810,1},
        {0x10,3}, {0x50b,0xffffffff}, {0xe05,0xffffffc0}, {0xe06,0x1ffff},
        {0xe09,0xfffff000}, {0xe0a,0x1ffff}, {0xe07,0xfffff000}, {0xe08,0x1ffff},
        {0xe0b,0x100000}, {0xe0c,0}, {0xe0d,0x17ffff}, {0xe0e,0}, {0xe18,0x804}, {0xe17,4},
        {0x8c2,0x010000c0}, {0x8c1,0x8040362c}, {0x8c3,128}, {0x9e00,0x00180000},
        {0x98d,1}, {0x500,1}, {0x8e08,2}, {0xb604,2}, {0xae02,2}, {0xe01,0x00200000},
        {0x1f,0x40cfffff}, {0xe19,1}, {SWEET_A618_PROTECT_CONTROL,SWEET_A618_PROTECT_ENABLE},
    };
    if (!run.tsb_preconfigured) {
        static const struct { unsigned reg; uint32_t value; } tsb[] = {
            {0xf803,0}, {0xf800,0xec000000}, {0xf801,0}, {0xf802,0x10000000},
        };
        for (const auto &item : tsb) {
            int rc = a618_cp_set(s, run, item.reg, item.value);
            if (rc) return rc;
        }
    }
    for (const auto &item : setup) {
        int rc = a618_cp_set(s, run, item.reg, item.value);
        if (rc) return rc;
    }
    for (unsigned i = 0; i < SWEET_A618_PROTECT_COUNT; ++i) {
        int rc = a618_cp_set(s, run, SWEET_A618_PROTECT_FIRST_SLOT+i, sweet_a618_protect_values[i]);
        if (rc) return rc;
    }
    return 0;
}

static int a618_cp_buffer(unsigned buffer, unsigned page, unsigned pa_index, void **cpu)
{
    uint64_t pa = 0;
    if (sweet_gpu_memory_page(a618_preparation.storage[buffer], page, cpu, &pa) || !*cpu ||
        (reinterpret_cast<uintptr_t>(*cpu) & 4095) || pa != a618_preparation.physical[pa_index])
        return -5;
    return 0;
}

static int a618_cp_publish(persistent_gmu *s, a618_cp_run &run)
{
    uint32_t values[SWEET_A618_PWRUP_REGISTERS], packet[SWEET_A618_CP_BOOT_WORDS];
    for (unsigned i = 0; i < SWEET_A618_PWRUP_REGISTERS; ++i) {
        uint32_t reg;
        if (sweet_a618_pwrup_dword_offset(i, &reg) || reg >= 0x10000 || a618_cp_guard(s, run)) return -3;
        values[i] = a618_cp_read(run, reg);
        if (values[i] == UINT32_MAX) return -2;
    }
    void *pwrup = nullptr, *scratch = nullptr, *ring[8] = {};
    if (a618_cp_buffer(3,0,17,&pwrup) || a618_cp_buffer(2,0,16,&scratch) ||
        sweet_a618_cp_boot_encode(packet,SWEET_A618_CP_BOOT_WORDS,SWEET_A618_PWRUP_IOVA,SWEET_A618_SCRATCH_IOVA))
        return -5;
    for (unsigned i = 0; i < 8; ++i)
        if (a618_cp_buffer(1,i,8+i,&ring[i])) return -5;
    if (sweet_a618_pwrup_build(pwrup,4096,values,SWEET_A618_PWRUP_REGISTERS)) return -5;
    run.shadow = static_cast<volatile uint32_t *>(scratch);
    *run.shadow = 0;
    for (unsigned page = 0; page < 8; ++page) {
        auto *words = static_cast<uint32_t *>(ring[page]);
        for (unsigned i = 0; i < 1024; ++i)
            words[i] = page == 0 && i < SWEET_A618_CP_BOOT_WORDS ? packet[i] : 0xaaaaaaaau;
        if (sweet_gpu_memory_clean(a618_preparation.storage[1],page)) return -5;
    }
    if (sweet_gpu_memory_clean(a618_preparation.storage[2],0) ||
        sweet_gpu_memory_clean(a618_preparation.storage[3],0) || complete(s)) return -5;
    return sweet_a618_address_space_check(a618_mapping.tables,a618_preparation.physical) ? -5 : 0;
}

static int a618_cp_first(persistent_gmu *s, a618_cp_run &run)
{
    constexpr unsigned extended_size = 0x40000 - a618_register_size;
    run.extended = ml_io_map(a618_register_base+a618_register_size,extended_size);
    if (!run.extended) return -2;
    int rc = a618_cp_preflight(s,run);
    if (rc) return rc;
    rc = a618_cp_configure(s,run);
    if (rc) return rc;
    rc = a618_cp_publish(s,run);
    if (rc) return rc;
    static const struct { unsigned reg; uint32_t value; } ring_setup[] = {
        {0x804,SWEET_A618_SCRATCH_IOVA},{0x805,0},{0x802,0x0800020c},
        {0x800,SWEET_A618_RING_IOVA},{0x801,0},{0x830,SWEET_A618_SQE_IOVA},{0x831,0},
    };
    for (const auto &item : ring_setup) {
        rc = a618_cp_set(s,run,item.reg,item.value);
        if (rc) return rc;
    }
    // GMU-relative byte offsets of A6XX_GPU_GMU_AO_GPU_CX_BUSY_STATUS and AHB_FENCE_STATUS.
    constexpr uint32_t busy_off = 0x23b0cu*4-0x6a000u;
    constexpr uint32_t fence_off = 0x23b13u*4-0x6a000u;
    if (a618_cp_guard(s,run) || post_read(s,fence_off,&run.fence) ||
        run.fence == UINT32_MAX || (run.fence & 1u) ||
        a618_cp_read(run,0x806) || a618_cp_read(run,0x807) || *run.shadow) return -3;
    IOLog("SWEET A618 CP: armed words=%u; releasing SQE with GPU OOB held\n",SWEET_A618_CP_BOOT_WORDS);
    run.sqe_attempted = true;
    mmio_write32(a618_cp_register(run,0x808),1);
    if (complete(s) || a618_cp_guard(s,run)) return -3;
    mmio_write32(a618_cp_register(run,0x807),SWEET_A618_CP_BOOT_WORDS);
    if (complete(s) || post_read(s,fence_off,&run.fence) ||
        run.fence == UINT32_MAX || (run.fence & 1u)) return -2;

    for (;;) {
        if (a618_cp_guard(s,run) || post_read(s,busy_off,&run.busy) || run.busy == UINT32_MAX) return -3;
        run.rptr = a618_cp_read(run,0x806);
        run.wptr = a618_cp_read(run,0x807);
        if (complete(s)) return -2;
        run.shadow_rptr = *run.shadow; // Normal-NC canonical pointer, no cached alias.
        if (run.wptr != SWEET_A618_CP_BOOT_WORDS || run.rptr > SWEET_A618_CP_BOOT_WORDS ||
            run.shadow_rptr > SWEET_A618_CP_BOOT_WORDS) return -5;
        if (run.rptr == SWEET_A618_CP_BOOT_WORDS && run.shadow_rptr == SWEET_A618_CP_BOOT_WORDS &&
            !(run.busy & (1u<<23))) return a618_cp_guard(s,run);
        if (run.waited_us == 2000000) return -4;
        IODelay(1000);
        run.waited_us += 1000;
    }
}
