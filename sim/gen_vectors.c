/*
 *  gen_vectors.c — generate a blitter test vector for one scenario.
 *
 *  Builds a DDR image (source heap + command ring + control block + a cleared
 *  framebuffer), runs the SAME C reference model (../refmodel) to produce the
 *  golden framebuffer, and emits:
 *      ddr_init.hex      full memory image (MEM_QW qwords)
 *      fb_expected.hex   golden framebuffer (FB_QWORDS qwords)
 *  The Verilog testbench loads these, runs blitter_top, and diffs the result.
 *
 *  Layout constants MUST match ../rtl/blitter_defs.vh.
 *  Usage: ./gen_vectors <scenario>
 *  GPL-3.0.
 */
#include "blitter_ref.h"
#include "blt_wire.h"   /* canonical command packing (shared with host emitter) */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

/* ---- layout (mirror blitter_defs.vh) ----------------------------------- */
#define FB_W       320
#define FB_H       240
#define FB_QWORDS  19200
#define FB0_QW     0u
#define FB1_QW     19200u
#define VCTRL_QW   38400u
#define BLTCTRL_QW 38416u
#define RING_QW    38432u
#define SRC_QW     39000u
#define MEM_QW     65536u
#define C_SUBMIT   0u
#define C_CMDCOUNT 1u
#define C_TARGET   2u
#define C_CLEAR    3u
#define C_FLAGS    4u
#define C_DONE     5u
#define C_STATUS   6u

static uint64_t mem[MEM_QW];
static uint8_t  heap[1 << 20];
static size_t   heap_len = 0;
static blt_cmd_t cmds[256];
static int      ncmds = 0;

static uint16_t scn_clear      = 0x0000; /* refmodel fb init color           */
static uint16_t img_fb_init    = 0x0000; /* what to preload in the DDR image */
static uint32_t scn_flags      = 0;      /* bit0 = CLEAR-before-list          */
static uint16_t scn_clearcolor = 0x0000; /* hw clear color (when flags.CLEAR) */
static int      scn_target     = 0;

/* ---- builders ---------------------------------------------------------- */
static void heap_solid(int w, int h, uint16_t val) {
    heap_len = (size_t)w * h * 2;
    for (size_t i = 0; i < (size_t)w*h; i++) { heap[i*2]=val&0xFF; heap[i*2+1]=val>>8; }
}
static void heap_bytes(const uint8_t *b, size_t n) { memcpy(heap, b, n); heap_len = n; }

static blt_cmd_t *nc(void) { blt_cmd_t *c = &cmds[ncmds++]; memset(c,0,sizeof(*c)); return c; }
static void add_fill(int16_t x,int16_t y,uint16_t w,uint16_t h,uint16_t color){
    blt_cmd_t*c=nc(); c->opcode=BLT_OP_FILL; c->dst_x=x; c->dst_y=y; c->w=w; c->h=h; c->color=color;
}
static void add_blit(int16_t x,int16_t y,uint16_t w,uint16_t h,uint16_t stride,
                     uint8_t blend,uint8_t flags,uint16_t key,uint8_t alpha){
    blt_cmd_t*c=nc(); c->opcode=BLT_OP_BLIT; c->blend_mode=blend; c->flags=flags;
    c->format=BLT_FMT_RGB565; c->src_off=0; c->src_stride=stride;
    c->w=w; c->h=h; c->dst_x=x; c->dst_y=y; c->colorkey=key; c->alpha=alpha;
}
static void add_end(void){ nc()->opcode = BLT_OP_END; }

/* [MFGPU] TRILIST: heap = [1x1 RGB565 tex @0][ntris*3 verts @16]; header points
 * at the vertex entry buffer (dst_x|dst_y<<16 = entry_off, w = triangle count). */
static void add_trilist(uint16_t texpix, uint8_t blend, uint8_t hdr_alpha,
                        const blt_vtx_t *v, int ntris){
    const uint32_t eoff = 16;
    size_t vbytes = (size_t)ntris*3*sizeof(blt_vtx_t);
    memset(heap, 0, eoff + vbytes);
    heap[0]=texpix&0xFF; heap[1]=texpix>>8;
    memcpy(heap+eoff, v, vbytes);
    heap_len = eoff + vbytes;
    blt_cmd_t*c=nc(); c->opcode=BLT_OP_TRILIST; c->blend_mode=blend;
    c->format=BLT_FMT_RGB565; c->src_off=0; c->src_stride=2; c->src_x=1; c->src_y=1;
    c->w=(uint16_t)ntris; c->dst_x=(int16_t)(eoff&0xFFFF); c->dst_y=(int16_t)(eoff>>16);
    c->alpha=hdr_alpha;
}
#define TV(px,py,cr,cg,cb,ca) { (int16_t)((px)<<4),(int16_t)((py)<<4),0,0, BLT_RGBA(cr,cg,cb,ca),0 }

static void fill_fb_region(uint32_t base, uint16_t color){
    uint64_t w4 = (uint64_t)color | ((uint64_t)color<<16) |
                  ((uint64_t)color<<32) | ((uint64_t)color<<48);
    for (uint32_t i=0;i<FB_QWORDS;i++) mem[base+i]=w4;
}

/* ---- scenario table ---------------------------------------------------- */
static int build(const char*s){
    if(!strcmp(s,"fill")){ scn_clear=img_fb_init=0x0000;
        add_fill(10,20,4,3,0xF800); add_end(); }
    else if(!strcmp(s,"copy")){ scn_clear=img_fb_init=0x001F; heap_solid(8,8,0x07E0);
        add_blit(100,100,8,8,16,BLT_BLEND_COPY,0,0,0); add_end(); }
    else if(!strcmp(s,"colorkey")){ scn_clear=img_fb_init=0x001F;
        uint8_t b[4]={0x00,0x00,0xE0,0x07}; heap_bytes(b,4);
        add_blit(50,50,2,1,4,BLT_BLEND_COLORKEY,0,0x0000,0); add_end(); }
    else if(!strcmp(s,"alpha")){ scn_clear=img_fb_init=0x0000; heap_solid(2,2,0xFFFF);
        add_blit(0,0,2,2,4,BLT_BLEND_CONST_ALPHA,0,0,128); add_end(); }
    else if(!strcmp(s,"hflip")){ scn_clear=img_fb_init=0x0000;
        uint8_t b[8]={1,0,2,0,3,0,4,0}; heap_bytes(b,8);
        add_blit(0,0,2,2,4,BLT_BLEND_COPY,BLT_F_HFLIP,0,0); add_end(); }
    else if(!strcmp(s,"vflip")){ scn_clear=img_fb_init=0x0000;
        uint8_t b[8]={1,0,2,0,3,0,4,0}; heap_bytes(b,8);
        add_blit(0,0,2,2,4,BLT_BLEND_COPY,BLT_F_VFLIP,0,0); add_end(); }
    else if(!strcmp(s,"clip_neg")){ scn_clear=img_fb_init=0x0000; heap_solid(16,16,0xFFFF);
        add_blit(-4,-4,16,16,32,BLT_BLEND_COPY,0,0,0); add_end(); }
    else if(!strcmp(s,"clip_off")){ scn_clear=img_fb_init=0x1234; heap_solid(16,16,0xFFFF);
        add_blit(400,0,16,16,32,BLT_BLEND_COPY,0,0,0); add_end(); }
    else if(!strcmp(s,"overdraw")){ scn_clear=img_fb_init=0x0000;
        add_fill(0,0,4,4,0x00AA); add_fill(0,0,2,2,0x00BB); add_end(); }
    else if(!strcmp(s,"clear")){ /* prove hw CLEAR: image preloaded with garbage */
        scn_flags=1; scn_clearcolor=0x1234; scn_clear=0x1234; img_fb_init=0xAAAA;
        add_fill(0,0,2,2,0x00BB); add_end(); }
    else if(!strcmp(s,"target1")){ scn_target=1; scn_clear=img_fb_init=0x0007;
        heap_solid(4,4,0x07E0); add_blit(8,8,4,4,8,BLT_BLEND_COPY,0,0,0); add_end(); }
    else if(!strcmp(s,"tri_copy")){ scn_clear=img_fb_init=0x0000;
        static const blt_vtx_t v[6]={
            TV(5,5,255,0,0,255),  TV(15,5,255,0,0,255),  TV(15,15,255,0,0,255),
            TV(5,5,255,0,0,255),  TV(15,15,255,0,0,255), TV(5,15,255,0,0,255) };
        add_trilist(0xFFFF, BLT_BLEND_COPY, 255, v, 2); add_end(); }
    else if(!strcmp(s,"tri_alpha")){ scn_clear=img_fb_init=0x001F; /* blue bg */
        static const blt_vtx_t v[6]={
            TV(0,0,255,0,0,128),  TV(20,0,255,0,0,128),  TV(20,20,255,0,0,128),
            TV(0,0,255,0,0,128),  TV(20,20,255,0,0,128), TV(0,20,255,0,0,128) };
        add_trilist(0xFFFF, BLT_BLEND_CONST_ALPHA, 255, v, 2); add_end(); }
    else { fprintf(stderr,"unknown scenario '%s'\n",s); return -1; }
    return 0;
}

int main(int argc,char**argv){
    if(argc<2){ fprintf(stderr,"usage: %s <scenario>\n",argv[0]); return 2; }
    if(build(argv[1])) return 2;

    uint32_t target_base = scn_target ? FB1_QW : FB0_QW;

    /* --- DDR image --- */
    memset(mem,0,sizeof(mem));
    fill_fb_region(FB0_QW, (scn_target? 0x0000 : img_fb_init));
    fill_fb_region(FB1_QW, (scn_target? img_fb_init : 0x0000));
    /* control block */
    mem[BLTCTRL_QW+C_SUBMIT]   = 1;
    mem[BLTCTRL_QW+C_CMDCOUNT] = (uint32_t)ncmds;
    mem[BLTCTRL_QW+C_TARGET]   = (uint32_t)scn_target;
    mem[BLTCTRL_QW+C_CLEAR]    = scn_clearcolor;
    mem[BLTCTRL_QW+C_FLAGS]    = scn_flags;
    mem[BLTCTRL_QW+C_DONE]     = 0;
    mem[BLTCTRL_QW+C_STATUS]   = 0;
    /* ring (canonical packing; 32 LE bytes == 4 qwords on this host) */
    for(int i=0;i<ncmds;i++){ uint8_t w[BLT_CMD_BYTES]; blt_pack_cmd(&cmds[i], w);
        memcpy((uint8_t*)&mem[RING_QW + (uint32_t)i*4], w, BLT_CMD_BYTES); }
    /* source heap (byte view; host is little-endian) */
    memcpy((uint8_t*)&mem[SRC_QW], heap, heap_len);

    /* --- reference model golden output --- */
    static uint16_t fb[FB_W*FB_H];
    for(int i=0;i<FB_W*FB_H;i++) fb[i]=scn_clear;
    blt_surface_heap_t h = { heap, heap_len };
    blt_execute(fb, &h, cmds, ncmds);

    /* --- emit --- */
    FILE*fd=fopen("ddr_init.hex","w");
    for(uint32_t i=0;i<MEM_QW;i++) fprintf(fd,"%016llx\n",(unsigned long long)mem[i]);
    fclose(fd);

    FILE*fe=fopen("fb_expected.hex","w");
    for(uint32_t w=0; w<FB_QWORDS; w++){
        uint64_t q = (uint64_t)fb[w*4] | ((uint64_t)fb[w*4+1]<<16) |
                     ((uint64_t)fb[w*4+2]<<32) | ((uint64_t)fb[w*4+3]<<48);
        fprintf(fe,"%016llx\n",(unsigned long long)q);
    }
    fclose(fe);

    (void)target_base;
    printf("vectors: scenario=%s cmds=%d target=%d heap=%zuB\n",
           argv[1], ncmds, scn_target, heap_len);
    return 0;
}
