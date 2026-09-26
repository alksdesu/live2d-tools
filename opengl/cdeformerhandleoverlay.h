#ifndef CDEFORMERHANDLEOVERLAY_H
#define CDEFORMERHANDLEOVERLAY_H

#include <QMatrix4x4>
#include <QPointF>
#include <QRectF>
#include <QVector>
#include <QWidget>
#include <optional>

class CDeformerHandleOverlay : public QWidget
{
    Q_OBJECT
public:
    enum Handle { None, WarpPoint, RotationOrigin, RotationAngle };
    Q_ENUM(Handle)
    struct Hit { Handle kind = None; int index = -1; };

    explicit CDeformerHandleOverlay(QWidget* parent = nullptr);
    bool SetWarp(const QVector<float>& points, const QMatrix4x4& mvp, const QSize& viewport);
    bool SetRotation(const QPointF& origin, const QPointF& direction, const QMatrix4x4& mvp, const QSize& viewport);
    void Clear();
    void SetSelected(QVector<int> selected);
    QVector<int> Selected() const;
    Hit HitTest(const QPointF& logicalPixel) const;
    QVector<int> PointsInRect(const QRectF& rectangle) const;
    void SetSelectionRect(std::optional<QRectF> rectangle);
    QPointF OriginPixel() const;
    QPointF AnglePixel() const;
    QVector<QPointF> PointPixels() const;

protected:
    void paintEvent(QPaintEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    struct Metrics { qreal point, origin, angle, hit, arm, stroke; };
    Metrics Measure() const;
    bool Ready() const;
    Handle m_mode = None;
    QSize m_viewport;
    QVector<QPointF> m_points;
    QVector<int> m_selected;
    QPointF m_origin;
    QPointF m_direction;
    std::optional<QRectF> m_rectangle;
};

#endif
