#include "cphysicsgraph.h"

#include <QJsonArray>
#include <QPainter>

CPhysicsGraphWidget::CPhysicsGraphWidget(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("physicsGraph"));
    setMinimumHeight(150);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

void CPhysicsGraphWidget::SetGroup(const QJsonObject& group)
{
    m_inputs = group.value(QStringLiteral("Input")).toArray();
    m_outputs = group.value(QStringLiteral("Output")).toArray();
    m_vertices = group.value(QStringLiteral("Vertices")).toArray();
    update();
}

void CPhysicsGraphWidget::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QColor surface = palette().color(QPalette::Base);
    const QColor border = palette().color(QPalette::Mid);
    const QColor text = palette().color(QPalette::Text);
    const QColor accent = palette().color(QPalette::Highlight);
    painter.fillRect(rect(), surface);
    painter.setPen(border);
    painter.drawRoundedRect(rect().adjusted(1, 1, -1, -1), 6, 6);
    painter.setPen(text);
    painter.drawText(12, 20, QStringLiteral("物理关系"));

    const int centerX = width() / 2;
    const int top = 38;
    const int availableHeight = qMax(1, height() - top - 18);
    const int inputStep = m_inputs.isEmpty() ? availableHeight : availableHeight / (m_inputs.size() + 1);
    const int outputStep = m_outputs.isEmpty() ? availableHeight : availableHeight / (m_outputs.size() + 1);
    painter.setPen(QPen(accent, 1.5));
    for (int i = 0; i < m_inputs.size(); ++i) {
        const QPoint point(32, top + inputStep * (i + 1));
        painter.drawLine(point, QPoint(centerX - 18, height() / 2));
        painter.setBrush(accent);
        painter.drawEllipse(point, 4, 4);
        painter.setPen(text);
        painter.drawText(point.x() + 9, point.y() + 4,
                         m_inputs.at(i).toObject().value(QStringLiteral("Type")).toString());
        painter.setPen(QPen(accent, 1.5));
    }
    for (int i = 0; i < m_outputs.size(); ++i) {
        const QPoint point(width() - 32, top + outputStep * (i + 1));
        painter.drawLine(QPoint(centerX + 18, height() / 2), point);
        painter.setBrush(accent);
        painter.drawEllipse(point, 4, 4);
        painter.setPen(text);
        painter.drawText(point.x() - 70, point.y() + 4,
                         m_outputs.at(i).toObject().value(QStringLiteral("Type")).toString());
        painter.setPen(QPen(accent, 1.5));
    }
    painter.setBrush(accent);
    painter.drawEllipse(QPoint(centerX, height() / 2), 7, 7);
    if (m_vertices.size() > 1) {
        const int startX = centerX - (m_vertices.size() - 2) * 14;
        const int y = height() - 20;
        painter.setPen(QPen(border, 2));
        QPoint previous(startX, y);
        for (int i = 1; i < m_vertices.size(); ++i) {
            const QPoint current(startX + (i - 1) * 28, y + qMin(10, i * 2));
            painter.drawLine(previous, current);
            painter.setBrush(border);
            painter.drawEllipse(current, 3, 3);
            previous = current;
        }
    }
}
