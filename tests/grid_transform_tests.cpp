#include "live2dstruct/coor/cgridtransform.h"
#include "live2dstruct/coor/cwarpcoortype.h"
#include <QtTest>
#include <array>
#include <limits>

using namespace live2dstruct;

class GridTransformTests : public QObject
{
    Q_OBJECT
private slots:
    void warpRoundTrip_data()
    {
        QTest::addColumn<bool>("quad");
        QTest::addColumn<bool>("reflected");
        QTest::newRow("triangles") << false << false;
        QTest::newRow("bilinear") << true << false;
        QTest::newRow("triangles-reflected") << false << true;
        QTest::newRow("bilinear-reflected") << true << true;
    }

    void warpRoundTrip()
    {
        QFETCH(bool, quad);
        QFETCH(bool, reflected);
        std::vector<float> control{0, 0, 1, -0.2f, 2, 0, -0.1f, 1, 1.2f, 1.1f, 2.1f, 1,
                                   0, 2, 1, 2.2f, 2, 2};
        if (reflected) for (size_t index = 0; index < control.size(); index += 2) control[index] = -control[index];
        CWarpCoorType warp(2, 2, control, quad);
        const std::array<float, 13> coordinates{-8, -2, -1, 0, 0.13f, 0.25f, 0.5f, 0.76f, 1, 1.5f, 2.5f, 3, 8};
        for (float x : coordinates) {
            for (float y : coordinates) {
                float local[]{x, y};
                float canvas[]{99, 99};
                float restored[]{99, 99};
                QVERIFY(warp.transformLocalToCanvas(local, canvas, 1, 0, 2));
                const QByteArray label = QStringLiteral("(%1, %2) -> (%3, %4)").arg(x).arg(y).arg(canvas[0]).arg(canvas[1]).toUtf8();
                QVERIFY2(warp.transformCanvasToLocal(canvas, restored, 1), label.constData());
                QVERIFY2(std::abs(restored[0] - x) < 0.00002f && std::abs(restored[1] - y) < 0.00002f, label.constData());
            }
        }
    }

    void modesUseDifferentInverseFunctions()
    {
        std::vector<float> control{0, 0, 2, 0, 0, 2, 3, 3};
        CWarpCoorType triangle(1, 1, control, false);
        CWarpCoorType quad(1, 1, control, true);
        float local[]{0.25f, 0.25f};
        float triangleResult[2];
        float quadResult[2];
        QVERIFY(triangle.transformLocalToCanvas(local, triangleResult, 1, 0, 2));
        QVERIFY(quad.transformLocalToCanvas(local, quadResult, 1, 0, 2));
        QCOMPARE(triangleResult[0], 0.5f);
        QCOMPARE(triangleResult[1], 0.5f);
        QCOMPARE(quadResult[0], 0.5625f);
        QCOMPARE(quadResult[1], 0.5625f);
        QVERIFY(triangle.transformCanvasToLocal(triangleResult, triangleResult, 1));
        QVERIFY(quad.transformCanvasToLocal(quadResult, quadResult, 1));
        QVERIFY(std::abs(triangleResult[0] - 0.25f) < 0.00001f);
        QVERIFY(std::abs(quadResult[0] - 0.25f) < 0.00001f);
    }

    void inverseAppliesDestinationRectangle()
    {
        const std::vector<float> control{0, 0, 2, 0, 0, 2, 2, 2};
        const CGridTransform grid(QRectF(10, -5, 4, 8), control, 1, 1);
        QVector2D destination(99, 99);
        QVERIFY(grid.inverseTransform(QVector2D(0.5f, 1.5f), destination, true));
        QCOMPARE(destination, QVector2D(11, 1));
        QVERIFY(grid.inverseTransform(QVector2D(-2, 4), destination, false));
        QCOMPARE(destination, QVector2D(6, 11));
    }

    void batchAndStridedTransformsSupportOverlap()
    {
        std::vector<float> control{10, 20, 12, 20, 10, 22, 12, 22};
        CWarpCoorType warp(1, 1, control, true);
        float data[]{77, 0, 0, 88, 0.5f, 1, 99};
        QVERIFY(warp.transformLocalToCanvas(data, data, 2, 1, 3));
        QCOMPARE(data[0], 77.0f);
        QCOMPARE(data[1], 10.0f);
        QCOMPARE(data[2], 20.0f);
        QCOMPARE(data[3], 88.0f);
        QCOMPARE(data[4], 11.0f);
        QCOMPARE(data[5], 22.0f);
        QCOMPARE(data[6], 99.0f);
        float overlapping[]{0, 0, 1, 1, 99};
        QVERIFY(warp.transformLocalToCanvas(overlapping, overlapping + 1, 2, 0, 2));
        QCOMPARE(overlapping[1], 10.0f);
        QCOMPARE(overlapping[2], 20.0f);
        QCOMPARE(overlapping[3], 12.0f);
        QCOMPARE(overlapping[4], 22.0f);
        const CGridTransform grid(QRectF(0, 0, 1, 1), control, 1, 1);
        QVERIFY(grid.inverseTransform(overlapping, 1, overlapping, 0, 2, true));
        QCOMPARE(overlapping[0], 0.0f);
        QCOMPARE(overlapping[1], 0.0f);
        QCOMPARE(overlapping[2], 1.0f);
        QCOMPARE(overlapping[3], 1.0f);
    }

    void invalidInputsLeaveOutputUntouched()
    {
        const float nan = std::numeric_limits<float>::quiet_NaN();
        const std::vector<std::vector<float>> invalid{{}, {0, 0, 1}, {0, 0, 1, 0, 0, 1, nan, 1}};
        for (const auto& points : invalid) {
            CGridTransform grid(QRectF(0, 0, 1, 1), points, 1, 1);
            QVERIFY(!grid.IsValid());
            QVector2D output(7, 9);
            QVERIFY(!grid.inverseTransform(QVector2D(0, 0), output, true));
            QCOMPARE(output, QVector2D(7, 9));
        }
        std::vector<float> control{0, 0, 2, 0, 0, 2, 2, 2};
        QVERIFY(!CGridTransform(QRectF(0, 0, 1, 1), control, 0, 1).IsValid());
        QVERIFY(!CGridTransform(QRectF(0, 0, 1, 1), control, std::numeric_limits<int>::max(), 1).IsValid());
        CWarpCoorType warp(1, 1, control, true);
        float source[]{0, 0, nan, 1};
        float destination[]{7, 8, 9, 10};
        QVERIFY(!warp.transformLocalToCanvas(source, destination, 2, 0, 2));
        QVERIFY(!warp.transformCanvasToLocal(source, destination, 2));
        QCOMPARE(destination[0], 7.0f);
        QCOMPARE(destination[3], 10.0f);
        QVERIFY(!warp.transformLocalToCanvas(source, destination, 1, -1, 2));
        QVERIFY(!warp.transformLocalToCanvas(source, destination, 1, 0, 1));
        QVERIFY(!warp.transformLocalToCanvas(source, destination, 1000001, 0, 2));
        QVERIFY(!warp.transformLocalToCanvas(source, destination, 2, std::numeric_limits<int>::max(), 2));
        QVERIFY(!warp.transformCanvasToLocal(nullptr, destination, 1));
        QVERIFY(warp.transformCanvasToLocal(nullptr, nullptr, 0));
        QVERIFY(warp.transformLocalToCanvas(nullptr, nullptr, 0, 0, 2));
    }

    void collapsedAndFoldedGridsNeedAnUnambiguousInverse()
    {
        const std::vector<float> collapsed{0, 0, 0, 0, 0, 0, 0, 0};
        QVector2D output(7, 9);
        const CGridTransform flat(QRectF(0, 0, 1, 1), collapsed, 1, 1);
        QVERIFY(!flat.inverseTransform(QVector2D(0, 0), output, true));
        QCOMPARE(output, QVector2D(7, 9));
        const std::vector<float> folded{0, 0, 1, 0, 0, 0, 0, 1, 1, 1, 0, 1};
        const CGridTransform grid(QRectF(0, 0, 1, 1), folded, 2, 1);
        for (bool quad : {false, true}) {
            QVERIFY(!grid.inverseTransform(QVector2D(0.5f, 0.5f), output, quad));
            QCOMPARE(output, QVector2D(7, 9));
            const QVector2D left(0.25f, 0.5f);
            QVERIFY(grid.inverseTransform(QVector2D(0.5f, 0.5f), output, quad, &left));
            QCOMPARE(output, left);
            const QVector2D right(0.75f, 0.5f);
            QVERIFY(grid.inverseTransform(QVector2D(0.5f, 0.5f), output, quad, &right));
            QCOMPARE(output, right);
            output = QVector2D(7, 9);
        }
    }

    void bilinearFoldRetainsBothRootsForBranchSelection()
    {
        const std::vector<float> control{0, 0, 1, 0, 0, 1, -1, -1};
        const CGridTransform grid(QRectF(0, 0, 1, 1), control, 1, 1);
        QVector2D output(7, 9);
        QVERIFY(!grid.inverseTransform(QVector2D(0.08f, 0.08f), output, true));
        QCOMPARE(output, QVector2D(7, 9));
        for (float coordinate : {0.1f, 0.4f}) {
            const QVector2D hint(coordinate, coordinate);
            QVERIFY(grid.inverseTransform(QVector2D(0.08f, 0.08f), output, true, &hint));
            QVERIFY((output - hint).length() < 0.00001f);
        }
    }

    void gridScaleDoesNotChangeNormalizedInverse()
    {
        for (float scale : {1.0e-20f, 1.0e20f}) {
            std::vector<float> control{0, 0, scale, 0, 0, scale, scale, scale};
            CWarpCoorType warp(1, 1, control, true);
            float local[]{0.25f, 0.75f};
            float canvas[2];
            float restored[2];
            QVERIFY(warp.transformLocalToCanvas(local, canvas, 1, 0, 2));
            QVERIFY(warp.transformCanvasToLocal(canvas, restored, 1));
            QVERIFY(std::abs(restored[0] - local[0]) < 0.00001f);
            QVERIFY(std::abs(restored[1] - local[1]) < 0.00001f);
        }
    }

    void finiteLargeGridBoundariesDoNotOverflowIntermediateValues()
    {
        const float scale = std::numeric_limits<float>::max() / 2;
        std::vector<float> control{0, 0, scale, 0, 0, scale, scale, scale};
        for (bool quad : {false, true}) {
            CWarpCoorType warp(1, 1, control, quad);
            for (const QVector2D& point : {QVector2D(0, 0), QVector2D(1, 0), QVector2D(0, 1),
                                           QVector2D(1, 1), QVector2D(0.25f, 0.75f), QVector2D(1.5f, -0.5f)}) {
                float source[]{point.x(), point.y()};
                float canvas[]{7, 9};
                float restored[]{7, 9};
                QVERIFY(warp.transformLocalToCanvas(source, canvas, 1, 0, 2));
                QCOMPARE(canvas[0], static_cast<float>(double(scale) * point.x()));
                QCOMPARE(canvas[1], static_cast<float>(double(scale) * point.y()));
                QVERIFY(warp.transformCanvasToLocal(canvas, restored, 1));
                QVERIFY(std::abs(restored[0] - point.x()) < 0.00001f);
                QVERIFY(std::abs(restored[1] - point.y()) < 0.00001f);
            }
            float source[]{0.5f, 0.5f, 3, 3};
            float destination[]{7, 8, 9, 10};
            QVERIFY(!warp.transformLocalToCanvas(source, destination, 2, 0, 2));
            QCOMPARE(destination[0], 7.0f);
            QCOMPARE(destination[1], 8.0f);
            QCOMPARE(destination[2], 9.0f);
            QCOMPARE(destination[3], 10.0f);
        }
    }

    void finiteTranslatedGridRetainsBoundaryAndExtensionCoordinates()
    {
        const float origin = 1.0e38f;
        const float end = origin + 1.0e37f;
        const double extent = double(end) - origin;
        std::vector<float> control{origin, origin, end, origin, origin, end, end, end};
        const std::array<float, 7> coordinates{-3, -1, 0, 0.25f, 1, 2, 4};
        for (bool quad : {false, true}) {
            CWarpCoorType warp(1, 1, control, quad);
            for (float x : coordinates) {
                for (float y : coordinates) {
                    float source[]{x, y};
                    float canvas[]{7, 9};
                    float restored[]{7, 9};
                    QVERIFY(warp.transformLocalToCanvas(source, canvas, 1, 0, 2));
                    QCOMPARE(canvas[0], static_cast<float>(origin + extent * x));
                    QCOMPARE(canvas[1], static_cast<float>(origin + extent * y));
                    QVERIFY(warp.transformCanvasToLocal(canvas, restored, 1));
                    QVERIFY(std::abs(restored[0] - x) < 0.00002f);
                    QVERIFY(std::abs(restored[1] - y) < 0.00002f);
                }
            }
        }
    }
};

QTEST_APPLESS_MAIN(GridTransformTests)
#include "grid_transform_tests.moc"
