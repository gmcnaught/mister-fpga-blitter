# mister-fpga-blitter

A command-driven **2D hardware blitter** for the MiSTer (DE10-Nano, Cyclone V),
to offload software 2D compositing from the Cortex-A9 onto the FPGA fabric.

This is the engine-agnostic core of the *FPGA graphics-offload* effort: software
engine ports (Solarus first, then gmloader / OpenBOR) are bottlenecked on the A9
doing all per-pixel compositing while the fabric sits idle apart from scanout.
The blitter lets the A9 emit a per-frame **display list** of blit commands into
DDR; the fabric executes them and composites the framebuffer.

## Status

| Stage | State |
|-------|-------|
| Feasibility / architecture (go/no-go) | ✅ **GO** — `docs/blitter-feasibility.md` |
| Command protocol + DDR ring + handshake | ✅ spec — `docs/blitter-protocol.md` |
| Software reference model + tests | ✅ `refmodel/` — `make test` = 28/28 pass |
| Minimal blitter RTL spike (one rect-copy, HW proof) | ⏳ next (`rtl/`) |
| Compositing feature set (alpha/colorkey/rects) | ⏳ |
| Host command emitter + engine backend | ⏳ (lives in the engine repos) |

## Architecture (fixed)

**Drop-in producer.** The blitter replaces an engine's software compositing +
framebuffer write and writes the *same* video control word the scanout reader
already consumes — so the existing MiSTer video reader is **unchanged**. It
shares the single f2h DDR port via an arbiter (scanout keeps priority) and
composites the inactive buffer of the existing double-buffer.

**On-chip buffer + burst-DMA to DDR** — the blitter never writes DDR per pixel;
it composites into an on-chip line/tile buffer and bursts to DDR. RTL pipeline:
`RingReader → SourceFetch+decode → RectBlitter → on-chip FrameBuffer (DMA)`.

Both decisions come from the prior-art survey of the MiSTer Cave / CV1000 core
(SH-3 → FPGA blitter → DDR framebuffer), the closest existing analog.

## Layout

```
docs/      design docs: feasibility (go/no-go) + protocol spec (the contract)
refmodel/  C software reference model — the GOLDEN output the RTL is diffed
           against, and the executable spec host emitters develop against
rtl/       SystemVerilog blitter (lands with the #003 RTL spike)
```

## Reference model

```sh
cd refmodel && make test     # builds + runs unit tests, no hardware/deps
```

`refmodel/blitter_ref.h` is the machine-readable copy of the command contract in
`docs/blitter-protocol.md`. `refmodel/blitter_ref.c` defines exact per-pixel
semantics (FILL / COPY / COLORKEY / CONST_ALPHA / flips / clipping /
walk-until-END) the RTL must reproduce bit-for-bit.

## License

GPL-3.0 — consistent with the MiSTer core ecosystem and the engine ports this
serves. See `LICENSE`.
