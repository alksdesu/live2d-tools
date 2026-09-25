#include "ccanvasgeometry.h"
#include "struct/ccanvas_info.h"

#include <cmath>
#include <limits>

namespace live2dstruct {
namespace {
bool fail(QString* error, const QString& message)
{
    if (error) *error = message;
    return false;
}
}

QSize CCanvasGeometry::PixelSize(double width, double height)
{
    const double maximum = std::numeric_limits<int>::max();
    if (!std::isfinite(width) || !std::isfinite(height) || width <= 0.0 || height <= 0.0 ||
        width > maximum || height > maximum) return {};
    const double roundedWidth = std::round(width), roundedHeight = std::round(height);
    if (roundedWidth < 1.0 || roundedHeight < 1.0 || roundedWidth > maximum || roundedHeight > maximum) return {};
    return QSize(static_cast<int>(roundedWidth), static_cast<int>(roundedHeight));
}

bool CCanvasGeometry::Validate(const CCanvas_info* canvas, QString* error)
{
    if (error) error->clear();
    if (!canvas) return fail(error, QStringLiteral("moc3 缺少 canvas_info"));
    struct Field { const char* name; float value; };
    const Field fields[]{
        {"pixels_per_unit", canvas->pixels_per_unit},
        {"origin_x", canvas->origin_x},
        {"origin_y", canvas->origin_y},
        {"width", canvas->width},
        {"height", canvas->height},
    };
    for (const Field& field : fields) {
        if (!std::isfinite(field.value))
            return fail(error, QStringLiteral("moc3 canvas_info.%1 不是有限数值").arg(QString::fromLatin1(field.name)));
    }
    if (canvas->pixels_per_unit <= 0.0f)
        return fail(error, QStringLiteral("moc3 canvas_info.pixels_per_unit 必须大于零"));
    if (PixelSize(canvas->width, canvas->height).isEmpty())
        return fail(error, QStringLiteral("moc3 canvas_info.width/height 无法转换为有效像素尺寸"));
    return true;
}

bool CCanvasGeometry::ModelToPixels(const CCanvas_info* canvas, QPointF model, QPointF& pixels)
{
    if (!std::isfinite(model.x()) || !std::isfinite(model.y()) || !Validate(canvas)) return false;
    const double ppu = canvas->pixels_per_unit;
    const double x = model.x() * ppu + canvas->origin_x;
    const double y = canvas->origin_y - model.y() * ppu;
    if (!std::isfinite(x) || !std::isfinite(y)) return false;
    pixels = QPointF(x, y);
    return true;
}
}
