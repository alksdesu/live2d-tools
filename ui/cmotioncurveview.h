#ifndef CMOTIONCURVEVIEW_H
#define CMOTIONCURVEVIEW_H

#include "cmotiontimeline.h"
#include <QWidget>

class CMotionCurveView : public QWidget
{
    Q_OBJECT
public:
    explicit CMotionCurveView(QWidget* parent = nullptr);
    void SetTimeline(const CMotionTimeline* timeline);
    void SetTime(float time);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    const CMotionTimeline* m_timeline = nullptr;
    float m_time = 0.0f;
};

#endif
