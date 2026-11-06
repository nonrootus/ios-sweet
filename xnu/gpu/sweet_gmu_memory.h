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
 * No MMIO, firmware execution or CPU queue mutation interface is provided.
 * Mapping refs must be held before publishing root PA, retained indefinitely
 * on ambiguous hardware failure, released only after proven detach/drain.
 * All calls require external serialization; create/destroy may sleep. */
struct sweet_gmu_memory *sweet_gmu_memory_create_legacy(void);
int sweet_gmu_memory_info(const struct sweet_gmu_memory *, uint64_t *root,
    uint32_t *hfi, uint32_t *dump, uint32_t *log);
int sweet_gmu_memory_retain_mapping(struct sweet_gmu_memory *, uint64_t *root);
int sweet_gmu_memory_release_mapping(struct sweet_gmu_memory *);
/* Relinquishes caller ownership. If retained, only matching release calls
 * are permitted afterwards; the last release completes deferred destruction. */
void sweet_gmu_memory_destroy(struct sweet_gmu_memory *);
#ifdef __cplusplus
}
#endif
#endif
