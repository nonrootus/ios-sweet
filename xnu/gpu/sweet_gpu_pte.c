#include "sweet_gpu_pte.h"
#define PAGE_MASK UINT64_C(0xfff)
#define PA_LIMIT (UINT64_C(1) << 36)
#define VA_LIMIT (UINT64_C(1) << 32)
#define TYPE_TABLE UINT64_C(3)
#define PAGE_UNPRIV (UINT64_C(1) << 6)
#define PAGE_READONLY (UINT64_C(1) << 7)
#define PAGE_OUTER_SHAREABLE (UINT64_C(2) << 8)
#define PAGE_ACCESSED (UINT64_C(1) << 10)
#define PAGE_NONGLOBAL (UINT64_C(1) << 11)

static int good_pa(uint64_t pa) { return pa < PA_LIMIT && !(pa & PAGE_MASK); }

int sweet_gpu_pte_indices(uint64_t iova, struct sweet_gpu_pte_indices *out)
{
    if (!out || iova >= VA_LIMIT || (iova & PAGE_MASK)) return -1;
    out->l1 = (unsigned)(iova >> 30);
    out->l2 = (unsigned)((iova >> 21) & 511);
    out->l3 = (unsigned)((iova >> 12) & 511);
    return 0;
}
int sweet_gpu_pte_table(uint64_t pa, uint64_t *out)
{
    if (!out || !good_pa(pa)) return -1;
    *out = pa | TYPE_TABLE;
    return 0;
}
int sweet_gpu_pte_page_access(uint64_t pa, int read_only, int privileged, uint64_t *out)
{
    if (!out || !good_pa(pa) || (read_only != 0 && read_only != 1) ||
        (privileged != 0 && privileged != 1)) return -1;
    *out = pa | TYPE_TABLE | (privileged ? 0 : PAGE_UNPRIV) | PAGE_OUTER_SHAREABLE | PAGE_ACCESSED | PAGE_NONGLOBAL |
        (read_only ? PAGE_READONLY : 0);
    return 0;
}
int sweet_gpu_pte_page(uint64_t pa, int read_only, uint64_t *out)
{
    return sweet_gpu_pte_page_access(pa, read_only, 0, out);
}
