# host/ — engine-agnostic blit display-list emitter

The ARM-side library that turns an engine's per-frame draws into a blitter
command list, manages the source heap + SDRAM residency, and drives the
submit/done handshake. Any software engine binds to it (Solarus first, then
gmloader / OpenBOR); the engine binding only translates *its* draw calls into
these calls. This is the library the shipping Solarus port runs on real
hardware.

```sh
make test     # emitter + codec unit tests AND the embedded self-test (no hardware, no deps)
```

## Files

| File | Role |
|------|------|
| `blt_wire.h` | **canonical** pack/unpack between `blt_cmd_t` and the 32-byte on-wire command. Shared by the emitter, `sim/gen_vectors.c`, and any RTL cross-check — one source of truth for the command layout (including the COLORMOD tint bytes and the tile-list header reuse). |
| `blt_emitter.{h,c}` | the emitter: command ring builder, source-heap uploads (RGB565 + ARGB4444), `STAGE`/SDRAM staging incl. the permanent (grow-only) residency region, tile-list emission, blended/alpha fills, color-mod blits, `cmd_count`/`submit_seq` latching. Embedded self-test via `-DBLT_EMITTER_SELFTEST`. |
| `blt_alloc.{h,c}` | free-list, first-fit + coalescing **offset allocator** used for the DDR3 heap and both SDRAM regions. Replaced the v1 bump allocator: with dynamic surfaces, a bump pointer leaks on every invalidate and overflows on scene transitions. |
| `test_emitter.c` | unit tests (22 checks). |

## Usage sketch

```c
blt_emitter_t e;
blt_emitter_init(&e, ring_ptr, ring_cap, heap_ptr, heap_cap);
blt_sdram_regions_init(&e, perm_base, perm_size, inter_base, inter_size);
blt_tile_list_init(&e, tl_ptr, tl_cap);

/* load time, once per quest: upload + stage every atlas into resident SDRAM */
blt_surface_ref_t atlas = blt_upload(&e, tile_px, 256, 256, 256*2);
blt_stage_surface_perm(&e, &atlas);

/* per frame */
blt_begin_frame(&e, target_buf, /*clear=*/1, bg_rgb565);
blt_tile_list_static(&e, atlas, BLT_BLEND_COLORKEY, key, 0, 0,
                     entry_off, n_tiles, bias_x, bias_y);   /* whole layer */
blt_blit(&e, atlas, sx,sy, w,h, dx,dy, BLT_BLEND_COLORKEY, key, 0, 0);
blt_fill_alpha(&e, 0,0, 320,240, 0 /*black*/, fade_alpha);  /* fade overlay */
blt_end_frame(&e);
/* caller: copy e.ring (e.cmd_count*32 B) + control block to DDR, bump doorbell */
```

On hardware, `ring_ptr`/`heap_ptr`/`tl_ptr` point at the DDR regions
(`0x3B000000`, see `../docs/blitter-protocol.md`); in tests they are `malloc`.
The emitter never writes the DDR control block itself — the engine binding
copies `e`'s control mirror (`cmd_count`, `target_buf`, `flags`, `clear_color`)
and bumps `submit_seq` last (the store-release ordering rule).

## Design notes

- **Upload-once, stage-once residency:** `blt_upload` + `blt_stage_surface_perm`
  at load time puts an atlas in permanent SDRAM; re-blitting it every frame is
  free and it never re-uploads mid-game. The permanent region is grow-only by
  design — freeing "immutable" assets was a source-corruption hazard, so the
  API simply doesn't allow it. Dynamic surfaces use the recycled region
  (`blt_stage_surface` / `blt_sdram_free`) instead.
- **Tile lists make emit cost O(layers), not O(tiles):** entries are recorded
  once per map into the tile-list buffer in **map coordinates**;
  `blt_tile_list_static` / `blt_tile_list_res` emit one 32-byte header with a
  per-frame map→screen bias. Camera movement costs zero entry rewrites.
- **Overflow is reported, never overrun:** `e.overflow` / `e.perm_overflow`
  flag capacity misses loudly so the binding can fail visibly (the silent
  variant of this bug once shipped as an unexplained black screen).
- **Cheap emit:** building a command is a struct fill + 32-byte pack.

## Verification

`make test` runs both suites:

- `test_emitter` (22 checks): wire-codec round-trips (incl. signed negative
  `dst_x/dst_y`), emitter scene == hand-built scene bit-exact through the
  reference model, static-atlas reuse across frames without heap growth,
  overflow guards.
- `selftest_emitter` (embedded, `-DBLT_EMITTER_SELFTEST`): header packing for
  the batch opcodes — tile-list emission (`_res` + `_static`, incl. bias +
  `FRT_UPLOAD`) and the const-alpha fill.

Because `sim/gen_vectors.c` uses the same `blt_wire.h`, the host emitter and
the RTL-verified simulation share one command codec end-to-end — and the
downstream engine repo vendors these exact files, so what's tested here is
what runs on the device.
