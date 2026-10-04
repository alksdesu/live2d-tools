#ifndef CMOTIONCURVEVIEW_H
#define CMOTIONCURVEVIEW_H

#include "live2dstruct/cmotiontimeline.h"
#include <QRectF>
#include <QWidget>
#include <optional>

class QMouseEvent;

class CMotionCurveView : public QWidget
{
    Q_OBJECT
public:
    explicit CMotionCurveView(QWidget* parent = nullptr);
    void SetTimeline(const CMotionTimeline* timeline);
    void SetTime(double time);
    void SetSelectedPoint(const CMotionTrack& track, int pointIndex);
    void SetSelectedPoints(const CMotionPointSelection& points);
    const CMotionPointSelection& SelectedPoints() const { return m_selectedPoints; }
    void SetFrameSnapping(bool enabled) { m_frameSnapping = enabled; }
    bool FrameSnapping() const { return m_frameSnapping; }
    void SetTargetFilter(int target);
    int TargetFilter() const { return m_targetFilter; }
    void CancelDrag();
    void FinishDrag();

signals:
    void SelectionChanged(const CMotionPointSelection& points);
    void SelectionDragStarted(const CMotionPointSelection& anchors);
    void SelectionTranslated(double timeDelta, double valueDelta);
    void SelectionDragFinished();
    void SelectionDragCanceled();
    void TangentDragStarted(CMotionPointRef key, CMotionTimeline::HandleSide side, bool linkForGesture);
    void TangentHandleEdited(CMotionPointRef key, CMotionTimeline::HandleSide side, QPointF position);
    void TangentDragFinished();
    void TangentDragCanceled();
    void EventDragStarted(int eventIndex, double time);
    void EventDragged(int eventIndex, double time);
    void EventDragFinished();
    void EventDragCanceled();
    void PointSelected(const CMotionTrack& track, int pointIndex);
    void PointEdited(const CMotionTrack& track, int pointIndex, float time, float value);
    void EditStarted();
    void EditFinished();
    void EventSelected(double time, const QString& value);

protected:
    bool event(QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    const CMotionTimeline* m_timeline = nullptr;
    double m_time = 0.0;
    double m_duration = 0.001;
    QJsonArray m_events;
    struct EventMarker {
        qreal x = 0.0;
        int count = 0;
        int firstEvent = -1;
        QVector<int> previews;
    };
    QVector<EventMarker> m_eventMarkers;
    CMotionPointSelection m_selectedPoints;
    QSet<CMotionPointRef> m_selectedSet;
    bool m_frameSnapping = false;
    struct Projection {
        QRectF plot;
        double duration = 0.001, minimum = 0, maximum = 1;
    };
    struct Gesture {
        enum Kind { Points, Bezier, Marquee, Event } kind = Points;
        quint64 serial = 0;
        Projection projection;
        QPointF press, current, anchor;
        CMotionPointSelection anchors, previousSelection;
        CMotionPointRef point;
        QVector<QPointF> controls;
        QPointF segmentStart, segmentEnd;
        double fps = 30;
        int control = -1;
        Qt::KeyboardModifiers modifiers;
        bool restricted = false, moveValue = true, linkForGesture = false;
        CMotionTimeline::HandleSide side = CMotionTimeline::HandleSide::Incoming;
        std::optional<double> previousTime, nextTime;
        int eventIndex = -1;
        double eventTime = 0;
        bool eventEditStarted = false;
    };
    std::optional<Gesture> m_gesture;
    quint64 m_gestureSerial = 0;
    int m_targetFilter = -1;
    bool TrackVisible(const CMotionTrack& track) const { return m_targetFilter < 0 || static_cast<int>(track.target) == m_targetFilter; }

    QRectF plotRect() const;
    QRectF eventRect() const;
    void rebuildEventMarkers();
    const EventMarker* hitEvent(const QPointF& position) const;
    QString eventToolTip(const EventMarker& marker) const;
    QString controlToolTip(const CMotionPointRef& segment, int control, bool* movable = nullptr) const;
    bool valueRange(double* minimum, double* maximum) const;
    bool projection(Projection& result) const;
    bool validPoint(const CMotionPointRef& point) const;
    void changeSelection(const CMotionPointSelection& points);
    void updateMarquee(const Gesture& gesture);
    bool hitControl(const QPointF& position, CMotionPointRef& segment, int& control) const;
    bool showControls(const CMotionTrack& track, int endIndex) const;
    void endGesture(bool canceled);
    void updateDrag(const QPointF& position, Qt::KeyboardModifiers modifiers);
    QPointF mapPoint(const QPointF& point, const QRectF& plot, double duration,
                     double minimum, double maximum) const;
    bool hitPoint(const QPointF& position, CMotionTrack* track, int* pointIndex) const;
};

#endif
