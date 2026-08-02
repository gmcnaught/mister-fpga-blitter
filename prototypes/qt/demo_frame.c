/*
 *  demo_frame.c — composite a cover-grid menu frame the way the offload would.
 *
 *  Builds a Zaparoo-shaped screen (tiled background, header bar, a grid of
 *  rounded cover cards with antialiased corners, a focused card whose art is
 *  re-scaled every frame, a status pill, bitmap-font labels), emits it as ONE
 *  blitter display list per frame, and runs that list through the golden
 *  reference model — i.e. exactly what the fabric would do with it.
 *
 *  The animated focus zoom is the point of the exercise. It is the draw a
 *  software renderer cannot afford (the study's §2: the team removed fades and
 *  slides precisely because a translucent overlay over a dense cover grid
 *  forces every cell to re-rasterize) and the one that pre-scaling at decode
 *  time cannot serve either, because the ratio is different on every frame.
 *  Here each frame of it costs the A9 one 32-byte command per card.
 *
 *    make demo    -> out/frame_NN.ppm + a per-frame command/pixel table
 *
 *  GPL-3.0.
 */
#include "ui_offload.h"
#include "blt_wire.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define RING_BYTES (256u * 1024u)
#define SRC_BYTES  (4u * 1024u * 1024u)
#define VTX_BYTES  (32u * 1024u)
#define FRAMES     12

/* ── theme ──────────────────────────────────────────────────────────────── */
#define C_BG      blt_rgb565( 12,  14,  20)
#define C_TILE    blt_rgb565( 18,  21,  30)
#define C_BAR     blt_rgb565( 26,  30,  42)
#define C_CARD    blt_rgb565( 38,  44,  60)
#define C_ACCENT  blt_rgb565(255, 176,  32)
#define C_TEXT    blt_rgb565(226, 230, 240)
#define C_DIM     blt_rgb565(130, 140, 160)
#define C_OK      blt_rgb565( 64, 210, 120)

/* ── procedural assets (stand-ins for decoded PNG cover art) ────────────── */
static uint16_t *make_cover(int w, int h, int variant)
{
    uint16_t *p = (uint16_t *)malloc((size_t)w * (size_t)h * sizeof *p);
    if (!p) return NULL;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int r, g, b;
            switch (variant & 3) {
            case 0:  r = 40 + x * 180 / w;  g = 30 + y * 120 / h;  b = 150 - x * 80 / w;  break;
            case 1:  r = ((x / 4 + y / 4) & 1) ? 210 : 40; g = 60 + y * 150 / h; b = 90; break;
            case 2:  r = 200 - y * 150 / h; g = 40 + x * 100 / w; b = 60 + y * 180 / h;  break;
            default: r = (x * y) % 200 + 30; g = 180 - x * 120 / w; b = 40 + y * 160 / h; break;
            }
            /* a light border so the scaling is easy to judge by eye */
            if (x < 2 || y < 2 || x >= w - 2 || y >= h - 2) { r = 240; g = 240; b = 250; }
            p[y * w + x] = blt_rgb565((uint8_t)r, (uint8_t)g, (uint8_t)b);
        }
    }
    return p;
}

static uint16_t *make_bg_tile(int n)
{
    uint16_t *p = (uint16_t *)malloc((size_t)n * (size_t)n * sizeof *p);
    if (!p) return NULL;
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++)
            p[y * n + x] = (x == 0 || y == 0) ? C_BAR : C_TILE;
    return p;
}

/* ── output ─────────────────────────────────────────────────────────────── */
static void write_ppm(const char *path, const uint16_t *fb)
{
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return; }
    fprintf(f, "P6\n%d %d\n255\n", BLT_FB_WIDTH, BLT_FB_HEIGHT);
    for (int i = 0; i < BLT_FB_PIXELS; i++) {
        unsigned c = fb[i];
        unsigned r5 = (c >> 11) & 0x1F, g6 = (c >> 5) & 0x3F, b5 = c & 0x1F;
        unsigned char rgb[3] = { (unsigned char)((r5 << 3) | (r5 >> 2)),
                                 (unsigned char)((g6 << 2) | (g6 >> 4)),
                                 (unsigned char)((b5 << 3) | (b5 >> 2)) };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

/* ── the frame ──────────────────────────────────────────────────────────── */
typedef struct {
    uio_t            *u;
    blt_emitter_t    *e;
    uint8_t          *src;
    blt_surface_ref_t tile;
    uint32_t          tile_entries_off;
    int               tile_entries;
    uio_image_ref_t   cover[4];
} scene_t;

static const char *const TITLES[8] = {
    "SOLARUS", "OPENBOR", "GMLOADER", "CAVE",
    "MEGADRIVE", "NEOGEO", "PSX", "ARCADE"
};

/*  The tiled background: recorded ONCE as a tile list, replayed as a single
 *  command per frame (the repo's TILELIST opcode — this is the study's "tiled
 *  background PNG -> one big COPY/tile blit" row, taken literally). */
static void build_tile_list(scene_t *s, int tile_px)
{
    int cols = (BLT_FB_WIDTH  + tile_px - 1) / tile_px;
    int rows = (BLT_FB_HEIGHT + tile_px - 1) / tile_px;
    s->tile_entries = cols * rows;
    uint32_t bytes = (uint32_t)s->tile_entries * (uint32_t)sizeof(blt_tile_entry_t);
    s->tile_entries_off = blt_alloc(&s->e->alloc, bytes);
    if (s->tile_entries_off == BLT_ALLOC_FAIL) { s->tile_entries = 0; return; }

    blt_tile_entry_t *ent = (blt_tile_entry_t *)(s->src + s->tile_entries_off);
    for (int r = 0; r < rows; r++)
        for (int c = 0; c < cols; c++) {
            blt_tile_entry_t *t = &ent[r * cols + c];
            t->src_x = 0; t->src_y = 0;
            t->w = (uint16_t)tile_px; t->h = (uint16_t)tile_px;
            t->dst_x = (int16_t)(c * tile_px);
            t->dst_y = (int16_t)(r * tile_px);
        }
}

static void emit_frame(scene_t *s, int frame)
{
    uio_t *u = s->u;
    /* focus zoom: 1.00 -> 1.18 -> 1.00, a new ratio on every frame */
    int phase = frame % FRAMES;
    int up    = phase <= FRAMES / 2 ? phase : FRAMES - phase;
    int zoom  = 100 + up * 2;                     /* percent */
    int focus = (frame / FRAMES) % 8;

    uio_begin_frame(u, frame & 1, 1, C_BG);

    /* 1 command: the whole tiled background. */
    if (s->tile_entries)
        blt_tile_list_static(s->e, s->tile, BLT_BLEND_COPY, 0, 255, 0,
                             s->tile_entries_off, s->tile_entries, 0, 0, 0);

    /* header bar + title + status pill */
    uio_rounded_rect(u, (uio_rect_t){ 4, 4, BLT_FB_WIDTH - 8, 22 }, 6, C_BAR, 255);
    uio_text(u, 12, 11, "ZAPAROO", C_TEXT);
    uio_rounded_rect(u, (uio_rect_t){ BLT_FB_WIDTH - 76, 8, 68, 14 }, 7, C_OK, 255);
    uio_text(u, BLT_FB_WIDTH - 68, 11, "READY", blt_rgb565(8, 24, 14));

    /* cover grid: 4 x 2 cards */
    const int CW = 68, CH = 88, GX = 10, GY = 34, GAP = 8;
    for (int i = 0; i < 8; i++) {
        int col = i % 4, row = i / 4;
        uio_rect_t card = { GX + col * (CW + GAP), GY + row * (CH + GAP), CW, CH };

        if (i == focus)   /* focus ring: a second rounded rect behind the card */
            uio_rounded_rect(u, (uio_rect_t){ card.x - 3, card.y - 3, card.w + 6, card.h + 6 },
                             9, C_ACCENT, 255);
        uio_rounded_rect(u, card, 6, C_CARD, 255);

        uio_image_ref_t art = s->cover[i & 3];
        uio_rect_t box = { card.x + 5, card.y + 6, card.w - 10, card.h - 28 };
        uio_rect_t fit = uio_fit(box, art.w, art.h);
        if (i == focus) {           /* animated zoom, re-scaled by the fabric */
            int nw = fit.w * zoom / 100, nh = fit.h * zoom / 100;
            fit.x -= (nw - fit.w) / 2; fit.y -= (nh - fit.h) / 2;
            fit.w = nw; fit.h = nh;
        }
        uio_scale_t sc;
        memset(&sc, 0, sizeof sc);
        sc.dst = fit;
        sc.blend = BLT_BLEND_COPY;
        sc.alpha = 255;
        uio_image_scaled(u, art, &sc);

        uio_text(u, card.x + 5, card.y + card.h - 14, TITLES[i], i == focus ? C_TEXT : C_DIM);
    }

    /* footer hint */
    uio_text(u, 10, BLT_FB_HEIGHT - 12, "SELECT: A   BACK: B", C_DIM);

    uio_end_frame(u);
}

int main(void)
{
    uint8_t *ring = (uint8_t *)calloc(RING_BYTES, 1);
    uint8_t *src  = (uint8_t *)calloc(SRC_BYTES, 1);
    uint16_t *fb  = (uint16_t *)calloc(BLT_FB_PIXELS, sizeof *fb);
    blt_cmd_t *cmds = (blt_cmd_t *)calloc(RING_BYTES / BLT_CMD_BYTES, sizeof *cmds);
    if (!ring || !src || !fb || !cmds) { fprintf(stderr, "out of memory\n"); return 1; }

    blt_emitter_t e;
    uio_t u;
    blt_emitter_init(&e, ring, RING_BYTES, src, SRC_BYTES);
    if (uio_init(&u, &e, src, VTX_BYTES) != 0) { fprintf(stderr, "uio_init failed\n"); return 1; }
    if (uio_load_font(&u) != 0) { fprintf(stderr, "font upload failed\n"); return 1; }

    scene_t s;
    memset(&s, 0, sizeof s);
    s.u = &u; s.e = &e; s.src = src;

    /* ---- load time: everything that touches pixels happens HERE ---------- */
    uint16_t *tilepx = make_bg_tile(16);
    s.tile = blt_upload(&e, tilepx, 16, 16, 32);
    free(tilepx);
    build_tile_list(&s, 16);

    for (int i = 0; i < 4; i++) {
        uint16_t *px = make_cover(40, 56, i);
        s.cover[i] = uio_upload_image(&u, px, 40, 56, 80);
        free(px);
        if (!s.cover[i].surf.valid) { fprintf(stderr, "cover upload failed\n"); return 1; }
    }
    uio_corner(&u, 6, 255);      /* pre-bake the radii the screen uses */
    uio_corner(&u, 9, 255);
    uio_corner(&u, 7, 255);
    printf("load: %u B of source uploaded (atlases + covers + tile), %d corner masks\n",
           blt_alloc_used(&e.alloc), u.ncorners);

    mkdir("out", 0777);

    /* ---- per frame: emit a display list, and touch no pixels ------------- */
    printf("\nframe  cmds  fills  blits  tris  glyphs   fabric px   A9 px avoided"
           "  (fill/AA/scale/text)\n");
    uint64_t total_a9 = 0;
    for (int f = 0; f < FRAMES; f++) {
        emit_frame(&s, f);
        if (e.overflow || e.dropped) {
            fprintf(stderr, "frame %d: overflow=%d dropped=%u\n", f, e.overflow, e.dropped);
            return 1;
        }

        /* execute the list exactly as the fabric walks it */
        for (int i = 0; i < e.cmd_count; i++)
            blt_unpack_cmd(ring + (size_t)i * BLT_CMD_BYTES, &cmds[i]);
        blt_surface_heap_t heap = { .base = src, .size = SRC_BYTES };
        memset(fb, 0, (size_t)BLT_FB_PIXELS * sizeof *fb);
        blt_execute(fb, &heap, cmds, e.cmd_count);

        char path[64];
        snprintf(path, sizeof path, "out/frame_%02d.ppm", f);
        write_ppm(path, fb);

        const uio_stats_t *st = &u.stats;
        uint64_t a9 = st->a9_fill_px + st->a9_aa_px + st->a9_resample_px + st->a9_glyph_px;
        total_a9 += a9;
        printf("%5d %5u %6u %6u %5u %7u %11llu %15llu  (%llu/%llu/%llu/%llu)\n",
               f, st->cmds, st->fills, st->blits, st->trilists, st->glyphs,
               (unsigned long long)st->fabric_px, (unsigned long long)a9,
               (unsigned long long)st->a9_fill_px, (unsigned long long)st->a9_aa_px,
               (unsigned long long)st->a9_resample_px, (unsigned long long)st->a9_glyph_px);
    }

    printf("\n%d frames written to out/frame_NN.ppm\n", FRAMES);
    printf("A9 per frame: ~%u commands (%u B of ring), 0 pixels rasterized.\n",
           u.stats.cmds, u.stats.cmds * BLT_CMD_BYTES);
    printf("Pixels the fabric composited instead of the A9: %llu over %d frames.\n",
           (unsigned long long)total_a9, FRAMES);
    printf("\nNOTE: this is emit-side accounting against the golden model, NOT a\n"
           "hardware measurement. The feasibility study's first recommendation is\n"
           "still to measure the real A9 frame time before building any of this.\n");

    free(ring); free(src); free(fb); free(cmds);
    return 0;
}
