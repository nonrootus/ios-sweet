/*
 * Just enough flattened-device-tree parsing to find what ABL put in
 * /chosen (initrd location). Big-endian, 4-byte aligned tokens.
 */
#include "fdt.h"

#define FDT_MAGIC	0xd00dfeed
#define FDT_BEGIN_NODE	1
#define FDT_END_NODE	2
#define FDT_PROP	3
#define FDT_NOP		4
#define FDT_END		9

static uint32_t be32(uintptr_t a)
{
	return __builtin_bswap32(*(volatile uint32_t *)a);
}

static int streq(const char *a, const char *b)
{
	while (*a && *a == *b)
		a++, b++;
	return *a == *b;
}

static uintptr_t align4(uintptr_t a)
{
	return (a + 3) & ~3UL;
}

static uintptr_t skip_str(uintptr_t p)
{
	/* byte loads are fine on Device memory as long as they are aligned to 1 */
	while (*(volatile char *)p)
		p++;
	return p + 1;
}

int fdt_chosen_u64(uintptr_t fdt, const char *prop, uint64_t *out)
{
	if (!fdt || be32(fdt) != FDT_MAGIC)
		return 0;
	uintptr_t p = fdt + be32(fdt + 8);		/* off_dt_struct */
	uintptr_t strings = fdt + be32(fdt + 12);	/* off_dt_strings */
	int depth = 0, in_chosen = 0;

	for (;;) {
		uint32_t tok = be32(p);
		p += 4;
		switch (tok) {
		case FDT_BEGIN_NODE: {
			const char *name = (const char *)p;
			depth++;
			in_chosen = depth == 2 && streq(name, "chosen");
			p = align4(skip_str(p));
			break;
		}
		case FDT_END_NODE:
			if (in_chosen)
				return 0;
			depth--;
			break;
		case FDT_PROP: {
			uint32_t len = be32(p), nameoff = be32(p + 4);
			uintptr_t val = p + 8;
			if (in_chosen && streq((const char *)(strings + nameoff), prop)) {
				if (len == 4)
					*out = be32(val);
				else if (len == 8)
					*out = (uint64_t)be32(val) << 32 | be32(val + 4);
				else
					return 0;
				return 1;
			}
			p = align4(val + len);
			break;
		}
		case FDT_NOP:
			break;
		default:	/* FDT_END or garbage */
			return 0;
		}
	}
}

static uint64_t be64(uintptr_t addr)
{
	return (uint64_t)be32(addr) << 32 | be32(addr + 4);
}

static uintptr_t bounded_string_end(uintptr_t start, uintptr_t end)
{
	for (; start < end; ++start)
		if (!*(volatile const char *)start)
			return start + 1;
	return 0;
}

/* Address/size pairs with two cells apiece. A range must not wrap. */
static int range_evidence(uintptr_t addr, uint32_t len, uint64_t base, uint64_t end)
{
	int result = 0;
	if (!len || (len & 15))
		return -1;
	for (; len; addr += 16, len -= 16) {
		uint64_t start = be64(addr), size = be64(addr + 8);
		if (size > UINT64_MAX - start)
			return -1;
		if (start <= base && end <= start + size)
			result |= FDT_RAM_CONTAINED;
		if (size && start < end && base < start + size)
			result |= FDT_RAM_FIXED_OVERLAP;
	}
	return result;
}

static int memory_name(const char *name)
{
	const char prefix[] = "memory";
	for (unsigned i = 0; i < sizeof(prefix) - 1; ++i)
		if (name[i] != prefix[i])
			return 0;
	return !name[6] || name[6] == '@';
}

unsigned fdt_ram_probe(uintptr_t fdt, uint64_t base, uint64_t size)
{
	if (!fdt || (fdt & 7) || fdt > UINTPTR_MAX - 0x200003 || !size ||
	    size > UINT64_MAX - base || be32(fdt) != FDT_MAGIC)
		return FDT_RAM_INVALID;
	uint32_t total = be32(fdt + 4), structure = be32(fdt + 8), strings = be32(fdt + 12);
	uint32_t reserve = be32(fdt + 16), strings_len = be32(fdt + 32), struct_len = be32(fdt + 36);
	if (be32(fdt + 20) != 17 || be32(fdt + 24) > 17 || total < 40 || total > 0x200000 ||
	    reserve < 40 || (reserve & 7) || (structure & 3) ||
	    structure < reserve || strings < structure || strings > total ||
	    struct_len > strings - structure || strings_len > total - strings)
		return FDT_RAM_INVALID;

	uint64_t end = base + size;
	unsigned result = fdt < end && base < fdt + total ? FDT_RAM_BLOB_OVERLAP : 0;
	for (;;) {
		if (structure - reserve < 16)
			return FDT_RAM_INVALID;
		if (!be64(fdt + reserve) && !be64(fdt + reserve + 8))
			break;
		int match = range_evidence(fdt + reserve, 16, base, end);
		if (match < 0)
			return FDT_RAM_INVALID;
		result |= match & FDT_RAM_FIXED_OVERLAP;
		reserve += 16;
	}

	uintptr_t p = fdt + structure, limit = p + struct_len, str = fdt + strings;
	unsigned depth = 0, roots = 0, root_cells = 0, reserved_cells = 0;
	int reserved = 0, reserved_seen = 0, memory = 0, memory_enabled = 1, reg = 0;
	int pool_reg = 0, pool_size = 0, pool_alloc = 0, pool_hits = 0;
	while (limit - p >= 4) {
		uint32_t token = be32(p);
		p += 4;
		if (token == FDT_BEGIN_NODE) {
			uintptr_t after = bounded_string_end(p, limit);
			if (!after || align4(after) > limit)
				return FDT_RAM_INVALID;
			const char *name = (const char *)p;
			if (++depth == 1 && (++roots != 1 || *name))
				return FDT_RAM_INVALID;
			if (depth == 2) {
				reserved = streq(name, "reserved-memory");
				if (reserved && reserved_seen++)
					return FDT_RAM_INVALID;
				memory = memory_name(name);
				memory_enabled = 1;
				reg = 0;
			} else if (depth == 3 && reserved) {
				pool_reg = pool_size = pool_alloc = pool_hits = 0;
			}
			p = align4(after);
		} else if (token == FDT_END_NODE) {
			if (!depth)
				return FDT_RAM_INVALID;
			if (depth == 2 && memory && memory_enabled)
				result |= reg & FDT_RAM_CONTAINED;
			if (depth == 3 && reserved) {
				if (!pool_reg && !pool_size)
					return FDT_RAM_INVALID;
				if (!pool_reg && pool_size && (!pool_alloc || pool_hits))
					result |= FDT_RAM_DYNAMIC_POSSIBLE;
			}
			--depth;
		} else if (token == FDT_PROP) {
			if (!depth || limit - p < 8)
				return FDT_RAM_INVALID;
			uint32_t len = be32(p), nameoff = be32(p + 4);
			p += 8;
			if (len > limit - p || align4(p + len) > limit || nameoff >= strings_len ||
			    !bounded_string_end(str + nameoff, str + strings_len))
				return FDT_RAM_INVALID;
			const char *name = (const char *)(str + nameoff);
			if (depth == 1 || (depth == 2 && reserved)) {
				unsigned *cells = depth == 1 ? &root_cells : &reserved_cells;
				unsigned bit = streq(name, "#address-cells") ? 1 : streq(name, "#size-cells") ? 2 : 0;
				if (bit) {
					if ((*cells & bit) || len != 4 || be32(p) != 2)
						return FDT_RAM_INVALID;
					*cells |= bit;
				} else if (depth == 2 && streq(name, "ranges")) {
					if ((*cells & 4) || len)
						return FDT_RAM_INVALID;
					*cells |= 4;
				}
			} else if (depth == 2 && memory && streq(name, "reg")) {
				reg = range_evidence(p, len, base, end);
				if (reg < 0)
					return FDT_RAM_INVALID;
			} else if (depth == 2 && memory && streq(name, "status")) {
				if (bounded_string_end(p, p + len) != p + len)
					return FDT_RAM_INVALID;
				memory_enabled = streq((const char *)p, "ok") || streq((const char *)p, "okay");
			} else if (depth == 3 && reserved) {
				if (streq(name, "reg") || streq(name, "alloc-ranges")) {
					int match = range_evidence(p, len, base, end);
					if (match < 0)
						return FDT_RAM_INVALID;
					if (streq(name, "reg")) {
						pool_reg = 1;
						result |= match & FDT_RAM_FIXED_OVERLAP;
					} else {
						pool_alloc = 1;
						pool_hits |= match & FDT_RAM_FIXED_OVERLAP;
					}
				} else if (streq(name, "size")) {
					if (len != 8 || !be64(p))
						return FDT_RAM_INVALID;
					pool_size = 1;
				}
			}
			p = align4(p + len);
		} else if (token == FDT_END) {
			return !depth && roots == 1 && root_cells == 3 && reserved_cells == 7 && p == limit ?
			       result : FDT_RAM_INVALID;
		} else if (token != FDT_NOP) {
			return FDT_RAM_INVALID;
		}
	}
	return FDT_RAM_INVALID;
}
