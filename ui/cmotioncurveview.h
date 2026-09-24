#ifndef CMOTIONCURVEVIEW_H
#define CMOTIONCURVEVIEW_H

#include "live2dstruct/cmotiontimeline.h"
#include <QRectF>
#include <QWidget>

class QMouseEvent;

class CMotionCurveView : public QWidget
{
    Q_OBJECT
public:
    explicit CMotionCurveView(QWidget* parent = nullptr);
    void SetTimeline(const CMotionTimeline* timeline);
    void SetTime(float time);
    void SetSelectedPoint(const QString& parameterId, int pointIndex);

signals:
    void PointSelected(const QString& parameterId, int pointIndex);
    void PointEdited(const QString& parameterId, int pointIndex, float time, float value);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    const CMotionTimeline* m_timeline = nullptr;
    float m_time = 0.0f;
    QString m_selectedParameter;
    int m_selectedPointIndex = -1;
    QString m_dragParameter;
    int m_dragPointIndex = -1;

    QRectF plotRect() const;
    bool valueRange(double* minimum, double* maximum) const;
    QPointF mapPoint(const QPointF& point, const QRectF& plot, float duration,
                     double minimum, double maximum) const;
    QPointF unmapPoint(const QPointF& point, const QRectF& plot, float duration,
                       double minimum, double maximum) const;
    bool hitPoint(const QPointF& position, QString* parameterId, int* pointIndex) const;
};

#endif
