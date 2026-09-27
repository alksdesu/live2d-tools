#include "cvertexselectionoverlay.h"
#include "ui/theme.h"
#include "cuvgeometry.h"
#include <QEvent>
#include <QFontMetricsF>
#include <QPainter>
#include <QPainterPath>
#include <QStyle>
#include <algorithm>
#include <cmath>

namespace {
bool finite(QPointF point) { return std::isfinite(point.x()) && std::isfinite(point.y()); }
bool fail(QString* error, const QString& message) { if (error) *error = message; return false; }
bool same(const CVertexSelectionOverlay::Frame& a, const CVertexSelectionOverlay::Frame& b)
{
    return a.viewport == b.viewport && a.points == b.points && a.weights == b.weights && a.hard == b.hard &&
        a.rectangle == b.rectangle && a.lasso == b.lasso && a.brushCenter == b.brushCenter &&
        a.brushRadius == b.brushRadius && a.brushHardness == b.brushHardness && a.brushOperation == b.brushOperation && a.drawPoints == b.drawPoints;
}
}

CVertexSelectionOverlay::CVertexSelectionOverlay(QWidget* parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("vertexSelectionOverlay")); setAccessibleName(QStringLiteral("点选择与影响预览"));
    setAttribute(Qt::WA_TransparentForMouseEvents); setAttribute(Qt::WA_NoSystemBackground); setAutoFillBackground(false); setFocusPolicy(Qt::NoFocus);
}
bool CVertexSelectionOverlay::SetFrame(const Frame& frame, QString* error)
{
    if (error) error->clear();
    if (!frame.viewport.isEmpty() && same(frame, m_frame)) return true;
    if (frame.viewport.isEmpty() || frame.viewport.width() > QWIDGETSIZE_MAX || frame.viewport.height() > QWIDGETSIZE_MAX ||
        frame.points.size() > 1000000 || frame.hard.size() > frame.points.size() || frame.lasso.size() > 4096 ||
        (!frame.weights.isEmpty() && frame.weights.size() != frame.points.size())) return fail(error, QStringLiteral("点选择覆盖层的数据尺寸无效"));
    for (const auto point : frame.points) if (!finite(point)) return fail(error, QStringLiteral("点选择覆盖层包含无效坐标"));
    for (const auto weight : frame.weights) if (!std::isfinite(weight) || weight < 0 || weight > 1) return fail(error, QStringLiteral("影响权重必须在 0 到 1 之间"));
    QVector<int> hard(frame.hard.cbegin(), frame.hard.cend());
    std::sort(hard.begin(), hard.end());
    int previous = -1;
    for (const int index : hard) {
        if (index <= previous || index < 0 || index >= frame.points.size() || (!frame.weights.isEmpty() && frame.weights[index] != 1))
            return fail(error, QStringLiteral("硬选区必须唯一，且权重为 1"));
        previous = index;
    }
    if (frame.rectangle && (!finite(frame.rectangle->topLeft()) || !finite(frame.rectangle->bottomRight()) ||
        !std::isfinite(frame.rectangle->width()) || !std::isfinite(frame.rectangle->height()))) return fail(error, QStringLiteral("框选区域无效"));
    for (const auto point : frame.lasso) if (!finite(point) || std::abs(point.x()) > 1e9 || std::abs(point.y()) > 1e9)
        return fail(error, QStringLiteral("套索预览坐标超出范围"));
    if (!std::isfinite(frame.brushRadius) || frame.brushRadius < 0 || frame.brushRadius > 1000000 ||
        !std::isfinite(frame.brushHardness) || frame.brushHardness < 0 || frame.brushHardness > 1 ||
        (frame.brushCenter && !finite(*frame.brushCenter)) ||
        (frame.brushOperation != live2dstruct::CVertexSelection::Combine::Replace && frame.brushOperation != live2dstruct::CVertexSelection::Combine::Add &&
         frame.brushOperation != live2dstruct::CVertexSelection::Combine::Subtract)) return fail(error, QStringLiteral("笔刷预览参数无效"));
    Frame owned = frame;
    owned.points = QVector<QPointF>(frame.points.cbegin(), frame.points.cend());
    owned.weights = QVector<double>(frame.weights.cbegin(), frame.weights.cend());
    owned.hard = QVector<int>(frame.hard.cbegin(), frame.hard.cend());
    owned.lasso = QVector<QPointF>(frame.lasso.cbegin(), frame.lasso.cend());
    m_frame = std::move(owned); m_sortedHard = std::move(hard); resize(frame.viewport); update(); return true;
}
void CVertexSelectionOverlay::Clear() { m_frame = {}; m_sortedHard.clear(); update(); }
void CVertexSelectionOverlay::paintEvent(QPaintEvent*)
{
    if (m_frame.viewport.isEmpty() || size() != m_frame.viewport) return;
    QPainter painter(this); painter.setRenderHint(QPainter::Antialiasing);
    const qreal stroke = std::max(1, style()->pixelMetric(QStyle::PM_DefaultFrameWidth, nullptr, this));
    const qreal mainStroke = std::max(stroke, 2.0 / devicePixelRatioF());
    const qreal radius = std::max(stroke * 2, QFontMetricsF(font()).height() / 4);
    const auto group = isEnabled() ? QPalette::Active : QPalette::Disabled;
    const QColor base = palette().color(group, QPalette::Window), ink = palette().color(group, QPalette::WindowText);
    const QColor accent(Theme::tokens(base.lightness() < 128).value(QStringLiteral("@accentInk@")));
    const QColor marked = base.lightness() < 128 ? base : palette().color(group, QPalette::HighlightedText);
    const QRectF visible(rect());
    if (m_frame.drawPoints) {
        qsizetype selected = 0;
        for (qsizetype index = 0; index < m_frame.points.size(); ++index) {
            const bool hard = selected < m_sortedHard.size() && m_sortedHard[selected] == index;
            if (hard) ++selected;
            const auto point = m_frame.points[index];
            if (!visible.adjusted(-radius * 3, -radius * 3, radius * 3, radius * 3).contains(point)) continue;
            const double weight = m_frame.weights.isEmpty() ? (hard ? 1 : 0) : m_frame.weights[index];
            const qreal extent = hard ? radius * 1.2 : weight > 0 ? radius * 1.45 : radius;
            painter.setPen(QPen(base, stroke * 3)); painter.setBrush(base); painter.drawEllipse(point, extent, extent);
            if (hard) {
                painter.setPen(QPen(marked, stroke)); painter.setBrush(accent); painter.drawEllipse(point, extent, extent);
            } else if (weight > 0) {
                auto color = accent; color.setAlphaF(.3 + .7 * weight);
                painter.setPen(QPen(color, stroke * (1 + weight))); painter.setBrush(Qt::NoBrush); painter.drawEllipse(point, extent, extent);
                const qreal center = radius * .6 * std::sqrt(weight);
                painter.setPen(Qt::NoPen); painter.setBrush(color); painter.drawEllipse(point, center, center);
            } else {
                painter.setPen(QPen(ink, stroke)); painter.setBrush(base); painter.drawEllipse(point, extent, extent);
            }
        }
    }
    const auto line = [&](QPointF first, QPointF second, Qt::PenStyle style) {
        if (!CUvGeometry::ClipSegment(first, second, visible)) return;
        painter.setBrush(Qt::NoBrush); painter.setPen(QPen(base, mainStroke * 3, style)); painter.drawLine(first, second);
        painter.setPen(QPen(accent, mainStroke, style)); painter.drawLine(first, second);
    };
    auto fill = accent; fill.setAlphaF(.1);
    if (m_frame.rectangle) {
        const auto area = m_frame.rectangle->normalized(); painter.fillRect(area.intersected(visible), fill);
        line(area.topLeft(), area.topRight(), Qt::DashLine); line(area.topRight(), area.bottomRight(), Qt::DashLine);
        line(area.bottomRight(), area.bottomLeft(), Qt::DashLine); line(area.bottomLeft(), area.topLeft(), Qt::DashLine);
    }
    if (m_frame.lasso.size() > 1) {
        QPainterPath path; path.setFillRule(Qt::OddEvenFill); path.moveTo(m_frame.lasso.first());
        for (qsizetype index = 1; index < m_frame.lasso.size(); ++index) path.lineTo(m_frame.lasso[index]);
        path.closeSubpath();
        painter.save(); painter.setClipRect(visible); painter.fillPath(path, fill); painter.restore();
        for (qsizetype index = 1; index < m_frame.lasso.size(); ++index) line(m_frame.lasso[index - 1], m_frame.lasso[index], Qt::SolidLine);
        line(m_frame.lasso.last(), m_frame.lasso.first(), Qt::DashLine);
    }
    if (m_frame.brushCenter && m_frame.brushRadius > 0) {
        const auto center = *m_frame.brushCenter; const double r = m_frame.brushRadius;
        if (center.x() >= -r && center.x() <= width() + r && center.y() >= -r && center.y() <= height() + r) {
            painter.setBrush(Qt::NoBrush); painter.setPen(QPen(base, mainStroke * 3)); painter.drawEllipse(center, r, r);
            painter.setPen(QPen(accent, mainStroke)); painter.drawEllipse(center, r, r);
            if (m_frame.brushHardness > 0 && m_frame.brushHardness < 1) { painter.setPen(QPen(accent, mainStroke, Qt::DotLine)); painter.drawEllipse(center, r * m_frame.brushHardness, r * m_frame.brushHardness); }
            const qreal arm = radius * .75;
            line(center - QPointF(arm, 0), center + QPointF(arm, 0), Qt::SolidLine);
            if (m_frame.brushOperation != live2dstruct::CVertexSelection::Combine::Subtract) line(center - QPointF(0, arm), center + QPointF(0, arm), Qt::SolidLine);
        }
    }
}
void CVertexSelectionOverlay::changeEvent(QEvent* event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange || event->type() == QEvent::PaletteChange || event->type() == QEvent::EnabledChange) update();
}
