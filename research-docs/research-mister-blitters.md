# Research: MiSTer hardware blitters — prior-art survey

**Task:** fpga-graphics-offload #001
**Status:** complete (survey + design input)
**Date:** 2026-06-12
**Mandate (PRD):** survey existing MiSTer 2D-acceleration cores to inform our
*custom* command-driven blitter. Literal RTL reuse is out of scope; licensing
noted for awareness only. Output feeds `fpga-hw-blitter` #002 (protocol +
reference model), #003 (RTL spike), and #005 (DDR source/bandwidth path).

---

## TL;DR — the one finding that changes our design

**Don't have the blitter write the DDR framebuffer per pixel. Blit to an on-chip
buffer with backpressure, and burst it to DDR with a separate DMA engine.**

The MiSTer Cave (CV1000) core — the closest existing analog to what we want — does
exactly this, and it's the answer to our biggest open risk (DDR bandwidth/coherency,
`fpga-hw-blitter` #005). Per-pixel blit writes are *random-access* and frequent;
DDR hates that. So the Cave blitter writes 16-bit pixels into an **on-chip**
framebuffer/line buffer port (`io_frameBuffer_wr/addr/din` + `wait_n` backpressure),
and a **separate DMA unit** moves it to DDR as **64-bit bursts** (`io_ddr_*` with
`burstLength`/`burstDone`). DDR only ever sees long sequential bursts, never
per-texel random traffic. Adopt this decoupling.

---

## Why CV1000 is the gold-standard prior art

CV1000 is Cave's last arcade platform: a **133 MHz SH-3 CPU + an Altera Cyclone
EP1C12 FPGA** acting as the graphics processor ("the Blitter"). The CPU issues
**commands** to the FPGA, which composites sprites into a framebuffer. That is
*structurally identical* to our target:

| CV1000 | Our MiSTer target |
|---|---|
| SH-3 CPU emits blit commands | Cortex-A9 emits blit display list |
| EP1C12 FPGA blitter executes | Cyclone V fabric blitter executes |
| Sprite list in VRAM | Command ring in DDR (0x3B000000 region) |
| On-chip FB → DMA to DDR VRAM | On-chip FB → DMA to DDR framebuffer |
| Video scans out the framebuffer | native_video_writer scans out the framebuffer |

The MiSTer core `MiSTer-devel/Arcade-Cave_MiSTer` (orig. Josh Bassett / nullobject)
is GPL; originally written in **Chisel (Scala→Verilog)**, now also a SystemVerilog
rewrite under `rtl/cave/`. **We do not reuse this RTL** — but its module boundaries
are a ready-made blueprint.

### CV1000 / Cave-core RTL pipeline (the blueprint)

```
 sprite list (VRAM)
        │  128-bit descriptors, up to 1024 sprites
        ▼
 SpriteProcessor.sv  ── walks the list (FSM: IDLE→LOAD→LATCH→CHECK→READY→
        │                PENDING→NEXT→DONE), emits per-sprite config + frameReady
        ▼
 SpriteDecoder.sv   ── fetches & decompresses sprite pixel data
        │              (burst reads from tile ROM/DDR, 64-bit dout)
        ▼
 SpriteBlitter.sv   ── per-sprite rect raster + zoom/flip; streams 16 px/clk;
        │              writes 16-bit pixels to FB port w/ wait_n backpressure
        ▼
 SpriteFrameBuffer.sv ── on-chip line/frame buffer + DMA engine →
        │                64-bit DDR bursts; CavePageFlipper double/triple buffer
        ▼
 ColorMixer.sv      ── priority + transparency + alpha blend
        ▼
 SystemFrameBuffer.sv ── final framebuffer DMA to DDR for MiSTer scanout
                          (page-flip on vBlank, baseAddr/stride/hSize/vSize)
```

### Concrete command/interface fields (from `SpriteBlitter.sv`)

A single blit command (`io_config_bits_sprite_*`) carries — this is a near-complete
template for our command word:

- `pos_x [17:0]`, `pos_y [17:0]` — destination position, **fixed-point** (sub-pixel,
  to support zoom)
- `cols [7:0]`, `rows [7:0]` — source size (in tiles)
- `zoom_x [15:0]`, `zoom_y [15:0]` — **integer+fractional scaling, essentially free**
- `hFlip`, `vFlip` — mirror on each axis
- `priority [1:0]` — compositing priority (handed to ColorMixer)
- `colorCode [5:0]` — palette/color selector (Cave is partly paletted; CV1000 proper
  is not — see below)
- pixel stream: **16 lanes × 8-bit** fed from the decoder per clock
- framebuffer write port: `wr`, `addr [16:0]`, `din [15:0]`, `wait_n` (backpressure)

### Pixel format & transparency (from CV1000 proper)

- **15-bit RGB (5/5/5) + 1 reserved bit**, packed 16-bit. **Not paletted** — color
  lives in the sprite data, so *no per-sprite color limit*.
- Transparency = the **reserved/last bit acts as a mask**: pixels marked transparent
  are simply **not copied** (skip the write). This is the cheap, common case.
- **8-bit alpha** blend modes on top of that for mixing effects, with **per-channel
  8-bit color multiply** (`0x80` = 100%) and separate **src/dst alpha** — i.e. it can
  do constant-alpha modulation and tinting, not just copy/colorkey.

### Timing / bandwidth data points (CV1000 hardware, via Buffi's research + MAME)

- Original board: **32-bit DDR VRAM @ 74 MHz**; replicating that bandwidth on MiSTer
  with dual SDRAM needs **>148 MHz** — VRAM bandwidth is *the* CV1000 challenge. Read
  as: budget DDR bandwidth carefully; the on-chip-buffer + burst-DMA pattern exists
  precisely to live within it.
- Source read cadence: a line of graphics data every **63.6 µs**, **2.16 µs** per read;
  each read = **32 bits × 16 clocks = 64 bytes** (a burst).
- **Clipping is a real optimization**: sprites fully outside the visible area cause
  **no memory copies** — the blitter skips them. Cull before you blit.

---

## Second example — Sega Saturn VDP1 (confirms the model, wider primitive)

The MiSTer Saturn core's VDP1 uses the *same display-list-to-framebuffer* pattern:
a **32-byte command table per primitive** is written to VRAM; the VDP1 walks the
**linked command list until an end command**, drawing into a **16bpp framebuffer**.
Differences worth noting:

- VDP1 draws **quads** (4-point distorted sprites) + polygons + polylines/lines, not
  just axis-aligned rects — more general than we need, but shows the command-list
  model scales to richer primitives if ever wanted.
- Double framebuffer with end-codes, table **jumps/links** (a real linked list, not a
  flat array) — links let the CPU patch/reuse parts of the list cheaply.

Takeaway: the **command-table-in-VRAM + walk-until-end + draw-to-FB** model is the
*de facto standard* across two independent real machines. We are on a well-trodden
path.

## Third example — the Amiga blitter (the archetype, but the wrong fit)

The epic frames our approach as "Amiga-blitter class." Worth a sentence of honesty:
the Amiga blitter is a general **bitplane block-copy** engine with masking and 256
logic minterms across up to 3 source channels. It's the *ancestor* of the idea, but
it's **bitplane/paletted and not framebuffer-sprite oriented** — CV1000/VDP1 are far
closer to our packed-pixel, alpha-blended, framebuffer-target reality. Use Amiga as
the conceptual name only; design from CV1000.

---

## Design input → our custom blitter (the deliverable's point)

Concrete decisions these findings feed into `fpga-hw-blitter`:

### → #002 Command protocol + DDR ring
1. **Command word** ≈ CV1000's: `{opcode, src_base, src_stride, src_w, src_h,
   dst_x, dst_y, blend_mode, key/alpha, flip}`. Use **fixed-point dst pos** if we
   ever want zoom; otherwise integer.
2. **Display list, walked until an END opcode** (Saturn model) rather than a fixed
   count — simplest for a variable per-frame draw count. A head/tail ring with an
   END terminator + per-frame "list ready / list done" handshake.
3. **Cheap path first:** colorkey/transparency-bit *skip-write* is the common case and
   the cheapest (no read-modify-write). Make plain copy + colorkey the fast path;
   make 8-bit alpha blend and per-channel modulate the optional slower path. (This
   matches our own [[blitter-compute-bound]] HW finding that BLEND/INTERP is the cost,
   not texel fetch — so a non-blended fast path is where the wins are.)
4. **Reserve fields for zoom_x/zoom_y and hFlip/vFlip** even if v1 ignores them —
   CV1000 gets scaling/flip almost for free in the raster loop, and Solarus/gmloader
   do use scaled blits.

### → #003 RTL spike
5. **Module split**: `RingReader (FSM)` → `[SourceFetch DMA + decode]` →
   `RectBlitter (raster + blend)` → `FrameBuffer (on-chip + DMA-to-DDR)`. Mirror the
   Cave `SpriteProcessor / SpriteDecoder / SpriteBlitter / SpriteFrameBuffer` split —
   it cleanly separates "what to draw" (control) from "move pixels" (data).
6. **Backpressure (`wait_n`) on every memory interface** — non-negotiable, because our
   DDR is shared with scanout + A9 + the 48 kHz audio ring ([[mister-ddr-input-audio]]).
7. Consider authoring in a higher-level HDL if complexity bites (Cave used Chisel),
   but default to SV/Verilog to fit the raetro/quartus CI ([[mister-rbf-ci-build]]).

### → #005 DDR source/bandwidth/coherency  ← the big one
8. **Decouple per-pixel writes from DDR.** Blit into an **on-chip** framebuffer/line
   buffer; **burst (64-bit) DMA** to the DDR framebuffer. Never let the raster loop
   touch DDR per pixel. This is the single most important lesson and directly retires
   the #005 risk's worst case.
9. **Burst all DDR masters** (`burstLength`/`burstDone`, 64-bit) to match the f2h
   SDRAM bridge — both source-surface reads and framebuffer writeback.
10. **Page-flip / double-buffer on vBlank** (CV1000 `CavePageFlipper`, Saturn dual FB)
    — reuse the existing native_video_writer double-buffer; swap on the video reader's
    vBlank, exactly as Cave swaps read/write pages.
11. **Cull fully-clipped commands** in the ring reader → zero DDR/blit cost for
    offscreen draws (CV1000 does this; our [[solarus-perf-40fps]] cull work agrees).
12. **Bandwidth budget reality check:** CV1000 needed >148 MHz-equivalent and still
    treats VRAM as the bottleneck. Our DE10-Nano DDR3 is shared three ways. Size the
    on-chip buffer and burst length so the blitter's DDR demand fits *alongside*
    scanout — measure on HW (#007), don't assume.

---

## Out-of-scope confirmations
- **No RTL reuse:** Cave core is GPL and tightly coupled to CV1000 sprite/tile formats
  and SDRAM/DDR arbiter; lifting it would import all of that. We build custom; the
  value here is the *architecture*, captured above.
- **No general GPU / quads / polygons:** Saturn VDP1's quad/polygon path is more than
  2D compositing needs. Axis-aligned rect blits (+ optional integer scale/flip) cover
  Solarus/gmloader/OpenBOR.

## Sources
- [Arcade-Cave_MiSTer (RTL: SpriteProcessor/Blitter/Decoder/FrameBuffer, ColorMixer)](https://github.com/MiSTer-devel/Arcade-Cave_MiSTer)
- [A Last Gasp of 2D: The Cave CV1000 — nicole.express](https://nicole.express/2022/games-made-in-a-cave.html)
- [CV1000 Blitter Research — Buffi (blog)](https://buffis.com/research/cv1000-blitter-research/) · [PDF](http://cave.buffis.com/docs/CV1000_Blitter_Research_by_buffi.pdf)
- [MAME PR #10849 — accurate CV1000 blitter timings (buffi)](https://github.com/mamedev/mame/pull/10849)
- [Saturn VDP1 VRAM & Command Tables — SegaXtreme](https://segaxtreme.net/threads/vdp1-vram-and-command-tables.23997/) · [VDP1 — Yabause wiki](https://wiki.yabause.org/index.php5?title=VDP1)
