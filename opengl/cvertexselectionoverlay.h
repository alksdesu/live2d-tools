#ifndef CVERTEXSELECTIONOVERLAY_H
#define CVERTEXSELECTIONOVERLAY_H

#include "live2dstruct/cvertexselection.h"
#include <QRectF>
#include <QWidget>
#include <optional>

class CVertexSelectionOverlay : public QWidget
{
    Q_OBJECT
public:
    struct Frame {
        QSize viewport;
        QVector<QPointF> points;
        QVector<double> weights;
        QVector<int> hard;
        std::optional<QRectF> rectangle;
        QVector<QPointF> lasso;
        std::optional<QPointF> brushCenter;
        double brushRadius = 0;
        double brushHardness = .5;
        live2dstruct::CVertexSelection::Combine brushOperation = live2dstruct::CVertexSelection::Combine::Replace;
        bool drawPoints = true;
    };
    explicit CVertexSelectionOverlay(QWidget* parent = nullptr);
    bool SetFrame(const Frame& frame, QString* error = nullptr);
    void Clear();
    const Frame& CurrentFrame() const { return m_frame; }

protected:
    void paintEvent(QPaintEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    Frame m_frame;
    QVector<int> m_sortedHard;
};

#endif
