/*
 *  test_emitter.c — host unit tests for the emitter + wire codec, verified
 *  against the reference model. No hardware. `make test`.
 *
 *  Strategy: build a scene with the emitter, decode its ring back through the
 *  wire codec, execute it on the reference model (emitter heap), and assert the
 *  framebuffer is identical to the SAME scene built as hand-written commands.
 *  This closes the loop: emitter -> wire -> refmodel == intended pixels.
 *  GPL-3.0.
 */
#include "blt_emitter.h"
#include "blt_wire.h"
#include "blitter_ref.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail = 0, g_checks = 0;
#define CHECK(c,m) do{ g_checks++; if(!(c)){ printf("  FAIL: %s (%s:%d)\n",m,__FILE__,__LINE__); g_fail++; } }while(0)

#define RING_CAP (64*1024)
#define HEAP_CAP (1*1024*1024)

/* run a command ring (decoded) onto a fresh framebuffer over `heap` */
static uint16_t *run_ring(const uint8_t *ring, int ncmds, const uint8_t *heap,
                          size_t heap_len, uint16_t clear)
{
    uint16_t *fb = malloc(BLT_FB_PIXELS * sizeof(uint16_t));
    for (int i=0;i<BLT_FB_PIXELS;i++) fb[i]=clear;
    blt_cmd_t *cmds = malloc((size_t)ncmds * sizeof(blt_cmd_t));
    for (int i=0;i<ncmds;i++) blt_unpack_cmd(ring + (size_t)i*BLT_CMD_BYTES, &cmds[i]);
    blt_surface_heap_t h = { heap, heap_len, NULL, NULL };
    blt_execute(fb, &h, cmds, ncmds);
    free(cmds);
    return fb;
}

/* ----- wire codec round-trips a command field-for-field ----------------- */
static void test_wire_roundtrip(void)
{
    printf("test_wire_roundtrip\n");
    blt_cmd_t a; memset(&a,0,sizeof(a));
    a.opcode=BLT_OP_BLIT; a.blend_mode=BLT_BLEND_CONST_ALPHA; a.format=BLT_FMT_RGB565;
    a.flags=BLT_F_HFLIP|BLT_F_COLORKEY; a.src_off=0x12345; a.src_stride=640;
    a.src_x=7; a.src_y=9; a.w=33; a.h=21; a.dst_x=-5; a.dst_y=-300;
    a.colorkey=0xF81F; a.alpha=200; a.color=0x07E0;
    uint8_t w[BLT_CMD_BYTES]; blt_cmd_t b;
    blt_pack_cmd(&a,w); blt_unpack_cmd(w,&b);
    CHECK(a.opcode==b.opcode && a.blend_mode==b.blend_mode && a.format==b.format
          && a.flags==b.flags, "header fields round-trip");
    CHECK(a.src_off==b.src_off && a.src_stride==b.src_stride
          && a.src_x==b.src_x && a.src_y==b.src_y, "source fields round-trip");
    CHECK(a.w==b.w && a.h==b.h, "size round-trips");
    CHECK(a.dst_x==b.dst_x && a.dst_y==b.dst_y, "signed dst round-trips (negatives)");
    CHECK(a.colorkey==b.colorkey && a.alpha==b.alpha && a.color==b.color,
          "key/alpha/color round-trip");
}

/* ----- emitter scene == hand-built scene (through refmodel) -------------- */
static void test_emitter_vs_handbuilt(void)
{
    printf("test_emitter_vs_handbuilt\n");
    uint8_t *ring = malloc(RING_CAP), *heap = malloc(HEAP_CAP);
    blt_emitter_t e; blt_emitter_init(&e, ring, RING_CAP, heap, HEAP_CAP);

    /* surface A: 8x8, each pixel = 0x100 + index (all distinct) */
    uint16_t A[64]; for (int i=0;i<64;i++) A[i]=(uint16_t)(0x100+i);
    /* surface B: 4x4 green, pixel (0,0) = colorkey 0x0000 */
    uint16_t B[16]; for (int i=0;i<16;i++) B[i]=0x07E0; B[0]=0x0000;

    blt_surface_ref_t ra = blt_upload(&e, A, 8, 8, 16);
    blt_surface_ref_t rb = blt_upload(&e, B, 4, 4, 8);
    CHECK(ra.valid && rb.valid, "uploads valid");
    CHECK(rb.off >= ra.off + 8*8*2, "second upload does not overlap first");

    const uint16_t CLEAR = 0x0008;
    blt_begin_frame(&e, 0, 1, CLEAR);
    blt_fill(&e, 0, 0, 20, 20, 0x1111);
    blt_blit_copy(&e, ra, 50, 40);
    blt_blit(&e, rb, 0, 0, 4, 4, 10, 10, BLT_BLEND_COLORKEY, 0x0000, 0, 0);
    blt_blit(&e, ra, 0, 0, 8, 8, 100, 100, BLT_BLEND_CONST_ALPHA, 0, 128, 0);
    blt_blit(&e, ra, 0, 0, 8, 8, 200, 30, BLT_BLEND_COPY, 0, 0, BLT_F_HFLIP);
    blt_end_frame(&e);
    CHECK(!e.overflow, "no ring/heap overflow");
    CHECK(e.cmd_count == 6, "5 ops + END counted");
    CHECK(e.submit_seq == 1, "submit_seq bumped");

    /* (1) emitter ring -> wire -> refmodel */
    uint16_t *fbA = run_ring(ring, e.cmd_count, heap, e.heap_used,
                             (e.flags & 1) ? e.clear_color : 0);

    /* (2) hand-built equivalent over the SAME heap/offsets */
    blt_cmd_t h[6]; memset(h,0,sizeof(h));
    h[0].opcode=BLT_OP_FILL; h[0].dst_x=0; h[0].dst_y=0; h[0].w=20; h[0].h=20; h[0].color=0x1111;
    h[1].opcode=BLT_OP_BLIT; h[1].blend_mode=BLT_BLEND_COPY; h[1].format=BLT_FMT_RGB565;
    h[1].src_off=ra.off; h[1].src_stride=ra.stride; h[1].w=8; h[1].h=8; h[1].dst_x=50; h[1].dst_y=40;
    h[2].opcode=BLT_OP_BLIT; h[2].blend_mode=BLT_BLEND_COLORKEY; h[2].format=BLT_FMT_RGB565;
    h[2].src_off=rb.off; h[2].src_stride=rb.stride; h[2].w=4; h[2].h=4; h[2].dst_x=10; h[2].dst_y=10; h[2].colorkey=0x0000;
    h[3].opcode=BLT_OP_BLIT; h[3].blend_mode=BLT_BLEND_CONST_ALPHA; h[3].format=BLT_FMT_RGB565;
    h[3].src_off=ra.off; h[3].src_stride=ra.stride; h[3].w=8; h[3].h=8; h[3].dst_x=100; h[3].dst_y=100; h[3].alpha=128;
    h[4].opcode=BLT_OP_BLIT; h[4].blend_mode=BLT_BLEND_COPY; h[4].format=BLT_FMT_RGB565;
    h[4].src_off=ra.off; h[4].src_stride=ra.stride; h[4].w=8; h[4].h=8; h[4].dst_x=200; h[4].dst_y=30; h[4].flags=BLT_F_HFLIP;
    h[5].opcode=BLT_OP_END;

    uint16_t *fbB = malloc(BLT_FB_PIXELS*sizeof(uint16_t));
    for (int i=0;i<BLT_FB_PIXELS;i++) fbB[i]=CLEAR;
    blt_surface_heap_t hp = { heap, e.heap_used, NULL, NULL };
    blt_execute(fbB, &hp, h, 6);

    CHECK(memcmp(fbA, fbB, BLT_FB_PIXELS*sizeof(uint16_t))==0,
          "emitter frame == hand-built frame (bit-exact via refmodel)");

    /* spot checks of intent */
    CHECK(fbA[5*BLT_FB_WIDTH+5]==0x1111, "fill landed");
    CHECK(fbA[40*BLT_FB_WIDTH+50]==A[0], "copy A landed at (50,40)");
    CHECK(fbA[10*BLT_FB_WIDTH+10]==0x1111, "colorkey skipped B(0,0) -> underlying fill shows");
    CHECK(fbA[11*BLT_FB_WIDTH+11]==0x07E0, "colorkey kept B(1,1) green");

    free(fbA); free(fbB); free(ring); free(heap);
}

/* ----- static atlas uploaded once, reused across frames ----------------- */
static void test_heap_persistence(void)
{
    printf("test_heap_persistence\n");
    uint8_t *ring = malloc(RING_CAP), *heap = malloc(HEAP_CAP);
    blt_emitter_t e; blt_emitter_init(&e, ring, RING_CAP, heap, HEAP_CAP);
    uint16_t T[4]={0xABCD,0xABCD,0xABCD,0xABCD};
    blt_surface_ref_t t = blt_upload(&e, T, 2, 2, 4);
    size_t used_after_upload = e.heap_used;

    for (int f=0; f<3; f++) {
        blt_begin_frame(&e, f&1, 0, 0);
        blt_blit_copy(&e, t, f*4, f*4);   /* reuse handle, no re-upload */
        blt_end_frame(&e);
        uint16_t *fb = run_ring(ring, e.cmd_count, heap, e.heap_used, 0);
        CHECK(fb[(f*4)*BLT_FB_WIDTH + f*4]==0xABCD, "reused atlas blits each frame");
        free(fb);
    }
    CHECK(e.heap_used == used_after_upload, "heap not grown by reuse (upload-once)");
    CHECK(e.submit_seq == 3, "three frames submitted");
    free(ring); free(heap);
}

/* ----- overflow is reported, not a buffer overrun ----------------------- */
static void test_overflow_guard(void)
{
    printf("test_overflow_guard\n");
    uint8_t *ring = malloc(RING_CAP);
    static uint16_t big[256*256];
    uint8_t small_heap[64];
    blt_emitter_t e; blt_emitter_init(&e, ring, RING_CAP, small_heap, sizeof(small_heap));
    blt_surface_ref_t r = blt_upload(&e, big, 256, 256, 512);
    CHECK(!r.valid && e.overflow, "oversized upload flagged, not overrun");

    /* tiny ring: ensure emit stops flagging instead of writing past cap */
    blt_emitter_t e2; blt_emitter_init(&e2, ring, BLT_CMD_BYTES, small_heap, sizeof(small_heap));
    blt_begin_frame(&e2, 0, 0, 0);
    blt_fill(&e2, 0,0,1,1, 0x1);   /* fits exactly one slot */
    blt_fill(&e2, 0,0,1,1, 0x2);   /* should overflow */
    CHECK(e2.overflow, "ring overflow flagged");
    free(ring);
}

/* ----- BLT_OP_TRILIST: push vertices + emit header, render via blt_execute --- */
static void test_emit_trilist_roundtrip(void)
{
    printf("test_emit_trilist_roundtrip\n");
    uint8_t *ring = malloc(RING_CAP);
    uint8_t dummy_heap[64];
    /* One source-DDR image holds BOTH the vertex entries (blt_push_tris, from
     * offset 0) and the texture (staged at a high offset). blt_execute reads
     * both from this single heap base. */
    static uint8_t srcdram[4096];
    memset(srcdram, 0, sizeof srcdram);
    const uint32_t TEX_OFF = 2048;
    srcdram[TEX_OFF] = 0xff; srcdram[TEX_OFF+1] = 0xff;   /* 1x1 white RGB565 */

    blt_emitter_t e;
    blt_emitter_init(&e, ring, RING_CAP, dummy_heap, sizeof dummy_heap);
    blt_vtx_buf_init(&e, srcdram, sizeof srcdram);
    blt_begin_frame(&e, 0, 0, 0);

    /* two-triangle red quad at (5,5)-(15,15); white texel modulated by red vtx color */
#define VV(px,py,cr,cg,cb,ca) (blt_vtx_t){ (int16_t)((px)<<4),(int16_t)((py)<<4),0,0, BLT_RGBA(cr,cg,cb,ca),0 }
    blt_vtx_t tris[6] = {
        VV(5,5,255,0,0,255),  VV(15,5,255,0,0,255),  VV(15,15,255,0,0,255),
        VV(5,5,255,0,0,255),  VV(15,15,255,0,0,255), VV(5,15,255,0,0,255),
    };
#undef VV
    uint32_t eoff = blt_push_tris(&e, tris, 2);
    CHECK(eoff == 0, "first push lands at offset 0");

    blt_surface_ref_t tex = { .off=TEX_OFF, .stride=2, .w=1, .h=1,
                              .format=BLT_FMT_RGB565, .valid=1,
                              .sdram_off=BLT_ALLOC_FAIL };
    CHECK(blt_trilist(&e, tex, BLT_BLEND_COPY, 0, 255, eoff, 2, 0) == 0, "blt_trilist emit ok");
    CHECK(!e.overflow, "no overflow");

    uint16_t *fb = run_ring(ring, e.cmd_count, srcdram, sizeof srcdram, 0x0000);
    CHECK(fb[10*BLT_FB_WIDTH+10]==0xF800, "interior red via emitted TRILIST");
    CHECK(fb[10*BLT_FB_WIDTH+15]==0x0000, "right edge exclusive (top-left rule)");
    free(fb);
    free(ring);
}

/* ----- BLT_OP_SET_TARGET: opcode + target id round-trip through the wire --- */
static int test_set_target_roundtrip(void) {
    uint8_t buf[BLT_CMD_BYTES];
    blt_cmd_t c = {0}; c.opcode = BLT_OP_SET_TARGET; c.color = BLT_TARGET_APPSURF;
    blt_pack_cmd(&c, buf);
    blt_cmd_t d = {0}; blt_unpack_cmd(buf, &d);
    if (d.opcode != BLT_OP_SET_TARGET) { printf("FAIL opcode %u\n", d.opcode); return 1; }
    if ((d.color & 0xFF) != BLT_TARGET_APPSURF) { printf("FAIL target %u\n", d.color); return 1; }
    printf("PASS set_target_roundtrip\n"); return 0;
}

int main(void)
{
    printf("=== emitter + wire codec tests ===\n");
    test_wire_roundtrip();
    test_emitter_vs_handbuilt();
    test_heap_persistence();
    test_overflow_guard();
    test_emit_trilist_roundtrip();
    g_checks++; if (test_set_target_roundtrip()) g_fail++;
    printf("=== %d checks, %d failures ===\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
