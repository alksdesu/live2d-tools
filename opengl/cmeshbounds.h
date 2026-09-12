#ifndef CMESHBOUNDS_H
#define CMESHBOUNDS_H

#include <QRectF>
#include <functional>

namespace MeshBounds {
QRectF Model(const float* vertices, int vertexCount);
QRect Viewport(const float* vertices, int vertexCount,
               const std::function<QPointF(float, float)>& transform,
               const QSize& viewport);
}

#endif
