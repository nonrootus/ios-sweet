/* One owned 16x16 RGBA8 target. PM4 from the stock A618, Mesa fd6 and
 * HakiOS's measured A6xx 2D test, with the A618 CCU settings. */
static uint32_t a618_odd_parity(uint32_t value)
{
    value ^= value >> 16;
    value ^= value >> 8;
    value ^= value >> 4;
    return (0x9669u >> (value & 15u)) & 1u;
}

static uint32_t a618_pkt7(uint32_t op, uint32_t count)
{
    return 0x70000000u | (op << 16) | (a618_odd_parity(op) << 23) |
        count | (a618_odd_parity(count) << 15);
}

static uint32_t a618_pkt4(uint32_t reg)
{
    return 0x40000000u | (a618_odd_parity(reg) << 27) | (reg << 8) | 1u;
}

static int a618_fill_submit(persistent_gmu *s, a618_cp_run &run,
    const char *phase, const uint32_t *packet, unsigned count, unsigned &tail,
    unsigned timeout_us = 2000000)
{
    if (tail >= 1024 || count + 3 > 1024 - tail) return -5;
    if (a618_cp_guard(s, run)) {
        IOLog("SWEET A618 RING: %s preflight guard failed\n", phase);
        return -3;
    }
    const uint32_t previous_rptr = a618_cp_read(run,0x806);
    const uint32_t previous_wptr = a618_cp_read(run,0x807);
    const uint32_t previous_shadow = *run.shadow;
    if (previous_rptr != tail || previous_wptr != tail || previous_shadow != tail) {
        IOLog("SWEET A618 RING: %s preflight tail=%u RPTR=%u WPTR=%u shadow=%u\n",
            phase, tail, previous_rptr, previous_wptr, previous_shadow);
        return -3;
    }
    void *cpu = nullptr;
    if (a618_cp_buffer(1, 0, 8, &cpu)) return -5;
    auto *ring = static_cast<uint32_t *>(cpu);
    for (unsigned i = 0; i < count; ++i) ring[tail + i] = packet[i];
    ring[tail + count] = a618_pkt7(0x62, 2); // CP_WHERE_AM_I
    ring[tail + count + 1] = SWEET_A618_SCRATCH_IOVA;
    ring[tail + count + 2] = 0;
    if (sweet_gpu_memory_clean(a618_preparation.storage[1], 0)) return -5;
    if (a618_cp_guard(s, run)) {
        IOLog("SWEET A618 RING: %s publication guard failed\n", phase);
        return -3;
    }
    const unsigned end = tail + count + 3;
    mmio_write32(a618_cp_register(run,0x807), end);
    if (complete(s)) return -3;
    IOLog("SWEET A618 RING: %s WPTR written=%u\n", phase, end);
    constexpr uint32_t busy_off = 0x23b0cu*4-0x6a000u;
    for (unsigned waited = 0; waited <= timeout_us; waited += 1000) {
        if (a618_cp_guard(s, run)) {
            IOLog("SWEET A618 RING: %s polling guard failed at %u us\n", phase, waited);
            return -3;
        }
        uint32_t fence = UINT32_MAX, busy = UINT32_MAX;
        if (post_read(s, a618_fence_status, &fence) || fence ||
            post_read(s, busy_off, &busy) || busy == UINT32_MAX) {
            IOLog("SWEET A618 RING: %s polling fence=%08x busy=%08x at %u us\n", phase, fence, busy, waited);
            return -3;
        }
        uint32_t wptr = a618_cp_read(run,0x807);
        if (wptr != end) {
            IOLog("SWEET A618 RING: %s WPTR read=%u expected=%u at %u us\n", phase, wptr, end, waited);
            return -3;
        }
        uint32_t rptr = a618_cp_read(run,0x806);
        if (complete(s)) return -3;
        uint32_t shadow = *run.shadow;
        if (rptr < tail || rptr > end || shadow < tail || shadow > end) {
            IOLog("SWEET A618 RING: %s pointer range=%u..%u RPTR=%u shadow=%u\n",
                phase, tail, end, rptr, shadow);
            return -5;
        }
        if (rptr == end && shadow == end && !(busy & (1u << 23))) {
            IOLog("SWEET A618 RING: %s complete RPTR=%u WPTR=%u shadow=%u\n", phase, rptr, wptr, shadow);
            tail = end;
            return 0;
        }
        if (waited == timeout_us)
            IOLog("SWEET A618 RING: %s timeout RPTR=%u WPTR=%u shadow=%u busy=%08x\n",
                phase, rptr, wptr, shadow, busy);
        if (waited != timeout_us) IODelay(1000);
    }
    return -4;
}

static int a618_fill_first(persistent_gmu *s, a618_cp_run &run, unsigned *completed_tail = nullptr)
{
    constexpr uint32_t sentinel = 0xa5a5a5a5, pixel = 0x42424242;
    constexpr uint32_t color_fence = 0x13579bdf, cache_fence = 0x2468ace0;
    constexpr uint32_t buffer = SWEET_A618_FILL_IOVA, target = buffer + 0x400;
    constexpr unsigned first_pixel = 0x400 / 4, pixels = 16 * 16;
    unsigned tail = SWEET_A618_CP_BOOT_WORDS;
    if (!run.zap_attempted || a618_cp_read(run,0xf400) != 1) return -3;
    void *cpu = nullptr;
    if (a618_cp_buffer(a618_fill_buffer,0,SWEET_A618_DATA_PAGES,&cpu)) return -5;
    const uint32_t mode[] = {a618_pkt7(0x66,1), 0}; // CP_SET_SECURE_MODE
    IOLog("SWEET A618 FILL: normal mode begin\n");
    int rc = a618_fill_submit(s, run, "mode", mode, sizeof(mode)/sizeof(mode[0]), tail);
    if (rc) return rc;
    uint32_t trust = a618_cp_read(run,0xf400);
    IOLog("SWEET A618 FILL: normal mode rc=0 TRUST=%08x\n", trust);
    if (trust) return -6;

    auto *words = static_cast<volatile uint32_t *>(cpu);
    for (unsigned i = 0; i < 1024; ++i) words[i] = sentinel;
    words[4] = 0;
    words[8] = 0;
    if (sweet_gpu_memory_clean(a618_preparation.storage[a618_fill_buffer],0)) return -5;
    const uint32_t draw[] = {
        a618_pkt7(0x26,0),                  // WFI before changing CCU mode
        a618_pkt4(0x8e07), 0x08000000u,     // A618 sysmem CCU profile
        a618_pkt7(0x65,1), 12,              // RM6_BLIT2DSCALE
        a618_pkt4(0x8405), 0,
        a618_pkt4(0x8406), 0x000f000fu,
        a618_pkt4(0x8c2c), 0x42,
        a618_pkt4(0x8c2d), 0x42,
        a618_pkt4(0x8c2e), 0x42,
        a618_pkt4(0x8c2f), 0x42,
        a618_pkt4(0x8c00), 0x10f03080u,
        a618_pkt4(0x8400), 0x10f03080u,
        a618_pkt4(0xacc0), 0xf180,
        a618_pkt4(0x8c01), 0,
        a618_pkt4(0x8c17), 0x30,            // Linear RGBA8
        a618_pkt4(0x8c18), target,
        a618_pkt4(0x8c19), 0,
        a618_pkt4(0x8c1a), 1,               // Pitch in units of 64 bytes
        a618_pkt7(0x26,0),                  // A618 blitter WFI quirk
        a618_pkt7(0x2c,1), 3,               // CP_BLIT scale
        a618_pkt7(0x26,0),
        a618_pkt7(0x46,4), 0x1d, buffer + 16, 0, color_fence,
        a618_pkt7(0x14,4), 0, buffer + 16, 0, color_fence,
        a618_pkt7(0x46,4), 0x04, buffer + 32, 0, cache_fence,
        a618_pkt7(0x14,4), 0, buffer + 32, 0, cache_fence,
    };
    IOLog("SWEET A618 FILL: 16x16 submit\n");
    rc = a618_fill_submit(s, run, "draw", draw, sizeof(draw)/sizeof(draw[0]), tail);
    if (rc) return rc;
    unsigned matches = 0, guards = 0;
    for (unsigned i = 0; i < 1024; ++i) {
        if (i >= first_pixel && i < first_pixel + pixels) matches += words[i] == pixel;
        else if (i != 4 && i != 8) guards += words[i] != sentinel;
    }
    const uint32_t color = words[4], cache = words[8];
    if (a618_cp_guard(s, run)) {
        IOLog("SWEET A618 RING: draw readback guard failed\n");
        return -3;
    }
    IOLog("SWEET A618 FILL: pixels=%u/256 guards_changed=%u fences=%08x/%08x\n",
        matches, guards, color, cache);
    if (matches != pixels || guards || color != color_fence || cache != cache_fence) return -5;
    if (completed_tail) *completed_tail = tail;
    return 0;
}
