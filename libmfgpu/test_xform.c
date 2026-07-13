/*  test_xform.c — golden checks for mfgpu_xform_vtx. GPL-3.0. */
#include "mfgpu_xform.h"
#include <assert.h>
#include <stdio.h>

int main(void){
    /* identity MVP, column-major */
    float I[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    blt_vtx_t o;

    /* (a) NDC(-1,-1) with 320x240 viewport -> screen (0, 240) [Y-flipped] */
    mfgpu_in_vtx_t a = {-1.0f,-1.0f,0.0f, 0.0f,0.0f, 0};
    mfgpu_xform_vtx(I, &a, &o, 320, 240, 64, 64);
    assert(o.x == 0);
    assert(o.y == (int16_t)(240<<4));

    /* (b) NDC(0,0) -> screen center (160,120) */
    mfgpu_in_vtx_t b = {0.0f,0.0f,0.0f, 0.0f,0.0f, 0};
    mfgpu_xform_vtx(I, &b, &o, 320, 240, 64, 64);
    assert(o.x == (int16_t)(160<<4));
    assert(o.y == (int16_t)(120<<4));

    /* (c) UV(0.5,0.25) with a 64x64 texture -> texel (32,16) in 12.4 */
    mfgpu_in_vtx_t c = {0.0f,0.0f,0.0f, 0.5f,0.25f, 0xDEADBEEF};
    mfgpu_xform_vtx(I, &c, &o, 320, 240, 64, 64);
    assert(o.u == (uint16_t)(32<<4));
    assert(o.v == (uint16_t)(16<<4));
    assert(o.rgba == 0xDEADBEEF);   /* colour passes through */

    printf("test_xform OK\n");
    return 0;
}
