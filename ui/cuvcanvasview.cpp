#include "cuvcanvasview.h"
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

CUvCanvasView::CUvCanvasView(QWidget* parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("uvCanvas"));
    setAccessibleName(QStringLiteral("UV 顶点编辑画布"));
    setFocusPolicy(Qt::StrongFocus); setMouseTracking(true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

void CUvCanvasView::SetSnapshot(const live2dstruct::CUvEditDraft::Snapshot& snapshot, const QMap<int, QImage>& images)
{
    const QPointer<CUvCanvasView> alive(this); CancelGesture(); if (!alive) return;
    if (!m_draft.Initialize(snapshot)) return;
    m_images = images; m_state = m_draft.InitialState();
    if (!m_state.isEmpty()) {
        if (m_mesh == m_state.first().id) FitAtlas(); else SetMesh(m_state.first().id);
    } else { CancelQueuedFit(); update(); }
}

void CUvCanvasView::SetState(const live2dstruct::CModelMaterialState& state)
{
    if (!m_draft.ValidateState(state)) return;
    const int index = MeshIndex(); const int oldSlot = index >= 0 ? m_state[index].textureIndex : -1;
    m_state = state;
    if (index >= 0 && oldSlot != m_state[index].textureIndex) FitAtlas();
    update();
}

int CUvCanvasView::MeshIndex() const { return m_draft.MeshIndex(m_mesh); }

void CUvCanvasView::SetMesh(const QString& mesh)
{
    if (mesh == m_mesh) return;
    const QPointer<CUvCanvasView> alive(this); CancelGesture(); if (!alive) return;
    m_mesh = mesh; m_selected.clear(); FitAtlas();
    emit SelectionChanged();
}

void CUvCanvasView::SetSelectedVertices(const QSet<int>& vertices)
{
    if (HasGesture()) return;
    const int index = MeshIndex(); QSet<int> accepted;
    if (index >= 0) for (int vertex : vertices) if (vertex >= 0 && vertex < m_state[index].uvs.size() / 2) accepted.insert(vertex);
    if (accepted == m_selected) return;
    m_selected.swap(accepted); update(); emit SelectionChanged();
}

void CUvCanvasView::SelectAll()
{
    const int index = MeshIndex(); QSet<int> vertices;
    if (index >= 0) for (int vertex = 0; vertex < m_state[index].uvs.size() / 2; ++vertex) vertices.insert(vertex);
    SetSelectedVertices(vertices);
}

QSize CUvCanvasView::minimumSizeHint() const
{
    return {fontMetrics().averageCharWidth() * 24, fontMetrics().height() * 12};
}

QPointF CUvCanvasView::PixelToViewport(QPointF point) const { return point * m_scale + m_offset; }
QPointF CUvCanvasView::ViewportToPixel(QPointF point) const { return (point - m_offset) / m_scale; }

QPointF CUvCanvasView::Point(int mesh, int vertex) const
{
    const auto& material = m_state[mesh]; const auto size = m_draft.Source().atlasSizes.value(material.textureIndex);
    return {double(material.uvs[vertex * 2]) * size.width(), double(material.uvs[vertex * 2 + 1]) * size.height()};
}

void CUvCanvasView::Fit(QRectF bounds)
{
    const double margin = fontMetrics().height();
    const double w = std::max(1.0, bounds.width()), h = std::max(1.0, bounds.height());
    m_scale = std::min(std::max(1.0, width() - margin * 2) / w, std::max(1.0, height() - margin * 2) / h);
    m_scale = std::clamp(m_scale, 1e-50, 64.0);
    m_offset = QPointF(width() / 2.0, height() / 2.0) - bounds.center() * m_scale;
    m_fitPending = false; update();
}

void CUvCanvasView::RequestFit(QRectF bounds)
{
    m_fitBounds = bounds; m_fitPending = true; m_fitViewport = {}; QueueFit();
}

void CUvCanvasView::QueueFit()
{
    if (!m_fitPending || m_fitQueued || !isVisible()) return;
    m_fitQueued = true;
    QTimer::singleShot(0, this, [this] {
        m_fitQueued = false;
        if (!m_fitPending || !isVisible()) return;
        const int margin = fontMetrics().height();
        if (width() <= margin * 2 || height() <= margin * 2) return;
        if (size() != m_fitViewport) { m_fitViewport = size(); QueueFit(); return; }
        Fit(m_fitBounds);
    });
}

void CUvCanvasView::CancelQueuedFit()
{
    m_fitPending = false; m_fitViewport = {};
}

void CUvCanvasView::FitAtlas()
{
    const int index = MeshIndex();
    if (index >= 0) RequestFit(QRectF(QPointF(), m_draft.Source().atlasSizes.value(m_state[index].textureIndex)));
}

void CUvCanvasView::FitSelection()
{
    QRectF bounds;
    if (m_draft.SelectionBounds(m_state, {m_mesh, m_selected}, bounds)) RequestFit(bounds);
}

void CUvCanvasView::paintEvent(QPaintEvent*)
{
    QPainter painter(this); painter.fillRect(rect(), palette().brush(QPalette::Window));
    const int index = MeshIndex(); if (index < 0) return;
    const int slot = m_state[index].textureIndex; const auto size = m_draft.Source().atlasSizes.value(slot);
    const QRectF atlas(PixelToViewport({0, 0}), PixelToViewport({double(size.width()), double(size.height())}));
    painter.fillRect(atlas.intersected(rect()), palette().brush(QPalette::Base));
    const auto image = m_images.constFind(slot);
    if (image != m_images.cend() && !image->isNull()) {
        painter.setRenderHint(QPainter::SmoothPixmapTransform, m_scale < 1);
        const auto target = atlas.intersected(rect());
        if (!target.isEmpty()) painter.drawImage(target, *image, QRectF(ViewportToPixel(target.topLeft()), ViewportToPixel(target.bottomRight())));
    }
    QPen boundary(palette().color(QPalette::Mid)); boundary.setCosmetic(true); painter.setPen(boundary); painter.setBrush(Qt::NoBrush); painter.drawRect(atlas);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF visible(ViewportToPixel(rect().topLeft()), ViewportToPixel(rect().bottomRight()));
    const auto tokens = Theme::tokens(palette().color(QPalette::Window).lightness() < 128);
    const QColor accent(tokens.value(QStringLiteral("@accentFill@")));
    const QColor accentText(tokens.value(QStringLiteral("@accentText@")));
    for (int mesh = 0; mesh < m_state.size(); ++mesh) {
        if (m_state[mesh].textureIndex != slot) continue;
        auto color = mesh == index ? accent : palette().color(QPalette::Text); color.setAlpha(mesh == index ? 255 : 80);
        QPen pen(color); pen.setCosmetic(true); painter.setPen(pen);
        const auto& triangles = m_draft.Source().meshes[mesh].triangles;
        for (int triangle = 0; triangle < triangles.size(); triangle += 3) for (int edge = 0; edge < 3; ++edge) {
            auto a = Point(mesh, triangles[triangle + edge]), b = Point(mesh, triangles[triangle + (edge + 1) % 3]);
            if (CUvGeometry::ClipSegment(a, b, visible)) {
                if (mesh == index) {
                    QPen halo(palette().color(QPalette::Window)); halo.setCosmetic(true); halo.setWidthF(std::max(2.0, fontMetrics().height() * .15));
                    painter.setPen(halo); painter.drawLine(PixelToViewport(a), PixelToViewport(b)); painter.setPen(pen);
                }
                painter.drawLine(PixelToViewport(a), PixelToViewport(b));
            }
        }
    }
    const double radius = std::max(2.0, fontMetrics().height() * .22);
    for (int vertex = 0; vertex < m_state[index].uvs.size() / 2; ++vertex) {
        const auto point = PixelToViewport(Point(index, vertex));
        if (!QRectF(rect()).adjusted(-radius, -radius, radius, radius).contains(point)) continue;
        const bool selected = m_selected.contains(vertex);
        painter.setPen(selected ? accentText : palette().color(QPalette::Text));
        painter.setBrush(selected ? accent : palette().color(QPalette::Base));
        painter.drawEllipse(point, radius, radius);
    }
    if (m_box) {
        auto fill = palette().color(QPalette::Highlight); fill.setAlpha(35);
        painter.setBrush(fill); QPen pen(palette().color(QPalette::Highlight)); pen.setStyle(Qt::DashLine); painter.setPen(pen);
        painter.drawRect(QRectF(m_press, m_last).normalized());
    }
    if (hasFocus()) { QPen pen(palette().color(QPalette::Highlight)); painter.setPen(pen); painter.setBrush(Qt::NoBrush); painter.drawRect(rect().adjusted(0, 0, -1, -1)); }
}

void CUvCanvasView::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    if (m_fitPending) { m_fitViewport = {}; QueueFit(); }
    else m_offset += QPointF(event->size().width() - event->oldSize().width(), event->size().height() - event->oldSize().height()) / 2.0;
}

void CUvCanvasView::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event); QueueFit();
}

int CUvCanvasView::HitVertex(QPointF position) const
{
    const int index = MeshIndex(); if (index < 0) return -1;
    double nearest = std::pow(std::max(5.0, fontMetrics().height() * .45), 2); int hit = -1;
    for (int vertex = 0; vertex < m_state[index].uvs.size() / 2; ++vertex) {
        const auto delta = PixelToViewport(Point(index, vertex)) - position;
        const double distance = delta.x() * delta.x() + delta.y() * delta.y();
        if (distance < nearest) { hit = vertex; nearest = distance; }
    }
    return hit;
}

int CUvCanvasView::HitTriangle(QPointF position) const
{
    const int index = MeshIndex(); if (index < 0) return -1;
    const auto point = ViewportToPixel(position); const auto& triangles = m_draft.Source().meshes[index].triangles;
    for (int triangle = 0; triangle < triangles.size(); triangle += 3)
        if (CUvGeometry::ContainsTriangle(point, Point(index, triangles[triangle]), Point(index, triangles[triangle + 1]), Point(index, triangles[triangle + 2]))) return triangles[triangle];
    return -1;
}

void CUvCanvasView::mousePressEvent(QMouseEvent* event)
{
    setFocus(Qt::MouseFocusReason); m_press = m_last = event->position();
    if (event->button() == Qt::MiddleButton || (event->button() == Qt::LeftButton && m_space)) { CancelQueuedFit(); m_pan = true; setCursor(Qt::ClosedHandCursor); event->accept(); return; }
    if (event->button() != Qt::LeftButton || HasGesture()) { QWidget::mousePressEvent(event); return; }
    const int index = MeshIndex(); if (index < 0 || !m_draft.Source().meshes[index].editable) return;
    CancelQueuedFit();
    const int vertex = HitVertex(event->position()); const QPointer<CUvCanvasView> alive(this);
    if (vertex >= 0) {
        auto selected = m_selected;
        if (event->modifiers().testFlag(Qt::AltModifier)) selected.remove(vertex);
        else if (event->modifiers().testFlag(Qt::ShiftModifier)) { if (selected.contains(vertex)) selected.remove(vertex); else selected.insert(vertex); }
        else if (!selected.contains(vertex)) selected = {vertex};
        SetSelectedVertices(selected); if (!alive) return;
        m_dragReady = m_selected.contains(vertex) && !event->modifiers().testFlag(Qt::AltModifier);
    } else {
        const int island = HitTriangle(event->position());
        const bool selectedIsland = island >= 0 && m_draft.ConnectedIsland(m_mesh, island).subtract(m_selected).isEmpty();
        if (selectedIsland && !(event->modifiers() & (Qt::ShiftModifier | Qt::AltModifier))) m_dragReady = true;
        else {
            m_box = true; m_boxBefore = m_selected; m_boxModifiers = event->modifiers();
            if (!(m_boxModifiers & (Qt::ShiftModifier | Qt::AltModifier))) SetSelectedVertices({});
        }
    }
    event->accept();
}

void CUvCanvasView::EmitTranslation(QPointF delta, Qt::KeyboardModifiers modifiers)
{
    int axis = m_axis;
    if (!axis && modifiers.testFlag(Qt::ShiftModifier)) axis = std::abs(delta.x()) >= std::abs(delta.y()) ? 1 : 2;
    emit TranslationPreview(delta, axis, m_snap);
}

void CUvCanvasView::mouseMoveEvent(QMouseEvent* event)
{
    const QPointer<CUvCanvasView> alive(this);
    if (m_pan) { m_offset += event->position() - m_last; m_last = event->position(); m_fitPending = false; update(); return; }
    m_last = event->position();
    if (m_dragReady && !m_dragging && (m_last - m_press).manhattanLength() >= QApplication::startDragDistance()) {
        m_dragging = true; emit GestureStarted(); if (!alive) return;
    }
    if (m_dragging) { EmitTranslation((m_last - m_press) / m_scale, event->modifiers()); if (!alive) return; }
    if (m_box) update();
    emit PointerMoved(ViewportToPixel(event->position()));
}

void CUvCanvasView::mouseReleaseEvent(QMouseEvent* event)
{
    if (m_pan) { m_pan = false; unsetCursor(); return; }
    if (event->button() != Qt::LeftButton) return;
    const QPointer<CUvCanvasView> alive(this);
    if (m_dragging) {
        EmitTranslation((event->position() - m_press) / m_scale, event->modifiers()); if (!alive) return;
        m_dragging = m_dragReady = false; m_axis = 0; emit GestureFinished(false); if (!alive) return;
    } else if (m_box) {
        const auto bounds = QRectF(m_press, event->position()).normalized(); auto selected = m_boxBefore;
        if (!(m_boxModifiers & (Qt::ShiftModifier | Qt::AltModifier))) selected.clear();
        const int index = MeshIndex();
        if (index >= 0) for (int vertex = 0; vertex < m_state[index].uvs.size() / 2; ++vertex)
            if (bounds.contains(PixelToViewport(Point(index, vertex)))) {
                if (m_boxModifiers.testFlag(Qt::AltModifier)) selected.remove(vertex); else selected.insert(vertex);
            }
        m_box = false; SetSelectedVertices(selected); if (!alive) return;
    }
    m_dragReady = false; update(); event->accept();
}

void CUvCanvasView::SelectIsland(int vertex, Qt::KeyboardModifiers modifiers)
{
    if (vertex < 0) return;
    const auto island = m_draft.ConnectedIsland(m_mesh, vertex); auto selected = m_selected;
    if (modifiers.testFlag(Qt::AltModifier)) selected.subtract(island);
    else if (modifiers.testFlag(Qt::ShiftModifier)) selected.unite(island);
    else selected = island;
    SetSelectedVertices(selected);
}

void CUvCanvasView::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton || m_space) return;
    m_box = m_dragReady = false;
    const int vertex = HitVertex(event->position());
    SelectIsland(vertex >= 0 ? vertex : HitTriangle(event->position()), event->modifiers());
    event->accept();
}

void CUvCanvasView::wheelEvent(QWheelEvent* event)
{
    if (HasGesture() || m_box || m_pan || !event->angleDelta().y()) { event->accept(); return; }
    CancelQueuedFit();
    const auto at = ViewportToPixel(event->position());
    m_scale = std::clamp(m_scale * std::pow(1.15, event->angleDelta().y() / 120.0), 1e-50, 64.0);
    m_offset = event->position() - at * m_scale; m_fitPending = false; update(); event->accept();
}

void CUvCanvasView::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Space) { m_space = true; event->accept(); return; }
    if (event->key() == Qt::Key_Escape && (HasGesture() || m_box)) { CancelGesture(); event->accept(); return; }
    if (event->matches(QKeySequence::SelectAll)) { SelectAll(); event->accept(); return; }
    if (event->key() == Qt::Key_X || event->key() == Qt::Key_Y) {
        m_axis = event->key() == Qt::Key_X ? 1 : 2;
        if (m_dragging) EmitTranslation((m_last - m_press) / m_scale, event->modifiers());
        event->accept(); return;
    }
    QPointF step;
    if (event->key() == Qt::Key_Left) step.setX(-1);
    else if (event->key() == Qt::Key_Right) step.setX(1);
    else if (event->key() == Qt::Key_Up) step.setY(-1);
    else if (event->key() == Qt::Key_Down) step.setY(1);
    else { QWidget::keyPressEvent(event); return; }
    if (m_selected.isEmpty() || m_dragging) return;
    CancelQueuedFit();
    const QPointer<CUvCanvasView> alive(this);
    if (!m_keyboardGesture) { m_keyboardGesture = true; m_keyboardDelta = {}; emit GestureStarted(); if (!alive) return; }
    m_keyboardDelta += step * (event->modifiers().testFlag(Qt::ShiftModifier) ? 10 : 1);
    EmitTranslation(m_keyboardDelta, Qt::NoModifier); event->accept();
}

void CUvCanvasView::keyReleaseEvent(QKeyEvent* event)
{
    if (event->isAutoRepeat()) { event->accept(); return; }
    if (event->key() == Qt::Key_Space) { m_space = false; event->accept(); return; }
    if (m_keyboardGesture && (event->key() == Qt::Key_Left || event->key() == Qt::Key_Right || event->key() == Qt::Key_Up || event->key() == Qt::Key_Down)) {
        m_keyboardGesture = false; m_axis = 0; emit GestureFinished(false); event->accept(); return;
    }
    QWidget::keyReleaseEvent(event);
}

void CUvCanvasView::CancelGesture()
{
    const bool active = HasGesture(); m_dragging = m_keyboardGesture = m_dragReady = m_box = m_pan = false;
    m_space = false; m_axis = 0; unsetCursor(); update();
    if (active) emit GestureFinished(true);
}

void CUvCanvasView::focusOutEvent(QFocusEvent* event) { const QPointer<CUvCanvasView> alive(this); CancelGesture(); if (alive) QWidget::focusOutEvent(event); }
void CUvCanvasView::hideEvent(QHideEvent* event) { const QPointer<CUvCanvasView> alive(this); CancelGesture(); if (alive) QWidget::hideEvent(event); }
