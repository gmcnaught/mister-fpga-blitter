# rtl/ — SystemVerilog blitter

## Current: `blitter_top.sv` — HW-validated single-transaction blitter ✅

A single-module functional blitter: walks the DDR command ring (until `END` /
`cmd_count`), decodes 32-byte commands, composites into the framebuffer
(FILL / COPY / COLORKEY / CONST_ALPHA / per-pixel alpha (PALPHA, ARGB4444 src) /
H+V flip / clip+cull), and writes the video control word as a drop-in producer.
**Diffed bit-exact against the C reference model** over 11 scenarios — `cd ../sim
&& make test` — and **validated on real hardware** driving the Solarus engine on
MiSTer: correct video, full A9 offload (`escape=0`), ~100 fps on heavy scenes.
The blend path uses a divide-free /255 reduction split across pipeline stages,
and source/dest addressing is incremental (registered) for timing.

`blitter_defs.vh` — shared sim memory layout (kept in sync with
`../sim/gen_vectors.c`). The command packing (`qw[k]={u32[2k+1],u32[2k]}`) is
frozen and matches `../docs/blitter-protocol.md`. The integration repo
(`solarus-mister`) overlays the real MiSTer DDR addresses (0x3A00_0000 frame-
buffers + 0x3B00_0000 4 MiB command/heap region) on the same logic.

Scope: a single Avalon-MM master with single-transaction reads/writes through a
shared-f2h arbiter (scanout keeps priority) — **functional and HW-proven, but not
bandwidth-optimal**. The memory *performance* architecture (below) is the next
lever.

## Next

- **Perf — on-chip buffer + burst-DMA (the CV1000 pattern, see `../docs/`):**
  composite into an on-chip line/tile buffer and burst it to DDR, so DDR never
  sees per-texel traffic. Implemented and sim-validated on the `burst-dma` branch
  of the `solarus-mister` integration repo, but **timing not yet met** at the
  ~100 MHz f2h clock (best worst-case setup slack −0.385 ns, from −4.979 — the
  limiter is the on-chip line-buffer read mux). Parked pending a profile of
  whether on-chip composite or DDR bandwidth is the real per-row bottleneck.
  Candidate module split when revived:
  - `ring_reader.sv` — ring walk + command decode
  - `source_fetch.sv` — f2h burst read master + format decode
  - `rect_blitter.sv` — clip + flip + colorkey/const-alpha raster
  - `frame_buffer.sv` — on-chip line/tile buffer + burst-DMA write master
  - `blitter_top.sv` — top-level + shared-f2h arbiter (scanout priority)

Every refactor stays gated on `../sim` staying green against `../refmodel/`.
