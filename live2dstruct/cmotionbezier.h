#ifndef CMOTIONBEZIER_H
#define CMOTIONBEZIER_H

#include <QPointF>

class CMotionBezier
{
public:
    static float Parameter(float start, float first, float second, float end, float time);
    static float Evaluate(const QPointF& start, const QPointF& first, const QPointF& second,
                          const QPointF& end, float time, bool restricted);
};

#endif
