#include "cmotioncurveview.h"

#include <QPainter>
#include <QPolygonF>
#include <QMouseEvent>
#include <QtMath>
#include <algorithm>
#include <limits>

CMotionCurveView::CMotionCurveView(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("motionCurveView"));
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setMinimumHeight(150);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

void CMotionCurveView::SetTimeline(const CMotionTimeline* timeline)
{
    m_timeline = timeline;
    update();
}

void CMotionCurveView::SetTime(float time)
{
    m_time = qMax(0.0f, time);
    update();
}

void CMotionCurveView::SetSelectedPoint(const CMotionTrack& track, int pointIndex)
{
    m_selectedTrack = track;
    m_selectedPointIndex = pointIndex;
    update();
}

void CMotionCurveView::SetTargetFilter(int target)
{
    if (m_targetFilter == target) return;
    CancelDrag();
    m_targetFilter = target;
    update();
}

void CMotionCurveView::CancelDrag()
{
    const bool dragging = m_dragTrack.has_value();
    m_dragTrack.reset();
    m_dragPointIndex = -1;
    if (dragging) emit EditFinished();
}

QRectF CMotionCurveView::plotRect() const
{
    return rect().adjusted(34, 16, -14, -22);
}

bool CMotionCurveView::valueRange(double* minimum, double* maximum) const
{
    if (!minimum || !maximum || !m_timeline || m_timeline->Tracks().isEmpty()) return false;
    *minimum = std::numeric_limits<float>::max();
    *maximum = std::numeric_limits<float>::lowest();
    for (auto it = m_timeline->Tracks().cbegin(); it != m_timeline->Tracks().cend(); ++it) {
        if (!TrackVisible(it.key())) continue;
        for (const QPointF& point : it.value()) {
            *minimum = qMin(*minimum, point.y());
            *maximum = qMax(*maximum, point.y());
        }
    }
    if (!qIsFinite(*minimum) || !qIsFinite(*maximum)) return false;
    const double span = *maximum - *minimum;
    const double padding = qMax(0.05, (span > 0.0 ? span : qAbs(*minimum)) * 0.05);
    *minimum = qMax(static_cast<double>(std::numeric_limits<float>::lowest()), *minimum - padding);
    *maximum = qMin(static_cast<double>(std::numeric_limits<float>::max()), *maximum + padding);
    return *maximum > *minimum;
}

QPointF CMotionCurveView::mapPoint(const QPointF& point, const QRectF& plot, float duration,
                                   double minimum, double maximum) const
{
    return QPointF(plot.left() + plot.width() * point.x() / duration,
                   plot.bottom() - plot.height() * (point.y() - minimum) / (maximum - minimum));
}

QPointF CMotionCurveView::unmapPoint(const QPointF& point, const QRectF& plot, float duration,
                                     double minimum, double maximum) const
{
    return QPointF(qBound(0.0, (point.x() - plot.left()) / plot.width(), 1.0) * duration,
                   maximum - qBound(0.0, (point.y() - plot.top()) / plot.height(), 1.0) * (maximum - minimum));
}

bool CMotionCurveView::hitPoint(const QPointF& position, CMotionTrack* track, int* pointIndex) const
{
    if (!m_timeline || !track || !pointIndex) return false;
    const QRectF plot = plotRect();
    double minimum = 0.0;
    double maximum = 0.0;
    if (!valueRange(&minimum, &maximum) || plot.width() <= 0.0 || plot.height() <= 0.0) return false;
    const float duration = qMax(0.001f, m_timeline->Duration());
    const float radiusSquared = 10.0f * 10.0f;
    float nearest = radiusSquared;
    bool found = false;
    for (auto curve = m_timeline->Tracks().cbegin(); curve != m_timeline->Tracks().cend(); ++curve) {
        if (!TrackVisible(curve.key())) continue;
        for (int index = 0; index < curve.value().size(); ++index) {
            const QPointF delta = mapPoint(curve.value().at(index), plot, duration, minimum, maximum) - position;
            const float distanceSquared = static_cast<float>(QPointF::dotProduct(delta, delta));
            if (distanceSquared <= nearest) {
                nearest = distanceSquared;
                *track = curve.key();
                *pointIndex = index;
                found = true;
            }
        }
    }
    return found;
}

void CMotionCurveView::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF plot = plotRect();
    const QColor border = palette().color(QPalette::Mid);
    const QColor text = palette().color(QPalette::Text);
    const QColor accent = palette().color(QPalette::Highlight);
    painter.fillRect(rect(), palette().color(QPalette::Base));
    painter.setPen(border);
    painter.drawRoundedRect(plot, 6, 6);
    painter.setPen(QColor(border.red(), border.green(), border.blue(), 100));
    for (int i = 1; i < 4; ++i) {
        const qreal x = plot.left() + plot.width() * i / 4.0;
        const qreal y = plot.top() + plot.height() * i / 4.0;
        painter.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
        painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
    }
    painter.setPen(text);
    painter.drawText(10, 14, QStringLiteral("曲线"));
    if (!m_timeline || m_timeline->Tracks().isEmpty()) {
        painter.drawText(plot, Qt::AlignCenter, QStringLiteral("暂无曲线"));
        return;
    }
    const float duration = qMax(0.001f, m_timeline->Duration());
    double minimum = 0.0;
    double maximum = 0.0;
    if (!valueRange(&minimum, &maximum)) {
        painter.drawText(plot, Qt::AlignCenter, QStringLiteral("当前类型暂无曲线"));
        return;
    }
    const QVector<QColor> colors = {accent, QColor(220, 120, 70), QColor(80, 170, 125), QColor(170, 110, 210)};
    int colorIndex = 0;
    for (auto it = m_timeline->Tracks().cbegin(); it != m_timeline->Tracks().cend(); ++it, ++colorIndex) {
        if (!TrackVisible(it.key())) continue;
        const QColor color = colors[colorIndex % colors.size()];
        painter.setPen(QPen(color, 1.5));
        QPolygonF line;
        for (int sample = 0; sample <= 80; ++sample) {
            const float time = static_cast<double>(duration) * sample / 80.0;
            line.append(mapPoint(QPointF(time, m_timeline->Evaluate(it.key(), time)), plot, duration, minimum, maximum));
        }
        painter.drawPolyline(line);
        painter.setBrush(color);
        for (int index = 0; index < it.value().size(); ++index) {
            const QPointF& point = it.value().at(index);
            const QPointF mapped = mapPoint(point, plot, duration, minimum, maximum);
            const bool selected = it.key() == m_selectedTrack && index == m_selectedPointIndex;
            painter.setPen(selected ? QPen(text, 2.0) : QPen(color, 1.0));
            painter.drawEllipse(mapped, selected ? 5.0 : 3.5, selected ? 5.0 : 3.5);
        }
    }
    const qreal playheadX = plot.left() + plot.width() * qBound(0.0f, m_time / duration, 1.0f);
    painter.setPen(QPen(text, 1));
    painter.drawLine(QPointF(playheadX, plot.top()), QPointF(playheadX, plot.bottom()));
}

void CMotionCurveView::mousePressEvent(QMouseEvent* event)
{
    if (!event || event->button() != Qt::LeftButton) return;
    CMotionTrack track;
    int pointIndex = -1;
    if (!hitPoint(event->position(), &track, &pointIndex)) return;
    setFocus(Qt::MouseFocusReason);
    m_dragTrack = track;
    m_dragPointIndex = pointIndex;
    emit EditStarted();
    SetSelectedPoint(track, pointIndex);
    emit PointSelected(track, pointIndex);
    event->accept();
}

void CMotionCurveView::mouseMoveEvent(QMouseEvent* event)
{
    if (!event || !m_dragTrack || m_dragPointIndex < 0 || !m_timeline) return;
    if (!(event->buttons() & Qt::LeftButton)) {
        CancelDrag();
        return;
    }
    const auto curve = m_timeline->Tracks().constFind(*m_dragTrack);
    if (curve == m_timeline->Tracks().cend() || m_dragPointIndex >= curve.value().size()) return;
    const QRectF plot = plotRect();
    double minimum = 0.0;
    double maximum = 0.0;
    if (!valueRange(&minimum, &maximum) || plot.width() <= 0.0 || plot.height() <= 0.0) return;
    const float duration = qMax(0.001f, m_timeline->Duration());
    QPointF point = unmapPoint(event->position(), plot, duration, minimum, maximum);
    const float epsilon = qMax(0.001f, duration / 10000.0f);
    if (m_dragPointIndex > 0) point.setX(qMax(point.x(), static_cast<float>(curve.value().at(m_dragPointIndex - 1).x()) + epsilon));
    if (m_dragPointIndex + 1 < curve.value().size()) point.setX(qMin(point.x(), static_cast<float>(curve.value().at(m_dragPointIndex + 1).x()) - epsilon));
    point.setX(qBound(0.0f, static_cast<float>(point.x()), duration));
    emit PointEdited(*m_dragTrack, m_dragPointIndex, static_cast<float>(point.x()), static_cast<float>(point.y()));
    event->accept();
}

void CMotionCurveView::mouseReleaseEvent(QMouseEvent* event)
{
    if (event && event->button() == Qt::LeftButton) {
        CancelDrag();
        event->accept();
    }
}
