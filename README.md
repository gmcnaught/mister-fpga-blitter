# mister-fpga-blitter

A command-driven **2D hardware compositor** for the MiSTer (DE10-Nano,
Cyclone V). It offloads software 2D compositing from the Cortex-A9 CPU onto the
FPGA fabric, so that software-rendered engines can run at full frame rate on
real hardware.

This is the engine-agnostic core of the *FPGA graphics-offload* effort.
Software engine ports (Solarus first, then gmloader / OpenBOR) are bottlenecked
on the A9 doing all per-pixel compositing while the fabric sits idle apart from
scanout. The blitter inverts that: the A9 emits a per-frame **display list**
(for tile maps, one `TILELIST` command per layer), and the fabric fetches
sources, composites the frame, and feeds scanout. **The A9 never touches a
pixel.**

## Data flow — from source pixels to screen

Three memories, three jobs. The design's central rule, learned the hard way
(see `docs/lessons-learned.md`): **keep per-pixel composite traffic off the
shared HPS bus.** The compositor's read-modify-write runs entirely on-chip;
the only frame pixels that cross the bus are one linear snapshot burst per
finished frame into the DDR3 scanout double-buffer, plus the reader's
line-granular bursts back out — sequential traffic the bus handles cheaply,
unlike the per-pixel RMW that originally killed it.

```
  ┌──────────────────────────────────────────────────────────────────────┐
  │  Cortex-A9  (software engine: Solarus / gmloader / OpenBOR)           │
  │  builds a per-frame display list instead of compositing pixels        │
  │  (tile layers: recorded ONCE per map, replayed as 1 cmd/layer)        │
  └───────────────────────────────┬──────────────────────────────────────┘
                                   │ commands + one-time atlas uploads
                                   ▼
  ┌──────────────────────────────────────────────────────────────────────┐
  │  DDR3 (shared f2h bus) — control traffic + the scanout FB0/FB1        │
  │   ┌─────────────┐ ┌────────────────┐ ┌────────────┐ ┌──────────────┐  │
  │   │ ctrl block  │ │ command ring   │ │ TL_BUF     │ │ upload heap  │  │
  │   │ 0x3B00_0000 │ │ 512 KiB        │ │ tile-list  │ │ (STAGE       │  │
  │   │ submit/done │ │ 0x3B00_0040    │ │ entries    │ │  staging)    │  │
  │   └──────▲──────┘ └───────┬────────┘ └─────┬──────┘ └──────┬───────┘  │
  └──────────┼────────────────┼────────────────┼───────────────┼──────────┘
        done_seq │            │ cmds           │ entries       │ STAGE (once,
             (A9 polls)       ▼                ▼               ▼  at load)
  ┌──────────────────────────────────────────────────────────────────────┐
  │  BLITTER core (FPGA fabric)                                           │
  │                                                                       │
  │   ring walk / decode / TILELIST expansion                             │
  │        │                                     ┌──────────────────────┐ │
  │        ▼                                     │ SDRAM (dedicated 2nd │ │
  │   comp_pipeline — 1 pixel/clock compositor ◀─┤ bus, 128 MB): quest  │ │
  │   COPY/KEY/ALPHA/PALPHA/ADD/MUL/tint         │ atlases, staged once,│ │
  │   (source spans via double-buffered linebuf) │ resident for session │ │
  │        │ composite / RMW                     └──────────────────────┘ │
  │        ▼                                                              │
  │   on-chip BRAM WORK framebuffer (320×240 RGB565, persistent RMW)      │
  │     WORK ──(frame-done snapshot burst)──▶ DDR3 FB0/FB1 double-buffer  │
  │              (fb_ddr_writer; fabric flips fb_bank)   │                │
  └──────────────────────────────────────────────────────┼────────────────┘
                                                         ▼ one line-granular
  ┌──────────────────────────────────────────────────────────────────────┐
  │  scanout reader (ddr3_scan_adapter, 80-qword burst per scanline)      │
  │     ──▶ HDMI / analog                                                 │
  └──────────────────────────────────────────────────────────────────────┘
```

Three architectural decisions define the core:

1. **Display list in a DDR ring, walk-until-END.** The proven submit/done
   doorbell handshake survived every architecture revision unchanged. Batch
   opcodes collapse whole layers into single commands whose entries live in
   separate buffers: `TILELIST` / `TILELIST_RES` (tile layers, recorded once
   per map in **map coordinates** — camera movement only re-biases the
   header), `SPRITELIST` (Y-sorted sprite batches, per-entry texture +
   palette), and `TILEMAP` (8px cell grids walked by the fabric with
   run-coalescing). The A9's per-frame emit cost is a few dozen commands.
2. **WORK framebuffer in on-chip BRAM; scanout double-buffer in DDR3.** The
   compositor RMWs a persistent WORK image in M10K — destination
   preload/write-back traffic, 44–66 % of compositor cycles when the
   framebuffer lived in external memory, is gone (FILL ~1.05 cyc/px, COPY
   ~1.65 in sim). The *scan copy* moved back off-chip in Stage 5 Phase 2
   (2026-07): at frame-done (immediately — not vblank-gated, which was worth
   real fps) `fb_ddr_writer` burst-streams WORK to the inactive DDR3
   framebuffer and the fabric flips `fb_bank`; the reader scans the active
   buffer with one 80-qword burst per line (`ddr3_scan_adapter`). Same
   tear-free double-buffer, ~160 M10K freed (the fit dropped from ~89 % BRAM),
   and the compositing datapath is untouched — only the finished frame ever
   crosses the bus.
3. **Sources resident in SDRAM on a dedicated bus.** Atlases are staged
   DDR3→SDRAM by `STAGE` commands once at load (whole-quest residency; a
   permanent, never-freed region) and fetched as spans through a
   double-buffered line buffer that overlaps span N+1's fetch with span N's
   composite. The pipeline composites at one pixel per clock (issue-interval 1)
   with colorkey, constant alpha, per-pixel alpha (ARGB4444), saturating ADD,
   MULTIPLY, and an RGB888 source tint — **nothing escapes to software**.
   Sources are 16bpp RGB565/ARGB4444 or 8bpp `PAL8` resolved through an
   on-chip 32-bank CLUT, which halves the atlas footprint.

## Prior art & acknowledgements

The architecture is modeled directly on **CAVE's CV1000 arcade hardware** and
its faithful FPGA recreation, the **MiSTer Cave core**. CV1000 is CAVE's last
arcade platform — a CPU that issues *commands* to an Altera Cyclone FPGA acting
as the graphics "Blitter," which composites sprites into a framebuffer. That is
structurally identical to our target (A9 → Cyclone V), which made it the
gold-standard blueprint for this project.

- **Arcade-Cave_MiSTer** — `github.com/MiSTer-devel/Arcade-Cave_MiSTer`,
  originally authored by **Josh Bassett (nullobject)**, with the MiSTer-devel
  community. GPL-licensed; originally written in Chisel (Scala → Verilog),
  later also a SystemVerilog rewrite under `rtl/cave/`.

The decisive lesson taken from that core — *blit into an on-chip buffer and
never let the display path contend with composite traffic* — proved out even
more literally than planned: the shipping design's framebuffer lives entirely
on-chip. The jtframe SDRAM subsystem by **Jose Tejada (jotego)**
(`github.com/jotego/jtframe`) provides the SDRAM controller lineage
(`jtframe_burst_sdram` + cache mux) used for the resident-atlas bus in the
production core.

> **Note on reuse:** this is an *independent, clean-room* implementation
> informed by the public design and module boundaries of the CAVE / CV1000
> core. **No RTL is copied** from Arcade-Cave_MiSTer. Both projects are
> GPL-3.0, consistent with the MiSTer ecosystem. Full survey:
> `docs/blitter-feasibility.md` and `research-docs/research-mister-blitters.md`.

## How to build / run the reference model

The software reference model is the executable spec — it builds and runs with
no hardware and no dependencies:

```sh
cd refmodel      && make test   # 34 contract checks + TRILIST goldens + embedded self-test
cd host          && make test   # 28 emitter/codec checks + self-test + grid gates
cd libmfgpu      && make test   # transform/cull → TRILIST display-list end-to-end
cd sim           && make test   # RTL ↔ model equivalence, 17 scenarios (iverilog)
cd prototypes/qt && make test   # UI-offload gates (AA corners, fabric scaling, text)
```

`refmodel/blitter_ref.h` is the machine-readable copy of the command contract
in `docs/blitter-protocol.md`. `refmodel/blitter_ref.c` defines the exact
per-pixel semantics (FILL / COPY / COLORKEY / CONST_ALPHA / per-pixel alpha
(ARGB4444) / PAL8+CLUT / ADD / MULTIPLY / color-mod tint / flips / clipping /
tile, sprite and grid lists / textured triangles / walk-until-END) that the
RTL must reproduce bit-for-bit, including the divide-free /255 reductions. It
is both the **golden output** the RTL is diffed against and the spec host
command emitters develop against.

## Hardware validation (Solarus on MiSTer)

The compositor is **validated end-to-end on real hardware** (DE10-Nano)
driving the Solarus 1.6.5 engine port (Mystery of Solarus DX, full quest).
The A9 emits the display list; the fabric composites from SDRAM-resident
atlases into the BRAM WORK framebuffer (scanned out from the DDR3
double-buffer since Stage 5); scanout shows correct, tear-free video with
`escape=0` — every draw of every frame runs on the fabric, across title,
overworld, dungeons, dialogs, and scene transitions. A whole quest's atlases
(~60 MiB) preload into SDRAM at load with an on-screen progress bar.

The A9's software renderer managed ~20 fps on heavy overworld maps. Moving
the pixels to the fabric then exposed the next wall: at ~3 800 per-tile
commands/frame the A9 became *emit*-bound (~7 fps on the densest maps) —
which is what the tile-list opcodes were built to kill. With tile lists +
residency the fabric composites those same frames from ~1–3 commands per
layer, and the remaining frame cost is game logic, not graphics.
The engine backend lives in the `solarus-mister` repo and vendors `host/` +
`refmodel/` from here.

## MFGPU triangle front-end (in progress)

Beyond rectangular blits, this repo is growing a minimal textured-triangle
path — enough of a GPU for GLES-style 2.5D front-ends without leaving the
32-byte command contract:

- **`BLT_OP_TRILIST` (12)** — header-only command pointing at 16-byte
  `blt_vtx_t` vertex triples in a separate vertex buffer; the rasterizer
  (`refmodel/blt_tri.c` golden, `rtl/blt_tri.sv` bit-exact in sim) draws
  textured, per-vertex-alpha triangles through the same blend modes.
- **`BLT_OP_SET_TARGET` (13)** — switches compositing between the WORK
  framebuffer and an off-screen app-surface render target, which TRILIST
  draws can sample as a texture (`flags.SRC_SURFACE`) for render-to-texture
  effects.
- **`libmfgpu/`** — the A9-side front-end: fixed-point MVP transform to
  screen 12.4, back-face/off-screen cull, batch assembly into TRILIST
  display lists via the host emitter.

Validated against the reference model and in RTL simulation (`sim/` tri
scenarios); not yet deployed in the production solarus-mister fabric. The
opcodes were renumbered 10→12 / 11→13 when `SPRITELIST`/`TILEMAP` shipped on
hardware holding 10/11.

## UI front-end offload (`prototypes/qt/`)

The same inversion applied to a **software-rendered UI front-end** rather than a
game engine: a Qt Quick Software / `QPainter` app (no GPU on the Cyclone V, so
the A9 rasterizes the whole scene) stops touching pixels and emits a display
list instead. The present path is unchanged — same DDR double-buffer, same
doorbell; only the frame's *source* moves to the fabric.

`prototypes/qt/` is a worked example of the two draws a fixed-function blitter
is usually said to be unable to do, both gated against the golden model:

- **Antialiased rounded corners** — the flat interior is three `FILL`s and each
  arc is one baked ARGB4444 coverage mask blitted with `HFLIP`/`VFLIP` under
  `BLT_BLEND_PALPHA`. The mask is colour-free (white RGB, coverage in A4) and
  gets its colour from `BLT_F_COLORMOD`, bit-exactly matching a `FILL` of the
  same RGB565 across all 65536 colours. 7 commands, zero A9 pixels.
- **Arbitrary-ratio image scaling** — exact ratios take the plain `BLIT` fast
  path; anything else becomes a two-triangle `BLT_OP_TRILIST` quad and the
  fabric resamples, bit-exactly reproducing nearest-neighbour sampling. One
  command per draw *at any ratio*, which is what makes a per-frame animated
  zoom free on the A9 — the case decode-time pre-scaling cannot serve.

Plus a 6×8 bitmap-font glyph atlas and a `QPaintEngine` adapter for the Qt seam
(not built here — this repo has no Qt dependency). `make demo` composites the
**Zaparoo front-end's browse screen**, with the layout derived from that
project's own `Theme`/`Sizing`/`Motion`/`Tile` rules rather than mocked up: a
3×2 cover grid whose focused tile animates every frame, then a modal scrim
fading in over it — the two draws a software renderer struggles with — at a
peak of 291 commands/frame. Study: `docs/qt-offload-feasibility.md`.

## Layout

```
docs/          feasibility (go/no-go), protocol spec (the contract), lessons learned,
               Qt front-end offload study
research-docs/  prior-art survey of existing MiSTer 2D-acceleration cores
refmodel/      C reference model — golden output for the RTL, exec spec for host
rtl/           SystemVerilog v1 spike (single-FSM, DDR framebuffer) + the
               TRILIST rasterizer (blt_tri.sv) — see rtl/README
sim/           testbench + DDR model: spike RTL ↔ reference-model equivalence
host/          host-side command emitter + heap/SDRAM allocators + tilemap
               grid builders (engine-agnostic)
libmfgpu/      MFGPU geometry front-end: fixed-point transform/cull turning
               triangle batches into TRILIST display lists — see below
prototypes/qt/ UI front-end offload example: AA rounded corners as baked
               coverage sprites, arbitrary-ratio scaling as TRILIST quads,
               6x8 glyph atlas, QPaintEngine seam — see above
```

The **production fabric** (pipelined compositor `comp_pipeline.sv`, BRAM WORK
framebuffer with DDR3 scanout, SDRAM cache subsystem, tile-list expansion — ~5 800 lines of
SystemVerilog with its own gating testbench suite) is developed in the
`solarus-mister` integration repo under `fpga/rtl/`, where it can be built into
a full MiSTer core and validated on hardware. This repo remains the home of
the **contract**: the protocol spec, the golden reference model, and the host
emitter library that any engine port reuses.

## Status

| Stage | State |
|-------|-------|
| Feasibility / architecture (go/no-go) | ✅ **GO** — `docs/blitter-feasibility.md` |
| Command protocol + DDR ring + handshake | ✅ shipped v2 — `docs/blitter-protocol.md` |
| Software reference model + tests | ✅ `refmodel/` — contract checks + self-test pass |
| v1 RTL spike ↔ model equivalence in sim | ✅ `rtl/` + `sim/` — 17/17 pass |
| Host command emitter + wire codec + allocators | ✅ `host/` — checks + self-test pass |
| Pipelined compositor (1 px/clk, all blends native) | ✅ production, in `solarus-mister:fpga/rtl/` |
| WORK framebuffer in BRAM (on-chip RMW, tear-free double-buffer) | ✅ production, HW-validated |
| Scanout from DDR3 double-buffer (frame-done snapshot, ~160 M10K freed) | ✅ production, HW-validated (Stage 5) |
| SDRAM-resident whole-quest atlases (128 MB) | ✅ production, HW-validated |
| Tile-list batch opcodes (static + animated) | ✅ production, HW-validated |
| Sprite-list batch opcode (`SPRITELIST`, per-entry texture/palette) | ✅ production, HW-validated |
| Tilemap grid-walk opcode (`TILEMAP`, 8px cell grids + host builders) | ✅ production, HW-validated |
| 8bpp paletted sources (`PAL8` + on-chip CLUT, halves atlas) | ✅ production, HW-validated |
| MFGPU triangle front-end (`TRILIST`/`SET_TARGET`, `libmfgpu/`) | 🧪 sim + model validated, not yet deployed |
| Qt front-end offload study (go/no-go) | ✅ **conditional GO, value-gated** — `docs/qt-offload-feasibility.md` |
| UI offload example (AA corners + fabric scaling + glyph atlas) | 🧪 model-validated example — `prototypes/qt/` |
| **On hardware (correct video, zero escapes, Solarus/MiSTer)** | ✅ **validated** — full quest playable |
| Lessons learned (transport, timing, sizing) | 📓 `docs/lessons-learned.md` |

## License

GPL-3.0 — consistent with the MiSTer core ecosystem (including the CAVE /
CV1000 prior art and jtframe) and the engine ports this serves. See `LICENSE`.
