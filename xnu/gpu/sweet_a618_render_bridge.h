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
extern "C" bool sweet_a618_panel_ready(void);
/* AppleCLCD and the renderer: which surfaces are on the panel, and whether the
 * GPU has already put this one there. */
extern "C" void sweet_a618_note_display(uint32_t surface);
extern "C" bool sweet_a618_direct_take(uint32_t surface);
/* IOSurfaces read by the GPU in place: pages of the import window (render_pool.h). */
extern "C" bool sweet_a618_import_ready(void);
extern "C" int sweet_a618_import_map(unsigned first, const uint64_t *physical, unsigned count);
extern "C" void sweet_a618_import_unmap(unsigned first, unsigned count);
extern "C" int sweet_a618_import_invalidate(void);

#endif
