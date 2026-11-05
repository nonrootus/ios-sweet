/* Opt-in CPU-side allocation diagnostic; never hands an address to a device. */
#include "sweet_gpu_memory.h"
#include "sweet_gpu_hfi_memory.h"
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOLib.h>
#include <kern/task.h>
#include <mach/vm_param.h>
extern "C" {
#include <arm/machine_routines.h>
#include <vm/pmap.h>
#include <vm/memory_types.h>
#include <pexpert/pexpert.h>
/* Internal XNU interfaces omitted from the IOKit exported header view.
 * Signatures match osfmk/arm/pmap/pmap.h and osfmk/vm/pmap.h. */
extern vm_map_address_t phystokv(uint64_t pa);
extern unsigned int pmap_cache_attributes(ppnum_t pn);
}

/* AT reads translation attributes without loading the target cache line.
 * Keep PAR_EL1 per-CPU scratch private, restoring it before interrupts return. */
static uint64_t translate(uintptr_t va)
{
    boolean_t enabled = ml_set_interrupts_enabled(FALSE);
    uint64_t saved, result;
    __asm__ volatile("mrs %0, par_el1" : "=r"(saved));
    __asm__ volatile("at s1e1r, %0\nisb\nmrs %1, par_el1"
        : "+r"(va), "=r"(result) :: "memory");
    __asm__ volatile("msr par_el1, %0\nisb" :: "r"(saved) : "memory");
    ml_set_interrupts_enabled(enabled);
    return result;
}

static bool mapping_ok(const char *kind, unsigned index, void *cpu,
    uint64_t pa, uint8_t attribute)
{
    const uint64_t mask = UINT64_C(0x0000fffffffff000);
    uint64_t par = translate(reinterpret_cast<uintptr_t>(cpu));
    /* Attribute-only observation of the physical alias, no data read. */
    uint64_t alias = translate(phystokv(pa));
    unsigned wimg = pmap_cache_attributes(static_cast<ppnum_t>(pa >> PAGE_SHIFT));
    bool ok = !(par & 1) && (par & mask) == (pa & mask) &&
        static_cast<uint8_t>(par >> 56) == attribute &&
        (wimg & VM_WIMG_MASK) == (attribute == 0x44 ? VM_WIMG_WCOMB : VM_WIMG_DEFAULT);
    IOLog("SWEET GPU MEM: %s[%u] pa=%llx par=%llx alias=%llx wimg=%x ok=%u\n",
        kind, index, (unsigned long long)pa, (unsigned long long)par,
        (unsigned long long)alias, wimg, ok ? 1u : 0u);
    return ok;
}

static bool allocation_cycle(unsigned cycle)
{
    auto *tables = sweet_gpu_memory_create(3);
    if (!tables) { IOLog("SWEET GPU MEM: cached allocation failed\n"); return false; }
    auto *queues = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,
        kIODirectionInOut | kIOMemoryMapperNone | kIOMapWriteCombineCache,
        5 * 4096, page_size);
    bool ok = queues != nullptr;
    void *cpu = queues ? queues->getBytesNoCopy() : nullptr;
    ok = ok && cpu && !(reinterpret_cast<uintptr_t>(cpu) & (page_size - 1));
    uint64_t table_pa[3] = {}, queue_pa[5] = {};
    for (unsigned i = 0; ok && i < 3; ++i) {
        void *table_cpu = nullptr;
        ok = !sweet_gpu_memory_page(tables, i, &table_cpu, &table_pa[i]) &&
            mapping_ok("cached", i, table_cpu, table_pa[i], 0xff);
    }
    for (unsigned i = 0; ok && i < 5; ++i) {
        IOByteCount length = 0;
        queue_pa[i] = queues->getPhysicalSegment(i * 4096, &length, kIOMemoryMapperNone);
        ok = queue_pa[i] && !(queue_pa[i] & 4095) &&
            queue_pa[i] < (UINT64_C(1) << 36) && length >= 4096;
        for (unsigned j = 0; ok && j < 3; ++j)
            ok = (queue_pa[i] & ~(uint64_t)(page_size - 1)) !=
                (table_pa[j] & ~(uint64_t)(page_size - 1));
        for (unsigned j = 0; ok && j < i; ++j) ok = queue_pa[i] != queue_pa[j];
        if (ok) ok = mapping_ok("normal-nc", i,
            static_cast<unsigned char *>(cpu) + i * 4096, queue_pa[i], 0x44);
    }
    if (ok) {
        /* Canonical NC VA only. This tests CPU access, never DMA coherency. */
        auto *words = static_cast<volatile uint32_t *>(cpu);
        for (unsigned i = 0; i < 5 * 1024; ++i) words[i] = 0x6d750000u ^ i ^ cycle;
        __asm__ volatile("dsb sy" ::: "memory");
        for (unsigned i = 0; i < 5 * 1024; ++i)
            if (words[i] != (0x6d750000u ^ i ^ cycle)) { ok = false; break; }
        for (unsigned i = 0; i < 5 * 1024; ++i) words[i] = 0;
        __asm__ volatile("dsb sy" ::: "memory");
    }
    /* These allocations have never been published for DMA. */
    if (queues) queues->release();
    sweet_gpu_memory_destroy(tables);
    IOLog("SWEET GPU MEM: cycle=%u allocation/access/release=%s\n", cycle, ok ? "pass" : "fail");
    return ok;
}

extern "C" void sweet_gpu_memory_probe(void);
void sweet_gpu_memory_probe(void)
{
    int enabled = 0, k6a = 0;
    if (!PE_parse_boot_argn("sweetgpumemory", &enabled, sizeof(enabled)) || enabled != 1 ||
        !PE_parse_boot_argn("sweetk6a", &k6a, sizeof(k6a)) || k6a != 1) return;
    uint64_t mair = 0;
    __asm__ volatile("mrs %0, mair_el1" : "=r"(mair));
    IOLog("SWEET GPU MEM: begin page=%lu MAIR=%llx CPU-only; no device DMA\n",
        (unsigned long)page_size, (unsigned long long)mair);
    if (page_size != 16384) { IOLog("SWEET GPU MEM: unexpected native page size\n"); return; }
    /* Exercise the real HFI composition separately, still cached/inactive. */
    auto *hfi = sweet_gpu_hfi_memory_create(0x60010000u);
    uint64_t root = 0; uint32_t iova = 0;
    bool ok = hfi && !sweet_gpu_hfi_memory_info(hfi, &root, &iova);
    IOLog("SWEET GPU MEM: inactive HFI=%s root=%llx iova=%x\n",
        ok ? "pass" : "fail", (unsigned long long)root, iova);
    sweet_gpu_hfi_memory_destroy(hfi);
    for (unsigned cycle = 0; ok && cycle < 2; ++cycle) ok = allocation_cycle(cycle);
    IOLog("SWEET GPU MEM: done result=%s; GPU remains unstarted\n", ok ? "pass" : "fail");
}
