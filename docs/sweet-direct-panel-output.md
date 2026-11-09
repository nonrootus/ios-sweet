# Direct panel output on sweet_k6a

Goal: put a CoreAnimation frame on the panel with the GPU, without the
`glReadPixels` -> b3a8 pack -> AppleCLCD unpack round trip. The b3a8 path stays
the working path until every gate below has passed, and stays the fallback.

Status 2026-11-09: stage 0 is written and host-tested. Its first phone run
(0181d) could not allocate the 12 MB target from the fragmented GPU pool, so the
target is now made on the first display frame (0181e). Nothing below has written
to the panel.

Why this is worth doing, measured on 0181d: a full-frame readback is 278 ms of
CPU copy from write-combining memory against 3-8 ms of GPU work, and the area a
moving UI changes has a bounding box of 81% of the screen at the median, so
partial readback is not a substitute.

## What is known

Observed on this phone or read from the tree:

| Fact | Source |
|---|---|
| Panel 1080x2400, stride 4320, B,G,R,A bytes, at PA 0x9c000000 | `SWEET FB: AppleCLCD ...` boot line; ABL continuous splash |
| Panel is DSC video mode, scanned continuously, one buffer | NOTES.md framebuffer entry |
| Presented size 1125x2436; panel shows a centred crop, no scaling, origin (22,18) | patch 0061, `fOffX`/`fOffY`; swap record `a 0,0 1125x2436 b 0,0 1125x2436` |
| Crop rows are symmetric: 18 above and 18 below; columns 22 left, 23 right | (2436-2400)/2, (1125-1080)/2 |
| Display targets are b3a8, RGB stride 4544, alpha stride 1152, drawn upright | 0181c log `-attachImage` lines, no `drawn inverted` |
| CoreAnimation alternates surfaces 2 and 3 | 0181c log |
| GPU memory is one pinned Normal-NC pool, 128 MiB at IOVA 0x48000000; no live SMMU edits | `sweet_a618_render_pool.h` |
| `submit` returns when the GPU is idle; a submit over 100 ms fails and poisons the renderer | `a618_render_ring`, `sweet_a618_render_submit` |
| Mesa exposes OpenGL ES 3.2 on FD618 | 0181c log |

## The stride problem

The panel's stride cannot be programmed into the GPU's colour or 2D paths:

- `RB_MRT[].PITCH` has `shr="6"`, a 64-byte unit (`a6xx.xml`).
- `RB_A2D_DEST_BUFFER_PITCH` has `shr="6"`; `RB_A2D_DEST_BUFFER_BASE` has `align="64"`.
- 4320 = 67.5 x 64. Every surface the GPU draws today is a multiple of 64
  (4544, 1152, 4608), which is why this has not shown up before.

A linear target at stride 4320 is therefore not expressible. Rounding to 4288
would shear the picture by 8 pixels a row. Three ways round it:

1. **Two interlaced fields (preferred first attempt).** 2 x 4320 = 8640 = 135 x 64
   is a legal pitch. Even rows: base = panel + 0, pitch 8640, 1200 rows. Odd
   rows: base = panel + 4288 (64-aligned), pitch 8640, drawn from x = 8 so
   that the first pixel lands at panel + 4320. Each field is a fragment-shader
   pass with `texelFetch` so rows are chosen exactly; a scaled `glBlitFramebuffer`
   would sample the wrong row. Plain ES 3.0 is enough.
2. **Compute with an SSBO.** Byte-addressed, so no pitch limit. Needs
   `glDispatchCompute` from libOSMesa and the panel bound as a buffer. Simplest
   shader, more untested driver paths.
3. **Move the scanout.** Point the display plane at a pool buffer with a
   64-aligned stride (4352) and flip at vblank. This also removes tearing, but it
   means programming MDSS, which nothing in the tree touches yet. Later work.

## Stages and gates

Each stage has to pass before the next is attempted.

### Stage 0: geometry and format, renderer-only (test 0181d)

A GPU-drawn test frame and the real frame's crop, into a 1080x2400 texture, read
back at stride 4320 into a CPU buffer with 16 KiB guards on each side.

Passes when the USB log shows, for `SWEET DIRECT: pattern` and `SWEET DIRECT: frame`:
`mismatches=0`, `guards=0,0`, `err=0`. The frame line compares against the b3a8
planes through `sweet_b3a8_unpack` at (22+x, 18+y), the same arithmetic AppleCLCD
uses, so it covers crop, orientation and channel order.

What it does not cover: the target is an ordinary tiled texture, so it says
nothing about the pitch problem or about linear memory.

### Stage 1: real linear memory, in the pool (needs XNU)

A panel-shaped buffer inside the pool (10,368,000 bytes, 633 16 KiB pages) with
guard pages on both sides, handed to Mesa through selector 0x7e. On sweet this
selector resolves reserved IDs to pool ranges; it is not Redwood's import and
makes no SMMU change. Render the test frame by interlaced fields.

Passes when the kernel, reading the buffer directly: finds the pattern at
every edge and in sampled rows, finds the guard pages untouched, and the field
seam shows no shear. CPU reads are from NC memory, so sample rather than scan.

### Stage 2: can the GPU reach the panel memory (read only)

Add the panel's 633 pages to the tree at build time, before the GPU context is
installed, as a separate IOVA range (five new L3 tables), read-only to the GPU.
Sample a few rows through the GPU into a pool buffer and compare with the CPU's
view. This is the first test touching a new physical range, and a fault here
costs nothing visible.

### Stage 3: first write to the panel

Make the mapping writable. Boot argument `sweetdirect=1`, off by default.
Write a bounded stripe first: 1080x64 at the top of the panel, once, after
SpringBoard is up, with b3a8 still presenting everything else. The owner confirms
on screen. Then the full frame.

### Stage 4: frame sync, then retire the CPU copy

See below. Only after the visual check does the b3a8 copy get throttled, and
only then is the readback cost actually gone from the frame.

## Frame sync

Order today: draw, `frame_end` copies to the IOSurface, CoreAnimation swaps,
AppleCLCD unpacks to the panel.

Proposed: `frame_end` draws the two fields straight to the panel. `submit` is
synchronous, so the frame is on the panel when it returns, and the swap that
follows has nothing left to copy. The kernel needs to know that:

1. EAGL, after the fields, calls a new selector with (surface ID, sequence).
2. `AppleCLCD::swap` skips the CPU unpack only when all hold: one enabled layer,
   layer 0, source 0,0 1125x2436, destination 0,0, format b3a8, the surface and
   sequence match the last report. Anything else takes the existing b3a8 path.
3. The sequence must be newer than the last swap's, so a dropped or repeated
   GPU frame can never freeze the panel: the CPU path takes over.

Tearing: the panel is single-buffered and scanned continuously. The CPU copy
tears today and takes longer than a scan, so the GPU write is not a regression.
Aligning the write to the scan needs the MDSS interface line counter, unknown
as yet. Option 3 above is the real cure.

Other users of the panel memory: the kernel console writes there until
`fConsolePaused`; keep that behaviour.

Limits to respect: a submit must stay under 100 ms. A fence on the panel write is
not needed while `submit` is synchronous; it will be if submit stops being so.

## What is not known

- Whether the panel carveout can be mapped for the GPU without a hypervisor or
  XPU fault (stage 2 answers this safely).
- Whether Mesa's `resource_from_handle` accepts pitch 8640 with a 4288 offset.
- Whether anything reads the b3a8 surfaces apart from the display (screenshots,
  snapshots). Needed before the copy can be throttled.
- The cost of the kernel's b3a8 unpack, which no log reports.
