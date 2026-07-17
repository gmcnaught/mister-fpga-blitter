/*
 *  test_blt_tri.c — golden unit tests for the reference triangle rasterizer.
 *  Copyright (C) 2026 — GPL-3.0.
 */
#include "blitter_ref.h"
#include "blt_tri.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* 1x1 white RGB565 texture at heap offset 0, so texel modulate == vertex color. */
static blt_surface_heap_t mk_white_tex(uint8_t *mem){
    mem[0]=0xff; mem[1]=0xff;               /* RGB565 0xFFFF = white, little-endian */
    blt_surface_heap_t h; memset(&h,0,sizeof h); h.base=mem; return h;
}
static blt_cmd_t mk_hdr(uint8_t blend){
    blt_cmd_t c; memset(&c,0,sizeof c);
    c.opcode=BLT_OP_TRILIST; c.blend_mode=blend; c.format=BLT_FMT_RGB565;
    c.src_off=0; c.src_stride=2; c.src_x=1; c.src_y=1; c.alpha=255;
    return c;
}
#define V(px,py,cr,cg,cb,ca) (blt_vtx_t){ (int16_t)((px)*16),(int16_t)((py)*16),0,0, BLT_RGBA(cr,cg,cb,ca),0 }

static void test_solid_red_quad_copy(void){
    uint8_t tex[2]; blt_surface_heap_t heap = mk_white_tex(tex);
    uint16_t fb[BLT_FB_WIDTH*BLT_FB_HEIGHT]; memset(fb,0,sizeof fb);
    blt_cmd_t h = mk_hdr(BLT_BLEND_COPY);
    /* axis-aligned 10x10 quad at (5,5), two tris, pure red */
    blt_vtx_t tris[6] = {
        V(5,5,255,0,0,255),  V(15,5,255,0,0,255),  V(15,15,255,0,0,255),
        V(5,5,255,0,0,255),  V(15,15,255,0,0,255), V(5,15,255,0,0,255),
    };
    blt_raster_tri(fb, &heap, &h, tris, 2, NULL);
    /* interior pixel (10,10) must be red 0xF800; a pixel outside (0,0) must be 0 */
    assert(fb[10*BLT_FB_WIDTH+10]==0xF800);
    assert(fb[0]==0x0000);
    /* pixel just outside the right edge (15,10) must NOT be filled (top-left rule, exclusive) */
    assert(fb[10*BLT_FB_WIDTH+15]==0x0000);
    printf("test_solid_red_quad_copy OK\n");
}

static void test_alpha_blend_half(void){
    uint8_t tex[2]; blt_surface_heap_t heap = mk_white_tex(tex);
    uint16_t fb[BLT_FB_WIDTH*BLT_FB_HEIGHT];
    for(int i=0;i<BLT_FB_WIDTH*BLT_FB_HEIGHT;i++) fb[i]=0x001F; /* blue background */
    blt_cmd_t h = mk_hdr(BLT_BLEND_CONST_ALPHA);
    blt_vtx_t tris[6] = {
        V(0,0,255,0,0,128),  V(20,0,255,0,0,128),  V(20,20,255,0,0,128),
        V(0,0,255,0,0,128),  V(20,20,255,0,0,128), V(0,20,255,0,0,128),
    };
    blt_raster_tri(fb, &heap, &h, tris, 2, NULL);
    uint16_t got = fb[10*BLT_FB_WIDTH+10];
    uint16_t expect = blt_blend565(0xF800, 0x001F, 128);
    assert(got==expect);
    printf("test_alpha_blend_half OK\n");
}

/* End-to-end: a TRILIST header in a command list, vertices in the entry buffer,
 * driven through blt_execute — same result as calling blt_raster_tri directly. */
static void test_trilist_via_execute(void){
    enum { ENTRY_OFF = 16 };   /* first vertex sits past the 2-byte tex, 16B-aligned */
    uint8_t heapbuf[ENTRY_OFF + 6*sizeof(blt_vtx_t)];
    memset(heapbuf,0,sizeof heapbuf);
    heapbuf[0]=0xff; heapbuf[1]=0xff;                 /* 1x1 white tex at offset 0 */
    blt_vtx_t tris[6] = {
        V(5,5,255,0,0,255),  V(15,5,255,0,0,255),  V(15,15,255,0,0,255),
        V(5,5,255,0,0,255),  V(15,15,255,0,0,255), V(5,15,255,0,0,255),
    };
    memcpy(heapbuf+ENTRY_OFF, tris, sizeof tris);
    blt_surface_heap_t heap; memset(&heap,0,sizeof heap);
    heap.base=heapbuf; heap.size=sizeof heapbuf;

    uint16_t fb[BLT_FB_WIDTH*BLT_FB_HEIGHT]; memset(fb,0,sizeof fb);
    blt_cmd_t cmds[2]; memset(cmds,0,sizeof cmds);
    cmds[0]=mk_hdr(BLT_BLEND_COPY);
    cmds[0].w=2;                                      /* triangle count */
    cmds[0].dst_x=(int16_t)(ENTRY_OFF & 0xFFFF);     /* entry_off low  */
    cmds[0].dst_y=(int16_t)(ENTRY_OFF >> 16);        /* entry_off high */
    cmds[1].opcode=BLT_OP_END;
    blt_execute(fb, &heap, cmds, 2);
    assert(fb[10*BLT_FB_WIDTH+10]==0xF800);          /* interior red */
    assert(fb[10*BLT_FB_WIDTH+15]==0x0000);          /* right edge exclusive */
    printf("test_trilist_via_execute OK\n");
}

/* two overlapping CONST_ALPHA quads: the overlap pixel must equal the sequential
 * blend of B over (A over bg). */
static void test_overlap_order(void){
    uint8_t tex[2]; blt_surface_heap_t heap = mk_white_tex(tex);
    uint16_t fb[BLT_FB_WIDTH*BLT_FB_HEIGHT];
    for(int i=0;i<BLT_FB_WIDTH*BLT_FB_HEIGHT;i++) fb[i]=0x001F; /* blue bg */
    blt_cmd_t h = mk_hdr(BLT_BLEND_CONST_ALPHA);
    blt_vtx_t A[6] = {
        V(5,5,255,0,0,128),  V(15,5,255,0,0,128),  V(15,15,255,0,0,128),
        V(5,5,255,0,0,128),  V(15,15,255,0,0,128), V(5,15,255,0,0,128) };
    blt_raster_tri(fb,&heap,&h,A,2, NULL);
    blt_vtx_t B[6] = {
        V(10,10,0,255,0,128), V(20,10,0,255,0,128), V(20,20,0,255,0,128),
        V(10,10,0,255,0,128), V(20,20,0,255,0,128), V(10,20,0,255,0,128) };
    blt_raster_tri(fb,&heap,&h,B,2, NULL);
    uint16_t afterA = blt_blend565(0xF800, 0x001F, 128);       /* red over blue   */
    uint16_t expect = blt_blend565(0x07E0, afterA, 128);       /* green over that */
    assert(fb[12*BLT_FB_WIDTH+12]==expect);                    /* overlap         */
    assert(fb[6*BLT_FB_WIDTH+6]==afterA);                      /* A-only region   */
    printf("test_overlap_order OK\n");
}

/* two overlapping ADD quads: the overlap must be saturating add of both sources. */
static void test_additive(void){
    uint8_t tex[2]; blt_surface_heap_t heap = mk_white_tex(tex);
    uint16_t fb[BLT_FB_WIDTH*BLT_FB_HEIGHT]; memset(fb,0,sizeof fb);   /* black bg */
    blt_cmd_t h = mk_hdr(BLT_BLEND_ADD);
    blt_vtx_t A[6] = {
        V(5,5,128,0,0,255),  V(15,5,128,0,0,255),  V(15,15,128,0,0,255),
        V(5,5,128,0,0,255),  V(15,15,128,0,0,255), V(5,15,128,0,0,255) };
    blt_raster_tri(fb,&heap,&h,A,2, NULL);
    blt_vtx_t B[6] = {
        V(10,10,128,0,0,255), V(20,10,128,0,0,255), V(20,20,128,0,0,255),
        V(10,10,128,0,0,255), V(20,20,128,0,0,255), V(10,20,128,0,0,255) };
    blt_raster_tri(fb,&heap,&h,B,2, NULL);
    uint16_t src    = blt_tint565(0xFFFF,128,0,0);             /* white*red-mod   */
    uint16_t afterA = blt_add565(src, 0x0000);
    uint16_t expect = blt_add565(src, afterA);
    assert(fb[12*BLT_FB_WIDTH+12]==expect);                    /* overlap (2x add)*/
    assert(fb[6*BLT_FB_WIDTH+6]==afterA);                      /* A-only          */
    printf("test_additive OK\n");
}

/* triangle straddling the left edge (negative X): no OOB write; on-screen pixels
 * inside the clipped triangle are filled, pixels outside stay background. */
static void test_offscreen_clip(void){
    uint8_t tex[2]; blt_surface_heap_t heap = mk_white_tex(tex);
    uint16_t fb[BLT_FB_WIDTH*BLT_FB_HEIGHT]; memset(fb,0,sizeof fb);
    blt_cmd_t h = mk_hdr(BLT_BLEND_COPY);
    /* right triangle: (-10,5),(10,5),(10,25) — left half is off-screen (x<0) */
    blt_vtx_t T[3] = { V(-10,5,255,0,0,255), V(10,5,255,0,0,255), V(10,25,255,0,0,255) };
    blt_raster_tri(fb,&heap,&h,T,1, NULL);
    assert(fb[10*BLT_FB_WIDTH+5]==0xF800);   /* interior on-screen pixel filled  */
    assert(fb[0]==0x0000);                   /* (0,0) above the triangle: bg      */
    assert(fb[10*BLT_FB_WIDTH+12]==0x0000);  /* x>10 (right of the triangle): bg  */
    printf("test_offscreen_clip OK\n");
}

/* 45°-rotated textured quad over a 4x4 checkerboard: the centre samples a checker
 * texel (covered, non-background); a far corner stays background. */
static void test_rotated_quad(void){
    uint8_t heapbuf[4*4*2];
    for(int y=0;y<4;y++) for(int x=0;x<4;x++){
        uint16_t c = ((x+y)&1) ? 0x07E0 : 0xF800;   /* green / red checker */
        heapbuf[(y*4+x)*2]=c&0xFF; heapbuf[(y*4+x)*2+1]=c>>8;
    }
    blt_surface_heap_t heap = { heapbuf, sizeof heapbuf, 0, 0 };
    uint16_t fb[BLT_FB_WIDTH*BLT_FB_HEIGHT]; memset(fb,0,sizeof fb);
    blt_cmd_t h; memset(&h,0,sizeof h);
    h.opcode=BLT_OP_TRILIST; h.blend_mode=BLT_BLEND_COPY; h.format=BLT_FMT_RGB565;
    h.src_off=0; h.src_stride=8; h.src_x=4; h.src_y=4; h.alpha=255;
    /* diamond (square rotated 45°) around (30,30), UV corners map the full 4x4 tex.
     * white vertex colour so the texel passes through untinted. u,v are 12.4. */
#define VT(px,py,u,v) (blt_vtx_t){ (int16_t)((px)<<4),(int16_t)((py)<<4), \
                                   (uint16_t)(u),(uint16_t)(v), BLT_RGBA(255,255,255,255), 0 }
    blt_vtx_t q[6] = {
        VT(30,20, 0,0),  VT(40,30, 64,0),  VT(30,40, 64,64),
        VT(30,20, 0,0),  VT(30,40, 64,64), VT(20,30, 0,64) };
#undef VT
    blt_raster_tri(fb,&heap,&h,q,2, NULL);
    uint16_t c = fb[30*BLT_FB_WIDTH+30];
    assert(c==0xF800 || c==0x07E0);          /* centre covered by a checker texel */
    assert(fb[0]==0x0000);                   /* far corner: background            */
    printf("test_rotated_quad OK\n");
}

int main(void){ test_solid_red_quad_copy(); test_alpha_blend_half();
    test_trilist_via_execute();
    test_overlap_order(); test_additive(); test_offscreen_clip(); test_rotated_quad();
    printf("ALL blt_tri tests OK\n"); return 0; }
