# `prototypes/qt` — a software-rendered UI front-end, composited by the fabric

A worked example of the offload argued in [`docs/qt-offload-feasibility.md`](../../docs/qt-offload-feasibility.md):
a Qt Quick **Software**-adaptation front-end (no GPU on the Cyclone V, so
`QPainter` rasterizes the whole scene on the Cortex-A9) stops rasterizing and
instead emits a per-frame blitter display list that the fabric composites.

The transport does not change. The application keeps its existing DDR
double-buffer + doorbell present path; only the **source** of the frame changes —
from a memcpy of a software-composited framebuffer to a display list the fabric
executes. That is the same inversion `solarus-mister` did to SDL compositing.

This directory exists to answer the study's two hardest objections in code:

| Objection | Answer here |
|---|---|
| "Antialiased rounded corners are on ~every surface and don't map to a fixed-function blitter." | `uio_rounded_rect()` — 3 `FILL`s for the flat interior, plus **one baked ARGB4444 coverage mask blitted four times** (`HFLIP`/`VFLIP`) under `BLT_BLEND_PALPHA`. 7 commands, zero A9 pixels. |
| "Arbitrary-ratio cover scaling is nearest-texel or nothing." | `uio_image_scaled()` — exact ratios take the cheap `BLIT` path; **any other ratio becomes a two-triangle `BLT_OP_TRILIST` quad and the fabric resamples**. 1 command, at any ratio, changing every frame. |

## Build and run

```sh
cd prototypes/qt
make test     # the gates below, against the golden reference model
make demo     # composite 12 animated frames -> out/frame_NN.ppm
```

No hardware, no dependencies, no Qt. Every display list the layer emits is
executed by `refmodel/blitter_ref.c` + `refmodel/blt_tri.c` — the same golden
model the RTL is diffed against — and the resulting framebuffer is compared
against an independent CPU model of what the draw should have produced.

`make demo` prints a per-frame table and writes PPMs of a cover-grid menu:
a tiled background, a header bar, eight rounded cover cards with antialiased
corners, bitmap-font labels, a status pill, and a **focused card whose art is
re-scaled by the fabric on every frame**.

```
frame  cmds  fills  blits  tris  glyphs   fabric px   A9 px avoided  (fill/AA/scale/text)
    0   162     32    120     8      76       88344           88344  (60828/1816/23040/2660)
    6   162     32    120     8      76       89439           89439  (60828/1816/24135/2660)
```

162 commands ≈ 5 KiB of ring per frame, and the A9 rasterizes nothing —
including the zoom, whose scale ratio is different on every frame.

## What's in here

| File | Role |
|---|---|
| `ui_offload.{h,c}` | The layer. UI primitives → blitter commands. Pure C, no Qt. |
| `corner_atlas.{h,c}` | Bakes antialiased quarter-disc coverage into ARGB4444 masks. |
| `font6x8.{h,c}` | Fixed-cell 6×8 bitmap font → one ARGB4444 glyph atlas. |
| `qt_blitter_paintengine.{h,cpp}` | Seam A: a `QPaintEngine` that translates `QPainter` calls into the layer above. **Not built here** — see below. |
| `demo_frame.c` | The animated cover-grid frame and its accounting table. |
| `test_ui_offload.c` | The gates. |

### The two mechanisms, in one paragraph each

**Antialiased corners.** A quarter disc's coverage is baked once per
`(radius, alpha)` by integer supersampling into an ARGB4444 mask whose RGB is
**white** and whose A4 is coverage. The draw supplies the colour through
`BLT_F_COLORMOD`, so one bake serves every theme colour — and because
`uio_565_to_888()`'s bit-replication expansion is the exact inverse of the
fabric's `round(ch*mod/255)` reduction, a tinted arc is *bit-identical* to a
plain `FILL` of the same RGB565 (gated over all 65536 colours). A translucent
card bakes its opacity into the coverage rather than falling back, because
`BLT_BLEND_PALPHA` has no constant-alpha input.

**Image scaling.** A scaled draw becomes two textured triangles whose uv range
is biased down by half a texel, which turns the rasterizer's
`round()`-to-nearest sampling into standard nearest-neighbour `floor()`
sampling. That bias is why `uio_upload_image()` surrounds every image with a
one-texel replicated border: the leading edge's uv would otherwise have to go
negative, and `blt_vtx_t`'s uv is unsigned. The border also stops the sampler's
clamp from bleeding a neighbouring atlas entry in.

## Gates (`make test`)

```
  tint round-trip: all 65536 RGB565 colours exact
  corner coverage: monotone, symmetric, antialiased (r=2..24)
  rounded rect: 7 commands, gap-free, mirror-symmetric, AA edge
  pill: radius clamped to h/2, ends rounded
  translucent card: opacity baked into coverage, cache keyed on alpha
  fabric scaling: 10 ratios bit-exact vs nearest-neighbour model
  animated zoom: 1 command/frame at every ratio, vertex arena reset
  unpadded image: scales, and the half-texel compromise is reported
  alpha art: 1:1 PALPHA blit on the fabric, scaling refused not approximated
  text: 9-glyph run, 1 blit each, colour by COLORMOD from one atlas
  aspect fit: PreserveAspectFit boxes centred and integral
  frame budget: 97 commands for 8 cards, 74592 px moved to the fabric
```

Two ratios are pinned at a one-texel tolerance rather than bit-exactness: a
destination pixel centre that lands *exactly* on a source texel boundary can
resolve to either neighbour, because the rasterizer interpolates uv in 12.4
fixed point with the top-left fill rule's one-unit edge bias (`refmodel/blt_tri.c`).
That is a property of the golden rasterizer, not of the uv mapping, so the test
pins it to "the adjacent texel" instead of waving it through.

## The Qt adapter

`qt_blitter_paintengine.{h,cpp}` is the interception seam: with Qt Quick's
Software adaptation, one `QPaintEngine` sees the entire scene, so there is no
widget/QML split to bridge.

| `QPainter` call | Offload |
|---|---|
| `fillRect` / `drawRects` (axis-aligned) | `uio_fill` → `FILL` |
| `drawRoundedRect` / `drawPath` (uniform-radius RR) | `uio_rounded_rect` → `FILL` + PALPHA arcs |
| `drawImage` / `drawPixmap`, 1:1 | `uio_image_blit` → `BLIT` |
| `drawImage` / `drawPixmap`, scaled | `uio_image_scaled` → `TRILIST` quad |
| `drawTextItem`, fixed bitmap font | `uio_text` → PALPHA + COLORMOD glyph blits |
| anything else | rasterized on the A9, uploaded, blitted — **and counted** |

It is **not compiled by this Makefile**: the repository has no Qt dependency.
Build it in the application's own project with `-DMFB_HAVE_QT` and Qt 5.15/6
headers; without that define the translation unit is empty. The gated contract
is the C layer it delegates to. Everything the adapter cannot map goes through
`fallbackRaster()`, which records the destination **area** it rasterized —
because the study's decisive number is not what fraction of *draws* fall back
but what fraction of A9 *time* does, and a silent fallback would hide exactly
that.

## What this does not claim

- **It is not a measurement.** `uio_stats_t` counts emit-side work: commands
  emitted, destination pixels handed to the fabric, and the pixels a `QPainter`
  path would have rasterized instead. The study's first recommendation stands:
  measure whether the A9 actually drops frames before building any of this.
- **`TRILIST` is not on hardware yet.** The triangle path is validated in
  simulation and against the reference model; the production `solarus-mister`
  fabric does not dispatch opcode 12 yet (see the repo README's status table).
  The scaling path here is therefore exercised against the golden model only.
- **The geometry is the model's 320×240 RGB565.** The study targets 352×240
  first and reaches 480i through a banded write-through WORK cache — a
  fabric-side change that does not alter the display list this layer emits.
- **The font is a demo face.** A real port bakes its own fixed-cell font
  (e.g. MxPlus HP 100LX 6×8) into the same atlas shape; nothing else changes.
- **Sources are read from the DDR heap, not staged into SDRAM.** On hardware,
  the load-time uploads would additionally be staged with
  `blt_stage_surface_perm()`; the reference model reads the heap directly, so
  the example leaves `sdram_src` off.
