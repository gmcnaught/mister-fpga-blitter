/*
 *  mfgpu.h — MiSTer FPGA GPU front-end (A9 geometry stage).
 *
 *  Transforms GameMaker-shaped 2D batches (textured triangles, an MVP matrix,
 *  a blend mode) into BLT_OP_TRILIST display lists via the host emitter. The A9
 *  (NEON) does transform / clip / cull here; the fabric does the pixel back-end
 *  (interpolate, sample, modulate, blend, framebuffer).
 *
 *  mfgpu_batch_t is the contract with the future gmloader interceptor.
 *  Copyright (C) 2026 — GPL-3.0.
 */
#ifndef MFGPU_H
#define MFGPU_H
#include <stdint.h>
#include <stddef.h>
#include "blt_emitter.h"

/* GM-batch vertex, model space; u,v are normalized 0..1 texture-page coords. */
typedef struct { float x, y, z, u, v; uint32_t rgba; } mfgpu_in_vtx_t;

typedef struct {
    const mfgpu_in_vtx_t *verts; int nverts;
    const uint16_t *indices; int nindices;    /* NULL => sequential triangles     */
    float mvp[16];                             /* column-major 4x4 (GLES convention)*/
    uint32_t tex_off; uint16_t tex_w, tex_h, tex_stride; uint8_t tex_format;
    uint8_t blend;                             /* BLT_BLEND_*                       */
    int screen_w, screen_h;                    /* viewport, for cull + NDC->screen  */
} mfgpu_batch_t;

typedef struct mfgpu_ctx mfgpu_t;

mfgpu_t *mfgpu_create(blt_emitter_t *e);
void     mfgpu_frame_begin(mfgpu_t *m);
int      mfgpu_submit_batch(mfgpu_t *m, const mfgpu_batch_t *b);  /* 0 = ok */
void     mfgpu_frame_end(mfgpu_t *m);
void     mfgpu_destroy(mfgpu_t *m);

#endif /* MFGPU_H */
