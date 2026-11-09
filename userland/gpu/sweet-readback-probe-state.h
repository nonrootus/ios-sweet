/* State of the readback measurements (sweet-readback-probe.inc), kept in the EAGL context. */
#ifndef SWEET_READBACK_PROBE_STATE_H
#define SWEET_READBACK_PROBE_STATE_H

#include <stdbool.h>
#include <stdint.h>

#define SWEET_PROBE_SURFACES 12

struct sweet_probe_surface {
	uint32_t id, frames;
	uint32_t now_bins[5], eff_bins[5];
};

struct sweet_probe {
	unsigned round, step, verify_lines, direct_state;
	/* the interlaced fields: shadow even, shadow odd, panel even, panel odd */
	unsigned field_texture[4], field_framebuffer[4];
	unsigned stage, icon_lines;
	int field_bias;
	uint64_t frames_on_panel;
	unsigned pbo, texture, framebuffer;
	struct sweet_probe_surface surfaces[SWEET_PROBE_SURFACES];
	/* the last frame of the two-buffer swap chain: what the other buffer has that this one lacks */
	uint32_t previous_id;
	int previous_box[4];
	bool previous_any, previous_full;
};

#endif
