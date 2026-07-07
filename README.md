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
(see `docs/lessons-learned.md`): **keep frame pixels off the shared HPS bus
entirely.**

```
  ┌──────────────────────────────────────────────────────────────────────┐
  │  Cortex-A9  (software engine: Solarus / gmloader / OpenBOR)           │
  │  builds a per-frame display list instead of compositing pixels        │
  │  (tile layers: recorded ONCE per map, replayed as 1 cmd/layer)        │
  └───────────────────────────────┬──────────────────────────────────────┘
                                   │ commands + one-time atlas uploads
                                   ▼
  ┌──────────────────────────────────────────────────────────────────────┐
  │  DDR3 (shared f2h bus) — CONTROL traffic only, no frame pixels        │
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
  │   on-chip BRAM framebuffer (320×240 RGB565)                           │
  │     WORK image ──(vblank snapshot)──▶ SCAN image                      │
  │                                          │                            │
  └──────────────────────────────────────────┼────────────────────────────┘
                                             ▼  same-cycle BRAM reads
  ┌──────────────────────────────────────────────────────────────────────┐
  │  scanout reader ──▶ HDMI / analog   (display never waits on a bus)    │
  └──────────────────────────────────────────────────────────────────────┘
```

Three architectural decisions define the core:

1. **Display list in a DDR ring, walk-until-END.** The proven submit/done
   doorbell handshake survived every architecture revision unchanged. Batch
   opcodes (`TILELIST` / `TILELIST_RES`) collapse a tile layer's thousands of
   draws into one command whose entries live in a separate buffer, recorded
   once per map in **map coordinates** — camera movement only re-biases the
   header, so the A9's per-frame emit cost is a few dozen commands.
2. **Framebuffer in on-chip BRAM, snapshot at vblank.** The compositor RMWs a
   persistent WORK image in M10K and hardware-copies WORK→SCAN at vblank for
   tear-free scanout. Destination preload/write-back traffic — 44–66 % of
   compositor cycles when the framebuffer lived in external memory — is gone
   (FILL ~1.05 cyc/px, COPY ~1.65 in sim), and scanout never touches a bus.
3. **Sources resident in SDRAM on a dedicated bus.** Atlases are staged
   DDR3→SDRAM by `STAGE` commands once at load (whole-quest residency; a
   permanent, never-freed region) and fetched as spans through a
   double-buffered line buffer that overlaps span N+1's fetch with span N's
   composite. The pipeline composites at one pixel per clock (issue-interval 1)
   with colorkey, constant alpha, per-pixel alpha (ARGB4444), saturating ADD,
   MULTIPLY, and an RGB888 source tint — **nothing escapes to software**.

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
cd refmodel && make test   # 28 contract checks + embedded self-test
cd host     && make test   # 22 emitter/codec checks + embedded self-test
cd sim      && make test   # RTL ↔ model equivalence, 11 scenarios (iverilog)
```

`refmodel/blitter_ref.h` is the machine-readable copy of the command contract
in `docs/blitter-protocol.md`. `refmodel/blitter_ref.c` defines the exact
per-pixel semantics (FILL / COPY / COLORKEY / CONST_ALPHA / per-pixel alpha
(ARGB4444) / ADD / MULTIPLY / color-mod tint / flips / clipping / tile lists /
walk-until-END) that the RTL must reproduce bit-for-bit, including the
divide-free /255 reductions. It is both the **golden output** the RTL is
diffed against and the spec host command emitters develop against.

## Hardware validation (Solarus on MiSTer)

The compositor is **validated end-to-end on real hardware** (DE10-Nano)
driving the Solarus 1.6.5 engine port (Mystery of Solarus DX, full quest).
The A9 emits the display list; the fabric composites from SDRAM-resident
atlases into the BRAM framebuffer; scanout shows correct, tear-free video with
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

## Layout

```
docs/          feasibility (go/no-go), protocol spec (the contract), lessons learned
research-docs/  prior-art survey of existing MiSTer 2D-acceleration cores
refmodel/      C reference model — golden output for the RTL, exec spec for host
rtl/           SystemVerilog v1 spike (single-FSM, DDR framebuffer) — see rtl/README
sim/           testbench + DDR model: v1 RTL ↔ reference-model equivalence
host/          host-side command emitter + heap/SDRAM allocators (engine-agnostic)
```

The **production fabric** (pipelined compositor `comp_pipeline.sv`, BRAM
framebuffer, SDRAM cache subsystem, tile-list expansion — ~5 800 lines of
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
| v1 RTL spike ↔ model equivalence in sim | ✅ `rtl/` + `sim/` — 11/11 pass |
| Host command emitter + wire codec + allocators | ✅ `host/` — checks + self-test pass |
| Pipelined compositor (1 px/clk, all blends native) | ✅ production, in `solarus-mister:fpga/rtl/` |
| Framebuffer in BRAM + vblank snapshot (tear-free) | ✅ production, HW-validated |
| SDRAM-resident whole-quest atlases (128 MB) | ✅ production, HW-validated |
| Tile-list batch opcodes (static + animated) | ✅ production, HW-validated |
| **On hardware (correct video, zero escapes, Solarus/MiSTer)** | ✅ **validated** — full quest playable |
| Lessons learned (transport, timing, sizing) | 📓 `docs/lessons-learned.md` |

## License

GPL-3.0 — consistent with the MiSTer core ecosystem (including the CAVE /
CV1000 prior art and jtframe) and the engine ports this serves. See `LICENSE`.
