# Blit command protocol + DDR ring + handshake

**Epic:** fpga-hw-blitter · **Task:** #002
**Status:** spec complete; software reference model implemented + tested
(`../refmodel/`, 28/28 checks pass).
**Companion:** the C header `../refmodel/blitter_ref.h` is the
machine-readable copy of this contract; the model in `blitter_ref.c` is the
golden output the RTL is diffed against.

---

## 1. Model & integration stance

The blitter is a **drop-in producer** for the existing double-buffered
framebuffer. Today the ARM's `NativeVideoWriter_WriteFrame` composites in
software, writes BUF0/BUF1, then bumps the video control word; `openbor_video_reader`
scans out whichever buffer the control word names. The blitter **replaces the
software compositing + buffer write**: the ARM emits a per-frame command list,
the fabric composites into the inactive buffer, then writes the **same** control
word. **The scanout reader is unchanged.**

```
 ARM (host)                         Fabric                      Scanout
 ----------                         ------                      -------
 build display list  ── ring ──▶  walk list (until END)
 upload changed atlases ─────────▶  blit each cmd into the
 bump submit_seq (doorbell) ─────▶  INACTIVE framebuffer
                                    write video ctrl word ───▶  openbor_video_reader
                                    bump done_seq               (UNCHANGED)
 poll done_seq ◀─────────────────  IRQ/flag
```

Pixel model v1: **320×240 RGB565**, matching the existing buffers and reader.
Transparency is **colorkey** (Solarus/SDL colorkey blits), with optional
**constant-alpha** for fades. Per-pixel alpha is deferred (needs a 32-bit source
format) — see §6.

## 2. DDR memory map

The existing 1 MiB region at `0x3A000000` is **untouched** except for the video
control word the blitter writes at end-of-frame (the existing producer contract).
All new blitter structures live in a **dedicated region** so nothing collides
with the joystick/cart/audio offsets:

| Phys base    | Size      | Purpose                                            |
|--------------|-----------|----------------------------------------------------|
| `0x3A000000` | 0x40      | **(existing)** video ctrl word + joy/cart/audio    |
| `0x3A000040` | 153,600 B | **(existing)** framebuffer BUF0 (blit target)      |
| `0x3A040040` | 153,600 B | **(existing)** framebuffer BUF1 (blit target)      |
| `0x3B000000` | 0x40      | **(new)** blitter control block (§3)               |
| `0x3B000040` | 256 KiB   | **(new)** command ring: 8192 entries × 32 B        |
| `0x3B040040` | ≥ N MiB   | **(new)** source-surface heap (atlases/sprites)    |

The blitter's read master fetches commands + source pixels from the new region;
its write master targets BUF0/BUF1 in the existing region. (Region base/size are
provisional — finalized against the f2h address map in the #003 spike.)

## 3. Control block (`0x3B000000`, all 32-bit LE)

| Off   | Name         | Writer | Meaning                                           |
|-------|--------------|--------|---------------------------------------------------|
| 0x00  | `submit_seq` | ARM    | doorbell: incremented after the list is in DDR    |
| 0x04  | `cmd_count`  | ARM    | number of valid commands in the ring this frame   |
| 0x08  | `target_buf` | ARM    | which framebuffer to composite into (0/1)         |
| 0x0C  | `clear_color`| ARM    | RGB565; if `flags.CLEAR`, fill target first       |
| 0x10  | `flags`      | ARM    | bit0 CLEAR-before-list; rest reserved             |
| 0x20  | `done_seq`   | fabric | set = `submit_seq` when the frame is composited   |
| 0x24  | `status`     | fabric | bit0 busy; error bits reserved                    |

**Frame handshake (per frame):**
1. ARM writes commands into the ring + uploads any changed atlases.
2. ARM sets `cmd_count`, `target_buf`, optional `clear_color`/`flags`.
3. ARM increments `submit_seq` (doorbell). *(All command/pixel writes must be
   committed before this store — same ordering rule the video writer already
   relies on.)*
4. Fabric sees `submit_seq != done_seq`, sets `status.busy`, optionally clears
   the target, walks the ring until `END` or `cmd_count`, compositing into
   `target_buf`.
5. Fabric writes the **video control word** (`0x3A000000` =
   `(frame_counter++ << 2) | target_buf`) — handing the frame to scanout exactly
   as the ARM does today.
6. Fabric sets `done_seq = submit_seq`, clears `status.busy`.
7. ARM polls `done_seq` (or takes an IRQ) before reusing that buffer.

This is single-frame in flight; double-buffering means the ARM can build frame
N+1's list while the fabric composites N.

## 4. Command word — 32 bytes / 8×u32

Mirrors `blt_cmd_t` in `blitter_ref.h`. All source offsets/strides are **bytes**;
`dst_x/dst_y` are **signed** (offscreen rects are culled with zero memory traffic,
CV1000-style).

| u32 | bits 31..24 | 23..16      | 15..8        | 7..0       |
|-----|-------------|-------------|--------------|------------|
| 0   | flags       | format      | blend_mode   | opcode     |
| 1   | src_off (byte offset into source heap) [31:0]            |||
| 2   | src_x [31:16]              | src_stride (bytes) [15:0]   ||
| 3   | h [31:16]                 | w [15:0] *(blit size, px)*  ||
| 4   | (reserved) [31:16]        | src_y [15:0]                ||
| 5   | dst_y (s16) [31:16]       | dst_x (s16) [15:0]          ||
| 6   | priority [31:24] | alpha [23:16] | colorkey (RGB565) [15:0]|||
| 7   | (reserved: tint/zoom) [31:16] | color (RGB565 fill) [15:0]||

Qwords pack as `qw[k] = {u32[2k+1], u32[2k]}` (little-endian). This layout is
**frozen** — implemented identically in `rtl/blitter_top.sv` (unpack) and
`sim/gen_vectors.c` (pack), and verified bit-exact end-to-end (§7).

**opcode:** `0 NOP · 1 END · 2 FILL · 3 BLIT`
**blend_mode (BLIT):** `0 COPY (opaque) · 1 COLORKEY (skip src==colorkey) · 2 CONST_ALPHA`
**flags:** `0x01 HFLIP · 0x02 VFLIP · 0x04 COLORKEY (also key a CONST_ALPHA blit)`
**format:** `0 RGB565` (v1); 1/2 reserved for ARGB1555/ARGB8888.

### Semantics (exactly as the reference model executes)
- **Walk until `END`** (or `cmd_count` commands) — a command after `END` never
  runs. Commands execute **in order**; later commands overdraw earlier ones
  (painter's order).
- **Clipping:** dst rect is clipped to 320×240. Fully-offscreen ⇒ **no writes**.
  Partial/negative origins draw only the visible sub-rect with the correct
  source pixels.
- **COPY:** `dst = src`.
- **COLORKEY:** if `src == colorkey`, **skip the write** (fast path, no RMW);
  else `dst = src`.
- **CONST_ALPHA:** `dst_c = (src_c*alpha + dst_c*(255-alpha) + 127)/255` per
  channel at native 5/6/5 width. With `flags.COLORKEY`, keyed pixels are skipped
  first. **Divide-free RTL form (verified bit-exact):**
  `div(t) = (t + 128 + ((t+128)>>8)) >> 8`, `t = src_c*alpha + dst_c*(255-alpha)`.
- **FILL:** write `color` to the clipped dst rect (no key, no blend).
- **HFLIP/VFLIP:** mirror source sampling within the blit rect.

## 5. Why this format / source-pixel decision

- **Compact (32 B):** ring bandwidth matters; 80–150 cmds/frame × 32 B × 60 fps
  ≈ 0.3 MB/s ring traffic — negligible.
- **Source format = RGB565** to match the framebuffer and the engine's colorkey
  transparency. The A9 RGB565 net-loss finding does **not** apply here: that was
  about the *A9* doing per-pixel conversion; fixed-function fabric reads RGB565
  sources directly with no penalty, and 16bpp halves source-read DDR bandwidth
  vs 32bpp.
- **Colorkey skip-write is the fast path** — matches the HW finding
  ([[blitter-compute-bound]]) that blend/interp, not fetch, is the cost. Opaque
  + keyed blits do one read + one conditional write, no read-modify-write.

## 6. Deferred / future (reserved, not in v1)
- Per-pixel alpha (ARGB8888/ARGB1555 source formats) — needs a wider source read
  + true src-alpha blend. Reserved in `format`.
- Integer/fractional **zoom** + sub-pixel dst (CV1000 carries `zoom_x/zoom_y`) —
  reserved in u32[7]. Solarus/gmloader use scaled blits, so this is the most
  likely v2 add.
- Tint / per-channel modulate (CV1000 `0x80`=100%) — reserved.

## 7. Verification

`../refmodel/` builds + runs on the dev machine with no deps:
`make test` → 28 checks covering FILL, COPY, COLORKEY, CONST_ALPHA (incl.
alpha=0/255 endpoints), HFLIP, VFLIP, negative/edge/offscreen clipping, the END
terminator, and painter-order overdraw. This model is (a) the golden output the
#003 RTL is diffed against, and (b) the executable spec the host emitter (#006)
develops against without hardware.

**RTL ↔ model equivalence (`../sim/`):** `rtl/blitter_top.sv` is simulated
(Icarus Verilog) against a behavioral DDR model; `sim/gen_vectors.c` drives the
*same* reference model to produce a golden framebuffer, and the testbench diffs
the blitter's DDR output qword-for-qword. `cd sim && make test` → **11/11
scenarios PASS** (FILL, COPY, COLORKEY, CONST_ALPHA, H/V flip, negative +
offscreen clip, painter-order overdraw, hardware CLEAR, buffer-1 target). This
makes the protocol above an *executable, dual-verified* contract — not just a
paper spec — before any Quartus build or hardware run.
