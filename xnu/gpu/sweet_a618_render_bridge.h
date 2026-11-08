#ifndef SWEET_A618_RENDER_BRIDGE_H
#define SWEET_A618_RENDER_BRIDGE_H

#include <stdint.h>
#include <stddef.h>
class IOBufferMemoryDescriptor;

/* Only the signed backboardd client may call the bridge. Every BO must lie in
 * the pre-pinned pool. A failed GPU publication poisons it until device reset. */
extern "C" bool sweet_a618_render_ready(void);
extern "C" IOBufferMemoryDescriptor *sweet_a618_render_descriptor(void);
extern "C" void *sweet_a618_render_cpu(void);
extern "C" int sweet_a618_render_submit(const uint64_t *iova,
    const uint32_t *dwords, unsigned count, uint32_t *fence);
extern "C" uint32_t sweet_a618_render_completed(void);

#endif
