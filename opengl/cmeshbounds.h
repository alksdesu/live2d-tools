#ifndef CMESHBOUNDS_H
#define CMESHBOUNDS_H

#include <QRectF>
#include <functional>

namespace MeshBounds {
enum class ProjectionState { Pixels, NoVertices, DegenerateBounds, OutsideViewport, Invalid };
struct Projection {
    QRect rect;
    ProjectionState state = ProjectionState::Invalid;
};
QRectF Model(const float* vertices, int vertexCount);
Projection ProjectViewport(const float* vertices, int vertexCount,
                           const std::function<QPointF(float, float)>& transform,
                           const QSize& viewport);
QRect Viewport(const float* vertices, int vertexCount,
               const std::function<QPointF(float, float)>& transform,
               const QSize& viewport);
}

#endif
