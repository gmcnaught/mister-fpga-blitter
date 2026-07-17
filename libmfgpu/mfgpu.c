/*
 *  mfgpu.c — mfgpu context + frame lifecycle. The batch geometry stage
 *  (mfgpu_submit_batch) is implemented in Task C3 on top of mfgpu_xform.
 *  Copyright (C) 2026 — GPL-3.0.
 */
#include "mfgpu.h"
#include "mfgpu_xform.h"
#include <stdlib.h>
#include <string.h>

struct mfgpu_ctx { blt_emitter_t *e; };

mfgpu_t *mfgpu_create(blt_emitter_t *e){
    mfgpu_t *m = (mfgpu_t*)calloc(1, sizeof *m);
    if (m) m->e = e;
    return m;
}
void mfgpu_frame_begin(mfgpu_t *m){ if (m && m->e) blt_begin_frame(m->e, 0, 0, 0); }

int mfgpu_submit_batch(mfgpu_t *m, const mfgpu_batch_t *b){
    if (!m || !m->e || !b) return -1;
    if (b->nverts <= 0 || !b->verts) return 0;   /* nothing to draw */

    /* 1) transform every vertex: model -> screen 12.4 + texel 12.4 */
    blt_vtx_t *xf = (blt_vtx_t*)malloc((size_t)b->nverts * sizeof *xf);
    if (!xf) return -1;
    for (int i=0;i<b->nverts;i++)
        mfgpu_xform_vtx(b->mvp, &b->verts[i], &xf[i],
                        b->screen_w, b->screen_h, b->tex_w, b->tex_h);

    /* 2) assemble triangles (indexed or sequential) and cull */
    int ntri_in = b->indices ? (b->nindices/3) : (b->nverts/3);
    blt_vtx_t *tris = (blt_vtx_t*)malloc((size_t)(ntri_in>0?ntri_in:1) * 3 * sizeof *tris);
    if (!tris) { free(xf); return -1; }
    const int32_t vw = (int32_t)b->screen_w << 4;   /* viewport bounds in 12.4 */
    const int32_t vh = (int32_t)b->screen_h << 4;
    int nsurv = 0;
    for (int t=0;t<ntri_in;t++){
        int i0,i1,i2;
        if (b->indices){ i0=b->indices[t*3]; i1=b->indices[t*3+1]; i2=b->indices[t*3+2]; }
        else           { i0=t*3;             i1=t*3+1;             i2=t*3+2;             }
        if (i0>=b->nverts || i1>=b->nverts || i2>=b->nverts) continue;
        blt_vtx_t a=xf[i0], bb=xf[i1], c=xf[i2];
        /* degenerate cull (zero 2x-area). NOT a back-face cull: the fabric
         * normalizes winding, so both orientations are valid. */
        long area = (long)(bb.x-a.x)*(c.y-a.y) - (long)(bb.y-a.y)*(c.x-a.x);
        if (area == 0) continue;
        /* 2D bbox cull vs the viewport [0,vw]x[0,vh] (all 12.4) */
        int32_t minx=a.x, maxx=a.x, miny=a.y, maxy=a.y;
        if (bb.x<minx)minx=bb.x; if (c.x<minx)minx=c.x;
        if (bb.x>maxx)maxx=bb.x; if (c.x>maxx)maxx=c.x;
        if (bb.y<miny)miny=bb.y; if (c.y<miny)miny=c.y;
        if (bb.y>maxy)maxy=bb.y; if (c.y>maxy)maxy=c.y;
        if (maxx<0 || minx>vw || maxy<0 || miny>vh) continue;
        tris[nsurv*3+0]=a; tris[nsurv*3+1]=bb; tris[nsurv*3+2]=c; nsurv++;
    }

    /* 3) push survivors + emit one TRILIST header for the batch */
    int rc = 0;
    if (nsurv > 0){
        uint32_t eoff = blt_push_tris(m->e, tris, nsurv);
        if (eoff == 0xFFFFFFFFu) rc = -1;
        else {
            blt_surface_ref_t tex; memset(&tex, 0, sizeof tex);
            tex.off=b->tex_off; tex.stride=b->tex_stride;
            tex.w=b->tex_w; tex.h=b->tex_h; tex.format=b->tex_format; tex.valid=1;
            rc = blt_trilist(m->e, tex, b->blend, 0, 255, eoff, nsurv, /*flags=*/0);
        }
    }
    free(tris); free(xf);
    return rc;
}

void mfgpu_frame_end(mfgpu_t *m){ if (m && m->e) blt_end_frame(m->e); }
void mfgpu_destroy(mfgpu_t *m){ free(m); }
