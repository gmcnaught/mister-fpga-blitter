/*
 *  zaparoo_ui.h — the Zaparoo front-end's own theme and sizing rules, in C.
 *
 *  A port of the design tokens the real UI derives every dimension from, so
 *  the offload demo composites the SHAPE THE APP ACTUALLY DRAWS rather than an
 *  invented mock-up. Ported 1:1 from the QML singletons in
 *  github.com/ZaparooProject/zaparoo-frontend:
 *
 *      src/ui/theme/Theme.qml      colours + the CRT bitmap font family
 *      src/ui/theme/Sizing.qml     pctH/pctW/fontSize/stroke, corner radius,
 *                                  header metrics, the browse-grid shape
 *                                  selector, and the cover decode tiers
 *      src/ui/theme/Motion.qml     press durations + the push-in scale target
 *      src/ui/components/Tile.qml  card / focus-ring / caption geometry
 *
 *  Two things fall out of these rules that matter for the offload, and both
 *  are load-bearing in demo_frame.c:
 *
 *  1. On the CRT path fontSize() collapses to 8 px, and the UI font is a
 *     fixed-cell 6x8 bitmap face (MxPlus HP 100LX 6x8) with NoAntialias. Every
 *     label on this path is therefore a uniform glyph blit — the study's §4.
 *
 *  2. gamesGridCoverSourceSize() snaps a cover's decode size UP to a tier
 *     (128/256/512/768) while the painted box is whatever the grid solve
 *     produces. The two are equal only by coincidence, so a grid of covers is
 *     a grid of ARBITRARY-RATIO resamples — the study's §3 item 2, and the
 *     draw uio_image_scaled() hands to the fabric.
 *
 *  Values are computed, not copied: change the screen size and every number
 *  re-solves exactly as the QML would.
 *
 *  GPL-3.0. (The ported constants describe the upstream design; the upstream
 *  project is PolyForm-Noncommercial and none of its code is included here.)
 */
#ifndef UIO_ZAPAROO_UI_H
#define UIO_ZAPAROO_UI_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Theme.qml ───────────────────────────────────────────────────────────── */
/* 24-bit hex exactly as the QML declares it; zui_565() converts. */
#define ZUI_BG_DEEP        0x0F0F23u
#define ZUI_BG_PANEL       0x1A1A35u
#define ZUI_BG_BAR         0x0A0A15u
#define ZUI_SURFACE_CARD   0x22223Au
#define ZUI_SELECTION      0x3A3A66u
#define ZUI_BORDER_SUBTLE  0x1A1A2Eu
#define ZUI_BORDER_MID     0x404060u
#define ZUI_TEXT_PRIMARY   0xFFFFFFu
#define ZUI_TEXT_LABEL     0x888888u
#define ZUI_TEXT_VARIANT   0x8A8AB2u
#define ZUI_ACCENT         0xFFB347u
#define ZUI_STATE_MARKER   0x9898CCu
#define ZUI_LOGO_PRIMARY   0x9898CCu
#define ZUI_LOGO_SECONDARY 0x6060A8u
#define ZUI_LOGO_SHADOW    0x3C3C80u
#define ZUI_LOGO_FOCUS_1   0xFFE3B8u
#define ZUI_LOGO_FOCUS_2   ZUI_ACCENT
#define ZUI_ERROR          0xFF8A7Au

/* RGB888 hex -> RGB565. */
uint16_t zui_565(uint32_t hex);

/* ── Motion.qml ──────────────────────────────────────────────────────────── */
#define ZUI_PRESS_MS        80    /* push-in feedback on accept/activate      */
#define ZUI_SETTLE_MS      110
#define ZUI_PRESS_SCALE_CRT 90    /* percent; 0.90 on the CRT path            */
/*  The persistent 1.06 focus scale Tile.qml REMOVED ("a persistent,
 *  per-focus-move cost ... on covered grids"). It is the concrete thing an
 *  offload buys back, so the demo can switch it on. */
#define ZUI_FOCUS_SCALE_RESTORED 106

/* ── Sizing.qml ──────────────────────────────────────────────────────────── */
typedef struct {
    int screen_w, screen_h;
    int crt;                      /* crtNativePath */
} zui_sizing_t;

int zui_pctH(const zui_sizing_t *s, double percent);
int zui_pctW(const zui_sizing_t *s, double percent);
int zui_stroke(double value);
int zui_half(double value);
/* Minimum 8 px to stay legible at 240p; the CRT path quantises to 8 or 16. */
int zui_font_size(const zui_sizing_t *s, double percent);
int zui_corner_radius(const zui_sizing_t *s);           /* pctH(3.5)          */

/* Header: logo row + status row, with the pill's row space always reserved. */
int zui_header_row_height(const zui_sizing_t *s);       /* fontSize(3.4)      */
int zui_header_stack_gap(const zui_sizing_t *s);        /* pctH(0.8)          */
int zui_header_top_margin(const zui_sizing_t *s);       /* pctH(2)            */
int zui_header_side_margin(const zui_sizing_t *s);      /* pctW(2)            */
int zui_header_height(const zui_sizing_t *s);
int zui_header_bottom(const zui_sizing_t *s);

/*  The browse-grid shape selector: score every (columns, rows) that clears the
 *  minimum cell size by how far its cell aspect misses the target, plus a
 *  penalty for missing the preferred page size. Ported from _selectGridShape;
 *  the games/systems configs differ only in their bounds. */
void zui_games_grid_shape(const zui_sizing_t *s, int *columns, int *rows);
void zui_systems_grid_shape(const zui_sizing_t *s, int *columns, int *rows);

/* Cover decode tiers, mirroring Core's resize ladder: 128/256/512/768. */
int zui_snap_cover_tier(int px);
/* Painted cover box height for a games-grid tile, and the decode size that
 * gets snapped up from it — the pair whose mismatch forces the resample. */
int zui_games_grid_cover_box(const zui_sizing_t *s);
int zui_games_grid_cover_source_size(const zui_sizing_t *s);

/* ── Tile.qml ────────────────────────────────────────────────────────────── */
typedef struct {
    int corner_radius;    /* card radius (Sizing.cornerRadius)               */
    int padding;          /* pctH(2)   — cover inset, top/left/right         */
    int outline_gap;      /* pctH(0.4) — focus ring inset from the card edge */
    int outline_width;    /* stroke(pctH(0.6)) — visible ring thickness      */
    int caption_height;   /* pctH(5.5)                                       */
    int caption_gap;      /* pctH(0.4)                                       */
    int caption_text_size;/* fontSize(2.2)                                   */
    int caption_side_inset;
    int border_width;     /* stroke(1) — the static card edge                */
} zui_tile_t;

void zui_tile_metrics(const zui_sizing_t *s, zui_tile_t *t);

#ifdef __cplusplus
}
#endif
#endif /* UIO_ZAPAROO_UI_H */
