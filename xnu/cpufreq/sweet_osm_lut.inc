/*
 * OSM frequency table rows, shared by the kernel (osfmk/arm64/sweet_cpufreq.c) and the host
 * test (xnu/cpufreq/sweet_osm_lut_test.c). Layout from Xiaomi 10c33d40
 * drivers/clk/qcom/clk-cpu-osm.c: row i is the word at 0x110 + 32 * i; bits 31:30 are the
 * clock source (0: a fixed 300 MHz), bits 18:16 the core count (1: a single-core boost row),
 * bits 7:0 the multiple of 19.2 MHz. Two rows with the same frequency end the table.
 */
#define SWEET_OSM_ROWS          40u
#define SWEET_OSM_XO_KHZ        19200u
#define SWEET_OSM_ALTERNATE_KHZ 300000u

struct sweet_osm_table {
	uint32_t khz[SWEET_OSM_ROWS];
	unsigned rows, low, high;
};

/* Returns 1 with low and high set to the slowest and fastest usable row, 0 if there is none. */
static int
sweet_osm_parse(const uint32_t *words, int boost_rows, struct sweet_osm_table *table)
{
	int found = 0;

	table->rows = 0;
	table->low = 0;
	table->high = 0;
	for (unsigned i = 0; i < SWEET_OSM_ROWS; i++) {
		uint32_t word = words[i];
		uint32_t khz = (word >> 30) != 0 ? SWEET_OSM_XO_KHZ * (word & 0xffu) : SWEET_OSM_ALTERNATE_KHZ;

		if (i > 0 && khz == table->khz[i - 1]) {
			break;
		}
		table->khz[i] = khz;
		table->rows = i + 1;
		if (boost_rows && ((word >> 16) & 7u) == 1u) {
			continue;
		}
		if (khz < SWEET_OSM_ALTERNATE_KHZ || khz > 3500000u) {
			continue;
		}
		if (!found || khz < table->khz[table->low]) {
			table->low = i;
		}
		if (!found || khz > table->khz[table->high]) {
			table->high = i;
		}
		found = 1;
	}
	return found;
}
