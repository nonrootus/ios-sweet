#ifndef SWEET_A618_RENDER_POOL_H
#define SWEET_A618_RENDER_POOL_H

#ifdef __cplusplus
extern "C" {
#endif
#include "sweet_a618_address_space.h"

/* 128 MiB of already pinned, Normal-NC memory. The tree is built before the
 * GPU context is installed; no live SMMU leaf replacement or TLB invalidation.
 * The first page of each 2 MiB window belongs to the same pool as the rest. */
#define SWEET_A618_RENDER_IOVA UINT32_C(0x48000000)
#define SWEET_A618_RENDER_TABLES 64u
#define SWEET_A618_RENDER_PAGES (SWEET_A618_RENDER_TABLES * 512u)
#define SWEET_A618_RENDER_BYTES (SWEET_A618_RENDER_PAGES * 4096u)

/* The base tree has already been built and verified. Every new table must be
 * zero and privately owned. On preflight failure no table is modified. */
int sweet_a618_render_pool_attach(const struct sweet_gpu_table_page base[3],
    const struct sweet_gpu_table_page extra[SWEET_A618_RENDER_TABLES],
    const uint64_t physical[SWEET_A618_RENDER_PAGES]);
int sweet_a618_render_pool_check(const struct sweet_gpu_table_page base[3],
    const struct sweet_gpu_table_page extra[SWEET_A618_RENDER_TABLES],
    const uint64_t physical[SWEET_A618_RENDER_PAGES]);
int sweet_a618_address_space_check_render(const struct sweet_gpu_table_page base[3],
    const uint64_t data_pa[SWEET_A618_DATA_PAGES], uint64_t fill_pa,
    const struct sweet_gpu_table_page extra[SWEET_A618_RENDER_TABLES],
    const uint64_t physical[SWEET_A618_RENDER_PAGES]);

#ifdef __cplusplus
}
#endif
#endif
