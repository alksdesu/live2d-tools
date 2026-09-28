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
    return ProjectViewport(vertices, vertexCount, transform, viewport).rect;
}

MeshBounds::Projection MeshBounds::ProjectViewport(const float* vertices, int vertexCount,
    const std::function<QPointF(float, float)>& transform, const QSize& viewport)
{
    if (viewport.isEmpty() || !transform || vertexCount < 0 || vertexCount > 1000000 ||
        (vertexCount && !vertices)) return {};
    if (!vertexCount) return {{}, ProjectionState::NoVertices};
    double left = std::numeric_limits<double>::infinity(), top = left;
    double right = -left, bottom = -left;
    for (int i = 0; i < vertexCount; ++i) {
        if (!std::isfinite(vertices[2 * i]) || !std::isfinite(vertices[2 * i + 1])) return {};
        const QPointF point = transform(vertices[2 * i], vertices[2 * i + 1]);
        const double x = (point.x() + 1) * viewport.width() / 2;
        const double y = (point.y() + 1) * viewport.height() / 2;
        if (!std::isfinite(x) || !std::isfinite(y)) return {};
        left = std::min(left, x); top = std::min(top, y);
        right = std::max(right, x); bottom = std::max(bottom, y);
    }
    if (left == right || top == bottom) return {{}, ProjectionState::DegenerateBounds};
    if (right <= 0 || bottom <= 0 || left >= viewport.width() || top >= viewport.height())
        return {{}, ProjectionState::OutsideViewport};
    const QRectF clipped(QPointF(std::max(0.0, left), std::max(0.0, top)),
                         QPointF(std::min(double(viewport.width()), right), std::min(double(viewport.height()), bottom)));
    return {clipped.toAlignedRect(), ProjectionState::Pixels};
}
