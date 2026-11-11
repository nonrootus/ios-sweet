/* Kernel ring writer for the renderer's own packets. All callers hold the
 * GPU submit lock. This is the full 32 KiB ring configured by CP_RB_CNTL.
 * After publishing WPTR any failure must permanently stop submissions. */
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

    if (a618_cp_guard(s, run) || a618_cp_read(run, 0x806) != tail ||
        a618_cp_read(run, 0x807) != tail || *run.shadow != tail)
        return -3;

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
    constexpr uint32_t busy_off = 0x23b0cu * 4 - 0x6a000u;
    auto within = [&](uint32_t pointer) {
        if (pointer >= words)
            return false;
        return wrap ? pointer >= tail || pointer <= end : pointer >= tail && pointer <= end;
    };
    /*
     * A sleep between looks made every submission take about 2 ms however little the GPU had
     * to do. Look again every 10 us for the first 8 ms; then sleep. A timeout stops the GPU
     * until reboot, so the limit stays well above the ~200 ms the old sleeps came to.
     */
    uint64_t pause_ticks = 0, spin_ticks = 0, limit_ticks = 0;
    nanoseconds_to_absolutetime(10000, &pause_ticks);
    nanoseconds_to_absolutetime(8000000, &spin_ticks);
    nanoseconds_to_absolutetime(500000000, &limit_ticks);
    const uint64_t began = mach_absolute_time();
    for (;;) {
        uint32_t status = UINT32_MAX, busy = UINT32_MAX;
        if (a618_cp_guard(s, run) || post_read(s, a618_fence_status, &status) || status ||
            post_read(s, busy_off, &busy) || busy == UINT32_MAX || a618_cp_read(run, 0x807) != end)
            return -3;
        const uint32_t rptr = a618_cp_read(run, 0x806);
        if (complete(s))
            return -3;
        const uint32_t shadow = *run.shadow;
        if (!within(rptr) || !within(shadow))
            return -5;
        if (rptr == end && shadow == end && !(busy & (1u << 23))) {
            if (wrap)
                IOLog("SWEET CA: ring wrap tail=%u end=%u pages=%02x\n", tail, end, dirty);
            tail = end;
            /* Under the submit lock, so plain counters do. */
            static uint64_t waited_ticks, longest_ticks;
            static unsigned waits;
            const uint64_t waited = mach_absolute_time() - began;
            waited_ticks += waited;
            if (waited > longest_ticks)
                longest_ticks = waited;
            if (++waits == 4096) {
                uint64_t mean_ns = 0, longest_ns = 0;
                absolutetime_to_nanoseconds(waited_ticks / waits, &mean_ns);
                absolutetime_to_nanoseconds(longest_ticks, &longest_ns);
                IOLog("SWEET CA: GPU wait mean %llu us longest %llu us over %u submissions\n",
                    mean_ns / 1000, longest_ns / 1000, waits);
                waited_ticks = longest_ticks = 0;
                waits = 0;
            }
            return 0;
        }
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
