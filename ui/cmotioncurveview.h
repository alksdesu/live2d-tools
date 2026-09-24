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
    void SetTime(float time);
    void SetSelectedPoint(const CMotionTrack& track, int pointIndex);
    void SetTargetFilter(int target);
    void CancelDrag();

signals:
    void PointSelected(const CMotionTrack& track, int pointIndex);
    void PointEdited(const CMotionTrack& track, int pointIndex, float time, float value);
    void EditStarted();
    void EditFinished();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    const CMotionTimeline* m_timeline = nullptr;
    float m_time = 0.0f;
    CMotionTrack m_selectedTrack;
    int m_selectedPointIndex = -1;
    std::optional<CMotionTrack> m_dragTrack;
    int m_targetFilter = -1;
    bool TrackVisible(const CMotionTrack& track) const { return m_targetFilter < 0 || static_cast<int>(track.target) == m_targetFilter; }
    int m_dragPointIndex = -1;

    QRectF plotRect() const;
    bool valueRange(double* minimum, double* maximum) const;
    QPointF mapPoint(const QPointF& point, const QRectF& plot, float duration,
                     double minimum, double maximum) const;
    QPointF unmapPoint(const QPointF& point, const QRectF& plot, float duration,
                       double minimum, double maximum) const;
    bool hitPoint(const QPointF& position, CMotionTrack* track, int* pointIndex) const;
};

#endif
