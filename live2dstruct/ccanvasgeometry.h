#ifndef CCANVASGEOMETRY_H
#define CCANVASGEOMETRY_H

#include <QPointF>
#include <QSize>
#include <QString>

namespace live2dstruct {
class CCanvas_info;

class CCanvasGeometry
{
public:
    static QSize PixelSize(double width, double height);
    static bool Validate(const CCanvas_info* canvas, QString* error = nullptr);
    static bool ModelToPixels(const CCanvas_info* canvas, QPointF model, QPointF& pixels);
};
}

#endif
