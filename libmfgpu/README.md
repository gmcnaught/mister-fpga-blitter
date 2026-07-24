# libmfgpu/ — MFGPU geometry front-end (A9 side)

The A9 geometry stage of the MFGPU triangle path: transforms GameMaker-shaped
2D batches (textured triangles + an MVP matrix + a blend mode) into
`BLT_OP_TRILIST` display lists through the host emitter. The split mirrors a
classic GPU: the A9 (eventually NEON) owns transform / clip / cull; the fabric
owns the pixel back-end (interpolate, sample, modulate, blend, framebuffer).

`mfgpu_batch_t` is the contract with the future gmloader interceptor: model
space `{x,y,z,u,v,rgba}` vertices (optionally indexed), a column-major GLES
4×4 MVP, a texture page, a `BLT_BLEND_*` mode, and the viewport.

```sh
make test     # transform goldens + batch -> display-list -> refmodel end-to-end
```

## Files

| File | Role |
|------|------|
| `mfgpu.h` | public API: `mfgpu_create` / `mfgpu_frame_begin` / `mfgpu_submit_batch` / `mfgpu_frame_end`, and `mfgpu_batch_t` (the interceptor contract). |
| `mfgpu.c` | batch assembly: walk (indexed or sequential) triangles, transform, cull, push surviving `blt_vtx_t` triples via `blt_push_tris`, emit one `blt_trilist` header per batch. |
| `mfgpu_xform.{h,c}` | vertex transform: MVP → clip → NDC → screen **12.4 fixed point**, normalized UV → texel 12.4, RGBA8888 pass-through. |
| `test_mfgpu.c` | end-to-end: `mfgpu_batch_t` → display list → `blt_execute` (reference model) → framebuffer pixel checks. |
| `test_xform.c` | transform-stage goldens (rounding, viewport mapping, degenerate cases). |

## Pipeline position

```
 GM batch (floats, MVP)                              32-byte cmd ring
      │                                                     ▲
      ▼                                                     │
 libmfgpu: transform -> screen 12.4, cull  ──▶  host/blt_emitter:
      blt_vtx_t triples into the vertex buffer, blt_trilist() header
                                                            │
                                                            ▼
                        fabric/refmodel: BLT_OP_TRILIST (opcode 12)
                        rasterize via blt_tri (top-left rule, nearest texel,
                        per-vertex colour/alpha, all blend modes)
```

Status: validated against the reference model and in RTL simulation
(`../sim/` `tri_*` scenarios); not yet deployed in the production
solarus-mister fabric. See the MFGPU section in the top-level README and
§5 of `../docs/blitter-protocol.md` for the wire format.
