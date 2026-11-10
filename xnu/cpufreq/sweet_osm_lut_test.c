/* Host test for sweet_osm_lut.inc. Build: cc -std=c11 -Wall -Wextra -Werror -I. sweet_osm_lut_test.c */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "sweet_osm_lut.inc"

static uint32_t
row(uint32_t source, uint32_t cores, uint32_t multiple)
{
	return source << 30 | cores << 16 | multiple;
}

int
main(void)
{
	uint32_t words[SWEET_OSM_ROWS] = { 0 };
	struct sweet_osm_table t;

	/* A55-like: 300 MHz alternate row, then XO multiples, a boost row, then the repeated end. */
	words[0] = row(0, 6, 0);           /* 300 MHz */
	words[1] = row(1, 6, 30);          /* 576 MHz */
	words[2] = row(1, 6, 50);          /* 960 MHz */
	words[3] = row(1, 6, 94);          /* 1804.8 MHz */
	words[4] = row(1, 1, 110);         /* 2112 MHz boost, single core */
	words[5] = row(1, 1, 110);         /* end */
	for (unsigned i = 6; i < SWEET_OSM_ROWS; i++) {
		words[i] = words[5];
	}
	assert(sweet_osm_parse(words, 1, &t));
	assert(t.rows == 5 && t.low == 0 && t.high == 3);
	assert(t.khz[0] == 300000 && t.khz[3] == 1804800 && t.khz[4] == 2112000);
	/* Without boost rows (L3), the single-core flag means nothing. */
	assert(sweet_osm_parse(words, 0, &t) && t.high == 4);

	/* The table ends at the first repeated frequency even if later rows differ. */
	words[2] = words[1];
	assert(sweet_osm_parse(words, 1, &t) && t.rows == 2 && t.low == 0 && t.high == 1);

	/* Nothing usable: only boost rows. */
	for (unsigned i = 0; i < SWEET_OSM_ROWS; i++) {
		words[i] = row(1, 1, (uint32_t)(40 + i));
	}
	assert(!sweet_osm_parse(words, 1, &t) && t.rows == SWEET_OSM_ROWS);

	/* Out of range frequencies are skipped: 19.2 MHz * 255 = 4896 MHz. */
	words[0] = row(1, 2, 255);
	words[1] = row(1, 2, 100);
	words[2] = row(1, 2, 100);
	assert(sweet_osm_parse(words, 1, &t) && t.rows == 2 && t.low == 1 && t.high == 1);
	printf("PASS OSM table: alternate row, boost rows, end of table, empty and out-of-range tables\n");
	return 0;
}
