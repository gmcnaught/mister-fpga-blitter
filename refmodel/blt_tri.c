/*
 *  blt_tri.c — reference textured-triangle rasterizer for BLT_OP_TRILIST.
 *
 *  THIS IS THE GOLDEN SPEC the RTL blt_tri module must reproduce bit-for-bit.
 *  Everything here is deterministic and integer-only so the fabric can match:
 *    - edge functions in 12.4 fixed-point (integer barycentric),
 *    - CCW-normalize by swapping b,c when the signed area is negative,
 *    - pixel-center sampling at ((px<<4)|8),
 *    - top-left fill rule via a -1 bias on non-top-left edges,
 *    - round-to-nearest attribute interpolation (divr),
 *    - nearest texel with clamp,
 *    - the blend-mode switch reuses the repo's canonical RGB565 helpers
 *      (blt_tint565 / blt_blend565 / blt_add565 / blt_mul565) — do NOT
 *      reimplement their rounding here.
 *
 *  Copyright (C) 2026 — GPL-3.0 (matches solarus-mister/fpga).
 */
#include "blt_tri.h"
#include <stdint.h>

#define SUB  4
#define ONE  (1<<SUB)
#define HALF (ONE>>1)

/* signed area x2 of triangle (a,b,c); >0 for CCW in a Y-down screen. */
static int64_t edge(int64_t ax,int64_t ay,int64_t bx,int64_t by,int64_t cx,int64_t cy){
    return (bx-ax)*(cy-ay) - (by-ay)*(cx-ax);
}
/* round-to-nearest signed divide, den>0. */
static int64_t divr(int64_t num, int64_t den){
    return (num>=0) ? (num + den/2)/den : -(((-num) + den/2)/den);
}
/* top-left rule: edge a->b is "inside" on E==0 iff it is a top or left edge. */
static int top_left(int64_t ax,int64_t ay,int64_t bx,int64_t by){
    return (ay==by && bx<ax) /*top*/ || (by>ay) /*left*/;
}
/* nearest-texel fetch (RGB565) with clamp to the texture rect [0,tw)x[0,th). */
static uint16_t tex_nearest(const blt_surface_heap_t *heap, const blt_cmd_t *h,
                            int u_fx, int v_fx){
    int tw=h->src_x, th=h->src_y;
    int tu=(u_fx+HALF)>>SUB, tv=(v_fx+HALF)>>SUB;
    if(tu<0)tu=0; else if(tu>=tw)tu=tw-1;
    if(tv<0)tv=0; else if(tv>=th)tv=th-1;
    const uint8_t *p = heap->base + h->src_off + (uint32_t)tv*h->src_stride + (uint32_t)tu*2u;
    return (uint16_t)(p[0] | (p[1]<<8));
}

void blt_raster_tri(uint16_t *fb, const blt_surface_heap_t *heap,
                    const blt_cmd_t *h, const blt_vtx_t *tris, int ntris){
    for(int t=0;t<ntris;t++){
        const blt_vtx_t *a=&tris[t*3+0], *b=&tris[t*3+1], *c=&tris[t*3+2];
        int64_t x0=a->x,y0=a->y, x1=b->x,y1=b->y, x2=c->x,y2=c->y;
        int64_t area = edge(x0,y0,x1,y1,x2,y2);
        if(area==0) continue;
        if(area<0){ const blt_vtx_t*tb2=b; b=c; c=tb2;
            int64_t tx=x1,ty=y1; x1=x2;y1=y2;x2=tx;y2=ty; area=-area; }
        int64_t lx = (x0<x1?(x0<x2?x0:x2):(x1<x2?x1:x2));
        int64_t hx = (x0>x1?(x0>x2?x0:x2):(x1>x2?x1:x2));
        int64_t ly = (y0<y1?(y0<y2?y0:y2):(y1<y2?y1:y2));
        int64_t hy = (y0>y1?(y0>y2?y0:y2):(y1>y2?y1:y2));
        int minx=(int)(lx>>SUB), maxx=(int)((hx+ONE-1)>>SUB);
        int miny=(int)(ly>>SUB), maxy=(int)((hy+ONE-1)>>SUB);
        if(minx<0)minx=0; if(miny<0)miny=0;
        if(maxx>=BLT_FB_WIDTH)maxx=BLT_FB_WIDTH-1; if(maxy>=BLT_FB_HEIGHT)maxy=BLT_FB_HEIGHT-1;
        int64_t bias0 = top_left(x1,y1,x2,y2)?0:-1; /* edge opposite vertex a (b->c) */
        int64_t bias1 = top_left(x2,y2,x0,y0)?0:-1; /* opposite b (c->a) */
        int64_t bias2 = top_left(x0,y0,x1,y1)?0:-1; /* opposite c (a->b) */
        for(int py=miny;py<=maxy;py++){
            int64_t sy=((int64_t)py<<SUB)|HALF;
            for(int px=minx;px<=maxx;px++){
                int64_t sx=((int64_t)px<<SUB)|HALF;
                int64_t w0=edge(x1,y1,x2,y2,sx,sy)+bias0;
                int64_t w1=edge(x2,y2,x0,y0,sx,sy)+bias1;
                int64_t w2=edge(x0,y0,x1,y1,sx,sy)+bias2;
                if(w0<0||w1<0||w2<0) continue;
                int u=(int)divr(w0*a->u + w1*b->u + w2*c->u, area);
                int v=(int)divr(w0*a->v + w1*b->v + w2*c->v, area);
                int cr=(int)divr(w0*(a->rgba&0xff)+w1*(b->rgba&0xff)+w2*(c->rgba&0xff),area);
                int cg=(int)divr(w0*((a->rgba>>8)&0xff)+w1*((b->rgba>>8)&0xff)+w2*((c->rgba>>8)&0xff),area);
                int cb=(int)divr(w0*((a->rgba>>16)&0xff)+w1*((b->rgba>>16)&0xff)+w2*((c->rgba>>16)&0xff),area);
                int ca=(int)divr(w0*((a->rgba>>24)&0xff)+w1*((b->rgba>>24)&0xff)+w2*((c->rgba>>24)&0xff),area);
                uint16_t texel=tex_nearest(heap,h,u,v);
                uint16_t src=blt_tint565(texel,(uint8_t)cr,(uint8_t)cg,(uint8_t)cb);
                uint16_t *dp=&fb[py*BLT_FB_WIDTH+px];
                int ea=(ca*h->alpha)/255;
                switch(h->blend_mode){
                  case BLT_BLEND_COPY:        *dp=src; break;
                  case BLT_BLEND_CONST_ALPHA: *dp=blt_blend565(src,*dp,(uint8_t)ea); break;
                  case BLT_BLEND_ADD:         *dp=blt_add565(src,*dp); break;
                  case BLT_BLEND_MULTIPLY:    *dp=blt_mul565(src,*dp); break;
                  case BLT_BLEND_COLORKEY:    if(texel!=h->colorkey)*dp=src; break;
                  default:                    *dp=src; break;
                }
            }
        }
    }
}
