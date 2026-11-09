#include "sweet_a618_render_pool.h"
#include "sweet_gpu_pte.h"

#define TABLE_ENTRIES 512u
#define PAGE_BYTES 4096u
_Static_assert((SWEET_A618_RENDER_IOVA >> 30)==1u, "renderer must share SID0 L1");
_Static_assert(((SWEET_A618_RENDER_IOVA >> 21)&511u)==64u, "renderer L2 start");

_Static_assert((SWEET_A618_PANEL_IOVA >> 30)==1u, "panel must share SID0 L1");
_Static_assert(((SWEET_A618_PANEL_IOVA >> 21)&511u)==SWEET_A618_PANEL_L2, "panel L2 start");
_Static_assert(SWEET_A618_PANEL_L2 >= 64u+SWEET_A618_RENDER_TABLES, "panel after the pool");
_Static_assert(SWEET_A618_PANEL_PAGES <= SWEET_A618_PANEL_TABLES*TABLE_ENTRIES, "panel tables");

static int overlap(uintptr_t a, size_t as, uintptr_t b, size_t bs)
{
    if (!a || !b || !as || !bs || a > UINTPTR_MAX-as || b > UINTPTR_MAX-bs)
        return 1;
    return a < b+bs && b < a+as;
}

static int valid(const struct sweet_gpu_table_page base[3],
    const struct sweet_gpu_table_page extra[SWEET_A618_RENDER_TABLES],
    const uint64_t physical[SWEET_A618_RENDER_PAGES], int attached)
{
    if (!base || !extra || !physical ||
        ((uintptr_t)base % _Alignof(struct sweet_gpu_table_page)) ||
        ((uintptr_t)extra % _Alignof(struct sweet_gpu_table_page)) ||
        ((uintptr_t)physical % _Alignof(uint64_t))) return -1;
    for (unsigned j = 0; j < 3; ++j) {
        uint64_t descriptor;
        if (!base[j].cpu || ((uintptr_t)base[j].cpu & (PAGE_BYTES-1)) ||
            sweet_gpu_pte_table(base[j].pa,&descriptor)) return -1;
    }
    for (unsigned t = 0; t < SWEET_A618_RENDER_TABLES; ++t) {
        uint64_t descriptor;
        if (!extra[t].cpu || ((uintptr_t)extra[t].cpu & (PAGE_BYTES-1)) ||
            sweet_gpu_pte_table(extra[t].pa, &descriptor) ||
            overlap((uintptr_t)extra[t].cpu,PAGE_BYTES,(uintptr_t)extra,
                SWEET_A618_RENDER_TABLES*sizeof(*extra)) ||
            overlap((uintptr_t)extra[t].cpu,PAGE_BYTES,(uintptr_t)physical,
                SWEET_A618_RENDER_PAGES*sizeof(*physical))) return -1;
        for (unsigned j = 0; j < 3; ++j)
            if (!base[j].cpu || extra[t].pa == base[j].pa ||
                overlap((uintptr_t)extra[t].cpu,PAGE_BYTES,(uintptr_t)base[j].cpu,PAGE_BYTES))
                return -1;
        for (unsigned j = 0; j < t; ++j)
            if (extra[t].pa == extra[j].pa ||
                overlap((uintptr_t)extra[t].cpu,PAGE_BYTES,(uintptr_t)extra[j].cpu,PAGE_BYTES))
                return -1;
        for (unsigned index = 0; index < SWEET_A618_RENDER_PAGES; ++index)
            if (extra[t].pa == physical[index]) return -1;
        if (base[1].cpu[64+t] != (attached ? descriptor : 0)) return -1;
        for (unsigned leaf = 0; leaf < TABLE_ENTRIES; ++leaf) {
            const unsigned index = t*TABLE_ENTRIES+leaf;
            uint64_t expected;
            if (sweet_gpu_pte_page_access(physical[index],0,0,&expected)) return -1;
            expected |= UINT64_C(3)<<53; /* PXN and UXN: never execute a BO. */
            if (extra[t].cpu[leaf] != (attached ? expected : 0)) return -1;
        }
    }
    return 0;
}

int sweet_a618_render_pool_attach(const struct sweet_gpu_table_page base[3],
    const struct sweet_gpu_table_page extra[SWEET_A618_RENDER_TABLES],
    const uint64_t physical[SWEET_A618_RENDER_PAGES])
{
    if (valid(base,extra,physical,0)) return -1;
    /* No fallible callback follows the first store. Complete children before
     * publishing the L2 descriptors and the already active L1 root. The caller
     * must still clean all tables and barrier before installing the SID0 root. */
    for (unsigned t = 0; t < SWEET_A618_RENDER_TABLES; ++t) {
        for (unsigned leaf = 0; leaf < TABLE_ENTRIES; ++leaf) {
            uint64_t pte;
            (void)sweet_gpu_pte_page_access(physical[t*TABLE_ENTRIES+leaf],0,0,&pte);
            extra[t].cpu[leaf] = pte | (UINT64_C(3)<<53);
        }
    }
    for (unsigned t = 0; t < SWEET_A618_RENDER_TABLES; ++t)
        base[1].cpu[64+t] = extra[t].pa | 3u;
    return 0;
}

int sweet_a618_panel_attach(const struct sweet_gpu_table_page base[3],
    const struct sweet_gpu_table_page tables[SWEET_A618_PANEL_TABLES])
{
    if (!base || !tables || !base[1].cpu) return -1;
    for (unsigned t = 0; t < SWEET_A618_PANEL_TABLES; ++t) {
        uint64_t descriptor;
        if (!tables[t].cpu || ((uintptr_t)tables[t].cpu & (PAGE_BYTES-1)) ||
            sweet_gpu_pte_table(tables[t].pa,&descriptor) ||
            base[1].cpu[SWEET_A618_PANEL_L2+t]) return -1;
        for (unsigned leaf = 0; leaf < TABLE_ENTRIES; ++leaf)
            if (tables[t].cpu[leaf]) return -1;
    }
    for (unsigned page = 0; page < SWEET_A618_PANEL_PAGES; ++page) {
        uint64_t pte;
        if (sweet_gpu_pte_page_access(SWEET_A618_PANEL_PA+(uint64_t)page*PAGE_BYTES,0,0,&pte))
            return -1;
        tables[page/TABLE_ENTRIES].cpu[page%TABLE_ENTRIES] = pte | (UINT64_C(3)<<53);
    }
    /* Complete children before the L2 descriptors; the caller cleans them all. */
    for (unsigned t = 0; t < SWEET_A618_PANEL_TABLES; ++t)
        base[1].cpu[SWEET_A618_PANEL_L2+t] = tables[t].pa | 3u;
    return 0;
}

int sweet_a618_render_pool_check(const struct sweet_gpu_table_page base[3],
    const struct sweet_gpu_table_page extra[SWEET_A618_RENDER_TABLES],
    const uint64_t physical[SWEET_A618_RENDER_PAGES])
{
    return valid(base,extra,physical,1);
}
