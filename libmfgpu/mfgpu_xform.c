/*  mfgpu_xform.c — scalar reference transform. A NEON path can be added later
 *  behind the same signature. Copyright (C) 2026 — GPL-3.0. */
#include "mfgpu_xform.h"
#include <math.h>

void mfgpu_xform_vtx(const float mvp[16], const mfgpu_in_vtx_t *in, blt_vtx_t *out,
                     int screen_w, int screen_h, uint16_t tex_w, uint16_t tex_h){
    float x=in->x, y=in->y, z=in->z;
    /* column-major 4x4 (GLES): clip = M * [x y z 1]^T (z-clip unused for 2D) */
    float cx = mvp[0]*x + mvp[4]*y + mvp[8]*z  + mvp[12];
    float cy = mvp[1]*x + mvp[5]*y + mvp[9]*z  + mvp[13];
    float cw = mvp[3]*x + mvp[7]*y + mvp[11]*z + mvp[15];
    float w  = (cw == 0.0f) ? 1.0f : cw;          /* guard divide-by-zero */
    double ndc_x = (double)cx / w;
    double ndc_y = (double)cy / w;
    /* NDC [-1,1] -> screen [0,screen], Y-flip (NDC +y is up, screen +y is down) */
    double sx = (ndc_x + 1.0) * 0.5 * screen_w;
    double sy = (1.0 - ndc_y) * 0.5 * screen_h;
    out->x = (int16_t)lround(sx * 16.0);
    out->y = (int16_t)lround(sy * 16.0);
    out->u = (uint16_t)lround((double)in->u * tex_w * 16.0);
    out->v = (uint16_t)lround((double)in->v * tex_h * 16.0);
    out->rgba  = in->rgba;
    out->_rsvd = 0;
}
