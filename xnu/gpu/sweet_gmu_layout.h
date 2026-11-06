#ifndef SWEET_GMU_LAYOUT_H
#define SWEET_GMU_LAYOUT_H
#include <stdint.h>

enum sweet_gmu_region_id {
    SWEET_GMU_ICACHE, SWEET_GMU_DCACHE, SWEET_GMU_DUMMY,
    SWEET_GMU_HFI, SWEET_GMU_DUMP, SWEET_GMU_LOG, SWEET_GMU_REGION_COUNT
};
struct sweet_gmu_region {
    uint32_t iova, bytes;
    unsigned privileged;
};
struct sweet_gmu_layout {
    struct sweet_gmu_region region[SWEET_GMU_REGION_COUNT];
    unsigned stream_id, data_pages, table_pages;
};
/* Source-derived A618 legacy flat-firmware profile, 4KiB SMMU granules.
 * Requires no metadata preallocations, no ECP allocations and a fresh address
 * space. Reject unsupported profiles; do not silently reuse these addresses.
 * All six regions are read/write, stage-1 Normal-NC. privileged describes
 * IOMMU_PRIV access, NOT CPU mapping attributes or the choice of context bank.
 * SID5 comes from this phone's gmu_kernel DT node. Neither TCM nor the user
 * window is mapped by this profile. This helper allocates/touches no memory
 * or hardware; actual firmware requirements and DMA remain unvalidated. */
int sweet_gmu_legacy_layout(struct sweet_gmu_layout *out,
    unsigned firmware_preallocations, unsigned ecp);
#endif
