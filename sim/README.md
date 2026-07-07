# sim/ — RTL ↔ reference-model equivalence

Proves `rtl/blitter_top.sv` (the v1 spike) produces **bit-identical**
framebuffers to the C reference model (`refmodel/`) over the v1 command set,
with no hardware.

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
(`qw[k]={u32[2k+1],u32[2k]}`) via the same `host/blt_wire.h` the emitter uses.
The scenarios mirror the reference-model unit tests so RTL and model coverage
line up.

## Scenarios (11, all passing)

`fill · copy · colorkey · alpha · hflip · vflip · clip_neg · clip_off ·
overdraw · clear · target1`

The reference model itself has grown past this set (PALPHA, ADD, MULTIPLY,
COLORMOD tint, STAGE, tile lists — see `docs/blitter-protocol.md`); it stays
backward-exact on the v1 semantics, which is what this suite pins. The v2
semantics are held bit-exact by the model's embedded self-test
(`refmodel/ make test`) and by the production fabric's own gating testbenches
(`tb_blitter_*` in the `solarus-mister` repo), which diff each pipeline stage
against the same golden functions.

## What this does / doesn't prove

- **Does:** command-ring walk + decode, clipping/cull, colorkey skip-write,
  const-alpha blend (bit-exact /255), flips, painter order, hardware CLEAR, the
  submit/done handshake, drop-in-producer control-word write, buffer selection.
- **Doesn't:** real bus timing/arbitration, Quartus fit + timing closure, the
  production architecture (pipelined compositor, BRAM framebuffer, SDRAM
  sources — see `rtl/README.md`). The behavioral `ddr_model.sv` is single-beat,
  never busy — a *functional* model, not a timing model. Timing-class bugs
  (multi-cycle ready double-accept, IOB capture placement) are exactly the ones
  this style of sim cannot catch — `docs/lessons-learned.md` covers the ones
  hardware taught us.

## Component notes

- `ddr_model.sv` — behavioral qword memory with byte-enables; registered 1-cycle
  read. Swappable for a timing-accurate / arbitrated model later.
- `blitter_top` uses simple per-pixel DDR access — deliberately functional.
  The production compositor keeps these exact per-pixel semantics behind a
  completely different transport.
