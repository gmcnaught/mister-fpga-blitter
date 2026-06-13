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
2. **Composite on-chip, burst-DMA to DDR** *(target architecture)*. The blitter
   composites into an on-chip line/tile buffer and bursts it out as long 64-bit
   sequential transfers, so DDR never sees random per-texel traffic. The
   **shipping/HW-validated** core (below) is the simpler single-transaction
   variant — it composites through the shared arbiter directly; the on-chip
   line-buffer + burst-DMA build is implemented and sim-validated on the
   `burst-dma` branch of the `solarus-mister` integration repo, but does **not
   yet meet timing** at the ~100 MHz f2h clock (best worst-case setup slack
   −0.385 ns, from −4.979), and is parked pending a profile of whether on-chip
   composite or DDR bandwidth is the real bottleneck.

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
semantics (FILL / COPY / COLORKEY / CONST_ALPHA / per-pixel alpha (PALPHA,
ARGB4444 source) / flips / clipping / walk-until-END) that the RTL must reproduce
bit-for-bit. It is both the **golden output** the RTL is diffed against and the
spec host command emitters develop against.

## Hardware validation (Solarus on MiSTer)

The blitter is **validated end-to-end on real hardware** (DE10-Nano) driving the
Solarus 1.6.5 engine port. The A9 emits the per-frame display list; the fabric
composites and the unchanged scanout displays it. Live captures show correct
video (title + animated scenes) with the engine diagnostics reporting full
offload — `escape=0`, every frame composited on the fabric — at **~100 fps** on
heavy scenes (vs ~30 fps for the A9 software renderer), using the 4 MiB source
heap so full scene transitions stay resident. This is the single-transaction
core (decision 2 above); the engine backend lives in the `solarus-mister` repo
and vendors `host/` + `refmodel/blitter_ref.h` from here verbatim.

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
| Host command emitter + wire codec | ✅ `host/` — `make test` = 22/22 pass |
| **On hardware (correct video + offload, Solarus/MiSTer)** | ✅ **validated** — ~100 fps heavy-scene offload, `escape=0`, 4 MiB heap |
| Host command emitter + engine backend | ✅ Solarus backend (COPY/COLORKEY/CONST_ALPHA/PALPHA/flips), HW-verified |
| Perf architecture: on-chip buffer + burst-DMA (line/tile) | ⏳ `solarus-mister:burst-dma` — sim-validated, timing not yet met (−0.385 ns); parked |

## License

GPL-3.0 — consistent with the MiSTer core ecosystem (including the CAVE / CV1000
prior art) and the engine ports this serves. See `LICENSE`.
</content>
</invoke>
