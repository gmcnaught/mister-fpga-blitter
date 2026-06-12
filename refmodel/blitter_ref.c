/*
 *  blitter_ref.c — Software reference model for the MiSTer fabric 2D blitter.
 *  See blitter_ref.h for the contract. GPL-3.0.
 *
 *  This is the GOLDEN model: its per-pixel output defines the exact semantics
 *  the RTL must reproduce. Keep all arithmetic integer and hardware-reducible.
 */
#include "blitter_ref.h"

/* ---- RGB565 helpers ----------------------------------------------------- */

uint16_t blt_rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r & 0xF8u) << 8) | ((g & 0xFCu) << 3) | (b >> 3));
}

/*
 *  Canonical 8-bit-alpha blend of one channel: out = (s*a + d*(255-a) + 127)/255.
 *  RTL MUST match this exactly. A divide-free equivalent, verified bit-exact over
 *  t = s*a+d*(255-a) in [0, 255*255], is:
 *      div(t) = (t + 128 + ((t + 128) >> 8)) >> 8
 *  (see ../docs/blitter-protocol.md). The model uses the plain /255 form.
 */
static uint32_t blend_chan(uint32_t s, uint32_t d, uint32_t a)
{
    return (s * a + d * (255u - a) + 127u) / 255u;
}

uint16_t blt_blend565(uint16_t src, uint16_t dst, uint8_t alpha)
{
    uint32_t a  = alpha;
    uint32_t sr = (src >> 11) & 0x1Fu, sg = (src >> 5) & 0x3Fu, sb = src & 0x1Fu;
    uint32_t dr = (dst >> 11) & 0x1Fu, dg = (dst >> 5) & 0x3Fu, db = dst & 0x1Fu;
    uint32_t r = blend_chan(sr, dr, a);
    uint32_t g = blend_chan(sg, dg, a);
    uint32_t b = blend_chan(sb, db, a);
    return (uint16_t)((r << 11) | (g << 5) | b);
}

/* ---- Source fetch (clamped to heap for model safety) -------------------- */

static uint16_t src_fetch(const blt_surface_heap_t *heap,
                          uint32_t src_off, uint16_t stride,
                          uint32_t sx, uint32_t sy)
{
    if (!heap || !heap->base) return 0;
    uint64_t byte = (uint64_t)src_off + (uint64_t)sy * stride + (uint64_t)sx * 2u;
    if (byte + 1u >= heap->size) return 0;          /* OOB -> 0 */
    const uint8_t *p = heap->base + byte;
    return (uint16_t)(p[0] | (p[1] << 8));          /* little-endian RGB565 */
}

/* ---- Rect clip against the framebuffer ---------------------------------- */
/* Fills clipped output bounds [x0,x1) x [y0,y1); returns 0 if fully offscreen. */
static int clip_rect(int dst_x, int dst_y, int w, int h,
                     int *x0, int *y0, int *x1, int *y1)
{
    int rx0 = dst_x, ry0 = dst_y;
    int rx1 = dst_x + w, ry1 = dst_y + h;
    if (rx0 < 0) rx0 = 0;
    if (ry0 < 0) ry0 = 0;
    if (rx1 > BLT_FB_WIDTH)  rx1 = BLT_FB_WIDTH;
    if (ry1 > BLT_FB_HEIGHT) ry1 = BLT_FB_HEIGHT;
    if (rx0 >= rx1 || ry0 >= ry1) return 0;          /* fully offscreen */
    *x0 = rx0; *y0 = ry0; *x1 = rx1; *y1 = ry1;
    return 1;
}

/* ---- Command executors -------------------------------------------------- */

static void do_fill(uint16_t *fb, const blt_cmd_t *c)
{
    int x0, y0, x1, y1;
    if (!clip_rect(c->dst_x, c->dst_y, c->w, c->h, &x0, &y0, &x1, &y1)) return;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++)
            fb[y * BLT_FB_WIDTH + x] = c->color;
}

static void do_blit(uint16_t *fb, const blt_surface_heap_t *heap,
                    const blt_cmd_t *c)
{
    int x0, y0, x1, y1;
    if (!clip_rect(c->dst_x, c->dst_y, c->w, c->h, &x0, &y0, &x1, &y1)) return;

    int hflip = (c->flags & BLT_F_HFLIP) != 0;
    int vflip = (c->flags & BLT_F_VFLIP) != 0;
    int keyed = (c->blend_mode == BLT_BLEND_COLORKEY) ||
                (c->flags & BLT_F_COLORKEY);

    for (int dy = y0; dy < y1; dy++) {
        int ly = dy - c->dst_y;                       /* 0..h-1 */
        int sy = vflip ? (c->h - 1 - ly) : ly;
        for (int dx = x0; dx < x1; dx++) {
            int lx = dx - c->dst_x;                    /* 0..w-1 */
            int sx = hflip ? (c->w - 1 - lx) : lx;
            uint16_t s = src_fetch(heap, c->src_off, c->src_stride,
                                   (uint32_t)(c->src_x + sx),
                                   (uint32_t)(c->src_y + sy));
            if (keyed && s == c->colorkey) continue;   /* skip-write fast path */

            uint16_t *d = &fb[dy * BLT_FB_WIDTH + dx];
            if (c->blend_mode == BLT_BLEND_CONST_ALPHA)
                *d = blt_blend565(s, *d, c->alpha);
            else
                *d = s;                                 /* COPY / COLORKEY */
        }
    }
}

/* ---- Top-level list walker ---------------------------------------------- */

int blt_execute(uint16_t *fb, const blt_surface_heap_t *heap,
                const blt_cmd_t *cmds, int count)
{
    int i;
    for (i = 0; i < count; i++) {
        const blt_cmd_t *c = &cmds[i];
        if (c->opcode == BLT_OP_END) { i++; break; }   /* walk-until-END */
        switch (c->opcode) {
            case BLT_OP_NOP:                      break;
            case BLT_OP_FILL: do_fill(fb, c);     break;
            case BLT_OP_BLIT: do_blit(fb, heap, c); break;
            default: /* unknown opcode: ignore (RTL: treat as NOP) */ break;
        }
    }
    return i;
}
