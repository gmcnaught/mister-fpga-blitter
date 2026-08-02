/*
 *  corner_atlas.h — bake antialiased rounded-corner coverage into ARGB4444
 *                   source sprites the fabric can blit.
 *
 *  This is the concrete answer to the "AA rounded corners don't map to a
 *  fixed-function blitter" objection in the Zaparoo feasibility study
 *  (docs/qt-offload-feasibility.md §3, §5.2): a rounded rectangle's flat
 *  interior is FILLs, and its four antialiased arcs are ONE baked quarter-disc
 *  coverage mask blitted four times with HFLIP/VFLIP under BLT_BLEND_PALPHA.
 *
 *  The mask is COLOUR-FREE: RGB is white and only the A4 channel carries
 *  coverage, so a single bake serves every themed colour — the draw supplies
 *  the colour through BLT_F_COLORMOD (see uio_565_to_888 in ui_offload.h,
 *  which makes the tinted arc bit-exact with a plain FILL of the same RGB565).
 *
 *  The bake is pure integer supersampling, once per (radius, alpha) pair at
 *  load time; per frame the A9 emits four 32-byte commands and touches no
 *  pixels. GPL-3.0.
 */
#ifndef UIO_CORNER_ATLAS_H
#define UIO_CORNER_ATLAS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Largest radius the baker accepts (a UI radius bigger than this is a design
 * error long before it is a blitter problem: the mask is radius^2 texels). */
#define UIO_CORNER_MAX_RADIUS 64

/* Supersampling grid used by the bake: UIO_CORNER_SS^2 samples per texel. */
#define UIO_CORNER_SS 8

/*
 *  Coverage of texel (i,j) of a top-left rounded corner of `radius`, as 0..255.
 *
 *  The corner occupies rect-local texels [0,radius) x [0,radius); the disc has
 *  centre (radius, radius) and radius `radius`, so (0,0) is the outside-most
 *  texel and (radius-1, radius-1) is fully inside. Integer-only (no libm, no
 *  float): each texel is sampled on a UIO_CORNER_SS x UIO_CORNER_SS grid at
 *  sample centres, and the result is the inside-sample fraction scaled to 255.
 *
 *  Returns 0 for out-of-range arguments.
 */
int uio_corner_coverage(int radius, int i, int j);

/*
 *  Bake the top-left quarter-disc into `out` as ARGB4444 {A4,R4,G4,B4}:
 *  RGB = white (0xF,0xF,0xF), A4 = round(coverage * alpha / 255 * 15 / 255).
 *
 *    out    : radius*radius uint16_t, row-major, row stride = radius texels
 *    radius : 1..UIO_CORNER_MAX_RADIUS
 *    alpha  : global opacity folded into the coverage (255 = opaque). Baking
 *             the opacity in is what lets a translucent rounded rect stay on
 *             the fabric: BLT_BLEND_PALPHA has no constant-alpha input, so a
 *             translucent card needs its own (radius, alpha) mask.
 *
 *  Returns 0 on success, -1 on a bad argument (out untouched).
 */
int uio_bake_corner_mask(uint16_t *out, int radius, uint8_t alpha);

#ifdef __cplusplus
}
#endif
#endif /* UIO_CORNER_ATLAS_H */
