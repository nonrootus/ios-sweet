#include "sweet_gmu_layout.h"

int sweet_gmu_legacy_layout(struct sweet_gmu_layout *out,
    unsigned firmware_preallocations, unsigned ecp)
{
    /* Xiaomi sweet_k6a-r-oss 10c33d40a11ecdc75cc1dadb5bd07894bab05815:
     * kgsl_gmu.c gmu_memory_probe / allocate_gmu_kmem place each uncached
     * allocation at the next 1MiB slot. Do not use newer KGSL's packed map.
     * HFIMEM_SIZE=16616 rounds to five pages (occupied queue bytes=16608).
     * Tables: root + low L2/L3 + high L2 with two L3 tables. */
    static const struct sweet_gmu_layout profile = {
        {
            {0x00004000u, 0x0003c000u, 1},
            {0x00044000u, 0x0003c000u, 1},
            {0x60000000u, 0x00001000u, 1},
            {0x60100000u, 0x00005000u, 0},
            {0x60200000u, 0x00004000u, 0},
            {0x60300000u, 0x00001000u, 1},
        },
        5, 131, 6
    };
    if (!out || firmware_preallocations || ecp) return -1;
    *out = profile;
    return 0;
}
