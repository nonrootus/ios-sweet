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

/*
 * The area measurements have done their work (0181d): a full read is 278 ms of CPU copy from
 * write-combining memory. They take 12 MB of a short pool, so they stay out of a normal run.
 * The test builds them in.
 */
#ifndef SWEET_PROBE_ROUNDS
#define SWEET_PROBE_ROUNDS 0
#endif
#ifndef SWEET_PROBE_VERIFY_DAMAGE
#define SWEET_PROBE_VERIFY_DAMAGE 0
#endif
#ifndef SWEET_DIRECT_STAGE_ZERO
#define SWEET_DIRECT_STAGE_ZERO 0
#endif
/*
 * Every frame straight to the panel. The checks below passed on this panel (0182: shadow and
 * stripe with no difference, panel read with none), so a normal build does not repeat them and
 * spend 10 MB of the pool on the test buffer. The test builds them in.
 */
#ifndef SWEET_DIRECT_PANEL_FRAMES
#define SWEET_DIRECT_PANEL_FRAMES 1
#endif
#ifndef SWEET_DIRECT_CHECKS
#define SWEET_DIRECT_CHECKS 0
#endif
static const unsigned sweet_probe_round_frames[] = { 96 };

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
	uint32_t format = IOSurfaceGetPixelFormat(image->surface);

	return image->width == SWEET_LOGICAL_WIDTH && image->height == SWEET_LOGICAL_HEIGHT &&
	    (format == SWEET_B3A8_FORMAT || format == 'BGRA');
}

static bool
sweet_probe_eligible(const struct eagl *e, const struct image *image)
{
	return e->probe.round < (unsigned)SWEET_PROBE_ROUNDS && sweet_probe_display(image);
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

	if (SWEET_DIRECT_STAGE_ZERO && sweet_probe_display(image) && e->probe.direct_state == SWEET_DIRECT_NOT_STARTED) {
		e->probe.direct_state = sweet_probe_target(e) ? SWEET_DIRECT_TARGET_MADE : SWEET_DIRECT_DONE;
		return;
	}
	if (SWEET_DIRECT_STAGE_ZERO && sweet_probe_display(image) && e->probe.direct_state == SWEET_DIRECT_TARGET_MADE && e->frames >= 1) {
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

/*
 * ---- the interlaced fields ---------------------------------------------------------------------
 * The panel's stride, 4320, is not a multiple of the 64 bytes the GPU counts pitch in; 8640 is. A
 * frame goes out as two fields of 1200 rows: the even panel rows from the buffer's start, the odd
 * from 4288 bytes in and drawn from column 8, which lands on row 1, column 0. Each check has to
 * pass before the next is tried: a buffer of the panel's shape in the GPU pool, reading the panel
 * through the GPU, then a stripe of colours on the panel. Nothing here replaces the b3a8 path.
 */
#define SWEET_FIELD_ID 0xf5ee0000u
#define SWEET_FIELD_SELECTOR 0x7fu
#define SWEET_FIELD_ROWS 1200
#define SWEET_FIELD_ODD_X 8
#define SWEET_STRIPE_ROWS 64

struct sweet_field_request {
	uint32_t op, surface;
	uint64_t user, bytes, result;
};
enum { SWEET_FIELD_COPY_SHADOW = 1, SWEET_FIELD_PRESENTED = 2, SWEET_FIELD_COPY_PANEL = 3, SWEET_FIELD_IS_DISPLAY = 4 };
enum { SWEET_STAGE_SHADOW, SWEET_STAGE_PANEL_READ, SWEET_STAGE_STRIPE, SWEET_STAGE_DONE, SWEET_STAGE_FAILED };
static const unsigned sweet_stage_frames[] = { 24, 32, 44 };

static bool
sweet_field_call(uint32_t op, uint32_t surface, void *user, size_t bytes, uint64_t *result)
{
	struct sweet_field_request request = { op, surface, (uint64_t)(uintptr_t)user, bytes, 0 };

	if (sweet_gpu_call(SWEET_FIELD_SELECTOR, &request, sizeof(request)) != 0) {
		return false;
	}
	if (result != NULL) {
		*result = request.result;
	}
	return true;
}

static void
sweet_fields_release(struct eagl *e, unsigned panel)
{
	for (unsigned i = panel * 2; i < panel * 2 + 2; i++) {
		if (e->probe.field_framebuffer[i] != 0) {
			glDeleteFramebuffers(1, &e->probe.field_framebuffer[i]);
			e->probe.field_framebuffer[i] = 0;
		}
		if (e->probe.field_texture[i] != 0) {
			glDeleteTextures(1, &e->probe.field_texture[i]);
			e->probe.field_texture[i] = 0;
		}
	}
}

static bool
sweet_fields_ready(struct eagl *e, unsigned panel)
{
	GLint texture = 0, draw = 0;
	bool ok = true;

	if (e->probe.field_framebuffer[panel * 2] != 0 && e->probe.field_framebuffer[panel * 2 + 1] != 0) {
		return true;
	}
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
	glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw);
	for (unsigned f = 0; f < 2 && ok; f++) {
		unsigned index = panel * 2 + f;

		glGenTextures(1, &e->probe.field_texture[index]);
		glBindTexture(GL_TEXTURE_2D, e->probe.field_texture[index]);
		ok = redwood_gl_surface_texture(e->gl, SWEET_FIELD_ID + index) != 0;
		if (ok) {
			glGenFramebuffers(1, &e->probe.field_framebuffer[index]);
			glBindFramebuffer(GL_DRAW_FRAMEBUFFER, e->probe.field_framebuffer[index]);
			glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, e->probe.field_texture[index], 0);
			ok = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
		}
	}
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)draw);
	glBindTexture(GL_TEXTURE_2D, (GLuint)texture);
	if (!ok) {
		say("SWEET DIRECT: fields of %s not usable\n", panel ? "the panel" : "the shadow");
		sweet_fields_release(e, panel);
	}
	return ok;
}

/*
 * A 2:1 NEAREST blit samples each destination row at its centre, source row s0 + 2d + 1, so even
 * rows start one above the first wanted row. `bias` is what the check found to hold on this GPU.
 */
#ifndef SWEET_FIELDS_WAIT
/*
 * 0: the flush is the end of it. The kernel's submit returns once the GPU has finished, so
 * waiting on a fence afterwards only added a second trip through the kernel to every frame.
 */
#define SWEET_FIELDS_WAIT 0
#endif

static void
sweet_fields_blit(struct eagl *e, unsigned panel, int bias)
{
	static unsigned logged;
	struct sweet_gpu_calls before, after;
	uint64_t started = now_us(), waited = 0;
	GLint draw = 0, box[4] = { 0, 0, 0, 0 };
	GLboolean scissored = glIsEnabled(GL_SCISSOR_TEST);

	sweet_gpu_calls_sample(&before);

	glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw);
	glGetIntegerv(GL_SCISSOR_BOX, box);
	glEnable(GL_SCISSOR_TEST);
	for (int f = 0; f < 2; f++) {
		int x0 = f ? SWEET_FIELD_ODD_X : 0;
		int s0 = SWEET_PANEL_CROP_Y - 1 + f + bias;

		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, e->probe.field_framebuffer[panel * 2 + (unsigned)f]);
		glScissor(x0, 0, SWEET_PANEL_WIDTH, SWEET_FIELD_ROWS);
		glBlitFramebuffer(SWEET_PANEL_CROP_X, s0, SWEET_PANEL_CROP_X + SWEET_PANEL_WIDTH, s0 + SWEET_PANEL_HEIGHT, x0, 0,
		    x0 + SWEET_PANEL_WIDTH, SWEET_FIELD_ROWS, GL_COLOR_BUFFER_BIT, GL_NEAREST);
	}
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)draw);
	glScissor(box[0], box[1], box[2], box[3]);
	(scissored ? glEnable : glDisable)(GL_SCISSOR_TEST);
	glFlush();
#if SWEET_FIELDS_WAIT
	waited = sweet_probe_sync();
#endif
	sweet_gpu_calls_sample(&after);
	if (logged++ % 300 == 0) {
		say("SWEET DIRECT: fields %llu us: submit %u in %llu us, wait %u in %llu us, prep %u in %llu us, "
		    "map %u in %llu us, fence %llu us\n", (unsigned long long)(now_us() - started),
		    after.submits - before.submits, (unsigned long long)(after.submit_us - before.submit_us),
		    after.waits - before.waits, (unsigned long long)(after.wait_us - before.wait_us),
		    after.cpu_preps - before.cpu_preps, (unsigned long long)(after.cpu_prep_us - before.cpu_prep_us),
		    after.maps - before.maps, (unsigned long long)(after.map_us - before.map_us), (unsigned long long)waited);
	}
}

/* A black or one-colour frame would pass any comparison: it proves nothing. */
static bool
sweet_fields_frame_varied(const struct xr10_planes *planes)
{
	uint32_t first, word;
	unsigned different = 0;

	memcpy(&first, planes->rgb + (size_t)SWEET_PANEL_CROP_Y * planes->rgb_stride, sizeof(first));
	for (int y = SWEET_PANEL_CROP_Y; y < SWEET_PANEL_CROP_Y + SWEET_PANEL_HEIGHT; y += 16) {
		for (int x = SWEET_PANEL_CROP_X; x < SWEET_PANEL_CROP_X + SWEET_PANEL_WIDTH; x += 16) {
			memcpy(&word, planes->rgb + (size_t)y * planes->rgb_stride + (size_t)x * 4, sizeof(word));
			different += word != first;
		}
	}
	return different > 500;
}

static void
sweet_stage_shadow(struct eagl *e, const struct xr10_planes *planes)
{
	static const int bias[] = { 0, 1, -1 };
	uint8_t *block = malloc(SWEET_DIRECT_BLOCK);
	struct sweet_direct_result result;
	unsigned next = SWEET_STAGE_FAILED;

	if (block != NULL && sweet_fields_ready(e, 0)) {
		for (size_t i = 0; i < sizeof(bias) / sizeof(bias[0]); i++) {
			uint64_t guards = ~0ull, started = now_us();

			sweet_fields_blit(e, 0, bias[i]);
			uint64_t drawn = now_us() - started;

			sweet_direct_prepare(block);
			if (!sweet_field_call(SWEET_FIELD_COPY_SHADOW, 0, block + SWEET_DIRECT_GUARD, SWEET_PANEL_BYTES, &guards)) {
				say("SWEET DIRECT: shadow cannot be read back\n");
				break;
			}
			sweet_direct_compare_b3a8(block, planes->rgb, planes->rgb_stride, planes->alpha, planes->alpha_stride, &result);
			say("SWEET DIRECT: shadow bias=%d frame=%u fields_us=%llu mismatches=%llu alpha=%llu kernel_guards=%llu user_guards=%u,%u "
			    "first=%d,%d got=%02x%02x%02x want=%02x%02x%02x\n", bias[i], e->frames, ULL(drawn), ULL(result.mismatches),
			    ULL(result.alpha_mismatches), ULL(guards), result.front_guard, result.back_guard, result.first_x, result.first_y,
			    result.got[0], result.got[1], result.got[2], result.want[0], result.want[1], result.want[2]);
			if (result.mismatches == 0 && guards == 0 && result.front_guard == 0 && result.back_guard == 0) {
				e->probe.field_bias = bias[i];
				next = SWEET_STAGE_PANEL_READ;
				break;
			}
		}
	}
	free(block);
	sweet_fields_release(e, 0);
	e->probe.stage = next;
}

/* The same rows through the GPU and through the CPU: is it the panel the GPU sees, laid out so? */
static void
sweet_stage_panel_read(struct eagl *e)
{
	enum { ROWS = 16 };
	size_t panel_bytes = (size_t)ROWS * 2 * SWEET_PANEL_STRIDE;
	uint8_t *panel = malloc(panel_bytes), *field = malloc((size_t)ROWS * SWEET_PANEL_WIDTH * 4);
	uint64_t wrong = 0, total = 0;
	GLint read = 0, saved[5];
	bool ok = panel != NULL && field != NULL && sweet_fields_ready(e, 1);

	glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read);
	b3a8_transfer_state(true, saved, false);
	glGetError();
	if (ok) {
		ok = sweet_field_call(SWEET_FIELD_COPY_PANEL, 0, panel, panel_bytes, NULL);
	}
	for (int f = 0; f < 2 && ok; f++) {
		glBindFramebuffer(GL_READ_FRAMEBUFFER, e->probe.field_framebuffer[2 + (unsigned)f]);
		glReadPixels(f ? SWEET_FIELD_ODD_X : 0, 0, SWEET_PANEL_WIDTH, ROWS, GL_BGRA, GL_UNSIGNED_BYTE, field);
		for (int r = 0; r < ROWS; r++) {
			const uint8_t *from_gpu = field + (size_t)r * SWEET_PANEL_WIDTH * 4;
			const uint8_t *from_cpu = panel + (size_t)(2 * r + f) * SWEET_PANEL_STRIDE;

			for (int x = 0; x < SWEET_PANEL_WIDTH; x++, from_gpu += 4, from_cpu += 4) {
				wrong += memcmp(from_gpu, from_cpu, 3) != 0;
				total++;
			}
		}
	}
	GLenum error = glGetError();

	glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)read);
	b3a8_transfer_state(true, saved, true);
	say("SWEET DIRECT: panel read frame=%u compared=%llu different=%llu err=%#x ok=%d\n", e->frames, ULL(total), ULL(wrong), error, ok);
	free(panel);
	free(field);
	e->probe.stage = ok && total != 0 && wrong == 0 && error == GL_NO_ERROR ? SWEET_STAGE_STRIPE : SWEET_STAGE_FAILED;
}

static const uint8_t sweet_stripe_colours[8][3] = {
	{ 0xff, 0x00, 0x00 }, { 0x00, 0xff, 0x00 }, { 0x00, 0x00, 0xff }, { 0xff, 0xff, 0x00 },
	{ 0xff, 0x00, 0xff }, { 0x00, 0xff, 0xff }, { 0xff, 0xff, 0xff }, { 0x20, 0x60, 0xa0 },
};

/* The first write: eight bars across the top 64 rows, left there until the next frame replaces them. */
static void
sweet_stage_stripe(struct eagl *e)
{
	size_t bytes = (size_t)SWEET_STRIPE_ROWS * SWEET_PANEL_STRIDE;
	uint8_t *panel = malloc(bytes);
	GLint draw = 0, box[4] = { 0, 0, 0, 0 };
	GLfloat colour[4] = { 0, 0, 0, 0 };
	GLboolean mask[4] = { 1, 1, 1, 1 }, scissored = glIsEnabled(GL_SCISSOR_TEST);
	uint64_t wrong = 0;
	bool ok = panel != NULL && sweet_fields_ready(e, 1);

	glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw);
	glGetIntegerv(GL_SCISSOR_BOX, box);
	glGetFloatv(GL_COLOR_CLEAR_VALUE, colour);
	glGetBooleanv(GL_COLOR_WRITEMASK, mask);
	if (ok) {
		glColorMask(1, 1, 1, 1);
		glEnable(GL_SCISSOR_TEST);
		for (int f = 0; f < 2; f++) {
			int x0 = f ? SWEET_FIELD_ODD_X : 0;

			glBindFramebuffer(GL_DRAW_FRAMEBUFFER, e->probe.field_framebuffer[2 + (unsigned)f]);
			for (int bar = 0; bar < 8; bar++) {
				const uint8_t *c = sweet_stripe_colours[bar];

				glScissor(x0 + bar * 135, 0, 135, SWEET_STRIPE_ROWS / 2);
				glClearColor(c[0] / 255.0f, c[1] / 255.0f, c[2] / 255.0f, 1.0f);
				glClear(GL_COLOR_BUFFER_BIT);
			}
		}
		glFlush();
		(void)sweet_probe_sync();
		ok = sweet_field_call(SWEET_FIELD_COPY_PANEL, 0, panel, bytes, NULL);
	}
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)draw);
	glScissor(box[0], box[1], box[2], box[3]);
	(scissored ? glEnable : glDisable)(GL_SCISSOR_TEST);
	glClearColor(colour[0], colour[1], colour[2], colour[3]);
	glColorMask(mask[0], mask[1], mask[2], mask[3]);
	for (int y = 0; y < SWEET_STRIPE_ROWS && ok; y++) {
		const uint8_t *row = panel + (size_t)y * SWEET_PANEL_STRIDE;

		for (int x = 0; x < SWEET_PANEL_WIDTH; x++, row += 4) {
			const uint8_t *c = sweet_stripe_colours[x / 135];

			wrong += row[0] != c[2] || row[1] != c[1] || row[2] != c[0];
		}
	}
	say("SWEET DIRECT: stripe frame=%u pixels=%d different=%llu ok=%d\n", e->frames, SWEET_STRIPE_ROWS * SWEET_PANEL_WIDTH, ULL(wrong), ok);
	free(panel);
	e->probe.stage = ok && wrong == 0 ? SWEET_STAGE_DONE : SWEET_STAGE_FAILED;
}

/* With the surface locked and packed: one check at a time, each on its own frame. */
static void
sweet_fields_step(struct eagl *e, const struct image *image, const struct xr10_planes *planes)
{
	unsigned stage = e->probe.stage;

	if (!SWEET_DIRECT_CHECKS || stage >= SWEET_STAGE_DONE || !sweet_probe_display(image) || image->invert || planes->alpha == NULL ||
	    e->frames < sweet_stage_frames[stage]) {
		return;
	}
	switch (stage) {
	case SWEET_STAGE_SHADOW:
		if (sweet_fields_frame_varied(planes)) {
			sweet_stage_shadow(e, planes);
		}
		break;
	case SWEET_STAGE_PANEL_READ:
		sweet_stage_panel_read(e);
		break;
	default:
		sweet_stage_stripe(e);
		break;
	}
}

/*
 * The whole frame to the panel, instead of reading it back. Only for a surface AppleCLCD has shown
 * as the one full-screen layer; the kernel is told, and skips its copy for that surface only.
 */
static bool
sweet_direct_frame(struct eagl *e, const struct image *image)
{
#if SWEET_DIRECT_PANEL_FRAMES
	uint64_t on_display = 0;
	uint32_t id = IOSurfaceGetID(image->surface);

	if ((SWEET_DIRECT_CHECKS && e->probe.stage != SWEET_STAGE_DONE) || !sweet_probe_display(image) || image->invert ||
	    !sweet_field_call(SWEET_FIELD_IS_DISPLAY, id, NULL, 0, &on_display) || on_display == 0 || !sweet_fields_ready(e, 1)) {
		return false;
	}
	sweet_fields_blit(e, 1, e->probe.field_bias);
	(void)sweet_field_call(SWEET_FIELD_PRESENTED, id, NULL, 0, NULL);
	{
		unsigned slot = e->probe.panel_surface[0] == id ? 0 : e->probe.panel_surface[1] == id ? 1 : e->probe.panel_next++ & 1;

		e->probe.panel_surface[slot] = id;
		e->probe.panel_texture[slot] = image->texture;
	}
	if (e->probe.frames_on_panel++ < 4) {
		say("SWEET DIRECT: frame %u of surface %u on the panel\n", e->frames, id);
	}
	return true;
#else
	(void)e;
	(void)image;
	return false;
#endif
}

/*
 * A display surface is attached to a new texture now and then. Normally the new texture starts
 * with what the surface holds; with frames going straight to the panel the surface still holds
 * what it held when that began, the boot logo, and it would be on the screen until everything
 * has been drawn again. So the new texture starts with the last frame: from the texture that
 * has it, or from the panel if that texture is gone. The bound texture is the new one.
 */
static bool
sweet_direct_keep_contents(struct eagl *e, struct image *image)
{
#if SWEET_DIRECT_PANEL_FRAMES
	uint32_t id = IOSurfaceGetID(image->surface);
	GLuint from = 0;

	if (image->invert || !sweet_probe_display(image)) {
		return false;
	}
	for (unsigned i = 0; i < 2; i++) {
		if (e->probe.panel_surface[i] == id) {
			from = e->probe.panel_texture[i];
		}
	}
	if (from == 0) {
		return false;
	}
	image->seed = IOSurfaceGetSeed(image->surface);
	if (from == image->texture) {
		return true;
	}
	const struct image *old = image_of(e, from);

	glGetError();
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, image->width, image->height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	if (glGetError() != GL_NO_ERROR) {
		say("SWEET DIRECT: no memory to keep the frame of surface %u\n", id);
		return false;
	}
	if (old != NULL && old != image && old->surface == image->surface) {
		GLint read = 0, draw = 0;
		GLboolean scissored = glIsEnabled(GL_SCISSOR_TEST);

		if (e->reader == 0) {
			glGenFramebuffers(1, &e->reader);
		}
		if (e->writer == 0) {
			glGenFramebuffers(1, &e->writer);
		}
		glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read);
		glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw);
		glDisable(GL_SCISSOR_TEST);
		glBindFramebuffer(GL_READ_FRAMEBUFFER, e->reader);
		glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, from, 0);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, e->writer);
		glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, image->texture, 0);
		glBlitFramebuffer(0, 0, image->width, image->height, 0, 0, image->width, image->height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
		glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)read);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)draw);
		(scissored ? glEnable : glDisable)(GL_SCISSOR_TEST);
		return true;
	}

	/* the old texture is gone: the panel still shows the last frame */
	uint8_t *panel = malloc(SWEET_PANEL_BYTES);
	GLint saved[5];
	bool ok = panel != NULL && sweet_field_call(SWEET_FIELD_COPY_PANEL, 0, panel, SWEET_PANEL_BYTES, NULL);

	if (ok) {
		for (size_t at = 0; at < SWEET_PANEL_BYTES; at += 4) {
			uint8_t blue = panel[at];

			panel[at] = panel[at + 2];
			panel[at + 2] = blue;
		}
		b3a8_transfer_state(false, saved, false);
		glTexSubImage2D(GL_TEXTURE_2D, 0, SWEET_PANEL_CROP_X, SWEET_PANEL_CROP_Y, SWEET_PANEL_WIDTH, SWEET_PANEL_HEIGHT, GL_RGBA,
		    GL_UNSIGNED_BYTE, panel);
		b3a8_transfer_state(false, saved, true);
	}
	free(panel);
	return ok;
#else
	(void)e;
	(void)image;
	return false;
#endif
}

/*
 * What a picture made of no named format holds, before it is uploaded. In premultiplied pixels
 * no colour exceeds the opacity, so the byte holding opacity has no violations.
 */
static void
sweet_probe_icon_bytes(struct eagl *e, const struct image *image, const uint8_t *base, size_t row)
{
	uint64_t sums[4] = { 0, 0, 0, 0 }, violations[4] = { 0, 0, 0, 0 };
	uint32_t corner[3] = { 0, 0, 0 };

	if (image->target || IOSurfaceGetPixelFormat(image->surface) != 0 || IOSurfaceGetBytesPerElement(image->surface) != 4 ||
	    image->width < 32 || image->height < 32 || e->probe.icon_lines >= 40) {
		return;
	}
	for (GLsizei y = 0; y < image->height; y++) {
		const uint8_t *pixel = base + (size_t)y * row;

		for (GLsizei x = 0; x < image->width; x++, pixel += 4) {
			for (unsigned k = 0; k < 4; k++) {
				sums[k] += pixel[k];
				violations[k] += pixel[(k + 1) & 3] > pixel[k] || pixel[(k + 2) & 3] > pixel[k] || pixel[(k + 3) & 3] > pixel[k];
			}
		}
	}
	/* A surface is empty when it is first made and filled later: the filled upload is the one to see. */
	if (sums[0] == 0 && sums[1] == 0 && sums[2] == 0 && sums[3] == 0) {
		return;
	}
	memcpy(&corner[0], base, 4);
	memcpy(&corner[1], base + (size_t)(image->height / 2) * row + (size_t)(image->width / 2) * 4, 4);
	memcpy(&corner[2], base + (size_t)(image->height - 1) * row + (size_t)(image->width - 1) * 4, 4);
	e->probe.icon_lines++;
	say("SWEET ICON: surface=%u %dx%d row=%zu asked=%#x/%#x sums=%llu,%llu,%llu,%llu not_opacity=%llu,%llu,%llu,%llu corner=%08x mid=%08x end=%08x\n",
	    IOSurfaceGetID(image->surface), image->width, image->height, row, image->internal, image->format, ULL(sums[0]), ULL(sums[1]), ULL(sums[2]),
	    ULL(sums[3]), ULL(violations[0]), ULL(violations[1]), ULL(violations[2]), ULL(violations[3]), corner[0], corner[1], corner[2]);
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

	if (!SWEET_PROBE_VERIFY_DAMAGE || planes->alpha == NULL || (e->frames & 7) != 0 || e->probe.verify_lines >= 256 || image->width != SWEET_LOGICAL_WIDTH ||
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
