/*
 *  font_stroke.c — see font_stroke.h.
 *
 *  Glyph coordinates: x grows right from the pen, y is 0 at the cap line and 1
 *  at the baseline, so a descender runs past 1. Everything is scaled by the
 *  requested cap height at raster time, which is what makes the face scalable
 *  and its metrics proportional.
 *
 *  GPL-3.0.
 */
#include "font_stroke.h"
#include <math.h>
#include <string.h>

#define UP (-1000.0f)          /* pen-up: starts a new polyline */

/* ── glyph outlines (ASCII 32..90; lowercase is folded to small caps) ────── */
static const float G_SPACE[]  = { 0 };
static const float G_BANG[]   = { 0.12f,0.00f, 0.12f,0.70f, UP,UP, 0.12f,0.88f, 0.12f,1.00f };
static const float G_QUOT[]   = { 0.10f,0.00f, 0.10f,0.24f, UP,UP, 0.28f,0.00f, 0.28f,0.24f };
static const float G_HASH[]   = { 0.20f,0.05f, 0.13f,0.95f, UP,UP, 0.45f,0.05f, 0.38f,0.95f,
                                  UP,UP, 0.06f,0.36f, 0.55f,0.36f, UP,UP, 0.04f,0.66f, 0.53f,0.66f };
static const float G_DOLLAR[] = { 0.50f,0.18f, 0.36f,0.08f, 0.16f,0.12f, 0.12f,0.32f,
                                  0.30f,0.46f, 0.48f,0.58f, 0.46f,0.82f, 0.28f,0.92f,
                                  0.10f,0.84f, UP,UP, 0.29f,0.00f, 0.29f,1.00f };
static const float G_PCT[]    = { 0.55f,0.05f, 0.09f,0.95f, UP,UP, 0.10f,0.05f, 0.24f,0.05f,
                                  0.24f,0.28f, 0.10f,0.28f, 0.10f,0.05f, UP,UP,
                                  0.40f,0.72f, 0.54f,0.72f, 0.54f,0.95f, 0.40f,0.95f, 0.40f,0.72f };
static const float G_AMP[]    = { 0.58f,0.95f, 0.18f,0.35f, 0.20f,0.12f, 0.36f,0.08f,
                                  0.42f,0.24f, 0.10f,0.62f, 0.12f,0.86f, 0.30f,0.95f, 0.52f,0.72f };
static const float G_APOS[]   = { 0.10f,0.00f, 0.10f,0.24f };
static const float G_LPAR[]   = { 0.34f,0.00f, 0.14f,0.28f, 0.14f,0.74f, 0.34f,1.05f };
static const float G_RPAR[]   = { 0.10f,0.00f, 0.30f,0.28f, 0.30f,0.74f, 0.10f,1.05f };
static const float G_STAR[]   = { 0.10f,0.20f, 0.46f,0.56f, UP,UP, 0.46f,0.20f, 0.10f,0.56f,
                                  UP,UP, 0.28f,0.14f, 0.28f,0.62f };
static const float G_PLUS[]   = { 0.28f,0.28f, 0.28f,0.80f, UP,UP, 0.05f,0.54f, 0.51f,0.54f };
static const float G_COMMA[]  = { 0.18f,0.86f, 0.16f,1.00f, 0.06f,1.14f };
static const float G_MINUS[]  = { 0.05f,0.56f, 0.51f,0.56f };
static const float G_DOT[]    = { 0.14f,0.92f, 0.14f,1.00f };
static const float G_SLASH[]  = { 0.44f,0.02f, 0.06f,1.00f };
static const float G_0[]      = { 0.08f,0.30f, 0.20f,0.06f, 0.42f,0.06f, 0.54f,0.30f,
                                  0.54f,0.74f, 0.42f,0.96f, 0.20f,0.96f, 0.08f,0.74f, 0.08f,0.30f,
                                  UP,UP, 0.16f,0.86f, 0.46f,0.18f };
static const float G_1[]      = { 0.10f,0.22f, 0.30f,0.04f, 0.30f,0.96f, UP,UP, 0.10f,0.96f, 0.50f,0.96f };
static const float G_2[]      = { 0.08f,0.24f, 0.22f,0.05f, 0.44f,0.07f, 0.54f,0.28f,
                                  0.42f,0.50f, 0.08f,0.96f, 0.56f,0.96f };
static const float G_3[]      = { 0.09f,0.10f, 0.36f,0.05f, 0.52f,0.20f, 0.40f,0.46f,
                                  0.20f,0.50f, UP,UP, 0.40f,0.46f, 0.55f,0.66f, 0.48f,0.90f,
                                  0.28f,0.97f, 0.08f,0.88f };
static const float G_4[]      = { 0.42f,0.05f, 0.06f,0.70f, 0.56f,0.70f, UP,UP, 0.42f,0.05f, 0.42f,0.96f };
static const float G_5[]      = { 0.52f,0.05f, 0.14f,0.05f, 0.11f,0.44f, 0.34f,0.38f,
                                  0.54f,0.54f, 0.50f,0.84f, 0.28f,0.97f, 0.08f,0.90f };
static const float G_6[]      = { 0.48f,0.08f, 0.24f,0.10f, 0.10f,0.42f, 0.10f,0.80f,
                                  0.26f,0.96f, 0.46f,0.92f, 0.52f,0.70f, 0.34f,0.56f, 0.12f,0.62f };
static const float G_7[]      = { 0.06f,0.05f, 0.56f,0.05f, 0.26f,0.96f };
static const float G_8[]      = { 0.30f,0.05f, 0.13f,0.16f, 0.16f,0.38f, 0.32f,0.50f,
                                  0.50f,0.62f, 0.50f,0.86f, 0.30f,0.96f, 0.11f,0.86f,
                                  0.11f,0.62f, 0.32f,0.50f, 0.48f,0.38f, 0.48f,0.16f, 0.30f,0.05f };
static const float G_9[]      = { 0.12f,0.92f, 0.36f,0.90f, 0.52f,0.60f, 0.52f,0.22f,
                                  0.36f,0.05f, 0.16f,0.09f, 0.09f,0.30f, 0.26f,0.44f, 0.50f,0.38f };
static const float G_COLON[]  = { 0.14f,0.32f, 0.14f,0.42f, UP,UP, 0.14f,0.88f, 0.14f,0.98f };
static const float G_SEMI[]   = { 0.16f,0.32f, 0.16f,0.42f, UP,UP, 0.18f,0.86f, 0.16f,1.00f, 0.06f,1.12f };
static const float G_LT[]     = { 0.46f,0.22f, 0.08f,0.56f, 0.46f,0.90f };
static const float G_EQ[]     = { 0.05f,0.42f, 0.51f,0.42f, UP,UP, 0.05f,0.68f, 0.51f,0.68f };
static const float G_GT[]     = { 0.08f,0.22f, 0.46f,0.56f, 0.08f,0.90f };
static const float G_QUEST[]  = { 0.08f,0.22f, 0.20f,0.05f, 0.42f,0.08f, 0.48f,0.28f,
                                  0.28f,0.48f, 0.28f,0.68f, UP,UP, 0.28f,0.88f, 0.28f,1.00f };
static const float G_AT[]     = { 0.52f,0.44f, 0.34f,0.38f, 0.28f,0.56f, 0.40f,0.68f,
                                  0.52f,0.60f, 0.52f,0.34f, 0.36f,0.06f, 0.16f,0.14f,
                                  0.06f,0.44f, 0.10f,0.82f, 0.32f,0.96f, 0.54f,0.90f };
static const float G_A[]      = { 0.05f,0.96f, 0.31f,0.04f, 0.57f,0.96f, UP,UP, 0.15f,0.66f, 0.47f,0.66f };
static const float G_B[]      = { 0.10f,0.05f, 0.10f,0.96f, UP,UP, 0.10f,0.05f, 0.42f,0.08f,
                                  0.52f,0.28f, 0.40f,0.48f, 0.10f,0.50f, UP,UP, 0.40f,0.48f,
                                  0.55f,0.68f, 0.46f,0.92f, 0.10f,0.96f };
static const float G_C[]      = { 0.55f,0.20f, 0.36f,0.05f, 0.16f,0.16f, 0.08f,0.50f,
                                  0.14f,0.84f, 0.34f,0.97f, 0.55f,0.84f };
static const float G_D[]      = { 0.10f,0.05f, 0.10f,0.96f, UP,UP, 0.10f,0.05f, 0.40f,0.10f,
                                  0.54f,0.36f, 0.54f,0.66f, 0.40f,0.92f, 0.10f,0.96f };
static const float G_E[]      = { 0.52f,0.05f, 0.10f,0.05f, 0.10f,0.96f, 0.52f,0.96f,
                                  UP,UP, 0.10f,0.50f, 0.42f,0.50f };
static const float G_F[]      = { 0.52f,0.05f, 0.10f,0.05f, 0.10f,0.96f, UP,UP, 0.10f,0.50f, 0.42f,0.50f };
static const float G_G[]      = { 0.55f,0.20f, 0.36f,0.05f, 0.16f,0.16f, 0.08f,0.50f,
                                  0.14f,0.84f, 0.34f,0.97f, 0.54f,0.86f, 0.54f,0.56f, 0.36f,0.56f };
static const float G_H[]      = { 0.10f,0.05f, 0.10f,0.96f, UP,UP, 0.54f,0.05f, 0.54f,0.96f,
                                  UP,UP, 0.10f,0.50f, 0.54f,0.50f };
static const float G_I[]      = { 0.08f,0.05f, 0.40f,0.05f, UP,UP, 0.24f,0.05f, 0.24f,0.96f,
                                  UP,UP, 0.08f,0.96f, 0.40f,0.96f };
static const float G_J[]      = { 0.48f,0.05f, 0.48f,0.78f, 0.36f,0.95f, 0.18f,0.92f, 0.12f,0.74f };
static const float G_K[]      = { 0.10f,0.05f, 0.10f,0.96f, UP,UP, 0.52f,0.05f, 0.12f,0.56f,
                                  UP,UP, 0.26f,0.42f, 0.54f,0.96f };
static const float G_L[]      = { 0.12f,0.05f, 0.12f,0.96f, 0.52f,0.96f };
static const float G_M[]      = { 0.08f,0.96f, 0.08f,0.05f, 0.36f,0.62f, 0.64f,0.05f, 0.64f,0.96f };
static const float G_N[]      = { 0.10f,0.96f, 0.10f,0.05f, 0.54f,0.96f, 0.54f,0.05f };
static const float G_O[]      = { 0.08f,0.32f, 0.22f,0.06f, 0.42f,0.06f, 0.56f,0.32f,
                                  0.56f,0.70f, 0.42f,0.96f, 0.22f,0.96f, 0.08f,0.70f, 0.08f,0.32f };
static const float G_P[]      = { 0.10f,0.96f, 0.10f,0.05f, 0.42f,0.08f, 0.53f,0.30f,
                                  0.42f,0.52f, 0.10f,0.54f };
static const float G_Q[]      = { 0.08f,0.32f, 0.22f,0.06f, 0.42f,0.06f, 0.56f,0.32f,
                                  0.56f,0.70f, 0.42f,0.96f, 0.22f,0.96f, 0.08f,0.70f, 0.08f,0.32f,
                                  UP,UP, 0.38f,0.76f, 0.60f,1.06f };
static const float G_R[]      = { 0.10f,0.96f, 0.10f,0.05f, 0.42f,0.08f, 0.53f,0.30f,
                                  0.42f,0.52f, 0.10f,0.54f, UP,UP, 0.30f,0.54f, 0.56f,0.96f };
static const float G_S[]      = { 0.54f,0.18f, 0.36f,0.05f, 0.15f,0.12f, 0.11f,0.32f,
                                  0.30f,0.46f, 0.50f,0.58f, 0.48f,0.84f, 0.28f,0.97f, 0.08f,0.86f };
static const float G_T[]      = { 0.04f,0.05f, 0.56f,0.05f, UP,UP, 0.30f,0.05f, 0.30f,0.96f };
static const float G_U[]      = { 0.10f,0.05f, 0.10f,0.74f, 0.24f,0.95f, 0.42f,0.95f,
                                  0.56f,0.74f, 0.56f,0.05f };
static const float G_V[]      = { 0.05f,0.05f, 0.31f,0.96f, 0.57f,0.05f };
static const float G_W[]      = { 0.04f,0.05f, 0.20f,0.96f, 0.36f,0.34f, 0.52f,0.96f, 0.68f,0.05f };
static const float G_X[]      = { 0.08f,0.05f, 0.54f,0.96f, UP,UP, 0.54f,0.05f, 0.08f,0.96f };
static const float G_Y[]      = { 0.06f,0.05f, 0.31f,0.50f, 0.56f,0.05f, UP,UP, 0.31f,0.50f, 0.31f,0.96f };
static const float G_Z[]      = { 0.06f,0.05f, 0.56f,0.05f, 0.06f,0.96f, 0.56f,0.96f };

typedef struct { const float *pts; int npts; float adv; } sglyph_t;
#define G(p, a) { p, (int)(sizeof p / sizeof(float)) / 2, a }

/* ASCII 32..90. */
static const sglyph_t GLYPHS[] = {
    { G_SPACE, 0, 0.34f },      /*   */ G(G_BANG,  0.26f),  G(G_QUOT,  0.38f),
    G(G_HASH,  0.62f),          G(G_DOLLAR,0.62f),          G(G_PCT,   0.66f),
    G(G_AMP,   0.68f),          G(G_APOS,  0.20f),          G(G_LPAR,  0.40f),
    G(G_RPAR,  0.40f),          G(G_STAR,  0.56f),          G(G_PLUS,  0.58f),
    G(G_COMMA, 0.26f),          G(G_MINUS, 0.58f),          G(G_DOT,   0.26f),
    G(G_SLASH, 0.50f),
    G(G_0, 0.64f), G(G_1, 0.60f), G(G_2, 0.64f), G(G_3, 0.64f), G(G_4, 0.64f),
    G(G_5, 0.64f), G(G_6, 0.62f), G(G_7, 0.62f), G(G_8, 0.62f), G(G_9, 0.62f),
    G(G_COLON, 0.26f),          G(G_SEMI,  0.26f),          G(G_LT,    0.54f),
    G(G_EQ,    0.58f),          G(G_GT,    0.54f),          G(G_QUEST, 0.58f),
    G(G_AT,    0.66f),
    G(G_A, 0.64f), G(G_B, 0.64f), G(G_C, 0.64f), G(G_D, 0.64f), G(G_E, 0.60f),
    G(G_F, 0.58f), G(G_G, 0.66f), G(G_H, 0.66f), G(G_I, 0.48f), G(G_J, 0.58f),
    G(G_K, 0.64f), G(G_L, 0.58f), G(G_M, 0.74f), G(G_N, 0.66f), G(G_O, 0.66f),
    G(G_P, 0.62f), G(G_Q, 0.68f), G(G_R, 0.64f), G(G_S, 0.62f), G(G_T, 0.62f),
    G(G_U, 0.66f), G(G_V, 0.64f), G(G_W, 0.76f), G(G_X, 0.62f), G(G_Y, 0.62f),
    G(G_Z, 0.62f),
};
#define GLYPH_FIRST 32
#define GLYPH_COUNT ((int)(sizeof GLYPHS / sizeof GLYPHS[0]))

void uio_stroke_font_default(uio_stroke_font_t *f)
{
    if (!f) return;
    f->weight   = 0.09f;
    f->tracking = 0.06f;
}

/* Squared distance from p to segment ab. */
static float seg_dist2(float px, float py, float ax, float ay, float bx, float by)
{
    float vx = bx - ax, vy = by - ay;
    float wx = px - ax, wy = py - ay;
    float vv = vx * vx + vy * vy;
    float t = (vv > 0.0f) ? (wx * vx + wy * vy) / vv : 0.0f;
    if (t < 0.0f) t = 0.0f; else if (t > 1.0f) t = 1.0f;
    float dx = wx - t * vx, dy = wy - t * vy;
    return dx * dx + dy * dy;
}

#define SS 4                            /* 4x4 samples per pixel */
#define MAXDIM (UIO_STROKE_MAX_PX * 2)

int uio_stroke_rasterize(void *ctx, uint32_t codepoint, int px_size, int phase,
                         uio_glyph_bmp_t *out)
{
    static uint8_t bitmap[MAXDIM * MAXDIM];
    const uio_stroke_font_t *f = (const uio_stroke_font_t *)ctx;
    if (!f || !out || px_size <= 0 || px_size > UIO_STROKE_MAX_PX) return -1;

    /* Lowercase folds to small caps: the same outline at x-height, with the
     * advance scaled to match — so a mixed-case run still exercises multiple
     * glyph sizes and advances through the cache. */
    float caps = 1.0f;
    uint32_t cp = codepoint;
    if (cp >= 'a' && cp <= 'z') { cp -= 32; caps = 0.78f; }
    if ((int)cp < GLYPH_FIRST || (int)cp >= GLYPH_FIRST + GLYPH_COUNT) return -1;
    const sglyph_t *g = &GLYPHS[cp - GLYPH_FIRST];

    const float scale = (float)px_size * caps;
    const float half  = f->weight * (float)px_size * 0.5f;
    const float xoff  = (phase > 0) ? (float)phase / 4.0f : 0.0f;   /* phases <= 4 */

    memset(out, 0, sizeof *out);
    out->advance = (int)(g->adv * scale + f->tracking * (float)px_size + 0.5f);
    if (g->npts == 0) return 0;                                     /* space */

    /*  Points in pixel space, y measured from the BASELINE (negative above),
     *  which is the metric convention glyph_cache.h expects back. */
    static float xs[256], ys[256];
    int n = g->npts > 256 ? 256 : g->npts;
    float minx = 1e30f, maxx = -1e30f, miny = 1e30f, maxy = -1e30f;
    for (int i = 0; i < n; i++) {
        float gx = g->pts[i * 2], gy = g->pts[i * 2 + 1];
        if (gx == UP) { xs[i] = UP; ys[i] = UP; continue; }
        /* the cap line sits `scale` above the baseline; y=1 IS the baseline */
        xs[i] = gx * scale + xoff;
        ys[i] = (gy - 1.0f) * scale;
        if (xs[i] < minx) minx = xs[i];
        if (xs[i] > maxx) maxx = xs[i];
        if (ys[i] < miny) miny = ys[i];
        if (ys[i] > maxy) maxy = ys[i];
    }
    if (minx > maxx) return 0;

    int x0 = (int)floorf(minx - half), x1 = (int)ceilf(maxx + half);
    int y0 = (int)floorf(miny - half), y1 = (int)ceilf(maxy + half);
    int w = x1 - x0, h = y1 - y0;
    if (w <= 0 || h <= 0) return 0;
    if (w > MAXDIM) w = MAXDIM;
    if (h > MAXDIM) h = MAXDIM;

    const float half2 = half * half;
    for (int py = 0; py < h; py++) {
        for (int px = 0; px < w; px++) {
            int inside = 0;
            for (int sy = 0; sy < SS; sy++) {
                float fy = (float)(y0 + py) + ((float)sy + 0.5f) / (float)SS;
                for (int sx = 0; sx < SS; sx++) {
                    float fx = (float)(x0 + px) + ((float)sx + 0.5f) / (float)SS;
                    for (int i = 0; i + 1 < n; i++) {
                        if (xs[i] == UP || xs[i + 1] == UP) continue;
                        if (seg_dist2(fx, fy, xs[i], ys[i], xs[i + 1], ys[i + 1]) <= half2) {
                            inside++;
                            goto next_sample;
                        }
                    }
next_sample:        ;
                }
            }
            bitmap[py * w + px] = (uint8_t)((inside * 255 + (SS * SS) / 2) / (SS * SS));
        }
    }

    out->cov = bitmap;
    out->pitch = w;
    out->w = w;
    out->h = h;
    out->bearing_x = x0;
    out->bearing_y = -y0;            /* rows from the baseline up to the top */
    return 0;
}
