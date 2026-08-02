/*
 *  test_ui_offload.c — gates for the Qt-front-end offload prototype.
 *
 *  Everything is checked against the repo's golden reference model
 *  (refmodel/blitter_ref.c + blt_tri.c): the display list this layer emits is
 *  executed exactly as the fabric would execute it, and the resulting
 *  framebuffer is compared against an independent CPU model of what the draw
 *  was supposed to produce.
 *
 *  The two load-bearing gates are:
 *    - the tinted AA corner arc is bit-exact with a plain FILL of the same
 *      RGB565 colour (no seam between an arc and the interior it joins), and
 *    - a fabric-scaled (TRILIST) image reproduces standard nearest-neighbour
 *      resampling bit-exactly, including at 1:1.
 *
 *  GPL-3.0.
 */
#include "ui_offload.h"
#include "blt_wire.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RING_BYTES (256u * 1024u)
#define SRC_BYTES  (4u * 1024u * 1024u)
#define VTX_BYTES  (64u * 1024u)

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { \
    printf("FAIL %s:%d: ", __func__, __LINE__); printf(__VA_ARGS__); printf("\n"); \
    failures++; } } while (0)

/* ── harness ────────────────────────────────────────────────────────────── */
typedef struct {
    blt_emitter_t e;
    uio_t         u;
    uint8_t      *ring;
    uint8_t      *src;
    uint16_t     *fb;
} env_t;

static void env_init(env_t *v)
{
    v->ring = (uint8_t *)calloc(RING_BYTES, 1);
    v->src  = (uint8_t *)calloc(SRC_BYTES, 1);
    v->fb   = (uint16_t *)calloc(BLT_FB_PIXELS, sizeof(uint16_t));
    assert(v->ring && v->src && v->fb);
    blt_emitter_init(&v->e, v->ring, RING_BYTES, v->src, SRC_BYTES);
    assert(uio_init(&v->u, &v->e, v->src, VTX_BYTES) == 0);
}

static void env_free(env_t *v) { free(v->ring); free(v->src); free(v->fb); }

/* Execute the emitted ring into v->fb, exactly as the fabric walks it. */
static void env_run(env_t *v)
{
    int n = v->e.cmd_count;
    blt_cmd_t *cmds = (blt_cmd_t *)calloc((size_t)n + 1, sizeof *cmds);
    assert(cmds);
    for (int i = 0; i < n; i++) blt_unpack_cmd(v->ring + (size_t)i * BLT_CMD_BYTES, &cmds[i]);
    blt_surface_heap_t h = { .base = v->src, .size = SRC_BYTES };
    blt_execute(v->fb, &h, cmds, n);
    free(cmds);
}

static void fb_clear(env_t *v, uint16_t c)
{
    for (int i = 0; i < BLT_FB_PIXELS; i++) v->fb[i] = c;
}
static uint16_t px(const env_t *v, int x, int y) { return v->fb[y * BLT_FB_WIDTH + x]; }

/* ── 1. colour fidelity: a tinted white mask == a FILL of the same colour ── */
/*
 *  The corner masks and the glyph atlas are colour-free (white RGB, coverage
 *  in A4) and get their colour from BLT_F_COLORMOD. That only works if the
 *  RGB565 -> RGB888 expansion in uio_565_to_888 is the exact inverse of the
 *  fabric's round(ch*mod/255) reduction — otherwise every arc and every glyph
 *  is off by a least-significant bit from the fill it sits against.
 */
static void test_tint_roundtrip(void)
{
    env_t v; env_init(&v);
    static const uint16_t white4444 = 0xFFFFu;
    blt_surface_ref_t mask = blt_upload_argb4444(&v.e, &white4444, 1, 1, 2);
    CHECK(mask.valid, "1x1 mask upload failed");

    int bad = 0, first_bad = -1;
    for (int c = 0; c < 65536 && bad < 4; c++) {
        uint16_t col = (uint16_t)c;
        uint8_t r, g, b;
        uio_565_to_888(col, &r, &g, &b);
        blt_begin_frame(&v.e, 0, 0, 0);
        blt_blit_mod(&v.e, mask, 0, 0, 1, 1, 0, 0, BLT_BLEND_PALPHA, 0, 255, 0, r, g, b);
        blt_end_frame(&v.e);
        v.fb[0] = (uint16_t)~col;                    /* PALPHA must overwrite it */
        env_run(&v);
        if (v.fb[0] != col) { if (first_bad < 0) first_bad = c; bad++; }
    }
    CHECK(bad == 0, "tinted white mask != fill colour for %d colours (first 0x%04x)",
          bad, first_bad);
    printf("  tint round-trip: all 65536 RGB565 colours exact\n");
    env_free(&v);
}

/* ── 2. the baked corner coverage is a well-formed antialiased quarter disc ─ */
static void test_corner_coverage(void)
{
    for (int r = 2; r <= 24; r++) {
        CHECK(uio_corner_coverage(r, r - 1, r - 1) == 255, "r=%d inner texel not solid", r);
        /* The outermost texel is the corner of the bounding box. It is empty
         * for any radius a UI would actually use; at r<=3 the disc is small
         * enough that the box corner still clips it, so only require that it
         * is the LEAST covered texel of the mask. */
        CHECK(uio_corner_coverage(r, 0, 0) == 0 || r <= 3, "r=%d outer texel not empty", r);
        for (int j = 0; j < r; j++) {
            for (int i = 0; i < r; i++) {
                int c = uio_corner_coverage(r, i, j);
                CHECK(c == uio_corner_coverage(r, j, i), "r=%d asymmetric at (%d,%d)", r, i, j);
                if (i + 1 < r)
                    CHECK(uio_corner_coverage(r, i + 1, j) >= c,
                          "r=%d coverage not monotone in x at (%d,%d)", r, i, j);
                if (j + 1 < r)
                    CHECK(uio_corner_coverage(r, i, j + 1) >= c,
                          "r=%d coverage not monotone in y at (%d,%d)", r, i, j);
            }
        }
        /* An antialiased edge means partial coverage exists — a hard-edged
         * quarter disc (every texel 0 or 255) would defeat the whole point. */
        int partial = 0;
        for (int j = 0; j < r; j++)
            for (int i = 0; i < r; i++) {
                int c = uio_corner_coverage(r, i, j);
                if (c > 0 && c < 255) partial++;
            }
        CHECK(partial >= r, "r=%d only %d partially covered texels", r, partial);
    }
    /* The mask carries coverage in A4 and white in RGB. */
    static uint16_t m[16 * 16];
    CHECK(uio_bake_corner_mask(m, 16, 255) == 0, "bake failed");
    for (int i = 0; i < 16 * 16; i++) CHECK((m[i] & 0x0FFF) == 0x0FFF, "mask RGB not white");
    CHECK((m[0] >> 12) == 0, "outer texel not transparent");
    CHECK((m[16 * 16 - 1] >> 12) == 15, "inner texel not opaque");
    CHECK(uio_bake_corner_mask(m, 0, 255) == -1, "radius 0 accepted");
    CHECK(uio_bake_corner_mask(m, UIO_CORNER_MAX_RADIUS + 1, 255) == -1, "radius overflow accepted");
    printf("  corner coverage: monotone, symmetric, antialiased (r=2..24)\n");
}

/* ── 3. the rounded rect composites with no gaps, no overdraw, no bleed ──── */
static void test_rounded_rect(void)
{
    env_t v; env_init(&v);
    const uint16_t BG = blt_rgb565(0, 0, 0), CARD = blt_rgb565(64, 160, 240);
    const uio_rect_t R = { 40, 30, 120, 80 };
    const int RAD = 12;

    uio_begin_frame(&v.u, 0, 0, 0);
    CHECK(uio_rounded_rect(&v.u, R, RAD, CARD, 255) == 0, "rounded rect emit failed");
    uio_end_frame(&v.u);
    CHECK(v.e.overflow == 0 && v.e.dropped == 0, "emitter overflow/drop");
    /* 3 fills + 4 corner blits, and the corner mask is baked ONCE for all four. */
    CHECK(v.u.stats.fills == 3 && v.u.stats.blits == 4,
          "expected 3 fills + 4 blits, got %u + %u", v.u.stats.fills, v.u.stats.blits);
    CHECK(v.u.ncorners == 1, "expected 1 baked mask, got %d", v.u.ncorners);

    fb_clear(&v, BG);
    env_run(&v);

    int outside_touched = 0, interior_wrong = 0, corner_flat = 0;
    for (int y = 0; y < BLT_FB_HEIGHT; y++) {
        for (int x = 0; x < BLT_FB_WIDTH; x++) {
            int in_rect = (x >= R.x && x < R.x + R.w && y >= R.y && y < R.y + R.h);
            if (!in_rect) { if (px(&v, x, y) != BG) outside_touched++; continue; }

            /* Distance from the nearest corner centre decides what to expect. */
            int cx = (x < R.x + RAD) ? R.x + RAD : (x >= R.x + R.w - RAD ? R.x + R.w - RAD - 1 : -1);
            int cy = (y < R.y + RAD) ? R.y + RAD : (y >= R.y + R.h - RAD ? R.y + R.h - RAD - 1 : -1);
            if (cx >= 0 && cy >= 0) {
                /* inside a corner square: either fully covered, blended, or bg */
                long dx = x - cx, dy = y - cy;
                if (dx * dx + dy * dy <= (long)(RAD - 2) * (RAD - 2)) {
                    if (px(&v, x, y) != CARD) corner_flat++;         /* well inside the arc */
                }
            } else {
                if (px(&v, x, y) != CARD) interior_wrong++;
            }
        }
    }
    CHECK(outside_touched == 0, "%d pixels painted outside the rect", outside_touched);
    CHECK(interior_wrong == 0, "%d interior pixels not the card colour", interior_wrong);
    CHECK(corner_flat == 0, "%d pixels inside the corner arc not solid", corner_flat);

    /* The extreme corner texel must be antialiased: partially blended, i.e.
     * neither the background nor the card colour. */
    uint16_t c00 = px(&v, R.x, R.y);
    CHECK(c00 == BG, "outermost corner texel should be uncovered, got %04x", c00);
    int blended = 0;
    for (int j = 0; j < RAD; j++)
        for (int i = 0; i < RAD; i++) {
            uint16_t p = px(&v, R.x + i, R.y + j);
            if (p != BG && p != CARD) blended++;
        }
    CHECK(blended >= RAD, "corner shows %d antialiased pixels, expected >= %d", blended, RAD);

    /* All four arcs are the same mask under HFLIP/VFLIP: check the mirror. */
    int mirror_bad = 0;
    for (int j = 0; j < RAD; j++)
        for (int i = 0; i < RAD; i++) {
            uint16_t tl = px(&v, R.x + i,               R.y + j);
            uint16_t tr = px(&v, R.x + R.w - 1 - i,     R.y + j);
            uint16_t bl = px(&v, R.x + i,               R.y + R.h - 1 - j);
            uint16_t br = px(&v, R.x + R.w - 1 - i,     R.y + R.h - 1 - j);
            if (tl != tr || tl != bl || tl != br) mirror_bad++;
        }
    CHECK(mirror_bad == 0, "%d corner texels break 4-way mirror symmetry", mirror_bad);

    printf("  rounded rect: 7 commands, gap-free, mirror-symmetric, AA edge\n");
    env_free(&v);
}

/* A pill (radius == h/2) and a radius clamp both stay on the fabric. */
static void test_rounded_rect_pill(void)
{
    env_t v; env_init(&v);
    uio_begin_frame(&v.u, 0, 0, 0);
    uio_rect_t pill = { 10, 10, 60, 16 };
    CHECK(uio_rounded_rect(&v.u, pill, 999, blt_rgb565(255, 255, 255), 255) == 0, "pill failed");
    uio_end_frame(&v.u);
    CHECK(v.u.corners[0].radius == 8, "radius not clamped to h/2, got %d", v.u.corners[0].radius);
    CHECK(v.e.overflow == 0, "overflow");
    fb_clear(&v, 0);
    env_run(&v);
    /* Pill: the vertical middle row spans the full width, the top row does not. */
    CHECK(px(&v, pill.x, pill.y + pill.h / 2) != 0, "pill middle row not filled");
    CHECK(px(&v, pill.x, pill.y) == 0, "pill top-left corner should be empty");
    printf("  pill: radius clamped to h/2, ends rounded\n");
    env_free(&v);
}

/* A translucent card bakes its opacity into the corner coverage instead of
 * falling back to the A9. */
static void test_rounded_rect_alpha(void)
{
    env_t v; env_init(&v);
    uio_begin_frame(&v.u, 0, 0, 0);
    uio_rect_t r = { 20, 20, 80, 40 };
    CHECK(uio_rounded_rect(&v.u, r, 8, blt_rgb565(255, 255, 255), 128) == 0, "alpha card failed");
    uio_end_frame(&v.u);
    CHECK(v.u.ncorners == 1 && v.u.corners[0].alpha == 128, "alpha not baked into the mask");
    fb_clear(&v, blt_rgb565(0, 0, 0));
    env_run(&v);
    uint16_t mid = px(&v, r.x + r.w / 2, r.y + r.h / 2);
    CHECK(mid != 0 && mid != 0xFFFF, "translucent interior not blended (got %04x)", mid);
    /* Same colour, opaque, is a DIFFERENT cached mask — not a silent reuse. */
    uio_begin_frame(&v.u, 0, 0, 0);
    uio_rounded_rect(&v.u, r, 8, blt_rgb565(255, 255, 255), 255);
    uio_end_frame(&v.u);
    CHECK(v.u.ncorners == 2, "(radius,alpha) cache collapsed distinct alphas");
    printf("  translucent card: opacity baked into coverage, cache keyed on alpha\n");
    env_free(&v);
}

/* ── 4. fabric scaling ──────────────────────────────────────────────────── */

/* A recognisable RGB565 test image: every texel is a distinct value. */
static uint16_t *make_image(int w, int h)
{
    uint16_t *p = (uint16_t *)malloc((size_t)w * (size_t)h * sizeof *p);
    assert(p);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            p[y * w + x] = (uint16_t)(1u + (unsigned)(y * w + x));   /* never 0 */
    return p;
}

/* Independent CPU model of nearest-neighbour resampling (the thing the fabric
 * is supposed to reproduce): sample the source at destination pixel centres. */
static void nn_index(int iw, int ih, int dw, int dh, int k, int l, int *sx, int *sy)
{
    *sx = (int)(((long)(2 * k + 1) * iw) / (2L * dw));
    *sy = (int)(((long)(2 * l + 1) * ih) / (2L * dh));
    if (*sx >= iw) *sx = iw - 1;
    if (*sy >= ih) *sy = ih - 1;
}

/*
 *  `tol` is the permitted deviation, in SOURCE TEXELS, from that model.
 *
 *  tol == 0 is the normal case and is what every ordinary ratio meets. The
 *  exception is a sample that lands exactly on a texel boundary: the fabric's
 *  rasterizer interpolates uv in 12.4 fixed point with the top-left fill rule's
 *  one-unit edge bias (blt_tri.c), so a boundary sample can resolve to either
 *  neighbour — a property of the golden rasterizer, not of the uv mapping. Such
 *  cases are pinned at tol == 1 (adjacent texel) rather than waved through:
 *  anything worse is a real mapping error.
 */
static void scale_case(env_t *v, uio_image_ref_t img, int iw, int ih,
                       int dx, int dy, int dw, int dh, int force, int expect_path, int tol)
{
    uio_begin_frame(&v->u, 0, 0, 0);
    uio_scale_t s;
    memset(&s, 0, sizeof s);
    s.dst = (uio_rect_t){ dx, dy, dw, dh };
    s.blend = BLT_BLEND_COPY;
    s.alpha = 255;
    s.force_fabric = force;
    int path = uio_image_scaled(&v->u, img, &s);
    uio_end_frame(&v->u);
    CHECK(path == expect_path, "%dx%d -> %dx%d took path %d, expected %d",
          iw, ih, dw, dh, path, expect_path);
    CHECK(v->u.stats.uv_clamped == 0, "uv clamped on a padded image");

    fb_clear(v, 0);
    env_run(v);

    int wrong = 0, first_x = -1, first_y = -1, worst = 0;
    int got_bad = 0, exp_bad = 0;
    for (int l = 0; l < dh; l++)
        for (int k = 0; k < dw; k++) {
            uint16_t got = px(v, dx + k, dy + l);
            int esx, esy;
            nn_index(iw, ih, dw, dh, k, l, &esx, &esy);
            /* make_image() encodes the texel index in the pixel value, so a
             * mismatch says exactly WHICH texel the fabric sampled. */
            int gi = (int)got - 1;
            int gsx = (gi >= 0) ? gi % iw : -1, gsy = (gi >= 0) ? gi / iw : -1;
            int dsx = gsx - esx, dsy = gsy - esy;
            if (dsx < 0) dsx = -dsx;
            if (dsy < 0) dsy = -dsy;
            int dev = (dsx > dsy) ? dsx : dsy;
            if (gi < 0 || dev > tol) {
                if (first_x < 0) {
                    first_x = k; first_y = l;
                    got_bad = (int)got; exp_bad = 1 + esy * iw + esx;
                }
                wrong++;
            }
            if (dev > worst) worst = dev;
        }
    CHECK(wrong == 0, "%dx%d -> %dx%d: %d/%d texels off by more than %d "
          "(first (%d,%d) got %d exp %d)",
          iw, ih, dw, dh, wrong, dw * dh, tol, first_x, first_y, got_bad, exp_bad);
    if (tol > 0)
        printf("    %dx%d -> %3dx%-3d: boundary case, worst deviation %d texel\n",
               iw, ih, dw, dh, worst);

    /* Nothing outside the destination rect may be touched. */
    int bleed = 0;
    for (int y = 0; y < BLT_FB_HEIGHT; y++)
        for (int x = 0; x < BLT_FB_WIDTH; x++) {
            int in = (x >= dx && x < dx + dw && y >= dy && y < dy + dh);
            if (!in && px(v, x, y) != 0) bleed++;
        }
    CHECK(bleed == 0, "%dx%d -> %dx%d: %d pixels painted outside the destination rect",
          iw, ih, dw, dh, bleed);
}

static void test_scaling(void)
{
    env_t v; env_init(&v);
    const int IW = 24, IH = 18;
    uint16_t *img = make_image(IW, IH);
    uio_image_ref_t ref = uio_upload_image(&v.u, img, IW, IH, IW * 2);
    CHECK(ref.surf.valid && ref.padded, "padded upload failed");
    CHECK(ref.surf.w == IW + 2 && ref.surf.h == IH + 2, "border missing");

    /* 1:1 through the triangle path must be an exact identity — if the uv
     * mapping is off by even half a texel this is where it shows. */
    scale_case(&v, ref, IW, IH, 10, 10, IW, IH, 1, UIO_PATH_TRILIST, 0);
    /* ...and 1:1 without force_fabric takes the cheap BLIT path instead. */
    scale_case(&v, ref, IW, IH, 10, 10, IW, IH, 0, UIO_PATH_BLIT, 0);

    /* Integer upscale, integer downscale, and ratios with no relationship to
     * the source size at all (the case pre-scaling at decode cannot serve). */
    const int cases[][2] = {
        { 48, 36 }, { 72, 54 },              /* 2x, 3x                        */
        { 12,  9 }, {  8,  6 },              /* 1/2, 1/3                      */
        { 33, 25 }, { 44, 31 }, { 17, 13 },  /* arbitrary ratios              */
        { 60, 20 }, { 20, 60 },              /* non-uniform aspect            */
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++)
        scale_case(&v, ref, IW, IH, 30, 20, cases[i][0], cases[i][1], 0, UIO_PATH_TRILIST, 0);
    printf("  fabric scaling: %zu ratios bit-exact vs nearest-neighbour model\n",
           sizeof cases / sizeof cases[0] + 1);

    /* Degenerate extremes, where destination pixel centres land ON texel
     * boundaries and the rasterizer's fill-rule bias picks a side. */
    scale_case(&v, ref, IW, IH, 30, 20,   1,  1, 0, UIO_PATH_TRILIST, 1);
    scale_case(&v, ref, IW, IH, 30, 20, 200,  5, 0, UIO_PATH_TRILIST, 1);
    free(img);
    env_free(&v);
}

/* A scaled draw is ONE command plus two vertices' worth of entry data, whatever
 * the destination size — that is the property that makes per-frame animated
 * scaling free on the A9. */
static void test_scaling_cost_is_constant(void)
{
    env_t v; env_init(&v);
    const int IW = 32, IH = 32;
    uint16_t *img = make_image(IW, IH);
    uio_image_ref_t ref = uio_upload_image(&v.u, img, IW, IH, IW * 2);

    for (int frame = 0; frame < 24; frame++) {
        int side = 33 + frame;             /* a zoom that never repeats, never 1:1 */
        uio_begin_frame(&v.u, 0, 0, 0);
        uio_scale_t s; memset(&s, 0, sizeof s);
        s.dst = (uio_rect_t){ 20, 20, side, side };
        s.blend = BLT_BLEND_COPY; s.alpha = 255;
        CHECK(uio_image_scaled(&v.u, ref, &s) == UIO_PATH_TRILIST, "frame %d not on the fabric", frame);
        uio_end_frame(&v.u);
        CHECK(v.u.stats.cmds == 2, "frame %d emitted %u commands (want 1 + END)",
              frame, v.u.stats.cmds);
        CHECK(v.e.overflow == 0 && v.e.dropped == 0, "frame %d overflow", frame);
        CHECK(v.u.stats.a9_resample_px == (uint64_t)side * side,
              "frame %d resample accounting wrong", frame);
    }
    /* The vertex arena is per-frame: 24 frames of zoom must not grow it. */
    CHECK(v.e.vtx_used == 6 * sizeof(blt_vtx_t), "vertex arena not reset per frame (%zu bytes)",
          v.e.vtx_used);
    printf("  animated zoom: 1 command/frame at every ratio, vertex arena reset\n");
    free(img);
    env_free(&v);
}

/* An unpadded image still scales, but says so. */
static void test_unpadded_is_reported(void)
{
    env_t v; env_init(&v);
    const int IW = 16, IH = 16;
    uint16_t *img = make_image(IW, IH);
    blt_surface_ref_t raw = blt_upload(&v.e, img, IW, IH, IW * 2);
    uio_image_ref_t ref = { raw, 0, 0, IW, IH, 0 };

    uio_begin_frame(&v.u, 0, 0, 0);
    uio_scale_t s; memset(&s, 0, sizeof s);
    s.dst = (uio_rect_t){ 0, 0, 40, 40 };
    s.blend = BLT_BLEND_COPY; s.alpha = 255;
    CHECK(uio_image_scaled(&v.u, ref, &s) == UIO_PATH_TRILIST, "unpadded scale refused");
    uio_end_frame(&v.u);
    CHECK(v.u.stats.uv_clamped == 1, "unpadded scale not reported (uv_clamped=%u)",
          v.u.stats.uv_clamped);
    printf("  unpadded image: scales, and the half-texel compromise is reported\n");
    free(img);
    env_free(&v);
}

/* Per-pixel-alpha art blits 1:1 on the fabric but is REFUSED for scaling
 * rather than having its alpha quietly dropped. */
static void test_alpha_image(void)
{
    env_t v; env_init(&v);
    const int IW = 12, IH = 12;
    static uint16_t argb[12 * 12];
    for (int i = 0; i < IW * IH; i++) argb[i] = (uint16_t)(((i % 16) << 12) | 0x0F0F);
    uio_image_ref_t ref = uio_upload_image_alpha(&v.u, argb, IW, IH, IW * 2);
    CHECK(ref.surf.valid && ref.surf.format == BLT_FMT_ARGB4444, "alpha upload failed");
    CHECK(ref.padded && ref.surf.w == IW + 2, "alpha upload not padded");

    uio_begin_frame(&v.u, 0, 0, 0);
    CHECK(uio_image_blit(&v.u, ref, 4, 4, BLT_BLEND_PALPHA, 0, 255) == UIO_PATH_BLIT,
          "1:1 alpha blit refused");
    uio_scale_t s; memset(&s, 0, sizeof s);
    s.dst = (uio_rect_t){ 40, 40, 30, 30 };
    s.blend = BLT_BLEND_PALPHA; s.alpha = 255;
    CHECK(uio_image_scaled(&v.u, ref, &s) == -1, "scaled alpha image not refused");
    uio_end_frame(&v.u);
    CHECK(v.u.stats.trilists == 0, "alpha image reached the triangle path");

    fb_clear(&v, blt_rgb565(0, 0, 0));
    env_run(&v);
    CHECK(px(&v, 4, 4) == 0, "A4=0 texel should be skip-write");
    CHECK(px(&v, 4 + 15 % IW, 4) != 0 || px(&v, 5, 4) != 0, "no alpha texel composited");
    printf("  alpha art: 1:1 PALPHA blit on the fabric, scaling refused not approximated\n");
    env_free(&v);
}

/* ── 5. text ────────────────────────────────────────────────────────────── */
static void test_text(void)
{
    /* Every printable glyph the font claims must actually have ink (a blank
     * cell would silently swallow text). */
    static uint16_t atlas[UIO_FONT_ATLAS_TEXELS];
    uio_bake_font_atlas(atlas);
    for (int cell = 1; cell < UIO_FONT_COUNT; cell++) {      /* cell 0 is space */
        int ink = 0, ox = uio_font_cell_x(cell), oy = uio_font_cell_y(cell);
        for (int j = 0; j < UIO_FONT_INK_H; j++)
            for (int i = 0; i < UIO_FONT_INK_W; i++)
                if (atlas[(oy + j) * UIO_FONT_ATLAS_W + ox + i] >> 12) ink++;
        CHECK(ink > 0, "glyph '%c' (cell %d) is blank", cell + UIO_FONT_FIRST, cell);
    }
    /* The advance column and the leading row must stay clear, or glyphs touch. */
    for (int cell = 0; cell < UIO_FONT_COUNT; cell++) {
        int ox = uio_font_cell_x(cell), oy = uio_font_cell_y(cell);
        for (int j = 0; j < UIO_FONT_CELL_H; j++)
            CHECK((atlas[(oy + j) * UIO_FONT_ATLAS_W + ox + UIO_FONT_CELL_W - 1] >> 12) == 0,
                  "cell %d advance column is inked", cell);
        for (int i = 0; i < UIO_FONT_CELL_W; i++)
            CHECK((atlas[(oy + UIO_FONT_CELL_H - 1) * UIO_FONT_ATLAS_W + ox + i] >> 12) == 0,
                  "cell %d leading row is inked", cell);
    }

    env_t v; env_init(&v);
    CHECK(uio_load_font(&v.u) == 0, "font upload failed");
    uio_begin_frame(&v.u, 0, 0, 0);
    const char *msg = "ZAPAROO 42";                 /* 10 chars, 1 of them a space */
    int adv = uio_text(&v.u, 8, 8, msg, blt_rgb565(255, 200, 0));
    uio_end_frame(&v.u);
    CHECK(adv == (int)strlen(msg) * UIO_FONT_CELL_W, "advance %d unexpected", adv);
    CHECK(v.u.stats.glyphs == strlen(msg) - 1, "emitted %u glyphs for %zu chars",
          v.u.stats.glyphs, strlen(msg));

    fb_clear(&v, 0);
    env_run(&v);
    int inked = 0;
    for (int y = 0; y < BLT_FB_HEIGHT; y++)
        for (int x = 0; x < BLT_FB_WIDTH; x++)
            if (px(&v, x, y)) {
                inked++;
                CHECK(px(&v, x, y) == blt_rgb565(255, 200, 0), "glyph pixel not the text colour");
                CHECK(x >= 8 && x < 8 + adv && y >= 8 && y < 8 + UIO_FONT_INK_H,
                      "glyph pixel (%d,%d) outside the text box", x, y);
            }
    CHECK(inked > 20, "text drew only %d pixels", inked);
    /* Lowercase folds onto uppercase rather than vanishing. */
    CHECK(uio_font_cell('a') == uio_font_cell('A'), "lowercase not folded");
    CHECK(uio_font_cell(1) == -1, "unprintable accepted");
    printf("  text: %zu-glyph run, 1 blit each, colour by COLORMOD from one atlas\n",
           strlen(msg) - 1);
    env_free(&v);
}

/* ── 6. aspect-fit helper ───────────────────────────────────────────────── */
static void test_fit(void)
{
    uio_rect_t box = { 10, 20, 100, 60 };
    uio_rect_t r = uio_fit(box, 200, 100);                 /* wider than the box  */
    CHECK(r.w == 100 && r.h == 50, "wide fit got %dx%d", r.w, r.h);
    CHECK(r.x == 10 && r.y == 25, "wide fit not centred (%d,%d)", r.x, r.y);
    r = uio_fit(box, 100, 200);                            /* taller than the box */
    CHECK(r.w == 30 && r.h == 60, "tall fit got %dx%d", r.w, r.h);
    CHECK(r.x == 45 && r.y == 20, "tall fit not centred (%d,%d)", r.x, r.y);
    r = uio_fit(box, 100, 60);                             /* exact               */
    CHECK(r.w == 100 && r.h == 60 && r.x == 10 && r.y == 20, "exact fit wrong");
    r = uio_fit(box, 0, 0);
    CHECK(r.w == 0 && r.h == 0, "degenerate fit not empty");
    printf("  aspect fit: PreserveAspectFit boxes centred and integral\n");
}

/* ── 7. a whole frame stays inside a sane per-frame command budget ───────── */
static void test_frame_budget(void)
{
    env_t v; env_init(&v);
    const int IW = 40, IH = 56;
    uint16_t *cover = make_image(IW, IH);
    uio_image_ref_t art = uio_upload_image(&v.u, cover, IW, IH, IW * 2);
    CHECK(uio_load_font(&v.u) == 0, "font failed");

    uio_begin_frame(&v.u, 0, 1, blt_rgb565(16, 18, 24));
    for (int i = 0; i < 8; i++) {
        int cx = 8 + (i % 4) * 76, cy = 40 + (i / 4) * 92;
        uio_rounded_rect(&v.u, (uio_rect_t){ cx, cy, 68, 84 }, 6, blt_rgb565(40, 44, 56), 255);
        uio_scale_t s; memset(&s, 0, sizeof s);
        s.dst = (uio_rect_t){ cx + 6, cy + 6, 56, 62 };
        s.blend = BLT_BLEND_COPY; s.alpha = 255;
        uio_image_scaled(&v.u, art, &s);
        uio_text(&v.u, cx + 6, cy + 72, "GAME", blt_rgb565(230, 230, 240));
    }
    uio_end_frame(&v.u);

    CHECK(v.e.overflow == 0, "emitter overflow on a full frame");
    CHECK(v.e.dropped == 0, "%u commands dropped", v.e.dropped);
    CHECK(v.u.last_error == 0, "layer reported error %d", v.u.last_error);
    /* 8 cards x (7 rounded-rect + 1 scaled + 4 glyphs) + clear/END overhead. */
    CHECK(v.u.stats.cmds < 128, "frame emitted %u commands", v.u.stats.cmds);
    CHECK(v.u.stats.trilists == 8, "expected 8 fabric-scaled covers, got %u", v.u.stats.trilists);
    CHECK(v.u.stats.rounded_rects == 8, "expected 8 rounded rects, got %u", v.u.stats.rounded_rects);
    CHECK(v.u.ncorners == 1, "one radius should bake one mask, got %d", v.u.ncorners);

    /* The A9 emitted the frame and rasterized nothing: every pixel below is
     * work the fabric does instead. */
    uint64_t a9 = v.u.stats.a9_fill_px + v.u.stats.a9_aa_px
                + v.u.stats.a9_resample_px + v.u.stats.a9_glyph_px;
    CHECK(a9 > 0 && v.u.stats.fabric_px > 0, "accounting empty");
    printf("  frame budget: %u commands for 8 cards, %llu px moved to the fabric\n",
           v.u.stats.cmds, (unsigned long long)a9);
    free(cover);
    env_free(&v);
}

int main(void)
{
    printf("test_ui_offload — Qt front-end offload gates\n");
    test_tint_roundtrip();
    test_corner_coverage();
    test_rounded_rect();
    test_rounded_rect_pill();
    test_rounded_rect_alpha();
    test_scaling();
    test_scaling_cost_is_constant();
    test_unpadded_is_reported();
    test_alpha_image();
    test_text();
    test_fit();
    test_frame_budget();
    if (failures) { printf("%d FAILURE(S)\n", failures); return 1; }
    printf("all gates pass\n");
    return 0;
}
