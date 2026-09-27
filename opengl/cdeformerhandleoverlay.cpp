#include "cdeformerhandleoverlay.h"
#include "ui/theme.h"
#include "ccanvascoordinates.h"

#include <QEvent>
#include <QFontMetricsF>
#include <QPainter>
#include <QPainterPath>
#include <QStyle>
#include <algorithm>
#include <cmath>

namespace {
bool finite(const QPointF& point)
{
    return std::isfinite(point.x()) && std::isfinite(point.y());
}

bool validRectangle(const QRectF& rectangle)
{
    return finite(rectangle.topLeft()) && finite(rectangle.bottomRight()) &&
        std::isfinite(rectangle.width()) && std::isfinite(rectangle.height());
}

bool contains(const QRectF& rectangle, const QPointF& point)
{
    return point.x() >= rectangle.left() && point.x() <= rectangle.right() &&
        point.y() >= rectangle.top() && point.y() <= rectangle.bottom();
}
}

CDeformerHandleOverlay::CDeformerHandleOverlay(QWidget* parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("deformerHandleOverlay"));
    setAccessibleName(QStringLiteral("变形器控制手柄"));
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_NoSystemBackground);
    setAutoFillBackground(false);
    setFocusPolicy(Qt::NoFocus);
}

CDeformerHandleOverlay::Metrics CDeformerHandleOverlay::Measure() const
{
    const qreal unit = qMax(qreal(1), QFontMetricsF(font()).height());
    const qreal stroke = qMax(1, style()->pixelMetric(QStyle::PM_DefaultFrameWidth, nullptr, this));
    const qreal point = qMax(stroke * 2, unit / 4);
    const qreal hit = qMax(point + stroke * 3, unit * .65);
    return {point, point * 1.5, point * 1.25, hit, qMax(unit * 3.5, hit * 4), stroke};
}

bool CDeformerHandleOverlay::Ready() const
{
    return m_mode != None && size() == m_viewport;
}

bool CDeformerHandleOverlay::SetWarp(const QVector<float>& points, const QMatrix4x4& mvp, const QSize& viewport)
{
    if (points.isEmpty() || points.size() % 2 || points.size() / 2 > 1000000) return false;
    QVector<QPointF> projected;
    projected.reserve(points.size() / 2);
    for (qsizetype index = 0; index < points.size(); index += 2) {
        QPointF pixel;
        if (!CCanvasCoordinates::ModelToScreen(mvp, viewport, QPointF(points[index], points[index + 1]), pixel)) return false;
        projected.append(pixel);
    }
    m_mode = WarpPoint;
    m_viewport = viewport;
    m_points.swap(projected);
    m_origin = {}; m_direction = {};
    m_selected.erase(std::remove_if(m_selected.begin(), m_selected.end(),
        [this](int index) { return index >= m_points.size(); }), m_selected.end());
    resize(viewport);
    update();
    return true;
}

bool CDeformerHandleOverlay::SetRotation(const QPointF& origin, const QPointF& direction,
                                        const QMatrix4x4& mvp, const QSize& viewport)
{
    if (!finite(origin) || !finite(direction)) return false;
    QPointF center, end;
    if (!CCanvasCoordinates::ModelToScreen(mvp, viewport, origin, center) ||
        !CCanvasCoordinates::ModelToScreen(mvp, viewport, origin + direction, end)) return false;
    const QPointF delta = end - center;
    const double length = std::hypot(delta.x(), delta.y());
    if (!finite(delta) || !std::isfinite(length) || length <= 0) return false;
    const auto unit = delta / length;
    const auto angle = center + unit * Measure().arm;
    if (!finite(angle) || angle == center) return false;
    m_mode = RotationOrigin;
    m_viewport = viewport;
    m_points.clear(); m_selected.clear(); m_rectangle.reset();
    m_origin = center; m_direction = unit;
    resize(viewport);
    update();
    return true;
}

void CDeformerHandleOverlay::Clear()
{
    m_mode = None; m_viewport = {};
    m_points.clear(); m_selected.clear(); m_rectangle.reset();
    m_origin = {}; m_direction = {};
    update();
}

void CDeformerHandleOverlay::SetSelected(QVector<int> selected)
{
    selected.erase(std::remove_if(selected.begin(), selected.end(), [this](int index) {
        return m_mode != WarpPoint || index < 0 || index >= m_points.size();
    }), selected.end());
    std::sort(selected.begin(), selected.end());
    selected.erase(std::unique(selected.begin(), selected.end()), selected.end());
    if (selected == m_selected) return;
    m_selected.swap(selected);
    update();
}

QVector<int> CDeformerHandleOverlay::Selected() const { return m_selected; }
QVector<QPointF> CDeformerHandleOverlay::PointPixels() const { return m_points; }
QPointF CDeformerHandleOverlay::OriginPixel() const { return m_mode == RotationOrigin ? m_origin : QPointF(); }
QPointF CDeformerHandleOverlay::AnglePixel() const
{
    return m_mode == RotationOrigin ? m_origin + m_direction * Measure().arm : QPointF();
}

CDeformerHandleOverlay::Hit CDeformerHandleOverlay::HitTest(const QPointF& pixel) const
{
    if (!Ready() || !finite(pixel) || !contains(QRectF(QPointF(), m_viewport), pixel)) return {};
    const auto metrics = Measure();
    const auto distance = [&](const QPointF& point) { return std::hypot(pixel.x() - point.x(), pixel.y() - point.y()); };
    if (m_mode == RotationOrigin) {
        if (distance(m_origin) <= metrics.hit) return {RotationOrigin, -1};
        if (distance(AnglePixel()) <= metrics.hit) return {RotationAngle, -1};
        return {};
    }
    int nearest = -1;
    double closest = metrics.hit;
    for (int index = 0; index < m_points.size(); ++index) {
        const double length = distance(m_points[index]);
        if (length < closest || (nearest < 0 && length <= closest)) { nearest = index; closest = length; }
    }
    return nearest < 0 ? Hit{} : Hit{WarpPoint, nearest};
}

QVector<int> CDeformerHandleOverlay::PointsInRect(const QRectF& rectangle) const
{
    QVector<int> result;
    if (!Ready() || m_mode != WarpPoint || !validRectangle(rectangle)) return result;
    const auto normalized = rectangle.normalized();
    if (!validRectangle(normalized)) return result;
    for (int index = 0; index < m_points.size(); ++index)
        if (contains(normalized, m_points[index])) result.append(index);
    return result;
}

void CDeformerHandleOverlay::SetSelectionRect(std::optional<QRectF> rectangle)
{
    if (rectangle) {
        if (!validRectangle(*rectangle)) return;
        rectangle = rectangle->normalized();
        if (!validRectangle(*rectangle)) return;
    }
    if (m_rectangle == rectangle) return;
    m_rectangle = rectangle;
    update();
}

void CDeformerHandleOverlay::paintEvent(QPaintEvent*)
{
    if (!Ready()) return;
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const auto metrics = Measure();
    const qreal mainStroke = std::max(metrics.stroke, 2.0 / devicePixelRatioF());
    const auto group = isEnabled() ? QPalette::Active : QPalette::Disabled;
    const QColor base = palette().color(group, QPalette::Window);
    const QColor ink = palette().color(group, QPalette::WindowText);
    const QColor accent(Theme::tokens(base.lightness() < 128).value(QStringLiteral("@accentInk@")));
    const QColor marked = base.lightness() < 128 ? base : palette().color(group, QPalette::HighlightedText);
    const QRectF visible(QPointF(), m_viewport);
    const auto disk = [&](const QPointF& center, qreal radius, const QColor& fill, const QColor& edge) {
        if (!visible.adjusted(-radius - metrics.stroke * 2, -radius - metrics.stroke * 2,
                              radius + metrics.stroke * 2, radius + metrics.stroke * 2).contains(center)) return;
        painter.setPen(QPen(base, metrics.stroke * 3)); painter.setBrush(fill);
        painter.drawEllipse(center, radius, radius);
        painter.setPen(QPen(edge, metrics.stroke));
        painter.drawEllipse(center, radius, radius);
    };
    if (m_mode == WarpPoint) {
        for (int index = 0; index < m_points.size(); ++index)
            if (!std::binary_search(m_selected.cbegin(), m_selected.cend(), index)) disk(m_points[index], metrics.point, base, ink);
        for (int index : m_selected) disk(m_points[index], metrics.point * 1.2, accent, marked);
    } else {
        const auto angle = AnglePixel();
        const qreal extent = metrics.origin + metrics.stroke * 2;
        const auto bounds = QRectF(m_origin, angle).normalized().adjusted(-extent, -extent, extent, extent);
        if (bounds.intersects(visible)) {
            painter.setPen(QPen(base, mainStroke * 3, Qt::SolidLine, Qt::RoundCap));
            painter.drawLine(m_origin, angle);
            QColor connection = ink; connection.setAlphaF(.6);
            painter.setPen(QPen(connection, mainStroke, Qt::SolidLine, Qt::RoundCap));
            painter.drawLine(m_origin, angle);
        }
        disk(m_origin, metrics.origin, base, ink);
        disk(angle, metrics.angle, accent, marked);
        if (visible.contains(m_origin)) {
            const qreal arm = metrics.origin * .55;
            painter.setPen(QPen(ink, metrics.stroke, Qt::SolidLine, Qt::RoundCap));
            painter.drawLine(m_origin - QPointF(arm, 0), m_origin + QPointF(arm, 0));
            painter.drawLine(m_origin - QPointF(0, arm), m_origin + QPointF(0, arm));
        }
        if (visible.contains(angle)) {
            const qreal arm = metrics.angle * .5;
            QPainterPath arrow;
            arrow.moveTo(angle + QPointF(-arm, arm * .3));
            arrow.cubicTo(angle + QPointF(-arm, -arm), angle + QPointF(arm, -arm), angle + QPointF(arm, arm * .3));
            arrow.lineTo(angle + QPointF(arm * .25, -arm * .15));
            painter.setPen(QPen(marked, metrics.stroke, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            painter.setBrush(Qt::NoBrush); painter.drawPath(arrow);
        }
    }
    if (m_rectangle) {
        const auto clipped = m_rectangle->intersected(visible);
        if (clipped.isEmpty()) return;
        QColor fill = accent; fill.setAlphaF(.12);
        painter.setPen(QPen(accent, mainStroke, Qt::DashLine)); painter.setBrush(fill);
        painter.drawRect(clipped);
    }
}

void CDeformerHandleOverlay::changeEvent(QEvent* event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange ||
        event->type() == QEvent::PaletteChange || event->type() == QEvent::EnabledChange) update();
}
