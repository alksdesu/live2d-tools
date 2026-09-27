#include "catlascanvasview.h"
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
#include <cmath>
#include <limits>

namespace {
bool fail(QString* error, const QString& text) { if (error) *error = text; return false; }
}

CAtlasCanvasView::CAtlasCanvasView(QWidget* parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("atlasCanvas"));
    setAccessibleName(QStringLiteral("图集素材编辑画布"));
    setFocusPolicy(Qt::StrongFocus); setMouseTracking(true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

bool CAtlasCanvasView::PrepareContent(const QMap<int, QImage>& images, const QVector<Patch>& patches, Content& result, QString* error) const
{
    QSet<QString> ids;
    for (auto image = images.cbegin(); image != images.cend(); ++image)
        if (image.key() < 0 || image->isNull()) return fail(error, QStringLiteral("图集画布缺少有效图像"));
    for (const auto& patch : patches) {
        if (patch.id.isEmpty() || ids.contains(patch.id) || (!patch.points.isEmpty() && !images.contains(patch.slot)))
            return fail(error, QStringLiteral("图集画布对象目录无效"));
        ids.insert(patch.id);
        for (const auto& point : patch.points)
            if (!std::isfinite(point.x()) || !std::isfinite(point.y())) return fail(error, QStringLiteral("图集素材坐标无效"));
        if (patch.triangles.size() % 3) return fail(error, QStringLiteral("图集素材连线无效"));
        for (auto vertex : patch.triangles)
            if (vertex >= patch.points.size()) return fail(error, QStringLiteral("图集素材连线越界"));
    }
    Content candidate{images, patches, m_selected}; candidate.selection.intersect(ids); result = std::move(candidate); return true;
}

void CAtlasCanvasView::AdoptContent(Content&& content)
{
    const bool refit = !m_images.contains(m_slot) || !content.images.contains(m_slot) || m_images.value(m_slot).size() != content.images.value(m_slot).size();
    CancelGesture(); m_images.swap(content.images); m_patches.swap(content.patches); m_selected.swap(content.selection);
    if (!m_images.contains(m_slot)) m_slot = m_images.isEmpty() ? -1 : m_images.firstKey();
    if (refit) FitAtlas();
    update();
}

bool CAtlasCanvasView::SetContent(const QMap<int, QImage>& images, const QVector<Patch>& patches, QString* error)
{
    Content content; if (!PrepareContent(images, patches, content, error)) return false;
    AdoptContent(std::move(content)); return true;
}

void CAtlasCanvasView::SetSlot(int slot)
{
    if (slot == m_slot || !m_images.contains(slot)) return;
    CancelGesture(); m_slot = slot; FitAtlas(); update();
}

void CAtlasCanvasView::SetSelectedMeshes(const QSet<QString>& ids)
{
    if (HasGesture()) return;
    QSet<QString> accepted;
    for (const auto& patch : m_patches) if (ids.contains(patch.id)) accepted.insert(patch.id);
    if (accepted == m_selected) return;
    m_selected.swap(accepted); update(); emit SelectionChanged();
}

QRectF CAtlasCanvasView::Bounds(const Patch& patch) const
{
    if (patch.points.isEmpty()) return {};
    double left = patch.points.first().x(), right = left, top = patch.points.first().y(), bottom = top;
    for (const auto& point : patch.points) {
        left = std::min(left, point.x()); right = std::max(right, point.x());
        top = std::min(top, point.y()); bottom = std::max(bottom, point.y());
    }
    return QRectF(QPointF(std::floor(left) - m_gutter, std::floor(top) - m_gutter),
                  QPointF(std::ceil(right) + m_gutter, std::ceil(bottom) + m_gutter));
}

bool CAtlasCanvasView::SelectionBounds(QRectF& bounds) const
{
    bool found = false; QRectF result;
    for (const auto& patch : m_patches) if (patch.slot == m_slot && m_selected.contains(patch.id) && !patch.points.isEmpty()) {
        const auto box = Bounds(patch); result = found ? result.united(box) : box; found = true;
    }
    if (found) bounds = result;
    return found;
}

void CAtlasCanvasView::SetEditingEnabled(bool enabled)
{
    if (!enabled) CancelGesture(); m_enabled = enabled; update();
}
void CAtlasCanvasView::SetGutter(int pixels) { m_gutter = std::max(0, pixels); update(); }
void CAtlasCanvasView::SelectAll()
{
    QSet<QString> ids; for (const auto& patch : m_patches) if (patch.slot == m_slot) ids.insert(patch.id);
    SetSelectedMeshes(ids);
}
bool CAtlasCanvasView::HasGesture() const { return m_drag || m_box || m_keyboard; }
void CAtlasCanvasView::CancelGesture()
{
    m_drag = m_box = m_keyboard = m_pan = false; m_ghost = {}; m_axis = 0; m_space = false;
    unsetCursor(); update();
}
QPointF CAtlasCanvasView::PixelToViewport(QPointF point) const { return point * m_scale + m_offset; }
QPointF CAtlasCanvasView::ViewportToPixel(QPointF point) const { return (point - m_offset) / m_scale; }
QSize CAtlasCanvasView::minimumSizeHint() const { return {fontMetrics().averageCharWidth() * 28, fontMetrics().height() * 13}; }

void CAtlasCanvasView::RequestFit(QRectF bounds)
{
    m_fitBounds = bounds; m_fitPending = true; m_fitViewport = {}; QueueFit();
}
void CAtlasCanvasView::QueueFit()
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
        m_fitPending = false; update();
    });
}
void CAtlasCanvasView::CancelQueuedFit() { m_fitPending = false; m_fitViewport = {}; }
void CAtlasCanvasView::FitAtlas() { if (m_images.contains(m_slot)) RequestFit(QRectF(QPointF(), m_images.value(m_slot).size())); }
void CAtlasCanvasView::FitSelection() { QRectF bounds; if (SelectionBounds(bounds)) RequestFit(bounds); }

void CAtlasCanvasView::paintEvent(QPaintEvent*)
{
    QPainter painter(this); painter.fillRect(rect(), palette().brush(QPalette::Window));
    if (!m_images.contains(m_slot)) return;
    const auto& image = *m_images.constFind(m_slot);
    const QRectF atlas(PixelToViewport({0, 0}), PixelToViewport({double(image.width()), double(image.height())}));
    const auto target = atlas.intersected(rect());
    painter.fillRect(target, palette().brush(QPalette::Base));
    painter.setRenderHint(QPainter::SmoothPixmapTransform, m_scale < 1);
    if (!target.isEmpty()) painter.drawImage(target, image, QRectF(ViewportToPixel(target.topLeft()), ViewportToPixel(target.bottomRight())));
    QPen boundary(palette().color(QPalette::Mid)); boundary.setCosmetic(true); painter.setPen(boundary);
    painter.setBrush(Qt::NoBrush); painter.drawRect(atlas);
    const QRectF visible(ViewportToPixel(rect().topLeft()), ViewportToPixel(rect().bottomRight()));
    const auto token = Theme::tokens(palette().color(QPalette::Window).lightness() < 128);
    const QColor accent(token.value(QStringLiteral("@accentInk@")));
    const auto drawBox = [&](QRectF box, QColor color, Qt::PenStyle style, double width = 1) {
        QPen pen(color); pen.setCosmetic(true); pen.setStyle(style); pen.setWidthF(width); painter.setPen(pen); painter.setBrush(Qt::NoBrush);
        const QPointF corners[] = {box.topLeft(), box.topRight(), box.bottomRight(), box.bottomLeft()};
        for (int edge = 0; edge < 4; ++edge) {
            auto first = corners[edge], second = corners[(edge + 1) % 4];
            if (CUvGeometry::ClipSegment(first, second, visible)) painter.drawLine(PixelToViewport(first), PixelToViewport(second));
        }
    };
    for (const auto& patch : m_patches) {
        if (patch.slot != m_slot || patch.points.isEmpty()) continue;
        const auto bounds = Bounds(patch); const bool selected = m_selected.contains(patch.id);
        auto color = selected ? accent : palette().color(QPalette::Text); if (!selected) color.setAlpha(80);
        if (selected) drawBox(bounds, palette().color(QPalette::Base), Qt::SolidLine, std::max(2.0, fontMetrics().height() * .18));
        drawBox(bounds, color, patch.editable ? Qt::SolidLine : Qt::DashLine);
        if (selected && (m_drag || m_keyboard)) drawBox(bounds.translated(m_ghost), accent, Qt::DashLine);
    }
    if (m_box) {
        auto fill = palette().color(QPalette::Highlight); fill.setAlpha(35); painter.setBrush(fill);
        QPen pen(accent); pen.setStyle(Qt::DashLine); painter.setPen(pen); painter.drawRect(QRectF(m_press, m_last).normalized());
    }
    if (hasFocus()) { painter.setPen(accent); painter.setBrush(Qt::NoBrush); painter.drawRect(rect().adjusted(0, 0, -1, -1)); }
}

void CAtlasCanvasView::showEvent(QShowEvent* event) { QWidget::showEvent(event); QueueFit(); }
void CAtlasCanvasView::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    if (m_fitPending) { m_fitViewport = {}; QueueFit(); }
    else m_offset += QPointF(event->size().width() - event->oldSize().width(), event->size().height() - event->oldSize().height()) / 2.0;
}
int CAtlasCanvasView::Hit(QPointF position) const
{
    const auto point = ViewportToPixel(position);
    for (int index = m_patches.size() - 1; index >= 0; --index)
        if (m_patches[index].slot == m_slot && !m_patches[index].points.isEmpty() && Bounds(m_patches[index]).contains(point)) return index;
    return -1;
}
void CAtlasCanvasView::mousePressEvent(QMouseEvent* event)
{
    setFocus(Qt::MouseFocusReason); m_press = m_last = event->position();
    if (event->button() == Qt::MiddleButton || (event->button() == Qt::LeftButton && m_space)) {
        CancelQueuedFit(); m_pan = true; setCursor(Qt::ClosedHandCursor); event->accept(); return;
    }
    if (event->button() != Qt::LeftButton || !m_enabled || m_keyboard) { QWidget::mousePressEvent(event); return; }
    const int hit = Hit(event->position());
    if (hit >= 0) {
        auto selection = m_selected; const auto id = m_patches[hit].id;
        if (event->modifiers().testFlag(Qt::AltModifier)) selection.remove(id);
        else if (event->modifiers().testFlag(Qt::ShiftModifier)) selection.insert(id);
        else if (!selection.contains(id)) selection = {id};
        const QPointer<CAtlasCanvasView> alive(this); SetSelectedMeshes(selection); if (!alive) return;
        if (m_selected.contains(id)) { m_drag = true; m_ghost = {}; }
    } else {
        m_box = true; m_boxBefore = m_selected; m_boxModifiers = event->modifiers();
    }
    event->accept(); update();
}
void CAtlasCanvasView::SetGhost(QPointF delta, Qt::KeyboardModifiers modifiers)
{
    if (m_axis == 1) delta.setY(0); else if (m_axis == 2) delta.setX(0);
    else if (modifiers.testFlag(Qt::ShiftModifier)) { if (std::abs(delta.x()) >= std::abs(delta.y())) delta.setY(0); else delta.setX(0); }
    const double bound = std::numeric_limits<int>::max();
    m_ghost = {std::round(std::clamp(delta.x(), -bound, bound)), std::round(std::clamp(delta.y(), -bound, bound))}; update();
}
void CAtlasCanvasView::mouseMoveEvent(QMouseEvent* event)
{
    const auto position = event->position();
    if (m_pan) { m_offset += position - m_last; m_last = position; update(); }
    else if (m_drag) { m_last = position; SetGhost((position - m_press) / m_scale, event->modifiers()); }
    else if (m_box) { m_last = position; update(); }
    emit PointerMoved(ViewportToPixel(position));
}
void CAtlasCanvasView::FinishTranslation()
{
    const auto delta = m_ghost.toPoint(); QStringList ids;
    for (const auto& patch : m_patches) if (patch.slot == m_slot && m_selected.contains(patch.id)) ids.append(patch.id);
    m_drag = m_keyboard = false; m_ghost = {}; m_axis = 0; update();
    if (!delta.isNull() && !ids.isEmpty()) emit TranslationRequested(ids, delta);
}
void CAtlasCanvasView::mouseReleaseEvent(QMouseEvent* event)
{
    if (m_pan && (event->button() == Qt::MiddleButton || event->button() == Qt::LeftButton)) {
        m_pan = false; unsetCursor(); event->accept(); return;
    }
    if (event->button() != Qt::LeftButton) { QWidget::mouseReleaseEvent(event); return; }
    if (m_drag) { event->accept(); FinishTranslation(); return; }
    if (m_box) {
        m_last = event->position();
        const QRectF box(ViewportToPixel(QRectF(m_press, m_last).normalized().topLeft()), ViewportToPixel(QRectF(m_press, m_last).normalized().bottomRight()));
        auto selection = m_boxModifiers.testFlag(Qt::ShiftModifier) || m_boxModifiers.testFlag(Qt::AltModifier) ? m_boxBefore : QSet<QString>();
        for (const auto& patch : m_patches) if (patch.slot == m_slot && box.intersects(Bounds(patch))) {
            if (m_boxModifiers.testFlag(Qt::AltModifier)) selection.remove(patch.id); else selection.insert(patch.id);
        }
        m_box = false; event->accept(); SetSelectedMeshes(selection); return;
    }
    QWidget::mouseReleaseEvent(event);
}
void CAtlasCanvasView::wheelEvent(QWheelEvent* event)
{
    if (!event->angleDelta().y()) { QWidget::wheelEvent(event); return; }
    CancelQueuedFit(); const auto anchor = ViewportToPixel(event->position());
    m_scale = std::clamp(m_scale * std::pow(1.2, event->angleDelta().y() / 120.0), 1e-50, 64.0);
    m_offset = event->position() - anchor * m_scale; update(); event->accept();
}
void CAtlasCanvasView::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Escape && HasGesture()) { CancelGesture(); event->accept(); return; }
    if (event->key() == Qt::Key_Space) { m_space = true; event->accept(); return; }
    if (event->matches(QKeySequence::SelectAll) && m_enabled && !HasGesture()) { event->accept(); SelectAll(); return; }
    if (m_drag && (event->key() == Qt::Key_X || event->key() == Qt::Key_Y)) {
        m_axis = event->key() == Qt::Key_X ? 1 : 2; SetGhost((m_last - m_press) / m_scale, event->modifiers()); event->accept(); return;
    }
    QPoint delta;
    if (event->key() == Qt::Key_Left) delta.setX(-1); else if (event->key() == Qt::Key_Right) delta.setX(1);
    else if (event->key() == Qt::Key_Up) delta.setY(-1); else if (event->key() == Qt::Key_Down) delta.setY(1);
    if (!delta.isNull() && m_enabled && !m_selected.isEmpty() && !m_drag && !m_box) {
        m_keyboard = true; if (event->modifiers().testFlag(Qt::ShiftModifier)) delta *= 10;
        SetGhost(m_ghost + delta, Qt::NoModifier); event->accept(); return;
    }
    QWidget::keyPressEvent(event);
}
void CAtlasCanvasView::keyReleaseEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Space) { m_space = false; event->accept(); return; }
    if (m_keyboard && !event->isAutoRepeat() && (event->key() == Qt::Key_Left || event->key() == Qt::Key_Right || event->key() == Qt::Key_Up || event->key() == Qt::Key_Down)) {
        event->accept(); FinishTranslation(); return;
    }
    if (m_drag && (event->key() == Qt::Key_X || event->key() == Qt::Key_Y)) { m_axis = 0; event->accept(); return; }
    QWidget::keyReleaseEvent(event);
}
void CAtlasCanvasView::focusOutEvent(QFocusEvent* event) { CancelGesture(); QWidget::focusOutEvent(event); }
void CAtlasCanvasView::hideEvent(QHideEvent* event) { CancelGesture(); QWidget::hideEvent(event); }
