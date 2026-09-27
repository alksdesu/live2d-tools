#include "cmotioncurveview.h"

#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QKeyEvent>
#include <cmath>
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
    const QPointer<CMotionCurveView> alive(this);
    if (m_timeline != timeline) {
        CancelDrag();
        if (!alive) return;
        m_selectedPoints.clear(); m_selectedSet.clear();
    }
    m_timeline = timeline;
    const auto events = timeline ? timeline->Events() : QJsonArray{};
    const double duration = timeline ? timeline->Timing().duration : 0.0;
    const double displayDuration = qIsFinite(duration) && duration > 0.0 ? duration : 0.001;
    const bool markersChanged = events != m_events || displayDuration != m_duration;
    m_events = events;
    m_duration = displayDuration;
    SetSelectedPoints(m_selectedPoints);
    if (markersChanged) rebuildEventMarkers();
    update();
}

void CMotionCurveView::SetTime(double time)
{
    m_time = qIsFinite(time) ? qMax(0.0, time) : 0.0;
    update();
}

bool CMotionCurveView::validPoint(const CMotionPointRef& point) const
{
    if (!m_timeline || !TrackVisible(point.track)) return false;
    const auto track = m_timeline->Tracks().constFind(point.track);
    return track != m_timeline->Tracks().cend() && point.index >= 0 && point.index < track->size();
}

void CMotionCurveView::SetSelectedPoint(const CMotionTrack& track, int pointIndex)
{
    SetSelectedPoints({{track, pointIndex}});
}

void CMotionCurveView::SetSelectedPoints(const CMotionPointSelection& points)
{
    CMotionPointSelection candidate;
    QSet<CMotionPointRef> unique;
    for (const auto& point : points) {
        if (!validPoint(point) || unique.contains(point)) continue;
        unique.insert(point); candidate.append(point);
    }
    std::sort(candidate.begin(), candidate.end());
    m_selectedPoints.swap(candidate); m_selectedSet.swap(unique);
    update();
}

void CMotionCurveView::changeSelection(const CMotionPointSelection& points)
{
    const auto previous = m_selectedPoints;
    SetSelectedPoints(points);
    if (previous != m_selectedPoints) {
        const auto selected = m_selectedPoints;
        emit SelectionChanged(selected);
    }
}

void CMotionCurveView::SetTargetFilter(int target)
{
    if (m_targetFilter == target) return;
    const QPointer<CMotionCurveView> alive(this);
    CancelDrag();
    if (!alive) return;
    m_targetFilter = target;
    changeSelection(m_selectedPoints);
    if (alive) update();
}

void CMotionCurveView::CancelDrag() { endGesture(true); }
void CMotionCurveView::FinishDrag() { endGesture(false); }

void CMotionCurveView::endGesture(bool canceled)
{
    if (!m_gesture) return;
    const auto gesture = *m_gesture;
    m_gesture.reset();
    const auto serial = ++m_gestureSerial;
    rebuildEventMarkers(); update();
    const QPointer<CMotionCurveView> alive(this);
    if (gesture.kind == Gesture::Marquee) {
        if (canceled) changeSelection(gesture.previousSelection);
        return;
    }
    if (gesture.kind == Gesture::Points) {
        if (canceled) emit SelectionDragCanceled(); else emit SelectionDragFinished();
    } else {
        if (canceled) emit TangentDragCanceled(); else emit TangentDragFinished();
    }
    if (alive && serial == m_gestureSerial) emit EditFinished();
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
        const double duration = m_gesture ? m_gesture->projection.duration : m_duration;
        const double ratio = qBound(0.0, time / duration, 1.0);
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

QString CMotionCurveView::controlToolTip(const CMotionPointRef& segment, int control, bool* movable) const
{
    if (movable) *movable = false;
    if (!m_timeline || control < 0 || control > 1) return {};
    const CMotionPointRef key{segment.track, control == 0 ? segment.index - 1 : segment.index};
    const auto capabilities = m_timeline->Capabilities(key);
    const auto capability = control == 0 ? capabilities.outgoing : capabilities.incoming;
    if (!capability.available) return {};
    if (!capability.moveTime && !capability.moveValue)
        return QStringLiteral("水平切线的值与受限 Bézier 的时间均固定；请在切线菜单转换为自由 Bézier，或更改切线模式");
    if (movable) *movable = true;
    QString text;
    switch (capabilities.state.mode) {
    case CMotionTimeline::TangentMode::Broken:
        text = QStringLiteral("独立切线；Ctrl/⌘ 开始拖动可临时联动另一侧，模式保持独立"); break;
    case CMotionTimeline::TangentMode::Aligned:
        text = QStringLiteral("对齐切线；调整一侧时，两侧保持相同斜率"); break;
    case CMotionTimeline::TangentMode::Flat:
        text = QStringLiteral("水平切线；可横向调整手柄长度，数值保持水平"); break;
    case CMotionTimeline::TangentMode::AutoClamped:
        text = QStringLiteral("自动钳制切线；有效拖动会转为对齐，Esc 取消并恢复自动模式"); break;
    }
    if (capabilities.restricted) text += QStringLiteral("；受限 Bézier 的手柄时间固定");
    return text;
}

bool CMotionCurveView::event(QEvent* event)
{
    if (event->type() == QEvent::ToolTip) {
        const auto* help = static_cast<QHelpEvent*>(event);
        const auto* marker = hitEvent(help->pos());
        CMotionPointRef segment; int control = -1;
        if (marker && !m_gesture) QToolTip::showText(help->globalPos(), eventToolTip(*marker), this, eventRect().toRect());
        else if (!m_gesture && hitControl(help->pos(), segment, control))
            QToolTip::showText(help->globalPos(), controlToolTip(segment, control), this);
        else QToolTip::hideText();
        return true;
    }
    const QPointer<CMotionCurveView> alive(this);
    if (event->type() == QEvent::KeyPress && static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape && m_gesture) {
        CancelDrag(); event->accept(); return true;
    }
    if (event->type() == QEvent::Hide || event->type() == QEvent::WindowDeactivate ||
        event->type() == QEvent::UngrabMouse || (event->type() == QEvent::EnabledChange && !isEnabled())) CancelDrag();
    return alive ? QWidget::event(event) : true;
}

void CMotionCurveView::resizeEvent(QResizeEvent* event)
{
    QToolTip::hideText();
    const QPointer<CMotionCurveView> alive(this);
    CancelDrag();
    if (!alive) return;
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
    bool found = false;
    const auto include = [&](const QPointF& point) {
        if (!std::isfinite(point.x()) || !std::isfinite(point.y())) return false;
        *minimum = std::min(*minimum, point.y()); *maximum = std::max(*maximum, point.y());
        found = true; return true;
    };
    for (auto track = m_timeline->Tracks().cbegin(); track != m_timeline->Tracks().cend(); ++track) {
        if (!TrackVisible(track.key())) continue;
        for (int index = 0; index < track->size(); ++index) {
            if (!include(track->at(index))) return false;
            for (const auto& control : m_timeline->BezierControls(track.key(), index)) if (!include(control)) return false;
        }
    }
    if (!found) return false;
    const double span = *maximum - *minimum;
    const double padding = std::max(0.05, (span > 0.0 ? span : std::abs(*minimum)) * 0.05);
    *minimum = std::max(double(std::numeric_limits<float>::lowest()), *minimum - padding);
    *maximum = std::min(double(std::numeric_limits<float>::max()), *maximum + padding);
    return *maximum > *minimum;
}

bool CMotionCurveView::projection(Projection& result) const
{
    if (m_gesture) { result = m_gesture->projection; return true; }
    Projection candidate;
    candidate.plot = plotRect(); candidate.duration = m_duration;
    if (candidate.plot.width() <= 0 || candidate.plot.height() <= 0 ||
        !valueRange(&candidate.minimum, &candidate.maximum)) return false;
    result = candidate; return true;
}

QPointF CMotionCurveView::mapPoint(const QPointF& point, const QRectF& plot, double duration,
                                   double minimum, double maximum) const
{
    return QPointF(plot.left() + plot.width() * point.x() / duration,
                   plot.bottom() - plot.height() * (point.y() - minimum) / (maximum - minimum));
}

bool CMotionCurveView::hitPoint(const QPointF& position, CMotionTrack* track, int* pointIndex) const
{
    Projection frame;
    if (!m_timeline || !track || !pointIndex || !projection(frame)) return false;
    double nearest = 100;
    bool found = false, selected = false;
    for (auto curve = m_timeline->Tracks().cbegin(); curve != m_timeline->Tracks().cend(); ++curve) {
        if (!TrackVisible(curve.key())) continue;
        for (int index = 0; index < curve->size(); ++index) {
            const auto delta = mapPoint(curve->at(index), frame.plot, frame.duration, frame.minimum, frame.maximum) - position;
            const double distance = QPointF::dotProduct(delta, delta);
            const bool currentSelected = m_selectedSet.contains({curve.key(), index});
            if (distance < nearest || (!found && distance == nearest) || (distance == nearest && currentSelected && !selected)) {
                nearest = distance; *track = curve.key(); *pointIndex = index; found = true; selected = currentSelected;
            }
        }
    }
    return found;
}

bool CMotionCurveView::showControls(const CMotionTrack& track, int endIndex) const
{
    return m_selectedSet.contains({track, endIndex}) || m_selectedSet.contains({track, endIndex - 1});
}

bool CMotionCurveView::hitControl(const QPointF& position, CMotionPointRef& segment, int& control) const
{
    Projection frame;
    if (!m_timeline || !projection(frame)) return false;
    double nearest = 64;
    bool found = false;
    for (auto track = m_timeline->Tracks().cbegin(); track != m_timeline->Tracks().cend(); ++track) {
        if (!TrackVisible(track.key())) continue;
        for (int index = 1; index < track->size(); ++index) {
            if (!showControls(track.key(), index)) continue;
            const auto controls = m_timeline->BezierControls(track.key(), index);
            for (int at = 0; at < controls.size(); ++at) {
                const CMotionPointRef owner{track.key(), at == 0 ? index - 1 : index};
                if (!m_selectedSet.contains(owner)) continue;
                const auto delta = mapPoint(controls[at], frame.plot, frame.duration, frame.minimum, frame.maximum) - position;
                const double distance = QPointF::dotProduct(delta, delta);
                if (distance < nearest || (!found && distance == nearest)) {
                    nearest = distance; segment = {track.key(), index}; control = at; found = true;
                }
            }
        }
    }
    return found;
}

void CMotionCurveView::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    Projection frame;
    const bool available = projection(frame);
    const QRectF plot = available ? frame.plot : plotRect();
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
    const double duration = available ? frame.duration : m_duration;
    const double minimum = frame.minimum, maximum = frame.maximum;
    if (!available) {
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
            if (it->isEmpty()) continue;
            QPainterPath line;
            const auto mapped = [&](const QPointF& point) { return mapPoint(point, plot, duration, minimum, maximum); };
            line.moveTo(mapped(QPointF(0, it->first().y())));
            line.lineTo(mapped(it->first()));
            for (int index = 1; index < it->size(); ++index) {
                const auto& previous = it->at(index - 1);
                const auto& point = it->at(index);
                const int type = m_timeline->SegmentType(it.key(), index);
                const auto controls = m_timeline->BezierControls(it.key(), index);
                if (type == 1 && controls.size() == 2) line.cubicTo(mapped(controls[0]), mapped(controls[1]), mapped(point));
                else {
                    if (type == 2) line.lineTo(mapped(QPointF(point.x(), previous.y())));
                    if (type == 3) line.lineTo(mapped(QPointF(previous.x(), point.y())));
                    line.lineTo(mapped(point));
                }
            }
            line.lineTo(mapped(QPointF(duration, it->last().y())));
            painter.setBrush(Qt::NoBrush);
            painter.drawPath(line);
            for (int index = 1; index < it->size(); ++index) {
                if (!showControls(it.key(), index)) continue;
                const auto controls = m_timeline->BezierControls(it.key(), index);
                if (controls.size() != 2) continue;
                for (int side = 0; side < controls.size(); ++side) {
                    const int owner = side == 0 ? index - 1 : index;
                    if (!m_selectedSet.contains({it.key(), owner})) continue;
                    const auto capability = m_timeline->Capabilities({it.key(), owner});
                    const auto handle = side == 0 ? capability.outgoing : capability.incoming;
                    painter.setPen(QPen(color, 1.0, Qt::DashLine)); painter.setBrush(Qt::NoBrush);
                    painter.drawLine(mapped(it->at(owner)), mapped(controls[side]));
                    painter.setPen(QPen(text, 1.0)); painter.setBrush(handle.moveTime || handle.moveValue ? QBrush(color) : QBrush(Qt::NoBrush));
                    const auto center = mapped(controls[side]); painter.drawRect(QRectF(center.x() - 4, center.y() - 4, 8, 8));
                }
            }
            painter.setBrush(color);
            for (int index = 0; index < it.value().size(); ++index) {
                const QPointF& point = it.value().at(index);
                const QPointF mapped = mapPoint(point, plot, duration, minimum, maximum);
                const bool selected = m_selectedSet.contains({it.key(), index});
                painter.setPen(selected ? QPen(text, 2.0) : QPen(color, 1.0));
                painter.drawEllipse(mapped, selected ? 5.0 : 3.5, selected ? 5.0 : 3.5);
            }
        }
        painter.restore();
    }
    if (m_gesture && m_gesture->kind == Gesture::Marquee) {
        auto fill = accent; fill.setAlpha(35);
        painter.setPen(QPen(accent, 1, Qt::DashLine)); painter.setBrush(fill);
        painter.drawRect(QRectF(m_gesture->press, m_gesture->current).normalized().intersected(plot));
    }
    if (hasFocus()) {
        painter.setPen(QPen(accent, 1, Qt::DotLine)); painter.setBrush(Qt::NoBrush);
        painter.drawRect(rect().adjusted(1, 1, -2, -2));
    }
    if (!m_timeline) return;
    const qreal playheadX = plot.left() + plot.width() * qBound(0.0, m_time / duration, 1.0);
    painter.setPen(QPen(text, 1));
    painter.drawLine(QPointF(playheadX, m_events.isEmpty() ? plot.top() : eventRect().top()), QPointF(playheadX, plot.bottom()));
}

void CMotionCurveView::mousePressEvent(QMouseEvent* event)
{
    if (!event || event->button() != Qt::LeftButton || m_gesture) return;
    const QPointer<CMotionCurveView> alive(this);
    setFocus(Qt::MouseFocusReason);
    if (!alive || m_gesture) return;
    if (const auto* marker = hitEvent(event->position())) {
        const auto entry = m_events.at(marker->firstEvent).toObject();
        event->accept();
        emit EventSelected(entry.value(QStringLiteral("Time")).toDouble(), entry.value(QStringLiteral("Value")).toString());
        return;
    }
    Projection frame;
    if (!projection(frame) || !frame.plot.adjusted(-10, -10, 10, 10).contains(event->position())) return;
    if (!m_timeline) return;
    QToolTip::hideText();
    const auto* timeline = m_timeline;
    const CMotionTimeline before = *timeline;
    Gesture gesture;
    gesture.serial = ++m_gestureSerial; gesture.projection = frame;
    gesture.press = gesture.current = event->position(); gesture.modifiers = event->modifiers();
    gesture.previousSelection = m_selectedPoints; gesture.fps = m_timeline->Timing().fps;
    CMotionPointRef segment; int control = -1;
    if (hitControl(event->position(), segment, control)) {
        gesture.kind = Gesture::Bezier; gesture.point = {segment.track, control == 0 ? segment.index - 1 : segment.index}; gesture.control = control;
        gesture.side = control == 0 ? CMotionTimeline::HandleSide::Outgoing : CMotionTimeline::HandleSide::Incoming;
        const auto capabilities = m_timeline->Capabilities(gesture.point);
        const auto capability = gesture.side == CMotionTimeline::HandleSide::Incoming ? capabilities.incoming : capabilities.outgoing;
        if (!capability.available || (!capability.moveTime && !capability.moveValue)) { event->accept(); return; }
        gesture.controls = m_timeline->BezierControls(segment.track, segment.index);
        const auto& points = m_timeline->Tracks()[segment.track];
        gesture.segmentStart = points[segment.index - 1]; gesture.segmentEnd = points[segment.index];
        gesture.anchor = gesture.controls[control]; gesture.restricted = !capability.moveTime; gesture.moveValue = capability.moveValue;
        gesture.linkForGesture = event->modifiers() & (Qt::ControlModifier | Qt::MetaModifier);
    } else {
        CMotionTrack track; int index = -1;
        if (!hitPoint(event->position(), &track, &index)) {
            if (!frame.plot.contains(event->position())) return;
            gesture.kind = Gesture::Marquee;
            m_gesture = gesture;
            updateMarquee(gesture);
            event->accept(); return;
        }
        const CMotionPointRef point{track, index};
        auto selected = m_selectedPoints;
        if (event->modifiers() & (Qt::ControlModifier | Qt::MetaModifier)) {
            if (selected.contains(point)) selected.removeAll(point); else selected.append(point);
        } else if (event->modifiers() & Qt::ShiftModifier) {
            if (!selected.contains(point)) selected.append(point);
        } else if (!selected.contains(point)) selected = {point};
        changeSelection(selected);
        if (!alive || m_gesture || m_gestureSerial != gesture.serial || m_timeline != timeline ||
            !m_timeline->HasSameContent(before) || !validPoint(point)) return;
        if (!m_selectedSet.contains(point)) { event->accept(); return; }
        gesture.point = point; gesture.anchors = m_selectedPoints;
        const auto& points = m_timeline->Tracks()[track];
        gesture.anchor = points[index];
        if (index > 0) gesture.previousTime = points[index - 1].x();
        if (index + 1 < points.size()) gesture.nextTime = points[index + 1].x();
    }
    m_gesture = gesture;
    event->accept(); update();
    if (gesture.kind == Gesture::Points) emit SelectionDragStarted(gesture.anchors);
    else emit TangentDragStarted(gesture.point, gesture.side, gesture.linkForGesture);
    if (!alive || !m_gesture || m_gesture->serial != gesture.serial) return;
    emit EditStarted();
    if (!alive || !m_gesture || m_gesture->serial != gesture.serial) return;
    if (gesture.kind == Gesture::Points && gesture.anchors.size() == 1)
        emit PointSelected(gesture.point.track, gesture.point.index);
}

void CMotionCurveView::updateMarquee(const Gesture& gesture)
{
    if (!m_timeline) return;
    const auto& frame = gesture.projection;
    const auto area = QRectF(gesture.press, gesture.current).normalized().intersected(frame.plot);
    QSet<CMotionPointRef> result(gesture.previousSelection.cbegin(), gesture.previousSelection.cend());
    const bool toggle = gesture.modifiers & (Qt::ControlModifier | Qt::MetaModifier);
    const bool subtract = gesture.modifiers & Qt::AltModifier;
    if (!toggle && !subtract && !(gesture.modifiers & Qt::ShiftModifier)) result.clear();
    for (auto track = m_timeline->Tracks().cbegin(); track != m_timeline->Tracks().cend(); ++track) {
        if (!TrackVisible(track.key())) continue;
        for (int index = 0; index < track->size(); ++index) {
            if (!area.contains(mapPoint(track->at(index), frame.plot, frame.duration, frame.minimum, frame.maximum))) continue;
            const CMotionPointRef point{track.key(), index};
            if (subtract || (toggle && result.contains(point))) result.remove(point); else result.insert(point);
        }
    }
    changeSelection(CMotionPointSelection(result.cbegin(), result.cend()));
}

void CMotionCurveView::updateDrag(const QPointF& position, Qt::KeyboardModifiers modifiers)
{
    if (!m_gesture || !std::isfinite(position.x()) || !std::isfinite(position.y())) return;
    m_gesture->current = position;
    const auto gesture = *m_gesture;
    if (gesture.kind == Gesture::Marquee) { updateMarquee(gesture); return; }
    const auto& frame = gesture.projection;
    const QPointF movement = position - gesture.press;
    double dt = movement.x() / frame.plot.width() * frame.duration;
    double dv = -movement.y() / frame.plot.height() * (frame.maximum - frame.minimum);
    if (modifiers & Qt::ShiftModifier) {
        if (std::abs(movement.x()) >= std::abs(movement.y())) dv = 0;
        else dt = 0;
    }
    if (m_frameSnapping && !(modifiers & Qt::AltModifier) && dt != 0 && std::isfinite(gesture.fps) && gesture.fps > 0)
        dt = std::round((gesture.anchor.x() + dt) * gesture.fps) / gesture.fps - gesture.anchor.x();
    if (!std::isfinite(dt) || !std::isfinite(dv)) return;
    const QPointer<CMotionCurveView> alive(this);
    if (gesture.kind == Gesture::Bezier) {
        const QPointF candidate = dt == 0 && dv == 0 ? gesture.anchor : QPointF(gesture.restricted ? gesture.anchor.x() :
            std::clamp(gesture.anchor.x() + dt, gesture.segmentStart.x(), gesture.segmentEnd.x()), gesture.moveValue ? gesture.anchor.y() + dv : gesture.anchor.y());
        if (!std::isfinite(candidate.y())) return;
        emit TangentHandleEdited(gesture.point, gesture.side, candidate);
        return;
    }
    emit SelectionTranslated(dt, dv);
    if (!alive || !m_gesture || m_gesture->serial != gesture.serial || gesture.anchors.size() != 1) return;
    double time = std::max(0.0, gesture.anchor.x() + dt);
    if (gesture.previousTime) time = std::max(time, double(std::nextafter(float(*gesture.previousTime), std::numeric_limits<float>::infinity())));
    if (gesture.nextTime) time = std::min(time, double(std::nextafter(float(*gesture.nextTime), -std::numeric_limits<float>::infinity())));
    if (time < 0 || (gesture.previousTime && time <= *gesture.previousTime) || (gesture.nextTime && time >= *gesture.nextTime)) return;
    const double value = gesture.anchor.y() + dv;
    if (!std::isfinite(time) || !std::isfinite(value) || time > std::numeric_limits<float>::max() ||
        std::abs(value) > std::numeric_limits<float>::max()) return;
    emit PointEdited(gesture.point.track, gesture.point.index, float(time), float(value));
}

void CMotionCurveView::mouseMoveEvent(QMouseEvent* event)
{
    if (!event) return;
    if (!m_gesture) {
        CMotionPointRef segment; int control;
        const auto* marker = hitEvent(event->position());
        bool movable = false;
        const auto text = hitControl(event->position(), segment, control) ? controlToolTip(segment, control, &movable) : QString();
        setCursor(marker || movable ? Qt::PointingHandCursor : Qt::ArrowCursor);
        if (QToolTip::isVisible()) {
            if (marker) QToolTip::showText(event->globalPosition().toPoint(), eventToolTip(*marker), this, eventRect().toRect());
            else if (!text.isEmpty()) QToolTip::showText(event->globalPosition().toPoint(), text, this);
            else QToolTip::hideText();
        }
        return;
    }
    if (!(event->buttons() & Qt::LeftButton)) { CancelDrag(); event->accept(); return; }
    updateDrag(event->position(), event->modifiers());
    event->accept();
}

void CMotionCurveView::mouseReleaseEvent(QMouseEvent* event)
{
    if (!event || event->button() != Qt::LeftButton || !m_gesture) return;
    const QPointer<CMotionCurveView> alive(this);
    if (m_gesture->current != event->position()) updateDrag(event->position(), event->modifiers());
    if (alive) FinishDrag();
    event->accept();
}
