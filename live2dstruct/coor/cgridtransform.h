#ifndef CGRIDTRANSFORM_H
#define CGRIDTRANSFORM_H

#include <QPointF>
#include <QRectF>
#include <QVector2D>
#include <vector>

namespace live2dstruct {

class CGridTransform
{
public:
    CGridTransform(QRectF rect, const std::vector<float>& points, int columns, int rows);
    bool IsValid() const { return m_valid; }
    bool inverseTransform(const float* source, int sourceOffset, float* destination,
                          int destinationOffset, int count, bool quad) const;
    bool inverseTransform(const QVector2D& source, QVector2D& destination, bool quad,
                          const QVector2D* preferred = nullptr) const;

private:
    QPointF GridPoint(int column, int row) const;
    bool InversePoint(const QPointF& source, QPointF& destination, bool quad,
                      const QPointF* preferred) const;
    int m_columns;
    int m_rows;
    QRectF m_rect;
    std::vector<QPointF> m_points;
    QPointF m_origin;
    QPointF m_axisX;
    QPointF m_axisY;
    bool m_valid = false;
};

}

#endif
