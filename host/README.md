# host/ — engine-agnostic blit display-list emitter

The ARM-side library that turns an engine's per-frame draws into a blitter
command list + source-heap uploads, and drives the submit/done handshake. Any
software engine binds to it (Solarus first, then gmloader / OpenBOR); the engine
binding only translates *its* draw calls into these calls.

```sh
make test     # emitter + wire-codec unit tests (no hardware, no deps)
```

## Files

| File | Role |
|------|------|
| `blt_wire.h` | **canonical** pack/unpack between `blt_cmd_t` and the 32-byte on-wire command. Shared by the emitter, `sim/gen_vectors.c`, and any RTL cross-check — one source of truth for the command layout. |
| `blt_emitter.{h,c}` | the emitter: builds the command ring, bump-allocates the source heap (uploads persist across frames), latches `cmd_count`/`submit_seq`. |
| `test_emitter.c` | unit tests (22 checks). |

## Usage sketch

```c
blt_emitter_t e;
blt_emitter_init(&e, ring_ptr, ring_cap, heap_ptr, heap_cap);

blt_surface_ref_t atlas = blt_upload(&e, tile_px, 256, 256, 256*2); // ONCE, static

/* per frame */
blt_begin_frame(&e, target_buf, /*clear=*/1, bg_rgb565);
blt_blit(&e, atlas, sx,sy, w,h, dx,dy, BLT_BLEND_COLORKEY, key, 0, 0);
blt_fill(&e, hx,hy, hw,hh, hud_rgb565);
blt_end_frame(&e);
/* caller: copy e.ring (e.cmd_count*32 B) + control block to DDR, bump doorbell */
```

On hardware, `ring_ptr`/`heap_ptr` point at the DDR ring/heap regions
(`0x3B000000`, see `../docs/blitter-protocol.md`); in tests they are `malloc`.
The emitter never writes the DDR control block itself — the engine binding
copies `e`'s control mirror (`cmd_count`, `target_buf`, `flags`, `clear_color`)
and bumps `submit_seq`, matching the existing `native_video_writer` ordering rule.

## Design notes
- **Upload-once static atlases:** `blt_upload` is a persistent bump allocation;
  keep the returned handle and re-blit it every frame for free. `blt_heap_reset`
  reclaims on a scene/quest change. (Dirty-tracking of *changed* dynamic surfaces
  is the #005 upload-path refinement.)
- **Overflow is reported, never overrun:** `e.overflow` flags a ring/heap
  capacity miss; the engine binding falls back to SDL for that frame.
- **Cheap emit:** building a command is a struct fill + 32-byte pack. This is
  what removes the A9-side per-draw SDL traversal/call overhead, not just the
  per-pixel blit.

## Verification

`make test` (22 checks):
- **wire codec round-trips** every field incl. signed negative `dst_x/dst_y`.
- **emitter scene == hand-built scene**, bit-exact through the reference model
  (emitter ring → `blt_unpack_cmd` → `blt_execute` over the emitter heap, vs a
  hand-written `blt_cmd_t[]` over the same heap).
- **static atlas reused** across 3 frames without heap growth.
- **overflow guards** on oversized upload + full ring.

Because `sim/gen_vectors.c` now uses the same `blt_wire.h`, the host emitter and
the RTL-verified simulation share one command codec end-to-end.
