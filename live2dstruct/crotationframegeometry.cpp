#include "crotationframegeometry.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace live2dstruct {
namespace {
bool fail(QString* error, const QString& message)
{
    if (error) *error = message;
    return false;
}

bool finite(QPointF point)
{
    return std::isfinite(point.x()) && std::isfinite(point.y());
}

bool near(double actual, double expected)
{
    return std::abs(actual - expected) <= 0.00002 + 0.000002 * std::max(std::abs(actual), std::abs(expected));
}
}

bool CRotationFrameGeometry::Validate(const Frame& frame, QString* error)
{
    if (error) error->clear();
    if (!finite(frame.origin) || std::abs(frame.origin.x()) > std::numeric_limits<float>::max() ||
        std::abs(frame.origin.y()) > std::numeric_limits<float>::max() || !finite(frame.xAxis) || !finite(frame.yAxis) ||
        !std::isfinite(frame.scale) || frame.scale < 0 || frame.scale > std::numeric_limits<float>::max())
        return fail(error, QStringLiteral("旋转坐标系包含无效坐标或尺度"));
    const double xLength = std::hypot(frame.xAxis.x(), frame.xAxis.y());
    const double yLength = std::hypot(frame.yAxis.x(), frame.yAxis.y());
    const double dot = QPointF::dotProduct(frame.xAxis, frame.yAxis);
    if (!std::isfinite(xLength) || !std::isfinite(yLength) || !std::isfinite(dot) ||
        std::abs(xLength - 1) > 0.000000001 || std::abs(yLength - 1) > 0.000000001 || std::abs(dot) > 0.000000001)
        return fail(error, QStringLiteral("旋转坐标系需要正交的两个单位方向"));
    return true;
}

bool CRotationFrameGeometry::Mirror(const Frame& frame, QPointF linePoint, QPointF lineNormal,
                                    Frame& result, QString* error)
{
    if (!Validate(frame, error)) return false;
    if (!finite(linePoint) || !finite(lineNormal))
        return fail(error, QStringLiteral("镜像轴坐标无效"));
    const double maximum = std::max(std::abs(lineNormal.x()), std::abs(lineNormal.y()));
    if (maximum == 0) return fail(error, QStringLiteral("镜像轴法线不能为零"));
    lineNormal /= maximum;
    lineNormal /= std::hypot(lineNormal.x(), lineNormal.y());
    const auto reflect = [lineNormal](QPointF value) {
        return value - 2 * QPointF::dotProduct(value, lineNormal) * lineNormal;
    };
    Frame candidate = frame;
    const double distance = QPointF::dotProduct(frame.origin, lineNormal) - QPointF::dotProduct(linePoint, lineNormal);
    candidate.origin = frame.origin - 2 * distance * lineNormal;
    candidate.xAxis = reflect(frame.xAxis);
    candidate.yAxis = reflect(frame.yAxis);
    if (!Validate(candidate, error)) return false;
    result = candidate;
    return true;
}

bool CRotationFrameGeometry::Matches(const Frame& actual, const Frame& expected, QString* error)
{
    if (!Validate(actual, error) || !Validate(expected, error)) return false;
    if (!near(actual.origin.x(), expected.origin.x()) || !near(actual.origin.y(), expected.origin.y()) ||
        !near(actual.scale, expected.scale) ||
        std::hypot(actual.xAxis.x() - expected.xAxis.x(), actual.xAxis.y() - expected.xAxis.y()) > 0.000002 ||
        std::hypot(actual.yAxis.x() - expected.yAxis.x(), actual.yAxis.y() - expected.yAxis.y()) > 0.000002)
        return fail(error, QStringLiteral("旋转坐标系无法在模型精度内重建"));
    return true;
}
}
