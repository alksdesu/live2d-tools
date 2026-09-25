#ifndef CUVGEOMETRY_H
#define CUVGEOMETRY_H

#include <QPointF>
#include <QRectF>
#include <vector>

class CUvGeometry
{
public:
    static int Orientation(QPointF a, QPointF b, QPointF c);
    static bool ContainsTriangle(QPointF point, QPointF a, QPointF b, QPointF c);
    static bool ClipSegment(QPointF& a, QPointF& b, QRectF rect);
    static std::vector<QPointF> ClipTriangle(QPointF a, QPointF b, QPointF c, QRectF rect);
};

#endif
