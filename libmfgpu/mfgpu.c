/*
 *  mfgpu.c — mfgpu context + frame lifecycle. The batch geometry stage
 *  (mfgpu_submit_batch) is implemented in Task C3 on top of mfgpu_xform.
 *  Copyright (C) 2026 — GPL-3.0.
 */
#include "mfgpu.h"
#include <stdlib.h>

struct mfgpu_ctx { blt_emitter_t *e; };

mfgpu_t *mfgpu_create(blt_emitter_t *e){
    mfgpu_t *m = (mfgpu_t*)calloc(1, sizeof *m);
    if (m) m->e = e;
    return m;
}
void mfgpu_frame_begin(mfgpu_t *m){ if (m && m->e) blt_begin_frame(m->e, 0, 0, 0); }
int  mfgpu_submit_batch(mfgpu_t *m, const mfgpu_batch_t *b){ (void)m; (void)b; return 0; }
void mfgpu_frame_end(mfgpu_t *m){ if (m && m->e) blt_end_frame(m->e); }
void mfgpu_destroy(mfgpu_t *m){ free(m); }
