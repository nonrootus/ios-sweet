#include "sweet_gpu_hfi_memory.h"
#include "sweet_gpu_memory.h"
#include <IOKit/IOLib.h>
extern "C" {
#ifdef KERNEL
#include "../../../bsd/dev/sweet_gpu_tables.h"
#include "../../../bsd/dev/sweet_gpu_hfi_table.h"
#else
#include "sweet_gpu_tables.h"
#include "sweet_gpu_hfi_table.h"
#endif
}

enum { DATA_PAGES = 5, ROOT_PAGE = 5, POOL_PAGES = 8, BYTES_PER_PAGE = 4096 };
static_assert(SWEET_HFI_V1_BYTES <= DATA_PAGES * BYTES_PER_PAGE, "HFI storage");
struct sweet_gpu_hfi_memory {
    /* Separate native-page-aligned descriptors: queue cache attributes must
     * never change the mapping of a page that also contains a table. */
    sweet_gpu_memory *data_memory;
    sweet_gpu_memory *table_memory;
    sweet_gpu_table_page pages[POOL_PAGES];
    uint64_t scratch[SWEET_GPU_TABLE_MAX_PAGES]; /* heap, never kernel stack */
    uint32_t iova;
    unsigned used;
    bool ready;
};

static int allocate_table(void *ctx, sweet_gpu_table_page *out)
{
    auto *s = static_cast<sweet_gpu_hfi_memory *>(ctx);
    for (unsigned i = ROOT_PAGE + 1; i < POOL_PAGES; ++i) {
        if (!(s->used & (1u << i))) {
            s->used |= 1u << i;
            *out = s->pages[i];
            return 0;
        }
    }
    return -1;
}

static void release_table(void *ctx, sweet_gpu_table_page page)
{
    auto *s = static_cast<sweet_gpu_hfi_memory *>(ctx);
    for (unsigned i = ROOT_PAGE + 1; i < POOL_PAGES; ++i) {
        if (s->pages[i].pa == page.pa && s->pages[i].cpu == page.cpu) {
            bzero(page.cpu, BYTES_PER_PAGE);
            s->used &= ~(1u << i);
            return;
        }
    }
}

static uint64_t *resolve_table(void *ctx, uint64_t pa)
{
    auto *s = static_cast<sweet_gpu_hfi_memory *>(ctx);
    for (unsigned i = ROOT_PAGE; i < POOL_PAGES; ++i)
        if ((s->used & (1u << i)) && s->pages[i].pa == pa) return s->pages[i].cpu;
    return nullptr;
}

static int clean_table(void *ctx, uint64_t *cpu, size_t bytes)
{
    auto *s = static_cast<sweet_gpu_hfi_memory *>(ctx);
    if (bytes != BYTES_PER_PAGE) return -1;
    for (unsigned i = ROOT_PAGE; i < POOL_PAGES; ++i)
        if ((s->used & (1u << i)) && s->pages[i].cpu == cpu)
            return sweet_gpu_memory_clean(s->table_memory, i - ROOT_PAGE);
    return -1;
}

/* memory_clean completes its own dsb sy before returning. */
static int cleaned_barrier(void *) { return 0; }

void sweet_gpu_hfi_memory_destroy(sweet_gpu_hfi_memory *s)
{
    if (!s) return;
    sweet_gpu_memory_destroy(s->table_memory);
    sweet_gpu_memory_destroy(s->data_memory);
    IOFree(s, sizeof(*s));
}

sweet_gpu_hfi_memory *sweet_gpu_hfi_memory_create(uint32_t iova)
{
    if (!iova || (iova & 65535u)) return nullptr;
    auto *s = static_cast<sweet_gpu_hfi_memory *>(IOMallocZero(sizeof(sweet_gpu_hfi_memory)));
    if (!s) return nullptr;
    s->iova = iova;
    s->data_memory = sweet_gpu_memory_create(DATA_PAGES);
    if (!s->data_memory) { sweet_gpu_hfi_memory_destroy(s); return nullptr; }
    s->table_memory = sweet_gpu_memory_create(POOL_PAGES - ROOT_PAGE);
    if (!s->table_memory) { sweet_gpu_hfi_memory_destroy(s); return nullptr; }
    for (unsigned i = 0; i < POOL_PAGES; ++i) {
        void *cpu = nullptr;
        auto *memory = i < ROOT_PAGE ? s->data_memory : s->table_memory;
        unsigned index = i < ROOT_PAGE ? i : i - ROOT_PAGE;
        if (sweet_gpu_memory_page(memory, index, &cpu, &s->pages[i].pa)) {
            sweet_gpu_hfi_memory_destroy(s); return nullptr;
        }
        s->pages[i].cpu = static_cast<uint64_t *>(cpu);
        if (i < DATA_PAGES && reinterpret_cast<uintptr_t>(cpu) !=
            reinterpret_cast<uintptr_t>(s->pages[0].cpu) + i * BYTES_PER_PAGE) {
            sweet_gpu_hfi_memory_destroy(s); return nullptr;
        }
    }
    s->used = 1u << ROOT_PAGE;
    sweet_gpu_table_ops ops = {s, allocate_table, release_table, resolve_table};
    sweet_gpu_table_publish_ops pub = {s, clean_table, cleaned_barrier};
    if (sweet_gpu_hfi_table_init(s->pages[0].cpu, DATA_PAGES * BYTES_PER_PAGE, iova)) {
        sweet_gpu_hfi_memory_destroy(s); return nullptr;
    }
    for (unsigned i = 0; i < DATA_PAGES; ++i) {
        if (sweet_gpu_table_map(s->pages[ROOT_PAGE], &ops,
            static_cast<uint64_t>(iova) + i * BYTES_PER_PAGE, s->pages[i].pa, 0) ||
            sweet_gpu_memory_clean(s->data_memory, i)) {
            sweet_gpu_hfi_memory_destroy(s); return nullptr;
        }
    }
    if (sweet_gpu_table_publish_inactive(s->pages[ROOT_PAGE], &ops, &pub,
        s->scratch, SWEET_GPU_TABLE_MAX_PAGES)) {
        sweet_gpu_hfi_memory_destroy(s); return nullptr;
    }
    s->ready = true;
    return s;
}

int sweet_gpu_hfi_memory_info(const sweet_gpu_hfi_memory *s, uint64_t *root_pa, uint32_t *iova)
{
    if (root_pa) *root_pa = 0;
    if (iova) *iova = 0;
    if (!s || !s->ready || !root_pa || !iova) return -1;
    *root_pa = s->pages[ROOT_PAGE].pa;
    *iova = s->iova;
    return 0;
}
