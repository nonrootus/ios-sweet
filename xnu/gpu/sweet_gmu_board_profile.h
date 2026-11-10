#ifndef SWEET_GMU_BOARD_PROFILE_H
#define SWEET_GMU_BOARD_PROFILE_H
#include "sweet_hfi_perf.h"
#include "sweet_hfi_bw.h"
#define SWEET_GMU_RECOVERY167 167u
/* Explicit recovery-matched operating profile, NOT a fuse result. Recovery's
 * kgsl lists exactly bin 167's rates (180..800MHz), so requests up to
 * index7/800MHz are permitted. gpu_bus_level[i] is bin 167's stock bus level
 * for GMU index i, sent with a request for that index. */
struct sweet_gmu_board_profile {
    struct sweet_hfi_perf_table perf;
    struct sweet_hfi_bw_table bw;
    unsigned initial_gpu_index, initial_bw_level, max_requested_gpu_index;
    unsigned gpu_bus_level[8];
};
struct sweet_gmu_profile_ops {
    void *ctx;
    int (*arc)(void *,const char *,uint32_t *,uint16_t [16],unsigned *);
    int (*bcm)(void *,const char *,uint32_t *,uint32_t *,uint32_t *);
};
/* Read-only binding: every ARC vote and all BCM addresses/unit/domain metadata
 * must match this profile. No register/power requests. Output unchanged on
 * failure. Caller uses a separate output object from the callback context. */
int sweet_gmu_board_profile_build(struct sweet_gmu_board_profile *,
    const struct sweet_gmu_profile_ops *,unsigned explicit_selection);
#endif
