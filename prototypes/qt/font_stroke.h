/*
 *  font_stroke.h — a scalable stand-in font, so the glyph cache can be
 *                  exercised with no font library present.
 *
 *  glyph_cache.h defines the interface that matters: uio_rasterize_fn, "hand me
 *  8-bit coverage plus metrics for this code point at this pixel size and
 *  subpixel phase". A real port implements it with the font stack the
 *  application already links — FreeType's FT_Render_Glyph, or Qt's
 *  QRawFont::alphaMapForGlyph. This file implements it with stroked polylines
 *  and analytic supersampling so this repository stays dependency-free while
 *  still producing genuinely antialiased, genuinely proportional, genuinely
 *  scalable glyphs.
 *
 *  It is a STAND-IN, and the ways it is not a real font are all in the shapes,
 *  never in the interface: uppercase plus digits and punctuation, lowercase
 *  rendered as small caps, no kerning pairs, no hinting, no ligatures. Nothing
 *  downstream of the rasterizer callback knows or cares.
 *
 *  GPL-3.0.
 */
#ifndef UIO_FONT_STROKE_H
#define UIO_FONT_STROKE_H

#include "glyph_cache.h"

#ifdef __cplusplus
extern "C" {
#endif

/*  Tuning for the stand-in face. `weight` is the stroke width as a fraction of
 *  the cap height (0.09 reads like a regular weight at 16 px); `tracking` is
 *  extra advance per glyph, also in cap-height units. */
typedef struct {
    float weight;
    float tracking;
} uio_stroke_font_t;

void uio_stroke_font_default(uio_stroke_font_t *f);

/*  A uio_rasterize_fn over uio_stroke_font_t (pass the struct as `ctx`).
 *
 *  `px_size` is the cap height in pixels; the em box is taller, so a 16 px
 *  request produces glyphs up to ~20 px tall including descenders. Coverage is
 *  4x4 supersampled against the stroked polylines, which is more resolution
 *  than the 16-level CLUT ramp downstream can carry — deliberately, so the
 *  quantisation error measured in the tests is the ramp's, not the
 *  rasterizer's.
 */
int uio_stroke_rasterize(void *ctx, uint32_t codepoint, int px_size, int phase,
                         uio_glyph_bmp_t *out);

/* Largest cap height the fixed scratch bitmap can hold. */
#define UIO_STROKE_MAX_PX 48

#ifdef __cplusplus
}
#endif
#endif /* UIO_FONT_STROKE_H */
