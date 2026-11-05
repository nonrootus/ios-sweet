#ifndef SWEET_GPU_MEMORY_H
#define SWEET_GPU_MEMORY_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Preparation-only wired storage. No device mapping or DMA may reference it.
 * Externally serialize all calls; create/destroy may sleep. PA is a host
 * physical address, not proof of device visibility through the platform.
 * create zeroes CPU memory; call clean explicitly after the final CPU writes.
 * A future active-DMA owner needs a separate quiesce/lifetime protocol. */
struct sweet_gpu_memory;
struct sweet_gpu_memory *sweet_gpu_memory_create(size_t pages);
/* Normal-NC via the canonical CPU pointer only. Validates effective PA/cache
 * attributes; requires highmem cache-sync fix0131. No access through physmap
 * or any other alias is permitted. Still preparation-only: no DMA lifetime. */
struct sweet_gpu_memory *sweet_gpu_memory_create_nc(size_t pages);
void sweet_gpu_memory_destroy(struct sweet_gpu_memory *memory);
int sweet_gpu_memory_page(struct sweet_gpu_memory *memory, size_t index,
    void **cpu, uint64_t *pa);
int sweet_gpu_memory_clean(struct sweet_gpu_memory *memory, size_t index);
#ifdef __cplusplus
}
#endif
#endif
