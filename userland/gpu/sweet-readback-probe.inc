/*
 * Where a b3a8 frame's time goes, one measurement per frame so that the window
 * server is never held up for long, and a first check of the direct panel
 * output against a separate guarded buffer. Included by eagl.c.
 *
 * The kernel's submit call waits for the GPU (a618_render_ring), so the submit
 * time below is the GPU's work and the wait for it together. What is left of a
 * glReadPixels after the driver's calls is CPU work: Mesa and the copy.
 */
#ifndef GL_MAP_READ_BIT
#define GL_MAP_READ_BIT 0x0001
#endif
#ifndef GL_STREAM_READ
#define GL_STREAM_READ 0x88e1
#endif
#ifndef GL_COLOR_WRITEMASK
#define GL_COLOR_WRITEMASK 0x0c23
#endif
#ifndef GL_TEXTURE_MIN_FILTER
#define GL_TEXTURE_MIN_FILTER 0x2801
#endif
#ifndef GL_TEXTURE_MAG_FILTER
#define GL_TEXTURE_MAG_FILTER 0x2800
#endif
#ifndef GL_NEAREST
#define GL_NEAREST 0x2600
#endif

enum {
	SWEET_PROBE_CONTROL, SWEET_PROBE_CLIENT_ROW, SWEET_PROBE_CLIENT_QUARTER, SWEET_PROBE_CLIENT_FULL,
	SWEET_PROBE_PBO_ROW, SWEET_PROBE_PBO_QUARTER, SWEET_PROBE_PBO_FULL,
	SWEET_PROBE_STEPS,
};

static const unsigned sweet_probe_round_frames[] = { 96 };
#define SWEET_PROBE_ROUNDS (sizeof(sweet_probe_round_frames) / sizeof(sweet_probe_round_frames[0]))

/*
 * The GPU pool hands out contiguous ranges and soon has no 12 MB run left, so
 * the panel-sized target is taken on the first display frame and given back as
 * soon as the check is done.
 */
enum { SWEET_DIRECT_NOT_STARTED, SWEET_DIRECT_TARGET_MADE, SWEET_DIRECT_BLIT_DONE, SWEET_DIRECT_DONE };

#define ULL(value) ((unsigned long long)(value))

static bool
sweet_probe_display(const struct image *image)
{
	return image->width == SWEET_LOGICAL_WIDTH && image->height == SWEET_LOGICAL_HEIGHT &&
	    IOSurfaceGetPixelFormat(image->surface) == SWEET_B3A8_FORMAT;
}

static bool
sweet_probe_eligible(const struct eagl *e, const struct image *image)
{
	return e->probe.round < SWEET_PROBE_ROUNDS && sweet_probe_display(image);
}

static void
sweet_probe_release(struct eagl *e)
{
	if (e->probe.framebuffer != 0) {
		glDeleteFramebuffers(1, &e->probe.framebuffer);
		e->probe.framebuffer = 0;
	}
	if (e->probe.texture != 0) {
		glDeleteTextures(1, &e->probe.texture);
		e->probe.texture = 0;
	}
}

static void
sweet_probe_advance(struct eagl *e)
{
	if (++e->probe.step < SWEET_PROBE_STEPS) {
		return;
	}
	e->probe.step = 0;
	e->probe.round++;
	if (e->probe.pbo != 0) {
		glDeleteBuffers(1, &e->probe.pbo);
		e->probe.pbo = 0;
	}
}

static uint64_t
sweet_probe_sync(void)
{
	uint64_t started = now_us();
	GLsync sync = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);

	if (sync != NULL) {
		(void)glClientWaitSync(sync, GL_SYNC_FLUSH_COMMANDS_BIT, 2000000000ull);
		glDeleteSync(sync);
	}
	return now_us() - started;
}

static uint64_t
sweet_probe_calls_us(const struct sweet_gpu_calls *a, const struct sweet_gpu_calls *b)
{
	return (b->submit_us - a->submit_us) + (b->gem_new_us - a->gem_new_us) + (b->cpu_prep_us - a->cpu_prep_us) +
	    (b->wait_us - a->wait_us) + (b->map_us - a->map_us);
}

static void
sweet_probe_control(struct eagl *e, const struct image *image)
{
	size_t bytes = (size_t)image->width * (size_t)image->height * 4;
	uint8_t *copy = malloc(bytes);
	uint64_t passes[3] = { 0, 0, 0 };

	if (copy == NULL) {
		say("SWEET RB: control has no memory\n");
		return;
	}
	memset(copy, 0x5a, bytes);
	for (unsigned i = 0; i < 3; i++) {
		uint64_t started = now_us();

		memcpy(copy, e->b3a8_pixels, bytes);
		passes[i] = now_us() - started;
	}
	say("SWEET RB: round=%u frame=%u control bytes=%zu cached_memcpy=%llu,%llu,%llu first=%u\n", e->probe.round, e->frames,
	    bytes, ULL(passes[0]), ULL(passes[1]), ULL(passes[2]), copy[0]);
	free(copy);
}

static void
sweet_probe_client(struct eagl *e, const struct image *image, GLsizei rows)
{
	struct sweet_gpu_calls before, after;
	uint64_t finish_started = now_us();

	glFinish();
	uint64_t finish = now_us() - finish_started;
	sweet_gpu_calls_sample(&before);
	uint64_t started = now_us();

	glReadPixels(0, 0, image->width, rows, GL_RGBA, GL_UNSIGNED_BYTE, e->b3a8_pixels);
	uint64_t read = now_us() - started;
	sweet_gpu_calls_sample(&after);
	GLenum error = glGetError();
	uint64_t calls = sweet_probe_calls_us(&before, &after);

	say("SWEET RB: round=%u frame=%u surface=%u client rows=%d finish=%llu read=%llu submit=%llu/%u gemnew=%llu/%u prep=%llu/%u "
	    "fence=%llu/%u map=%llu/%u cpu=%llu err=%#x\n", e->probe.round, e->frames, IOSurfaceGetID(image->surface), rows, ULL(finish),
	    ULL(read), ULL(after.submit_us - before.submit_us), after.submits - before.submits,
	    ULL(after.gem_new_us - before.gem_new_us), after.gem_news - before.gem_news,
	    ULL(after.cpu_prep_us - before.cpu_prep_us), after.cpu_preps - before.cpu_preps,
	    ULL(after.wait_us - before.wait_us), after.waits - before.waits,
	    ULL(after.map_us - before.map_us), after.maps - before.maps,
	    ULL(read > calls ? read - calls : 0), error);
}

static void
sweet_probe_pbo(struct eagl *e, const struct image *image, GLsizei rows)
{
	size_t bytes = (size_t)image->width * (size_t)rows * 4;
	size_t full = (size_t)image->width * (size_t)image->height * 4;
	struct sweet_gpu_calls before, after;
	uint64_t made_started = now_us();

	if (e->probe.pbo == 0) {
		glGenBuffers(1, &e->probe.pbo);
		glBindBuffer(GL_PIXEL_PACK_BUFFER, e->probe.pbo);
		glBufferData(GL_PIXEL_PACK_BUFFER, (GLsizeiptr)full, NULL, GL_STREAM_READ);
	} else {
		glBindBuffer(GL_PIXEL_PACK_BUFFER, e->probe.pbo);
	}
	uint64_t made = now_us() - made_started;

	glFinish();
	sweet_gpu_calls_sample(&before);
	uint64_t started = now_us();

	glReadPixels(0, 0, image->width, rows, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	uint64_t request = now_us() - started;
	uint64_t wait = sweet_probe_sync();
	sweet_gpu_calls_sample(&after);
	GLenum error = glGetError();
	uint64_t map_started = now_us();
	void *mapped = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, (GLsizeiptr)bytes, GL_MAP_READ_BIT);
	uint64_t map = now_us() - map_started;
	uint64_t copy[2] = { 0, 0 }, unmap = 0;

	if (mapped != NULL) {
		for (unsigned i = 0; i < 2; i++) {
			uint64_t copy_started = now_us();

			memcpy(e->b3a8_pixels, mapped, bytes);
			copy[i] = now_us() - copy_started;
		}
		uint64_t unmap_started = now_us();

		glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
		unmap = now_us() - unmap_started;
	}
	glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
	say("SWEET RB: round=%u frame=%u surface=%u pbo rows=%d made=%llu request=%llu sync=%llu map=%llu copy=%llu,%llu unmap=%llu "
	    "submit=%llu/%u gemnew=%llu/%u bytes=%zu mapped=%d err=%#x,%#x\n", e->probe.round, e->frames, IOSurfaceGetID(image->surface),
	    rows, ULL(made), ULL(request), ULL(wait), ULL(map), ULL(copy[0]), ULL(copy[1]), ULL(unmap),
	    ULL(after.submit_us - before.submit_us), after.submits - before.submits,
	    ULL(after.gem_new_us - before.gem_new_us), after.gem_news - before.gem_news, bytes, mapped != NULL, error, glGetError());
}

/* A panel-sized render target of our own, kept apart from everything CoreAnimation sees. */
static bool
sweet_probe_target(struct eagl *e)
{
	GLint texture = 0, draw = 0;

	if (e->probe.framebuffer != 0) {
		return true;
	}
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
	glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw);
	glGenTextures(1, &e->probe.texture);
	glBindTexture(GL_TEXTURE_2D, e->probe.texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, SWEET_PANEL_WIDTH, SWEET_PANEL_HEIGHT, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glGenFramebuffers(1, &e->probe.framebuffer);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, e->probe.framebuffer);
	glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, e->probe.texture, 0);
	GLenum status = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);

	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)draw);
	glBindTexture(GL_TEXTURE_2D, (GLuint)texture);
	if (status != GL_FRAMEBUFFER_COMPLETE) {
		say("SWEET DIRECT: target incomplete status=%#x\n", status);
		sweet_probe_release(e);
		return false;
	}
	return true;
}

/* The GPU's part of the direct path: the crop, into a target of the panel's size. */
static void
sweet_probe_blit(struct eagl *e, const struct image *image)
{
	if (!sweet_probe_target(e)) {
		return;
	}
	struct sweet_direct_rect crop = sweet_direct_crop(image->invert);
	struct sweet_gpu_calls before, after;
	GLint draw = 0;
	GLboolean scissored = glIsEnabled(GL_SCISSOR_TEST);

	glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw);
	glDisable(GL_SCISSOR_TEST);
	glFinish();
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, e->probe.framebuffer);
	sweet_gpu_calls_sample(&before);
	uint64_t started = now_us();

	glBlitFramebuffer(crop.src_x0, crop.src_y0, crop.src_x1, crop.src_y1, crop.dst_x0, crop.dst_y0, crop.dst_x1, crop.dst_y1,
	    GL_COLOR_BUFFER_BIT, GL_NEAREST);
	uint64_t blit = now_us() - started;
	started = now_us();
	glFlush();
	uint64_t flush = now_us() - started;
	uint64_t wait = sweet_probe_sync();
	sweet_gpu_calls_sample(&after);
	GLenum error = glGetError();

	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)draw);
	(scissored ? glEnable : glDisable)(GL_SCISSOR_TEST);
	say("SWEET RB: round=%u frame=%u surface=%u crop-blit blit=%llu flush=%llu sync=%llu submit=%llu/%u err=%#x\n", e->probe.round,
	    e->frames, IOSurfaceGetID(image->surface), ULL(blit), ULL(flush), ULL(wait), ULL(after.submit_us - before.submit_us),
	    after.submits - before.submits, error);
}

/* Before the frame's own read: the read framebuffer is the target and packing state is plain. */
static void
sweet_probe_area_step(struct eagl *e, const struct image *image)
{
	GLsizei quarter = image->height / 4;

	if (sweet_probe_display(image) && e->probe.direct_state == SWEET_DIRECT_NOT_STARTED) {
		e->probe.direct_state = sweet_probe_target(e) ? SWEET_DIRECT_TARGET_MADE : SWEET_DIRECT_DONE;
		return;
	}
	if (sweet_probe_display(image) && e->probe.direct_state == SWEET_DIRECT_TARGET_MADE && e->frames >= 1) {
		sweet_probe_blit(e, image);
		e->probe.direct_state = SWEET_DIRECT_BLIT_DONE;
		return;
	}
	if (!sweet_probe_eligible(e, image) || (e->probe.step == 0 && e->frames < sweet_probe_round_frames[e->probe.round])) {
		return;
	}
	switch (e->probe.step) {
	case SWEET_PROBE_CONTROL:
		sweet_probe_control(e, image);
		break;
	case SWEET_PROBE_CLIENT_ROW:
		sweet_probe_client(e, image, 1);
		break;
	case SWEET_PROBE_CLIENT_QUARTER:
		sweet_probe_client(e, image, quarter);
		break;
	case SWEET_PROBE_CLIENT_FULL:
		sweet_probe_client(e, image, image->height);
		break;
	case SWEET_PROBE_PBO_ROW:
		sweet_probe_pbo(e, image, 1);
		break;
	case SWEET_PROBE_PBO_QUARTER:
		sweet_probe_pbo(e, image, quarter);
		break;
	case SWEET_PROBE_PBO_FULL:
		sweet_probe_pbo(e, image, image->height);
		break;
	default:
		return;
	}
	sweet_probe_advance(e);
}

static void
sweet_probe_say_result(const char *what, unsigned round, unsigned frame, int invert, const struct sweet_direct_result *r, GLenum error, int fallback)
{
	say("SWEET DIRECT: %s round=%u frame=%u invert=%d mismatches=%llu alpha=%llu guards=%u,%u first=%d,%d got=%02x%02x%02x%02x "
	    "want=%02x%02x%02x%02x err=%#x rgba_fallback=%d\n", what, round, frame, invert, ULL(r->mismatches), ULL(r->alpha_mismatches),
	    r->front_guard, r->back_guard, r->first_x, r->first_y, r->got[0], r->got[1], r->got[2], r->got[3], r->want[0], r->want[1],
	    r->want[2], r->want[3], error, fallback);
}

/* Read the panel-sized target out as the panel's memory. */
static GLenum
sweet_probe_read_panel(uint8_t *block, int *fallback)
{
	uint8_t *body = block + SWEET_DIRECT_GUARD;

	sweet_direct_prepare(block);
	glPixelStorei(GL_PACK_ROW_LENGTH, SWEET_PANEL_WIDTH);
	glPixelStorei(GL_PACK_ALIGNMENT, 4);
	glGetError();
	glReadPixels(0, 0, SWEET_PANEL_WIDTH, SWEET_PANEL_HEIGHT, GL_BGRA, GL_UNSIGNED_BYTE, body);
	GLenum error = glGetError();

	*fallback = 0;
	if (error != GL_NO_ERROR) {
		*fallback = 1;
		sweet_direct_prepare(block);
		glReadPixels(0, 0, SWEET_PANEL_WIDTH, SWEET_PANEL_HEIGHT, GL_RGBA, GL_UNSIGNED_BYTE, body);
		for (size_t at = 0; at < SWEET_PANEL_BYTES; at += 4) {
			uint8_t red = body[at];

			body[at] = body[at + 2];
			body[at + 2] = red;
		}
		error = glGetError();
	}
	return error;
}

/*
 * After the frame has been packed, with the surface locked: first a known
 * picture, then this very frame through the crop the panel would get, against
 * the b3a8 planes. Nothing here is shown.
 */
static void
sweet_probe_direct_step(struct eagl *e, const struct image *image, const struct xr10_planes *planes)
{
	if (!sweet_probe_display(image) || e->probe.direct_state != SWEET_DIRECT_BLIT_DONE || e->frames < 2) {
		return;
	}
	e->probe.direct_state = SWEET_DIRECT_DONE;

	uint8_t *block = malloc(SWEET_DIRECT_BLOCK);

	if (block == NULL || planes->alpha == NULL || !sweet_probe_target(e)) {
		say("SWEET DIRECT: step skipped\n");
		free(block);
		sweet_probe_release(e);
		return;
	}
	struct sweet_direct_result result;
	struct sweet_direct_rect crop = sweet_direct_crop(image->invert);
	GLint read = 0, draw = 0, box[4] = { 0, 0, 0, 0 }, saved[5];
	GLfloat colour[4] = { 0, 0, 0, 0 };
	GLboolean mask[4] = { 1, 1, 1, 1 };
	GLboolean scissored = glIsEnabled(GL_SCISSOR_TEST);
	GLenum error;
	int fallback = 0;

	glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read);
	glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw);
	glGetIntegerv(GL_SCISSOR_BOX, box);
	glGetFloatv(GL_COLOR_CLEAR_VALUE, colour);
	glGetBooleanv(GL_COLOR_WRITEMASK, mask);
	b3a8_transfer_state(true, saved, false);

	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, e->probe.framebuffer);
	glColorMask(1, 1, 1, 1);
	glEnable(GL_SCISSOR_TEST);
	for (size_t i = 0; i < SWEET_DIRECT_PAINTS; i++) {
		const struct sweet_direct_paint *paint = &sweet_direct_paints[i];

		glScissor(paint->x, paint->y, paint->width, paint->height);
		glClearColor(paint->rgba[0] / 255.0f, paint->rgba[1] / 255.0f, paint->rgba[2] / 255.0f, paint->rgba[3] / 255.0f);
		glClear(GL_COLOR_BUFFER_BIT);
	}
	glFinish();
	glBindFramebuffer(GL_READ_FRAMEBUFFER, e->probe.framebuffer);
	error = sweet_probe_read_panel(block, &fallback);
	sweet_direct_check_pattern(block, &result);
	sweet_probe_say_result("pattern", e->probe.round, e->frames, image->invert, &result, error, fallback);

	glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)read);
	glDisable(GL_SCISSOR_TEST);
	glBlitFramebuffer(crop.src_x0, crop.src_y0, crop.src_x1, crop.src_y1, crop.dst_x0, crop.dst_y0, crop.dst_x1, crop.dst_y1,
	    GL_COLOR_BUFFER_BIT, GL_NEAREST);
	glFinish();
	glBindFramebuffer(GL_READ_FRAMEBUFFER, e->probe.framebuffer);
	error = sweet_probe_read_panel(block, &fallback);
	sweet_direct_compare_b3a8(block, planes->rgb, planes->rgb_stride, planes->alpha, planes->alpha_stride, &result);
	sweet_probe_say_result("frame", e->probe.round, e->frames, image->invert, &result, error, fallback);

	b3a8_transfer_state(true, saved, true);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)read);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)draw);
	glScissor(box[0], box[1], box[2], box[3]);
	(scissored ? glEnable : glDisable)(GL_SCISSOR_TEST);
	glClearColor(colour[0], colour[1], colour[2], colour[3]);
	glColorMask(mask[0], mask[1], mask[2], mask[3]);
	free(block);
	sweet_probe_release(e);
}

/* ---- damage ------------------------------------------------------------------------------- */

static void
sweet_probe_damage_note(struct image *image)
{
	GLint box[4] = { 0, 0, 0, 0 };

	if (image->damage_full) {
		return;
	}
	if (!glIsEnabled(GL_SCISSOR_TEST)) {
		image->damage_full = true;
		return;
	}
	glGetIntegerv(GL_SCISSOR_BOX, box);
	sweet_damage_clamp(box, image->width, image->height);
	sweet_damage_union(image->damage, &image->damage_any, box);
}

static struct sweet_probe_surface *
sweet_probe_surface_record(struct eagl *e, uint32_t id)
{
	for (unsigned i = 0; i < SWEET_PROBE_SURFACES; i++) {
		struct sweet_probe_surface *record = &e->probe.surfaces[i];

		if (record->id == id || record->id == 0) {
			record->id = id;
			return record;
		}
	}
	return NULL;
}

/*
 * What the frame drew into a target, and what its copy-out would have to take
 * with two buffers in turn: this frame's box and the one the other buffer got.
 * A freshly attached target has nothing to build on and counts as all of it.
 */
static void
sweet_probe_damage_close(struct eagl *e, struct image *image)
{
	int64_t total = (int64_t)image->width * image->height;
	uint32_t id = IOSurfaceGetID(image->surface);
	bool chain = IOSurfaceGetPixelFormat(image->surface) == SWEET_B3A8_FORMAT;
	bool full = image->damage_full, any = image->damage_any;
	int box[4];

	memcpy(box, image->damage, sizeof(box));
	bool effective_full = full || !image->damage_seen;
	bool effective_any = any;
	int effective[4];

	memcpy(effective, box, sizeof(effective));
	if (!effective_full && chain && e->probe.previous_id != 0 && e->probe.previous_id != id) {
		effective_full = e->probe.previous_full;
		if (e->probe.previous_any) {
			sweet_damage_union(effective, &effective_any, e->probe.previous_box);
		}
	}
	memcpy(image->damage_effective, effective, sizeof(effective));
	image->damage_effective_full = effective_full || !effective_any;
	if (chain) {
		e->probe.previous_id = id;
		e->probe.previous_any = any;
		e->probe.previous_full = full;
		memcpy(e->probe.previous_box, box, sizeof(box));
	}
	image->damage_any = image->damage_full = false;
	image->damage_seen = true;

	struct sweet_probe_surface *record = total >= 65536 ? sweet_probe_surface_record(e, id) : NULL;

	if (record == NULL) {
		return;
	}
	int64_t area = full ? total : any ? (int64_t)box[2] * box[3] : 0;
	int64_t effective_area = effective_full ? total : effective_any ? (int64_t)effective[2] * effective[3] : 0;

	record->frames++;
	record->now_bins[sweet_damage_bin(area, total)]++;
	record->eff_bins[sweet_damage_bin(effective_area, total)]++;
	if (record->frames <= 2 || record->frames % 64 == 0) {
		say("SWEET DMG: surface=%u %dx%d invert=%d frames=%u now=%u,%u,%u,%u,%u eff=%u,%u,%u,%u,%u last=%d,%d %dx%d full=%d "
		    "eff=%d,%d %dx%d full=%d\n", id, image->width, image->height, image->invert, record->frames, record->now_bins[0],
		    record->now_bins[1], record->now_bins[2], record->now_bins[3], record->now_bins[4], record->eff_bins[0], record->eff_bins[1],
		    record->eff_bins[2], record->eff_bins[3], record->eff_bins[4], box[0], box[1], box[2], box[3], full, effective[0],
		    effective[1], effective[2], effective[3], effective_full);
	}
}

/* Before packing: the surface still holds its own last frame. Does the box tell the truth? */
static void
sweet_probe_verify_damage(struct eagl *e, const struct image *image, const struct xr10_planes *planes, const uint8_t *pixels)
{
	struct sweet_changed changed;

	if (planes->alpha == NULL || (e->frames & 7) != 0 || e->probe.verify_lines >= 256 || image->width != SWEET_LOGICAL_WIDTH ||
	    image->height != SWEET_LOGICAL_HEIGHT) {
		return;
	}
	uint64_t started = now_us();

	sweet_direct_changed(pixels, image->width, image->height, image->invert, planes->rgb, planes->rgb_stride, planes->alpha,
	    planes->alpha_stride, image->damage_effective_full ? NULL : image->damage_effective, &changed);
	e->probe.verify_lines++;
	say("SWEET DMG: verify surface=%u frame=%u us=%llu changed=%llu outside=%llu true=%d,%d %dx%d declared=%d,%d %dx%d full=%d\n",
	    IOSurfaceGetID(image->surface), e->frames, ULL(now_us() - started), ULL(changed.changed), ULL(changed.outside), changed.box[0],
	    changed.box[1], changed.box[2], changed.box[3], image->damage_effective[0], image->damage_effective[1],
	    image->damage_effective[2], image->damage_effective[3], image->damage_effective_full);
}
