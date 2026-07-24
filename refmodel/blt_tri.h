/*
 *  blt_tri.h — reference textured-triangle rasterizer (BLT_OP_TRILIST).
 *  Golden spec for the RTL blt_tri module. Copyright (C) 2026 — GPL-3.0.
 */
#ifndef BLT_TRI_H
#define BLT_TRI_H
#include "blitter_ref.h"
void blt_raster_tri(uint16_t *fb, const blt_surface_heap_t *heap,
                    const blt_cmd_t *h, const blt_vtx_t *tris, int ntris,
                    const uint16_t *surface);
#endif /* BLT_TRI_H */
