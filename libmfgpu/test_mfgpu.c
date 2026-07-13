/*  test_mfgpu.c — libmfgpu lifecycle smoke test + end-to-end batch golden.
 *  The batch path is exercised all the way through the emitter and blt_execute:
 *  mfgpu_batch_t -> transform/cull -> BLT_OP_TRILIST -> reference framebuffer.
 *  GPL-3.0. */
#include "mfgpu.h"
#include "blt_wire.h"      /* blt_unpack_cmd, BLT_CMD_BYTES */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void test_smoke(void){
    static uint8_t ring[4096], heap[4096];
    blt_emitter_t e;
    blt_emitter_init(&e, ring, sizeof ring, heap, sizeof heap);
    mfgpu_t *m = mfgpu_create(&e);
    assert(m != NULL);
    mfgpu_frame_begin(m);
    mfgpu_batch_t empty = {0};
    assert(mfgpu_submit_batch(m, &empty) == 0);
    mfgpu_frame_end(m);
    mfgpu_destroy(m);
    printf("test_mfgpu smoke OK\n");
}

/* Run the emitter's ring through blt_execute into a fresh framebuffer. */
static uint16_t *run_ring(const uint8_t *ring, int ncmds, const uint8_t *src, size_t srclen){
    uint16_t *fb = (uint16_t*)calloc(BLT_FB_PIXELS, sizeof(uint16_t));
    blt_cmd_t cmds[128];
    int n = ncmds > 128 ? 128 : ncmds;
    for (int i=0;i<n;i++) blt_unpack_cmd(ring + (size_t)i*BLT_CMD_BYTES, &cmds[i]);
    blt_surface_heap_t h = { src, srclen, 0, 0 };
    blt_execute(fb, &h, cmds, n);
    return fb;
}

static void test_batch_end_to_end(void){
    static uint8_t ring[8192], dummy_heap[64];
    /* one source-DDR image holds both the vertex entries (blt_push_tris, from 0)
     * and the texture (staged high). blt_execute reads both from this base. */
    static uint8_t srcdram[8192];
    memset(srcdram, 0, sizeof srcdram);
    const uint32_t TEX_OFF = 4096;
    srcdram[TEX_OFF] = 0xff; srcdram[TEX_OFF+1] = 0xff;   /* 1x1 white RGB565 */

    blt_emitter_t e;
    blt_emitter_init(&e, ring, sizeof ring, dummy_heap, sizeof dummy_heap);
    blt_vtx_buf_init(&e, srcdram, sizeof srcdram);
    mfgpu_t *m = mfgpu_create(&e);

    float I[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    /* NDC quad (-0.5,-0.5)..(0.5,0.5) -> screen (80,60)..(240,180) in 320x240 */
    mfgpu_in_vtx_t verts[4] = {
        {-0.5f,-0.5f,0, 0,0, BLT_RGBA(255,0,0,255)},
        { 0.5f,-0.5f,0, 0,0, BLT_RGBA(255,0,0,255)},
        { 0.5f, 0.5f,0, 0,0, BLT_RGBA(255,0,0,255)},
        {-0.5f, 0.5f,0, 0,0, BLT_RGBA(255,0,0,255)},
    };
    uint16_t idx[6] = {0,1,2, 0,2,3};
    mfgpu_batch_t batch; memset(&batch, 0, sizeof batch);
    batch.verts=verts; batch.nverts=4; batch.indices=idx; batch.nindices=6;
    memcpy(batch.mvp, I, sizeof I);
    batch.tex_off=TEX_OFF; batch.tex_w=1; batch.tex_h=1; batch.tex_stride=2;
    batch.tex_format=BLT_FMT_RGB565; batch.blend=BLT_BLEND_COPY;
    batch.screen_w=320; batch.screen_h=240;

    mfgpu_frame_begin(m);
    assert(mfgpu_submit_batch(m, &batch) == 0);
    int cc = e.cmd_count;
    mfgpu_frame_end(m);

    uint16_t *fb = run_ring(ring, cc, srcdram, sizeof srcdram);
    assert(fb[120*BLT_FB_WIDTH + 160] == 0xF800);   /* screen centre is red */
    assert(fb[10*BLT_FB_WIDTH + 10]   == 0x0000);   /* outside the quad: bg */
    free(fb);

    /* off-screen batch (all verts at NDC ~ 2) must be fully culled: nothing
     * pushed to the vertex buffer and no TRILIST header emitted. */
    mfgpu_in_vtx_t off[4] = {
        {1.5f,1.5f,0,0,0, BLT_RGBA(0,255,0,255)}, {2.0f,1.5f,0,0,0, BLT_RGBA(0,255,0,255)},
        {2.0f,2.0f,0,0,0, BLT_RGBA(0,255,0,255)}, {1.5f,2.0f,0,0,0, BLT_RGBA(0,255,0,255)},
    };
    mfgpu_batch_t ob = batch; ob.verts = off;
    mfgpu_frame_begin(m);
    size_t vtx_before = e.vtx_used; int cc_before = e.cmd_count;
    assert(mfgpu_submit_batch(m, &ob) == 0);
    assert(e.vtx_used  == vtx_before);   /* nothing pushed */
    assert(e.cmd_count == cc_before);    /* no header emitted */
    mfgpu_frame_end(m);

    mfgpu_destroy(m);
    printf("test_batch_end_to_end OK\n");
}

int main(void){
    test_smoke();
    test_batch_end_to_end();
    printf("ALL mfgpu tests OK\n");
    return 0;
}
