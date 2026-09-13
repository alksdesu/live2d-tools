#include "cmotioncurveview.h"

#include <QPainter>
#include <QPolygonF>
#include <QtMath>
#include <limits>

CMotionCurveView::CMotionCurveView(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("motionCurveView"));
    setMinimumHeight(150);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

void CMotionCurveView::SetTimeline(const CMotionTimeline* timeline)
{
    m_timeline = timeline;
    update();
}

void CMotionCurveView::SetTime(float time)
{
    m_time = qMax(0.0f, time);
    update();
}

void CMotionCurveView::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF plot = rect().adjusted(34, 16, -14, -22);
    const QColor border = palette().color(QPalette::Mid);
    const QColor text = palette().color(QPalette::Text);
    const QColor accent = palette().color(QPalette::Highlight);
    painter.fillRect(rect(), palette().color(QPalette::Base));
    painter.setPen(border);
    painter.drawRoundedRect(plot, 6, 6);
    painter.setPen(QColor(border.red(), border.green(), border.blue(), 100));
    for (int i = 1; i < 4; ++i) {
        const qreal x = plot.left() + plot.width() * i / 4.0;
        const qreal y = plot.top() + plot.height() * i / 4.0;
        painter.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
        painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
    }
    painter.setPen(text);
    painter.drawText(10, 14, QStringLiteral("曲线"));
    if (!m_timeline || m_timeline->Curves().isEmpty()) {
        painter.drawText(plot, Qt::AlignCenter, QStringLiteral("暂无参数曲线"));
        return;
    }
    const float duration = qMax(0.001f, m_timeline->Duration());
    float minimum = std::numeric_limits<float>::max();
    float maximum = std::numeric_limits<float>::lowest();
    for (const auto& points : m_timeline->Curves()) {
        for (const QPointF& point : points) {
            minimum = qMin(minimum, static_cast<float>(point.y()));
            maximum = qMax(maximum, static_cast<float>(point.y()));
        }
    }
    if (!qIsFinite(minimum) || !qIsFinite(maximum)) return;
    if (qFuzzyCompare(minimum + 1.0f, maximum + 1.0f)) maximum = minimum + 1.0f;
    const auto mapPoint = [&plot, duration, minimum, maximum](const QPointF& point) {
        return QPointF(plot.left() + plot.width() * point.x() / duration,
                       plot.bottom() - plot.height() * (point.y() - minimum) / (maximum - minimum));
    };
    const QVector<QColor> colors = {accent, QColor(220, 120, 70), QColor(80, 170, 125), QColor(170, 110, 210)};
    int colorIndex = 0;
    for (auto it = m_timeline->Curves().cbegin(); it != m_timeline->Curves().cend(); ++it, ++colorIndex) {
        const QColor color = colors[colorIndex % colors.size()];
        painter.setPen(QPen(color, 1.5));
        QPolygonF line;
        for (int sample = 0; sample <= 80; ++sample) {
            const float time = duration * sample / 80.0f;
            line.append(mapPoint(QPointF(time, m_timeline->Evaluate(it.key(), time))));
        }
        painter.drawPolyline(line);
        painter.setBrush(color);
        for (const QPointF& point : it.value()) painter.drawEllipse(mapPoint(point), 3.5, 3.5);
    }
    const qreal playheadX = plot.left() + plot.width() * qBound(0.0f, m_time / duration, 1.0f);
    painter.setPen(QPen(text, 1));
    painter.drawLine(QPointF(playheadX, plot.top()), QPointF(playheadX, plot.bottom()));
}
