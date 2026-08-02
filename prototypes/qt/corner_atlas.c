/*
 *  corner_atlas.c — see corner_atlas.h.
 *
 *  Everything here runs ONCE per (radius, alpha) at load time. The per-frame
 *  cost of an antialiased rounded rect is then four BLT_BLEND_PALPHA blits of
 *  this mask (plus three FILLs for the flat interior) — no A9 pixel work at
 *  all, which is the whole point of the exercise.
 *
 *  GPL-3.0.
 */
#include "corner_atlas.h"
#include <stddef.h>

/* Sample centres are at (i + (s+0.5)/SS). Working in units of 1/(2*SS) of a
 * texel keeps every term an exact integer: a sample centre is 2*SS*i + 2*s + 1
 * and the disc centre is 2*SS*radius. */
int uio_corner_coverage(int radius, int i, int j)
{
    if (radius <= 0 || radius > UIO_CORNER_MAX_RADIUS) return 0;
    if (i < 0 || j < 0 || i >= radius || j >= radius)   return 0;

    const int   S  = UIO_CORNER_SS;
    const long  cx = 2L * S * radius;                 /* disc centre, 1/(2S) units */
    const long  rr = cx * cx;                         /* radius^2 in the same units */
    int inside = 0;

    for (int t = 0; t < S; t++) {
        long dy = (2L * S * j + 2 * t + 1) - cx;
        long dy2 = dy * dy;
        if (dy2 > rr) continue;                       /* whole sample row is outside */
        for (int s = 0; s < S; s++) {
            long dx = (2L * S * i + 2 * s + 1) - cx;
            if (dx * dx + dy2 <= rr) inside++;
        }
    }
    /* inside/(S*S) scaled to 0..255, round-to-nearest. */
    return (int)((inside * 255 + (S * S) / 2) / (S * S));
}

int uio_bake_corner_mask(uint16_t *out, int radius, uint8_t alpha)
{
    if (!out || radius <= 0 || radius > UIO_CORNER_MAX_RADIUS) return -1;

    for (int j = 0; j < radius; j++) {
        for (int i = 0; i < radius; i++) {
            int cov = uio_corner_coverage(radius, i, j);      /* 0..255 */
            int a   = (cov * (int)alpha + 127) / 255;         /* 0..255 */
            int a4  = (a * 15 + 127) / 255;                   /* 0..15  */
            /* RGB white: the draw's BLT_F_COLORMOD tint supplies the colour. */
            out[(size_t)j * (size_t)radius + (size_t)i] =
                (uint16_t)((a4 << 12) | 0x0FFF);
        }
    }
    return 0;
}
