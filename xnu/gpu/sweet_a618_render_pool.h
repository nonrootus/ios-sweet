#ifndef SWEET_A618_RENDER_POOL_H
#define SWEET_A618_RENDER_POOL_H

#ifdef __cplusplus
extern "C" {
#endif
#include "sweet_a618_address_space.h"

/* 256 MiB of already pinned, Normal-NC memory. The tree is built before the
 * GPU context is installed; no live SMMU leaf replacement or TLB invalidation.
 * The first page of each 2 MiB window belongs to the same pool as the rest. */
#define SWEET_A618_RENDER_IOVA UINT32_C(0x48000000)
#define SWEET_A618_RENDER_TABLES 128u
#define SWEET_A618_RENDER_PAGES (SWEET_A618_RENDER_TABLES * 512u)
#define SWEET_A618_RENDER_BYTES (SWEET_A618_RENDER_PAGES * 4096u)

/* The panel's memory, for writing a frame to it with the GPU. It is mapped once,
 * while the tree is built, and never changed. One page past the panel's rows,
 * because the odd field's buffer starts 4288 bytes in. */
#define SWEET_A618_PANEL_IOVA UINT32_C(0x58000000)
#define SWEET_A618_PANEL_PA UINT64_C(0x9c000000)
#define SWEET_A618_PANEL_PAGES 2533u
#define SWEET_A618_PANEL_TABLES 5u
#define SWEET_A618_PANEL_L2 192u

/* IOSurfaces the GPU reads in place: 512 MiB of GPU VA whose L3 tables are attached
 * empty before the context is installed. Leaves are entered and cleared at run time
 * (sweet_a618_renderer.inc); a cleared leaf needs a TLB invalidation before reuse. */
#define SWEET_A618_IMPORT_IOVA UINT32_C(0x60000000)
#define SWEET_A618_IMPORT_L2 256u
#define SWEET_A618_IMPORT_TABLES 256u
#define SWEET_A618_IMPORT_PAGES (SWEET_A618_IMPORT_TABLES * 512u)

/* The base tree has already been built and verified. Every new table must be
 * zero and privately owned. On preflight failure no table is modified. */
int sweet_a618_render_pool_attach(const struct sweet_gpu_table_page base[3],
    const struct sweet_gpu_table_page extra[SWEET_A618_RENDER_TABLES],
    const uint64_t physical[SWEET_A618_RENDER_PAGES]);
int sweet_a618_render_pool_check(const struct sweet_gpu_table_page base[3],
    const struct sweet_gpu_table_page extra[SWEET_A618_RENDER_TABLES],
    const uint64_t physical[SWEET_A618_RENDER_PAGES]);
int sweet_a618_panel_attach(const struct sweet_gpu_table_page base[3],
    const struct sweet_gpu_table_page tables[SWEET_A618_PANEL_TABLES]);
/* Empty, zeroed, owned tables only; nothing is changed on failure. */
int sweet_a618_import_attach(const struct sweet_gpu_table_page base[3],
    const struct sweet_gpu_table_page tables[SWEET_A618_IMPORT_TABLES]);
int sweet_a618_import_check(const struct sweet_gpu_table_page base[3],
    const struct sweet_gpu_table_page tables[SWEET_A618_IMPORT_TABLES]);
/* Leaves for pages first.. of the window: only empty ones, all or none. The caller
 * completes the stores (dsb) before the GPU may use them. */
int sweet_a618_import_set(const struct sweet_gpu_table_page tables[SWEET_A618_IMPORT_TABLES],
    unsigned first, const uint64_t *physical, unsigned count);
void sweet_a618_import_clear(const struct sweet_gpu_table_page tables[SWEET_A618_IMPORT_TABLES],
    unsigned first, unsigned count);
int sweet_a618_address_space_check_render(const struct sweet_gpu_table_page base[3],
    const uint64_t data_pa[SWEET_A618_DATA_PAGES], uint64_t fill_pa,
    const struct sweet_gpu_table_page extra[SWEET_A618_RENDER_TABLES],
    const uint64_t physical[SWEET_A618_RENDER_PAGES]);

#ifdef __cplusplus
}
#endif
#endif
