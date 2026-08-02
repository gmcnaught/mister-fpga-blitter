/*
 *  font6x8.h — fixed-cell 6x8 bitmap font, baked into an ARGB4444 glyph atlas.
 *
 *  The feasibility study's "bitmap-font freebie" (docs/qt-offload-feasibility.md
 *  §4): the CRT path already forces a fixed-cell bitmap font with NoAntialias,
 *  which is the ideal glyph-atlas case — every glyph is a uniform cell, so text
 *  becomes one uniform blit per glyph out of a single atlas, with no distance
 *  fields, no sub-pixel positioning and no per-glyph raster on the A9.
 *
 *  Like the corner masks (corner_atlas.h), the atlas is COLOUR-FREE: RGB is
 *  white and A4 is 0 or 15, so one atlas serves every text colour through
 *  BLT_F_COLORMOD.
 *
 *  The glyph shapes are a 5x7 demo face laid out in 6x8 cells (one column of
 *  advance, one row of leading). It covers ASCII 32..95 and folds lowercase
 *  onto uppercase — enough for a menu front-end's labels, and deliberately
 *  small: the point of the prototype is the offload path, not typography. A
 *  real port bakes its own font (e.g. MxPlus HP 100LX 6x8) into the same atlas
 *  shape and nothing else changes.
 *
 *  GPL-3.0.
 */
#ifndef UIO_FONT6X8_H
#define UIO_FONT6X8_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UIO_FONT_CELL_W  6
#define UIO_FONT_CELL_H  8
#define UIO_FONT_INK_W   5     /* inked columns; column 5 is the advance gap  */
#define UIO_FONT_INK_H   7     /* inked rows;    row 7 is the leading gap     */
#define UIO_FONT_FIRST   32    /* first encoded code point (space)            */
#define UIO_FONT_COUNT   64    /* ASCII 32..95; lowercase folds to uppercase  */
#define UIO_FONT_COLS    16
#define UIO_FONT_ROWS    4
#define UIO_FONT_ATLAS_W (UIO_FONT_COLS * UIO_FONT_CELL_W)   /* 96 */
#define UIO_FONT_ATLAS_H (UIO_FONT_ROWS * UIO_FONT_CELL_H)   /* 32 */
#define UIO_FONT_ATLAS_TEXELS (UIO_FONT_ATLAS_W * UIO_FONT_ATLAS_H)

/* Atlas cell index for `ch` (lowercase folded), or -1 if the font has no glyph
 * for it. Unmapped code points are the caller's fallback decision — this
 * prototype draws them as nothing rather than as a substitution box. */
int uio_font_cell(int ch);

/* Top-left texel of cell `cell` within the atlas. */
static inline int uio_font_cell_x(int cell) { return (cell % UIO_FONT_COLS) * UIO_FONT_CELL_W; }
static inline int uio_font_cell_y(int cell) { return (cell / UIO_FONT_COLS) * UIO_FONT_CELL_H; }

/* Bake the whole atlas into `out` (UIO_FONT_ATLAS_TEXELS uint16_t, row stride
 * UIO_FONT_ATLAS_W) as ARGB4444: RGB white, A4 = 15 on ink, 0 elsewhere. */
void uio_bake_font_atlas(uint16_t *out);

#ifdef __cplusplus
}
#endif
#endif /* UIO_FONT6X8_H */
