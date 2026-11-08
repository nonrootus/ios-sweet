/* First bridge to AppleCLCD. GPU addresses are fixed inside the retained
 * private Normal-NC fill page. The panel and IOSurface are CPU addresses. */
static constexpr unsigned a618_present_words = 256;
static constexpr unsigned a618_present_source = 0x100 / 4;
static constexpr unsigned a618_present_destination = 0x800 / 4;
static constexpr unsigned a618_present_fence = 4;
static constexpr uint32_t a618_present_done = 0x1710c0de;
static constexpr uint32_t a618_present_guard = 0xa5a5a5a5;
static_assert(a618_present_source + a618_present_words < a618_present_destination);
static_assert(a618_present_destination + a618_present_words < 1024);

static struct {
    a618_cp_run run;
    unsigned tail;
    unsigned armed;
} a618_present;

static void a618_present_arm(const a618_cp_run &run, unsigned tail)
{
    a618_present.run = run;
    a618_present.tail = tail;
    IOLog("SWEET A618 PRESENT: armed after verified 2D fill\n");
    __atomic_store_n(&a618_present.armed, 1u, __ATOMIC_RELEASE);
}

extern "C" bool sweet_gpu_present_span(const void *source, void *destination, size_t bytes)
{
    if (bytes != a618_present_words * sizeof(uint32_t) || !source || !destination ||
        !__atomic_load_n(&a618_present.armed, __ATOMIC_ACQUIRE)) return false;

    uint32_t expected[a618_present_words];
    memcpy(expected, source, sizeof(expected));
    bool varies = false;
    for (uint32_t word : expected) varies |= word != expected[0];
    if (!varies) return false;
    // A failed or uncertain submission consumes the single attempt as well.
    if (!__sync_bool_compare_and_swap(&a618_present.armed, 1u, 0u)) return false;

    void *cpu = nullptr;
    if (a618_cp_buffer(a618_fill_buffer, 0, SWEET_A618_DATA_PAGES, &cpu)) {
        IOLog("SWEET A618 PRESENT: buffer validation failed; CPU fallback\n");
        return false;
    }
    auto *words = static_cast<volatile uint32_t *>(cpu);
    for (unsigned i = 0; i < 1024; ++i) words[i] = a618_present_guard;
    words[a618_present_fence] = 0;
    for (unsigned i = 0; i < a618_present_words; ++i) {
        words[a618_present_source + i] = expected[i];
        words[a618_present_destination + i] = ~expected[i];
    }
    if (sweet_gpu_memory_clean(a618_preparation.storage[a618_fill_buffer], 0)) {
        IOLog("SWEET A618 PRESENT: buffer publish failed; CPU fallback\n");
        return false;
    }

    constexpr uint32_t base = SWEET_A618_FILL_IOVA;
    constexpr uint32_t fence = base + a618_present_fence * 4;
    const uint32_t packet[] = {
        a618_pkt7(0x75,5), a618_present_words, // CP_MEMCPY: DWORD count, source, destination
        base + a618_present_source * 4, 0, base + a618_present_destination * 4, 0,
        a618_pkt7(0x26,0), // CP_WAIT_FOR_IDLE
        a618_pkt7(0x46,4), 0x04, fence, 0, a618_present_done,
        a618_pkt7(0x14,4), 0, fence, 0, a618_present_done,
    };
    auto &run = a618_present.run;
    IOLog("SWEET A618 PRESENT: IOSurface copy bytes=%zu\n", bytes);
    int rc = a618_fill_submit(&probe, run, "present", packet,
        sizeof(packet)/sizeof(packet[0]), a618_present.tail, 5000);
    if (rc) {
        IOLog("SWEET A618 PRESENT: submission rc=%d; CPU fallback\n", rc);
        return false;
    }

    uint32_t output[a618_present_words];
    unsigned matches = 0, changed = 0;
    for (unsigned i = 0; i < 1024; ++i) {
        const uint32_t word = words[i];
        if (i >= a618_present_destination && i < a618_present_destination + a618_present_words) {
            unsigned index = i - a618_present_destination;
            output[index] = word;
            matches += word == expected[index];
        } else if (i >= a618_present_source && i < a618_present_source + a618_present_words) {
            changed += word != expected[i - a618_present_source];
        } else if (i != a618_present_fence) {
            changed += word != a618_present_guard;
        }
    }
    const uint32_t observed = words[a618_present_fence];
    const bool valid = matches == a618_present_words && !changed && observed == a618_present_done &&
        !a618_cp_guard(&probe, run);
    IOLog("SWEET A618 PRESENT: words=%u/%u changed=%u fence=%08x verified=%u\n",
        matches, a618_present_words, changed, observed, unsigned(valid));
    if (!valid) return false;
    memcpy(destination, output, sizeof(output));
    return true;
}
