/*
 *  ui_offload.h — a menu-front-end draw layer that runs entirely on the fabric.
 *
 *  WHAT THIS IS
 *  ------------
 *  A worked example of the offload argued in docs/qt-offload-feasibility.md:
 *  a software-rendered (Qt Quick Software / QPainter) front-end stops
 *  rasterizing pixels on the Cortex-A9 and instead emits a per-frame blitter
 *  display list that the fabric composites. The transport is unchanged — the
 *  same DDR double-buffer + doorbell the app already presents through; only
 *  the *source* of the frame changes (feasibility doc §1).
 *
 *  It exists because the study's two hardest objections are the interesting
 *  ones, and both are answered here in code rather than in prose:
 *
 *    1. "Antialiased rounded corners (39 sites) can't map to a fixed-function
 *       blitter."  -> uio_rounded_rect(): the flat interior is three FILLs and
 *       each arc is one baked ARGB4444 coverage mask blitted under
 *       BLT_BLEND_PALPHA with HFLIP/VFLIP. The AA moves into the source asset;
 *       the A9 emits 7 commands and touches no pixels. (corner_atlas.h)
 *
 *    2. "Arbitrary-ratio cover scaling (69 sites) is nearest-texel or nothing."
 *       -> uio_image_scaled(): an exact-ratio draw takes the plain BLIT fast
 *       path, and an arbitrary ratio is emitted as a two-triangle
 *       BLT_OP_TRILIST quad, so the *fabric* resamples. That is what makes a
 *       per-frame animated zoom (focus cues, held-focus animation) free on the
 *       A9 — the case where pre-scaling at decode time cannot help because the
 *       ratio changes every frame.
 *
 *  WHAT IT IS NOT
 *  --------------
 *  Not a hardware measurement. The study's recommendation is measure-first,
 *  and nothing here substitutes for that. uio_stats_t counts *emit-side* work:
 *  commands emitted, destination pixels handed to the fabric, and the pixels a
 *  QPainter path would have rasterized instead. Treat it as an accounting of
 *  what moved, not as a frame-time claim.
 *
 *  MODEL LIMITS (the reference model is 320x240 RGB565, blitter_ref.h)
 *  ------------------------------------------------------------------
 *  The study targets 352x240 first and reaches 480i through a banded
 *  write-through WORK cache (§5.1). The reference model has a fixed 320x240
 *  framebuffer, so that is the geometry used here; nothing in this layer knows
 *  the framebuffer size except through BLT_FB_WIDTH/HEIGHT, and banding is a
 *  fabric-side concern that does not change the display list.
 *
 *  TRILIST is validated in simulation and against the reference model, but is
 *  NOT yet deployed in the production solarus-mister fabric (see the repo
 *  README) — so the scaling path is exercised here against the golden model,
 *  not on hardware.
 *
 *  GPL-3.0.
 */
#ifndef UIO_UI_OFFLOAD_H
#define UIO_UI_OFFLOAD_H

#include "blt_emitter.h"
#include "corner_atlas.h"
#include "font6x8.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- small geometry ----------------------------------------------------- */
typedef struct { int x, y, w, h; } uio_rect_t;

/*  An uploaded image.
 *
 *  uio_upload_image() surrounds the pixels with a one-texel replicated border.
 *  That border is not decoration: the fabric's triangle rasterizer samples
 *  nearest-texel with a half-texel bias, so an exact nearest-neighbour mapping
 *  needs uv to reach half a texel *outside* the image rect, and blt_vtx_t's uv
 *  is unsigned. The border makes that representable (and stops the sampler's
 *  clamp from bleeding a neighbouring atlas entry in). An unpadded image still
 *  scales — uio_image_scaled just reports it through stats.uv_clamped, and the
 *  mapping is up to half a source texel off at the leading edge.
 */
typedef struct {
    blt_surface_ref_t surf;    /* the uploaded surface (border included)      */
    uint16_t x, y;             /* image origin inside surf (1,1 when padded)  */
    uint16_t w, h;             /* image size in texels                        */
    uint8_t  padded;           /* 1 = has the one-texel border                */
} uio_image_ref_t;

/* ---- per-frame accounting (emit-side; see the header comment) ----------- */
typedef struct {
    uint32_t cmds;             /* blitter commands emitted this frame         */
    uint32_t fills;
    uint32_t blits;
    uint32_t trilists;         /* TRILIST commands (one per scaled image)     */
    uint32_t glyphs;
    uint32_t rounded_rects;
    uint32_t uv_clamped;       /* scaled draws from an UNPADDED image         */
    uint64_t fabric_px;        /* destination pixels the fabric will touch    */
    uint64_t a9_fill_px;       /* pixels a QPainter path would have filled    */
    uint64_t a9_aa_px;         /*  ...of which antialiased corner coverage    */
    uint64_t a9_resample_px;   /*  ...of which bilinear resample destinations */
    uint64_t a9_glyph_px;      /*  ...of which glyph raster                   */
} uio_stats_t;

/* ---- context ------------------------------------------------------------ */
/*  Cached (radius, alpha) corner masks. A screen needs one per distinct radius
 *  it draws — and an animated per-frame scale walks through radii as the shape
 *  grows, so this is sized for a scaling UI rather than a static one. Each mask
 *  costs radius^2 * 2 bytes, so the whole cache is a few KiB. */
#define UIO_MAX_CORNERS 32

typedef struct {
    int      radius;
    uint8_t  alpha;
    blt_surface_ref_t surf;    /* radius x radius ARGB4444 coverage mask      */
} uio_corner_t;

typedef struct {
    blt_emitter_t *e;
    uint8_t  *src_base;        /* base of the source region blt_execute reads */
    uint32_t  vtx_off;         /* vertex arena, byte offset within src_base   */
    uint32_t  vtx_bytes;
    uio_corner_t corners[UIO_MAX_CORNERS];
    int       ncorners;
    blt_surface_ref_t font;
    int       font_ready;
    uio_stats_t stats;         /* reset by uio_begin_frame                    */
    int       last_error;      /* first non-zero emitter/bake failure seen    */
} uio_t;

/* ---- lifecycle ---------------------------------------------------------- */

/*  Bind to an emitter whose source heap is `src_base` (the region the fabric —
 *  or blt_execute — reads sources AND TRILIST vertex entries from), and carve
 *  a `vtx_bytes` vertex arena out of that heap.
 *
 *  The arena has to come out of the heap because a TRILIST header carries a
 *  byte offset resolved against the source-heap base; keeping the arena inside
 *  the heap is what makes those offsets valid for both blt_execute and the
 *  fabric. Returns 0, or -1 if the arena does not fit.
 */
int uio_init(uio_t *u, blt_emitter_t *e, void *src_base, uint32_t vtx_bytes);

/* Bake + upload the 6x8 glyph atlas (once). Returns 0 or -1. */
int uio_load_font(uio_t *u);

/*  Bake + upload the corner coverage mask for (radius, alpha), or return the
 *  cached one. Call at load time for every radius the UI uses; uio_rounded_rect
 *  will also bake on demand. Returns the cache slot, or -1 on failure.
 */
int uio_corner(uio_t *u, int radius, uint8_t alpha);

/* Upload an RGB565 image with a one-texel replicated border (see uio_image_ref_t).
 * `pitch` is the source row stride in bytes. On failure the handle's
 * surf.valid is 0. */
uio_image_ref_t uio_upload_image(uio_t *u, const uint16_t *px, int w, int h, int pitch);

/*  Same, for an ARGB4444 image (art with per-pixel alpha), which blits under
 *  BLT_BLEND_PALPHA.
 *
 *  Such an image can be blitted 1:1 but NOT scaled on the fabric: the triangle
 *  rasterizer samples 16bpp colour with no alpha channel, so uio_image_scaled
 *  rejects it rather than quietly dropping the alpha. Scaled art with real
 *  per-pixel alpha is one of the cases that stays on the A9 — a limit of the
 *  current TRILIST contract, not of this layer. */
uio_image_ref_t uio_upload_image_alpha(uio_t *u, const uint16_t *px4444,
                                       int w, int h, int pitch);

/* ---- frame -------------------------------------------------------------- */
void uio_begin_frame(uio_t *u, int target_buf, int clear, uint16_t clear_color);
void uio_end_frame(uio_t *u);

/* ---- primitives --------------------------------------------------------- */

/* Solid fill. `alpha` < 255 emits a CONST_ALPHA fill. */
int uio_fill(uio_t *u, uio_rect_t r, uint16_t color, uint8_t alpha);

/*  Antialiased rounded rectangle, entirely on the fabric.
 *
 *  radius is clamped to min(w,h)/2 (radius == that is a pill, which is exactly
 *  what CoreStatusPill.qml wants). alpha < 255 is baked into the corner mask's
 *  coverage, because BLT_BLEND_PALPHA has no constant-alpha input — so a
 *  translucent rounded card costs one extra (radius, alpha) mask, not a
 *  fallback to the A9.
 *
 *  Emits 3 FILLs + 4 PALPHA blits (fewer when the rect is degenerate).
 *  Returns 0, or -1 if a mask could not be baked/uploaded.
 */
int uio_rounded_rect(uio_t *u, uio_rect_t r, int radius, uint16_t color, uint8_t alpha);

/*  A rounded rect with a uniform outline: an outer rounded rect in
 *  `ring_color` and an inner one, inset by `thickness`, in `fill_color`.
 *
 *  This is not a convenience wrapper invented here — it is the shape the real
 *  UI already draws. Zaparoo's Tile.qml builds its focus ring from "two stacked
 *  *filled* rounded rectangles ... significantly smoother on the corners under
 *  Qt's software adaptation: filled rounded rects honour the AA path, while
 *  thin rounded *borders* are tessellated without subpixel coverage and step
 *  visibly at the corners". The same construction is what a fixed-function
 *  blitter wants, for the same reason: two coverage-masked shapes, no stroking.
 *
 *  Emits 14 commands (two rounded rects). Returns 0, or -1.
 */
int uio_rounded_rect_outline(uio_t *u, uio_rect_t r, int radius, int thickness,
                             uint16_t ring_color, uint16_t fill_color, uint8_t alpha);

/* 1:1 image blit (no scaling). `blend` is BLT_BLEND_COPY / COLORKEY / CONST_ALPHA. */
int uio_image_blit(uio_t *u, uio_image_ref_t img, int dx, int dy,
                   uint8_t blend, uint16_t colorkey, uint8_t alpha);

/* Which path a scaled draw took (uio_image_scaled's return value). */
#define UIO_PATH_BLIT     1    /* exact ratio -> plain BLT_OP_BLIT            */
#define UIO_PATH_TRILIST  2    /* arbitrary ratio -> BLT_OP_TRILIST quad      */

typedef struct {
    uio_rect_t dst;            /* destination rect, framebuffer pixels        */
    uio_rect_t src;            /* source sub-rect; w==0 or h==0 = whole image */
    uint8_t    blend;          /* COPY / COLORKEY / CONST_ALPHA               */
    uint16_t   colorkey;
    uint8_t    alpha;          /* CONST_ALPHA opacity                         */
    uint8_t    tint[3];        /* per-channel modulation; {255,255,255} and
                                * {0,0,0} (the zero value) both mean "none"   */
    int        force_fabric;   /* 1 = TRILIST even at 1:1 (test/bench hook)   */
} uio_scale_t;

/*  Draw `img` into s->dst, scaling on the FABRIC.
 *
 *  Exact 1:1 takes the cheaper BLT path (UIO_PATH_BLIT). Any other ratio —
 *  including a ratio that changes every frame, which is the case pre-scaling
 *  at decode time cannot serve — becomes a two-triangle TRILIST quad
 *  (UIO_PATH_TRILIST) whose uv mapping reproduces standard nearest-neighbour
 *  sampling exactly for a padded image.
 *
 *  Returns UIO_PATH_*, or -1 on a bad argument, an emitter overflow, or a
 *  per-pixel-alpha (ARGB4444) image that needs scaling — see
 *  uio_upload_image_alpha for why that one is refused rather than approximated.
 */
int uio_image_scaled(uio_t *u, uio_image_ref_t img, const uio_scale_t *s);

/* Text run from the glyph atlas: one PALPHA+COLORMOD blit per inked glyph, so
 * a single white atlas serves every colour. Returns the advance width in
 * pixels (also computable without emitting via uio_text_width). */
int uio_text(uio_t *u, int x, int y, const char *str, uint16_t color);
int uio_text_width(const char *str);

/* ---- helpers ------------------------------------------------------------ */

/*  PreserveAspectFit: the largest iw x ih box that fits in `box`, centred.
 *  (fillMode: PreserveAspectFit is 32 of the study's 69 scaling sites.) */
uio_rect_t uio_fit(uio_rect_t box, int iw, int ih);

/*  Expand RGB565 to the RGB888 modulation triple that reproduces it exactly
 *  through BLT_F_COLORMOD on a white source. Bit-replication expansion is
 *  exactly the inverse of the fabric's round(ch * mod / 255) reduction, which
 *  is why a tinted corner arc and a plain FILL of the same colour meet with no
 *  seam. (Gated over all 65536 colours in test_ui_offload.c.) */
static inline void uio_565_to_888(uint16_t c, uint8_t *r, uint8_t *g, uint8_t *b)
{
    unsigned r5 = (c >> 11) & 0x1Fu, g6 = (c >> 5) & 0x3Fu, b5 = c & 0x1Fu;
    *r = (uint8_t)((r5 << 3) | (r5 >> 2));
    *g = (uint8_t)((g6 << 2) | (g6 >> 4));
    *b = (uint8_t)((b5 << 3) | (b5 >> 2));
}

#ifdef __cplusplus
}
#endif
#endif /* UIO_UI_OFFLOAD_H */
