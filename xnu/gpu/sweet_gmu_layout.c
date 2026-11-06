#include "sweet_gmu_layout.h"

int sweet_gmu_legacy_layout(struct sweet_gmu_layout *out,
    unsigned firmware_preallocations, unsigned ecp)
{
    /* kernel/msm b6410c3f75259c0ca2df45a01838d2e46272723e:
     * kgsl_gmu.c gmu_vma_legacy, gmu_cache_finalize, gmu_memory_probe.
     * load_firmware/cache_finalize precedes memory_probe in gmu_init.
     * HFIMEM_SIZE=16616 rounds to five pages (occupied queue bytes=16608).
     * Tables: L1 plus L2/L3 pairs for the low and 0x60000000 ranges. */
    static const struct sweet_gmu_layout profile = {
        {
            {0x00004000u, 0x0003c000u, 1},
            {0x00044000u, 0x0003c000u, 1},
            {0x60000000u, 0x00001000u, 1},
            {0x60001000u, 0x00005000u, 0},
            {0x60006000u, 0x00004000u, 0},
            {0x6000a000u, 0x00001000u, 1},
        },
        5, 131, 5
    };
    if (!out || firmware_preallocations || ecp) return -1;
    *out = profile;
    return 0;
}
