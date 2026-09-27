#include "ccanvascoordinates.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <limits>

namespace {
using Matrix3 = std::array<std::array<double, 3>, 3>;

struct CanvasProjection
{
    Matrix3 forward;
    Matrix3 inverse;
    double width;
    double height;
};

bool finitePoint(const QPointF& point)
{
    return std::isfinite(point.x()) && std::isfinite(point.y());
}

template<size_t Size>
bool invertible(const std::array<std::array<double, Size>, Size>& matrix)
{
    static_assert(Size == 3 || Size == 4);
    struct Expansion {
        std::array<double, 256> terms{};
        size_t count = 0;

        void add(double value)
        {
            size_t written = 0;
            for (size_t index = 0; index < count; ++index) {
                const double sum = value + terms[index];
                const double recovered = sum - value;
                const double error = (value - (sum - recovered)) + (terms[index] - recovered);
                if (error != 0.0) terms[written++] = error;
                value = sum;
            }
            if (value != 0.0) terms[written++] = value;
            count = written;
        }
    };
    // Four float factors fit double without overflow or underflow, including the rounding remainders.
    Expansion determinant;
    std::array<size_t, Size> permutation;
    for (size_t index = 0; index < Size; ++index) permutation[index] = index;
    do {
        int inversions = 0;
        for (size_t first = 0; first < Size; ++first)
            for (size_t second = first + 1; second < Size; ++second)
                if (permutation[first] > permutation[second]) ++inversions;
        Expansion product;
        product.add(inversions % 2 ? -1.0 : 1.0);
        for (size_t row = 0; row < Size; ++row) {
            Expansion next;
            for (size_t term = 0; term < product.count; ++term) {
                const double value = product.terms[term] * matrix[row][permutation[row]];
                next.add(std::fma(product.terms[term], matrix[row][permutation[row]], -value));
                next.add(value);
            }
            product = next;
        }
        for (size_t term = 0; term < product.count; ++term) determinant.add(product.terms[term]);
    } while (std::next_permutation(permutation.begin(), permutation.end()));
    for (size_t term = 0; term < determinant.count; ++term) {
        if (!std::isfinite(determinant.terms[term])) return false;
    }
    return determinant.count != 0;
}

bool prepareProjection(const QMatrix4x4& mvp, const QSize& size, CanvasProjection& projection)
{
    if (size.width() <= 0 || size.height() <= 0) return false;
    std::array<std::array<double, 4>, 4> matrix;
    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column) {
            matrix[row][column] = mvp(row, column);
            if (!std::isfinite(matrix[row][column])) return false;
        }
    }
    if (!invertible(matrix)) return false;
    Matrix3 plane{{
        {{matrix[0][0], matrix[0][1], matrix[0][3]}},
        {{matrix[1][0], matrix[1][1], matrix[1][3]}},
        {{matrix[3][0], matrix[3][1], matrix[3][3]}}
    }};
    if (!invertible(plane)) return false;
    const double a = plane[0][0], b = plane[0][1], c = plane[0][2];
    const double d = plane[1][0], e = plane[1][1], f = plane[1][2];
    const double g = plane[2][0], h = plane[2][1], i = plane[2][2];
    Matrix3 inverse{{
        {{e * i - f * h, c * h - b * i, b * f - c * e}},
        {{f * g - d * i, a * i - c * g, c * d - a * f}},
        {{d * h - e * g, b * g - a * h, a * e - b * d}}
    }};
    double scale = 0;
    for (const auto& row : inverse)
        for (double value : row) scale = std::max(scale, std::abs(value));
    if (!std::isfinite(scale) || scale == 0.0) return false;
    for (auto& row : inverse)
        for (double& value : row) value /= scale;
    scale = 0;
    for (const auto& row : plane)
        for (double value : row) scale = std::max(scale, std::abs(value));
    for (auto& row : plane)
        for (double& value : row) value /= scale;
    projection = {plane, inverse, double(size.width()), double(size.height())};
    return true;
}

std::array<double, 3> transform(const Matrix3& matrix, const std::array<double, 3>& point)
{
    std::array<double, 3> result;
    for (size_t row = 0; row < 3; ++row)
        result[row] = matrix[row][0] * point[0] + matrix[row][1] * point[1] + matrix[row][2] * point[2];
    return result;
}

bool usableDivisor(const Matrix3& matrix, const std::array<double, 3>& point, double divisor)
{
    const double magnitude = std::abs(matrix[2][0] * point[0]) + std::abs(matrix[2][1] * point[1]) +
                             std::abs(matrix[2][2] * point[2]);
    return std::isfinite(divisor) && std::abs(divisor) > 8.0 * std::numeric_limits<double>::epsilon() * magnitude;
}

bool project(const CanvasProjection& projection, const QPointF& model, QPointF& pixel)
{
    if (!finitePoint(model)) return false;
    const double scale = std::max({1.0, std::abs(model.x()), std::abs(model.y())});
    const std::array<double, 3> normalized{{model.x() / scale, model.y() / scale, 1.0 / scale}};
    const auto point = transform(projection.forward, normalized);
    if (!usableDivisor(projection.forward, normalized, point[2])) return false;
    const QPointF candidate((point[0] + point[2]) * (projection.width * 0.5) / point[2],
                            (point[2] - point[1]) * (projection.height * 0.5) / point[2]);
    if (!finitePoint(candidate)) return false;
    pixel = candidate;
    return true;
}
}

bool CCanvasCoordinates::ScreenToModel(const QMatrix4x4& mvp, const QSize& viewSize,
                                       const QPointF& logicalPixel, QPointF& model)
{
    CanvasProjection projection;
    if (!finitePoint(logicalPixel) || !prepareProjection(mvp, viewSize, projection)) return false;
    const double scale = std::max({1.0, std::abs(logicalPixel.x()), std::abs(logicalPixel.y())});
    const double reciprocal = 1.0 / scale;
    const std::array<double, 3> normalized{{
        (logicalPixel.x() / scale) * 2.0 / projection.width - reciprocal,
        reciprocal - (logicalPixel.y() / scale) * 2.0 / projection.height,
        reciprocal
    }};
    const auto point = transform(projection.inverse, normalized);
    if (!usableDivisor(projection.inverse, normalized, point[2])) return false;
    const QPointF candidate(point[0] / point[2], point[1] / point[2]);
    if (!finitePoint(candidate)) return false;
    model = candidate;
    return true;
}

bool CCanvasCoordinates::ModelToScreen(const QMatrix4x4& mvp, const QSize& viewSize,
                                       const QPointF& model, QPointF& logicalPixel)
{
    CanvasProjection projection;
    return prepareProjection(mvp, viewSize, projection) && project(projection, model, logicalPixel);
}

bool CCanvasCoordinates::ProjectVertices(const QMatrix4x4& mvp, const QSize& viewSize,
    const QVector<float>& modelVertices, QVector<QPointF>& logicalPixels, QString* error)
try {
    const auto fail = [&](const QString& reason) {
        if (error)
            *error = reason;
        return false;
    };
    if (modelVertices.size() % 2 || modelVertices.size() > 2000000)
        return fail(QStringLiteral("待投影的顶点数量无效"));
    CanvasProjection projection;
    if (!prepareProjection(mvp, viewSize, projection))
        return fail(QStringLiteral("画布投影或逻辑尺寸无效"));
    QVector<QPointF> candidate;
    candidate.reserve(modelVertices.size() / 2);
    for (qsizetype index = 0; index < modelVertices.size(); index += 2) {
        QPointF point;
        if (!project(projection, { modelVertices[index], modelVertices[index + 1] }, point))
            return fail(QStringLiteral("顶点 %1 无法投影到当前画布").arg(index / 2));
        candidate.append(point);
    }
    logicalPixels.swap(candidate);
    if (error)
        error->clear();
    return true;
} catch (const std::exception&) {
    if (error)
        *error = QStringLiteral("无法分配画布投影结果");
    return false;
}

int CCanvasCoordinates::NearestVertex(const QMatrix4x4& mvp, const QSize& viewSize,
                                      const QPointF& logicalPixel, const QVector<float>& coreVertices,
                                      double radiusPixels)
{
    if (!finitePoint(logicalPixel) || !std::isfinite(radiusPixels) || radiusPixels < 0.0 ||
        coreVertices.isEmpty() || coreVertices.size() % 2 ||
        coreVertices.size() / 2 > std::numeric_limits<int>::max()) return -1;
    CanvasProjection projection;
    if (!prepareProjection(mvp, viewSize, projection)) return -1;
    int nearest = -1;
    double nearestDistance = radiusPixels;
    for (qsizetype index = 0; index < coreVertices.size() / 2; ++index) {
        QPointF pixel;
        if (!project(projection, QPointF(coreVertices[index * 2], coreVertices[index * 2 + 1]), pixel)) return -1;
        const double distance = std::hypot(pixel.x() - logicalPixel.x(), pixel.y() - logicalPixel.y());
        if (distance < nearestDistance || (nearest < 0 && distance <= nearestDistance)) {
            nearest = int(index);
            nearestDistance = distance;
        }
    }
    return nearest;
}
