/*
 *  zaparoo_ui.c — see zaparoo_ui.h.
 *
 *  JS Math.round() rounds half AWAY FROM ZERO toward +infinity, so the ports
 *  use floor(x + 0.5) rather than C's round() to stay bit-identical with the
 *  QML for the .5 cases the percentage helpers hit constantly.
 *
 *  GPL-3.0.
 */
#include "zaparoo_ui.h"
#include <math.h>

static int js_round(double v) { return (int)floor(v + 0.5); }

uint16_t zui_565(uint32_t hex)
{
    unsigned r = (hex >> 16) & 0xFFu, g = (hex >> 8) & 0xFFu, b = hex & 0xFFu;
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

/* swapPercentageAxes (used when the scene is rotated) is not modelled: whole
 * frame rotation belongs at the output stage — MiSTer sys/screen_rotate — not
 * in the compositor (feasibility doc §5.2). */
int zui_pctH(const zui_sizing_t *s, double percent) { return js_round(s->screen_h * percent / 100.0); }
int zui_pctW(const zui_sizing_t *s, double percent) { return js_round(s->screen_w * percent / 100.0); }

int zui_stroke(double value) { int v = js_round(value); return v < 1 ? 1 : v; }
int zui_half(double value)   { return js_round(value / 2.0); }

int zui_font_size(const zui_sizing_t *s, double percent)
{
    int size = zui_pctH(s, percent);
    if (size < 8) size = 8;
    if (!s->crt) return size;
    return size < 12 ? 8 : 16;
}

int zui_corner_radius(const zui_sizing_t *s)      { return zui_pctH(s, 3.5); }
int zui_header_row_height(const zui_sizing_t *s)  { return zui_font_size(s, 3.4); }
int zui_header_stack_gap(const zui_sizing_t *s)   { return zui_pctH(s, 0.8); }
int zui_header_top_margin(const zui_sizing_t *s)  { return zui_pctH(s, 2); }
int zui_header_side_margin(const zui_sizing_t *s) { return zui_pctW(s, 2); }

int zui_header_height(const zui_sizing_t *s)
{
    return 2 * zui_header_row_height(s) + zui_header_stack_gap(s);
}

int zui_header_bottom(const zui_sizing_t *s)
{
    return zui_header_top_margin(s) + zui_header_height(s);
}

/* ── grid shape ──────────────────────────────────────────────────────────── */
typedef struct {
    int    min_cell_w, min_cell_h;
    int    preferred_page, min_cols, max_cols, min_rows, max_rows;
    double target_aspect;
} grid_cfg_t;

static void base_cfg(const zui_sizing_t *s, grid_cfg_t *c)
{
    c->min_cell_w     = s->crt ? 72 : 160;
    c->preferred_page = s->crt ? 6 : 10;
    c->min_cols       = 2;
    c->max_cols       = s->crt ? 3 : 5;
    c->min_rows       = 2;
    c->max_rows       = s->crt ? 3 : 5;
}

static void select_shape(int vw, int vh, const grid_cfg_t *o, int *columns, int *rows)
{
    double safe_w = vw > 1 ? (double)vw : 1.0;
    double safe_h = vh > 1 ? (double)vh : 1.0;
    int best_c = o->min_cols, best_r = o->min_rows;
    double best = 1e300;

    for (int c = o->min_cols; c <= o->max_cols; c++) {
        double cw = safe_w / c;
        if (cw < o->min_cell_w) continue;
        for (int r = o->min_rows; r <= o->max_rows; r++) {
            double ch = safe_h / r;
            if (ch < o->min_cell_h) continue;
            double aspect_err = fabs(log((cw / ch) / o->target_aspect));
            double page_pen = fabs((double)(c * r) - o->preferred_page) * 0.04;
            double score = aspect_err + page_pen;
            if (score < best) { best = score; best_c = c; best_r = r; }
        }
    }
    *columns = best_c;
    *rows    = best_r;
}

void zui_games_grid_shape(const zui_sizing_t *s, int *columns, int *rows)
{
    grid_cfg_t c;
    base_cfg(s, &c);
    c.min_cell_h    = s->crt ? 96 : 210;
    c.target_aspect = s->crt ? 0.78 : 0.71;
    select_shape(s->screen_w, s->screen_h, &c, columns, rows);
}

void zui_systems_grid_shape(const zui_sizing_t *s, int *columns, int *rows)
{
    grid_cfg_t c;
    base_cfg(s, &c);
    c.min_cell_h     = s->crt ? 72 : 140;
    c.preferred_page = s->crt ? 9 : 12;
    c.target_aspect  = 1.25;
    select_shape(s->screen_w, s->screen_h, &c, columns, rows);
}

/* ── cover decode tiers ──────────────────────────────────────────────────── */
int zui_snap_cover_tier(int px)
{
    if (px <= 128) return 128;
    if (px <= 256) return 256;
    if (px <= 512) return 512;
    return 768;
}

int zui_games_grid_cover_box(const zui_sizing_t *s)
{
    int columns, rows;
    zui_games_grid_shape(s, &columns, &rows);
    int vh = s->screen_h > 1 ? s->screen_h : 1;
    int tile_h = (vh + rows - 1) / rows;                 /* Math.ceil(vh/rows) */
    int box = tile_h - zui_pctH(s, 2) - (zui_pctH(s, 5.5) + zui_pctH(s, 0.4));
    return box < 1 ? 1 : box;
}

int zui_games_grid_cover_source_size(const zui_sizing_t *s)
{
    return zui_snap_cover_tier(zui_games_grid_cover_box(s));
}

/* ── tile metrics ────────────────────────────────────────────────────────── */
void zui_tile_metrics(const zui_sizing_t *s, zui_tile_t *t)
{
    t->corner_radius     = zui_corner_radius(s);
    t->padding           = zui_pctH(s, 2);
    t->outline_gap       = zui_pctH(s, 0.4);
    t->outline_width     = zui_stroke(zui_pctH(s, 0.6));
    t->caption_height    = zui_pctH(s, 5.5);
    t->caption_gap       = zui_pctH(s, 0.4);
    t->caption_text_size = zui_font_size(s, 2.2);
    t->caption_side_inset = zui_half(t->corner_radius);
    t->border_width      = zui_stroke(1);
}
