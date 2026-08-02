/*
 *  ui_offload.c — see ui_offload.h.
 *
 *  Every function here does the same thing: turn one UI primitive into blitter
 *  commands and touch no framebuffer pixels. The only per-pixel loops in this
 *  file run at LOAD time (baking a corner mask, padding an uploaded image);
 *  the per-frame path is pure command emission.
 *
 *  GPL-3.0.
 */
#include "ui_offload.h"
#include <stdlib.h>
#include <string.h>

#define UIO_FX 4                       /* blt_vtx_t fixed-point: 12.4 */

static void note_err(uio_t *u, int err) { if (err && !u->last_error) u->last_error = err; }

/* ────────────────────────────────────────────────────────────────────────
 *  Lifecycle
 * ──────────────────────────────────────────────────────────────────────── */
int uio_init(uio_t *u, blt_emitter_t *e, void *src_base, uint32_t vtx_bytes)
{
    if (!u || !e || !src_base || vtx_bytes == 0) return -1;
    memset(u, 0, sizeof *u);
    u->e = e;
    u->src_base = (uint8_t *)src_base;

    /* The vertex arena is allocated FROM the source heap so a TRILIST header's
     * entry offset (resolved against the heap base) is valid, and so uploads
     * can never land on top of it. */
    uint32_t off = blt_alloc(&e->alloc, vtx_bytes);
    if (off == BLT_ALLOC_FAIL) return -1;
    u->vtx_off   = off;
    u->vtx_bytes = vtx_bytes;
    blt_vtx_buf_init(e, u->src_base + off, vtx_bytes);
    return 0;
}

int uio_load_font(uio_t *u)
{
    if (!u || !u->e) return -1;
    if (u->font_ready) return 0;

    static uint16_t atlas[UIO_FONT_ATLAS_TEXELS];
    uio_bake_font_atlas(atlas);
    u->font = blt_upload_argb4444(u->e, atlas, UIO_FONT_ATLAS_W, UIO_FONT_ATLAS_H,
                                  UIO_FONT_ATLAS_W * 2);
    if (!u->font.valid) { note_err(u, -1); return -1; }
    u->font_ready = 1;
    return 0;
}

int uio_corner(uio_t *u, int radius, uint8_t alpha)
{
    if (!u || !u->e || radius <= 0 || radius > UIO_CORNER_MAX_RADIUS) return -1;

    for (int i = 0; i < u->ncorners; i++)
        if (u->corners[i].radius == radius && u->corners[i].alpha == alpha) return i;
    if (u->ncorners >= UIO_MAX_CORNERS) { note_err(u, -1); return -1; }

    static uint16_t mask[UIO_CORNER_MAX_RADIUS * UIO_CORNER_MAX_RADIUS];
    if (uio_bake_corner_mask(mask, radius, alpha) != 0) { note_err(u, -1); return -1; }

    blt_surface_ref_t s = blt_upload_argb4444(u->e, mask, radius, radius, radius * 2);
    if (!s.valid) { note_err(u, -1); return -1; }

    int slot = u->ncorners++;
    u->corners[slot].radius = radius;
    u->corners[slot].alpha  = alpha;
    u->corners[slot].surf   = s;
    return slot;
}

static uio_image_ref_t upload_padded(uio_t *u, const uint16_t *px, int w, int h, int pitch,
                                     int argb4444)
{
    uio_image_ref_t img;
    memset(&img, 0, sizeof img);
    if (!u || !u->e || !px || w <= 0 || h <= 0) return img;

    /* One-texel replicated border — see uio_image_ref_t's doc comment. */
    int pw = w + 2, ph = h + 2;
    uint16_t *pad = (uint16_t *)malloc((size_t)pw * (size_t)ph * sizeof *pad);
    if (!pad) { note_err(u, -1); return img; }

    for (int y = 0; y < ph; y++) {
        int sy = y - 1; if (sy < 0) sy = 0; else if (sy >= h) sy = h - 1;
        const uint16_t *srow = (const uint16_t *)((const uint8_t *)px + (size_t)sy * (size_t)pitch);
        uint16_t *drow = pad + (size_t)y * (size_t)pw;
        for (int x = 0; x < pw; x++) {
            int sx = x - 1; if (sx < 0) sx = 0; else if (sx >= w) sx = w - 1;
            drow[x] = srow[sx];
        }
    }

    img.surf = argb4444 ? blt_upload_argb4444(u->e, pad, pw, ph, pw * 2)
                        : blt_upload(u->e, pad, pw, ph, pw * 2);
    free(pad);
    if (!img.surf.valid) { note_err(u, -1); return img; }

    img.x = 1; img.y = 1;
    img.w = (uint16_t)w; img.h = (uint16_t)h;
    img.padded = 1;
    return img;
}

uio_image_ref_t uio_upload_image(uio_t *u, const uint16_t *px, int w, int h, int pitch)
{
    return upload_padded(u, px, w, h, pitch, 0);
}

uio_image_ref_t uio_upload_image_alpha(uio_t *u, const uint16_t *px4444, int w, int h, int pitch)
{
    return upload_padded(u, px4444, w, h, pitch, 1);
}

/* ────────────────────────────────────────────────────────────────────────
 *  Frame
 * ──────────────────────────────────────────────────────────────────────── */
void uio_begin_frame(uio_t *u, int target_buf, int clear, uint16_t clear_color)
{
    if (!u || !u->e) return;
    memset(&u->stats, 0, sizeof u->stats);
    blt_begin_frame(u->e, target_buf, clear, clear_color);
}

void uio_end_frame(uio_t *u)
{
    if (!u || !u->e) return;
    blt_end_frame(u->e);
    u->stats.cmds = (uint32_t)u->e->cmd_count;
}

/* ────────────────────────────────────────────────────────────────────────
 *  Primitives
 * ──────────────────────────────────────────────────────────────────────── */
static int emit_fill(uio_t *u, int x, int y, int w, int h, uint16_t color, uint8_t alpha)
{
    if (w <= 0 || h <= 0) return 0;
    int rc = (alpha == 255) ? blt_fill(u->e, x, y, w, h, color)
                            : blt_fill_alpha(u->e, x, y, w, h, color, alpha);
    if (rc != 0) { note_err(u, rc); return rc; }
    u->stats.fills++;
    u->stats.fabric_px  += (uint64_t)w * (uint64_t)h;
    u->stats.a9_fill_px += (uint64_t)w * (uint64_t)h;
    return 0;
}

int uio_fill(uio_t *u, uio_rect_t r, uint16_t color, uint8_t alpha)
{
    if (!u || !u->e) return -1;
    return emit_fill(u, r.x, r.y, r.w, r.h, color, alpha);
}

int uio_rounded_rect(uio_t *u, uio_rect_t r, int radius, uint16_t color, uint8_t alpha)
{
    if (!u || !u->e || r.w <= 0 || r.h <= 0) return -1;

    int maxr = (r.w < r.h ? r.w : r.h) / 2;
    if (radius > maxr) radius = maxr;
    if (radius > UIO_CORNER_MAX_RADIUS) radius = UIO_CORNER_MAX_RADIUS;
    if (radius <= 0) return emit_fill(u, r.x, r.y, r.w, r.h, color, alpha);

    int slot = uio_corner(u, radius, alpha);
    if (slot < 0) return -1;
    blt_surface_ref_t mask = u->corners[slot].surf;

    /* Flat interior: a band between the top corners, the full middle, and a
     * band between the bottom corners. */
    emit_fill(u, r.x + radius, r.y,              r.w - 2*radius, radius,          color, alpha);
    emit_fill(u, r.x,          r.y + radius,     r.w,            r.h - 2*radius,  color, alpha);
    emit_fill(u, r.x + radius, r.y + r.h-radius, r.w - 2*radius, radius,          color, alpha);

    /* Four arcs from ONE baked mask: the top-left quarter, mirrored. The mask
     * is white with coverage in A4, so BLT_F_COLORMOD supplies the colour and
     * BLT_BLEND_PALPHA composites the coverage. */
    uint8_t cr, cg, cb;
    uio_565_to_888(color, &cr, &cg, &cb);
    const struct { int dx, dy; uint8_t flags; } arc[4] = {
        { r.x,               r.y,                0                          },
        { r.x + r.w - radius, r.y,               BLT_F_HFLIP                },
        { r.x,               r.y + r.h - radius, BLT_F_VFLIP                },
        { r.x + r.w - radius, r.y + r.h - radius, BLT_F_HFLIP | BLT_F_VFLIP },
    };
    for (int i = 0; i < 4; i++) {
        int rc = blt_blit_mod(u->e, mask, 0, 0, radius, radius, arc[i].dx, arc[i].dy,
                              BLT_BLEND_PALPHA, 0, 255, arc[i].flags, cr, cg, cb);
        if (rc != 0) { note_err(u, rc); return rc; }
        u->stats.blits++;
        u->stats.fabric_px += (uint64_t)radius * (uint64_t)radius;
        u->stats.a9_aa_px  += (uint64_t)radius * (uint64_t)radius;
    }
    u->stats.rounded_rects++;
    return 0;
}

int uio_image_blit(uio_t *u, uio_image_ref_t img, int dx, int dy,
                   uint8_t blend, uint16_t colorkey, uint8_t alpha)
{
    if (!u || !u->e || !img.surf.valid) return -1;
    uint8_t flags = (blend == BLT_BLEND_COLORKEY) ? BLT_F_COLORKEY : 0u;
    int rc = blt_blit(u->e, img.surf, img.x, img.y, img.w, img.h, dx, dy,
                      blend, colorkey, alpha, flags);
    if (rc != 0) { note_err(u, rc); return rc; }
    u->stats.blits++;
    u->stats.fabric_px  += (uint64_t)img.w * (uint64_t)img.h;
    u->stats.a9_fill_px += (uint64_t)img.w * (uint64_t)img.h;
    return UIO_PATH_BLIT;
}

/*  uv for a TRILIST quad edge.
 *
 *  The fabric samples nearest-texel as round(u) (blt_tri.c: (u_fx+8)>>4), while
 *  standard nearest-neighbour resampling wants floor(u) of the pixel-centre
 *  coordinate. The difference is a half-texel, so the quad's uv range is biased
 *  down by half a texel — which is exactly why an image wants the one-texel
 *  border: without it the leading edge's uv would have to go negative, and
 *  blt_vtx_t's uv is unsigned.
 */
static int uv_lo(int texel, int *clamped)
{
    int v = (texel << UIO_FX) - (1 << (UIO_FX - 1));
    if (v < 0) { v = 0; if (clamped) *clamped = 1; }
    return v;
}

int uio_image_scaled(uio_t *u, uio_image_ref_t img, const uio_scale_t *s)
{
    if (!u || !u->e || !s || !img.surf.valid) return -1;
    if (s->dst.w <= 0 || s->dst.h <= 0) return -1;

    int sw = (s->src.w > 0) ? s->src.w : img.w;
    int sh = (s->src.h > 0) ? s->src.h : img.h;
    int sx = img.x + s->src.x;
    int sy = img.y + s->src.y;
    if (sw <= 0 || sh <= 0) return -1;

    const uint8_t no_tint = (s->tint[0] == 255 && s->tint[1] == 255 && s->tint[2] == 255)
                          || (s->tint[0] == 0 && s->tint[1] == 0 && s->tint[2] == 0);
    uint8_t tr = no_tint ? 255 : s->tint[0];
    uint8_t tg = no_tint ? 255 : s->tint[1];
    uint8_t tb = no_tint ? 255 : s->tint[2];

    /* Exact ratio: the plain blit is cheaper on the fabric than a rasterized
     * quad, so an unscaled draw never pays for the triangle path. */
    if (!s->force_fabric && sw == s->dst.w && sh == s->dst.h) {
        uint8_t flags = (s->blend == BLT_BLEND_COLORKEY) ? BLT_F_COLORKEY : 0u;
        int rc;
        if (no_tint)
            rc = blt_blit(u->e, img.surf, sx, sy, sw, sh, s->dst.x, s->dst.y,
                          s->blend, s->colorkey, s->alpha, flags);
        else
            rc = blt_blit_mod(u->e, img.surf, sx, sy, sw, sh, s->dst.x, s->dst.y,
                              s->blend, s->colorkey, s->alpha, flags, tr, tg, tb);
        if (rc != 0) { note_err(u, rc); return rc; }
        u->stats.blits++;
        u->stats.fabric_px  += (uint64_t)sw * (uint64_t)sh;
        u->stats.a9_fill_px += (uint64_t)sw * (uint64_t)sh;
        return UIO_PATH_BLIT;
    }

    /* The triangle path samples 16bpp colour with no alpha channel, so an
     * image with per-pixel alpha cannot be fabric-scaled. Refuse it (the caller
     * falls back and counts it) rather than silently drop the alpha. */
    if (img.surf.format == BLT_FMT_ARGB4444) return -1;

    /* Arbitrary ratio: hand the resample to the fabric as a textured quad. */
    int clamped = 0;
    int u0 = uv_lo(sx, &clamped),      u1 = uv_lo(sx + sw, NULL);
    int v0 = uv_lo(sy, &clamped),      v1 = uv_lo(sy + sh, NULL);
    if (clamped && !img.padded) u->stats.uv_clamped++;

    int x0 = s->dst.x << UIO_FX,       x1 = (s->dst.x + s->dst.w) << UIO_FX;
    int y0 = s->dst.y << UIO_FX,       y1 = (s->dst.y + s->dst.h) << UIO_FX;
    uint32_t rgba = BLT_RGBA(tr, tg, tb, 255);

    const blt_vtx_t q[4] = {
        { (int16_t)x0, (int16_t)y0, (uint16_t)u0, (uint16_t)v0, rgba, 0 },
        { (int16_t)x1, (int16_t)y0, (uint16_t)u1, (uint16_t)v0, rgba, 0 },
        { (int16_t)x1, (int16_t)y1, (uint16_t)u1, (uint16_t)v1, rgba, 0 },
        { (int16_t)x0, (int16_t)y1, (uint16_t)u0, (uint16_t)v1, rgba, 0 },
    };
    const blt_vtx_t tris[6] = { q[0], q[1], q[2], q[0], q[2], q[3] };

    uint32_t off = blt_push_tris(u->e, tris, 2);
    if (off == 0xFFFFFFFFu) { note_err(u, -1); return -1; }
    int rc = blt_trilist(u->e, img.surf, s->blend, s->colorkey, s->alpha,
                         u->vtx_off + off, 2, 0);
    if (rc != 0) { note_err(u, rc); return rc; }

    u->stats.trilists++;
    u->stats.fabric_px     += (uint64_t)s->dst.w * (uint64_t)s->dst.h;
    u->stats.a9_resample_px += (uint64_t)s->dst.w * (uint64_t)s->dst.h;
    return UIO_PATH_TRILIST;
}

int uio_text_width(const char *str)
{
    int n = 0;
    for (const char *p = str; p && *p; p++) n++;
    return n * UIO_FONT_CELL_W;
}

int uio_text(uio_t *u, int x, int y, const char *str, uint16_t color)
{
    if (!u || !u->e || !str) return 0;
    if (!u->font_ready && uio_load_font(u) != 0) return 0;

    uint8_t cr, cg, cb;
    uio_565_to_888(color, &cr, &cg, &cb);

    int pen = x;
    for (const char *p = str; *p; p++, pen += UIO_FONT_CELL_W) {
        int cell = uio_font_cell((unsigned char)*p);
        if (cell < 0 || *p == ' ') continue;             /* no ink, advance only */
        int rc = blt_blit_mod(u->e, u->font,
                              uio_font_cell_x(cell), uio_font_cell_y(cell),
                              UIO_FONT_INK_W, UIO_FONT_INK_H, pen, y,
                              BLT_BLEND_PALPHA, 0, 255, 0, cr, cg, cb);
        if (rc != 0) { note_err(u, rc); break; }
        u->stats.glyphs++;
        u->stats.blits++;
        u->stats.fabric_px   += UIO_FONT_INK_W * UIO_FONT_INK_H;
        u->stats.a9_glyph_px += UIO_FONT_INK_W * UIO_FONT_INK_H;
    }
    return pen - x;
}

uio_rect_t uio_fit(uio_rect_t box, int iw, int ih)
{
    uio_rect_t out = box;
    if (iw <= 0 || ih <= 0 || box.w <= 0 || box.h <= 0) { out.w = out.h = 0; return out; }

    /* Largest integer box with the source aspect that fits, centred. */
    long by_w = (long)box.w * ih;          /* height if width-limited  */
    long by_h = (long)box.h * iw;          /* width  if height-limited */
    if (by_w <= by_h) { out.w = box.w;                 out.h = (int)(by_w / iw); }
    else              { out.w = (int)(by_h / ih);      out.h = box.h;            }
    if (out.w < 1) out.w = 1;
    if (out.h < 1) out.h = 1;
    out.x = box.x + (box.w - out.w) / 2;
    out.y = box.y + (box.h - out.h) / 2;
    return out;
}
