/* Only the signed stock A618 ZAP layout. Qualcomm authenticates the exact
 * bundle bytes. This loader is reached with GPU OOB held and SQE halted. */
#ifdef KERNEL
#include <vm/memory_types.h>

extern "C" int sweet_a618_zap_low_check(void);
extern "C" vm_offset_t ml_io_map_unmappable(vm_offset_t, vm_size_t, uint32_t);
extern "C" void ml_io_unmap(vm_offset_t, vm_size_t);
extern "C" uint64_t kvtophys(vm_offset_t);

static constexpr uint64_t zap_pa = 0xb0000000ull;
static constexpr unsigned zap_span = 0x1000, zap_map = 0x4000;
static sweet_gpu_memory *zap_metadata;

static uint32_t zap_word(const uint8_t *data, unsigned offset)
{
    const uint8_t *p = data + offset;
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
        (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

static bool zap_layout(const uint8_t *mdt, size_t size, size_t segment_size)
{
    if (!mdt || size != 6860 || segment_size != 0x7c0 ||
        zap_word(mdt,0) != 0x464c457f || zap_word(mdt,4) != 0x00010101 ||
        zap_word(mdt,16) != 0x00a40002 || zap_word(mdt,20) != 1 ||
        zap_word(mdt,24) != 0x5000 || zap_word(mdt,28) != 52 ||
        zap_word(mdt,40) != 0x00200034 || zap_word(mdt,44) != 3) return false;
    const uint8_t *header = mdt + 52, *hash = header + 32, *load = hash + 32;
    return zap_word(header,0) == 0 && zap_word(header,16) == 148 &&
        zap_word(hash,0) == 0 && zap_word(hash,16) == size - 148 &&
        ((zap_word(hash,24) >> 24) & 7u) == 2 &&
        zap_word(load,0) == 1 && zap_word(load,4) == 0x3000 &&
        zap_word(load,8) == 0x5000 && zap_word(load,12) == 0x5000 &&
        zap_word(load,16) == segment_size && zap_word(load,20) == segment_size &&
        zap_word(load,24) == 0x08000007 && zap_word(load,28) == 0x100000;
}

static bool zap_cold(persistent_gmu *s, const a618_cp_run &run)
{
    uint32_t fence = 0;
    return !a618_cp_guard(s, run) && !post_read(s, a618_fence_status, &fence) && !fence &&
        a618_cp_read(run, 0xf400) == 1 && !a618_cp_read(run, 0x808) &&
        !a618_cp_read(run, 0x806) && !a618_cp_read(run, 0x807) && !complete(s) && !*run.shadow;
}

static int a618_zap_load(persistent_gmu *s, a618_cp_run &run)
{
    if (!zap_cold(s, run) || sweet_a618_zap_low_check()) return -3;
    constexpr uint32_t init = 0x02000201, memory = 0x02000202, auth = 0x02000205;
    auto self = a618_scm_info_query(a618_smc_64 | a618_smc_fast, a618_scm_info | a618_smc_fast);
    if (self.status || self.available != 1 || !zap_cold(s, run)) return -3;
    const uint32_t calls[] = {init, memory, auth};
    for (uint32_t id : calls) {
        auto reply = a618_scm_info_query(a618_smc_64, id);
        if (reply.status || reply.available != 1 || !zap_cold(s, run)) return -3;
    }
    const void *mdt = nullptr, *segment = nullptr;
    size_t mdt_size = 0, segment_size = 0;
    if (sweet_gpu_firmware_get(2, &mdt, &mdt_size) ||
        sweet_gpu_firmware_get(3, &segment, &segment_size) || !segment ||
        !zap_layout(static_cast<const uint8_t *>(mdt), mdt_size, segment_size)) return -5;

    zap_metadata = sweet_gpu_memory_create_nc(4);
    void *cpu = nullptr, *next = nullptr;
    uint64_t pa = 0, second = 0;
    if (!zap_metadata || sweet_gpu_memory_page(zap_metadata, 0, &cpu, &pa) ||
        sweet_gpu_memory_page(zap_metadata, 1, &next, &second) ||
        second != pa + 4096 || reinterpret_cast<uintptr_t>(next) != reinterpret_cast<uintptr_t>(cpu) + 4096 ||
        !cpu || !pa || (pa & 0x3fff)) return -5;
    auto *dst = static_cast<volatile uint8_t *>(cpu);
    auto *src = static_cast<const uint8_t *>(mdt);
    for (size_t i = 0; i < mdt_size; ++i) dst[i] = src[i];
    if (sweet_gpu_memory_clean(zap_metadata, 0) || !zap_cold(s, run)) return -3;

    run.zap_attempted = true;
    IOLog("SWEET A618 FILL: PAS INIT begin\n");
    auto reply = a618_scm_call(a618_smc_64 | init, 0x82, 13, pa, 0);
    IOLog("SWEET A618 FILL: PAS INIT status=%d result=%llx\n",
        reply.status, static_cast<unsigned long long>(reply.available));
    if (reply.status || reply.available || !zap_cold(s, run)) return -6;
    IOLog("SWEET A618 FILL: PAS MEM_SETUP begin\n");
    reply = a618_scm_call(a618_smc_64 | memory, 3, 13, zap_pa, zap_span);
    IOLog("SWEET A618 FILL: PAS MEM_SETUP status=%d result=%llx\n",
        reply.status, static_cast<unsigned long long>(reply.available));
    if (reply.status || reply.available || !zap_cold(s, run) || sweet_a618_zap_low_check()) return -6;

    /* ml_io_unmap releases an allocation from kernel_map. */
    vm_offset_t va = ml_io_map_unmappable(zap_pa, zap_map, VM_WIMG_WCOMB);
    if (!va) return -5;
    bool ok = kvtophys(va) == zap_pa;
    if (ok) {
        dst = reinterpret_cast<volatile uint8_t *>(va);
        src = static_cast<const uint8_t *>(segment);
        for (size_t i = 0; i < zap_span; ++i) dst[i] = i < segment_size ? src[i] : 0;
        __asm__ volatile("dsb sy" ::: "memory");
        for (size_t i = 0; i < zap_span; ++i)
            if (dst[i] != (i < segment_size ? src[i] : 0)) { ok = false; break; }
    }
    ml_io_unmap(va, zap_map);
    if (!ok || kvtophys(va) || !zap_cold(s, run)) return -5;

    IOLog("SWEET A618 FILL: PAS AUTH begin\n");
    reply = a618_scm_call(a618_smc_64 | auth, 1, 13, 0, 0);
    IOLog("SWEET A618 FILL: PAS AUTH status=%d result=%llx\n",
        reply.status, static_cast<unsigned long long>(reply.available));
    return !reply.status && !reply.available && zap_cold(s, run) ? 0 : -6;
}
#else
static int a618_zap_load(persistent_gmu *, a618_cp_run &) { return -1; }
#endif
