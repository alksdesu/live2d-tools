#ifndef CROTATIONFRAMEGEOMETRY_H
#define CROTATIONFRAMEGEOMETRY_H

#include <QPointF>
#include <QString>

namespace live2dstruct {
class CRotationFrameGeometry
{
public:
    struct Frame {
        QPointF origin;
        QPointF xAxis{1, 0};
        QPointF yAxis{0, 1};
        double scale = 0;
    };

    static bool Validate(const Frame& frame, QString* error = nullptr);
    static bool Mirror(const Frame& frame, QPointF linePoint, QPointF lineNormal,
                       Frame& result, QString* error = nullptr);
    static bool Matches(const Frame& actual, const Frame& expected, QString* error = nullptr);
};
}

#endif
