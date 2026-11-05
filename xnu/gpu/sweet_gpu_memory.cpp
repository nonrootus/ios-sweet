#include "sweet_gpu_memory.h"
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOLib.h>
#include <kern/task.h>
#include <mach/vm_param.h>
#include <libkern/libkern.h>
extern "C" {
#include <arm/caches_internal.h>
#include <arm/machine_routines.h>
#include <vm/memory_types.h>
extern unsigned int pmap_cache_attributes(ppnum_t pn);
}

#if defined(__ARM_COHERENT_IO__) || defined(APPLE_ARM64_ARCH_FAMILY)
#error This adapter requires the SWEET non-coherent cache maintenance implementation
#endif

static bool nc_mapping_valid(void *cpu, uint64_t pa)
{
    uint64_t saved, par;
    uintptr_t va = reinterpret_cast<uintptr_t>(cpu);
    boolean_t enabled = ml_set_interrupts_enabled(FALSE);
    __asm__ volatile("mrs %0, par_el1" : "=r"(saved));
    __asm__ volatile("at s1e1r, %0\nisb\nmrs %1, par_el1"
        : "+r"(va), "=r"(par) :: "memory");
    __asm__ volatile("msr par_el1, %0\nisb" :: "r"(saved) : "memory");
    ml_set_interrupts_enabled(enabled);
    unsigned wimg = pmap_cache_attributes(static_cast<ppnum_t>(pa >> PAGE_SHIFT));
    bool ok = !(par & 1) && ((par ^ pa) & UINT64_C(0x0000fffffffff000)) == 0 &&
        (par >> 56) == 0x44 &&
        (wimg & VM_WIMG_MASK) == VM_WIMG_WCOMB;
    if (!ok) IOLog("SWEET GPU MEM: NC validation failed pa=%llx par=%llx wimg=%x\n",
        (unsigned long long)pa, (unsigned long long)par, wimg);
    return ok;
}

#define GPU_PAGE 4096u
#define GPU_MAX_PAGES 64u
#define GPU_PA_LIMIT (UINT64_C(1) << 36)

struct sweet_gpu_memory {
    IOBufferMemoryDescriptor *buffer;
    void *cpu;
    size_t pages;
    uint64_t pa[GPU_MAX_PAGES];
    bool nc;
    bool initialized;
};

void sweet_gpu_memory_destroy(struct sweet_gpu_memory *m)
{
    if (!m) return;
    if (m->nc && m->initialized) {
        /* Never DMA-visible yet. The canonical NC VA is the only data access.
         * Remove cached alias lines before the pages return to the allocator. */
        __asm__ volatile ("dsb sy" ::: "memory");
        for (size_t i = 0; i < m->pages; i += page_size / GPU_PAGE)
            flush_dcache(static_cast<vm_offset_t>(m->pa[i]), (unsigned)page_size, TRUE);
        __asm__ volatile ("dsb sy" ::: "memory");
    }
    if (m->buffer) m->buffer->release();
    IOFree(m, sizeof(*m));
}

static sweet_gpu_memory *create_memory(size_t pages, bool nc)
{
    if (!pages || pages > GPU_MAX_PAGES || page_size != 16384) return nullptr;
    auto *m = static_cast<sweet_gpu_memory *>(IOMallocZero(sizeof(sweet_gpu_memory)));
    if (!m) return nullptr;
    m->nc = nc;
    /* No pageable/purgeable or implicit IOMMU mapping. Kernel default is wired.
     * Contiguous CPU storage does not imply contiguous physical pages. */
    m->buffer = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,
        kIODirectionInOut | kIOMemoryMapperNone | (nc ? kIOMapWriteCombineCache : 0),
        pages * GPU_PAGE, page_size);
    if (!m->buffer) { sweet_gpu_memory_destroy(m); return nullptr; }
    m->cpu = m->buffer->getBytesNoCopy();
    if (!m->cpu || (reinterpret_cast<uintptr_t>(m->cpu) & (page_size - 1))) {
        sweet_gpu_memory_destroy(m); return nullptr;
    }
    m->pages = pages;
    for (size_t i = 0; i < pages; ++i) {
        IOByteCount length = 0;
        uint64_t pa = m->buffer->getPhysicalSegment(i * GPU_PAGE, &length, kIOMemoryMapperNone);
        if (!pa || (pa & (GPU_PAGE - 1)) || pa > GPU_PA_LIMIT - GPU_PAGE || length < GPU_PAGE) {
            sweet_gpu_memory_destroy(m); return nullptr;
        }
        for (size_t j = 0; j < i; ++j) {
            if (m->pa[j] == pa) { sweet_gpu_memory_destroy(m); return nullptr; }
        }
        m->pa[i] = pa;
        if (nc && ((i % (page_size / GPU_PAGE) == 0 && (pa & (page_size - 1))) ||
            !nc_mapping_valid(static_cast<unsigned char *>(m->cpu) + i * GPU_PAGE, pa))) {
            sweet_gpu_memory_destroy(m); return nullptr;
        }
    }
    bzero(m->cpu, pages * GPU_PAGE);
    m->initialized = true;
    return m;
}

struct sweet_gpu_memory *sweet_gpu_memory_create(size_t pages) { return create_memory(pages, false); }
struct sweet_gpu_memory *sweet_gpu_memory_create_nc(size_t pages) { return create_memory(pages, true); }

int sweet_gpu_memory_page(struct sweet_gpu_memory *m, size_t index, void **cpu, uint64_t *pa)
{
    if (cpu) *cpu = nullptr;
    if (pa) *pa = 0;
    if (!m || !cpu || !pa || index >= m->pages) return -1;
    *cpu = static_cast<unsigned char *>(m->cpu) + index * GPU_PAGE;
    *pa = m->pa[index];
    return 0;
}

int sweet_gpu_memory_clean(struct sweet_gpu_memory *m, size_t index)
{
    if (!m || index >= m->pages) return -1;
    auto va = reinterpret_cast<vm_offset_t>(m->cpu) + index * GPU_PAGE;
    if (!m->nc) clean_dcache(va, GPU_PAGE, FALSE);
    __asm__ volatile ("dsb sy" ::: "memory");
    return 0;
}
