#ifndef SWEET_SMMU_K6A_H
#define SWEET_SMMU_K6A_H
#include "sweet_smmu_context.h"
#ifdef __cplusplus
extern "C" {
#endif
enum sweet_smmu_k6a_state {
    SWEET_SMMU_K6A_EMPTY, SWEET_SMMU_K6A_READY, SWEET_SMMU_K6A_POISONED
};
enum sweet_smmu_k6a_stage {
    SWEET_SMMU_K6A_PREFLIGHT, SWEET_SMMU_K6A_INVALIDATE,
    SWEET_SMMU_K6A_GLOBAL, SWEET_SMMU_K6A_HALT,
    SWEET_SMMU_K6A_ATTACH, SWEET_SMMU_K6A_RESUME
};
struct sweet_smmu_k6a {
    struct sweet_smmu_context_ops ops;
    enum sweet_smmu_k6a_state state;
    enum sweet_smmu_k6a_stage stage;
    uint32_t last_offset, last_value;
};
/* Zero-initialize and externally serialize. Requires exclusive ownership of
 * the entire GPU SMMU, powered/clocks stable, all masters stopped and drained.
 * The quiescent callback must check these prerequisites, not just SCTLR bits.
 * No CPU/GMU PA is installed. No ATS request, GMU start or security call.
 * On failure after any write, retain ownership/power and require a separately
 * proven device reset; there is no speculative rollback or retry.
 * READY means initialization writes and core register checks completed; the
 * implementation-defined registers are not read back, following the vendor.
 * It never proves working translations. The vendor's first attach pair clears
 * local HALT, so stopped/drained masters must remain so through every pair.
 * This fixed board recipe uses the twelve captured k6a attach-impl-defs pairs.
 * It must not be reused for another board with matching SMMU ID registers. */
int sweet_smmu_k6a_prepare(struct sweet_smmu_k6a *, const struct sweet_smmu_context_ops *);
#ifdef __cplusplus
}
#endif
#endif
