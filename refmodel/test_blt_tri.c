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
#define V(px,py,cr,cg,cb,ca) (blt_vtx_t){ (int16_t)((px)<<4),(int16_t)((py)<<4),0,0, BLT_RGBA(cr,cg,cb,ca),0 }

static void test_solid_red_quad_copy(void){
    uint8_t tex[2]; blt_surface_heap_t heap = mk_white_tex(tex);
    uint16_t fb[BLT_FB_WIDTH*BLT_FB_HEIGHT]; memset(fb,0,sizeof fb);
    blt_cmd_t h = mk_hdr(BLT_BLEND_COPY);
    /* axis-aligned 10x10 quad at (5,5), two tris, pure red */
    blt_vtx_t tris[6] = {
        V(5,5,255,0,0,255),  V(15,5,255,0,0,255),  V(15,15,255,0,0,255),
        V(5,5,255,0,0,255),  V(15,15,255,0,0,255), V(5,15,255,0,0,255),
    };
    blt_raster_tri(fb, &heap, &h, tris, 2);
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
    blt_raster_tri(fb, &heap, &h, tris, 2);
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

int main(void){ test_solid_red_quad_copy(); test_alpha_blend_half();
    test_trilist_via_execute();
    printf("ALL blt_tri tests OK\n"); return 0; }
