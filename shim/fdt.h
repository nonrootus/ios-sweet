#pragma once
#include <stdint.h>

/* Read a u32/u64 (cell-size agnostic) property of /chosen. Returns 0 if absent. */
int fdt_chosen_u64(uintptr_t fdt, const char *prop, uint64_t *out);

/* Evidence from the current handoff. Dynamic pools describe possible placement. */
enum fdt_ram_evidence {
	FDT_RAM_CONTAINED = 0x01,
	FDT_RAM_FIXED_OVERLAP = 0x02,
	FDT_RAM_DYNAMIC_POSSIBLE = 0x04,
	FDT_RAM_BLOB_OVERLAP = 0x08,
	FDT_RAM_INVALID = 0x10,
	FDT_RAM_BOOT_OVERLAP = 0x20,
};
unsigned fdt_ram_probe(uintptr_t fdt, uint64_t base, uint64_t size);
