#ifndef SWEET_HFI_PERF_H
#define SWEET_HFI_PERF_H
#include <stdint.h>
#include <stddef.h>
/* Pinned legacy hfi_dcvstable_v1_cmd, not the later ACD-bearing format. */
#define SWEET_HFI_PERF_WORDS 43u
#define SWEET_HFI_PERF_COMMAND 4u
#define SWEET_HFI_TEST_COMMAND 5u
struct sweet_hfi_perf_pair { uint32_t vote, frequency_khz; };
struct sweet_hfi_perf_table {
    uint32_t gpu_count, gmu_count;
    struct sweet_hfi_perf_pair gpu[16], gmu[4];
};
/* Offline codec only. Caller chooses supported OPPs and derives votes from
 * the real ARC tables. Frequencies are kHz (stock uses Hz / 1000), not Hz.
 * Input must start with the off entry and ascend. No hardware is accessed.
 * Caller must verify the firmware uses legacy HFI major < 2 and that its
 * selected OPP/ARC tables provide the required zero-frequency/zero-vote entry.
 * Nonoverlapping input/output; errors leave output unchanged, padding zero. */
int sweet_hfi_perf_encode(uint32_t *, size_t, uint16_t,
    const struct sweet_hfi_perf_table *);
/* Legacy A618 HFI_USE_REG terminator; this encodes TEST, not CORE_FW_START. */
int sweet_hfi_v1_test(uint32_t *, size_t, uint16_t);
#endif
