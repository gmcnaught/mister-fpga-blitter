# Blit command protocol + DDR ring + handshake

**Status:** shipped + hardware-validated (DE10-Nano, Solarus 1.6.5 port).
**Companion:** the C header `../refmodel/blitter_ref.h` is the machine-readable
copy of this contract; the model in `blitter_ref.c` is the golden output the RTL
is diffed against. The production fabric implementation lives in the
`solarus-mister` integration repo (`fpga/rtl/blitter_top.sv` +
`comp_pipeline.sv`); `blitter_defs.vh` there mirrors every constant below.

This document describes the **current shipped contract (v2)**. Where the
original v1 contract differed, the change is called out inline — the deltas are
themselves hardware lessons.

---

## 1. Model & integration stance

The A9 never composites. The engine builds a per-frame **display list** (mostly
one `TILELIST` command per map layer plus a few dozen sprite blits), the fabric
executes it, and scanout reads the result:

```
 ARM (host)                          Fabric                        Scanout
 ----------                          ------                        -------
 build display list  ── ring ──▶   walk list (until END)
 (quest load: STAGE all atlases     fetch sources from SDRAM-
  DDR3 → SDRAM, once)               resident atlases, composite
 bump submit_seq (doorbell) ────▶   into the ON-CHIP (BRAM) FB
                                    write video ctrl word
 poll done_seq ◀────────────────    bump done_seq
                                    at vblank: snapshot WORK→SCAN ──▶ reader
```

Three memories, three jobs:

- **DDR3 (shared f2h bus)** carries only *control* traffic: the command ring,
  the control block, tile-list entries, and the one-time texture upload staging.
  **No framebuffer pixels cross the f2h bus.**
- **SDRAM (dedicated second bus)** holds the source atlases, staged once at
  load (`STAGE` commands) and resident for the session.
- **On-chip BRAM (M10K)** holds the 320×240 RGB565 framebuffer: a persistent
  WORK image the compositor RMWs, and a SCAN snapshot copied from WORK at
  vblank (tear-free double-buffer, no engine-side page flip needed).

> **v1 → v2:** v1 composited into the two DDR3 framebuffers (BUF0/BUF1 at
> `0x3A000040`/`0x3A040040`) as a drop-in producer for the existing scanout
> reader, page-flipping via the video control word. That worked, but the
> destination read-modify-write + write-back was 44–66 % of compositor cycles
> and every pixel crossed the contended f2h bus. Moving the framebuffer
> on-chip (and sources to SDRAM) removed both. The DDR3 buffers and control
> word survive as legacy interface; the pixels no longer live there.

Pixel model: **320×240 RGB565** destination. Sources are RGB565 or ARGB4444
(per-pixel alpha). Transparency is colorkey, constant alpha, or per-pixel
alpha; ADD / MULTIPLY blends and an RGB888 source tint (color-mod) are native,
so **no draw ever escapes back to software**.

## 2. DDR memory map (shipped v4 layout)

All blitter structures live in a dedicated 16 MiB region at `0x3B000000`
(HW-verified reserved-safe against Linux + engine + video/audio activity).
The `0x3A000000` region keeps the existing video/joystick/audio contract.

| Phys base    | Size      | Purpose                                              |
|--------------|-----------|------------------------------------------------------|
| `0x3A000000` | 0x40      | (existing) video ctrl word + joy/cart/audio           |
| `0x3A000040` | 2×150 KiB | (legacy) DDR3 framebuffers BUF0/BUF1 — pixels now in BRAM |
| `0x3A070000` | 4 B       | `vsync_count` written by scanout — frame pacing       |
| `0x3B000000` | 0x40      | blitter control block (§3)                            |
| `0x3B000040` | **512 KiB** | command ring: ~16 382 × 32 B, walk-until-END        |
| `0x3B080000` | ~15.2 MiB | texture upload heap (staging source for `STAGE`)      |
| `0x3BF40000` | 512 KiB   | `TL_BUF` — tile-list entry buffer (§5)                |
| `0x3BFC0000` | 8 KiB     | `FRT` — frame-rect table (`TILELIST_RES`, §5)         |
| `0x3BFC2000` | 256 B     | `CFT` — current-frame table (`TILELIST_RES`, §5)      |

**Ring-size lesson (v1 → v2):** the ring shipped at 32 KiB (1022 commands) and
was believed generous at ~100 cmds/frame. Dense 8×8-tile maps emit **>1250
on-screen commands per frame** — the ring overflowed, latched a scene-too-big
error, and the screen went black. The ring is now 512 KiB, and the same maps
were later collapsed to ~1–3 `TILELIST` commands anyway (§5). Size command
rings for the pathological scene, not the average one — and note the ring cap
and the fabric's heap base are **coupled constants** that must deploy together.

SDRAM (second bus, 128 MB module required) is not directly host-addressable:
the host allocates SDRAM offsets and the **fabric** copies DDR3→SDRAM when it
walks a `STAGE` command. A permanent (grow-only, never freed) region holds the
whole quest's atlases, staged once at load.

## 3. Control block (`0x3B000000`, one u32 per qword slot)

| Qword | Name         | Writer | Meaning                                           |
|-------|--------------|--------|---------------------------------------------------|
| 0     | `submit_seq` | ARM    | doorbell: incremented after the list is in DDR    |
| 1     | `cmd_count`  | ARM    | number of valid commands in the ring this frame   |
| 2     | `target_buf` | ARM    | legacy target select (single-buffer mode: constant)|
| 3     | `clear_color`| ARM    | RGB565; if `flags.CLEAR`, fill target first       |
| 4     | `flags`      | ARM    | bit0 CLEAR-before-list; rest reserved             |
| 5     | `done_seq`   | fabric | set = `submit_seq` when the frame is composited   |
| 6     | `status`     | fabric | 0 = OK; nonzero = error latch (e.g. ring overflow)|
| 7     | `srcsel/pipe`| ARM    | bit0 SDRAM-source master enable; bit1 pipelined-compositor select; bits[15:8] throttle |

**Offset-7 aliasing lesson:** the ring begins at qword 8 of the region, so
control qword 8 **aliases the first ring command**. A new control field added
there read cmd0's opcode bit and spuriously enabled itself. New control bits
now pack into spare bits of qword 7. When a control block abuts a ring, the
boundary is part of the ABI.

**Frame handshake** (unchanged from v1 — it survived every architecture
revision): host writes ring + control fields, then bumps `submit_seq` last
(store-release ordering); fabric composites when `submit_seq != done_seq`,
writes the video control word, then sets `done_seq = submit_seq`. Single frame
in flight; the host builds frame N+1's list while the fabric composites N.
Diagnostic signature worth knowing: `status != 0` = ring overflow / error
latch, while `done_seq == submit_seq` with a *falling* command count usually
means the engine outlived a core reload (stale resident atlases — restart the
engine, not the fabric).

## 4. Command word — 32 bytes / 8×u32

Mirrors `blt_cmd_t` in `blitter_ref.h`; canonical pack/unpack in
`../host/blt_wire.h`. All source offsets/strides are **bytes**; `dst_x/dst_y`
are **signed** (offscreen rects are culled with zero memory traffic).

| u32 | bits 31..24 | 23..16      | 15..8        | 7..0       |
|-----|-------------|-------------|--------------|------------|
| 0   | flags       | format      | blend_mode   | opcode     |
| 1   | src_off (byte offset into source heap) [31:0]            |||
| 2   | src_x [31:16]              | src_stride (bytes) [15:0]   ||
| 3   | h [31:16]                 | w [15:0] *(blit size, px)*  ||
| 4   | (reserved) [31:16]        | src_y [15:0]                ||
| 5   | dst_y (s16) [31:16]       | dst_x (s16) [15:0]          ||
| 6   | **cmod_b [31:24]** | alpha [23:16] | colorkey (RGB565) [15:0]|||
| 7   | **cmod_g [31:24]** | **cmod_r [23:16]** | color (RGB565 fill) [15:0]||

Qwords pack as `qw[k] = {u32[2k+1], u32[2k]}` (little-endian). The layout is
frozen; the three bytes that were reserved in v1 (27, 30, 31) now carry the
RGB888 **color-mod tint** when `flags.COLORMOD` is set — an ABI-compatible
extension (zero-filled commands behave exactly as v1).

**opcode:**
`0 NOP · 1 END · 2 FILL · 3 BLIT · 4 STAGE · 5 TILELIST · 6 TILELIST_RES · 7 FRT_UPLOAD`

**blend_mode:**
`0 COPY · 1 COLORKEY · 2 CONST_ALPHA · 3 PALPHA (ARGB4444 src, per-pixel alpha) · 4 ADD (saturating) · 5 MULTIPLY`
ADD/MULTIPLY also apply to `FILL` (src channel = `color` channel).

**format:** `0 RGB565 · 1 ARGB4444` (`{A4,R4,G4,B4}`, A in [15:12]; required
for PALPHA — A4==0 pixels are skip-write).

**flags:**
`0x01 HFLIP · 0x02 VFLIP · 0x04 COLORKEY (also key a CONST_ALPHA blit) ·
0x08 STAGE_DST (STAGE: u32[2] carries the SDRAM dest offset) ·
0x10 SRC_SDRAM (per-command source mux: read this source from SDRAM) ·
0x20 SRC_FB (source is a compositor-written FB; fires the coherency barrier) ·
0x40 COLORMOD (tint bytes valid)`

**Per-command source mux lesson:** v1's follow-on made "read sources from
SDRAM" a single global control bit; real frames mix staged and unstaged
sources during transitions, which corrupted every unstaged blit. The global
bit (`ctrl qword 7 bit0`) is now only a master *enable*; each command carries
its own `SRC_SDRAM` flag.

### Per-pixel semantics

Exactly as the reference model executes — `blitter_ref.c` is normative,
including the **divide-free reductions** the RTL matches bit-for-bit:

- blend: `div(t) = (t + 128 + ((t+128)>>8)) >> 8`, `t = src_c*a + dst_c*(255-a)`
- PALPHA: expand A4→A8 (`{a4,a4}`), widen R4/G4/B4 to dest channel widths,
  then the same /255 reduction per pixel (`blt_blend4444`, kept `static inline`
  in the header so host and tests share one body).
- ADD: `out_ch = min(src_ch + dst_ch, chan_max)` at RGB565 widths.
- MULTIPLY: `out_ch = round(src_ch * dst_ch / chan_max)` — `blt_mul565` owns
  the exact reduction.
- COLORMOD: `src_ch' = round(src_ch * mod_ch / 255)` applied **before** the
  blend, so it composes with every blend mode. `(255,255,255)` is an exact
  identity; the host only sets the flag for a non-identity tint.
- Walk-until-END, painter's order, clip to 320×240, fully-offscreen = no
  memory traffic: unchanged from v1.

## 5. Batch opcodes — STAGE and the tile lists

These opcodes are why the A9 emit cost stopped being the bottleneck.

### `STAGE` (4) — DDR3 → SDRAM atlas staging
`src_off` = byte offset in the DDR3 upload heap; `w | h<<16` = byte size
(32-bit size round-trips through the existing u32[3] slot). With
`flags.STAGE_DST`, u32[2] carries an SDRAM destination offset decoupled from
the DDR3 offset — this is what lets a whole quest's atlases (tens of MiB,
larger than the DDR3 heap) be staged into a permanent SDRAM region through a
small DDR3 bounce buffer. A `stage_barrier` in the fabric flushes the staging
write channel and invalidates the source-read cache so a blit issued right
after a STAGE reads coherent data.

### `TILELIST` (5) — direct tile batch
One command draws N tiles that share a texture + blend. The 32-byte header
carries the shared params; the N 12-byte entries
(`{src_x, src_y, w, h, dst_x, dst_y}`, `blt_tile_entry_t`) live in the
dedicated `TL_BUF` region, written once when a map loads. Header field reuse:
`w | h<<16` = entry count N; `dst_x | dst_y<<16` = entry-array byte offset;
`src_x/src_y` = a signed per-batch **bias** added by the fabric to every
entry's dst.

**Camera-independence lesson:** entries store **map coordinates**, not screen
coordinates. Only the header's bias (map→screen) changes when the camera
moves, so a whole layer's list is recorded once per map and replayed for the
map's lifetime — camera motion costs zero entry rewrites.

### `TILELIST_RES` (6) + `FRT_UPLOAD` (7) — pattern-indexed (animated) batch
Same header packing, but each entry is 8 bytes
(`{pattern_id, dst_x, dst_y, _rsvd}`, one aligned qword). The fabric resolves
each tile's source rect as `FRT[pattern_id][CFT[pattern_id]]`:

- **FRT** (frame-rect table): per-pattern, per-frame source rects
  (`BLT_MAXP=128` patterns × `BLT_MAXF=8` frames, 8 B each), uploaded once per
  scene via `FRT_UPLOAD` (header `w | h<<16` = qword count) into fabric BRAM.
- **CFT** (current-frame table): one u16 per pattern; the A9 writes the
  animation frame index each frame — that write is the *entire* per-frame
  animated-tile cost.

Combined effect on hardware: a dense map that emitted ~3 758 per-tile commands
per frame collapsed to ~1–3 header commands per layer.

## 6. Why this format / source-pixel decisions (unchanged rationale)

- **Compact (32 B)** ring entries; with tile lists, ring traffic is trivial.
- **RGB565 sources** match the framebuffer; fixed-function fabric reads them
  with no conversion penalty and half the bandwidth of 32bpp. ARGB4444 was
  added for per-pixel alpha at the same 16bpp cost — enough alpha resolution
  for the sprite/fade work these engines do.
- **Colorkey skip-write is the fast path** — blend, not fetch, was the
  original cost; after the pipelined compositor both are one pixel per clock.

## 7. Verification

Everything host-side builds and runs with no hardware and no deps:

- `../refmodel/ make test` — 28 contract checks + the embedded self-test
  (`-DBLT_REF_SELFTEST`: exhaustive divide-free-reduction proofs, COLORMOD /
  ADD / MULTIPLY goldens, TILELIST ≡ N-blits equivalence).
- `../host/ make test` — 22 emitter/codec checks + the embedded self-test
  (STAGE, SDRAM staging + permanent-region allocators, tile-list emit paths).
- `../sim/ make test` — the v1 RTL spike diffed qword-for-qword against the
  reference model (11 scenarios). The production fabric in `solarus-mister`
  carries its own gating testbenches (`tb_blitter_*`) that hold each pipeline
  stage bit-exact to the same golden model.

The bit-exact-golden discipline is the single most-repaid decision in the
project: every fabric rewrite (per-pixel FSM → pipelined compositor → BRAM
framebuffer) shipped against an unchanged software truth, so hardware debug
was only ever about *transport* (buses, timing, caches), never about pixel
math.
