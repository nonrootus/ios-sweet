#ifndef SWEET_GPU_HFI_TABLE_H
#define SWEET_GPU_HFI_TABLE_H
#include <stddef.h>
#include <stdint.h>
#define SWEET_HFI_V1_BYTES 16608u
/* Pinned KGSL b6410c3f four-header legacy layout, 4KiB-aligned GMU base.
 * Initializes only the occupied bytes. Caller must zero the entire allocation
 * (five pages), and owns mapping, cache maintenance and hardware protocol. */
int sweet_gpu_hfi_table_init(void *, size_t, uint32_t gmu_iova);
#endif
