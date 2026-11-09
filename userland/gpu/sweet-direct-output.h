/*
 * Geometry and checks for the direct panel output of sweet_k6a. Nothing here
 * touches the panel: a frame is written into a separate guarded buffer laid
 * out as the panel memory, and compared with what the b3a8 path shows.
 *
 * AppleCLCD presents the 1080x2400 panel as an iPhone X, 1125x2436, and shows
 * the centred part without scaling: the origin is (fOffX, fOffY) = (22, 18).
 * The panel memory is top row first, B, G, R, A bytes, 4320 bytes a row.
 */
#ifndef SWEET_DIRECT_OUTPUT_H
#define SWEET_DIRECT_OUTPUT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define SWEET_PANEL_WIDTH 1080
#define SWEET_PANEL_HEIGHT 2400
#define SWEET_PANEL_STRIDE 4320u
#define SWEET_LOGICAL_WIDTH 1125
#define SWEET_LOGICAL_HEIGHT 2436
#define SWEET_PANEL_CROP_X 22
#define SWEET_PANEL_CROP_Y 18
#define SWEET_PANEL_BYTES ((size_t)SWEET_PANEL_STRIDE * SWEET_PANEL_HEIGHT)
#define SWEET_DIRECT_GUARD 16384u
#define SWEET_DIRECT_BLOCK (SWEET_PANEL_BYTES + 2u * (size_t)SWEET_DIRECT_GUARD)
#define SWEET_DIRECT_GUARD_BYTE 0xa5
#define SWEET_DIRECT_BODY_BYTE 0xee

struct sweet_direct_rect {
	int src_x0, src_y0, src_x1, src_y1;
	int dst_x0, dst_y0, dst_x1, dst_y1;
};

/*
 * A row y read from the target is surface row (invert ? 2435 - y : y), and
 * panel row = surface row - 18. The buffer's rows are read back in order, so
 * buffer row 0 must be the panel's top row whichever way the target is drawn.
 */
static inline struct sweet_direct_rect
sweet_direct_crop(bool invert)
{
	struct sweet_direct_rect rect;

	rect.src_x0 = SWEET_PANEL_CROP_X;
	rect.src_x1 = SWEET_PANEL_CROP_X + SWEET_PANEL_WIDTH;
	rect.dst_x0 = 0;
	rect.dst_x1 = SWEET_PANEL_WIDTH;
	if (invert) {
		rect.src_y0 = SWEET_LOGICAL_HEIGHT - SWEET_PANEL_CROP_Y - SWEET_PANEL_HEIGHT;
		rect.src_y1 = SWEET_LOGICAL_HEIGHT - SWEET_PANEL_CROP_Y;
		rect.dst_y0 = SWEET_PANEL_HEIGHT;
		rect.dst_y1 = 0;
	} else {
		rect.src_y0 = SWEET_PANEL_CROP_Y;
		rect.src_y1 = SWEET_PANEL_CROP_Y + SWEET_PANEL_HEIGHT;
		rect.dst_y0 = 0;
		rect.dst_y1 = SWEET_PANEL_HEIGHT;
	}
	return rect;
}

/* The test frame: painted in order, a later rectangle wins. Each colour has
 * distinct channels so that a swapped channel cannot go unnoticed. */
struct sweet_direct_paint {
	int x, y, width, height;
	uint8_t rgba[4];
};

static const struct sweet_direct_paint sweet_direct_paints[] = {
	{ 0, 0, SWEET_PANEL_WIDTH, SWEET_PANEL_HEIGHT, { 0x10, 0x20, 0x30, 0xff } },
	{ 0, 0, SWEET_PANEL_WIDTH, 1, { 0xa0, 0x01, 0x02, 0xff } },
	{ 0, SWEET_PANEL_HEIGHT - 1, SWEET_PANEL_WIDTH, 1, { 0x03, 0xb0, 0x04, 0xff } },
	{ 0, 0, 1, SWEET_PANEL_HEIGHT, { 0x05, 0x06, 0xc0, 0xff } },
	{ SWEET_PANEL_WIDTH - 1, 0, 1, SWEET_PANEL_HEIGHT, { 0xd0, 0xe0, 0x07, 0xff } },
	{ 0, 0, 32, 32, { 0xf1, 0x11, 0x21, 0xff } },
	{ SWEET_PANEL_WIDTH - 32, 0, 32, 32, { 0x12, 0xf2, 0x22, 0xff } },
	{ 0, SWEET_PANEL_HEIGHT - 32, 32, 32, { 0x13, 0x23, 0xf3, 0xff } },
	{ SWEET_PANEL_WIDTH - 32, SWEET_PANEL_HEIGHT - 32, 32, 32, { 0xf4, 0xe4, 0xd4, 0xff } },
};
#define SWEET_DIRECT_PAINTS (sizeof(sweet_direct_paints) / sizeof(sweet_direct_paints[0]))

static inline const uint8_t *
sweet_direct_pattern(int x, int y)
{
	for (size_t i = SWEET_DIRECT_PAINTS; i-- > 1;) {
		const struct sweet_direct_paint *p = &sweet_direct_paints[i];

		if (x >= p->x && x < p->x + p->width && y >= p->y && y < p->y + p->height) {
			return p->rgba;
		}
	}
	return sweet_direct_paints[0].rgba;
}

struct sweet_direct_result {
	uint64_t mismatches, alpha_mismatches;
	uint32_t front_guard, back_guard;
	int first_x, first_y;
	uint8_t got[4], want[4];
};

static inline void
sweet_direct_prepare(uint8_t *block)
{
	memset(block, SWEET_DIRECT_GUARD_BYTE, SWEET_DIRECT_GUARD);
	memset(block + SWEET_DIRECT_GUARD, SWEET_DIRECT_BODY_BYTE, SWEET_PANEL_BYTES);
	memset(block + SWEET_DIRECT_GUARD + SWEET_PANEL_BYTES, SWEET_DIRECT_GUARD_BYTE, SWEET_DIRECT_GUARD);
}

static inline void
sweet_direct_begin(const uint8_t *block, struct sweet_direct_result *result)
{
	memset(result, 0, sizeof(*result));
	result->first_x = result->first_y = -1;
	for (size_t i = 0; i < SWEET_DIRECT_GUARD; i++) {
		result->front_guard += block[i] != SWEET_DIRECT_GUARD_BYTE;
		result->back_guard += block[SWEET_DIRECT_GUARD + SWEET_PANEL_BYTES + i] != SWEET_DIRECT_GUARD_BYTE;
	}
}

/* got and want are B, G, R, A bytes: colour differences count apart from opacity. */
static inline void
sweet_direct_note(struct sweet_direct_result *result, int x, int y, const uint8_t *got, const uint8_t *want)
{
	if (got[0] == want[0] && got[1] == want[1] && got[2] == want[2]) {
		result->alpha_mismatches += got[3] != want[3];
		return;
	}
	if (result->mismatches++ == 0) {
		result->first_x = x;
		result->first_y = y;
		memcpy(result->got, got, 4);
		memcpy(result->want, want, 4);
	}
}

static inline void
sweet_direct_check_pattern(const uint8_t *block, struct sweet_direct_result *result)
{
	const uint8_t *body = block + SWEET_DIRECT_GUARD;

	sweet_direct_begin(block, result);
	for (int y = 0; y < SWEET_PANEL_HEIGHT; y++) {
		const uint8_t *row = body + (size_t)y * SWEET_PANEL_STRIDE;

		for (int x = 0; x < SWEET_PANEL_WIDTH; x++, row += 4) {
			const uint8_t *rgba = sweet_direct_pattern(x, y);
			const uint8_t want[4] = { rgba[2], rgba[1], rgba[0], rgba[3] };

			sweet_direct_note(result, x, y, row, want);
		}
	}
}

/* What AppleCLCD would put on the panel from the b3a8 planes, pixel for pixel. */
static inline void
sweet_direct_compare_b3a8(const uint8_t *block, const uint8_t *rgb, size_t rgb_stride, const uint8_t *alpha,
    size_t alpha_stride, struct sweet_direct_result *result)
{
	const uint8_t *body = block + SWEET_DIRECT_GUARD;

	sweet_direct_begin(block, result);
	for (int y = 0; y < SWEET_PANEL_HEIGHT; y++) {
		const uint8_t *row = body + (size_t)y * SWEET_PANEL_STRIDE;
		const uint8_t *colours = rgb + (size_t)(y + SWEET_PANEL_CROP_Y) * rgb_stride + (size_t)SWEET_PANEL_CROP_X * 4;
		const uint8_t *opacities = alpha + (size_t)(y + SWEET_PANEL_CROP_Y) * alpha_stride + (size_t)SWEET_PANEL_CROP_X;

		for (int x = 0; x < SWEET_PANEL_WIDTH; x++, row += 4) {
			uint32_t word;
			uint8_t want[4];

			memcpy(&word, colours + (size_t)x * 4, sizeof(word));
			sweet_b3a8_unpack(word, opacities[x], want, 1);
			sweet_direct_note(result, x, y, row, want);
		}
	}
}

/* ---- damage: boxes are x, y, width, height in the target's own rows ------------------------- */

static inline void
sweet_damage_union(int box[4], bool *any, const int add[4])
{
	if (add[2] <= 0 || add[3] <= 0) {
		return;
	}
	if (!*any) {
		memcpy(box, add, sizeof(int) * 4);
		*any = true;
		return;
	}
	int x0 = box[0] < add[0] ? box[0] : add[0];
	int y0 = box[1] < add[1] ? box[1] : add[1];
	int x1 = box[0] + box[2] > add[0] + add[2] ? box[0] + box[2] : add[0] + add[2];
	int y1 = box[1] + box[3] > add[1] + add[3] ? box[1] + box[3] : add[1] + add[3];

	box[0] = x0;
	box[1] = y0;
	box[2] = x1 - x0;
	box[3] = y1 - y0;
}

/* A scissor box may reach outside the target. */
static inline void
sweet_damage_clamp(int box[4], int width, int height)
{
	int x0 = box[0] < 0 ? 0 : box[0], y0 = box[1] < 0 ? 0 : box[1];
	int x1 = box[0] + box[2] > width ? width : box[0] + box[2];
	int y1 = box[1] + box[3] > height ? height : box[1] + box[3];

	box[0] = x0;
	box[1] = y0;
	box[2] = x1 > x0 ? x1 - x0 : 0;
	box[3] = y1 > y0 ? y1 - y0 : 0;
}

/* Bins: up to 1%, 10%, 50%, under 100%, all of it. */
static inline unsigned
sweet_damage_bin(int64_t area, int64_t total)
{
	return area * 100 <= total ? 0 : area * 10 <= total ? 1 : area * 2 <= total ? 2 : area < total ? 3 : 4;
}

/*
 * Which pixels did this frame really change against what the surface holds?
 * It catches writes that no scissor box accounts for. `declared` is the box
 * claimed to cover them; NULL claims the whole target.
 */
struct sweet_changed {
	bool any;
	int box[4];
	uint64_t changed, outside;
};

static inline void
sweet_direct_changed(const uint8_t *rgba, int width, int height, bool invert, const uint8_t *rgb, size_t rgb_stride,
    const uint8_t *alpha, size_t alpha_stride, const int *declared, struct sweet_changed *out)
{
	int x0 = width, y0 = height, x1 = -1, y1 = -1;

	memset(out, 0, sizeof(*out));
	for (int y = 0; y < height; y++) {
		size_t row = (size_t)(invert ? height - 1 - y : y);
		const uint8_t *old_rgb = rgb + row * rgb_stride;
		const uint8_t *old_alpha = alpha != NULL ? alpha + row * alpha_stride : NULL;
		const uint8_t *now = rgba + (size_t)y * (size_t)width * 4;

		for (int x = 0; x < width; x++, now += 4) {
			uint32_t word = sweet_b3a8_pack(now), old;

			memcpy(&old, old_rgb + (size_t)x * 4, sizeof(old));
			if (word == old && (old_alpha == NULL || old_alpha[x] == now[3])) {
				continue;
			}
			out->changed++;
			x0 = x < x0 ? x : x0;
			y0 = y < y0 ? y : y0;
			x1 = x > x1 ? x : x1;
			y1 = y > y1 ? y : y1;
			if (declared != NULL && (x < declared[0] || y < declared[1] || x >= declared[0] + declared[2] ||
			    y >= declared[1] + declared[3])) {
				out->outside++;
			}
		}
	}
	if (out->changed != 0) {
		out->any = true;
		out->box[0] = x0;
		out->box[1] = y0;
		out->box[2] = x1 - x0 + 1;
		out->box[3] = y1 - y0 + 1;
	}
}

#endif
