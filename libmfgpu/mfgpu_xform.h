/*
 *  mfgpu_xform.h — A9 per-vertex transform: model space -> screen 12.4 + texel.
 *  Copyright (C) 2026 — GPL-3.0.
 */
#ifndef MFGPU_XFORM_H
#define MFGPU_XFORM_H
#include "mfgpu.h"        /* mfgpu_in_vtx_t */
#include "blitter_ref.h"  /* blt_vtx_t      */

/* Transform one GM-batch vertex through the column-major MVP, perspective-divide,
 * map NDC -> screen with a Y-flip, and quantize to the fabric's fixed-point:
 * screen x/y = signed 12.4 (pixels<<4), texel u/v = unsigned 12.4 (texels<<4).
 * Vertex colour passes through unchanged. */
void mfgpu_xform_vtx(const float mvp[16], const mfgpu_in_vtx_t *in, blt_vtx_t *out,
                     int screen_w, int screen_h, uint16_t tex_w, uint16_t tex_h);

#endif /* MFGPU_XFORM_H */
