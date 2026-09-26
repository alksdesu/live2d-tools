#include "cphysicsgraph.h"

#include <QPainter>
#include <QPolygonF>
#include <QStyle>
#include <cmath>
#include <algorithm>

CPhysicsGraphWidget::CPhysicsGraphWidget(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("physicsGraph"));
    setMinimumHeight(fontMetrics().height() * 12);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setAccessibleName(QStringLiteral("物理摆锤与通道曲线"));
}

void CPhysicsGraphWidget::SetGroup(const QJsonObject& group)
{
    m_inputs = group.value(QStringLiteral("Input")).toArray();
    m_outputs = group.value(QStringLiteral("Output")).toArray();
    m_vertices = group.value(QStringLiteral("Vertices")).toArray();
    update();
}

void CPhysicsGraphWidget::ClearObservation()
{
    m_observation = {};
    m_history.clear();
    update();
}

void CPhysicsGraphWidget::SetChannel(bool output, int row)
{
    if (m_outputChannel == output && m_channelRow == row) return;
    m_outputChannel = output;
    m_channelRow = row;
    m_history.clear();
    update();
}

void CPhysicsGraphWidget::SetObservation(const Live2D::Cubism::Framework::PhysicsObservation& sample)
{
    const bool same = m_observation.generation == sample.generation && m_observation.groupIndex == sample.groupIndex &&
        m_observation.groupId == sample.groupId && m_observation.sourceDigest == sample.sourceDigest;
    if (same && m_observation.sampled && sample.sampled && sample.sequence <= m_observation.sequence) return;
    if (!same || !sample.sampled || sample.time < m_observation.time) m_history.clear();
    m_observation = sample;
    const Live2D::Cubism::Framework::PhysicsInputObservation* channel = nullptr;
    if (m_outputChannel && m_channelRow >= 0 && m_channelRow < sample.outputs.size()) channel = &sample.outputs[m_channelRow];
    if (!m_outputChannel && m_channelRow >= 0 && m_channelRow < sample.inputs.size()) channel = &sample.inputs[m_channelRow];
    if (sample.sampled && channel && channel->active && channel->sampled && std::isfinite(channel->value) && std::isfinite(sample.time)) {
        if (m_history.size() == HistoryCapacity) m_history.removeFirst();
        m_history.append(QPointF(sample.time, channel->value));
    }
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
    const qreal line = fontMetrics().height();
    const qreal margin = qMax(1, style()->pixelMetric(QStyle::PM_LayoutLeftMargin));
    const QRectF area = QRectF(rect()).adjusted(margin, margin, -margin, -margin);
    painter.fillRect(rect(), surface);
    painter.setPen(text);
    if (!m_observation.sampled) {
        painter.drawText(area, Qt::AlignTop | Qt::AlignLeft, QStringLiteral("配置关系 · 尚未实时"));
        const QPointF center(area.center().x(), area.center().y());
        const QPointF left(area.left() + line, center.y()), right(area.right() - line, center.y());
        painter.setPen(QPen(accent, 1.5));
        painter.drawLine(left, center); painter.drawLine(center, right);
        painter.setBrush(accent); painter.drawEllipse(center, line / 3, line / 3);
        painter.drawEllipse(left, line / 5, line / 5); painter.drawEllipse(right, line / 5, line / 5);
        painter.setPen(text);
        painter.drawText(QRectF(area.left(), center.y() - 2 * line, area.width() / 2, line), Qt::AlignLeft, QStringLiteral("输入 %1").arg(m_inputs.size()));
        painter.drawText(QRectF(center.x(), center.y() - 2 * line, area.width() / 2, line), Qt::AlignRight, QStringLiteral("输出 %1").arg(m_outputs.size()));
        painter.drawText(QRectF(area.left(), center.y() + line, area.width(), 2 * line), Qt::AlignCenter, QStringLiteral("%1 个摆锤节点\n等待运行时物理样本").arg(m_vertices.size()));
        return;
    }
    painter.drawText(area, Qt::AlignTop | Qt::AlignLeft, QStringLiteral("实时摆锤"));
    QRectF pendulum = area.adjusted(line / 2, line * 1.5, -line / 2, -area.height() * .4);
    if (!m_observation.particles.isEmpty() && pendulum.width() > 0 && pendulum.height() > 0) {
        double magnitude = 1;
        for (const auto& point : m_observation.particles) {
            if (!std::isfinite(point.x()) || !std::isfinite(point.y())) return;
            magnitude = std::max({magnitude, std::abs(point.x()), std::abs(point.y())});
        }
        double minX = 1, minY = 1, maxX = -1, maxY = -1;
        for (const auto& point : m_observation.particles) {
            minX = std::min(minX, point.x() / magnitude); maxX = std::max(maxX, point.x() / magnitude);
            minY = std::min(minY, point.y() / magnitude); maxY = std::max(maxY, point.y() / magnitude);
        }
        const double extent = std::max(maxX - minX, maxY - minY);
        const double scale = extent > 0 ? std::min(pendulum.width(), pendulum.height()) / extent : 1;
        QPolygonF points; points.reserve(m_observation.particles.size());
        for (const auto& point : m_observation.particles)
            points.append(pendulum.center() + QPointF((point.x() / magnitude - (minX + maxX) / 2) * scale,
                                                     (point.y() / magnitude - (minY + maxY) / 2) * scale));
        painter.setPen(QPen(accent, 1.5)); painter.drawPolyline(points);
        painter.setBrush(accent);
        const int markerCount = qMax(2, int(pendulum.width() / line));
        const int stride = qMax(1, int((points.size() + markerCount - 1) / markerCount));
        for (int i = 0; i < points.size(); i += stride) painter.drawEllipse(points[i], line / 5, line / 5);
        painter.drawEllipse(points.last(), line / 5, line / 5);
        painter.setBrush(surface); painter.drawEllipse(points.first(), line / 3, line / 3);
    }
    const QRectF chart(area.left(), area.bottom() - area.height() * .3, area.width(), area.height() * .3);
    painter.setPen(border); painter.drawLine(chart.topLeft(), chart.topRight());
    painter.setPen(text);
    if (m_history.isEmpty()) {
        painter.drawText(chart, Qt::AlignCenter, QStringLiteral("选择通道后记录趋势"));
        return;
    }
    double low = m_history.first().y(), high = low;
    for (const auto& point : m_history) { low = std::min(low, point.y()); high = std::max(high, point.y()); }
    const auto limits = QStringLiteral("%1 … %2").arg(QString::number(low, 'g', 5), QString::number(high, 'g', 5));
    painter.drawText(chart, Qt::AlignTop | Qt::AlignLeft, limits);
    const QRectF plot = chart.adjusted(0, line * 1.3, 0, -line / 3);
    if (plot.height() <= 0) return;
    const double duration = m_history.last().x() - m_history.first().x();
    QPolygonF points; points.reserve(m_history.size());
    for (int i = 0; i < m_history.size(); ++i) {
        const double x = duration > 0 ? (m_history[i].x() - m_history.first().x()) / duration : .5;
        const double y = high > low ? (m_history[i].y() - low) / (high - low) : .5;
        points.append(QPointF(plot.left() + x * plot.width(), plot.bottom() - y * plot.height()));
    }
    painter.setPen(QPen(accent, 1.5)); painter.drawPolyline(points);
    painter.setBrush(accent); painter.drawEllipse(points.last(), line / 6, line / 6);
}
