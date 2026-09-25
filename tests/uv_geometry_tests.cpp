#include "opengl/cuvgeometry.h"

#include <QtTest>
#include <algorithm>
#include <cmath>
#include <limits>

namespace {
bool HasPoint(const std::vector<QPointF>& points, QPointF expected)
{
    return std::any_of(points.cbegin(), points.cend(), [&](QPointF point) {
        return point.x() == expected.x() && point.y() == expected.y();
    });
}
}

class UvGeometryTests : public QObject
{
    Q_OBJECT
private slots:
    void orientationRetainsCancellationRemainders()
    {
        QCOMPARE(CUvGeometry::Orientation({0, 0}, {2, 0}, {0, 2}), 1);
        QCOMPARE(CUvGeometry::Orientation({0, 0}, {0, 2}, {2, 0}), -1);
        QCOMPARE(CUvGeometry::Orientation({0, 0}, {1, 1}, {2, 2}), 0);
        QCOMPARE(CUvGeometry::Orientation({0, 0}, {9227465, 5702887}, {14930352, 9227465}), 1);
        const double huge = std::numeric_limits<float>::max();
        QCOMPARE(CUvGeometry::Orientation({-huge, -huge}, {huge, huge}, {0, 1}), 1);
        QCOMPARE(CUvGeometry::Orientation({huge, huge}, {-huge, -huge}, {0, 1}), -1);
        const double tiny = std::numeric_limits<float>::denorm_min();
        QCOMPARE(CUvGeometry::Orientation({0, 0}, {tiny, 0}, {0, tiny}), 1);
    }

    void triangleCoverageIncludesBoundariesForEitherWinding()
    {
        for (bool reverse : {false, true}) {
            const QPointF a(0, 0), b = reverse ? QPointF(0, 2) : QPointF(2, 0);
            const QPointF c = reverse ? QPointF(2, 0) : QPointF(0, 2);
            QVERIFY(CUvGeometry::ContainsTriangle({0.5, 0.5}, a, b, c));
            QVERIFY(CUvGeometry::ContainsTriangle({0, 0}, a, b, c));
            QVERIFY(CUvGeometry::ContainsTriangle({1, 1}, a, b, c));
            QVERIFY(!CUvGeometry::ContainsTriangle({1.5, 1.5}, a, b, c));
            QVERIFY(!CUvGeometry::ContainsTriangle({-0.1, 0.5}, a, b, c));
        }
        QVERIFY(!CUvGeometry::ContainsTriangle({1, 1}, {0, 0}, {1, 1}, {2, 2}));
    }

    void hugeNarrowTriangleKeepsVisiblePixelCenters()
    {
        const double huge = double(std::numeric_limits<float>::max()) * 4;
        const QPointF a(-huge, -huge), b(huge, huge), c(0, 4);
        for (bool reverse : {false, true}) {
            const QPointF first = reverse ? b : a, second = reverse ? a : b;
            QVERIFY(CUvGeometry::ContainsTriangle({0.5, 1.5}, first, second, c));
            QVERIFY(CUvGeometry::ContainsTriangle({1.5, 1.5}, first, second, c));
            QVERIFY(!CUvGeometry::ContainsTriangle({1.5, 0.5}, first, second, c));
            QVERIFY(!CUvGeometry::ContainsTriangle({0.5, 5.5}, first, second, c));
        }
    }

    void segmentClippingPreservesDirectionAndRejectsAtomically()
    {
        const QRectF rect(0, 0, 4, 4);
        QPointF a(-2, 1), b(6, 3);
        QVERIFY(CUvGeometry::ClipSegment(a, b, rect));
        QCOMPARE(a, QPointF(0, 1.5));
        QCOMPARE(b, QPointF(4, 2.5));
        a = {6, 3}; b = {-2, 1};
        QVERIFY(CUvGeometry::ClipSegment(a, b, rect));
        QCOMPARE(a, QPointF(4, 2.5));
        QCOMPARE(b, QPointF(0, 1.5));
        a = {-2, -1}; b = {6, -1};
        QVERIFY(!CUvGeometry::ClipSegment(a, b, rect));
        QCOMPARE(a, QPointF(-2, -1));
        QCOMPARE(b, QPointF(6, -1));
        a = {0, 0}; b = a;
        QVERIFY(CUvGeometry::ClipSegment(a, b, rect));
        QCOMPARE(a, b);
    }

    void extremeSegmentIntersectionsAvoidLargeCoordinateSubtraction()
    {
        const double huge = std::numeric_limits<float>::max();
        const QRectF rect(0, 0, 4, 4);
        QPointF a(-huge, -huge), b(huge, huge);
        QVERIFY(CUvGeometry::ClipSegment(a, b, rect));
        QCOMPARE(a, QPointF(0, 0));
        QCOMPARE(b, QPointF(4, 4));
        a = {-huge, 2}; b = {huge, 2};
        QVERIFY(CUvGeometry::ClipSegment(a, b, rect));
        QCOMPARE(a, QPointF(0, 2));
        QCOMPARE(b, QPointF(4, 2));
        a = {2, -huge}; b = {2, huge};
        QVERIFY(CUvGeometry::ClipSegment(a, b, rect));
        QCOMPARE(a, QPointF(2, 0));
        QCOMPARE(b, QPointF(2, 4));
        a = {-huge, -huge}; b = {0, 1};
        QVERIFY(CUvGeometry::ClipSegment(a, b, QRectF(-4, -4, 8, 8)));
        QCOMPARE(a, QPointF(-4, -3));
        QCOMPARE(b, QPointF(0, 1));
    }

    void triangleClippingReturnsKnownConvexIntersection()
    {
        const QRectF rect(0, 0, 2, 2);
        for (bool reverse : {false, true}) {
            const QPointF a(-1, 1), b = reverse ? QPointF(3, 3) : QPointF(1, -1);
            const QPointF c = reverse ? QPointF(1, -1) : QPointF(3, 3);
            const auto polygon = CUvGeometry::ClipTriangle(a, b, c, rect);
            QCOMPARE(polygon.size(), size_t(6));
            for (QPointF point : {QPointF(0, 0), QPointF(1.5, 0), QPointF(2, 1),
                                  QPointF(2, 2), QPointF(1, 2), QPointF(0, 1.5)}) QVERIFY(HasPoint(polygon, point));
            QCOMPARE(CUvGeometry::Orientation(polygon[0], polygon[1], polygon[2]), reverse ? -1 : 1);
            for (QPointF point : polygon) QVERIFY(rect.contains(point));
        }
        const auto enclosing = CUvGeometry::ClipTriangle({-1, -1}, {5, -1}, {-1, 5}, rect);
        QCOMPARE(enclosing.size(), size_t(4));
        for (QPointF point : {rect.topLeft(), rect.topRight(), rect.bottomRight(), rect.bottomLeft()})
            QVERIFY(HasPoint(enclosing, point));
        QVERIFY(CUvGeometry::ClipTriangle({3, 3}, {4, 3}, {3, 4}, rect).empty());
        QVERIFY(CUvGeometry::ClipTriangle({-1, 0}, {0, 0}, {-1, 1}, rect).empty());
    }

    void hugeAndNearlyCoincidentClippedVerticesRemainDistinct()
    {
        const double huge = double(std::numeric_limits<float>::max()) * 4;
        const QRectF rect(0, 0, 4, 4);
        const auto polygon = CUvGeometry::ClipTriangle({-huge, -huge}, {huge, huge}, {0, 4}, rect);
        QCOMPARE(polygon.size(), size_t(3));
        for (QPointF point : {QPointF(0, 0), QPointF(4, 4), QPointF(0, 4)}) QVERIFY(HasPoint(polygon, point));
        const auto thin = CUvGeometry::ClipTriangle({0, 0}, {1, 1}, {1, 1 + 1e-13}, QRectF(0, 0, 2, 2));
        QCOMPARE(thin.size(), size_t(3));
        QCOMPARE(CUvGeometry::Orientation(thin[0], thin[1], thin[2]), 1);
    }

    void invalidInputsNeverReachClippedOutput()
    {
        const double infinity = std::numeric_limits<double>::infinity();
        const double nan = std::numeric_limits<double>::quiet_NaN();
        for (double value : {infinity, -infinity, nan}) {
            QCOMPARE(CUvGeometry::Orientation({value, 0}, {1, 0}, {0, 1}), 0);
            QVERIFY(!CUvGeometry::ContainsTriangle({value, 0}, {0, 0}, {1, 0}, {0, 1}));
            QVERIFY(CUvGeometry::ClipTriangle({value, 0}, {1, 0}, {0, 1}, QRectF(0, 0, 1, 1)).empty());
            QPointF a(0, 0), b(value, 1);
            QVERIFY(!CUvGeometry::ClipSegment(a, b, QRectF(0, 0, 1, 1)));
            QCOMPARE(a, QPointF(0, 0));
        }
        for (QRectF invalid : {QRectF(), QRectF(0, 0, -1, 2), QRectF(0, 0, infinity, 2), QRectF(nan, 0, 2, 2)}) {
            QPointF a(0, 0), b(1, 1);
            QVERIFY(!CUvGeometry::ClipSegment(a, b, invalid));
            QCOMPARE(a, QPointF(0, 0));
            QCOMPARE(b, QPointF(1, 1));
            QVERIFY(CUvGeometry::ClipTriangle({0, 0}, {1, 0}, {0, 1}, invalid).empty());
        }
    }
};

QTEST_GUILESS_MAIN(UvGeometryTests)
#include "uv_geometry_tests.moc"
