#include "cgridtransform.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace live2dstruct {
namespace {
constexpr int kMaxPoints = 1000000;
constexpr double kCoordinateTolerance = 8.0 * std::numeric_limits<float>::epsilon();
constexpr double kArithmeticTolerance = 64.0 * std::numeric_limits<double>::epsilon();

bool finite(const QPointF& point)
{
    return std::isfinite(point.x()) && std::isfinite(point.y());
}

bool representable(const QPointF& point)
{
    return finite(point) && std::abs(point.x()) <= std::numeric_limits<float>::max() &&
           std::abs(point.y()) <= std::numeric_limits<float>::max();
}

double cross(const QPointF& a, const QPointF& b)
{
    return a.x() * b.y() - a.y() * b.x();
}

double dot(const QPointF& a, const QPointF& b)
{
    return a.x() * b.x() + a.y() * b.y();
}

bool invertBasis(const QPointF& x, const QPointF& y, const QPointF& delta, QPointF& result)
{
    const double determinant = cross(x, y);
    const double scale = std::abs(x.x() * y.y()) + std::abs(x.y() * y.x());
    if (std::abs(determinant) <= kArithmeticTolerance * scale) return false;
    result = QPointF(cross(delta, y) / determinant, cross(x, delta) / determinant);
    return finite(result);
}

bool inUnit(double value)
{
    return std::isfinite(value) && value >= -kCoordinateTolerance && value <= 1.0 + kCoordinateTolerance;
}

bool invertTriangle(const QPointF& point, const QPointF& a, const QPointF& b,
                    const QPointF& c, QPointF& result)
{
    if (!invertBasis(b - a, c - a, point - a, result)) return false;
    if (!inUnit(result.x()) || !inUnit(result.y()) || result.x() + result.y() > 1.0 + kCoordinateTolerance)
        return false;
    result.setX(std::clamp(result.x(), 0.0, 1.0));
    result.setY(std::clamp(result.y(), 0.0, 1.0));
    return true;
}

int invertQuad(const QPointF& point, const QPointF& a, const QPointF& b,
               const QPointF& c, const QPointF& d, std::array<QPointF, 2>& results)
{
    const QPointF x = b - a;
    const QPointF y = c - a;
    const QPointF bend = d - b - c + a;
    const QPointF delta = point - a;
    const double qa = -cross(x, bend);
    const double qb = cross(delta, bend) - cross(x, y);
    const double qc = cross(delta, y);
    const double scale = std::max({std::abs(qa), std::abs(qb), std::abs(qc)});
    std::array<double, 2> roots{};
    int rootCount = 0;
    if (std::abs(qa) <= kArithmeticTolerance * scale) {
        if (std::abs(qb) <= kArithmeticTolerance * scale) return 0;
        roots[rootCount++] = -qc / qb;
    } else {
        double discriminant = qb * qb - 4.0 * qa * qc;
        const double error = kArithmeticTolerance * (qb * qb + std::abs(4.0 * qa * qc));
        if (discriminant < -error) return 0;
        discriminant = std::max(0.0, discriminant);
        const double q = -0.5 * (qb + std::copysign(std::sqrt(discriminant), qb));
        if (q == 0.0) roots[rootCount++] = -qb / (2.0 * qa);
        else {
            roots[rootCount++] = q / qa;
            if (discriminant > 0.0) roots[rootCount++] = qc / q;
        }
    }
    int count = 0;
    for (int index = 0; index < rootCount; ++index) {
        const double u = roots[index];
        if (!inUnit(u)) continue;
        const QPointF vertical = y + bend * u;
        const double lengthSquared = dot(vertical, vertical);
        if (lengthSquared == 0.0) continue;
        const double v = dot(delta - x * u, vertical) / lengthSquared;
        if (!inUnit(v)) continue;
        QPointF inverse;
        if (!invertBasis(x + bend * v, vertical, delta, inverse)) continue;
        const QPointF residual = x * u + y * v + bend * (u * v) - delta;
        const double extent = std::max({dot(x, x), dot(y, y), dot(bend, bend), dot(delta, delta)});
        if (dot(residual, residual) > kCoordinateTolerance * kCoordinateTolerance * extent) continue;
        results[count++] = QPointF(std::clamp(u, 0.0, 1.0), std::clamp(v, 0.0, 1.0));
    }
    return count;
}

double gridCoordinate(int index, int divisions)
{
    if (index < 0) return -2.0;
    if (index > divisions) return 3.0;
    return static_cast<double>(index) / divisions;
}
}

CGridTransform::CGridTransform(QRectF rect, const std::vector<float>& points, int columns, int rows)
    : m_columns(columns), m_rows(rows), m_rect(rect)
{
    if (columns <= 0 || rows <= 0 || columns >= kMaxPoints || rows >= kMaxPoints ||
        !finite(rect.topLeft()) || !std::isfinite(rect.width()) || !std::isfinite(rect.height()) ||
        rect.width() == 0.0 || rect.height() == 0.0) return;
    const qint64 count = (static_cast<qint64>(columns) + 1) * (static_cast<qint64>(rows) + 1);
    if (count > kMaxPoints || static_cast<quint64>(count * 2) != points.size()) return;
    for (float value : points) if (!std::isfinite(value)) return;
    m_points.reserve(static_cast<size_t>(count));
    for (size_t index = 0; index < points.size(); index += 2)
        m_points.emplace_back(points[index], points[index + 1]);
    const QPointF a = m_points.front();
    const QPointF b = m_points[columns];
    const QPointF c = m_points[static_cast<size_t>(rows) * (columns + 1)];
    const QPointF d = m_points.back();
    m_axisX = (d - a + b - c) * 0.5;
    m_axisY = (d - a - b + c) * 0.5;
    m_origin = (a + b + c + d) * 0.25 - (m_axisX + m_axisY) * 0.5;
    m_valid = true;
}

QPointF CGridTransform::GridPoint(int column, int row) const
{
    if (column >= 0 && column <= m_columns && row >= 0 && row <= m_rows)
        return m_points[static_cast<size_t>(row) * (m_columns + 1) + column];
    return m_origin + m_axisX * gridCoordinate(column, m_columns) + m_axisY * gridCoordinate(row, m_rows);
}

bool CGridTransform::InversePoint(const QPointF& source, QPointF& destination, bool quad,
                                  const QPointF* preferred) const
{
    if (!m_valid || !finite(source) || (preferred && !finite(*preferred))) return false;
    QPointF best;
    bool found = false;
    double bestDistance = 0.0;
    const auto accept = [&](QPointF normalized) {
        const QPointF local(m_rect.x() + m_rect.width() * normalized.x(),
                            m_rect.y() + m_rect.height() * normalized.y());
        if (!representable(local)) return true;
        if (!found) {
            best = normalized;
            found = true;
            if (preferred) bestDistance = dot(local - *preferred, local - *preferred);
            return true;
        }
        if (dot(normalized - best, normalized - best) <= kCoordinateTolerance * kCoordinateTolerance) return true;
        if (!preferred) return false;
        const double distance = dot(local - *preferred, local - *preferred);
        if (distance < bestDistance) {
            best = normalized;
            bestDistance = distance;
        }
        return true;
    };
    for (int row = -1; row <= m_rows; ++row) {
        for (int column = -1; column <= m_columns; ++column) {
            const QPointF a = GridPoint(column, row);
            const QPointF b = GridPoint(column + 1, row);
            const QPointF c = GridPoint(column, row + 1);
            const QPointF d = GridPoint(column + 1, row + 1);
            const double left = std::min({a.x(), b.x(), c.x(), d.x()});
            const double right = std::max({a.x(), b.x(), c.x(), d.x()});
            const double top = std::min({a.y(), b.y(), c.y(), d.y()});
            const double bottom = std::max({a.y(), b.y(), c.y(), d.y()});
            const double margin = kCoordinateTolerance * std::max(right - left, bottom - top);
            if (source.x() < left - margin || source.x() > right + margin ||
                source.y() < top - margin || source.y() > bottom + margin) continue;
            std::array<QPointF, 2> candidates;
            int count = 0;
            if (quad && column >= 0 && column < m_columns && row >= 0 && row < m_rows) {
                count = invertQuad(source, a, b, c, d, candidates);
            } else {
                QPointF candidate;
                if (invertTriangle(source, a, b, c, candidate)) candidates[count++] = candidate;
                if (invertTriangle(source, d, c, b, candidate))
                    candidates[count++] = QPointF(1.0 - candidate.x(), 1.0 - candidate.y());
            }
            const double u = gridCoordinate(column, m_columns);
            const double v = gridCoordinate(row, m_rows);
            const double width = gridCoordinate(column + 1, m_columns) - u;
            const double height = gridCoordinate(row + 1, m_rows) - v;
            for (int index = 0; index < count; ++index) {
                if (!accept(QPointF(u + width * candidates[index].x(), v + height * candidates[index].y())))
                    return false;
            }
        }
    }
    QPointF outside;
    if (invertBasis(m_axisX, m_axisY, source - m_origin, outside) &&
        (outside.x() <= -2.0 || outside.x() >= 3.0 || outside.y() <= -2.0 || outside.y() >= 3.0)) {
        if (!accept(outside)) return false;
    }
    if (!found) return false;
    destination = QPointF(m_rect.x() + m_rect.width() * best.x(), m_rect.y() + m_rect.height() * best.y());
    return true;
}

bool CGridTransform::inverseTransform(const QVector2D& source, QVector2D& destination, bool quad,
                                      const QVector2D* preferred) const
{
    QPointF result;
    const QPointF hint = preferred ? preferred->toPointF() : QPointF();
    if (!InversePoint(source.toPointF(), result, quad, preferred ? &hint : nullptr)) return false;
    destination = QVector2D(result);
    return true;
}

bool CGridTransform::inverseTransform(const float* source, int sourceOffset, float* destination,
                                      int destinationOffset, int count, bool quad) const
{
    if (!m_valid || sourceOffset < 0 || destinationOffset < 0 || count < 0 || count > kMaxPoints ||
        static_cast<qint64>(std::max(sourceOffset, destinationOffset)) + static_cast<qint64>(count) * 2 >
            std::numeric_limits<int>::max()) return false;
    if (count == 0) return true;
    if (!source || !destination) return false;
    std::vector<QPointF> results;
    results.reserve(count);
    for (int index = 0; index < count; ++index) {
        const int offset = sourceOffset + index * 2;
        QPointF result;
        if (!InversePoint(QPointF(source[offset], source[offset + 1]), result, quad, nullptr)) return false;
        results.push_back(result);
    }
    for (int index = 0; index < count; ++index) {
        const int offset = destinationOffset + index * 2;
        destination[offset] = static_cast<float>(results[index].x());
        destination[offset + 1] = static_cast<float>(results[index].y());
    }
    return true;
}

}
