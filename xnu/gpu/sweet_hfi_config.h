#ifndef SWEET_HFI_CONFIG_H
#define SWEET_HFI_CONFIG_H
#include "sweet_hfi_perf.h"
#include "sweet_hfi_bw.h"

/* Complete legacy A618 HFI_USE_REG configuration, prepared before publishing
 * anything. Caller proves the tables and initial indexes belong to its board
 * and the same retained session completed INIT1/VERSION2 with CMD7/7.
 * This helper does not access queues, MMIO or firmware and grants no ownership.
 * A runtime sender must keep the order PERF3/BW4/TEST5, validate each ACK and
 * prohibit further TX once TEST may have been published. */
struct sweet_hfi_config_packets {
    uint32_t perf[SWEET_HFI_PERF_WORDS];
    uint32_t bw[SWEET_HFI_BW_WORDS];
    uint32_t test[2];
    uint32_t initial_gpu_index, initial_bw_level, boot_vote;
};
/* 0 prepared; -1 invalid input. Output unchanged on failure. No aliasing with
 * either input table. Explicit GPU index is required: two frequencies may
 * share one rail vote. Initial bandwidth is a nonzero supported table index;
 * its frequency-to-bandwidth relation must be validated by the board caller.
 * Preparing packets is NOT completed HFI initialization or GPU readiness. */
int sweet_hfi_config_prepare(struct sweet_hfi_config_packets *,
    const struct sweet_hfi_perf_table *, const struct sweet_hfi_bw_table *,
    unsigned initial_gpu_index, unsigned initial_bw_level, uint32_t boot_vote);
#endif
