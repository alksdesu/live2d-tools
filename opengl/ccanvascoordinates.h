#ifndef CCANVASCOORDINATES_H
#define CCANVASCOORDINATES_H

#include <QMatrix4x4>
#include <QPointF>
#include <QSize>
#include <QString>
#include <QVector>

class CCanvasCoordinates
{
public:
    static bool ScreenToModel(const QMatrix4x4& mvp, const QSize& viewSize,
                              const QPointF& logicalPixel, QPointF& model);
    static bool ModelToScreen(const QMatrix4x4& mvp, const QSize& viewSize,
                              const QPointF& model, QPointF& logicalPixel);
    static bool ProjectVertices(const QMatrix4x4& mvp, const QSize& viewSize, const QVector<float>& modelVertices,
        QVector<QPointF>& logicalPixels, QString* error = nullptr);
    static int NearestVertex(const QMatrix4x4& mvp, const QSize& viewSize,
                             const QPointF& logicalPixel, const QVector<float>& coreVertices,
                             double radiusPixels = 10.0);
};

#endif
