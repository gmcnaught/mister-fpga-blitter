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
make demo     # composite 24 animated frames -> out/frame_NN.ppm
```

No hardware, no dependencies, no Qt. Every display list the layer emits is
executed by `refmodel/blitter_ref.c` + `refmodel/blt_tri.c` — the same golden
model the RTL is diffed against — and the resulting framebuffer is compared
against an independent CPU model of what the draw should have produced.

## The demo is the real screen, not a mock-up

`make demo` composites the **Zaparoo games-browse screen**, with every
dimension derived from the front-end's own design rules rather than invented.
`zaparoo_ui.c` ports them from
[ZaparooProject/zaparoo-frontend](https://github.com/ZaparooProject/zaparoo-frontend):
`Theme.qml` (colours, the CRT bitmap font), `Sizing.qml` (`pctH`/`pctW`/
`fontSize`/`stroke`, corner radius, header metrics, the browse-grid shape
selector, the cover decode tiers), `Motion.qml` (press duration and scale
target) and `Tile.qml` (card / focus-ring / caption geometry). Change the
screen size and the layout re-solves the way the app's would.

Two properties of those rules do the arguing:

```
  grid shape      : 3 columns x 2 rows (Sizing._selectGridShape)
  corner radius   : 8 px    tile padding 5, ring 1 px inset 1
  caption font    : 8 px (fixed-cell 6x8 bitmap face)
  cover decode    : 128 px tier for a 101 px painted box  -> every cover is
                    an arbitrary-ratio resample
```

- On the CRT path `fontSize()` collapses to **8 px** and the font is a
  fixed-cell 6×8 bitmap face with `NoAntialias` — so every label is a uniform
  glyph blit. The study's "bitmap-font freebie", straight out of the app's own
  sizing rule.
- `gamesGridCoverSourceSize()` snaps a cover's decode size **up to a tier**
  (128/256/512/768) while the painted box is whatever the grid solve produces
  (101 px here). They match only by coincidence, so **a grid of covers is a
  grid of arbitrary-ratio resamples** — which is why "just pre-scale at decode
  time" does not dispose of the problem.

The demo runs two scenes, both chosen because they are what the software
renderer struggles with:

| Frames | Scene | Why it's the interesting one |
|---|---|---|
| 0–15 | Browse grid, focused tile animating | The focused tile carries the transient push-in cue (`Motion.pressScale` 0.90 over 80 ms) **and** the persistent 1.06 focus scale `Tile.qml` deleted for being "a persistent, per-focus-move cost … on covered grids". Every frame is a different ratio. |
| 16–23 | A modal scrim fading in over that grid | The draw the team engineered away: a translucent overlay over a dense cover grid forces every cell underneath to re-rasterize, every frame. On the fabric the scrim is **one** const-alpha `FILL` over an already-composited frame. |

```
frame  cmds  fills  blits  tris  glyphs   fabric px   A9 px avoided  (fill/AA/scale/text)
    0   223     58    157     6      77      186418          186418  (147516/3244/32963/2695)
    8   223     58    157     6      77      171303          171303  (134408/3036/31164/2695)
   23   291     77    206     6     102      316873          316873  (275792/4548/32963/3570)  <- modal scrim
```

Peak 291 commands ≈ 9 KiB of ring per frame, and the A9 rasterizes nothing —
including the per-frame scale animation and the scrim.

## What's in here

| File | Role |
|---|---|
| `ui_offload.{h,c}` | The layer. UI primitives → blitter commands. Pure C, no Qt. |
| `corner_atlas.{h,c}` | Bakes antialiased quarter-disc coverage into ARGB4444 masks. |
| `font6x8.{h,c}` | Fixed-cell 6×8 bitmap font → one ARGB4444 glyph atlas. |
| `zaparoo_ui.{h,c}` | The front-end's Theme/Sizing/Motion/Tile rules, ported to C. |
| `qt_blitter_paintengine.{h,cpp}` | Seam A: a `QPaintEngine` that translates `QPainter` calls into the layer above. **Not built here** — see below. |
| `demo_frame.c` | The browse screen + modal scenes and the accounting table. |
| `test_ui_offload.c` | The gates. |

### Mapped against the real components

| Front-end element | Offload |
|---|---|
| `MainLayout.qml` tiled `bg-circuit.png` (`fillMode: Image.Tile`, `smooth: false`) | one `BLT_OP_TILELIST` for the whole screen |
| `Tile.qml` card — `surfaceCard` fill + 1 px `borderMid` edge | `uio_rounded_rect_outline()` — 14 commands |
| `Tile.qml` focus ring — *"two stacked filled rounded rectangles … filled rounded rects honour the AA path, while thin rounded borders are tessellated without subpixel coverage"* | the same construction, `uio_rounded_rect_outline()`. The QML's reason for choosing filled rects over a stroke is exactly the blitter's reason. |
| `Tile.qml` cover — `PreserveAspectFit` + `smooth: true`, decode tier ≠ painted box | `uio_fit()` + `uio_image_scaled()` → one `TRILIST` quad |
| `ScrollingCaption` label at `fontSize(2.2)` = 8 px | `uio_text()` — one PALPHA+COLORMOD blit per glyph |
| `CoreStatusPill.qml` — `radius: half(height)` track + accent progress fill | two `uio_rounded_rect()` pills |
| `Modal.qml` — `Theme.scrim` `#cc000000` over the screen, `bgPanel` panel, accent-bordered buttons | one const-alpha `FILL` + outlined rounded rects |
| `MainLayout.qml` whole-scene 90° tate `rotation:` | **not the compositor's job** — `sys/screen_rotate` at the output stage (study §5.2) |

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
  outlined rounded rect: 14 commands, ring thickness exact, corners concentric
  fabric scaling: 10 ratios bit-exact vs nearest-neighbour model
  animated zoom: 1 command/frame at every ratio, vertex arena reset
  unpadded image: scales, and the half-texel compromise is reported
  alpha art: 1:1 PALPHA blit on the fabric, scaling refused not approximated
  text: 9-glyph run, 1 blit each, colour by COLORMOD from one atlas
  aspect fit: PreserveAspectFit boxes centred and integral
  frame budget: 97 commands for 8 cards, 74592 px moved to the fabric
  zaparoo layout: 3x2 grid, r=8, 8px bitmap font, 128 px cover -> 101 px box
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
- **The font is a demo face.** The real CRT path uses MxPlus HP 100LX 6×8; a
  port bakes that TTF into the same atlas shape and nothing else changes. The
  glyph shapes here are stand-ins, and so is the procedurally generated art.
- **The layout rules are ported, the widgets are not.** `zaparoo_ui.c`
  reproduces the sizing/theme/motion tokens so the demo has the real screen's
  proportions; it is not a port of the QML components, and the upstream project
  (PolyForm-Noncommercial) contributes no code here.
- **Sources are read from the DDR heap, not staged into SDRAM.** On hardware,
  the load-time uploads would additionally be staged with
  `blt_stage_surface_perm()`; the reference model reads the heap directly, so
  the example leaves `sdram_src` off.
