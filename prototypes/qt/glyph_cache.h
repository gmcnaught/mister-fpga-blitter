/*
 *  glyph_cache.h — general text on the fabric: any font, any size, antialiased.
 *
 *  WHY THIS EXISTS
 *  ---------------
 *  font6x8.h handles the easy case the feasibility study calls the "bitmap-font
 *  freebie" (§4): a fixed-cell face on the CRT path, where every glyph is a
 *  uniform blit out of one atlas. That case does not generalise — it has one
 *  size, one advance width, no antialiasing, and 64 code points.
 *
 *  This is the general case, and it rests on one observation: THE FABRIC DOES
 *  NOT NEED TO RASTERIZE TEXT IN ORDER TO COMPOSITE IT. A glyph outline is
 *  rasterized once, by the A9, at the size it is first seen; from then on every
 *  frame that draws that glyph is a blit of a cached coverage bitmap. Steady
 *  state is what matters for an offload, and in steady state text is pixels
 *  being moved, not pixels being computed.
 *
 *  So "generalise text" is really three problems, and the contract already has
 *  an answer to each:
 *
 *    1. Antialiasing. Coverage goes in the source's ALPHA, exactly like the
 *       antialiased corner masks — BLT_BLEND_PALPHA composites it. Text AA does
 *       not need a rasterizer in the fabric; it needs alpha in the atlas.
 *
 *    2. Colour. The atlas is colour-free: a texel IS a coverage level, and a
 *       BLT_FMT_PAL8 source resolves it through a 16-entry CLUT ramp whose
 *       entries all carry the same RGB565 and ascending A4. One atlas serves
 *       every colour, at 1 byte per texel instead of 2, and the colour is a
 *       palette selection rather than a re-bake.
 *
 *    3. Cost per frame. A sprite-list entry carries its OWN palette word
 *       (blt_wire.h [Task 4b]), so a whole screen of text — mixed colours
 *       included — collapses into ONE BLT_OP_SPRITELIST command instead of one
 *       command per glyph.
 *
 *  Both PAL8+CLUT and SPRITELIST are production, hardware-validated paths (see
 *  the repo README's status table), so none of this needs new fabric.
 *
 *  WHAT STAYS ON THE A9 — and why that is fine
 *  -------------------------------------------
 *  Outline rasterization (FreeType, QRawFont, whatever the app already uses).
 *  The cost is per DISTINCT (code point, pixel size, subpixel phase), paid once
 *  and amortised to zero: a menu screen touches a few hundred distinct glyphs,
 *  after which the cache is warm for the rest of the session. What does NOT
 *  amortise — and therefore what this does not fix — is text whose SIZE
 *  animates continuously: every size is a fresh rasterization, and the
 *  triangle path cannot scale a cached glyph because it samples no alpha
 *  channel. Scale the box, not the type; or accept the re-raster.
 *
 *  GPL-3.0.
 */
#ifndef UIO_GLYPH_CACHE_H
#define UIO_GLYPH_CACHE_H

#include "ui_offload.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Coverage levels a PAL8 ramp resolves. A4 is the alpha width the CLUT entry
 * carries (comp_clut.vh's CLUT_MAKE: RGB565 in [15:0], alpha in [19:16]), so
 * 16 levels is the ceiling for per-pixel alpha anywhere in this contract —
 * ARGB4444 sources have exactly the same 4-bit alpha. test_glyph_cache bounds
 * the resulting compositing error against 8-bit coverage. */
#define UIO_COV_LEVELS 16

/* ── the host-side rasterizer the cache calls on a miss ─────────────────── */

/*  One rasterized glyph, in the FreeType convention:
 *    cov        : w*h 8-bit coverage, row stride `pitch`
 *    bearing_x  : pen-relative left edge of the bitmap
 *    bearing_y  : rows from the baseline UP to the bitmap's top edge
 *    advance    : pen advance, whole pixels
 *  So the blit lands at (pen_x + bearing_x, baseline_y - bearing_y).
 */
typedef struct {
    const uint8_t *cov;
    int pitch;
    int w, h;
    int bearing_x, bearing_y;
    int advance;
} uio_glyph_bmp_t;

/*  Rasterize `codepoint` at `px_size`, horizontally offset by
 *  phase/`phases` of a pixel. Return 0 on success, non-zero to skip the glyph.
 *  The returned coverage buffer only has to stay valid until the call returns.
 *
 *  A real port implements this with the font stack the app already links
 *  (FreeType's FT_Render_Glyph, or Qt's QRawFont::alphaMapForGlyph); the demo
 *  implements it with font_stroke.c so the cache path can be exercised with no
 *  font library present.
 */
typedef int (*uio_rasterize_fn)(void *ctx, uint32_t codepoint, int px_size,
                               int phase, uio_glyph_bmp_t *out);

/* ── the cache ──────────────────────────────────────────────────────────── */
typedef struct {
    uint32_t codepoint;
    uint16_t px_size;
    uint8_t  phase;
    uint8_t  valid;
    uint16_t sx, sy, w, h;      /* rect within the atlas (w,h = inked size)  */
    uint16_t cell_w, cell_h;    /* rect the SHELF gave this slot. Kept across
                                 * evictions: a recycled slot that recorded only
                                 * the smaller glyph's size would shrink the
                                 * atlas a little on every reuse, until nothing
                                 * large could be placed at all. */
    int16_t  bearing_x, bearing_y, advance;
    uint32_t used_frame;        /* for LRU + the in-flight guard             */
} uio_glyph_t;

typedef struct {
    uint8_t          *pixels;   /* host view of the PAL8 atlas (inside the heap) */
    blt_surface_ref_t surf;     /* the same memory, as a source handle           */
    int               w, h;

    /* shelf packer: glyphs are similar heights, so shelves waste little */
    int shelf_x, shelf_y, shelf_h;

    uio_glyph_t *slots;
    int          nslots, count;

    uio_rasterize_fn raster;
    void            *raster_ctx;
    int              phases;    /* 1 = pixel-aligned; N = N subpixel phases   */

    uint32_t frame;             /* bumped by uio_glyph_atlas_frame()          */
    uint32_t dirty_lo, dirty_hi;/* byte range written since the last flush    */

    /* diagnostics — the numbers that decide whether this is worth it */
    uint32_t hits, misses, evictions, refused, rasterized_px;
} uio_glyph_atlas_t;

/*  Bind a device CLUT mirror (BLT_CLUT_BANKS * BLT_CLUT_ENTRIES 32-bit words)
 *  and reset the ramp allocator. Pass the same buffer as blt_surface_heap_t's
 *  `clut` when executing, and upload it to the fabric with
 *  blt_emit_clut_upload() whenever uio_text_ramp() adds a colour. */
int uio_clut_bind(uio_t *u, void *clut, size_t bytes);

/*  Get (or allocate) the 16-entry coverage ramp for `rgb565`, packed as the
 *  command/entry colour word via blt_pal_color(). Returns -1 if the CLUT is
 *  unbound or out of ramps. A ramp is 16 of the 256 slots in a bank, so a bank
 *  holds 16 colours and the CLUT holds 512.
 *
 *  Entry i of a ramp is {RGB565 = rgb565, A4 = i}, which makes a fully covered
 *  texel composite bit-identically to a FILL of the same colour. */
int uio_text_ramp(uio_t *u, uint16_t rgb565);
int uio_clut_dirty(const uio_t *u);      /* 1 = re-upload the CLUT before use */
int uio_clut_upload(uio_t *u);           /* emit BLT_OP_CLUT_UPLOAD, clear it  */

/*  Initialise a glyph atlas of `w` x `h` PAL8 texels, allocated from the
 *  emitter's source heap (so the fabric can read it). `slots` is caller-owned
 *  storage for the cache directory. Returns 0, or -1 if the heap is full. */
int uio_glyph_atlas_init(uio_t *u, uio_glyph_atlas_t *fa, int w, int h,
                         uio_glyph_t *slots, int nslots,
                         uio_rasterize_fn fn, void *ctx, int phases);

/*  Advance the atlas's frame counter. Call once per frame: it is what makes
 *  LRU eviction safe, because a glyph used in the last two frames is never
 *  evicted — the fabric may still be reading the frame the A9 just submitted. */
void uio_glyph_atlas_frame(uio_glyph_atlas_t *fa);

/*  Emit a STAGE for the atlas bytes written since the last flush, so newly
 *  rasterized glyphs reach the source memory the fabric reads. A no-op when
 *  nothing is dirty. (In sdram_src mode this becomes blt_stage_to with the
 *  atlas's SDRAM offset; the reference model reads the heap directly.) */
int uio_glyph_atlas_flush(uio_t *u, uio_glyph_atlas_t *fa);

/* ── drawing ────────────────────────────────────────────────────────────── */

/*  Draw a UTF-8 run at `px_size`, with `x` the pen start and `baseline_y` the
 *  baseline. Returns the advance width in pixels. Glyphs missing from the
 *  cache are rasterized and inserted on the spot.
 *
 *  x may be fractional: pass it in 8.8 fixed point via uio_text_run_fx to get
 *  subpixel placement when the atlas was built with phases > 1. */
int uio_text_run(uio_t *u, uio_glyph_atlas_t *fa, int px_size,
                 int x, int baseline_y, const char *utf8, uint16_t color);
int uio_text_run_fx(uio_t *u, uio_glyph_atlas_t *fa, int px_size,
                    int x_fx8, int baseline_y, const char *utf8, uint16_t color);

/* Advance width of a run, filling the cache as needed (same metrics path). */
int uio_text_run_width(uio_t *u, uio_glyph_atlas_t *fa, int px_size, const char *utf8);

/* ── batching ───────────────────────────────────────────────────────────── */

/*  While a batch is open, text runs accumulate into `ch` instead of emitting a
 *  blit per glyph, and the flush emits one BLT_OP_SPRITELIST per maximal run of
 *  entries sharing a header key. Since the palette (i.e. the colour) lives in
 *  the ENTRY, a mixed-colour screen still collapses to a single command.
 *
 *  `ch` must be bound to the emitter's sprite arena (uio_init reserves one).
 *  Z-order is emission order, which the channel preserves. */
void uio_text_batch_begin(uio_t *u, blt_sprite_channel_t *ch);
int  uio_text_batch_flush(uio_t *u);     /* -> commands emitted (0 if empty)  */
int  uio_text_batch_active(const uio_t *u);

#ifdef __cplusplus
}
#endif
#endif /* UIO_GLYPH_CACHE_H */
