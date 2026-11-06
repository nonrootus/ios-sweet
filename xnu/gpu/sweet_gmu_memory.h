#ifndef SWEET_GMU_MEMORY_H
#define SWEET_GMU_MEMORY_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
struct sweet_gmu_memory;
/* Inactive A618 legacy/no-preallocation/no-ECP memory image. Caller must first
 * validate that firmware/target profile; this function does not inspect a blob.
 * Six isolated canonical Normal-NC allocations, separate cached page tables.
 * No MMIO or firmware execution is performed by this allocator.
 * Mapping refs must be held before publishing root PA, retained indefinitely
 * on ambiguous hardware failure, released only after proven detach/drain.
 * All calls require external serialization; create/destroy may sleep. */
struct sweet_gmu_memory *sweet_gmu_memory_create_legacy(void);
int sweet_gmu_memory_info(const struct sweet_gmu_memory *, uint64_t *root,
    uint32_t *hfi, uint32_t *dump, uint32_t *log);
int sweet_gmu_memory_retain_mapping(struct sweet_gmu_memory *, uint64_t *root);
int sweet_gmu_memory_release_mapping(struct sweet_gmu_memory *);
/* One-way first-bring-up latch. Requires ready storage and an existing mapping
 * reference held by the externally serialized caller; refs do not prove its
 * identity, translations, power, ownership or device quiescence. Call BEFORE
 * any operation that might start the producer. On success only the canonical
 * Normal-NC HFI pointer and logical 0x5000 bytes are exposed. Normal-NC still
 * requires the HFI transport's acquire/completion barriers.
 * All storage is then permanently retained until reboot, including on aborted
 * attempts. There is no unlatch/stop/drain API; retain/release_mapping refuse.
 * Kernel must remain resident. Subsequent allocator calls never touch HFI
 * bytes. 0 success, -2 already latched, -1 invalid/destroy requested;
 * outputs are zeroed on failure. This does not start or test GMU. */
int sweet_gmu_memory_begin_execution(struct sweet_gmu_memory *,
    volatile uint32_t **hfi_cpu, uint32_t *hfi_bytes);
/* Relinquishes caller ownership. If retained, only matching release calls
 * are permitted afterwards; the last release completes deferred destruction.
 * Exception: after the execution latch, destroy only marks pending, repeated
 * destroy is harmless, release refuses, and the returned HFI view stays valid
 * until reboot. Info/retain/begin calls then refuse with zeroed outputs. */
void sweet_gmu_memory_destroy(struct sweet_gmu_memory *);
#ifdef __cplusplus
}
#endif
#endif
