/*
 *  test_blitter_ref.c — host unit tests for the blitter reference model.
 *  Build+run on the dev machine: `make test` (no hardware, no deps).
 *  Each test exercises one primitive and checks exact pixel values, so this
 *  doubles as the golden-output spec the RTL is diffed against.
 *  GPL-3.0.
 */
#include "blitter_ref.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

static int g_fail = 0;
static int g_checks = 0;

#define CHECK(cond, msg) do {                                            \
    g_checks++;                                                          \
    if (!(cond)) { printf("  FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); g_fail++; } \
} while (0)

#define PX(fb, x, y) ((fb)[(y) * BLT_FB_WIDTH + (x)])

static uint16_t *new_fb(uint16_t clear)
{
    uint16_t *fb = malloc(BLT_FB_PIXELS * sizeof(uint16_t));
    for (int i = 0; i < BLT_FB_PIXELS; i++) fb[i] = clear;
    return fb;
}

/* A small solid source surface of WxH filled with `val`, plus an optional
 * single keyed pixel. Returned buffer is heap-owned; caller frees. */
static uint8_t *make_surface(int w, int h, uint16_t val, size_t *out_size)
{
    size_t n = (size_t)w * h;
    uint8_t *buf = malloc(n * 2);
    for (size_t i = 0; i < n; i++) { buf[i*2] = val & 0xFF; buf[i*2+1] = val >> 8; }
    *out_size = n * 2;
    return buf;
}

/* ------------------------------------------------------------------------- */

static void test_fill(void)
{
    printf("test_fill\n");
    uint16_t *fb = new_fb(0x0000);
    blt_cmd_t cmds[2] = {0};
    cmds[0].opcode = BLT_OP_FILL;
    cmds[0].dst_x = 10; cmds[0].dst_y = 20; cmds[0].w = 4; cmds[0].h = 3;
    cmds[0].color = 0xF800; /* red */
    cmds[1].opcode = BLT_OP_END;

    int n = blt_execute(fb, NULL, cmds, 2);
    CHECK(n == 2, "executed 1 fill + END");
    CHECK(PX(fb, 10, 20) == 0xF800, "fill top-left set");
    CHECK(PX(fb, 13, 22) == 0xF800, "fill bottom-right set");
    CHECK(PX(fb, 14, 20) == 0x0000, "fill respects width (x=14 untouched)");
    CHECK(PX(fb, 10, 23) == 0x0000, "fill respects height (y=23 untouched)");
    CHECK(PX(fb, 9, 20)  == 0x0000, "fill left edge untouched");
    free(fb);
}

static void test_copy(void)
{
    printf("test_copy\n");
    uint16_t *fb = new_fb(0x001F); /* blue bg */
    size_t sz;
    uint8_t *surf = make_surface(8, 8, 0x07E0, &sz); /* green */
    blt_surface_heap_t heap = { surf, sz, NULL, NULL };
    blt_cmd_t cmds[2] = {0};
    cmds[0].opcode = BLT_OP_BLIT; cmds[0].blend_mode = BLT_BLEND_COPY;
    cmds[0].format = BLT_FMT_RGB565;
    cmds[0].src_stride = 8 * 2; cmds[0].w = 8; cmds[0].h = 8;
    cmds[0].dst_x = 100; cmds[0].dst_y = 100;
    cmds[1].opcode = BLT_OP_END;

    blt_execute(fb, &heap, cmds, 2);
    CHECK(PX(fb, 100, 100) == 0x07E0, "copy wrote green at origin");
    CHECK(PX(fb, 107, 107) == 0x07E0, "copy wrote green at far corner");
    CHECK(PX(fb, 108, 100) == 0x001F, "copy did not overrun width");
    CHECK(PX(fb, 99, 100)  == 0x001F, "copy left untouched");
    free(surf); free(fb);
}

static void test_colorkey(void)
{
    printf("test_colorkey\n");
    uint16_t *fb = new_fb(0x001F);
    /* 2x1 surface: pixel0 = key (0x0000), pixel1 = green */
    uint8_t surf[4] = { 0x00, 0x00, 0xE0, 0x07 };
    blt_surface_heap_t heap = { surf, sizeof(surf), NULL, NULL };
    blt_cmd_t cmds[2] = {0};
    cmds[0].opcode = BLT_OP_BLIT; cmds[0].blend_mode = BLT_BLEND_COLORKEY;
    cmds[0].src_stride = 4; cmds[0].w = 2; cmds[0].h = 1;
    cmds[0].dst_x = 50; cmds[0].dst_y = 50; cmds[0].colorkey = 0x0000;
    cmds[1].opcode = BLT_OP_END;

    blt_execute(fb, &heap, cmds, 2);
    CHECK(PX(fb, 50, 50) == 0x001F, "keyed pixel skipped (bg preserved)");
    CHECK(PX(fb, 51, 50) == 0x07E0, "non-keyed pixel written");
    free(fb);
}

static void test_const_alpha(void)
{
    printf("test_const_alpha\n");
    uint16_t *fb = new_fb(0x0000); /* black dst */
    size_t sz;
    uint8_t *surf = make_surface(2, 2, 0xFFFF, &sz); /* white src */
    blt_surface_heap_t heap = { surf, sz, NULL, NULL };
    blt_cmd_t cmds[2] = {0};
    cmds[0].opcode = BLT_OP_BLIT; cmds[0].blend_mode = BLT_BLEND_CONST_ALPHA;
    cmds[0].src_stride = 4; cmds[0].w = 2; cmds[0].h = 2;
    cmds[0].dst_x = 0; cmds[0].dst_y = 0; cmds[0].alpha = 128;
    cmds[1].opcode = BLT_OP_END;

    blt_execute(fb, &heap, cmds, 2);
    /* white(31,63,31) over black at a=128: round(31*128/255)=16, round(63*128/255)=32 */
    uint16_t expect = (uint16_t)((16u << 11) | (32u << 5) | 16u);
    CHECK(PX(fb, 0, 0) == expect, "const-alpha 50% white over black");
    /* a=255 -> exact src; a=0 -> exact dst */
    CHECK(blt_blend565(0xFFFF, 0x0000, 255) == 0xFFFF, "alpha=255 is src");
    CHECK(blt_blend565(0xFFFF, 0x0000, 0)   == 0x0000, "alpha=0 is dst");
    free(surf); free(fb);
}

static void test_flips(void)
{
    printf("test_flips\n");
    /* 2x2 surface with distinct pixels:
       (0,0)=A 0x0001  (1,0)=B 0x0002
       (0,1)=C 0x0003  (1,1)=D 0x0004 */
    uint8_t surf[8] = { 1,0, 2,0, 3,0, 4,0 };
    blt_surface_heap_t heap = { surf, sizeof(surf), NULL, NULL };

    /* HFLIP: top row becomes B,A */
    uint16_t *fb = new_fb(0x0000);
    blt_cmd_t c[2] = {0};
    c[0].opcode = BLT_OP_BLIT; c[0].blend_mode = BLT_BLEND_COPY;
    c[0].src_stride = 4; c[0].w = 2; c[0].h = 2; c[0].flags = BLT_F_HFLIP;
    c[1].opcode = BLT_OP_END;
    blt_execute(fb, &heap, c, 2);
    CHECK(PX(fb,0,0) == 0x0002 && PX(fb,1,0) == 0x0001, "hflip top row B,A");
    CHECK(PX(fb,0,1) == 0x0004 && PX(fb,1,1) == 0x0003, "hflip bot row D,C");
    free(fb);

    /* VFLIP: rows swap */
    fb = new_fb(0x0000);
    c[0].flags = BLT_F_VFLIP;
    blt_execute(fb, &heap, c, 2);
    CHECK(PX(fb,0,0) == 0x0003 && PX(fb,1,0) == 0x0004, "vflip top row C,D");
    CHECK(PX(fb,0,1) == 0x0001 && PX(fb,1,1) == 0x0002, "vflip bot row A,B");
    free(fb);
}

static void test_clipping(void)
{
    printf("test_clipping\n");
    size_t sz;
    uint8_t *surf = make_surface(16, 16, 0xFFFF, &sz);
    blt_surface_heap_t heap = { surf, sz, NULL, NULL };

    /* Negative dst origin: only the in-bounds part is drawn, correct src pixel */
    uint16_t *fb = new_fb(0x0000);
    blt_cmd_t c[2] = {0};
    c[0].opcode = BLT_OP_BLIT; c[0].blend_mode = BLT_BLEND_COPY;
    c[0].src_stride = 16*2; c[0].w = 16; c[0].h = 16;
    c[0].dst_x = -4; c[0].dst_y = -4;
    c[1].opcode = BLT_OP_END;
    blt_execute(fb, &heap, c, 2);
    CHECK(PX(fb,0,0) == 0xFFFF, "clip: visible corner drawn");
    CHECK(PX(fb,11,11) == 0xFFFF, "clip: interior drawn");
    CHECK(PX(fb,12,0) == 0x0000, "clip: beyond visible width untouched");

    /* Fully offscreen: no writes at all */
    uint16_t *fb2 = new_fb(0x1234);
    c[0].dst_x = 400; c[0].dst_y = 0;
    blt_execute(fb2, &heap, c, 2);
    int dirty = 0;
    for (int i = 0; i < BLT_FB_PIXELS; i++) if (fb2[i] != 0x1234) dirty = 1;
    CHECK(!dirty, "fully-offscreen blit is a no-op (zero writes)");

    /* Bottom-right overhang clipped to fb edge */
    uint16_t *fb3 = new_fb(0x0000);
    c[0].dst_x = BLT_FB_WIDTH - 8; c[0].dst_y = BLT_FB_HEIGHT - 8;
    blt_execute(fb3, &heap, c, 2);
    CHECK(PX(fb3, BLT_FB_WIDTH-1, BLT_FB_HEIGHT-1) == 0xFFFF, "clip: last pixel drawn");
    free(surf); free(fb); free(fb2); free(fb3);
}

static void test_end_and_overdraw(void)
{
    printf("test_end_and_overdraw\n");
    uint16_t *fb = new_fb(0x0000);
    blt_cmd_t cmds[4] = {0};
    cmds[0].opcode = BLT_OP_FILL; cmds[0].dst_x=0; cmds[0].dst_y=0;
    cmds[0].w=2; cmds[0].h=2; cmds[0].color = 0x0011;
    cmds[1].opcode = BLT_OP_END;
    /* Command after END must NOT execute */
    cmds[2].opcode = BLT_OP_FILL; cmds[2].dst_x=0; cmds[2].dst_y=0;
    cmds[2].w=2; cmds[2].h=2; cmds[2].color = 0x0099;
    cmds[3].opcode = BLT_OP_END;

    int n = blt_execute(fb, NULL, cmds, 4);
    CHECK(n == 2, "execution stops at first END");
    CHECK(PX(fb,0,0) == 0x0011, "command after END did not run (painter order honored)");

    /* Overdraw / painter's order within a list: later command wins */
    uint16_t *fb2 = new_fb(0x0000);
    blt_cmd_t ov[3] = {0};
    ov[0].opcode=BLT_OP_FILL; ov[0].w=4; ov[0].h=4; ov[0].color=0x00AA;
    ov[1].opcode=BLT_OP_FILL; ov[1].w=2; ov[1].h=2; ov[1].color=0x00BB;
    ov[2].opcode=BLT_OP_END;
    blt_execute(fb2, NULL, ov, 3);
    CHECK(PX(fb2,0,0) == 0x00BB, "overdraw: later fill wins");
    CHECK(PX(fb2,3,3) == 0x00AA, "overdraw: uncovered area keeps first fill");
    free(fb); free(fb2);
}

/* [app-surface render target, step 1] Two-pass scene: pass A renders a solid
 * 8x8 magenta quad into the app-surface at (0,0) (plus an explicit whole-
 * surface clear first, so the test is deterministic regardless of what any
 * earlier test in this binary left in the internal `appsurf` buffer -- that
 * buffer is static/persistent across blt_execute() calls by design, matching
 * the real hardware's second BRAM bank having no per-call reset). Pass B
 * (SET_TARGET WORK) draws a fullscreen quad sampling the app-surface with
 * BLT_F_SRC_SURFACE, BLEND_COPY.
 *
 * The fullscreen quad's position AND uv corners use the identical (px<<4,
 * py<<4) encoding (see blt_tri.c's rasterizer: vertex attributes carry no
 * implicit pixel-center bias, but the per-pixel sample point does — sx =
 * (px<<4)|8). Interpolating an attribute that is numerically identical to
 * position at every vertex reproduces the SAMPLE POINT exactly, so the
 * fetched texel at destination pixel (px,py) is texel ((px<<4|8 + 8)>>4,
 * ...) = (px+1, py+1) -- an exact, deterministic +1 texel offset (not a
 * rounding tie: 16 divides the +8+8=16 remainder cleanly). This is existing,
 * pre-existing rasterizer behavior (shared with every other textured TRILIST
 * in this file), not something new to BLT_F_SRC_SURFACE — accounted for
 * below by placing the magenta quad so screen (0,0) lands on surface texel
 * (1,1) (inside [0,8)x[0,8)) and screen (0,8) lands on texel (1,9) (outside). */
static int build_surface_src_scene(blt_cmd_t *cmds, blt_vtx_t *verts) {
    const uint16_t SURF_CLEAR = 0x0410; /* dark teal-ish; just != magenta */
    const uint16_t MAGENTA = 0xF81F;    /* rgb565(255,0,255) */
    int n = 0;

    cmds[n]=(blt_cmd_t){0}; cmds[n].opcode=BLT_OP_SET_TARGET; cmds[n].color=BLT_TARGET_APPSURF; n++;
    cmds[n]=(blt_cmd_t){0}; cmds[n].opcode=BLT_OP_FILL;
        cmds[n].dst_x=0; cmds[n].dst_y=0; cmds[n].w=BLT_FB_WIDTH; cmds[n].h=BLT_FB_HEIGHT;
        cmds[n].color=SURF_CLEAR; n++;
    cmds[n]=(blt_cmd_t){0}; cmds[n].opcode=BLT_OP_FILL;
        cmds[n].dst_x=0; cmds[n].dst_y=0; cmds[n].w=8; cmds[n].h=8;
        cmds[n].color=MAGENTA; n++;
    cmds[n]=(blt_cmd_t){0}; cmds[n].opcode=BLT_OP_SET_TARGET; cmds[n].color=BLT_TARGET_WORK; n++;

    /* fullscreen quad, position == uv (12.4, texel units, no half-texel bias --
     * see comment above), 2 triangles, entry_off=0 in the vertex heap. */
#define SV(px,py) (blt_vtx_t){ (int16_t)((px)<<4), (int16_t)((py)<<4), \
                               (uint16_t)((px)<<4), (uint16_t)((py)<<4), \
                               BLT_RGBA(255,255,255,255), 0 }
    verts[0]=SV(0,0);              verts[1]=SV(BLT_FB_WIDTH,0);            verts[2]=SV(BLT_FB_WIDTH,BLT_FB_HEIGHT);
    verts[3]=SV(0,0);              verts[4]=SV(BLT_FB_WIDTH,BLT_FB_HEIGHT); verts[5]=SV(0,BLT_FB_HEIGHT);
#undef SV
    cmds[n]=(blt_cmd_t){0};
    cmds[n].opcode=BLT_OP_TRILIST; cmds[n].blend_mode=BLT_BLEND_COPY; cmds[n].format=BLT_FMT_RGB565;
    cmds[n].flags=BLT_F_SRC_SURFACE; cmds[n].alpha=255;
    cmds[n].w=2;                                    /* triangle count */
    cmds[n].dst_x=0; cmds[n].dst_y=0;                /* entry_off = 0, low|high 16 */
    n++;
    cmds[n]=(blt_cmd_t){0}; cmds[n].opcode=BLT_OP_END; n++;
    return n;
}

static void test_surface_src(void)
{
    printf("test_surface_src\n");
    enum { NCMDS = 6 };
    blt_cmd_t cmds[NCMDS];
    blt_vtx_t verts[6];
    int n = build_surface_src_scene(cmds, verts);
    CHECK(n == NCMDS, "scene built the expected command count");

    uint8_t heapbuf[6 * sizeof(blt_vtx_t)];
    memcpy(heapbuf, verts, sizeof heapbuf);
    blt_surface_heap_t heap = { heapbuf, sizeof heapbuf, NULL, NULL };

    uint16_t *fb = new_fb(0x0000);
    int executed = blt_execute(fb, &heap, cmds, n);
    CHECK(executed == NCMDS, "all commands executed (incl. END)");

    uint16_t magenta = 0xF81F, surf_clear = 0x0410;
    CHECK(PX(fb, 0, 0) == magenta, "surface-sampled pixel (0,0) is magenta");
    CHECK(PX(fb, 0, 8) != magenta, "row 8 (just outside the 8x8 quad) is not magenta");
    CHECK(PX(fb, 0, 8) == surf_clear, "row 8 shows the app-surface's own clear color");
    CHECK(PX(fb, 200, 150) == surf_clear, "far corner also shows the surface clear color");
    free(fb);
}

static void test_trilist_layout(void){
    assert(sizeof(blt_vtx_t) == 16);
    assert(BLT_OP_TRILIST == 10);
    blt_vtx_t v = { .x=1, .y=2, .u=3, .v=4, .rgba=BLT_RGBA(10,20,30,40), ._rsvd=0 };
    assert((v.rgba & 0xff)==10 && ((v.rgba>>24)&0xff)==40);
    printf("test_trilist_layout OK\n");
}

int main(void)
{
    printf("=== blitter reference model unit tests ===\n");
    test_trilist_layout();
    test_fill();
    test_copy();
    test_colorkey();
    test_const_alpha();
    test_flips();
    test_clipping();
    test_end_and_overdraw();
    test_surface_src();
    printf("=== %d checks, %d failures ===\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
