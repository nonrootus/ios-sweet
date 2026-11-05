#ifndef SWEET_GPU_HFI_MEMORY_H
#define SWEET_GPU_HFI_MEMORY_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
struct sweet_gpu_hfi_memory;
/* Build an inactive HFI v1 image and translation tree in owned wired memory.
 * 64KiB-aligned IOVA is supplied by a caller that reserves that range.
 * No SMMU programming or TLB operations are provided. Calls may sleep;
 * externally serialize access. No CPU mapping is exposed for later mutation.
 * Queue storage and table storage use distinct native-page-aligned allocations.
 * Queue data uses a validated Normal-NC canonical CPU mapping; tables remain
 * cached. This does not establish device visibility or DMA coherency.
 * HFI headers share cache lines between CPU-written and GMU-written fields.
 * Active queue access still needs ordered reads/writes and verified SMMU/device
 * lifetime integration before hardware use; this object only initializes HFI.
 * IOVA must eventually come from the firmware's permitted GMU window. */
struct sweet_gpu_hfi_memory *sweet_gpu_hfi_memory_create(uint32_t iova);
int sweet_gpu_hfi_memory_info(const struct sweet_gpu_hfi_memory *,
    uint64_t *root_pa, uint32_t *iova);
void sweet_gpu_hfi_memory_destroy(struct sweet_gpu_hfi_memory *);
/* Retain before exposing any table PA to hardware; release only before the
 * first hardware write, or after a proven successful stop/detach/TLB sync.
 * A timeout or ambiguous hardware error must keep the reference indefinitely.
 * These operations enforce allocation lifetime, not hardware quiescence.
 * destroy relinquishes caller ownership: when retained it defers freeing until
 * the last release. After destroy, only matching release calls may use s.
 * All operations, including destroy, require external serialization. */
int sweet_gpu_hfi_memory_retain_mapping(struct sweet_gpu_hfi_memory *, uint64_t *root_pa);
int sweet_gpu_hfi_memory_release_mapping(struct sweet_gpu_hfi_memory *);
#ifdef __cplusplus
}
#endif
#endif
