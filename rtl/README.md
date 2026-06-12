# rtl/ — SystemVerilog blitter

## Current: `blitter_top.sv` — functional spike (#003), simulation-verified ✅

A single-module functional blitter: walks the DDR command ring (until `END` /
`cmd_count`), decodes 32-byte commands, composites into the framebuffer
(FILL / COPY / COLORKEY / CONST_ALPHA / H+V flip / clip+cull), and writes the
video control word as a drop-in producer. **Diffed bit-exact against the C
reference model** over 11 scenarios — `cd ../sim && make test`.

`blitter_defs.vh` — shared sim memory layout (kept in sync with
`../sim/gen_vectors.c`). The command packing (`qw[k]={u32[2k+1],u32[2k]}`) is
frozen and matches `../docs/blitter-protocol.md`.

Scope of the spike (deliberate): a single Avalon-MM master with simple per-pixel
reads/writes — **functional, not yet bandwidth-optimal**. It proves the
command/handshake/pixel logic; the memory *performance* architecture is next.

## Next

- **#003 remainder (needs hardware):** integrate beside the video reader in a
  fork of the branded Solarus core, add the shared-f2h arbiter, build via CI,
  prove on HW (DDR frame counter advances; mrext screenshot shows the rect).
- **#004/#005 (perf):** refactor into the burst-oriented module split below —
  on-chip line/tile buffer + burst-DMA (the CV1000 pattern, see `../docs/`),
  keeping the now-verified semantics:
  - `ring_reader.sv` — ring walk + command decode
  - `source_fetch.sv` — f2h burst read master + format decode
  - `rect_blitter.sv` — clip + flip + colorkey/const-alpha raster
  - `frame_buffer.sv` — on-chip line/tile buffer + burst-DMA write master
  - `blitter_top.sv` — top-level + shared-f2h arbiter (scanout priority)

Every refactor stays gated on `../sim` staying green against `../refmodel/`.
