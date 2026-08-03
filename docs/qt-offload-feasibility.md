# Qt front-end draw-offload feasibility (generic study)

**Question:** can a software-rendered Qt front-end on MiSTer offload its draw
path onto the fabric the way `solarus-mister` offloaded SDL compositing?

**Worked example:** [`prototypes/qt/`](../prototypes/qt/) — a running
implementation of the two mitigations this document argues for (antialiased
corners as baked coverage sprites, arbitrary-ratio scaling as `TRILIST` quads),
gated against the golden reference model. Its demo composites the Zaparoo
front-end's browse screen with the layout derived from that project's own
`Theme`/`Sizing`/`Motion`/`Tile` rules, so the shapes and proportions are the
ones a real front-end actually draws.

---

## TL;DR

**Architecturally clean; on value, conditional.** A Qt app on a Cyclone V has
no GPU, so Qt Quick runs its **Software** adaptation and `QPainter` rasterizes
the entire scene on the Cortex-A9 — structurally the same "A9 owns every pixel,
fabric only scans out" starting point Solarus had. If the app already presents
through a DDR double-buffer with a doorbell (the standard MiSTer pattern), the
transport is already blitter-shaped and only the *source* of the frame changes.

The tension is not capability, it is **cost distribution**:

> The operations that are expensive on the A9 are disproportionately the ones a
> fixed-function blitter cannot do — antialiased rounded corners, arbitrary-ratio
> bilinear image scaling, glyph rasterization — while the operations that map
> cleanly (solid fills, tiled backgrounds, 1:1 blits) are already cheap.

Offloading only the cheap column moves little. The offload is worth building
when either (a) the expensive column is made mappable by moving its
antialiasing into the **source assets** and its resampling onto the **triangle
path**, or (b) the goal is not "make today's frame faster" but "**make
per-frame recompositing cheap enough to allow motion the software renderer
currently bans**".

| | Finding |
|---|---|
| Transport | ✅ A DDR double-buffer + doorbell present path is the blitter's contract already (`docs/blitter-protocol.md`). Swapping "memcpy a software-rendered framebuffer" for "the fabric composites a display list into that slot" reuses it verbatim. |
| Rendering model | ✅ No GPU ⇒ Qt Quick Software ⇒ one `QPainter` sees the whole scene. The interception seam is singular. |
| Draw profile | ⚠️ Depends on the app. A front-end already tuned for a software renderer (no gradients, shaders, `MultiEffect`, `Canvas`, `Shape`, layer compositing) is *already* the subset a fixed-function blitter wants. |
| Workload shape | ⚠️ An event-driven menu composites on navigation and then idles. There may be **no sustained per-frame load to offload** — unlike Solarus's measured ~19 ms/frame at 60 fps. |
| Expensive ops | ⚠️ AA corners, arbitrary-ratio scaling and glyphs are where the A9 time is, and none of them map without mitigation (§3). |
| Resolution | ⚠️ A full-screen on-chip WORK image fits at ~320×240 RGB565 (~150 KB M10K). Higher modes need a **banded write-through cache** (§5.1). |
| Text | ✅ A fixed-cell **bitmap** face atlases trivially; a proportional antialiased face works too, through a glyph cache the A9 fills once per (glyph, size) — coverage in alpha, colour from a CLUT ramp, a whole screen in one `SPRITELIST` (§4). |

## 1. What is already in place

The hard part of a MiSTer offload — a proven present path from the A9 to the
fabric — usually already exists in these apps: a `/dev/mem` mapping of a DDR
region, two frame slots, a doorbell word published with a `seq_cst` fence, and a
custom scanout reader in the fabric consuming it per vblank. The blitter
presents the *same way* (`docs/blitter-protocol.md`: DDR command ring + control
block + submit/done doorbell + double-buffer).

So the work is **not** "invent a transport" — it is "replace the frame source",
exactly the Solarus inversion, where the scanout reader was untouched. That is
what makes the integration risk low.

**The seam.** Because there is no GPU, Qt Quick composites through `QPainter`,
so a custom paint path — a `QPaintEngine` behind the backing store, or a
`QSGSoftwareRenderer`-level adapter — captures the *entire* scene in one place.
There is no widget/QML split to bridge. (`prototypes/qt/qt_blitter_paintengine.{h,cpp}`
is that adapter. The Qt Quick scene-graph → triangle path is moot when no GPU
scene graph is in use.)

## 2. The real question: is the A9 the bottleneck?

This is where a menu front-end diverges sharply from a game engine.

Solarus was a **sustained** compositor: ~150 blits into a 320×240 surface every
frame, measured at ~19 ms of A9 time — a hard, continuous ceiling the fabric
could demolish (`docs/blitter-feasibility.md` §1). A front-end menu is
**event-driven**: it composites a screen on navigation and then goes idle. At
352×240 an occasional full recomposite is cheap, and between inputs there is
nothing to offload.

But there is usually a known pain point, and it reframes the value: teams
building for a software renderer routinely **remove fades, slides and held-focus
animation**, because a translucent overlay over a dense grid forces every cell
to re-rasterize per frame. The jank is engineered away by not animating.

So the honest framing is not "make the current UI faster". It is:

> **An FPGA compositor that makes full-screen per-frame recompositing cheap
> lifts the constraint that bans motion in the first place.**

That is a real win *if richer motion is wanted*. If instant cuts are fine, the
offload buys little.

## 3. The mapping — and the mitigations that make it worth doing

By painted **area**, a steady-state frame is dominated by blitter-friendly work:

| Maps cleanly | How |
|---|---|
| Tiled background | one `TILELIST` command for the whole screen |
| Cover / logo / icon images | 1:1 `BLIT`, or `PALPHA` for per-pixel alpha |
| Solid rectangles (cards, bars, backstops) | `FILL` |
| Static `opacity` | `CONST_ALPHA` fill/blit |
| Rectangular `clip: true` | destination-rect clamp |

The **cost**, however, lives in the column that does not map:

1. **Antialiased rounded corners**, on virtually every surface (cards, focus
   rings, modals, pills). The interior is a `FILL`; the arcs are AA polygon
   raster.
2. **Arbitrary-ratio image scaling** (`PreserveAspectFit`, `smooth: true`).
3. **Glyph text** via FreeType raster.
4. **Whole-scene 90° rotation** — which need not touch the compositor at all
   (§5.2).
5. **Transient press/activate scale cues** — brief resamples of one element.

The mitigations, each of which is a design constraint rather than a blocker:

- **Corners → baked coverage sprites.** Bake four ARGB4444 quarter-disc masks
  per radius with the antialiasing in their *alpha*, and blit them `PALPHA`.
  Coverage AA with no rasterizer. Keep the mask colour-free (white RGB) and
  supply the colour per draw through `BLT_F_COLORMOD`, so one bake serves the
  whole theme. Fold a translucent surface's opacity into the baked coverage —
  `PALPHA` has no constant-alpha input.
  *(Implemented: `prototypes/qt/corner_atlas.c`, `uio_rounded_rect()`.)*
- **Scaling → two paths.** Pre-scale at decode time to the exact on-screen size
  where the size is known and stable (image providers usually resize on their
  decode threads already) so the draw becomes a 1:1 blit; and for ratios that
  change per frame — a focus zoom, a held-focus animation — emit a
  **`BLT_OP_TRILIST` textured quad** and let the fabric resample. Sampling is
  nearest-texel, which is the fidelity trade; the win is that a per-frame
  changing ratio costs one 32-byte command.
  *(Implemented: `uio_image_scaled()`.)*

  **Do not assume the pre-scale is available.** A cover pipeline typically
  snaps decode sizes to a **tier ladder** (Zaparoo: 128/256/512/768, mirroring
  what its Core delivers) precisely so the request size equals the decode size
  and small resolution wobble does not move the tier and force a re-decode. The
  painted box, meanwhile, comes from the grid solve — 101 px against a 128 px
  tier at 240p. The two agree only by coincidence, so **a grid of covers is a
  grid of arbitrary-ratio resamples by design**, and "emit the exact on-screen
  size instead" is not a free change: it trades the tier ladder's decode-cache
  stability for the 1:1 blit. Where that trade is not wanted, the triangle path
  is what is left.
- **Text → a glyph cache.** A fixed-cell bitmap face atlases directly; a
  proportional antialiased face works too, through a coverage atlas the A9
  fills once per (glyph, size) — see §4, which is where the interesting version
  of this argument lives.
  *(Implemented: `prototypes/qt/font6x8.c` + `glyph_cache.c`.)*
- **Rotation → the output stage** (§5.2), off both the A9 and the compositor.

## 4. Text — including the general case

Text is usually named as the hard part of a UI offload. It is not, and it is
worth being precise about why, because the easy version of the argument (a
fixed-cell bitmap font atlases trivially) only covers the CRT path.

**The fixed-cell case.** A CRT path typically forces a bitmap face with
`NoAntialias` anyway. Every glyph is a uniform cell, so the atlas is baked once
and each glyph is one blit. Nearly free, but it has one size, one advance
width, no antialiasing, and no subpixel positions.

**The general case.** Proportional, antialiased, scalable text also composites
on the fabric, and the reason is that **the fabric never has to rasterize text
in order to draw it**. An outline is rasterized once, by the A9, at the size it
is first seen; every frame after that is a blit of a cached coverage bitmap.
For an offload only the steady state matters, and in the steady state text is
pixels being *moved*, not pixels being *computed*.

That makes "generalise text" three separate problems, and the contract already
answers each:

1. **Antialiasing → alpha.** Coverage lives in the source's alpha channel,
   exactly like the baked corner masks, and `BLT_BLEND_PALPHA` composites it.
   AA needs alpha in the atlas, not a rasterizer in the fabric.
2. **Colour → the CLUT.** Make the atlas colour-free by letting a texel *be* a
   coverage level: a `BLT_FMT_PAL8` glyph resolves through a 16-entry ramp
   whose entries share one RGB565 with alpha climbing 0..15. One atlas serves
   every colour, at 1 byte per texel, and a colour change is a palette
   selection rather than a re-bake. (A ramp is 16 of a bank's 256 slots, so the
   CLUT holds 512 text colours.)
3. **Per-frame cost → the sprite list.** A `BLT_OP_SPRITELIST` entry carries
   its *own* palette word, so an entire screen of text — mixed colours included
   — is ONE command instead of one per glyph.

Both `PAL8`+CLUT and `SPRITELIST` are production, hardware-validated paths, so
none of this needs new fabric. Worked implementation:
`prototypes/qt/glyph_cache.{h,c}`, gated against the reference model. In the
demo's detail pane, 111 glyphs at three sizes in four colours cost **one
SPRITELIST command and zero A9 raster** once the cache is warm.

**What this does not fix**, and must be stated rather than glossed:

- **Rasterization stays on the A9.** The cost is per distinct
  (code point, size, subpixel phase), paid once. A menu's working set is a few
  hundred glyphs; after that it is zero. But a **continuously animating text
  size** never amortises — every size is a fresh raster, and the triangle path
  cannot rescale a cached glyph because it samples no alpha channel. Scale the
  box, not the type.
- **16 coverage levels.** A4 is the alpha width every per-pixel-alpha source in
  this contract carries, so 16 levels is the ceiling for AA anywhere, text
  included. Measured against ideal 8-bit coverage that is ≤8/255 of alpha and
  ≤1 step of a 5-bit channel after the blend — below an RGB565 LSB, i.e. not
  the thing that will look wrong.
- **Subpixel positioning costs cache entries.** N phases multiply the glyph
  count by N. Pixel-aligned text (phases = 1) is free and is what a bitmap face
  wants; smooth horizontal motion — a marquee caption — wants 3 or 4.
- **The atlas is read asynchronously.** A glyph drawn in the last two frames
  cannot be evicted, because the fabric may still be reading the frame the A9
  just submitted. An over-subscribed atlas therefore *refuses* new glyphs
  rather than corrupting a frame in flight — which means text can go missing,
  which means the refusal count is a number an app has to watch (and size its
  atlas by), not an internal detail.
- **No sub-pixel RGB (LCD) filtering.** Grayscale coverage only.

## 5. Gates

### 5.1. Framebuffer resolution — a banded write-through cache

The blitter's central win is a WORK image in on-chip M10K, read-modify-written
on-chip so per-pixel alpha never touches DDR; only the finished frame bursts out
sequentially (repo README, `docs/lessons-learned.md`). A **full-screen** WORK
image only fits at ~320×240 RGB565 (~150 KB); 720×480×2 = 691 KB does not.

The fix is to make WORK a multi-line **write-through band** rather than a full
screen — tile/band-based (sort-middle) rendering, the standard way tile
compositors scale past on-chip capacity. Per band: walk the display list in
painter's order, skip commands that do not intersect the band, composite the
intersecting rows in BRAM, then write the band through to the DDR scanout slot
as one linear burst.

This preserves the invariant that matters: the alpha RMW stays on-chip and the
destination is never read back from DDR, so there is still zero per-pixel DDR
RMW. DDR *write* traffic is identical to today's full-frame snapshot, just
chunked; each output row lives in exactly one band, so source rows are still
fetched once. BRAM drops from O(frame) to O(band).

Two costs, both usually acceptable:

- **The command ring is re-walked once per band.** Commands are 32 B; use a
  light Y-binning pass for large lists, or replay-and-clip for a few-hundred-
  command UI. Painter's order is preserved *within* each band, which is all
  correctness requires.
- **It forbids destination-readback effects** — anything sampling
  already-composited pixels outside the current band (`SRC_FB`, the `TRILIST`
  app-surface render target, a blur or `OpacityMask`). Record this: it closes
  the door on those effects later.

Design the WORK cache as banded from day one and the higher modes become a
config change rather than a rearchitecture.

### 5.2. What the output stage can do — and what it cannot

**Rotation → `sys/screen_rotate`: yes.** The standard MiSTer tate component
rotates the whole frame by buffering it in DDRAM and reading it back
transposed; the pipeline order is core → mixer → `screen_rotate` → `ascal` →
output. The UI composites **upright** and the 90° happens at the output stage,
so the blitter needs no transpose mode (it has only `HFLIP`/`VFLIP`) and Qt
drops its full-scene `rotation:` transform. This composes with the banded cache:
band compositing writes a complete upright DDR frame, which `screen_rotate`
reads transposed. Costs: a DDR frame buffer, one frame of latency, and strided
reads.

**Antialiasing → not recoverable at the output stage.** Moving compositing to
the fabric means losing Qt's per-element AA, and `ascal` cannot give it back:

- `ascal` is a **whole-frame polyphase resampler at scanout** with no coverage
  or geometry input, so it cannot synthesise geometric edge AA that was not
  computed at raster time — scaling a hard corner yields a scaled hard corner.
- Its legitimate role is the composite-res → output-res upscale. For **HDMI**
  that is a freebie (composite at 320×240 or 352×240 and let `ascal` upscale,
  which also keeps bands small). For **analog CRT**, output is native res and
  `ascal` contributes nothing.
- **Supersample-and-downscale buys ~nothing** for a fill+blit compositor: fills
  are pixel-aligned and blitted sprites carry their own alpha, so 2× + downsample
  adds no edge AA that is not already in the sprites, at 4× the pixel cost.

So AA **moves into the source assets** — which is exactly the §3 mitigation
list, restated as the AA strategy.

**Reachability caveat:** both components live in the sys video pipeline. If the
app uses a custom scanout from its own DDR region rather than the sys mixer/FB
path, confirm the wiring before assuming either is available; you may need to
instantiate `screen_rotate` or fold a transposed read into the reader, and
`ascal` is bypassed on analog anyway.

### 5.3. 32bpp → RGB565

A 32bpp present path composited as RGB565 either converts (possible banding on
subtle UI tones) or widens the fabric framebuffer to RGB888 (double the BRAM —
much cheaper once §5.1's banding makes the buffer O(band)). Measure against the
actual theme art.

### 5.4. Fallback share, weighted by time

The single most decisive number, and it must be read **with cost weighting**:
not "what fraction of draws fall back" but "what fraction of A9 *time* falls
back". Corner/scaling/glyph time dominates unless the §3 mitigations are
adopted — which is precisely what moves that time onto the fabric. A fallback
path must therefore record destination **area**, not just a call count, and must
never be silent. (`prototypes/qt/qt_blitter_paintengine.cpp`'s `fallbackRaster()`
does exactly this.)

## 6. Recommendation — measure before building

**Do not start with a binding.** Start with the two measurements that decide
whether any of it is worth it:

1. **Is the A9 dropping frames, and where?** Instrument the present cadence and
   the software renderer's frame time on real hardware for the plausible
   hotspots: a dense grid scroll, and a full-screen recomposite at the target
   mode. If the UI already hits its frame rate, the honest answer is **no-go on
   value grounds** — the architecture is clean but there is nothing to speed up.
2. **Is richer motion wanted?** If the goal is the fades/slides/held-focus
   animation the software renderer bans, that is the actual justification —
   scope it explicitly, because it changes the target from "match today's frame"
   to "composite a dense grid every frame", which forces the §3 mitigations.

**If a bottleneck (or a motion goal) is confirmed, the tractable v1 is narrow:**

- Target the mode whose full-screen WORK image fits first, but design WORK as a
  **banded write-through cache from day one** (§5.1).
- CRT/bitmap-font path first: it gets the glyph-atlas freebie; AA comes from
  baked alpha sprites, not from `ascal` (§5.2).
- Make images mappable: pre-scale at decode to exact sizes where the size is
  stable, and use the triangle path where the ratio is animated.
- Accept square corners, or blit baked corner sprites.
- Reuse the existing present contract verbatim; the fabric composites the
  display list into the slot the reader already scans.
- Emit through `host/blt_emitter.h`, intercepting at the Software adaptation's
  `QPainter` (`prototypes/qt/`).

Everything outside that stays on the A9 as an **explicit, measured** fallback —
never a silent drop.

## Verdict

**Conditional GO, value-gated rather than capability-gated.** The capabilities
line up: the present path usually already exists, rendering is software-only,
a front-end tuned for a software renderer has already pruned what a blitter
cannot do, and a fixed-cell bitmap font makes text the easy case. What is
typically missing is a **demonstrated need** — an event-driven menu with no
sustained compositing load, whose one known constraint the team has already
designed around by removing motion.

Prove the bottleneck (or commit to the motion goal), constrain the first target,
and this is a clean, low-risk offload on a transport that is already built.
Absent that proof, it is elegant engineering in search of a problem — which is
why `prototypes/qt/` reports what it moved and refuses to call it a measurement.
