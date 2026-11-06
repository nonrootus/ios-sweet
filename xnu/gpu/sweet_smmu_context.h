#ifndef SWEET_SMMU_CONTEXT_H
#define SWEET_SMMU_CONTEXT_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* One externally reserved context bank, stream slot and ASID. This helper is
 * not a device initializer. The caller must own the GPU SMMU, complete the
 * board's attach-impl-defs/power/clock setup, serialize all callers, and keep
 * producers stopped/drained throughout install/remove. No ATS operations.
 * Requires the captured board's absent qcom,no-asid-retention property; this
 * implementation invalidates a caller-reserved ASID, not every context ASID.
 * Callback errors may occur after a write reached hardware: never retry a
 * poisoned instance or release its retained memory without a proven reset. */
enum sweet_smmu_context_state {
    SWEET_SMMU_CONTEXT_EMPTY, SWEET_SMMU_CONTEXT_ACTIVE,
    SWEET_SMMU_CONTEXT_RETIRED, SWEET_SMMU_CONTEXT_POISONED
};
enum sweet_smmu_context_result {
    SWEET_SMMU_CONTEXT_OK = 0, SWEET_SMMU_CONTEXT_INVALID = -1,
    SWEET_SMMU_CONTEXT_BUSY = -2, SWEET_SMMU_CONTEXT_IO = -3,
    SWEET_SMMU_CONTEXT_TIMEOUT = -4, SWEET_SMMU_CONTEXT_MISMATCH = -5
};
struct sweet_smmu_context_ops {
    void *ctx;
    int (*read32)(void *, uint32_t offset, uint32_t *);
    int (*write32)(void *, uint32_t offset, uint32_t);
    int (*write64)(void *, uint32_t offset, uint64_t); /* one atomic MMIO store */
    int (*barrier)(void *); /* complete prior MMIO and memory writes */
    int (*delay_us)(void *, unsigned);
    int (*quiescent)(void *); /* 0 only with powered, owned, stopped/drained device */
    /* Pin all table/leaf pages and return the already-published root PA.
     * Failure must retain nothing. unpin is called only before any MMIO write,
     * or after successful removal. These callbacks may sleep. */
    int (*pin)(void *, uint64_t *root_pa);
    void (*unpin)(void *);
};
struct sweet_smmu_context {
    struct sweet_smmu_context_ops ops;
    enum sweet_smmu_context_state state;
    uint64_t root_pa;
    unsigned bank, slot, sid, asid, retained;
};
/* Zero-initialize storage; keep its address and callback context alive until
 * RETIRED, or indefinitely if POISONED. Exact observed k6a ID registers are
 * required. Readback proves register acceptance, not working device DMA. */
int sweet_smmu_context_install(struct sweet_smmu_context *,
    const struct sweet_smmu_context_ops *, unsigned bank, unsigned slot,
    unsigned sid, unsigned asid);
int sweet_smmu_context_remove(struct sweet_smmu_context *);
/* Separate inactive-only teardown for this controller when FAULT writes are
 * ignored. Only for the early-boot register test: no producer may have run
 * since reset. Not a general live-GPU teardown API.
 * Caller must own ALL stream slots, keep all producers stopped and
 * drained, and reserve this controller until reset. Select before teardown;
 * never call as a retry/fallback on a POISONED context. The S2CR remains TRANS
 * behind an invalid SMR; this is not a reusable/reinitialized stream slot. */
int sweet_smmu_context_remove_unmatched(struct sweet_smmu_context *);
#ifdef __cplusplus
}
#endif
#endif
