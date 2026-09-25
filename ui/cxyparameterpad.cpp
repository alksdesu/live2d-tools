#include "cxyparameterpad.h"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QtMath>

CXYParameterPad::CXYParameterPad(QWidget* parent)
    : QWidget(parent)
{
    setMinimumSize(132, 104);
    setFocusPolicy(Qt::StrongFocus);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

void CXYParameterPad::SetRange(float xMinimum, float xMaximum, float yMinimum, float yMaximum)
{
    if (!qIsFinite(xMinimum) || !qIsFinite(xMaximum) || !qIsFinite(yMinimum) || !qIsFinite(yMaximum) ||
        xMinimum > xMaximum || yMinimum > yMaximum) return;
    m_xMinimum = xMinimum;
    m_xMaximum = xMaximum;
    m_yMinimum = yMinimum;
    m_yMaximum = yMaximum;
    SetValues(m_xValue, m_yValue);
}

void CXYParameterPad::SetValues(float xValue, float yValue)
{
    if (!qIsFinite(xValue) || !qIsFinite(yValue)) return;
    const float nextX = qBound(m_xMinimum, xValue, m_xMaximum);
    const float nextY = qBound(m_yMinimum, yValue, m_yMaximum);
    if (nextX == m_xValue && nextY == m_yValue) {
        update();
        return;
    }
    m_xValue = nextX;
    m_yValue = nextY;
    update();
    emit ValuesChanged(m_xValue, m_yValue);
}

QRectF CXYParameterPad::PlotRect() const
{
    return rect().adjusted(14, 10, -14, -14);
}

QPointF CXYParameterPad::PositionForValues() const
{
    const QRectF plot = PlotRect();
    const double xRatio = m_xMaximum > m_xMinimum
        ? (double(m_xValue) - m_xMinimum) / (double(m_xMaximum) - m_xMinimum) : 0.5;
    const double yRatio = m_yMaximum > m_yMinimum
        ? (double(m_yValue) - m_yMinimum) / (double(m_yMaximum) - m_yMinimum) : 0.5;
    return {plot.left() + plot.width() * xRatio, plot.bottom() - plot.height() * yRatio};
}

void CXYParameterPad::SetValuesFromPosition(const QPointF& position)
{
    const QRectF plot = PlotRect();
    if (!qIsFinite(position.x()) || !qIsFinite(position.y()) || plot.width() <= 0 || plot.height() <= 0) return;
    const double xRatio = qBound(0.0, (position.x() - plot.left()) / plot.width(), 1.0);
    const double yRatio = qBound(0.0, (plot.bottom() - position.y()) / plot.height(), 1.0);
    SetValues(float(double(m_xMinimum) + (double(m_xMaximum) - m_xMinimum) * xRatio),
              float(double(m_yMinimum) + (double(m_yMaximum) - m_yMinimum) * yRatio));
}

void CXYParameterPad::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF plot = PlotRect();
    const QColor base = palette().color(QPalette::Base);
    const QColor border = palette().color(QPalette::Mid);
    const QColor accent = palette().color(QPalette::Highlight);
    painter.setPen(border);
    painter.setBrush(base);
    painter.drawRoundedRect(plot, 6, 6);
    painter.setPen(QColor(border.red(), border.green(), border.blue(), 130));
    painter.drawLine(QPointF(plot.center().x(), plot.top()), QPointF(plot.center().x(), plot.bottom()));
    painter.drawLine(QPointF(plot.left(), plot.center().y()), QPointF(plot.right(), plot.center().y()));
    painter.setPen(accent);
    painter.setBrush(accent);
    painter.drawEllipse(PositionForValues(), 5.0, 5.0);
    painter.setPen(palette().color(QPalette::Text));
    painter.drawText(QRectF(0, 0, width(), 12), Qt::AlignCenter, QStringLiteral("XY"));
}

void CXYParameterPad::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton || !PlotRect().contains(event->position())) return;
    m_dragging = true;
    setFocus(Qt::MouseFocusReason);
    SetValuesFromPosition(event->position());
}

void CXYParameterPad::mouseMoveEvent(QMouseEvent* event)
{
    if (m_dragging && (event->buttons() & Qt::LeftButton)) SetValuesFromPosition(event->position());
}

void CXYParameterPad::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) m_dragging = false;
    QWidget::mouseReleaseEvent(event);
}

void CXYParameterPad::keyPressEvent(QKeyEvent* event)
{
    const double divisor = event->modifiers() & Qt::ShiftModifier ? 20.0 : 100.0;
    const double xStep = (double(m_xMaximum) - m_xMinimum) / divisor;
    const double yStep = (double(m_yMaximum) - m_yMinimum) / divisor;
    double x = m_xValue;
    double y = m_yValue;
    if (event->key() == Qt::Key_Left) x -= xStep;
    else if (event->key() == Qt::Key_Right) x += xStep;
    else if (event->key() == Qt::Key_Down) y -= yStep;
    else if (event->key() == Qt::Key_Up) y += yStep;
    else { QWidget::keyPressEvent(event); return; }
    SetValues(float(qBound(double(m_xMinimum), x, double(m_xMaximum))),
              float(qBound(double(m_yMinimum), y, double(m_yMaximum))));
    event->accept();
}
