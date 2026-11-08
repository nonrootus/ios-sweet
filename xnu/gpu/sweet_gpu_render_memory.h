#ifndef SWEET_GPU_RENDER_MEMORY_H
#define SWEET_GPU_RENDER_MEMORY_H

#ifdef KERNEL
#include "../../../bsd/dev/sweet_a618_render_pool.h"
#else
#include "sweet_a618_render_pool.h"
#endif
class IOBufferMemoryDescriptor;

/* One permanent, wired, Normal-NC allocation made before the GPU context is
 * installed. The descriptor keeps every BO's physical backing alive. */
struct sweet_gpu_render_memory;
sweet_gpu_render_memory *sweet_gpu_render_memory_create(void);
void sweet_gpu_render_memory_destroy_inactive(sweet_gpu_render_memory *);
IOBufferMemoryDescriptor *sweet_gpu_render_memory_descriptor(sweet_gpu_render_memory *);
void *sweet_gpu_render_memory_cpu(sweet_gpu_render_memory *);
const uint64_t *sweet_gpu_render_memory_physical(sweet_gpu_render_memory *);

#endif
