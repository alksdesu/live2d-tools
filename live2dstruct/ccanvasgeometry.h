#ifndef CCANVASGEOMETRY_H
#define CCANVASGEOMETRY_H

#include <QPointF>
#include <QSize>
#include <QString>
#include <array>

namespace live2dstruct {
class CCanvas_info;

class CCanvasGeometry
{
public:
    static QSize PixelSize(double width, double height);
    static bool Validate(const CCanvas_info* canvas, QString* error = nullptr);
    static bool ModelToPixels(const CCanvas_info* canvas, QPointF model, QPointF& pixels);
    static bool PixelsToModel(const CCanvas_info* canvas, QPointF pixels, QPointF& model);
    static bool ModelToNdc(double width, double height, double originX, double originY,
                           double pixelsPerUnit, const QSize& pixelSize, std::array<float, 16>& matrix);
};
}

#endif
