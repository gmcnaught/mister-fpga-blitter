# rtl/ — SystemVerilog blitter (v1 reference spike)

## `blitter_top.sv` — the v1 single-transaction blitter

A single-module functional blitter: walks the DDR command ring (until `END` /
`cmd_count`), decodes 32-byte commands, composites into the framebuffer
(FILL / COPY / COLORKEY / CONST_ALPHA / per-pixel alpha (PALPHA, ARGB4444 src) /
H+V flip / clip+cull), and writes the video control word as a drop-in producer.
It also hosts the MFGPU `TRILIST` (opcode 12) engine: vertex fetch/decode
states driving `blt_tri.sv`, a combinational per-pixel evaluator for textured
triangle lists (integer 12.4 edge functions with the top-left fill rule,
nearest-texel sampling, per-vertex colour/alpha interpolation) — bit-exact to
`../refmodel/blt_tri.c` by construction, a simulation-equivalence model rather
than a synthesis-optimized datapath.
**Diffed bit-exact against the C reference model** over 17 scenarios (incl.
6 `tri_*` cases) — `cd ../sim && make test`. The blend path uses a divide-free
/255 reduction split across pipeline stages, and source/dest addressing is
incremental (registered) for timing.

`blitter_defs.vh` — shared sim memory layout (kept in sync with
`../sim/gen_vectors.c`). The command packing (`qw[k]={u32[2k+1],u32[2k]}`) is
frozen and matches `../docs/blitter-protocol.md`.

**Role today:** this spike is kept as the smallest complete, self-simulable
implementation of the command contract — the fastest way to understand the
protocol end-to-end, and the regression gate that keeps the reference model
honest (`../sim` must stay green). It was HW-validated in its day, but it is
**not** what ships.

## Where the production RTL lives

The shipping fabric is developed in the `solarus-mister` integration repo
under `fpga/rtl/` (~5 800 lines), where it builds into a full MiSTer core and
is validated on hardware. What replaced this spike, in order (details in
`../docs/lessons-learned.md`):

- `comp_pipeline.sv` — pipelined compositor: band-chunked RMW, **one pixel per
  clock**, all blend modes + RGB888 tint native (zero software escapes),
  source spans through a double-buffered line buffer that overlaps span N+1's
  fetch with span N's composite.
- `comp_fbram.sv` + `fbram_snapshot.sv` — the framebuffer moved **on-chip**
  (M10K): persistent WORK image, hardware WORK→SCAN copy at vblank (tear-free
  double-buffer). This *superseded* the "composite on-chip, burst-DMA out"
  plan sketched here in v1 — with the target resident in BRAM there is nothing
  to DMA out, and the destination preload/write-back (44–66 % of compositor
  cycles) is simply gone.
- `sdram_fb_cache.sv` (jtframe cache mux + burst controller) — source atlases
  resident in SDRAM on a dedicated second bus, staged once at load by `STAGE`
  commands.
- `blitter_top.sv` (same name, evolved) — ring walk, decode, `TILELIST` /
  `TILELIST_RES` expansion from the tile-list buffer, staging FSM, snapshot
  control. Since then it has grown `SPRITELIST` (ordered 24-byte-entry sprite
  batches from `SP_BUF`), the `TILEMAP` grid-walk FSM (per-layer 8px cell
  grids from `GRID_BUF`, run-coalesced, timing-closed via split
  `S_GRID_SETUP`/`S_GRID_BOUNDS` stages), `CLUT_UPLOAD` + on-chip CLUT for
  8bpp `PAL8` sources (halves atlas size), and an immediate (non-vblank-gated)
  work→scan snapshot with fabric-owned `fb_bank` alternation for the DDR3
  double-buffer.
- `fbram_scan_adapter.sv` — scanout served from BRAM with same-cycle reads;
  the display deadline never touches a bus.

Every stage of that evolution stayed gated on bit-exactness against
`../refmodel/` — the same golden model this spike is diffed against.
