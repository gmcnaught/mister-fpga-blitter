/*
 *  qt_blitter_paintengine.cpp — see qt_blitter_paintengine.h.
 *
 *  NOT built by this repository's Makefile (no Qt dependency here); compile it
 *  in the application's build with -DMFB_HAVE_QT. Everything it can decide
 *  without Qt types is delegated to ui_offload.c, which IS gated against the
 *  golden reference model.
 *
 *  GPL-3.0.
 */
#include "qt_blitter_paintengine.h"

#ifdef MFB_HAVE_QT

#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QTextItem>
#include <QtMath>
#include <cstring>

namespace {

inline uint16_t rgb565(const QColor &c)
{
    return blt_rgb565((uint8_t)c.red(), (uint8_t)c.green(), (uint8_t)c.blue());
}

inline uint8_t alpha8(const QColor &c, qreal opacity)
{
    qreal a = (c.alphaF() * opacity);
    if (a < 0) a = 0;
    if (a > 1) a = 1;
    return (uint8_t)qRound(a * 255.0);
}

/* The blitter's destination geometry is integer pixels, and its only source
 * transforms are HFLIP/VFLIP — so a translate/scale transform maps, and a
 * rotation or shear does not. Whole-scene 90 degree tate rotation is NOT this
 * function's problem: it belongs at the output stage (MiSTer sys/screen_rotate,
 * feasibility doc §5.2), which rotates the finished frame and leaves the
 * compositor drawing upright. */
inline bool axisAligned(const QTransform &t)
{
    return t.type() <= QTransform::TxScale;
}

inline QRect mapRect(const QTransform &t, const QRectF &r)
{
    return t.mapRect(r).toAlignedRect();
}

} // namespace

QtBlitterPaintEngine::QtBlitterPaintEngine(uio_t *u)
    : QPaintEngine(QPaintEngine::PrimitiveTransform |
                   QPaintEngine::PixmapTransform |
                   QPaintEngine::AlphaBlend |
                   QPaintEngine::PainterPaths)
    , m_uio(u)
{
}

QtBlitterPaintEngine::~QtBlitterPaintEngine() = default;

/*  begin()/end() bracket ONE Qt paint session, not one blitter frame: the
 *  application still owns the frame lifecycle (uio_begin_frame before painting,
 *  uio_end_frame + the doorbell after), because the target buffer, the clear
 *  colour and the present are transport decisions, not painting decisions. */
bool QtBlitterPaintEngine::begin(QPaintDevice *dev)
{
    Q_UNUSED(dev);
    if (!m_uio) return false;
    recycleScratch();
    m_fallback.reset();
    m_brush = Qt::transparent;
    m_pen = Qt::white;
    m_opacity = 1.0;
    m_transform = QTransform();
    m_hasClip = false;
    return true;
}

bool QtBlitterPaintEngine::end()
{
    return true;
}

void QtBlitterPaintEngine::recycleScratch()
{
    /* Free what was uploaded two frames ago: the fabric may still be reading
     * the previous frame's sources. */
    int old = m_scratchBank ^ 1;
    for (const Scratch &s : m_scratch[old])
        blt_emitter_free(m_uio->e, s.off, s.size);
    m_scratch[old].clear();
    m_scratchBank = old;
}

void QtBlitterPaintEngine::updateState(const QPaintEngineState &state)
{
    const QPaintEngine::DirtyFlags f = state.state();
    if (f & QPaintEngine::DirtyBrush)   m_brush = state.brush().color();
    if (f & QPaintEngine::DirtyPen)     m_pen = state.pen().color();
    if (f & QPaintEngine::DirtyOpacity) m_opacity = state.opacity();
    if (f & QPaintEngine::DirtyTransform) m_transform = state.transform();
    if (f & (QPaintEngine::DirtyClipRegion | QPaintEngine::DirtyClipPath |
             QPaintEngine::DirtyClipEnabled)) {
        /* Rectangular clips (ListView / Flickable, `clip: true`) become a
         * destination-rect clamp; a non-rectangular clip is not expressible and
         * is intersected conservatively with its bounding rect. */
        if (state.clipOperation() == Qt::NoClip) {
            m_hasClip = false;
        } else {
            m_clip = state.clipRegion().boundingRect();
            m_hasClip = true;
        }
    }
}

/* ── rectangles ─────────────────────────────────────────────────────────── */
void QtBlitterPaintEngine::drawRects(const QRect *rects, int count)
{
    for (int i = 0; i < count; i++) {
        const QRectF r(rects[i]);
        drawRects(&r, 1);
    }
}

void QtBlitterPaintEngine::drawRects(const QRectF *rects, int count)
{
    for (int i = 0; i < count; i++) {
        if (!axisAligned(m_transform)) {
            m_fallback.transforms++;
            m_fallback.px += (quint64)rects[i].width() * (quint64)rects[i].height();
            continue;
        }
        QRect r = mapRect(m_transform, rects[i]);
        if (m_hasClip) r &= m_clip;
        if (r.isEmpty()) continue;
        uio_fill(m_uio, uio_rect_t{ r.x(), r.y(), r.width(), r.height() },
                 rgb565(m_brush), alpha8(m_brush, m_opacity));
    }
}

/* ── rounded rectangles: the 39 antialiased-corner sites ────────────────── */
bool QtBlitterPaintEngine::asRoundedRect(const QPainterPath &path, QRect *rect, int *radius) const
{
    const QRectF br = path.boundingRect();
    if (br.width() < 2 || br.height() < 2) return false;
    if (path.elementCount() < 4) return false;

    /* A rounded rect meets its top edge at left+r: take the leftmost element
     * sitting on the top edge and read the radius off it. */
    qreal best = br.width();
    bool found = false;
    for (int i = 0; i < path.elementCount(); i++) {
        const QPainterPath::Element e = path.elementAt(i);
        if (qAbs(e.y - br.top()) > 0.01) continue;
        qreal r = e.x - br.left();
        if (r >= 0.5 && r < best) { best = r; found = true; }
    }
    if (!found) return false;

    const int r = qRound(best);
    if (r < 1 || r > UIO_CORNER_MAX_RADIUS) return false;

    /* Verify: rebuild the shape Qt would have produced and compare. Anything
     * that is not exactly a uniform-radius rounded rect (a squircle, a
     * per-corner radius, a clipped path) must take the fallback rather than be
     * silently redrawn as something else. */
    QPainterPath probe;
    probe.addRoundedRect(br, r, r);
    if (probe.elementCount() != path.elementCount()) return false;
    for (int i = 0; i < path.elementCount(); i++) {
        const QPainterPath::Element a = path.elementAt(i), b = probe.elementAt(i);
        if (a.type != b.type || qAbs(a.x - b.x) > 0.02 || qAbs(a.y - b.y) > 0.02) return false;
    }

    *rect = br.toAlignedRect();
    *radius = r;
    return true;
}

void QtBlitterPaintEngine::drawPath(const QPainterPath &path)
{
    QRect r;
    int radius = 0;
    if (axisAligned(m_transform) && asRoundedRect(path, &r, &radius)) {
        r = mapRect(m_transform, QRectF(r));
        radius = qRound(radius * m_transform.m11());
        if (m_hasClip && !m_clip.contains(r)) {
            /* A rounded rect cut by a clip is not four whole arcs any more. */
            m_fallback.other++;
            m_fallback.px += (quint64)r.width() * (quint64)r.height();
        } else {
            uio_rounded_rect(m_uio, uio_rect_t{ r.x(), r.y(), r.width(), r.height() },
                             radius, rgb565(m_brush), alpha8(m_brush, m_opacity));
            return;
        }
    }

    const QRectF b = m_transform.mapRect(path.boundingRect());
    fallbackRaster(b, [&](QPainter &p) { p.fillPath(path.translated(-b.topLeft()), m_brush); },
                   &m_fallback.paths);
}

/* ── images: the 69 scaling sites ───────────────────────────────────────── */
uio_image_ref_t QtBlitterPaintEngine::imageRef(const QImage &img)
{
    const qint64 key = img.cacheKey();
    auto it = m_images.find(key);
    if (it != m_images.end()) return it.value();

    uio_image_ref_t ref;
    memset(&ref, 0, sizeof ref);
    if (img.hasAlphaChannel()) {
        /* ARGB4444 for per-pixel alpha (PALPHA blits). Note this format cannot
         * be fabric-SCALED: the triangle path samples 16bpp colour only, so a
         * scaled draw of an image with alpha takes the fallback below. */
        QImage a = img.convertToFormat(QImage::Format_ARGB32);
        QVector<uint16_t> buf(a.width() * a.height());
        for (int y = 0; y < a.height(); y++) {
            const QRgb *row = reinterpret_cast<const QRgb *>(a.constScanLine(y));
            for (int x = 0; x < a.width(); x++) {
                const QRgb c = row[x];
                buf[y * a.width() + x] = (uint16_t)(((qAlpha(c) >> 4) << 12) |
                                                    ((qRed(c)   >> 4) <<  8) |
                                                    ((qGreen(c) >> 4) <<  4) |
                                                     (qBlue(c)  >> 4));
            }
        }
        ref = uio_upload_image_alpha(m_uio, buf.constData(), a.width(), a.height(),
                                     a.width() * 2);
    } else {
        QImage c = img.convertToFormat(QImage::Format_RGB16);   /* RGB565 */
        ref = uio_upload_image(m_uio, reinterpret_cast<const uint16_t *>(c.constBits()),
                               c.width(), c.height(), c.bytesPerLine());
    }
    if (ref.surf.valid) m_images.insert(key, ref);
    return ref;
}

void QtBlitterPaintEngine::drawImage(const QRectF &r, const QImage &img, const QRectF &sr,
                                     Qt::ImageConversionFlags flags)
{
    Q_UNUSED(flags);
    const QRect dst = mapRect(m_transform, r);
    if (!axisAligned(m_transform)) {
        m_fallback.transforms++;
        m_fallback.px += (quint64)dst.width() * (quint64)dst.height();
        return;
    }

    const QRect src = sr.isNull() ? img.rect() : sr.toAlignedRect();
    const bool scaled = (dst.width() != src.width() || dst.height() != src.height());
    uio_image_ref_t ref = imageRef(img);
    if (!ref.surf.valid) { m_fallback.other++; return; }

    if (scaled && img.hasAlphaChannel()) {
        /* Per-pixel alpha AND an arbitrary ratio: the triangle path has no
         * alpha channel, so this one genuinely stays on the A9. Counted, not
         * dropped — this is the row the study wants measured. */
        fallbackRaster(QRectF(dst), [&](QPainter &p) {
            p.drawImage(QRectF(0, 0, dst.width(), dst.height()), img, sr);
        }, &m_fallback.other);
        return;
    }

    const uint8_t a = alpha8(QColor(Qt::white), m_opacity);
    const uint8_t blend = img.hasAlphaChannel() ? BLT_BLEND_PALPHA
                        : (a < 255 ? BLT_BLEND_CONST_ALPHA : BLT_BLEND_COPY);

    if (!scaled) {
        uio_image_blit(m_uio, ref, dst.x(), dst.y(), blend, 0, a);
        return;
    }

    /*  Arbitrary-ratio scaling on the FABRIC. This is the draw that a decode-
     *  time pre-scale cannot serve when the ratio changes per frame (a focus
     *  zoom, a held-focus animation) — here it costs one command. */
    uio_scale_t s;
    memset(&s, 0, sizeof s);
    s.dst = uio_rect_t{ dst.x(), dst.y(), dst.width(), dst.height() };
    s.src = uio_rect_t{ src.x(), src.y(), src.width(), src.height() };
    s.blend = blend;
    s.alpha = a;
    uio_image_scaled(m_uio, ref, &s);
}

void QtBlitterPaintEngine::drawPixmap(const QRectF &r, const QPixmap &pm, const QRectF &sr)
{
    drawImage(r, pm.toImage(), sr, Qt::AutoColor);
}

/* ── text: free on the CRT path, a fallback everywhere else ─────────────── */
void QtBlitterPaintEngine::drawTextItem(const QPointF &p, const QTextItem &ti)
{
    const QFont f = ti.font();
    const bool atlasFont = !m_atlasFamily.isEmpty()
                        && f.family() == m_atlasFamily
                        && f.pixelSize() == UIO_FONT_CELL_H;

    if (atlasFont && axisAligned(m_transform)) {
        const QPointF o = m_transform.map(p);
        const int top = qRound(o.y() - ti.ascent());
        uio_text(m_uio, qRound(o.x()), top, ti.text().toLatin1().constData(), rgb565(m_pen));
        return;
    }

    /* Antialiased/scalable text is the study's known non-mappable case off the
     * CRT path: rasterize it on the A9 and say so. */
    const QRectF b(m_transform.map(p) - QPointF(0, ti.ascent()),
                   QSizeF(ti.width(), ti.ascent() + ti.descent()));
    fallbackRaster(b, [&](QPainter &pnt) {
        pnt.setPen(m_pen);
        pnt.setFont(f);
        pnt.drawText(QPointF(0, ti.ascent()), ti.text());
    }, &m_fallback.text);
}

/* ── the fallback: raster on the A9, upload, blit, COUNT ────────────────── */
void QtBlitterPaintEngine::fallbackRaster(const QRectF &bounds,
                                          const std::function<void(QPainter &)> &draw,
                                          quint32 *counter)
{
    QRect b = bounds.toAlignedRect();
    if (m_hasClip) b &= m_clip;
    if (b.isEmpty() || !m_uio) return;

    QImage tmp(b.size(), QImage::Format_ARGB32_Premultiplied);
    tmp.fill(Qt::transparent);
    {
        QPainter p(&tmp);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setOpacity(m_opacity);
        draw(p);
    }

    QVector<uint16_t> buf(b.width() * b.height());
    for (int y = 0; y < b.height(); y++) {
        const QRgb *row = reinterpret_cast<const QRgb *>(tmp.constScanLine(y));
        for (int x = 0; x < b.width(); x++) {
            const QRgb c = row[x];
            buf[y * b.width() + x] = (uint16_t)(((qAlpha(c) >> 4) << 12) |
                                                ((qRed(c)   >> 4) <<  8) |
                                                ((qGreen(c) >> 4) <<  4) |
                                                 (qBlue(c)  >> 4));
        }
    }
    blt_surface_ref_t s = blt_upload_argb4444(m_uio->e, buf.constData(),
                                              b.width(), b.height(), b.width() * 2);
    if (s.valid) {
        blt_blit(m_uio->e, s, 0, 0, b.width(), b.height(), b.x(), b.y(),
                 BLT_BLEND_PALPHA, 0, 255, 0);
        m_scratch[m_scratchBank].append(Scratch{ s.off, s.size });
    }

    if (counter) (*counter)++;
    m_fallback.px += (quint64)b.width() * (quint64)b.height();
}

#endif /* MFB_HAVE_QT */
