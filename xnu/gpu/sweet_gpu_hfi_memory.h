#ifndef SWEET_GPU_HFI_MEMORY_H
#define SWEET_GPU_HFI_MEMORY_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
struct sweet_gpu_hfi_memory;
/* Build an inactive HFI v1 image and translation tree in owned wired memory.
 * 64KiB-aligned IOVA is supplied by a caller that reserves that range.
 * No SMMU programming, TLB operations or DMA lifetime management is provided.
 * These objects must never be installed in a live context yet. Calls may sleep;
 * externally serialize access. No CPU mapping is exposed for later mutation.
 * Queue storage and table storage use distinct native-page-aligned allocations.
 * Both are still cached; separation alone does not establish DMA coherency.
 * HFI headers share cache lines between CPU-written and GMU-written fields.
 * The current cached allocation is unsuitable for live non-coherent queues:
 * choose a verified uncached/shared-memory protocol before hardware use.
 * IOVA must eventually come from the firmware's permitted GMU window. */
struct sweet_gpu_hfi_memory *sweet_gpu_hfi_memory_create(uint32_t iova);
int sweet_gpu_hfi_memory_info(const struct sweet_gpu_hfi_memory *,
    uint64_t *root_pa, uint32_t *iova);
void sweet_gpu_hfi_memory_destroy(struct sweet_gpu_hfi_memory *);
#ifdef __cplusplus
}
#endif
#endif
