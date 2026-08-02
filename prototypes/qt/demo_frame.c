/*
 *  demo_frame.c — the Zaparoo games-browse screen, composited by the fabric.
 *
 *  Not a mock-up: every dimension comes from zaparoo_ui.c, which ports the
 *  front-end's own Theme/Sizing/Motion/Tile rules (see that file's header for
 *  the upstream sources). Change the screen size and the layout re-solves the
 *  way the app's would — including the grid shape, which the app picks by
 *  scoring cell aspect against a target.
 *
 *  Two scenes, both chosen because they are the draws the real UI struggles
 *  with on the A9:
 *
 *  1. BROWSE — a grid of cover cards. Each card is a rounded rect with an
 *     antialiased edge, and each cover is a decode-tier bitmap (128 px) painted
 *     into a box the grid solve produced (~85 px) — i.e. an arbitrary-ratio
 *     resample per cover per frame. The focused tile carries BOTH the transient
 *     push-in cue (Motion.pressScale 0.90 over 80 ms) and the persistent 1.06
 *     focus scale that Tile.qml REMOVED for being "a persistent, per-focus-move
 *     cost ... on covered grids". Every frame the focused tile's geometry is
 *     different, so nothing about it can be pre-scaled at decode time.
 *
 *  2. MODAL — a translucent scrim fading in over that grid. This is the exact
 *     draw the team engineered away: a translucent overlay over a dense cover
 *     grid forces every cell underneath to re-rasterize, every frame. On the
 *     fabric the scrim is ONE const-alpha FILL over an already-composited
 *     frame, and the grid beneath it costs what it always cost.
 *
 *    make demo    -> out/frame_NN.ppm + a per-frame command/pixel table
 *
 *  GPL-3.0.
 */
#include "ui_offload.h"
#include "zaparoo_ui.h"
#include "blt_wire.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define RING_BYTES (256u * 1024u)
#define SRC_BYTES  (4u * 1024u * 1024u)
#define VTX_BYTES  (32u * 1024u)

#define BROWSE_FRAMES 16
#define MODAL_FRAMES   8
#define FRAMES        (BROWSE_FRAMES + MODAL_FRAMES)

/*  The reference model's framebuffer is 320x240 RGB565. The app's CRT modes are
 *  352x240 / 352x288 / 720x480 at 32bpp (native_video_writer's mode table); the
 *  study reaches those through a banded write-through WORK cache, which is a
 *  fabric-side change that does not alter the display list emitted here. */
static const zui_sizing_t SZ = { BLT_FB_WIDTH, BLT_FB_HEIGHT, 1 /* crtNativePath */ };

/* ── procedural stand-ins for decoded assets ────────────────────────────── */

/* Box art at a Core decode tier: portrait 3:4 at the snapped tier height. */
static uint16_t *make_cover(int w, int h, int variant)
{
    uint16_t *p = (uint16_t *)malloc((size_t)w * (size_t)h * sizeof *p);
    if (!p) return NULL;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int r, g, b;
            switch (variant % 6) {
            case 0:  r = 30 + x * 150 / w;  g = 20 + y * 90 / h;   b = 160 - x * 60 / w;  break;
            case 1:  r = ((x / 8 + y / 8) & 1) ? 200 : 30; g = 50 + y * 140 / h; b = 80;  break;
            case 2:  r = 210 - y * 140 / h; g = 30 + x * 110 / w;  b = 70 + y * 150 / h;  break;
            case 3:  r = 40 + y * 60 / h;   g = 150 - x * 90 / w;  b = 200 - y * 90 / h;  break;
            case 4:  r = 180 - x * 60 / w;  g = 60 + y * 120 / h;  b = 40 + x * 100 / w;  break;
            default: r = 90 + ((x * y) % 120); g = 40 + y * 100 / h; b = 120 + x * 80 / w; break;
            }
            /* a light frame so an arbitrary-ratio resample is easy to judge */
            if (x < 3 || y < 3 || x >= w - 3 || y >= h - 3) { r = 235; g = 235; b = 245; }
            p[y * w + x] = blt_rgb565((uint8_t)r, (uint8_t)g, (uint8_t)b);
        }
    }
    return p;
}

/* The tiled circuit-trace background (MainLayout.qml: fillMode: Image.Tile,
 * smooth: false — "1:1 tile, filtering would just blur the lines"). */
static uint16_t *make_bg_tile(int n)
{
    uint16_t *p = (uint16_t *)malloc((size_t)n * (size_t)n * sizeof *p);
    if (!p) return NULL;
    const uint16_t deep  = zui_565(ZUI_BG_DEEP);
    const uint16_t trace = zui_565(ZUI_BORDER_SUBTLE);
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) {
            int on = (x == 0) || (y == 0)
                  || (y == n / 2 && x > n / 2) || (x == n / 2 && y < n / 2);
            p[y * n + x] = on ? trace : deep;
        }
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

/* ── the scene ──────────────────────────────────────────────────────────── */
#define TILE_COUNT 6
#define COVER_W 96                       /* 3:4 box art at the 128 px tier */

typedef struct {
    uio_t            *u;
    blt_emitter_t    *e;
    uint8_t          *src;
    blt_surface_ref_t tile_tex;
    uint32_t          tile_entries_off;
    int               tile_entries;
    uio_image_ref_t   cover[TILE_COUNT];
    zui_tile_t        tm;
    int               columns, rows;
} scene_t;

static const char *const TITLES[TILE_COUNT] = {
    "SONIC 2", "STREETS OF RAGE", "GUNSTAR HEROES",
    "ALADDIN", "COMIX ZONE", "VECTORMAN"
};

/* The whole tiled background as ONE command (BLT_OP_TILELIST). */
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

/* Centre-scale a rect by `pct`, the way QML's `scale:` transforms an Item. */
static uio_rect_t scale_rect(uio_rect_t r, int pct)
{
    uio_rect_t o;
    o.w = r.w * pct / 100;
    o.h = r.h * pct / 100;
    o.x = r.x + (r.w - o.w) / 2;
    o.y = r.y + (r.h - o.h) / 2;
    return o;
}

/*  HeaderBar.qml + TopStatusStrip + CoreStatusPill: two stacked rows whose
 *  height is locked so the pill's space is reserved even when it is idle. */
static void emit_header(scene_t *s, int progress_pct)
{
    uio_t *u = s->u;
    const int side = zui_header_side_margin(&SZ);
    const int top  = zui_header_top_margin(&SZ);
    const int row  = zui_header_row_height(&SZ);
    const int gap  = zui_header_stack_gap(&SZ);

    /* logo row: brand mark + wordmark, clock right-aligned */
    uio_rounded_rect(u, (uio_rect_t){ side, top, row, row }, 2, zui_565(ZUI_ACCENT), 255);
    uio_text(u, side + row + 3, top, "ZAPAROO", zui_565(ZUI_TEXT_PRIMARY));
    const char *clock = "13:45";
    uio_text(u, BLT_FB_WIDTH - side - uio_text_width(clock), top,
             clock, zui_565(ZUI_TEXT_LABEL));

    /* status row: state markers, then the core status pill */
    const int y2 = top + row + gap;
    for (int i = 0; i < 3; i++)
        uio_rounded_rect(u, (uio_rect_t){ side + i * (row + 2), y2, row, row }, 2,
                         zui_565(ZUI_STATE_MARKER), 255);

    const char *pill_text = "INDEXING";
    const int pill_w = uio_text_width(pill_text) + 2 * row;
    const int pill_x = BLT_FB_WIDTH - side - pill_w;
    const int radius = zui_half(row);
    uio_rounded_rect(u, (uio_rect_t){ pill_x, y2, pill_w, row }, radius,
                     zui_565(ZUI_SURFACE_CARD), 255);
    /* progress fill inside the pill track, same pill radius */
    int fill_w = pill_w * progress_pct / 100;
    if (fill_w > 2 * radius)
        uio_rounded_rect(u, (uio_rect_t){ pill_x, y2, fill_w, row }, radius,
                         zui_565(ZUI_ACCENT), 255);
    uio_text(u, pill_x + row, y2, pill_text, zui_565(ZUI_TEXT_PRIMARY));
}

/*  One games-grid tile: Tile.qml's card (surfaceCard + a 1 px borderMid edge),
 *  the focus ring (two stacked filled rounded rects), the cover, the caption. */
static void emit_tile(scene_t *s, int index, uio_rect_t cell, int focused, int scale_pct)
{
    uio_t *u = s->u;
    const zui_tile_t *tm = &s->tm;

    uio_rect_t tile = focused ? scale_rect(cell, scale_pct) : cell;
    int radius = tm->corner_radius * (focused ? scale_pct : 100) / 100;

    /* card + its static 1 px edge, as one outlined rounded rect */
    uio_rounded_rect_outline(u, tile, radius, tm->border_width,
                             zui_565(ZUI_BORDER_MID), zui_565(ZUI_SURFACE_CARD), 255);

    /* focus ring: inset by _outlineGap, _outlineWidth thick, punched back to
     * the card surface — Tile.qml's two-filled-rounded-rects construction */
    if (focused) {
        uio_rect_t ring = { tile.x + tm->outline_gap, tile.y + tm->outline_gap,
                            tile.w - 2 * tm->outline_gap, tile.h - 2 * tm->outline_gap };
        uio_rounded_rect_outline(u, ring, radius - tm->outline_gap, tm->outline_width,
                                 zui_565(ZUI_ACCENT), zui_565(ZUI_SURFACE_CARD), 255);
    }

    /* cover: PreserveAspectFit into the padded box, minus the caption band */
    uio_image_ref_t art = s->cover[index];
    int pad = tm->padding * (focused ? scale_pct : 100) / 100;
    uio_rect_t box = { tile.x + pad, tile.y + pad, tile.w - 2 * pad,
                       tile.h - pad - (tm->caption_height + tm->caption_gap)
                                        * (focused ? scale_pct : 100) / 100 };
    uio_rect_t fit = uio_fit(box, art.w, art.h);

    uio_scale_t sc;
    memset(&sc, 0, sizeof sc);
    sc.dst = fit;
    sc.blend = BLT_BLEND_COPY;
    sc.alpha = 255;
    uio_image_scaled(u, art, &sc);

    /*  Caption. Not scaled with the tile: the CRT path's font is a fixed 6x8
     *  bitmap face with NoAntialias, and resampling it is precisely the
     *  artefact that font exists to avoid. QML's `scale:` would resample it;
     *  the offload keeps glyphs at 1:1 and moves the tile around them. */
    const char *title = TITLES[index];
    int tw = uio_text_width(title);
    int max_w = cell.w - 2 * tm->caption_side_inset;
    int chars = max_w / UIO_FONT_CELL_W;
    char clipped[32];
    if (tw > max_w && chars > 1 && chars < (int)sizeof clipped) {
        memcpy(clipped, title, (size_t)chars - 1);
        clipped[chars - 1] = '.';                  /* elide, as ScrollingCaption does */
        clipped[chars] = '\0';
        title = clipped;
        tw = uio_text_width(title);
    }
    uio_text(u, cell.x + (cell.w - tw) / 2,
             cell.y + cell.h - tm->caption_height + (tm->caption_height - UIO_FONT_INK_H) / 2,
             title, zui_565(focused ? ZUI_TEXT_PRIMARY : ZUI_TEXT_LABEL));
}

/*  Modal.qml: a translucent scrim over the whole screen, then a bgPanel
 *  rounded panel with accent-bordered buttons. */
static void emit_modal(scene_t *s, int scrim_alpha)
{
    uio_t *u = s->u;
    if (scrim_alpha <= 0) return;

    /* The whole scrim: ONE const-alpha fill over the composited grid. */
    uio_fill(u, (uio_rect_t){ 0, 0, BLT_FB_WIDTH, BLT_FB_HEIGHT },
             zui_565(0x000000), (uint8_t)scrim_alpha);

    const int radius = zui_corner_radius(&SZ);
    const int pw = BLT_FB_WIDTH * 78 / 100;                 /* parent.width * 0.78 */
    const int ph = zui_pctH(&SZ, 40);
    uio_rect_t panel = { (BLT_FB_WIDTH - pw) / 2, (BLT_FB_HEIGHT - ph) / 2, pw, ph };
    uio_rounded_rect_outline(u, panel, radius, 1,
                             zui_565(ZUI_BORDER_MID), zui_565(ZUI_BG_PANEL), 255);

    const char *title = "LAUNCH GAME?";
    const char *body  = "SONIC 2";
    uio_text(u, panel.x + (panel.w - uio_text_width(title)) / 2, panel.y + zui_pctH(&SZ, 5),
             title, zui_565(ZUI_TEXT_PRIMARY));
    uio_text(u, panel.x + (panel.w - uio_text_width(body)) / 2, panel.y + zui_pctH(&SZ, 13),
             body, zui_565(ZUI_TEXT_VARIANT));

    /* two buttons: surfaceCard fill, 2 px accent border on the focused one */
    const int bh = zui_pctH(&SZ, 7), bw = zui_pctW(&SZ, 28);
    const int by = panel.y + panel.h - bh - zui_pctH(&SZ, 5);
    uio_rect_t cancel = { panel.x + zui_pctW(&SZ, 6), by, bw, bh };
    uio_rect_t accept = { panel.x + panel.w - bw - zui_pctW(&SZ, 6), by, bw, bh };
    uio_rounded_rect_outline(u, cancel, radius, 1,
                             zui_565(ZUI_BORDER_MID), zui_565(ZUI_SURFACE_CARD), 255);
    uio_rounded_rect_outline(u, accept, radius, zui_stroke(2),
                             zui_565(ZUI_ACCENT), zui_565(ZUI_SURFACE_CARD), 255);
    uio_text(u, cancel.x + (cancel.w - uio_text_width("BACK")) / 2,
             cancel.y + (bh - UIO_FONT_INK_H) / 2, "BACK", zui_565(ZUI_TEXT_PRIMARY));
    uio_text(u, accept.x + (accept.w - uio_text_width("PLAY")) / 2,
             accept.y + (bh - UIO_FONT_INK_H) / 2, "PLAY", zui_565(ZUI_TEXT_PRIMARY));
}

/*  The focused tile's scale for this frame.
 *
 *  Frames 0-3   rest at the restored persistent focus scale (1.06),
 *  frames 4-8   push in to Motion.pressScale (0.90) — the accept cue,
 *  frames 9-15  settle back.
 *  Every frame is a different ratio, which is the whole point: no decode-time
 *  pre-scale can serve this, and on the fabric each frame is one command. */
static int focus_scale_pct(int frame)
{
    const int rest = ZUI_FOCUS_SCALE_RESTORED, press = ZUI_PRESS_SCALE_CRT;
    if (frame < 4)  return rest;
    if (frame <= 8) return rest - (rest - press) * (frame - 3) / 5;
    if (frame < BROWSE_FRAMES)
        return press + (rest - press) * (frame - 8) / (BROWSE_FRAMES - 8);
    return rest;
}

static void emit_frame(scene_t *s, int frame)
{
    uio_t *u = s->u;
    uio_begin_frame(u, frame & 1, 1, zui_565(ZUI_BG_DEEP));

    if (s->tile_entries)
        blt_tile_list_static(s->e, s->tile_tex, BLT_BLEND_COPY, 0, 255, 0,
                             s->tile_entries_off, s->tile_entries, 0, 0, 0);

    emit_header(s, 30 + frame * 2);

    const int top = zui_header_bottom(&SZ);
    const int cell_w = BLT_FB_WIDTH / s->columns;
    const int cell_h = (BLT_FB_HEIGHT - top) / s->rows;
    const int inset = zui_pctH(&SZ, 0.8);
    const int focus = 1;                       /* the selected tile */

    for (int i = 0; i < TILE_COUNT && i < s->columns * s->rows; i++) {
        int col = i % s->columns, row = i / s->columns;
        uio_rect_t cell = { col * cell_w + inset, top + row * cell_h + inset,
                            cell_w - 2 * inset, cell_h - 2 * inset };
        emit_tile(s, i, cell, i == focus, focus_scale_pct(frame));
    }

    if (frame >= BROWSE_FRAMES) {
        /* Modal.qml's scrim is #cc000000 — alpha 0xcc = 204. Fade it in. */
        int step = frame - BROWSE_FRAMES + 1;
        emit_modal(s, 204 * step / MODAL_FRAMES);
    }

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
    zui_tile_metrics(&SZ, &s.tm);
    zui_games_grid_shape(&SZ, &s.columns, &s.rows);

    const int tier = zui_games_grid_cover_source_size(&SZ);
    const int box  = zui_games_grid_cover_box(&SZ);
    printf("Zaparoo browse screen, %dx%d, CRT native path\n", SZ.screen_w, SZ.screen_h);
    printf("  grid shape      : %d columns x %d rows (Sizing._selectGridShape)\n",
           s.columns, s.rows);
    printf("  corner radius   : %d px    tile padding %d, ring %d px inset %d\n",
           s.tm.corner_radius, s.tm.padding, s.tm.outline_width, s.tm.outline_gap);
    printf("  caption font    : %d px (fixed-cell 6x8 bitmap face)\n", s.tm.caption_text_size);
    printf("  cover decode    : %d px tier for a %d px painted box"
           "  -> every cover is an arbitrary-ratio resample\n", tier, box);

    /* ---- load time: everything that touches pixels happens HERE ---------- */
    uint16_t *tilepx = make_bg_tile(16);
    s.tile_tex = blt_upload(&e, tilepx, 16, 16, 32);
    free(tilepx);
    build_tile_list(&s, 16);

    for (int i = 0; i < TILE_COUNT; i++) {
        uint16_t *px = make_cover(COVER_W, tier, i);
        s.cover[i] = uio_upload_image(&u, px, COVER_W, tier, COVER_W * 2);
        free(px);
        if (!s.cover[i].surf.valid) { fprintf(stderr, "cover upload failed\n"); return 1; }
    }
    printf("  uploaded        : %u B of sources (covers + background tile + glyph atlas)\n",
           blt_alloc_used(&e.alloc));

    mkdir("out", 0777);

    /* ---- per frame: emit a display list, and touch no pixels ------------- */
    printf("\nframe  cmds  fills  blits  tris  glyphs   fabric px   A9 px avoided"
           "  (fill/AA/scale/text)\n");
    uint64_t total_a9 = 0;
    uint32_t peak_cmds = 0;
    for (int f = 0; f < FRAMES; f++) {
        emit_frame(&s, f);
        if (e.overflow || e.dropped || u.last_error) {
            fprintf(stderr, "frame %d: overflow=%d dropped=%u err=%d\n",
                    f, e.overflow, e.dropped, u.last_error);
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
        if (st->cmds > peak_cmds) peak_cmds = st->cmds;
        printf("%5d %5u %6u %6u %5u %7u %11llu %15llu  (%llu/%llu/%llu/%llu)%s\n",
               f, st->cmds, st->fills, st->blits, st->trilists, st->glyphs,
               (unsigned long long)st->fabric_px, (unsigned long long)a9,
               (unsigned long long)st->a9_fill_px, (unsigned long long)st->a9_aa_px,
               (unsigned long long)st->a9_resample_px, (unsigned long long)st->a9_glyph_px,
               f >= BROWSE_FRAMES ? "  <- modal scrim" : "");
    }

    printf("\n%d frames written to out/frame_NN.ppm\n", FRAMES);
    printf("Peak %u commands/frame (%u B of ring). A9 pixels rasterized: 0.\n",
           peak_cmds, peak_cmds * BLT_CMD_BYTES);
    printf("Corner masks baked for the whole run: %d (%s)\n", u.ncorners,
           "one per distinct radius the animated scale walks through");
    printf("Pixels the fabric composited instead of the A9: %llu over %d frames.\n",
           (unsigned long long)total_a9, FRAMES);
    printf("\nNOTE: emit-side accounting against the golden model, NOT a hardware\n"
           "measurement. The feasibility study's first recommendation is still to\n"
           "measure the real A9 frame time before building any of this.\n");

    free(ring); free(src); free(fb); free(cmds);
    return 0;
}
