/*
 * Host test of the panel crop and of the guarded test frame, against a small
 * model of the GL calls they make. Build with the tree's sweet_b3a8.h:
 *   cc -std=c11 -I. -I<dir with sweet_b3a8.h> sweet-readback-probe-test.c
 */
#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sweet_b3a8.h"
#include "sweet-direct-output.h"
#include "sweet-gpu-calls.h"
#include "sweet-readback-probe-state.h"

typedef unsigned GLenum, GLuint, GLbitfield;
typedef int GLint, GLsizei;
typedef long GLsizeiptr;
typedef long GLintptr;
typedef unsigned char GLboolean;
typedef float GLfloat;
typedef void *GLsync;
typedef void *IOSurfaceRef;

#define GL_NO_ERROR 0
#define GL_RGBA 0x1908
#define GL_BGRA 0x80e1
#define GL_RGBA8 0x8058
#define GL_UNSIGNED_BYTE 0x1401
#define GL_TEXTURE_2D 0x0de1
#define GL_TEXTURE_BINDING_2D 0x8069
#define GL_READ_FRAMEBUFFER 0x8ca8
#define GL_DRAW_FRAMEBUFFER 0x8ca9
#define GL_READ_FRAMEBUFFER_BINDING 0x8caa
#define GL_DRAW_FRAMEBUFFER_BINDING 0x8ca6
#define GL_COLOR_ATTACHMENT0 0x8ce0
#define GL_FRAMEBUFFER_COMPLETE 0x8cd5
#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_SCISSOR_TEST 0x0c11
#define GL_SCISSOR_BOX 0x0c10
#define GL_COLOR_CLEAR_VALUE 0x0c22
#define GL_PACK_ROW_LENGTH 0x0d02
#define GL_PACK_ALIGNMENT 0x0d05
#define GL_PIXEL_PACK_BUFFER 0x88eb
#define GL_SYNC_GPU_COMMANDS_COMPLETE 0x9117
#define GL_SYNC_FLUSH_COMMANDS_BIT 1
#define SWEET_B3A8_TEST_FORMAT 0x62336138

#ifndef SWEET_B3A8_FORMAT
#define SWEET_B3A8_FORMAT SWEET_B3A8_TEST_FORMAT
#endif

struct image {
	IOSurfaceRef surface;
	bool invert, target;
	GLenum internal, format;
	GLsizei width, height;
	int damage[4], damage_effective[4];
	bool damage_any, damage_full, damage_seen, damage_effective_full;
};
struct redwood_gl;
struct eagl {
	unsigned frames;
	uint8_t *b3a8_pixels;
	struct redwood_gl *gl;
	struct sweet_probe probe;
};
struct xr10_planes {
	uint8_t *rgb, *alpha;
	size_t rgb_stride, alpha_stride;
};

/* ---- the model ---- */
#define READER 5
#define PROBE_FB 77
static uint8_t *source;
static uint8_t target[SWEET_PANEL_WIDTH * SWEET_PANEL_HEIGHT * 4];
static GLint read_fb = READER, draw_fb = 9, scissor_box[4] = { 3, 4, 5, 6 }, pack_row, pack_align = 1;
static bool scissor_on = true;
static GLfloat clear_colour[4] = { .25f, .5f, .75f, 1 };
static GLboolean write_mask[4] = { 1, 0, 1, 0 };
static GLenum next_error, status = GL_FRAMEBUFFER_COMPLETE;
static int fake_blit_dx, fake_blit_dy, fake_swap_channels, fake_overrun;
static bool bgra_refused;
static char log_text[8192];

static IOSurfaceRef fake_surface = (IOSurfaceRef)1;
static uint32_t fake_surface_format = SWEET_B3A8_FORMAT;
static uint32_t IOSurfaceGetPixelFormat(IOSurfaceRef s) { (void)s; return fake_surface_format; }
static uint32_t IOSurfaceGetID(IOSurfaceRef s) { (void)s; return 2; }
static size_t IOSurfaceGetBytesPerElement(IOSurfaceRef s) { (void)s; return 4; }
static uint64_t now_us(void) { static uint64_t t; return t += 7; }
static void say(const char *format, ...)
{
	va_list ap;
	size_t used = strlen(log_text);

	va_start(ap, format);
	vsnprintf(log_text + used, sizeof(log_text) - used, format, ap);
	va_end(ap);
}
void sweet_gpu_calls_sample(struct sweet_gpu_calls *out) { memset(out, 0, sizeof(*out)); }

/* ---- the interlaced buffers: [0] the pool's shadow, [1] the panel, at pitch 8640 ---- */
#define FIELD_FB 100
#define FIELD_PITCH 8640
#define FIELD_BYTES 10375168u
#define FIELD_OFFSET(index) (((index) & 1) ? 4288u : 0u)
static uint8_t *field_memory[2];
static int pending_field = -1, fake_centre_shift, fake_odd_shift;
static uint64_t fake_kernel_guards;
static uint32_t fake_display = 2, fake_presented;
struct redwood_gl;

static int
redwood_gl_surface_texture(struct redwood_gl *gl, uint32_t surface)
{
	(void)gl;
	assert(surface >= 0xf5ee0000u && surface <= 0xf5ee0003u);
	pending_field = (int)(surface - 0xf5ee0000u);
	return 1;
}

int
sweet_gpu_call(uint32_t selector, void *data, size_t size)
{
	struct { uint32_t op, surface; uint64_t user, bytes, result; } *r = data;

	assert(selector == 0x7f && size == 32);
	switch (r->op) {
	case 1:
		memcpy((void *)(uintptr_t)r->user, field_memory[0], r->bytes);
		r->result = fake_kernel_guards;
		return 0;
	case 2:
		fake_presented = r->surface;
		return 0;
	case 3:
		memcpy((void *)(uintptr_t)r->user, field_memory[1], r->bytes);
		return 0;
	case 4:
		r->result = r->surface == fake_display;
		return 0;
	default:
		abort();
	}
}

static void b3a8_transfer_state(bool pack, GLint saved[5], bool restore)
{
	assert(pack);
	if (restore) {
		pack_row = saved[0];
		pack_align = saved[1];
	} else {
		saved[0] = pack_row;
		saved[1] = pack_align;
		pack_row = 0;
		pack_align = 1;
	}
}

static GLenum glGetError(void) { GLenum e = next_error; next_error = 0; return e; }
static void glFinish(void) {}
static void glFlush(void) {}
static GLsync glFenceSync(GLenum c, GLbitfield f) { (void)c; (void)f; return (GLsync)1; }
static GLenum glClientWaitSync(GLsync s, GLbitfield f, uint64_t t) { (void)s; (void)f; (void)t; return 0; }
static void glDeleteSync(GLsync s) { (void)s; }
static void glGenFramebuffers(GLsizei n, GLuint *f)
{
	(void)n;
	*f = pending_field >= 0 ? (GLuint)(FIELD_FB + pending_field) : PROBE_FB;
	pending_field = -1;
}
static void glDeleteFramebuffers(GLsizei n, const GLuint *f) { (void)n; (void)f; }
static void glGenTextures(GLsizei n, GLuint *t) { (void)n; *t = 88; }
static void glDeleteTextures(GLsizei n, const GLuint *t) { (void)n; (void)t; }
static void glBindTexture(GLenum t, GLuint b) { (void)t; (void)b; }
static void glTexParameteri(GLenum t, GLenum p, GLint v) { (void)t; (void)p; (void)v; }
static void glTexImage2D(GLenum t, GLint l, GLint i, GLsizei w, GLsizei h, GLint b, GLenum f, GLenum y, const void *d)
{ (void)t; (void)l; (void)i; (void)w; (void)h; (void)b; (void)f; (void)y; (void)d; }
static void glFramebufferTexture2D(GLenum t, GLenum a, GLenum x, GLuint n, GLint l) { (void)t; (void)a; (void)x; (void)n; (void)l; }
static GLenum glCheckFramebufferStatus(GLenum t) { (void)t; return status; }
static void glGenBuffers(GLsizei n, GLuint *b) { (void)n; *b = 3; }
static void glDeleteBuffers(GLsizei n, const GLuint *b) { (void)n; (void)b; }
static void glBindBuffer(GLenum t, GLuint b) { (void)t; (void)b; }
static void glBufferData(GLenum t, GLsizeiptr s, const void *d, GLenum u) { (void)t; (void)s; (void)d; (void)u; }
static void *glMapBufferRange(GLenum t, GLintptr o, GLsizeiptr l, GLbitfield a) { (void)t; (void)o; (void)l; (void)a; return NULL; }
static GLboolean glUnmapBuffer(GLenum t) { (void)t; return 1; }
static void glBindFramebuffer(GLenum t, GLuint f)
{
	if (t == GL_READ_FRAMEBUFFER) { read_fb = (GLint)f; } else { draw_fb = (GLint)f; }
}
static GLboolean glIsEnabled(GLenum c) { assert(c == GL_SCISSOR_TEST); return scissor_on; }
static void glEnable(GLenum c) { assert(c == GL_SCISSOR_TEST); scissor_on = true; }
static void glDisable(GLenum c) { assert(c == GL_SCISSOR_TEST); scissor_on = false; }
static void glScissor(GLint x, GLint y, GLsizei w, GLsizei h) { scissor_box[0] = x; scissor_box[1] = y; scissor_box[2] = w; scissor_box[3] = h; }
static void glClearColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a) { clear_colour[0] = r; clear_colour[1] = g; clear_colour[2] = b; clear_colour[3] = a; }
static void glColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a) { write_mask[0] = r; write_mask[1] = g; write_mask[2] = b; write_mask[3] = a; }
static void glPixelStorei(GLenum p, GLint v) { if (p == GL_PACK_ROW_LENGTH) { pack_row = v; } else { pack_align = v; } }
static void glGetIntegerv(GLenum p, GLint *v)
{
	switch (p) {
	case GL_READ_FRAMEBUFFER_BINDING: *v = read_fb; break;
	case GL_DRAW_FRAMEBUFFER_BINDING: *v = draw_fb; break;
	case GL_SCISSOR_BOX: memcpy(v, scissor_box, sizeof(scissor_box)); break;
	case GL_TEXTURE_BINDING_2D: *v = 1; break;
	default: abort();
	}
}
static void glGetFloatv(GLenum p, GLfloat *v) { (void)p; memcpy(v, clear_colour, sizeof(clear_colour)); }
static void glGetBooleanv(GLenum p, GLboolean *v) { (void)p; memcpy(v, write_mask, sizeof(write_mask)); }

static void glClear(GLbitfield mask)
{
	assert(mask == GL_COLOR_BUFFER_BIT && scissor_on);
	if (draw_fb >= FIELD_FB) {
		int index = draw_fb - FIELD_FB;

		for (int y = scissor_box[1]; y < scissor_box[1] + scissor_box[3]; y++) {
			for (int x = scissor_box[0]; x < scissor_box[0] + scissor_box[2]; x++) {
				uint8_t *p = field_memory[index / 2] + FIELD_OFFSET(index) + (size_t)y * FIELD_PITCH + (size_t)x * 4;

				p[0] = (uint8_t)(clear_colour[2] * 255.0f + 0.5f);
				p[1] = (uint8_t)(clear_colour[1] * 255.0f + 0.5f);
				p[2] = (uint8_t)(clear_colour[0] * 255.0f + 0.5f);
				p[3] = (uint8_t)(clear_colour[3] * 255.0f + 0.5f);
			}
		}
		return;
	}
	assert(draw_fb == PROBE_FB);
	for (int y = scissor_box[1]; y < scissor_box[1] + scissor_box[3]; y++) {
		for (int x = scissor_box[0]; x < scissor_box[0] + scissor_box[2]; x++) {
			uint8_t *p = target + ((size_t)y * SWEET_PANEL_WIDTH + (size_t)x) * 4;

			for (int c = 0; c < 4; c++) {
				p[c] = (uint8_t)(clear_colour[c] * 255.0f + 0.5f);
			}
		}
	}
}

static void glBlitFramebuffer(GLint sx0, GLint sy0, GLint sx1, GLint sy1, GLint dx0, GLint dy0, GLint dx1, GLint dy1, GLbitfield m, GLenum f)
{
	(void)m;
	(void)f;
	if (draw_fb >= FIELD_FB) {
		int index = draw_fb - FIELD_FB;

		assert(scissor_on && read_fb == READER && dy0 == 0 && dy1 == 1200 && sy1 - sy0 == 2400 && dx1 - dx0 == SWEET_PANEL_WIDTH);
		assert(scissor_box[0] == dx0 && scissor_box[2] == SWEET_PANEL_WIDTH && scissor_box[1] == 0 && scissor_box[3] == 1200);
		for (int d = 0; d < 1200; d++) {
			/* the centre of each destination row, twice as far along in the source */
			int row = sy0 + 2 * d + 1 + fake_centre_shift;

			for (int x = 0; x < SWEET_PANEL_WIDTH; x++) {
				const uint8_t *in = source + ((size_t)row * SWEET_LOGICAL_WIDTH + (size_t)(sx0 + x)) * 4;
				uint8_t *out = field_memory[index / 2] + FIELD_OFFSET(index) + (size_t)d * FIELD_PITCH +
				    (size_t)(dx0 + x + ((index & 1) ? fake_odd_shift : 0)) * 4;

				out[0] = in[2];
				out[1] = in[1];
				out[2] = in[0];
				out[3] = in[3];
			}
		}
		return;
	}
	assert(!scissor_on && read_fb == READER && draw_fb == PROBE_FB);
	assert(sx1 - sx0 == SWEET_PANEL_WIDTH && sy1 - sy0 == SWEET_PANEL_HEIGHT && dx0 == 0 && dx1 == SWEET_PANEL_WIDTH);
	bool flip = dy0 > dy1;

	for (int y = 0; y < SWEET_PANEL_HEIGHT; y++) {
		int from_y = sy0 + y + fake_blit_dy;
		int to_y = flip ? SWEET_PANEL_HEIGHT - 1 - y : y;

		memcpy(target + (size_t)to_y * SWEET_PANEL_WIDTH * 4,
		    source + ((size_t)from_y * SWEET_LOGICAL_WIDTH + (size_t)(sx0 + fake_blit_dx)) * 4, SWEET_PANEL_WIDTH * 4);
	}
}

static void glReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum f, GLenum t, void *pixels)
{
	assert(t == GL_UNSIGNED_BYTE);
	if (pixels == NULL) {
		return;
	}
	if (f == GL_BGRA && bgra_refused) {
		next_error = 0x502;
		return;
	}
	if (read_fb >= FIELD_FB) {
		int index = read_fb - FIELD_FB;

		assert(f == GL_BGRA);
		for (int r = 0; r < h; r++) {
			memcpy((uint8_t *)pixels + (size_t)r * (size_t)w * 4,
			    field_memory[index / 2] + FIELD_OFFSET(index) + (size_t)(y + r) * FIELD_PITCH + (size_t)x * 4, (size_t)w * 4);
		}
		return;
	}
	const uint8_t *from = read_fb == PROBE_FB ? target : source;
	int source_width = read_fb == PROBE_FB ? SWEET_PANEL_WIDTH : SWEET_LOGICAL_WIDTH;
	size_t stride = (size_t)(pack_row ? pack_row : w) * 4;

	for (int r = 0; r < h + fake_overrun; r++) {
		uint8_t *out = (uint8_t *)pixels + (size_t)r * stride;
		int row = r < h ? r : h - 1;

		for (int c = 0; c < w; c++, out += 4) {
			const uint8_t *in = from + ((size_t)(y + row) * (size_t)source_width + (size_t)(x + c)) * 4;
			bool swap = (f == GL_BGRA) != (fake_swap_channels != 0);

			out[0] = in[swap ? 2 : 0];
			out[1] = in[1];
			out[2] = in[swap ? 0 : 2];
			out[3] = in[3];
		}
	}
}

#define SWEET_PROBE_ROUNDS 1
#define SWEET_PROBE_VERIFY_DAMAGE 1
#define SWEET_DIRECT_STAGE_ZERO 1
#define SWEET_DIRECT_PANEL_FRAMES 1
#include "sweet-readback-probe.inc"

/* ---- the frame ---- */
static struct xr10_planes planes;

static void
frame_colour(int x, int y, uint8_t rgba[4])
{
	rgba[0] = (uint8_t)(x * 7 + y * 3);
	rgba[1] = (uint8_t)(x * 11 + y * 5 + 1);
	rgba[2] = (uint8_t)(x * 13 + y * 17 + 2);
	rgba[3] = (uint8_t)(x + y + 3);
}

/* What the renderer does: the target's rows packed into the planes. */
static void
frame_setup(bool invert)
{
	for (int y = 0; y < SWEET_LOGICAL_HEIGHT; y++) {
		size_t row = (size_t)(invert ? SWEET_LOGICAL_HEIGHT - 1 - y : y);

		for (int x = 0; x < SWEET_LOGICAL_WIDTH; x++) {
			uint8_t *pixel = source + ((size_t)y * SWEET_LOGICAL_WIDTH + (size_t)x) * 4;
			uint32_t word;

			frame_colour(x, y, pixel);
			word = sweet_b3a8_pack(pixel);
			memcpy(planes.rgb + row * planes.rgb_stride + (size_t)x * 4, &word, sizeof(word));
			planes.alpha[row * planes.alpha_stride + (size_t)x] = pixel[3];
		}
	}
}

static const char *
find_line(const char *kind)
{
	const char *line = strstr(log_text, kind);

	assert(line != NULL);
	return line;
}

static void
run_direct(bool invert, unsigned *pattern_bad, unsigned *frame_bad, unsigned *pattern_guard)
{
	struct eagl e;
	struct image image;
	unsigned long long pattern, frame;
	unsigned front, back;
	int inv;
	const char *line;

	memset(&e, 0, sizeof(e));
	memset(&image, 0, sizeof(image));
	frame_setup(invert);
	log_text[0] = 0;
	e.probe.direct_state = SWEET_DIRECT_BLIT_DONE;
	e.frames = 2;
	image.surface = fake_surface;
	image.invert = invert;
	image.width = SWEET_LOGICAL_WIDTH;
	image.height = SWEET_LOGICAL_HEIGHT;
	read_fb = READER;
	draw_fb = 9;
	scissor_on = true;
	glScissor(3, 4, 5, 6);
	sweet_probe_direct_step(&e, &image, &planes);

	line = find_line("SWEET DIRECT: pattern");
	assert(sscanf(strstr(line, "invert="), "invert=%d mismatches=%llu", &inv, &pattern) == 2 && inv == (int)invert);
	assert(sscanf(strstr(line, "guards="), "guards=%u,%u", &front, &back) == 2);
	*pattern_bad = (unsigned)pattern;
	*pattern_guard = front + back;
	line = find_line("SWEET DIRECT: frame");
	assert(sscanf(strstr(line, "mismatches="), "mismatches=%llu", &frame) == 1);
	*frame_bad = (unsigned)frame;

	/* everything the step changed has been put back */
	assert(read_fb == READER && draw_fb == 9 && scissor_on);
	assert(scissor_box[0] == 3 && scissor_box[1] == 4 && scissor_box[2] == 5 && scissor_box[3] == 6);
	assert(clear_colour[0] == .25f && write_mask[1] == 0 && pack_row == 0 && pack_align == 1);
	assert(e.probe.direct_state == SWEET_DIRECT_DONE && e.probe.framebuffer == 0 && e.probe.texture == 0);
}

static void
field_image(struct eagl *e, struct image *image)
{
	memset(e, 0, sizeof(*e));
	memset(image, 0, sizeof(*image));
	image->surface = fake_surface;
	image->width = SWEET_LOGICAL_WIDTH;
	image->height = SWEET_LOGICAL_HEIGHT;
	read_fb = READER;
	draw_fb = 9;
	scissor_on = true;
	glScissor(3, 4, 5, 6);
	log_text[0] = 0;
	memset(field_memory[0], 0, FIELD_BYTES);
	memset(field_memory[1], 0x33, FIELD_BYTES);
}

static void
assert_state_back(void)
{
	assert(read_fb == READER && draw_fb == 9 && scissor_on && scissor_box[0] == 3 && scissor_box[3] == 6);
	assert(clear_colour[0] == .25f && write_mask[1] == 0 && pack_row == 0 && pack_align == 1);
}

/* What the panel would show for this frame, against the bytes of a buffer of panel layout. */
static uint64_t
panel_differences(const uint8_t *memory)
{
	uint64_t wrong = 0;

	for (int y = 0; y < SWEET_PANEL_HEIGHT; y++) {
		for (int x = 0; x < SWEET_PANEL_WIDTH; x++) {
			const uint8_t *in = source + ((size_t)(y + SWEET_PANEL_CROP_Y) * SWEET_LOGICAL_WIDTH + (size_t)(x + SWEET_PANEL_CROP_X)) * 4;
			const uint8_t *out = memory + (size_t)y * SWEET_PANEL_STRIDE + (size_t)x * 4;

			wrong += out[0] != in[2] || out[1] != in[1] || out[2] != in[0];
		}
	}
	return wrong;
}

/* The checks in order, each on its frame, and what they refuse. */
static void
run_fields(void)
{
	struct eagl e;
	struct image image;

	field_memory[0] = malloc(FIELD_BYTES);
	field_memory[1] = malloc(FIELD_BYTES);
	assert(field_memory[0] != NULL && field_memory[1] != NULL);
	frame_setup(false);

	field_image(&e, &image);
	e.frames = 23;
	sweet_fields_step(&e, &image, &planes);
	assert(e.probe.stage == SWEET_STAGE_SHADOW);
	e.frames = 24;
	sweet_fields_step(&e, &image, &planes);
	assert(e.probe.stage == SWEET_STAGE_PANEL_READ && e.probe.field_bias == 0);
	assert(strstr(log_text, "bias=0") != NULL && strstr(log_text, "mismatches=0") != NULL);
	assert(panel_differences(field_memory[0]) == 0);
	assert(e.probe.field_framebuffer[0] == 0);          /* the test buffer is given back */
	assert_state_back();

	e.frames = 32;
	sweet_fields_step(&e, &image, &planes);
	assert(e.probe.stage == SWEET_STAGE_STRIPE && strstr(log_text, "different=0") != NULL);
	assert_state_back();

	e.frames = 44;
	sweet_fields_step(&e, &image, &planes);
	assert(e.probe.stage == SWEET_STAGE_DONE);
	assert_state_back();
	for (int y = 0; y < SWEET_STRIPE_ROWS; y++) {
		for (int x = 0; x < SWEET_PANEL_WIDTH; x++) {
			const uint8_t *out = field_memory[1] + (size_t)y * SWEET_PANEL_STRIDE + (size_t)x * 4;
			const uint8_t *c = sweet_stripe_colours[x / 135];

			assert(out[0] == c[2] && out[1] == c[1] && out[2] == c[0]);
		}
	}
	/* below the stripe, and past the panel's end, nothing was written */
	assert(field_memory[1][(size_t)SWEET_STRIPE_ROWS * SWEET_PANEL_STRIDE] == 0x33);
	assert(field_memory[1][SWEET_PANEL_BYTES] == 0x33);

	/* every frame to the panel, once the checks have passed, and the kernel is told */
	fake_presented = 0;
	assert(sweet_direct_frame(&e, &image));
	assert(fake_presented == 2 && panel_differences(field_memory[1]) == 0);
	assert(field_memory[1][SWEET_PANEL_BYTES] == 0x33);
	assert_state_back();

	/* a surface AppleCLCD has not shown is left to the b3a8 path */
	fake_display = 3;
	fake_presented = 0;
	assert(!sweet_direct_frame(&e, &image) && fake_presented == 0);
	fake_display = 2;

	/* this GPU may sample the rows one lower: the check finds out */
	field_image(&e, &image);
	e.frames = 24;
	fake_centre_shift = 1;
	sweet_fields_step(&e, &image, &planes);
	assert(e.probe.stage == SWEET_STAGE_PANEL_READ && e.probe.field_bias == -1);
	fake_centre_shift = 0;

	/* an odd field a pixel out cannot pass whatever the bias */
	field_image(&e, &image);
	e.frames = 24;
	fake_odd_shift = 1;
	sweet_fields_step(&e, &image, &planes);
	assert(e.probe.stage == SWEET_STAGE_FAILED);
	fake_odd_shift = 0;
	assert(!sweet_direct_frame(&e, &image));

	/* a kernel guard page touched fails the shadow even if the pixels agree */
	field_image(&e, &image);
	e.frames = 24;
	fake_kernel_guards = 1;
	sweet_fields_step(&e, &image, &planes);
	assert(e.probe.stage == SWEET_STAGE_FAILED);
	fake_kernel_guards = 0;

	/* a flat frame proves nothing: it waits for one that does */
	field_image(&e, &image);
	e.frames = 24;
	memset(planes.rgb, 0, planes.rgb_stride * SWEET_LOGICAL_HEIGHT);
	sweet_fields_step(&e, &image, &planes);
	assert(e.probe.stage == SWEET_STAGE_SHADOW && strstr(log_text, "shadow bias") == NULL);
	frame_setup(false);
	free(field_memory[0]);
	free(field_memory[1]);
}

static void
run_icons(void)
{
	static uint8_t pixels[64 * 64 * 4];
	struct eagl e;
	struct image image;
	unsigned long long v[4];

	memset(&e, 0, sizeof(e));
	memset(&image, 0, sizeof(image));
	image.surface = fake_surface;
	image.width = image.height = 64;
	for (size_t i = 0; i < sizeof(pixels); i += 4) {
		pixels[i] = 0xff;
		pixels[i + 1] = 0x10;
		pixels[i + 2] = 0x20;
		pixels[i + 3] = 0x30;
	}
	log_text[0] = 0;
	fake_surface_format = 0;
	sweet_probe_icon_bytes(&e, &image, pixels, 64 * 4);
	fake_surface_format = SWEET_B3A8_FORMAT;
	assert(sscanf(strstr(log_text, "not_opacity="), "not_opacity=%llu,%llu,%llu,%llu", &v[0], &v[1], &v[2], &v[3]) == 4);
	assert(v[0] == 0 && v[1] != 0 && v[2] != 0 && v[3] != 0);
	image.target = true;
}

/* The target is taken before the first frame's read and the check follows two frames on. */
static void
run_order(void)
{
	struct eagl e;
	struct image image;

	memset(&e, 0, sizeof(e));
	memset(&image, 0, sizeof(image));
	frame_setup(false);
	image.surface = fake_surface;
	image.width = SWEET_LOGICAL_WIDTH;
	image.height = SWEET_LOGICAL_HEIGHT;
	read_fb = READER;
	draw_fb = 9;
	scissor_on = true;
	log_text[0] = 0;

	sweet_probe_area_step(&e, &image);
	assert(e.probe.direct_state == SWEET_DIRECT_TARGET_MADE && e.probe.framebuffer == PROBE_FB && e.probe.step == 0);
	sweet_probe_direct_step(&e, &image, &planes);
	assert(e.probe.direct_state == SWEET_DIRECT_TARGET_MADE);
	e.frames = 1;
	sweet_probe_area_step(&e, &image);
	assert(e.probe.direct_state == SWEET_DIRECT_BLIT_DONE && strstr(log_text, "crop-blit") != NULL);
	sweet_probe_direct_step(&e, &image, &planes);
	assert(e.probe.direct_state == SWEET_DIRECT_BLIT_DONE);
	e.frames = 2;
	sweet_probe_direct_step(&e, &image, &planes);
	assert(e.probe.direct_state == SWEET_DIRECT_DONE && e.probe.framebuffer == 0 && strstr(log_text, "SWEET DIRECT: frame") != NULL);

	/* then the measured round, and the buffer it used is given back */
	e.b3a8_pixels = source;
	e.frames = 96;
	sweet_probe_area_step(&e, &image);
	assert(e.probe.step == 1 && e.probe.round == 0);
	e.probe.step = SWEET_PROBE_STEPS - 1;
	e.probe.pbo = 3;
	sweet_probe_area_step(&e, &image);
	assert(e.probe.step == 0 && e.probe.round == 1 && e.probe.pbo == 0);
	assert(read_fb == READER && draw_fb == 9 && scissor_on);

	/* no memory for the target: the check is given up once, not retried every frame */
	memset(&e, 0, sizeof(e));
	status = 0x8cdd;
	sweet_probe_area_step(&e, &image);
	assert(e.probe.direct_state == SWEET_DIRECT_DONE && e.probe.framebuffer == 0);
	status = GL_FRAMEBUFFER_COMPLETE;
}

int
main(void)
{
	unsigned pattern, frame, guards;
	struct sweet_direct_rect crop;

	source = malloc((size_t)SWEET_LOGICAL_WIDTH * SWEET_LOGICAL_HEIGHT * 4);
	planes.rgb_stride = 4544;
	planes.alpha_stride = 1152;
	planes.rgb = malloc(planes.rgb_stride * SWEET_LOGICAL_HEIGHT);
	planes.alpha = malloc(planes.alpha_stride * SWEET_LOGICAL_HEIGHT);
	assert(source != NULL && planes.rgb != NULL && planes.alpha != NULL);

	crop = sweet_direct_crop(false);
	assert(crop.src_x0 == 22 && crop.src_x1 == 1102 && crop.src_y0 == 18 && crop.src_y1 == 2418 && crop.dst_y0 == 0 && crop.dst_y1 == 2400);
	crop = sweet_direct_crop(true);
	assert(crop.src_y0 == 18 && crop.src_y1 == 2418 && crop.dst_y0 == 2400 && crop.dst_y1 == 0);
	assert(SWEET_PANEL_BYTES == 10368000u);

	for (int invert = 0; invert < 2; invert++) {
		run_direct(invert != 0, &pattern, &frame, &guards);
		assert(pattern == 0 && frame == 0 && guards == 0);
	}

	run_order();
	run_fields();
	run_icons();

	/* each fault the check exists for must show */
	fake_blit_dx = 1;
	run_direct(false, &pattern, &frame, &guards);
	assert(pattern == 0 && frame != 0);
	fake_blit_dx = 0;
	fake_blit_dy = 1;
	run_direct(false, &pattern, &frame, &guards);
	assert(pattern == 0 && frame != 0);
	fake_blit_dy = 0;
	fake_swap_channels = 1;
	run_direct(false, &pattern, &frame, &guards);
	assert(pattern != 0 && frame != 0);
	fake_swap_channels = 0;
	fake_overrun = 1;
	run_direct(false, &pattern, &frame, &guards);
	assert(guards != 0);
	fake_overrun = 0;

	/* GL_BGRA refused: read as RGBA and reordered, and said so */
	bgra_refused = true;
	run_direct(false, &pattern, &frame, &guards);
	assert(pattern == 0 && frame == 0 && strstr(log_text, "rgba_fallback=1") != NULL);
	bgra_refused = false;

	/* damage */
	{
		int box[4] = { 0, 0, 0, 0 };
		bool any = false;
		const int a[4] = { 10, 20, 30, 40 }, b[4] = { 5, 50, 10, 100 }, empty[4] = { 1, 1, 0, 9 };
		int clamped[4] = { -5, 2430, 2000, 100 };

		sweet_damage_union(box, &any, empty);
		assert(!any);
		sweet_damage_union(box, &any, a);
		sweet_damage_union(box, &any, b);
		assert(any && box[0] == 5 && box[1] == 20 && box[2] == 35 && box[3] == 130);
		sweet_damage_clamp(clamped, 1125, 2436);
		assert(clamped[0] == 0 && clamped[1] == 2430 && clamped[2] == 1125 && clamped[3] == 6);
		assert(sweet_damage_bin(1, 100) == 0 && sweet_damage_bin(2, 100) == 1 && sweet_damage_bin(11, 100) == 2);
		assert(sweet_damage_bin(51, 100) == 3 && sweet_damage_bin(100, 100) == 4);
	}

	/* the true change against a declared box, both ways up */
	for (int invert = 0; invert < 2; invert++) {
		struct sweet_changed changed;
		const int declared[4] = { 100, 200, 10, 10 };
		uint8_t *pixels = malloc((size_t)SWEET_LOGICAL_WIDTH * SWEET_LOGICAL_HEIGHT * 4);

		frame_setup(invert != 0);
		memcpy(pixels, source, (size_t)SWEET_LOGICAL_WIDTH * SWEET_LOGICAL_HEIGHT * 4);
		sweet_direct_changed(pixels, SWEET_LOGICAL_WIDTH, SWEET_LOGICAL_HEIGHT, invert != 0, planes.rgb, planes.rgb_stride, planes.alpha,
		    planes.alpha_stride, declared, &changed);
		assert(!changed.any && changed.changed == 0);
		pixels[((size_t)205 * SWEET_LOGICAL_WIDTH + 105) * 4] ^= 0x40;
		pixels[((size_t)900 * SWEET_LOGICAL_WIDTH + 700) * 4 + 3] ^= 0x40;
		sweet_direct_changed(pixels, SWEET_LOGICAL_WIDTH, SWEET_LOGICAL_HEIGHT, invert != 0, planes.rgb, planes.rgb_stride, planes.alpha,
		    planes.alpha_stride, declared, &changed);
		assert(changed.changed == 2 && changed.outside == 1);
		assert(changed.box[0] == 105 && changed.box[1] == 205 && changed.box[2] == 596 && changed.box[3] == 696);
		free(pixels);
	}

	/* the damage close: full on first sight, then the box and the other buffer's */
	{
		struct eagl e;
		struct image a, b;

		memset(&e, 0, sizeof(e));
		memset(&a, 0, sizeof(a));
		a.surface = fake_surface;
		a.width = SWEET_LOGICAL_WIDTH;
		a.height = SWEET_LOGICAL_HEIGHT;
		b = a;
		a.damage_any = true;
		a.damage[0] = 10; a.damage[1] = 20; a.damage[2] = 30; a.damage[3] = 40;
		log_text[0] = 0;
		sweet_probe_damage_close(&e, &a);
		assert(a.damage_effective_full && a.damage_seen && !a.damage_any);
		assert(e.probe.surfaces[0].now_bins[0] == 1 && e.probe.surfaces[0].eff_bins[4] == 1);

		/* the same surface ID again: no other buffer to take in */
		a.damage_any = true;
		a.damage[0] = 50; a.damage[1] = 60; a.damage[2] = 5; a.damage[3] = 5;
		sweet_probe_damage_close(&e, &a);
		assert(!a.damage_effective_full && a.damage_effective[0] == 50 && e.probe.surfaces[0].eff_bins[0] == 1);

		/* another buffer: its box comes in */
		b.damage_seen = true;
		b.damage_any = true;
		b.damage[0] = 100; b.damage[1] = 100; b.damage[2] = 10; b.damage[3] = 10;
		e.probe.previous_id = 3;
		e.probe.previous_any = true;
		e.probe.previous_full = false;
		e.probe.previous_box[0] = 200; e.probe.previous_box[1] = 200; e.probe.previous_box[2] = 10; e.probe.previous_box[3] = 10;
		sweet_probe_damage_close(&e, &b);
		assert(!b.damage_effective_full && b.damage_effective[0] == 100 && b.damage_effective[2] == 110 && b.damage_effective[3] == 110);

		/* the other buffer drew everything: so must this copy */
		b.damage_any = true;
		b.damage[0] = 1; b.damage[1] = 1; b.damage[2] = 1; b.damage[3] = 1;
		e.probe.previous_id = 3;
		e.probe.previous_any = false;
		e.probe.previous_full = true;
		sweet_probe_damage_close(&e, &b);
		assert(b.damage_effective_full);

		/* a drawn target with no box at all */
		b.damage_full = true;
		sweet_probe_damage_close(&e, &b);
		assert(b.damage_effective_full);
	}
	puts("sweet readback probe: all checks passed");
	free(source);
	free(planes.rgb);
	free(planes.alpha);
	return 0;
}
