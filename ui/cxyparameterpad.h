#ifndef CXYPARAMETERPAD_H
#define CXYPARAMETERPAD_H

#include <QWidget>

class CXYParameterPad : public QWidget
{
    Q_OBJECT
public:
    explicit CXYParameterPad(QWidget* parent = nullptr);

    void SetRange(float xMinimum, float xMaximum, float yMinimum, float yMaximum);
    void SetValues(float xValue, float yValue);
    float XValue() const { return m_xValue; }
    float YValue() const { return m_yValue; }

signals:
    void ValuesChanged(float xValue, float yValue);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    void SetValuesFromPosition(const QPointF& position);
    QPointF PositionForValues() const;
    QRectF PlotRect() const;

    float m_xMinimum = -1.0f;
    float m_xMaximum = 1.0f;
    float m_yMinimum = -1.0f;
    float m_yMaximum = 1.0f;
    float m_xValue = 0.0f;
    float m_yValue = 0.0f;
    bool m_dragging = false;
};

#endif
