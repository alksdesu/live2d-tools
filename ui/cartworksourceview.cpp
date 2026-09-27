#include "cartworksourceview.h"
#include "theme.h"
#include "opengl/cuvgeometry.h"
#include <QApplication>
#include <QFocusEvent>
#include <QHideEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QResizeEvent>
#include <QShowEvent>
#include <QTimer>
#include <QWheelEvent>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>

namespace {
bool fail(QString* error, const QString& text) { if (error) *error = text; return false; }
std::array<double, 9> values(const QTransform& m) { return {m.m11(), m.m12(), m.m13(), m.m21(), m.m22(), m.m23(), m.dx(), m.dy(), m.m33()}; }
bool equal(const QTransform& first, const QTransform& second)
{
    const auto a = values(first), b = values(second); return std::memcmp(a.data(), b.data(), sizeof(a)) == 0;
}
bool finite(QPointF point) { return std::isfinite(point.x()) && std::isfinite(point.y()); }
bool inverse(const QTransform& m, QTransform& result, QString* error)
{
    for (double value : values(m)) if (!std::isfinite(value)) return fail(error, QStringLiteral("配准映射包含无效数值"));
    if (m.m13() != 0 || m.m23() != 0 || m.m33() != 1) return fail(error, QStringLiteral("配准只支持二维仿射映射"));
    const double scale = std::max({std::abs(m.m11()), std::abs(m.m12()), std::abs(m.m21()), std::abs(m.m22())});
    if (scale == 0) return fail(error, QStringLiteral("配准映射不可逆"));
    const double a = m.m11() / scale, b = m.m12() / scale, c = m.m21() / scale, d = m.m22() / scale;
    const double determinant = a * d - b * c;
    const double norm = std::max(std::abs(a) + std::abs(c), std::abs(b) + std::abs(d));
    const double inverseNorm = std::max(std::abs(d) + std::abs(c), std::abs(b) + std::abs(a));
    if (determinant == 0 || !std::isfinite(determinant) || norm * inverseNorm > std::abs(determinant) * 1e12)
        return fail(error, QStringLiteral("配准映射不可逆或精度不足"));
    const double ia = (d / determinant) / scale, ib = (-b / determinant) / scale;
    const double ic = (-c / determinant) / scale, id = (a / determinant) / scale;
    const QTransform candidate(ia, ib, ic, id, -(ia * m.dx() + ic * m.dy()), -(ib * m.dx() + id * m.dy()));
    for (double value : values(candidate)) if (!std::isfinite(value)) return fail(error, QStringLiteral("配准逆映射超出有限范围"));
    result = candidate; return true;
}
QRectF rectangle(QRect rect) { return {double(rect.x()), double(rect.y()), double(rect.width()), double(rect.height())}; }
bool contains(QRect rect, QPointF point)
{
    return point.x() >= rect.x() && point.y() >= rect.y() &&
        point.x() < double(rect.x()) + rect.width() && point.y() < double(rect.y()) + rect.height();
}
bool imageBounds(const QImage& image, QRect bounds, QString* error)
{
    if (bounds.width() < 0 || bounds.height() < 0 ||
        double(bounds.x()) + bounds.width() > std::numeric_limits<int>::max() ||
        double(bounds.y()) + bounds.height() > std::numeric_limits<int>::max())
        return fail(error, QStringLiteral("素材区域超出整数像素范围"));
    if (image.isNull()) return bounds.isEmpty() || fail(error, QStringLiteral("素材区域缺少图像"));
    return image.size() == bounds.size() || fail(error, QStringLiteral("图像尺寸与素材区域不一致"));
}
bool mappedBounds(QRect bounds, const QTransform& mapping, QString* error)
{
    const auto rect = rectangle(bounds);
    const std::array<QPointF, 4> points{rect.topLeft(), rect.topRight(), rect.bottomRight(), rect.bottomLeft()};
    for (const auto& point : points) if (!finite(mapping.map(point))) return fail(error, QStringLiteral("配准后的素材坐标超出有限范围"));
    if (!bounds.isEmpty() && (mapping.map(points[0]) == mapping.map(points[1]) || mapping.map(points[0]) == mapping.map(points[3])))
        return fail(error, QStringLiteral("配准后的素材范围精度不足"));
    return true;
}
}

CArtworkSourceView::CArtworkSourceView(QWidget* parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("artworkSourceView")); setAccessibleName(QStringLiteral("PSD 来源像素配准画布"));
    setFocusPolicy(Qt::StrongFocus); setMouseTracking(true); setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}
bool CArtworkSourceView::ReadyForData(QString* error) const
{
    return (!m_busy && !m_gesture && !m_dragReady && !m_pan) || fail(error, QStringLiteral("请先完成或取消当前配准拖动"));
}
bool CArtworkSourceView::SetContent(const Content& content, QString* error)
{
    if (!ReadyForData(error) || !imageBounds(content.source, content.sourceBounds, error) || !imageBounds(content.reference, content.atlasBounds, error)) return false;
    if (content.documentCanvas.width() <= 0 || content.documentCanvas.height() <= 0) return fail(error, QStringLiteral("PSD 文档画布尺寸无效"));
    QTransform inverted;
    if (!inverse(content.atlasToDocument, inverted, error) || !mappedBounds(content.atlasBounds, content.atlasToDocument, error)) return false;
    if (content.atlasPixels.size() > 1000000 || content.triangles.size() > 6000000 || content.triangles.size() % 3) return fail(error, QStringLiteral("配准网格数量或连线无效"));
    for (const auto& point : content.atlasPixels) if (!finite(point) || !finite(content.atlasToDocument.map(point))) return fail(error, QStringLiteral("配准网格包含无效像素坐标"));
    for (auto index : content.triangles) if (index >= content.atlasPixels.size()) return fail(error, QStringLiteral("配准网格连线越界"));
    m_source = content.source; m_reference = content.reference; m_sourceBounds = content.sourceBounds; m_atlasBounds = content.atlasBounds;
    m_documentCanvas = content.documentCanvas; m_mapping = content.atlasToDocument; m_inverse = inverted;
    m_vertices = content.atlasPixels; m_triangles = content.triangles; FitAll(); update(); if (error) error->clear(); return true;
}
bool CArtworkSourceView::SetSource(const QImage& raster, QRect bounds, QSize canvas, QString* error)
{
    if (!ReadyForData(error) || !imageBounds(raster, bounds, error)) return false;
    if (canvas.width() <= 0 || canvas.height() <= 0) return fail(error, QStringLiteral("PSD 文档画布尺寸无效"));
    m_source = raster; m_sourceBounds = bounds; m_documentCanvas = canvas; FitAll(); update();
    if (error) error->clear(); return true;
}
bool CArtworkSourceView::SetReference(const QImage& crop, QRect bounds, const QTransform& mapping, QString* error)
{
    if (!ReadyForData(error) || !imageBounds(crop, bounds, error)) return false;
    QTransform inverted;
    if (!inverse(mapping, inverted, error) || !mappedBounds(bounds, mapping, error)) return false;
    for (const auto& point : m_vertices) if (!finite(mapping.map(point))) return fail(error, QStringLiteral("配准后的网格坐标无效"));
    m_reference = crop; m_atlasBounds = bounds; m_mapping = mapping; m_inverse = inverted; FitAll(); update();
    if (error) error->clear(); return true;
}
bool CArtworkSourceView::SetMeshOverlay(const QVector<QPointF>& points, const QVector<quint16>& triangles, QString* error)
{
    if (!ReadyForData(error)) return false;
    if (points.size() > 1000000 || triangles.size() > 6000000 || triangles.size() % 3) return fail(error, QStringLiteral("配准网格数量或连线无效"));
    for (const auto& point : points) if (!finite(point) || !finite(m_mapping.map(point))) return fail(error, QStringLiteral("配准网格包含无效像素坐标"));
    for (auto index : triangles) if (index >= points.size()) return fail(error, QStringLiteral("配准网格连线越界"));
    m_vertices = points; m_triangles = triangles; update(); if (error) error->clear(); return true;
}
bool CArtworkSourceView::PrepareMapping(const QTransform& mapping, QTransform& inverted, QString* error) const
{
    if (!inverse(mapping, inverted, error) || !mappedBounds(m_atlasBounds, mapping, error)) return false;
    for (const auto& point : m_vertices) if (!finite(mapping.map(point))) return fail(error, QStringLiteral("配准后的网格坐标无效"));
    return true;
}
bool CArtworkSourceView::SetMapping(const QTransform& mapping, QString* error)
{
    if (!ReadyForData(error)) return false;
    QTransform inverted; if (!PrepareMapping(mapping, inverted, error)) return false;
    m_mapping = mapping; m_inverse = inverted; update(); if (error) error->clear(); return true;
}
bool CArtworkSourceView::SetSourceOpacity(double opacity)
{
    if (!std::isfinite(opacity) || opacity < 0 || opacity > 1) return false;
    m_sourceOpacity = opacity; update(); return true;
}
bool CArtworkSourceView::SetReferenceOpacity(double opacity)
{
    if (!std::isfinite(opacity) || opacity < 0 || opacity > 1) return false;
    m_referenceOpacity = opacity; update(); return true;
}
void CArtworkSourceView::SetPreviewHandler(MappingHandler handler) { if (!m_busy && !m_gesture) m_preview = std::move(handler); }
void CArtworkSourceView::SetCommitHandler(MappingHandler handler) { if (!m_busy && !m_gesture) m_commit = std::move(handler); }
void CArtworkSourceView::SetCancellationHandler(CancellationHandler handler) { if (!m_busy && !m_gesture) m_cancel = std::move(handler); }
bool CArtworkSourceView::SetEditingEnabled(bool enabled, QString* error)
{
    const QPointer<CArtworkSourceView> alive(this);
    if (!enabled && !CancelGesture(error)) return false;
    if (!alive) return false;
    m_editing = enabled; return true;
}
QPointF CArtworkSourceView::DocumentToViewport(QPointF point) const { return point * m_scale + m_offset; }
QPointF CArtworkSourceView::ViewportToDocument(QPointF point) const { return (point - m_offset) / m_scale; }
CArtworkSourceView::Coordinates CArtworkSourceView::CoordinatesAt(QPointF point) const
{
    Coordinates result; result.document = ViewportToDocument(point); result.layerLocal = result.document - m_sourceBounds.topLeft();
    result.onLayer = !m_source.isNull() && contains(m_sourceBounds, result.document);
    result.hasAtlas = !m_reference.isNull(); result.atlas = m_inverse.map(result.document);
    result.onReference = result.hasAtlas && finite(result.atlas) && contains(m_atlasBounds, result.atlas);
    return result;
}
QSize CArtworkSourceView::minimumSizeHint() const { return {fontMetrics().averageCharWidth() * 28, fontMetrics().height() * 13}; }
QRectF CArtworkSourceView::ReferenceBounds() const { return m_reference.isNull() ? QRectF() : m_mapping.mapRect(rectangle(m_atlasBounds)); }
void CArtworkSourceView::RequestFit(QRectF bounds)
{
    if (m_gesture || m_busy || m_dragReady || m_pan) return;
    if (bounds.isEmpty() || !finite(bounds.topLeft()) || !finite(bounds.bottomRight())) return;
    m_fitBounds = bounds; m_fitPending = true; m_fitViewport = {}; QueueFit();
}
void CArtworkSourceView::QueueFit()
{
    if (!m_fitPending || m_fitQueued || !isVisible()) return;
    m_fitQueued = true;
    QTimer::singleShot(0, this, [this] {
        m_fitQueued = false;
        if (!m_fitPending || !isVisible()) return;
        const int margin = fontMetrics().height();
        if (width() <= margin * 2 || height() <= margin * 2) return;
        if (size() != m_fitViewport) { m_fitViewport = size(); QueueFit(); return; }
        m_scale = std::clamp(std::min((width() - margin * 2) / std::max(1.0, m_fitBounds.width()),
                                     (height() - margin * 2) / std::max(1.0, m_fitBounds.height())), 1e-50, 64.0);
        m_offset = QPointF(width() / 2.0, height() / 2.0) - m_fitBounds.center() * m_scale;
        m_fitPending = false; update(); emit ViewChanged();
    });
}
void CArtworkSourceView::CancelQueuedFit() { m_fitPending = false; m_fitViewport = {}; }
void CArtworkSourceView::FitAll()
{
    QRectF bounds(QPointF(), m_documentCanvas);
    if (!m_source.isNull()) bounds = bounds.united(rectangle(m_sourceBounds));
    if (!m_reference.isNull()) bounds = bounds.united(ReferenceBounds());
    RequestFit(bounds);
}
void CArtworkSourceView::FitSource() { RequestFit(m_source.isNull() ? QRectF(QPointF(), m_documentCanvas) : rectangle(m_sourceBounds)); }
void CArtworkSourceView::FitReference() { RequestFit(ReferenceBounds()); }

bool CArtworkSourceView::PreviewMapping(const QTransform& requested, QString* error)
{
    if (m_busy || !m_gesture) return fail(error, QStringLiteral("配准手势未就绪"));
    QTransform inverted; if (!PrepareMapping(requested, inverted, error)) return false;
    if (equal(requested, m_mapping)) return true;
    const QPointer<CArtworkSourceView> alive(this); const auto handler = m_preview; const auto mapping = requested; bool accepted = true;
    m_busy = true;
    try { if (handler) accepted = handler(mapping, error); }
    catch (...) { accepted = fail(error, QStringLiteral("配准预览失败，保留上一次成功预览")); }
    if (!alive) return false;
    m_busy = false;
    if (!accepted) return false;
    m_mapping = mapping; m_inverse = inverted; m_error.clear(); update(); emit MappingPreviewed(mapping); return bool(alive);
}
void CArtworkSourceView::BeginGesture()
{
    m_gestureBefore = m_mapping; m_gesture = true; m_keyboardDelta = {}; m_error.clear(); emit GestureStarted();
}
void CArtworkSourceView::ResetGesture()
{
    m_gesture = m_dragReady = m_dragging = m_keyboard = false; m_keyboardDelta = {}; m_gestureBefore = {}; update();
}
bool CArtworkSourceView::FinishGesture(QString* error)
{
    if (m_busy) return fail(error, QStringLiteral("配准映射正在发布"));
    if (!m_gesture) { m_dragReady = false; return true; }
    if (equal(m_mapping, m_gestureBefore)) {
        const QPointer<CArtworkSourceView> alive(this); ResetGesture(); emit GestureFinished(false); return bool(alive);
    }
    const QPointer<CArtworkSourceView> alive(this); const auto handler = m_commit; const auto mapping = m_mapping; bool accepted = true;
    m_busy = true;
    try { if (handler) accepted = handler(mapping, error); }
    catch (...) { accepted = fail(error, QStringLiteral("配准提交失败，可重试或取消当前拖动")); }
    if (!alive) return false;
    m_busy = false;
    if (!accepted) return false;
    ResetGesture(); m_error.clear(); emit MappingCommitted(mapping);
    if (alive) emit GestureFinished(false); return bool(alive);
}
bool CArtworkSourceView::CancelGesture(QString* error)
{
    if (m_busy) return fail(error, QStringLiteral("配准映射正在发布"));
    m_dragReady = m_dragging = m_keyboard = m_pan = m_space = false; unsetCursor();
    if (!m_gesture) return true;
    const QPointer<CArtworkSourceView> alive(this); const auto handler = m_cancel; const auto before = m_gestureBefore; bool accepted = true;
    if (!equal(m_mapping, before)) {
        m_busy = true;
        try { if (handler) accepted = handler(error); else if (m_preview) accepted = m_preview(before, error); }
        catch (...) { accepted = fail(error, QStringLiteral("未能恢复配准映射，请重试取消")); }
        if (!alive) return false;
        m_busy = false;
    }
    if (!accepted) return false;
    QTransform inverted;
    if (!PrepareMapping(before, inverted, error)) return false;
    m_mapping = before; m_inverse = inverted; ResetGesture(); m_error.clear(); emit MappingCanceled(before);
    if (alive) emit GestureFinished(true); return bool(alive);
}
bool CArtworkSourceView::Translate(QPointF delta, Qt::KeyboardModifiers modifiers, QString* error)
{
    if (modifiers.testFlag(Qt::ShiftModifier)) { if (std::abs(delta.x()) >= std::abs(delta.y())) delta.setY(0); else delta.setX(0); }
    if (delta.isNull()) return PreviewMapping(m_gestureBefore, error);
    const auto& m = m_gestureBefore;
    return PreviewMapping(QTransform(m.m11(), m.m12(), m.m13(), m.m21(), m.m22(), m.m23(),
        delta.x() == 0 ? m.dx() : m.dx() + delta.x(), delta.y() == 0 ? m.dy() : m.dy() + delta.y(), m.m33()), error);
}
void CArtworkSourceView::RejectEdit(const QString& error)
{
    m_error = error.isEmpty() ? QStringLiteral("配准操作未能完成") : error; const auto message = m_error; emit EditRejected(message);
}

void CArtworkSourceView::paintEvent(QPaintEvent*)
{
    QPainter painter(this); const auto token = Theme::tokens(palette().color(QPalette::Window).lightness() < 128);
    const QColor base(token.value("@surface@")), alternate(token.value("@surfaceHover@"));
    const int cell = std::max(2, fontMetrics().height() / 2); QPixmap tile(cell * 2, cell * 2); tile.fill(base);
    { QPainter pattern(&tile); pattern.fillRect(0, 0, cell, cell, alternate); pattern.fillRect(cell, cell, cell, cell, alternate); }
    painter.fillRect(rect(), QBrush(tile));
    const auto drawImage = [&](const QImage& image, QRect bounds, const QTransform& mapping, double opacity) {
        if (image.isNull() || opacity == 0) return;
        const auto origin = DocumentToViewport(mapping.map(QPointF(bounds.x(), bounds.y())));
        const QTransform matrix(mapping.m11() * m_scale, mapping.m12() * m_scale, mapping.m21() * m_scale, mapping.m22() * m_scale, origin.x(), origin.y());
        painter.save(); painter.setOpacity(opacity); painter.setTransform(matrix);
        painter.setRenderHint(QPainter::SmoothPixmapTransform,
            m_scale * std::hypot(mapping.m11(), mapping.m12()) < 1 || m_scale * std::hypot(mapping.m21(), mapping.m22()) < 1 || mapping.type() >= QTransform::TxRotate);
        painter.drawImage(QRectF(QPointF(), image.size()), image, QRectF(QPointF(), image.size())); painter.restore();
    };
    drawImage(m_source, m_sourceBounds, QTransform(), m_sourceOpacity);
    drawImage(m_reference, m_atlasBounds, m_mapping, m_referenceOpacity);
    const QRectF visible(ViewportToDocument(rect().topLeft()), ViewportToDocument(rect().bottomRight()));
    const auto line = [&](QPointF first, QPointF second, const QColor& color, Qt::PenStyle style, double width = 1) {
        if (!CUvGeometry::ClipSegment(first, second, visible)) return;
        QPen pen(color); pen.setStyle(style); pen.setCosmetic(true); pen.setWidthF(width); painter.setPen(pen);
        painter.drawLine(DocumentToViewport(first), DocumentToViewport(second));
    };
    const auto outline = [&](QRectF bounds, const QTransform& mapping, const QColor& color, Qt::PenStyle style, double width = 1) {
        const std::array<QPointF, 4> corners{bounds.topLeft(), bounds.topRight(), bounds.bottomRight(), bounds.bottomLeft()};
        for (int index = 0; index < 4; ++index) line(mapping.map(corners[index]), mapping.map(corners[(index + 1) % 4]), color, style, width);
    };
    outline(QRectF(QPointF(), m_documentCanvas), {}, QColor(token.value("@borderStrong@")), Qt::SolidLine);
    if (!m_source.isNull()) outline(rectangle(m_sourceBounds), {}, QColor(token.value("@textMuted@")), Qt::DashLine);
    const QColor accent(token.value("@accentInk@"));
    if (!m_reference.isNull()) {
        outline(rectangle(m_atlasBounds), m_mapping, base, Qt::SolidLine, std::max(2.0, fontMetrics().height() * .18));
        outline(rectangle(m_atlasBounds), m_mapping, accent, Qt::SolidLine);
        for (int triangle = 0; triangle < m_triangles.size(); triangle += 3) for (int edge = 0; edge < 3; ++edge)
            line(m_mapping.map(m_vertices[m_triangles[triangle + edge]]), m_mapping.map(m_vertices[m_triangles[triangle + (edge + 1) % 3]]), accent, Qt::SolidLine);
    }
    if (hasFocus()) { painter.setPen(accent); painter.setBrush(Qt::NoBrush); painter.drawRect(rect().adjusted(0, 0, -1, -1)); }
}
void CArtworkSourceView::showEvent(QShowEvent* event) { QWidget::showEvent(event); QueueFit(); }
void CArtworkSourceView::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    if (m_fitPending) { m_fitViewport = {}; QueueFit(); }
    else { m_offset += QPointF(event->size().width() - event->oldSize().width(), event->size().height() - event->oldSize().height()) / 2.0; update(); }
}
void CArtworkSourceView::mousePressEvent(QMouseEvent* event)
{
    const QPointer<CArtworkSourceView> alive(this); setFocus(Qt::MouseFocusReason); if (!alive) return;
    m_press = m_last = event->position();
    if (event->button() == Qt::MiddleButton || (event->button() == Qt::LeftButton && m_space)) {
        if (m_gesture) { event->accept(); return; }
        CancelQueuedFit(); m_pan = true; setCursor(Qt::ClosedHandCursor); event->accept(); return;
    }
    if (event->button() == Qt::LeftButton && m_editing && !m_gesture && !m_busy && CoordinatesAt(event->position()).onReference) {
        CancelQueuedFit(); m_pressDocument = ViewportToDocument(event->position()); m_dragReady = true; event->accept(); return;
    }
    QWidget::mousePressEvent(event);
}
void CArtworkSourceView::mouseMoveEvent(QMouseEvent* event)
{
    const QPointer<CArtworkSourceView> alive(this); const auto position = event->position();
    if (m_pan) { m_offset += position - m_last; m_last = position; update(); emit ViewChanged(); if (!alive) return; }
    else {
        m_last = position;
        if (m_dragReady && !m_dragging && (position - m_press).manhattanLength() >= QApplication::startDragDistance()) {
            m_dragging = true; BeginGesture(); if (!alive) return;
        }
        if (m_dragging) {
            QString error; const bool accepted = Translate(ViewportToDocument(position) - m_pressDocument, event->modifiers(), &error);
            if (!alive) return; if (!accepted) RejectEdit(error); if (!alive) return;
        }
    }
    const auto point = CoordinatesAt(position); emit PointerMoved(point.document, point.layerLocal, point.atlas, point.onReference);
}
void CArtworkSourceView::mouseReleaseEvent(QMouseEvent* event)
{
    if (m_pan && (event->button() == Qt::LeftButton || event->button() == Qt::MiddleButton)) { m_pan = false; unsetCursor(); event->accept(); return; }
    if (event->button() != Qt::LeftButton) { QWidget::mouseReleaseEvent(event); return; }
    const QPointer<CArtworkSourceView> alive(this); event->accept();
    if (m_dragging) {
        QString error; const bool accepted = Translate(ViewportToDocument(event->position()) - m_pressDocument, event->modifiers(), &error);
        if (!alive) return;
        m_dragging = m_dragReady = false;
        if (!accepted) { RejectEdit(error); return; }
        if (!FinishGesture(&error) && alive) RejectEdit(error);
    } else m_dragReady = false;
}
void CArtworkSourceView::wheelEvent(QWheelEvent* event)
{
    if (m_gesture || m_dragReady || m_pan || !event->angleDelta().y()) { event->accept(); return; }
    CancelQueuedFit(); const auto anchor = ViewportToDocument(event->position());
    m_scale = std::clamp(m_scale * std::pow(1.15, event->angleDelta().y() / 120.0), 1e-50, 64.0);
    m_offset = event->position() - anchor * m_scale; update(); event->accept(); emit ViewChanged();
}
void CArtworkSourceView::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Space) { m_space = true; event->accept(); return; }
    if (event->key() == Qt::Key_Escape && (m_gesture || m_dragReady)) {
        const QPointer<CArtworkSourceView> alive(this); QString error; event->accept(); if (!CancelGesture(&error) && alive) RejectEdit(error); return;
    }
    QPointF delta;
    if (event->key() == Qt::Key_Left) delta.setX(-1); else if (event->key() == Qt::Key_Right) delta.setX(1);
    else if (event->key() == Qt::Key_Up) delta.setY(-1); else if (event->key() == Qt::Key_Down) delta.setY(1);
    else { QWidget::keyPressEvent(event); return; }
    if (!m_editing || m_reference.isNull() || m_busy || m_dragReady || m_dragging || (m_gesture && !m_keyboard)) { event->accept(); return; }
    CancelQueuedFit(); const QPointer<CArtworkSourceView> alive(this);
    if (!m_keyboard) { m_keyboard = true; BeginGesture(); if (!alive) return; }
    m_keyboardDelta += delta * (event->modifiers().testFlag(Qt::ShiftModifier) ? 10 : 1); QString error; event->accept();
    if (!Translate(m_keyboardDelta, Qt::NoModifier, &error) && alive) RejectEdit(error);
}
void CArtworkSourceView::keyReleaseEvent(QKeyEvent* event)
{
    if (event->isAutoRepeat()) { event->accept(); return; }
    if (event->key() == Qt::Key_Space) { m_space = false; event->accept(); return; }
    if (m_keyboard && (event->key() == Qt::Key_Left || event->key() == Qt::Key_Right || event->key() == Qt::Key_Up || event->key() == Qt::Key_Down)) {
        m_keyboard = false; const QPointer<CArtworkSourceView> alive(this); QString error; event->accept();
        if (!FinishGesture(&error) && alive) RejectEdit(error); return;
    }
    QWidget::keyReleaseEvent(event);
}
void CArtworkSourceView::focusOutEvent(QFocusEvent* event)
{
    const QPointer<CArtworkSourceView> alive(this); QString error;
    if (!m_busy && !CancelGesture(&error) && alive) RejectEdit(error); if (alive) QWidget::focusOutEvent(event);
}
void CArtworkSourceView::hideEvent(QHideEvent* event)
{
    const QPointer<CArtworkSourceView> alive(this); QString error;
    if (!m_busy && !CancelGesture(&error) && alive) RejectEdit(error); if (alive) QWidget::hideEvent(event);
}
