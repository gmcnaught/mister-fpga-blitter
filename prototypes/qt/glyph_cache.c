/*
 *  glyph_cache.c — see glyph_cache.h.
 *
 *  The only per-pixel loop here runs on a cache MISS, copying a rasterized
 *  coverage bitmap into the atlas. Everything on the steady-state path is
 *  metrics arithmetic and command emission.
 *
 *  GPL-3.0.
 */
#include "glyph_cache.h"
#include "blt_wire.h"
#include <string.h>

#define RAMPS_PER_BANK (BLT_CLUT_ENTRIES / UIO_COV_LEVELS)   /* 256/16 = 16 */

static void note_err(uio_t *u, int err) { if (err && !u->last_error) u->last_error = err; }

/* ── CLUT ramps ─────────────────────────────────────────────────────────── */
int uio_clut_bind(uio_t *u, void *clut, size_t bytes)
{
    if (!u || !clut) return -1;
    size_t need = (size_t)BLT_CLUT_BANKS * BLT_CLUT_ENTRIES * 4u;
    if (bytes < need) return -1;
    u->clut = (uint8_t *)clut;
    u->clut_bytes = bytes;
    u->nramps = 0;
    u->clut_dirty = 0;
    memset(u->clut, 0, need);
    return 0;
}

/* CLUT word layout mirrors comp_clut.vh's CLUT_MAKE: RGB565 in [15:0], the
 * 4-bit alpha in [19:16]. Little-endian, one 32-bit word per entry. */
static void clut_write(uint8_t *clut, unsigned bank, unsigned slot,
                       uint16_t rgb565, unsigned a4)
{
    uint32_t w = (uint32_t)rgb565 | ((uint32_t)(a4 & 0xFu) << 16);
    uint8_t *p = clut + ((size_t)bank * BLT_CLUT_ENTRIES + slot) * 4u;
    p[0] = (uint8_t)w; p[1] = (uint8_t)(w >> 8);
    p[2] = (uint8_t)(w >> 16); p[3] = (uint8_t)(w >> 24);
}

int uio_text_ramp(uio_t *u, uint16_t rgb565)
{
    if (!u || !u->clut) return -1;
    for (int i = 0; i < u->nramps; i++)
        if (u->ramp_color[i] == rgb565) return (int)u->ramp_word[i];
    if (u->nramps >= UIO_MAX_TEXT_RAMPS) return -1;

    int idx = u->nramps;
    unsigned bank = (unsigned)(idx / RAMPS_PER_BANK);
    unsigned base = (unsigned)(idx % RAMPS_PER_BANK) * UIO_COV_LEVELS;
    if (bank >= BLT_CLUT_BANKS) return -1;

    /*  A coverage ramp: same colour in every entry, alpha climbing 0..15. The
     *  fully covered entry composites bit-identically to a FILL of `rgb565`
     *  (alpha 15 -> a8 255 -> the blend's src term), which is what keeps text
     *  and the shapes around it in the same colour space. Entry 0 has A4 == 0,
     *  which the PALPHA path treats as skip-write, so background texels of a
     *  glyph cost no framebuffer traffic at all. */
    for (unsigned i = 0; i < UIO_COV_LEVELS; i++)
        clut_write(u->clut, bank, base + i, rgb565, i);

    u->ramp_color[idx] = rgb565;
    u->ramp_word[idx]  = blt_pal_color((uint8_t)bank, (uint8_t)base);
    u->nramps++;
    u->clut_dirty = 1;
    return (int)u->ramp_word[idx];
}

int uio_clut_dirty(const uio_t *u) { return u && u->clut_dirty; }

int uio_clut_upload(uio_t *u)
{
    if (!u || !u->e || !u->clut) return -1;
    int rc = blt_emit_clut_upload(u->e, 0, BLT_CLUT_BANKS * BLT_CLUT_ENTRIES);
    if (rc != 0) { note_err(u, rc); return rc; }
    u->clut_dirty = 0;
    return 0;
}

/* ── atlas ──────────────────────────────────────────────────────────────── */
int uio_glyph_atlas_init(uio_t *u, uio_glyph_atlas_t *fa, int w, int h,
                         uio_glyph_t *slots, int nslots,
                         uio_rasterize_fn fn, void *ctx, int phases)
{
    if (!u || !u->e || !fa || !slots || nslots <= 0 || !fn) return -1;
    if (w <= 0 || h <= 0 || phases <= 0 || phases > 8) return -1;

    memset(fa, 0, sizeof *fa);
    uint32_t bytes = (uint32_t)w * (uint32_t)h;              /* PAL8: 1 B/texel */
    uint32_t off = blt_alloc(&u->e->alloc, bytes);
    if (off == BLT_ALLOC_FAIL) return -1;

    fa->pixels = u->src_base + off;
    memset(fa->pixels, 0, bytes);                            /* index 0 = empty */

    /* The atlas is written in place rather than uploaded, so the handle is
     * built by hand — blt_upload copies a finished surface, which a cache that
     * grows every time a new glyph appears is not. */
    fa->surf.off    = off;
    fa->surf.stride = (uint16_t)w;
    fa->surf.w      = (uint16_t)w;
    fa->surf.h      = (uint16_t)h;
    fa->surf.format = BLT_FMT_PAL8;
    fa->surf.size   = bytes;
    fa->surf.sdram_off = BLT_ALLOC_FAIL;
    fa->surf.valid  = 1;

    fa->w = w; fa->h = h;
    fa->slots = slots; fa->nslots = nslots;
    memset(slots, 0, (size_t)nslots * sizeof *slots);
    fa->raster = fn; fa->raster_ctx = ctx;
    fa->phases = phases;
    fa->frame = 1;
    fa->dirty_lo = bytes; fa->dirty_hi = 0;
    return 0;
}

void uio_glyph_atlas_frame(uio_glyph_atlas_t *fa) { if (fa) fa->frame++; }

int uio_glyph_atlas_flush(uio_t *u, uio_glyph_atlas_t *fa)
{
    if (!u || !u->e || !fa) return -1;
    if (fa->dirty_lo >= fa->dirty_hi) return 0;              /* nothing new */
    /*  On hardware with staged sources this is blt_stage_to(ddr, sdram, size)
     *  for the dirty span; with sources read from the heap (the reference
     *  model's world) STAGE has no framebuffer effect and the write is already
     *  visible. Emitting it either way keeps the shape honest. */
    int rc = blt_stage(u->e, fa->surf.off + fa->dirty_lo, fa->dirty_hi - fa->dirty_lo);
    if (rc != 0) { note_err(u, rc); return rc; }
    fa->dirty_lo = (uint32_t)(fa->w * fa->h);
    fa->dirty_hi = 0;
    return 0;
}

static void mark_dirty(uio_glyph_atlas_t *fa, uint32_t lo, uint32_t hi)
{
    if (lo < fa->dirty_lo) fa->dirty_lo = lo;
    if (hi > fa->dirty_hi) fa->dirty_hi = hi;
}

/*  Shelf packing: glyphs at one size share a height, so a shelf wastes little,
 *  and a shelf-based packer needs no per-row bookkeeping to stay O(1). */
static int shelf_place(uio_glyph_atlas_t *fa, int w, int h, int *out_x, int *out_y)
{
    if (w > fa->w || h > fa->h) return -1;
    if (fa->shelf_x + w > fa->w) {                 /* start a new shelf */
        fa->shelf_y += fa->shelf_h;
        fa->shelf_x = 0;
        fa->shelf_h = 0;
    }
    if (fa->shelf_y + h > fa->h) return -1;        /* atlas full */
    *out_x = fa->shelf_x;
    *out_y = fa->shelf_y;
    fa->shelf_x += w;
    if (h > fa->shelf_h) fa->shelf_h = h;
    return 0;
}

/*  Evict the least recently used glyph whose rect can hold this one.
 *
 *  A glyph used in the last two frames is NOT a candidate: the fabric may still
 *  be reading the frame the A9 most recently submitted, so overwriting its
 *  texels would corrupt a frame in flight. Same discipline as the paint
 *  engine's deferred scratch recycling. */
static int evict_for(uio_glyph_atlas_t *fa, int w, int h)
{
    int best = -1;
    uint32_t best_age = 0;
    for (int i = 0; i < fa->nslots; i++) {
        uio_glyph_t *g = &fa->slots[i];
        if (!g->valid || g->cell_w < w || g->cell_h < h) continue;
        if (fa->frame - g->used_frame < 2) continue;         /* possibly in flight */
        uint32_t age = fa->frame - g->used_frame;
        if (age > best_age) { best_age = age; best = i; }
    }
    return best;
}

static uio_glyph_t *cache_lookup(uio_glyph_atlas_t *fa, uint32_t cp, int px, int phase)
{
    for (int i = 0; i < fa->nslots; i++) {
        uio_glyph_t *g = &fa->slots[i];
        if (g->valid && g->codepoint == cp && g->px_size == px && g->phase == phase)
            return g;
    }
    return NULL;
}

static uio_glyph_t *cache_insert(uio_glyph_atlas_t *fa, uint32_t cp, int px, int phase)
{
    uio_glyph_bmp_t bmp;
    memset(&bmp, 0, sizeof bmp);
    if (fa->raster(fa->raster_ctx, cp, px, phase, &bmp) != 0) { fa->refused++; return NULL; }
    if (bmp.w < 0 || bmp.h < 0) { fa->refused++; return NULL; }

    /*  Find a home: a fresh shelf slice if the directory and the atlas both
     *  have room, otherwise recycle the least recently used slot whose SHELF
     *  CELL is big enough (cell_w/cell_h, not the inked w/h — see the field's
     *  comment in the header). */
    uio_glyph_t *g = NULL;
    int ax = 0, ay = 0, cw = 0, chh = 0;
    int placed = 0;
    if (fa->count < fa->nslots && bmp.w > 0 && bmp.h > 0)
        placed = (shelf_place(fa, bmp.w, bmp.h, &ax, &ay) == 0);

    if (placed) {
        g = &fa->slots[fa->count++];
        cw = bmp.w; chh = bmp.h;
    } else if (bmp.w == 0 || bmp.h == 0) {
        if (fa->count >= fa->nslots) { fa->refused++; return NULL; }
        g = &fa->slots[fa->count++];        /* space: metrics only, no texels */
    } else {
        int victim = evict_for(fa, bmp.w, bmp.h);
        if (victim < 0) { fa->refused++; return NULL; }
        g = &fa->slots[victim];
        ax = g->sx; ay = g->sy;
        cw = g->cell_w; chh = g->cell_h;
        fa->evictions++;
    }

    /*  Quantise 8-bit coverage to the CLUT ramp's 16 levels. The texel IS the
     *  coverage index; colour arrives later through the ramp, which is what
     *  lets one atlas serve every text colour. */
    for (int y = 0; y < bmp.h; y++) {
        const uint8_t *srow = bmp.cov + (size_t)y * (size_t)bmp.pitch;
        uint8_t *drow = fa->pixels + (size_t)(ay + y) * (size_t)fa->w + (size_t)ax;
        for (int x = 0; x < bmp.w; x++)
            drow[x] = (uint8_t)(((unsigned)srow[x] * (UIO_COV_LEVELS - 1) + 127u) / 255u);
    }
    if (bmp.w > 0 && bmp.h > 0) {
        mark_dirty(fa, (uint32_t)(ay * fa->w),
                       (uint32_t)((ay + bmp.h - 1) * fa->w + ax + bmp.w));
        fa->rasterized_px += (uint32_t)(bmp.w * bmp.h);
    }

    g->codepoint = cp;
    g->px_size   = (uint16_t)px;
    g->phase     = (uint8_t)phase;
    g->valid     = 1;
    g->sx = (uint16_t)ax; g->sy = (uint16_t)ay;
    g->w  = (uint16_t)bmp.w; g->h = (uint16_t)bmp.h;
    g->cell_w = (uint16_t)cw; g->cell_h = (uint16_t)chh;
    g->bearing_x = (int16_t)bmp.bearing_x;
    g->bearing_y = (int16_t)bmp.bearing_y;
    g->advance   = (int16_t)bmp.advance;
    g->used_frame = fa->frame;
    fa->misses++;
    return g;
}

static uio_glyph_t *glyph_for(uio_glyph_atlas_t *fa, uint32_t cp, int px, int phase)
{
    uio_glyph_t *g = cache_lookup(fa, cp, px, phase);
    if (g) { fa->hits++; g->used_frame = fa->frame; return g; }
    return cache_insert(fa, cp, px, phase);
}

/* Minimal UTF-8 decode; malformed bytes are consumed one at a time as U+FFFD. */
static uint32_t utf8_next(const char **p)
{
    const unsigned char *s = (const unsigned char *)*p;
    uint32_t c = *s++;
    int extra = 0;
    if (c >= 0xF0)      { c &= 0x07u; extra = 3; }
    else if (c >= 0xE0) { c &= 0x0Fu; extra = 2; }
    else if (c >= 0xC0) { c &= 0x1Fu; extra = 1; }
    else if (c >= 0x80) { *p = (const char *)s; return 0xFFFDu; }
    for (int i = 0; i < extra; i++) {
        if ((*s & 0xC0u) != 0x80u) { *p = (const char *)s; return 0xFFFDu; }
        c = (c << 6) | (uint32_t)(*s++ & 0x3Fu);
    }
    *p = (const char *)s;
    return c;
}

/* ── batching ───────────────────────────────────────────────────────────── */
void uio_text_batch_begin(uio_t *u, blt_sprite_channel_t *ch)
{
    if (!u || !ch) return;
    u->batch = ch;
}

int uio_text_batch_active(const uio_t *u) { return u && u->batch != NULL; }

int uio_text_batch_flush(uio_t *u)
{
    if (!u || !u->batch) return 0;
    int runs = blt_sprite_channel_flush(u->batch, 0, 0);
    u->stats.glyph_batches += (uint32_t)runs;
    u->batch = NULL;
    return runs;
}

/* ── drawing ────────────────────────────────────────────────────────────── */
static void emit_glyph(uio_t *u, uio_glyph_atlas_t *fa, const uio_glyph_t *g,
                       int dx, int dy, uint16_t pal_word)
{
    if (g->w == 0 || g->h == 0) return;                 /* space, or blank */

    if (u->batch) {
        /*  One entry per glyph, and the entry carries its own palette word —
         *  so a mixed-colour screen still flushes as a single SPRITELIST
         *  command instead of one command per colour change. */
        blt_sprite_run_key_t k;
        memset(&k, 0, sizeof k);
        k.src_stride = fa->surf.stride;
        k.format = BLT_FMT_PAL8;
        k.blend  = BLT_BLEND_PALPHA;
        k.alpha  = 255;
        blt_sprite_entry_t se;
        memset(&se, 0, sizeof se);
        se.src_off = fa->surf.off;
        se.src_x = g->sx; se.src_y = g->sy; se.w = g->w; se.h = g->h;
        se.dst_x = (int16_t)dx; se.dst_y = (int16_t)dy;
        se.color = pal_word;
        if (!blt_sprite_channel_push(u->batch, &k, &se)) { note_err(u, -1); return; }
    } else {
        int rc = blt_blit_pal8(u->e, fa->surf, g->sx, g->sy, g->w, g->h, dx, dy,
                               BLT_BLEND_PALPHA, 0, 255, 0,
                               blt_pal_id(pal_word), blt_base_off(pal_word));
        if (rc != 0) { note_err(u, rc); return; }
        u->stats.blits++;
    }
    u->stats.glyphs++;
    u->stats.fabric_px   += (uint64_t)g->w * (uint64_t)g->h;
    u->stats.a9_glyph_px += (uint64_t)g->w * (uint64_t)g->h;
}

int uio_text_run_fx(uio_t *u, uio_glyph_atlas_t *fa, int px_size,
                    int x_fx8, int baseline_y, const char *utf8, uint16_t color)
{
    if (!u || !u->e || !fa || !utf8 || px_size <= 0) return 0;
    int ramp = uio_text_ramp(u, color);
    if (ramp < 0) { note_err(u, -1); return 0; }

    const int start = x_fx8;
    int pen = x_fx8;
    for (const char *p = utf8; *p; ) {
        uint32_t cp = utf8_next(&p);
        /* Subpixel phase from the pen's fractional part; with phases == 1 this
         * is always 0 and every glyph lands pixel-aligned. */
        int frac = pen & 0xFF;
        int phase = (frac * fa->phases + 128) >> 8;
        int carry = 0;
        if (phase >= fa->phases) { phase = 0; carry = 1; }
        uio_glyph_t *g = glyph_for(fa, cp, px_size, phase);
        if (!g) continue;
        int dx = (pen >> 8) + carry + g->bearing_x;
        emit_glyph(u, fa, g, dx, baseline_y - g->bearing_y, (uint16_t)ramp);
        pen += g->advance << 8;
    }
    return (pen - start) >> 8;
}

int uio_text_run(uio_t *u, uio_glyph_atlas_t *fa, int px_size,
                 int x, int baseline_y, const char *utf8, uint16_t color)
{
    return uio_text_run_fx(u, fa, px_size, x << 8, baseline_y, utf8, color);
}

int uio_text_run_width(uio_t *u, uio_glyph_atlas_t *fa, int px_size, const char *utf8)
{
    if (!fa || !utf8 || px_size <= 0) return 0;
    (void)u;
    int w = 0;
    for (const char *p = utf8; *p; ) {
        uint32_t cp = utf8_next(&p);
        uio_glyph_t *g = glyph_for(fa, cp, px_size, 0);
        if (g) w += g->advance;
    }
    return w;
}
