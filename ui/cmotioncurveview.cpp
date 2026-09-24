#include "cmotioncurveview.h"

#include <QPainter>
#include <QPolygonF>
#include <QMouseEvent>
#include <QHelpEvent>
#include <QResizeEvent>
#include <QToolTip>
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
    QToolTip::hideText();
    if (m_timeline != timeline) CancelDrag();
    m_timeline = timeline;
    const auto events = timeline ? timeline->Events() : QJsonArray{};
    const double duration = timeline ? timeline->Timing().duration : 0.0;
    const double displayDuration = qIsFinite(duration) && duration > 0.0 ? duration : 0.001;
    const bool markersChanged = events != m_events || displayDuration != m_duration;
    m_events = events;
    m_duration = displayDuration;
    if (markersChanged) rebuildEventMarkers();
    update();
}

void CMotionCurveView::SetTime(double time)
{
    m_time = qIsFinite(time) ? qMax(0.0, time) : 0.0;
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
    return rect().adjusted(34, m_events.isEmpty() ? 16 : 52, -14, -22);
}

QRectF CMotionCurveView::eventRect() const
{
    return QRectF(34, 16, qMax(0, width() - 48), 22);
}

void CMotionCurveView::rebuildEventMarkers()
{
    m_eventMarkers.clear();
    const QRectF lane = eventRect();
    if (m_events.isEmpty() || lane.width() <= 0.0) return;
    const int buckets = qMax(1, static_cast<int>(lane.width() / 14.0));
    QVector<EventMarker> markers(buckets);
    for (int index = 0; index < m_events.size(); ++index) {
        const auto entry = m_events.at(index).toObject();
        const double time = entry.value(QStringLiteral("Time")).toDouble(-1.0);
        if (!qIsFinite(time) || time < 0.0) continue;
        const double ratio = qBound(0.0, time / m_duration, 1.0);
        const int bucket = qMin(buckets - 1, static_cast<int>(ratio * buckets));
        auto& marker = markers[bucket];
        marker.x = lane.left() + (bucket + 0.5) * lane.width() / buckets;
        if (marker.firstEvent < 0 || time < m_events.at(marker.firstEvent).toObject().value(QStringLiteral("Time")).toDouble())
            marker.firstEvent = index;
        ++marker.count;
        if (marker.previews.size() < 6) marker.previews.append(index);
    }
    for (const auto& marker : markers)
        if (marker.count > 0) m_eventMarkers.append(marker);
}

const CMotionCurveView::EventMarker* CMotionCurveView::hitEvent(const QPointF& position) const
{
    if (m_events.isEmpty() || !eventRect().contains(position)) return nullptr;
    const EventMarker* nearest = nullptr;
    qreal distance = 8.0;
    for (const auto& marker : m_eventMarkers) {
        const qreal candidate = qAbs(marker.x - position.x());
        if (candidate <= distance) {
            distance = candidate;
            nearest = &marker;
        }
    }
    return nearest;
}

QString CMotionCurveView::eventToolTip(const EventMarker& marker) const
{
    QStringList lines{QStringLiteral("%1 个事件 · 点击定位").arg(marker.count)};
    for (const int index : marker.previews) {
        const auto entry = m_events.at(index).toObject();
        QString value = entry.value(QStringLiteral("Value")).toString();
        if (value.size() > 200) {
            value.truncate(value.at(199).isHighSurrogate() ? 199 : 200);
            value += QChar(0x2026);
        }
        lines.append(QStringLiteral("%1 s · %2")
            .arg(entry.value(QStringLiteral("Time")).toDouble(), 0, 'g', 15).arg(value.toHtmlEscaped()));
    }
    if (marker.count > marker.previews.size())
        lines.append(QStringLiteral("另有 %1 个事件，可在动作设置中查看").arg(marker.count - marker.previews.size()));
    return QStringLiteral("<qt>%1</qt>").arg(lines.join(QStringLiteral("<br>")));
}

bool CMotionCurveView::event(QEvent* event)
{
    if (event->type() == QEvent::ToolTip) {
        const auto* help = static_cast<QHelpEvent*>(event);
        const auto* marker = hitEvent(help->pos());
        if (marker && !m_dragTrack) QToolTip::showText(help->globalPos(), eventToolTip(*marker), this, eventRect().toRect());
        else QToolTip::hideText();
        return true;
    }
    return QWidget::event(event);
}

void CMotionCurveView::resizeEvent(QResizeEvent* event)
{
    QToolTip::hideText();
    rebuildEventMarkers();
    QWidget::resizeEvent(event);
}

void CMotionCurveView::leaveEvent(QEvent* event)
{
    QToolTip::hideText();
    QWidget::leaveEvent(event);
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

QPointF CMotionCurveView::mapPoint(const QPointF& point, const QRectF& plot, double duration,
                                   double minimum, double maximum) const
{
    return QPointF(plot.left() + plot.width() * point.x() / duration,
                   plot.bottom() - plot.height() * (point.y() - minimum) / (maximum - minimum));
}

QPointF CMotionCurveView::unmapPoint(const QPointF& point, const QRectF& plot, double duration,
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
    const double duration = m_duration;
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
    if (!m_events.isEmpty()) {
        const QRectF lane = eventRect();
        painter.setPen(border);
        painter.drawRoundedRect(lane, 4, 4);
        painter.setPen(text);
        painter.drawText(10, 14, QStringLiteral("事件 · %1").arg(m_events.size()));
        for (const auto& marker : m_eventMarkers) {
            const QPointF center(marker.x, lane.center().y());
            painter.setPen(accent);
            painter.setBrush(accent);
            if (marker.count > 1) {
                painter.drawEllipse(center, 6, 6);
                painter.setPen(palette().color(QPalette::HighlightedText));
                painter.drawText(QRectF(center.x() - 6, center.y() - 8, 12, 16), Qt::AlignCenter,
                                 marker.count < 10 ? QString::number(marker.count) : QStringLiteral("+"));
            } else {
                painter.drawPolygon(QPolygonF{center + QPointF(0, -5), center + QPointF(5, 0),
                                              center + QPointF(0, 5), center + QPointF(-5, 0)});
            }
        }
        painter.setBrush(Qt::NoBrush);
    }
    if (plot.width() <= 0.0 || plot.height() <= 0.0) return;
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
    painter.drawText(10, m_events.isEmpty() ? 14 : 50, QStringLiteral("曲线"));
    const double duration = m_duration;
    double minimum = 0.0;
    double maximum = 0.0;
    if (!valueRange(&minimum, &maximum)) {
        painter.drawText(plot, Qt::AlignCenter, !m_timeline || m_timeline->Tracks().isEmpty()
                         ? (m_events.isEmpty() ? QStringLiteral("暂无曲线") : QStringLiteral("此动作仅包含事件"))
                         : QStringLiteral("当前类型暂无曲线"));
    } else {
        painter.save();
        painter.setClipRect(plot.adjusted(-6, -6, 6, 6));
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
        painter.restore();
    }
    if (!m_timeline) return;
    const qreal playheadX = plot.left() + plot.width() * qBound(0.0, m_time / duration, 1.0);
    painter.setPen(QPen(text, 1));
    painter.drawLine(QPointF(playheadX, m_events.isEmpty() ? plot.top() : eventRect().top()), QPointF(playheadX, plot.bottom()));
}

void CMotionCurveView::mousePressEvent(QMouseEvent* event)
{
    if (!event || event->button() != Qt::LeftButton) return;
    if (const auto* marker = hitEvent(event->position())) {
        setFocus(Qt::MouseFocusReason);
        const auto entry = m_events.at(marker->firstEvent).toObject();
        emit EventSelected(entry.value(QStringLiteral("Time")).toDouble(), entry.value(QStringLiteral("Value")).toString());
        event->accept();
        return;
    }
    CMotionTrack track;
    int pointIndex = -1;
    if (!hitPoint(event->position(), &track, &pointIndex)) return;
    setFocus(Qt::MouseFocusReason);
    QToolTip::hideText();
    m_dragTrack = track;
    m_dragPointIndex = pointIndex;
    emit EditStarted();
    SetSelectedPoint(track, pointIndex);
    emit PointSelected(track, pointIndex);
    event->accept();
}

void CMotionCurveView::mouseMoveEvent(QMouseEvent* event)
{
    if (!event) return;
    if (!m_dragTrack) {
        const auto* marker = hitEvent(event->position());
        setCursor(marker ? Qt::PointingHandCursor : Qt::ArrowCursor);
        if (QToolTip::isVisible()) {
            if (marker) QToolTip::showText(event->globalPosition().toPoint(), eventToolTip(*marker), this, eventRect().toRect());
            else QToolTip::hideText();
        }
        return;
    }
    if (m_dragPointIndex < 0 || !m_timeline) return;
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
    const double duration = m_duration;
    QPointF point = unmapPoint(event->position(), plot, duration, minimum, maximum);
    const double epsilon = qMax(0.001, duration / 10000.0);
    if (m_dragPointIndex > 0) point.setX(qMax(point.x(), static_cast<float>(curve.value().at(m_dragPointIndex - 1).x()) + epsilon));
    if (m_dragPointIndex + 1 < curve.value().size()) point.setX(qMin(point.x(), static_cast<float>(curve.value().at(m_dragPointIndex + 1).x()) - epsilon));
    point.setX(qBound(0.0, point.x(), duration));
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
