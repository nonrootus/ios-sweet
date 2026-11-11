/* Kernel ring writer for the renderer's own packets. All callers hold the
 * GPU submit lock. This is the full 32 KiB ring configured by CP_RB_CNTL.
 * It returns once WPTR is published (0215: the GPU works on while the CPU goes
 * on). After publishing WPTR any failure must permanently stop submissions. */
#include <mach/mach_time.h>

static int a618_render_ring(persistent_gmu *s, const a618_cp_run &run,
    const uint32_t *packet, unsigned count, unsigned &tail)
{
    constexpr unsigned pages = 8, page_words = 1024, words = pages * page_words;
    constexpr unsigned last_tail = words - 2; // Stock KGSL leaves room for CP_NOP.
    constexpr unsigned suffix_words = 3;
    if (!run.shadow || !packet || !count || count > last_tail - suffix_words || tail > last_tail)
        return -5;
    const unsigned size = count + suffix_words;
    const bool wrap = size > last_tail - tail;
    if (wrap && size >= tail)
        return -5;
    const unsigned start = wrap ? 0 : tail;
    const unsigned end = start + size;

    if (a618_cp_guard(s, run) || a618_cp_read(run, 0x807) != tail)
        return -3;
    /*
     * Asynchronous (0215): earlier submissions may still be running. Wait until the CP has read past
     * every word about to be written: with RPTR at or behind tail the free part is [tail, end of ring)
     * and [0, RPTR); with RPTR ahead of tail (it is still in the previous lap) only [tail, RPTR). WPTR
     * must not become RPTR (that would read as an empty ring). Look every 10 us for 8 ms, then sleep.
     */
    {
        uint64_t pause_ticks = 0, spin_ticks = 0, limit_ticks = 0;
        nanoseconds_to_absolutetime(10000, &pause_ticks);
        nanoseconds_to_absolutetime(8000000, &spin_ticks);
        nanoseconds_to_absolutetime(500000000, &limit_ticks);
        const uint64_t began = mach_absolute_time();
        for (;;) {
            const uint32_t rptr = a618_cp_read(run, 0x806);
            if (rptr >= words)
                return -5;
            const bool room = wrap ? rptr <= tail && end < rptr : rptr <= tail || end < rptr;
            if (room)
                break;
            if (a618_cp_guard(s, run))
                return -3;
            const uint64_t now = mach_absolute_time();
            if (now - began >= limit_ticks)
                return -4;
            if (now - began >= spin_ticks) {
                IOSleep(1);
            } else {
                while (mach_absolute_time() - now < pause_ticks)
                    __asm__ volatile("yield");
            }
        }
    }

    unsigned dirty = 0;
    for (unsigned page = start / page_words; page <= (end - 1) / page_words; ++page)
        dirty |= 1u << page;
    if (wrap)
        dirty |= 1u << (tail / page_words);

    uint32_t *page_cpu[pages] = {};
    for (unsigned page = 0; page < pages; ++page) {
        if (!(dirty & (1u << page)))
            continue;
        void *cpu = nullptr;
        if (a618_cp_buffer(1, page, 8 + page, &cpu))
            return -5;
        page_cpu[page] = static_cast<uint32_t *>(cpu);
    }

    auto store = [&](unsigned index, uint32_t value) {
        page_cpu[index / page_words][index % page_words] = value;
    };
    if (wrap)
        store(tail, a618_pkt7(0x10, words - tail - 1)); // CP_NOP; remaining words are ignored.
    for (unsigned i = 0; i < count; ++i)
        store(start + i, packet[i]);
    store(start + count, a618_pkt7(0x62, 2)); // CP_WHERE_AM_I
    store(start + count + 1, SWEET_A618_SCRATCH_IOVA);
    store(start + count + 2, 0);

    for (unsigned page = 0; page < pages; ++page) {
        if ((dirty & (1u << page)) && sweet_gpu_memory_clean(a618_preparation.storage[1], page))
            return -5;
    }
    if (a618_cp_guard(s, run))
        return -3;

    mmio_write32(a618_cp_register(run, 0x807), end);
    if (complete(s))
        return -3;
    if (wrap)
        IOLog("SWEET CA: ring wrap tail=%u end=%u pages=%02x\n", tail, end, dirty);
    tail = end;
    return 0;
}
