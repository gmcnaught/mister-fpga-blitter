# mister-fpga-blitter

A command-driven **2D hardware blitter** for the MiSTer (DE10-Nano, Cyclone V).
It offloads software 2D compositing from the Cortex-A9 CPU onto the FPGA fabric,
so that software-rendered engines can run at full frame rate on real hardware.

This is the engine-agnostic core of the *FPGA graphics-offload* effort. Software
engine ports (Solarus first, then gmloader / OpenBOR) are bottlenecked on the A9
doing all per-pixel compositing while the fabric sits idle apart from scanout.
The blitter inverts that: the A9 emits a per-frame **display list** of blit
commands into DDR, and the fabric executes them and composites the framebuffer.

## Data flow — from source pixels to screen

The blitter is a **drop-in producer**: it replaces the engine's software
compositing + framebuffer write, and emits the *same* video control word the
existing MiSTer scanout reader already consumes. The video reader is **unchanged**.

```
  ┌──────────────────────────────────────────────────────────────────────┐
  │  Cortex-A9  (software engine: Solarus / gmloader / OpenBOR)           │
  │  builds a per-frame display list instead of compositing pixels        │
  └───────────────────────────────┬──────────────────────────────────────┘
                                   │  emit blit commands + source atlases
                                   ▼
  ┌──────────────────────────────────────────────────────────────────────┐
  │  DDR3  ── 0x3B00_0000 region   (new blitter structures)               │
  │   ┌─────────────┐   ┌───────────────┐   ┌─────────────────────┐        │
  │   │ ctrl block  │   │ command ring  │   │ source heap / atlas │        │
  │   │ 0x3B00_0000 │   │ 0x3B00_0040   │   │ 0x3B04_0040         │        │
  │   │ submit_seq /│   │ 8192 × 32 B,  │   │ RGB565 source px     │       │
  │   │ done_seq    │   │ walk to END   │   │                     │        │
  │   └──────▲──────┘   └───────┬───────┘   └──────────┬──────────┘        │
  └──────────┼──────────────────┼─────────────────────┼────────────────────┘
       done_seq │ (A9 polls)    │ cmds                │ source px
                │               ▼                     ▼
  ┌─────────────┼──────────────────────────────────────────────────────────┐
  │  BLITTER core  (FPGA fabric — Cyclone V)                                │
  │                                                                          │
  │   RingReader ──▶ SourceFetch+decode ──▶ RectBlitter ──▶ ColorMixer      │
  │   (walk list)    (burst read + fmt)     (clip/flip/      (colorkey /     │
  │                                          raster)          const-alpha)   │
  │                               │                                          │
  │                               ▼  composite into on-chip line/tile        │
  │                        ┌───────────────┐  buffer (never per-pixel        │
  │                        │  on-chip FB   │  DDR writes)                     │
  │                        └───────┬───────┘                                  │
  │                                │  burst-DMA (64-bit sequential bursts)    │
  └────────────────────────────────┼──────────── shared f2h DDR + arbiter ───┘
                                   ▼
  ┌──────────────────────────────────────────────────────────────────────┐
  │  DDR3  ── 0x3A00_0000 region   (existing framebuffer — UNCHANGED map)  │
  │   ┌──────────────┐  ┌──────────────┐   page-flip on vBlank             │
  │   │ BUF 0        │  │ BUF 1        │   (composite the inactive buffer)  │
  │   │ 0x3A00_0040  │  │ 0x3A04_0040  │                                    │
  │   └──────┬───────┘  └──────┬───────┘                                    │
  │          └───────┬─────────┘                                            │
  │                  ▼                                                       │
  │      video control word @ 0x3A00_0000  (frame_counter | buf)            │
  └────────────────────────────────┬───────────────────────────────────────┘
                                   ▼
  ┌──────────────────────────────────────────────────────────────────────┐
  │  openbor_video_reader  (UNCHANGED MiSTer scanout)  ──▶  HDMI / screen  │
  └──────────────────────────────────────────────────────────────────────┘
```

Two architectural decisions define the core:

1. **Drop-in producer on the existing double-buffer.** The blitter shares the
   single f2h DDR port via a small arbiter (video scanout keeps priority) and
   composites the *inactive* buffer during the inter-frame window, then page-flips
   on vBlank. The proven control-word handshake and scanout reader are untouched.
2. **Composite on-chip, burst-DMA to DDR.** The blitter never writes DDR per
   pixel; it composites into an on-chip line/tile buffer and bursts it out as
   long 64-bit sequential transfers. DDR never sees random per-texel traffic.

## Prior art & acknowledgements

The architecture is modeled directly on **CAVE's CV1000 arcade hardware** and its
faithful FPGA recreation, the **MiSTer Cave core**. CV1000 is CAVE's last arcade
platform — a CPU that issues *commands* to an Altera Cyclone FPGA acting as the
graphics "Blitter," which composites sprites into a DDR framebuffer. That is
structurally identical to our target (A9 → Cyclone V → DDR), which makes it the
gold-standard blueprint for this project.

- **Arcade-Cave_MiSTer** — `github.com/MiSTer-devel/Arcade-Cave_MiSTer`,
  originally authored by **Josh Bassett (nullobject)**, with the MiSTer-devel
  community. GPL-licensed; originally written in Chisel (Scala → Verilog), later
  also a SystemVerilog rewrite under `rtl/cave/`.

Our module split deliberately mirrors that core's pipeline —
`SpriteProcessor → SpriteDecoder → SpriteBlitter → SpriteFrameBuffer → ColorMixer`
maps onto our `RingReader → SourceFetch → RectBlitter → on-chip FrameBuffer → ColorMixer`.
The decisive lesson — *blit to an on-chip buffer with backpressure and burst-DMA
to DDR* — comes straight from how `SpriteBlitter`/`SpriteFrameBuffer` decouple
compositing from DDR traffic.

> **Note on reuse:** this is an *independent, clean-room* implementation informed
> by the public design and module boundaries of the CAVE / CV1000 core. **No RTL
> is copied** from Arcade-Cave_MiSTer. It serves as prior-art guidance only. Both
> projects are GPL-3.0, consistent with the MiSTer ecosystem. Full survey:
> `docs/blitter-feasibility.md` and `research-docs/research-mister-blitters.md`.

## How to build / run the reference model

The software reference model is the executable spec — it builds and runs with no
hardware and no dependencies:

```sh
cd refmodel && make test     # builds + runs unit tests (28/28 pass)
```

`refmodel/blitter_ref.h` is the machine-readable copy of the command contract in
`docs/blitter-protocol.md`. `refmodel/blitter_ref.c` defines the exact per-pixel
semantics (FILL / COPY / COLORKEY / CONST_ALPHA / flips / clipping /
walk-until-END) that the RTL must reproduce bit-for-bit. It is both the **golden
output** the RTL is diffed against and the spec host command emitters develop
against.

## Layout

```
docs/          design docs: feasibility (go/no-go) + protocol spec (the contract)
research-docs/  prior-art survey of existing MiSTer 2D-acceleration cores
refmodel/      C reference model — golden output for the RTL, exec spec for host
rtl/           SystemVerilog blitter core
sim/           testbench + DDR model: RTL ↔ reference-model equivalence vectors
host/          host-side command emitter (display-list builder for engine backend)
```

## Status

| Stage | State |
|-------|-------|
| Feasibility / architecture (go/no-go) | ✅ **GO** — `docs/blitter-feasibility.md` |
| Command protocol + DDR ring + handshake | ✅ spec — `docs/blitter-protocol.md` |
| Software reference model + tests | ✅ `refmodel/` — `make test` = 28/28 pass |
| Blitter RTL ↔ model equivalence in sim | ✅ `rtl/` + `sim/` — `make test` = 11/11 pass |
| RTL spike on hardware (DDR-frame-counter / screenshot proof) | ⏳ needs MiSTer online |
| Perf architecture: on-chip buffer + burst-DMA (line/tile) | ⏳ #004/#005 |
| Host command emitter + engine backend | ⏳ `host/` + engine repos |

## License

GPL-3.0 — consistent with the MiSTer core ecosystem (including the CAVE / CV1000
prior art) and the engine ports this serves. See `LICENSE`.
</content>
</invoke>
