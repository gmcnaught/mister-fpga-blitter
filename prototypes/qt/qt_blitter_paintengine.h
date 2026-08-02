/*
 *  qt_blitter_paintengine.h — Seam A: intercept Qt's software paint path and
 *                             emit a blitter display list instead.
 *
 *  This is the adapter the feasibility study calls the "singular and clean"
 *  interception point (docs/qt-offload-feasibility.md §1): with no GPU on the
 *  Cyclone V, Qt Quick runs its Software adaptation and composites the whole
 *  scene through QPainter, so ONE QPaintEngine sees every draw in the scene —
 *  there is no widget/QML split to bridge.
 *
 *  It is deliberately thin. Every decision that can be made without Qt types
 *  lives in ui_offload.c, which is gated against the golden reference model in
 *  test_ui_offload.c; this file only translates QPainter calls into that API:
 *
 *      QPainter call                     -> offload
 *      ─────────────────────────────────────────────────────────────────────
 *      fillRect / drawRects (aligned)    -> uio_fill              (FILL)
 *      drawRoundedRect / drawPath (RR)   -> uio_rounded_rect      (FILL + PALPHA arcs)
 *      drawImage / drawPixmap 1:1        -> uio_image_blit        (BLIT)
 *      drawImage / drawPixmap scaled     -> uio_image_scaled      (TRILIST quad)
 *      drawTextItem (fixed bitmap font)  -> uio_text              (PALPHA + COLORMOD)
 *      anything else                     -> raster on the A9, upload, blit
 *                                           and COUNT it (never a silent drop)
 *
 *  BUILDING
 *  --------
 *  This file is NOT built by the prototype's Makefile: this repository has no
 *  Qt dependency, and `make test` gates the C layer instead. Compile it in the
 *  application's own build with -DMFB_HAVE_QT and Qt's headers on the include
 *  path (Qt 5.15 or Qt 6, widgets/gui module). Without that define the
 *  translation unit is empty, so it can sit in a shared tree harmlessly.
 *
 *  INSTALLING IT
 *  -------------
 *  A QPaintEngine reaches Qt through a QPaintDevice. The intended host is the
 *  application's existing present path: a QPaintDevice whose paintEngine()
 *  returns this engine, set as the window's backing store surface, so the same
 *  DDR slot the app already publishes to is filled by the fabric instead of by
 *  a memcpy of a software-composited frame. The transport (double-buffer slot,
 *  doorbell word, scanout reader) is untouched — only the frame's SOURCE
 *  changes, which is what makes this a low-risk swap.
 *
 *  GPL-3.0.
 */
#ifndef UIO_QT_BLITTER_PAINTENGINE_H
#define UIO_QT_BLITTER_PAINTENGINE_H

#ifdef MFB_HAVE_QT

#include <QPaintEngine>
#include <QColor>
#include <QHash>
#include <QString>
#include <QTransform>
#include <QVector>
#include <functional>

extern "C" {
#include "ui_offload.h"
}

/*  Counters for the draws that did NOT make it onto the fabric.
 *
 *  The study is explicit that the decisive number is not "what fraction of
 *  draws fall back" but "what fraction of A9 *time* falls back" (§5.4), so the
 *  fallback path records destination area, not just a call count. A frame with
 *  a non-zero `px` here is a frame that still rasterized on the A9 — which is
 *  a finding, not a failure, and must be visible rather than silent.
 */
struct QtBlitterFallbackStats {
    quint32 paths      = 0;   /* non-rounded-rect QPainterPath              */
    quint32 text       = 0;   /* glyph runs from a font the atlas can't serve */
    quint32 transforms = 0;   /* rotated/sheared draws (see the note on tate) */
    quint32 other      = 0;
    quint64 px         = 0;   /* destination pixels rasterized on the A9     */
    void reset() { paths = text = transforms = other = 0; px = 0; }
};

class QtBlitterPaintEngine : public QPaintEngine
{
public:
    /*  `u` is an initialised ui_offload context bound to an emitter whose ring
     *  and source heap live in the DDR regions the fabric reads. The engine
     *  does not own it: resource uploads (glyph atlas, corner masks, decoded
     *  images) persist across frames by design, and outlive any one paint. */
    explicit QtBlitterPaintEngine(uio_t *u);
    ~QtBlitterPaintEngine() override;

    bool begin(QPaintDevice *dev) override;
    bool end() override;
    Type type() const override { return QPaintEngine::User; }

    void updateState(const QPaintEngineState &state) override;

    void drawRects(const QRect *rects, int count) override;
    void drawRects(const QRectF *rects, int count) override;
    void drawPath(const QPainterPath &path) override;
    void drawImage(const QRectF &r, const QImage &img, const QRectF &sr,
                   Qt::ImageConversionFlags flags) override;
    void drawPixmap(const QRectF &r, const QPixmap &pm, const QRectF &sr) override;
    void drawTextItem(const QPointF &p, const QTextItem &ti) override;

    const QtBlitterFallbackStats &fallbacks() const { return m_fallback; }
    const uio_stats_t &stats() const { return m_uio->stats; }

    /*  Text is mapped to the 6x8 atlas only when the painter's font IS the
     *  fixed-cell bitmap font the atlas was baked from — the study's §4
     *  "bitmap-font freebie" holds on the CRT path and nowhere else. Tell the
     *  engine which family that is; everything else falls back to A9 raster. */
    void setAtlasFontFamily(const QString &family) { m_atlasFamily = family; }

private:
    /*  Recognise the one path shape that matters: a rounded rectangle with a
     *  single uniform radius (Tile.qml cards, Modal.qml, CoreStatusPill.qml,
     *  focus rings — the 39 antialiased-corner sites). Returns false for
     *  anything else, which then takes the fallback. */
    bool asRoundedRect(const QPainterPath &path, QRect *rect, int *radius) const;

    /* Rasterize an unmappable draw on the A9, upload it, blit it, and count it. */
    void fallbackRaster(const QRectF &bounds, const std::function<void(QPainter &)> &draw,
                        quint32 *counter);

    /*  Scratch uploads made by the fallback path are per-frame garbage, but the
     *  fabric is still reading frame N's sources while the A9 builds frame N+1,
     *  so they are freed two frames later — never at the end of the frame that
     *  made them. (The permanent atlases in m_images are never freed.) */
    void recycleScratch();

    /* Upload cache: QImage::cacheKey() -> uploaded RGB565 surface. Decoded art
     * is uploaded ONCE and re-blitted every frame for free, which is what makes
     * per-frame recompositing cheap in the first place. */
    uio_image_ref_t imageRef(const QImage &img);

    struct Scratch { quint32 off, size; };

    uio_t   *m_uio = nullptr;
    QHash<qint64, uio_image_ref_t> m_images;
    QVector<Scratch> m_scratch[2];      /* this frame / the previous one */
    int      m_scratchBank = 0;
    QtBlitterFallbackStats m_fallback;
    QString  m_atlasFamily;

    /* mirrored painter state */
    QColor     m_brush;
    QColor     m_pen;
    qreal      m_opacity   = 1.0;
    QTransform m_transform;
    QRect      m_clip;
    bool       m_hasClip   = false;
};

#endif /* MFB_HAVE_QT */
#endif /* UIO_QT_BLITTER_PAINTENGINE_H */
