/* Included by arm_vm_init.c so this probe can inspect the physical aperture.
 * Called after VM startup. It only translates addresses and never accesses
 * the contents of the reserved pages. */
void sweet_a618_zap_reserve_probe(void);

void
sweet_a618_zap_reserve_probe(void)
{
	DTEntry chosen;
	const void *property;
	unsigned int length;
	uint64_t reserve[2];
	const pmap_paddr_t base = 0x1fca00000ull;
	const vm_size_t size = 0x100000u;
	vm_offset_t tail = 0;

	if (SecureDTLookupEntry(NULL, "/chosen", &chosen) != kSuccess ||
	    SecureDTGetProperty(chosen, "sweet-a618-zap-reserve", &property, &length) != kSuccess) {
		return;
	}
	if (length != sizeof(reserve)) {
		printf("SWEET A618 ZAP: invalid reserve descriptor size=%u\n", length);
		return;
	}
	memcpy(reserve, property, sizeof(reserve));
	if (reserve[0] != base || reserve[1] != size || gPhysBase != 0x100000000ull ||
	    real_avail_end != base || avail_end != base || !kva_active || PAGE_SIZE != 0x4000u) {
		printf("SWEET A618 ZAP: reserve extent mismatch managed_end=%llx real_end=%llx\n",
		    (unsigned long long)avail_end, (unsigned long long)real_avail_end);
		return;
	}
	for (size_t i = 0; i < PTOV_TABLE_SIZE; ++i) {
		const ptov_table_entry *entry = &ptov_table[i];
		if (entry->len == 0) {
			continue;
		}
		pmap_paddr_t end = entry->pa + entry->len;
		if (end < entry->pa || (entry->pa < base + size && end > base)) {
			printf("SWEET A618 ZAP: physical aperture overlaps reserve\n");
			return;
		}
		if (end == base) {
			tail = entry->va + entry->len;
		}
	}
	if (!tail || !pmap_valid_page(atop(base - PAGE_SIZE)) ||
	    kvtophys(tail - PAGE_SIZE) != base - PAGE_SIZE) {
		printf("SWEET A618 ZAP: adjacent managed page check failed\n");
		return;
	}
	const vm_offset_t linear = gVirtBase + (base - gPhysBase);
	for (vm_size_t offset = 0; offset < size; offset += PAGE_SIZE) {
		if (pmap_valid_page(atop(base + offset)) || kvtophys(tail + offset) ||
		    kvtophys(linear + offset)) {
			printf("SWEET A618 ZAP: reserve page owned or mapped offset=%lx\n", (unsigned long)offset);
			return;
		}
	}
	printf("SWEET A618 ZAP: reserve verified pa=%llx bytes=%lx pages=%lu\n",
	    (unsigned long long)base, (unsigned long)size, (unsigned long)(size / PAGE_SIZE));
}
