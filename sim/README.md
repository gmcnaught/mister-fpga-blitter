# sim/ — RTL ↔ reference-model equivalence

Proves `rtl/blitter_top.sv` produces **bit-identical** framebuffers to the C
reference model (`refmodel/`) over the v1 command set, with no hardware.

```sh
make test     # build + run all scenarios (needs iverilog + cc)
make clean
```

## Flow

```
 gen_vectors.c ──┬─▶ runs refmodel/blitter_ref.c ─▶ fb_expected.hex  (golden)
   (per scenario)└─▶ lays out DDR image          ─▶ ddr_init.hex
                          │
 tb_blitter.sv  ◀─────────┘  $readmemh both, drive blitter to frame-done,
   + ddr_model.sv             diff target framebuffer vs golden  ─▶ RESULT PASS/FAIL
```

`gen_vectors.c` and `rtl/blitter_defs.vh` share one memory layout (qword bases);
`gen_vectors.c` and `rtl/blitter_top.sv` share one command packing
(`qw[k]={u32[2k+1],u32[2k]}`). The scenarios mirror the reference-model unit
tests so RTL and model coverage line up.

## Scenarios (11, all passing)

`fill · copy · colorkey · alpha · hflip · vflip · clip_neg · clip_off ·
overdraw · clear · target1`

## What this does / doesn't prove

- **Does:** command-ring walk + decode, clipping/cull, colorkey skip-write,
  const-alpha blend (bit-exact /255), flips, painter order, hardware CLEAR, the
  submit/done handshake, drop-in-producer control-word write, buffer selection.
- **Doesn't (next, needs hardware):** real f2h Avalon timing/arbitration with the
  scanout reader, Quartus fit + timing closure, and the on-chip-buffer/burst-DMA
  performance refinement. The behavioral `ddr_model.sv` is single-beat, never
  busy — a *functional* model, not a timing model.

## Component notes

- `ddr_model.sv` — behavioral qword memory with byte-enables; registered 1-cycle
  read. Swappable for a timing-accurate / arbitrated model later.
- `blitter_top` currently uses simple per-pixel DDR access — deliberately
  functional. The perf architecture (line buffer + burst) is #004/#005 and keeps
  these exact semantics.
