#include "cuvgeometry.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace {
bool finitePoint(QPointF point)
{
    return std::isfinite(point.x()) && std::isfinite(point.y());
}

class Expansion
{
public:
    bool IsValid() const { return m_valid; }
    size_t Size() const { return m_size; }
    double Component(size_t index) const { return m_components[index]; }
    int Sign() const
    {
        if (!m_valid || m_size == 0) return 0;
        return m_components[m_size - 1] > 0.0 ? 1 : -1;
    }
    double Estimate() const
    {
        double value = 0.0;
        for (size_t index = 0; index < m_size; ++index) value += m_components[index];
        return value;
    }
    void Add(double value)
    {
        if (!m_valid || !std::isfinite(value)) { m_valid = false; return; }
        std::array<double, 32> result{};
        size_t size = 0;
        for (size_t index = 0; index < m_size; ++index) {
            const double component = m_components[index];
            const double sum = value + component;
            if (!std::isfinite(sum)) { m_valid = false; return; }
            const double shifted = sum - value;
            const double error = (value - (sum - shifted)) + (component - shifted);
            if (error != 0.0) {
                if (size == result.size()) { m_valid = false; return; }
                result[size++] = error;
            }
            value = sum;
        }
        if (value != 0.0) {
            if (size == result.size()) { m_valid = false; return; }
            result[size++] = value;
        }
        m_components = result;
        m_size = size;
    }
    void AddProduct(double first, double second)
    {
        const double product = first * second;
        if (!std::isfinite(product)) { m_valid = false; return; }
        Add(std::fma(first, second, -product));
        Add(product);
    }
private:
    std::array<double, 32> m_components{};
    size_t m_size = 0;
    bool m_valid = true;
};

Expansion determinant(QPointF a, QPointF b, QPointF c)
{
    Expansion value;
    value.AddProduct(a.x(), b.y());
    value.AddProduct(-a.y(), b.x());
    value.AddProduct(b.x(), c.y());
    value.AddProduct(-b.y(), c.x());
    value.AddProduct(c.x(), a.y());
    value.AddProduct(-c.y(), a.x());
    return value;
}

bool divide(const Expansion& numerator, const Expansion& denominator, double& value)
{
    if (!numerator.IsValid() || !denominator.IsValid() || denominator.Sign() == 0) return false;
    const double divisor = denominator.Estimate();
    const double quotient = numerator.Estimate() / divisor;
    if (!std::isfinite(quotient)) return false;
    Expansion remainder = numerator;
    for (size_t index = 0; index < denominator.Size(); ++index)
        remainder.AddProduct(-quotient, denominator.Component(index));
    if (!remainder.IsValid()) return false;
    value = quotient + remainder.Estimate() / divisor;
    return std::isfinite(value);
}

struct Bounds
{
    double left, top, right, bottom;
    bool Set(QRectF rect)
    {
        left = rect.left();
        top = rect.top();
        right = rect.right();
        bottom = rect.bottom();
        return std::isfinite(left) && std::isfinite(top) && std::isfinite(right) && std::isfinite(bottom) &&
               left < right && top < bottom;
    }
    int Outside(QPointF point) const
    {
        return (point.x() < left ? 1 : (point.x() > right ? 2 : 0)) |
               (point.y() < top ? 4 : (point.y() > bottom ? 8 : 0));
    }
};

bool intersection(QPointF a, QPointF b, bool vertical, double coordinate, QPointF& point)
{
    Expansion numerator, denominator;
    if (vertical) {
        numerator.AddProduct(a.y(), b.x());
        numerator.AddProduct(-a.x(), b.y());
        numerator.AddProduct(coordinate, b.y());
        numerator.AddProduct(-coordinate, a.y());
        denominator.Add(b.x());
        denominator.Add(-a.x());
    } else {
        numerator.AddProduct(a.x(), b.y());
        numerator.AddProduct(-a.y(), b.x());
        numerator.AddProduct(coordinate, b.x());
        numerator.AddProduct(-coordinate, a.x());
        denominator.Add(b.y());
        denominator.Add(-a.y());
    }
    double remaining;
    if (!divide(numerator, denominator, remaining)) return false;
    point = vertical ? QPointF(coordinate, remaining) : QPointF(remaining, coordinate);
    return true;
}
}

int CUvGeometry::Orientation(QPointF a, QPointF b, QPointF c)
{
    if (!finitePoint(a) || !finitePoint(b) || !finitePoint(c)) return 0;
    const std::array<double, 6> products{a.x() * b.y(), -a.y() * b.x(), b.x() * c.y(),
                                       -b.y() * c.x(), c.x() * a.y(), -c.y() * a.x()};
    double value = 0.0, magnitude = 0.0;
    for (double product : products) {
        value += product;
        magnitude += std::abs(product);
    }
    const double error = 16.0 * std::numeric_limits<double>::epsilon() * magnitude;
    if (std::isfinite(value) && std::isfinite(error) && std::abs(value) > error) return value > 0 ? 1 : -1;
    return determinant(a, b, c).Sign();
}

bool CUvGeometry::ContainsTriangle(QPointF point, QPointF a, QPointF b, QPointF c)
{
    if (!finitePoint(point) || !finitePoint(a) || !finitePoint(b) || !finitePoint(c) ||
        point.x() < std::min({a.x(), b.x(), c.x()}) || point.x() > std::max({a.x(), b.x(), c.x()}) ||
        point.y() < std::min({a.y(), b.y(), c.y()}) || point.y() > std::max({a.y(), b.y(), c.y()})) return false;
    const int winding = Orientation(a, b, c);
    if (winding == 0) return false;
    for (int side : {Orientation(a, b, point), Orientation(b, c, point), Orientation(c, a, point)})
        if (side != 0 && side != winding) return false;
    return true;
}

bool CUvGeometry::ClipSegment(QPointF& a, QPointF& b, QRectF rect)
{
    Bounds bounds;
    if (!finitePoint(a) || !finitePoint(b) || !bounds.Set(rect)) return false;
    QPointF first = a, second = b;
    for (int iteration = 0; iteration < 8; ++iteration) {
        const int firstOutside = bounds.Outside(first), secondOutside = bounds.Outside(second);
        if (!(firstOutside | secondOutside)) { a = first; b = second; return true; }
        if (firstOutside & secondOutside) return false;
        const int outside = firstOutside ? firstOutside : secondOutside;
        QPointF clipped;
        bool valid;
        if (outside & 4) valid = intersection(a, b, false, bounds.top, clipped);
        else if (outside & 8) valid = intersection(a, b, false, bounds.bottom, clipped);
        else if (outside & 2) valid = intersection(a, b, true, bounds.right, clipped);
        else valid = intersection(a, b, true, bounds.left, clipped);
        if (!valid) return false;
        if (firstOutside) first = clipped;
        else second = clipped;
    }
    return false;
}

std::vector<QPointF> CUvGeometry::ClipTriangle(QPointF a, QPointF b, QPointF c, QRectF rect)
{
    Bounds bounds;
    if (!bounds.Set(rect)) return {};
    const int winding = Orientation(a, b, c);
    if (winding == 0) return {};
    std::vector<QPointF> points;
    for (QPointF corner : {rect.topLeft(), rect.topRight(), rect.bottomRight(), rect.bottomLeft()})
        if (ContainsTriangle(corner, a, b, c)) points.push_back(corner);
    const std::array<QPointF, 3> vertices{a, b, c};
    for (size_t index = 0; index < vertices.size(); ++index) {
        QPointF first = vertices[index], second = vertices[(index + 1) % vertices.size()];
        if (ClipSegment(first, second, rect)) {
            points.push_back(first);
            points.push_back(second);
        }
    }
    std::sort(points.begin(), points.end(), [](QPointF first, QPointF second) {
        return first.x() < second.x() || (first.x() == second.x() && first.y() < second.y());
    });
    points.erase(std::unique(points.begin(), points.end(), [](QPointF first, QPointF second) {
        return first.x() == second.x() && first.y() == second.y();
    }), points.end());
    if (points.size() < 3) return {};
    std::vector<QPointF> hull;
    for (QPointF point : points) {
        while (hull.size() >= 2 && Orientation(hull[hull.size() - 2], hull.back(), point) <= 0) hull.pop_back();
        hull.push_back(point);
    }
    const size_t lowerSize = hull.size();
    for (auto point = points.crbegin() + 1; point != points.crend(); ++point) {
        while (hull.size() > lowerSize && Orientation(hull[hull.size() - 2], hull.back(), *point) <= 0) hull.pop_back();
        hull.push_back(*point);
    }
    hull.pop_back();
    if (hull.size() < 3) return {};
    if (winding < 0) std::reverse(hull.begin(), hull.end());
    return hull;
}
