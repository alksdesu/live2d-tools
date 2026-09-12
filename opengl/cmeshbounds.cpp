#include "cmeshbounds.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace {
template <typename Transform>
QRectF bounds(const float* vertices, int vertexCount, Transform transform)
{
    if (!vertices || vertexCount <= 0) return {};
    double left = std::numeric_limits<double>::infinity();
    double top = left;
    double right = -left;
    double bottom = -left;
    for (int i = 0; i < vertexCount; ++i) {
        const QPointF point = transform(vertices[2 * i], vertices[2 * i + 1]);
        if (!std::isfinite(point.x()) || !std::isfinite(point.y())) return {};
        left = std::min(left, point.x());
        top = std::min(top, point.y());
        right = std::max(right, point.x());
        bottom = std::max(bottom, point.y());
    }
    return QRectF(QPointF(left, top), QPointF(right, bottom));
}
}

QRectF MeshBounds::Model(const float* vertices, int vertexCount)
{
    return bounds(vertices, vertexCount, [](float x, float y) { return QPointF(x, y); });
}

QRect MeshBounds::Viewport(const float* vertices, int vertexCount,
                           const std::function<QPointF(float, float)>& transform,
                           const QSize& viewport)
{
    if (viewport.isEmpty()) return {};
    const QRectF projected = bounds(vertices, vertexCount, [&](float x, float y) {
        const QPointF point = transform(x, y);
        return QPointF((point.x() + 1) * viewport.width() / 2,
                       (point.y() + 1) * viewport.height() / 2);
    });
    const QRectF clipped = projected.intersected(QRectF(QPointF(), QSizeF(viewport)));
    return clipped.isEmpty() ? QRect() : clipped.toAlignedRect();
}
