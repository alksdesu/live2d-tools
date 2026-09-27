#include "opengl/ccanvascoordinates.h"

#include <QtTest>
#include <algorithm>
#include <cmath>
#include <limits>

class CanvasCoordinatesTests : public QObject
{
    Q_OBJECT

    static void comparePoint(const QPointF& actual, const QPointF& expected, double tolerance = 1e-11)
    {
        QVERIFY(std::isfinite(actual.x()) && std::isfinite(actual.y()));
        QVERIFY2(std::abs(actual.x() - expected.x()) <= tolerance * std::max(1.0, std::abs(expected.x())), "Unexpected X coordinate");
        QVERIFY2(std::abs(actual.y() - expected.y()) <= tolerance * std::max(1.0, std::abs(expected.y())), "Unexpected Y coordinate");
    }

private slots:
    void knownTransforms_data()
    {
        QTest::addColumn<QMatrix4x4>("matrix");
        QTest::addColumn<QSize>("size");
        QTest::addColumn<QPointF>("model");
        QTest::addColumn<QPointF>("pixel");
        QMatrix4x4 identity;
        QTest::newRow("center") << identity << QSize(200, 100) << QPointF(0, 0) << QPointF(100, 50);
        QTest::newRow("top-left") << identity << QSize(200, 100) << QPointF(-1, 1) << QPointF(0, 0);
        QTest::newRow("bottom-right") << identity << QSize(200, 100) << QPointF(1, -1) << QPointF(200, 100);
        QTest::newRow("quarter") << identity << QSize(200, 100) << QPointF(-0.5, 0.5) << QPointF(50, 25);
        QTest::newRow("outside-viewport") << identity << QSize(200, 100) << QPointF(2, -3) << QPointF(300, 200);
        QTest::newRow("resized") << identity << QSize(800, 600) << QPointF(-0.5, 0.5) << QPointF(200, 150);
        QMatrix4x4 scaled;
        scaled(0, 0) = 2;
        scaled(1, 1) = 4;
        scaled(0, 3) = 0.25f;
        scaled(1, 3) = -0.5f;
        QTest::newRow("scale-translate") << scaled << QSize(200, 100) << QPointF(0.25, 0.125) << QPointF(175, 50);
        QMatrix4x4 reflected;
        reflected(0, 0) = -2;
        reflected(1, 1) = 0.5f;
        reflected(0, 3) = 0.25f;
        reflected(1, 3) = 0.5f;
        QTest::newRow("reflection") << reflected << QSize(200, 100) << QPointF(0.125, -1) << QPointF(100, 50);
        QMatrix4x4 rotated;
        rotated(0, 0) = 0;
        rotated(0, 1) = -1;
        rotated(1, 0) = 1;
        rotated(1, 1) = 0;
        QTest::newRow("quarter-turn") << rotated << QSize(200, 100) << QPointF(0.5, 0.25) << QPointF(75, 25);
        QMatrix4x4 perspective;
        perspective(3, 0) = 0.5f;
        QTest::newRow("homogeneous-divide") << perspective << QSize(200, 100) << QPointF(0.5, 0.25) << QPointF(140, 40);
        QMatrix4x4 depthCoupled;
        depthCoupled(0, 2) = 2;
        depthCoupled(2, 0) = 0.25f;
        depthCoupled(2, 3) = 3;
        QTest::newRow("model-z-zero-plane") << depthCoupled << QSize(200, 100) << QPointF(0.5, 0.25) << QPointF(150, 37.5);
    }

    void knownTransforms()
    {
        QFETCH(QMatrix4x4, matrix);
        QFETCH(QSize, size);
        QFETCH(QPointF, model);
        QFETCH(QPointF, pixel);
        QPointF projected(123, 456), unprojected(123, 456);
        QVERIFY(CCanvasCoordinates::ModelToScreen(matrix, size, model, projected));
        comparePoint(projected, pixel);
        QVERIFY(CCanvasCoordinates::ScreenToModel(matrix, size, pixel, unprojected));
        comparePoint(unprojected, model);
    }

    void logicalPixelsRemainIndependentOfDevicePixelRatio()
    {
        const QSize logicalSize(320, 200);
        const int devicePixelRatio = 2;
        const QSize framebufferSize = logicalSize * devicePixelRatio;
        const QPointF model(-0.5, 0.5);
        QPointF logicalPixel, framebufferPixel, restored;
        QVERIFY(CCanvasCoordinates::ModelToScreen(QMatrix4x4(), logicalSize, model, logicalPixel));
        QCOMPARE(logicalPixel, QPointF(80, 50));
        QVERIFY(CCanvasCoordinates::ModelToScreen(QMatrix4x4(), framebufferSize, model, framebufferPixel));
        QCOMPARE(framebufferPixel, logicalPixel * devicePixelRatio);
        QVERIFY(CCanvasCoordinates::ScreenToModel(QMatrix4x4(), logicalSize, logicalPixel, restored));
        comparePoint(restored, model);
        QCOMPARE(CCanvasCoordinates::NearestVertex(QMatrix4x4(), logicalSize, QPointF(89, 50), {-0.5f, 0.5f}), 0);
        QCOMPARE(CCanvasCoordinates::NearestVertex(QMatrix4x4(), logicalSize, QPointF(91, 50), {-0.5f, 0.5f}), -1);
    }

    void singularAndNonfiniteTransformsPreserveOutputs_data()
    {
        QTest::addColumn<QMatrix4x4>("matrix");
        QMatrix4x4 matrix;
        matrix(0, 0) = 0;
        QTest::newRow("zero-x-scale") << matrix;
        matrix.setToIdentity();
        matrix(2, 2) = 0;
        QTest::newRow("singular-full-matrix-with-invertible-plane") << matrix;
        matrix.setToIdentity();
        matrix(0, 0) = 0;
        matrix(0, 2) = 1;
        matrix(2, 0) = -1;
        matrix(2, 2) = 0;
        QTest::newRow("invertible-full-matrix-with-singular-plane") << matrix;
        matrix.setToIdentity();
        matrix(0, 0) = 1; matrix(0, 1) = 2; matrix(0, 3) = 3;
        matrix(1, 0) = 4; matrix(1, 1) = 5; matrix(1, 3) = 6;
        matrix(3, 0) = 7; matrix(3, 1) = 8; matrix(3, 3) = 9;
        QTest::newRow("linearly-dependent-rows") << matrix;
        matrix.setToIdentity();
        matrix(0, 0) = 1; matrix(0, 1) = 2; matrix(0, 2) = 3; matrix(0, 3) = 4;
        matrix(1, 0) = 5; matrix(1, 1) = 6; matrix(1, 2) = 7; matrix(1, 3) = 8;
        matrix(2, 0) = 6; matrix(2, 1) = 8; matrix(2, 2) = 10; matrix(2, 3) = 12;
        QTest::newRow("dependent-depth-row-with-invertible-plane") << matrix;
        matrix.setToIdentity();
        matrix(2, 3) = std::numeric_limits<float>::quiet_NaN();
        QTest::newRow("nan-outside-plane") << matrix;
        matrix.setToIdentity();
        matrix(1, 1) = std::numeric_limits<float>::infinity();
        QTest::newRow("infinite-scale") << matrix;
    }

    void singularAndNonfiniteTransformsPreserveOutputs()
    {
        QFETCH(QMatrix4x4, matrix);
        const QPointF sentinel(123, 456);
        QPointF result = sentinel;
        QVERIFY(!CCanvasCoordinates::ScreenToModel(matrix, QSize(200, 100), QPointF(100, 50), result));
        QCOMPARE(result, sentinel);
        QVERIFY(!CCanvasCoordinates::ModelToScreen(matrix, QSize(200, 100), QPointF(0, 0), result));
        QCOMPARE(result, sentinel);
        QCOMPARE(CCanvasCoordinates::NearestVertex(matrix, QSize(200, 100), QPointF(100, 50), {0, 0}), -1);
    }

    void invalidInputsPreserveOutputs()
    {
        const QPointF sentinel(123, 456);
        const QMatrix4x4 matrix;
        for (const QSize& size : {QSize(), QSize(0, 100), QSize(100, 0), QSize(-1, 100)}) {
            QPointF result = sentinel;
            QVERIFY(!CCanvasCoordinates::ScreenToModel(matrix, size, QPointF(1, 1), result));
            QCOMPARE(result, sentinel);
            QVERIFY(!CCanvasCoordinates::ModelToScreen(matrix, size, QPointF(1, 1), result));
            QCOMPARE(result, sentinel);
            QCOMPARE(CCanvasCoordinates::NearestVertex(matrix, size, QPointF(1, 1), {0, 0}), -1);
        }
        const double nan = std::numeric_limits<double>::quiet_NaN();
        const double infinity = std::numeric_limits<double>::infinity();
        for (const QPointF& point : {QPointF(nan, 0), QPointF(0, nan), QPointF(infinity, 0), QPointF(0, -infinity)}) {
            QPointF result = sentinel;
            QVERIFY(!CCanvasCoordinates::ScreenToModel(matrix, QSize(200, 100), point, result));
            QCOMPARE(result, sentinel);
            QVERIFY(!CCanvasCoordinates::ModelToScreen(matrix, QSize(200, 100), point, result));
            QCOMPARE(result, sentinel);
            QCOMPARE(CCanvasCoordinates::NearestVertex(matrix, QSize(200, 100), point, {0, 0}), -1);
        }
        for (double radius : {-1.0, nan, infinity})
            QCOMPARE(CCanvasCoordinates::NearestVertex(matrix, QSize(200, 100), QPointF(100, 50), {0, 0}, radius), -1);
        QCOMPARE(CCanvasCoordinates::NearestVertex(matrix, QSize(200, 100), QPointF(100, 50), {}), -1);
        QCOMPARE(CCanvasCoordinates::NearestVertex(matrix, QSize(200, 100), QPointF(100, 50), {0}), -1);
        QCOMPARE(CCanvasCoordinates::NearestVertex(matrix, QSize(200, 100), QPointF(100, 50),
                    {0, 0, std::numeric_limits<float>::quiet_NaN(), 0}), -1);
        QCOMPARE(CCanvasCoordinates::NearestVertex(matrix, QSize(200, 100), QPointF(100, 50),
                    {0, 0, 0, std::numeric_limits<float>::infinity()}), -1);
    }

    void homogeneousHorizonIsRejectedAtomically()
    {
        QMatrix4x4 matrix;
        matrix(3, 0) = 0.5f;
        QPointF result(123, 456);
        QVERIFY(!CCanvasCoordinates::ModelToScreen(matrix, QSize(200, 100), QPointF(-2, 0), result));
        QCOMPARE(result, QPointF(123, 456));
        QVERIFY(!CCanvasCoordinates::ScreenToModel(matrix, QSize(200, 100), QPointF(300, 50), result));
        QCOMPARE(result, QPointF(123, 456));
        QCOMPARE(CCanvasCoordinates::NearestVertex(matrix, QSize(200, 100), QPointF(100, 50), {0, 0, -2, 0}), -1);
    }

    void finiteExtremeScalesRemainUsable()
    {
        QMatrix4x4 matrix;
        matrix(0, 0) = std::numeric_limits<float>::max();
        matrix(1, 1) = std::numeric_limits<float>::denorm_min();
        const QPointF model(0.5 / double(matrix(0, 0)), 0.5 / double(matrix(1, 1)));
        QPointF pixel, restored;
        QVERIFY(CCanvasCoordinates::ModelToScreen(matrix, QSize(200, 100), model, pixel));
        comparePoint(pixel, QPointF(150, 25));
        QVERIFY(CCanvasCoordinates::ScreenToModel(matrix, QSize(200, 100), QPointF(150, 25), restored));
        QVERIFY(std::abs(restored.x() / model.x() - 1.0) < 1e-12);
        QVERIFY(std::abs(restored.y() / model.y() - 1.0) < 1e-12);
        QMatrix4x4 perspective;
        perspective(0, 0) = std::numeric_limits<float>::max();
        perspective(3, 0) = std::numeric_limits<float>::max();
        QVERIFY(CCanvasCoordinates::ModelToScreen(perspective, QSize(200, 100),
                                                  QPointF(std::numeric_limits<double>::max() / 16, 0), pixel));
        comparePoint(pixel, QPointF(200, 50));
        const int extent = std::numeric_limits<int>::max();
        QVERIFY(CCanvasCoordinates::ModelToScreen(QMatrix4x4(), QSize(extent, extent), QPointF(0, 0), pixel));
        QCOMPARE(pixel, QPointF(double(extent) / 2, double(extent) / 2));
    }

    void finiteOverflowDoesNotPublishPartialCoordinates()
    {
        QMatrix4x4 matrix;
        matrix(0, 0) = std::numeric_limits<float>::max();
        QPointF result(123, 456);
        QVERIFY(!CCanvasCoordinates::ModelToScreen(matrix, QSize(200, 100),
                                                   QPointF(std::numeric_limits<double>::max() / 4, 0), result));
        QCOMPARE(result, QPointF(123, 456));
        matrix(0, 0) = std::numeric_limits<float>::denorm_min();
        QVERIFY(!CCanvasCoordinates::ScreenToModel(matrix, QSize(1, 1),
                                                   QPointF(std::numeric_limits<double>::max() / 4, 0), result));
        QCOMPARE(result, QPointF(123, 456));
    }

    void homogeneousScaleAndAliasedOutputsRemainValid()
    {
        QMatrix4x4 tiny;
        tiny *= std::numeric_limits<float>::denorm_min();
        QPointF point(-0.5, 0.5);
        QVERIFY(CCanvasCoordinates::ModelToScreen(tiny, QSize(200, 100), point, point));
        comparePoint(point, QPointF(50, 25));
        QVERIFY(CCanvasCoordinates::ScreenToModel(tiny, QSize(200, 100), point, point));
        comparePoint(point, QPointF(-0.5, 0.5));
        QMatrix4x4 singular;
        singular(2, 2) = 0;
        const QPointF before = point;
        QVERIFY(!CCanvasCoordinates::ModelToScreen(singular, QSize(200, 100), point, point));
        QCOMPARE(point, before);
        QVERIFY(!CCanvasCoordinates::ScreenToModel(singular, QSize(200, 100), point, point));
        QCOMPARE(point, before);
    }

    void nearestUsesVisualDistanceAndStableTies()
    {
        const QMatrix4x4 identity;
        const QSize size(200, 100);
        const QPointF center(100, 50);
        const QVector<float> ties{0.0625f, 0, 0, 0.125f, -0.0625f, 0};
        QCOMPARE(CCanvasCoordinates::NearestVertex(identity, size, center, ties, 6), -1);
        QCOMPARE(CCanvasCoordinates::NearestVertex(identity, size, center, ties, 6.25), 0);
        QCOMPARE(CCanvasCoordinates::NearestVertex(identity, size, center, {0.125f, 0, 0.03125f, 0, 0.0625f, 0}), 1);
        QCOMPARE(CCanvasCoordinates::NearestVertex(identity, size, center, {0, 0, 0, 0}, 0), 0);
        QCOMPARE(CCanvasCoordinates::NearestVertex(identity, size, center, {0.03125f, 0}, 0), -1);
        QMatrix4x4 zoomed;
        zoomed(0, 0) = 4;
        QCOMPARE(CCanvasCoordinates::NearestVertex(zoomed, size, center, {0.03125f, 0}), -1);
        zoomed(0, 0) = 0.25f;
        QCOMPARE(CCanvasCoordinates::NearestVertex(zoomed, size, center, {0.25f, 0}), 0);
        QCOMPARE(CCanvasCoordinates::NearestVertex(identity, QSize(800, 100), QPointF(400, 50), {0.03125f, 0}), -1);
        QMatrix4x4 reflected;
        reflected(0, 0) = -1;
        reflected(0, 3) = 0.5f;
        QCOMPARE(CCanvasCoordinates::NearestVertex(reflected, size, QPointF(125, 50), {0, 0, 0.25f, 0}), 1);
    }

    void largeFiniteRadiusDoesNotOverflowDistanceComparison()
    {
        const QPointF query(1e200, -1e200);
        QCOMPARE(CCanvasCoordinates::NearestVertex(QMatrix4x4(), QSize(200, 100), query,
                                                   {0, 0, 0.5f, 0}, 2e200), 0);
        QCOMPARE(CCanvasCoordinates::NearestVertex(QMatrix4x4(), QSize(200, 100), query,
                                                   {0, 0}, 1e200), -1);
    }
    void batchProjectionUsesLogicalPixelsAndHomogeneousDivision()
    {
        QMatrix4x4 matrix;
        matrix(3, 0) = .5f;
        QVector<QPointF> output;
        QString error;
        QVERIFY2(
            CCanvasCoordinates::ProjectVertices(matrix, { 200, 100 }, { 0, 0, .5f, .25f, -1, .5f }, output, &error),
            qPrintable(error));
        QCOMPARE(output.size(), 3);
        comparePoint(output[0], { 100, 50 });
        comparePoint(output[1], { 140, 40 });
        comparePoint(output[2], { -100, 0 });
        QVector<QPointF> doubled;
        QVERIFY(CCanvasCoordinates::ProjectVertices(matrix, { 400, 200 }, { 0, 0, .5f, .25f, -1, .5f }, doubled));
        for (int i = 0; i < output.size(); ++i)
            comparePoint(doubled[i], output[i] * 2);
    }

    void batchProjectionRejectsLateFailureWithoutPartialOutput()
    {
        const QVector<QPointF> original { { 123, 456 } };
        QVector<QPointF> output = original;
        QString error;
        QMatrix4x4 horizon;
        horizon(3, 0) = .5f;
        QVERIFY(!CCanvasCoordinates::ProjectVertices(horizon, { 200, 100 }, { 0, 0, -2, 0 }, output, &error));
        QCOMPARE(output, original);
        QVERIFY(!error.isEmpty());
        QVERIFY(!CCanvasCoordinates::ProjectVertices({ }, { 200, 100 }, { 0, 0, 1 }, output, &error));
        QCOMPARE(output, original);
        QVERIFY(!CCanvasCoordinates::ProjectVertices(
            { }, { 200, 100 }, { 0, 0, std::numeric_limits<float>::quiet_NaN(), 0 }, output, &error));
        QCOMPARE(output, original);
        QMatrix4x4 singular;
        singular(2, 2) = 0;
        QVERIFY(!CCanvasCoordinates::ProjectVertices(singular, { 200, 100 }, { }, output, &error));
        QCOMPARE(output, original);
        QVERIFY(CCanvasCoordinates::ProjectVertices({ }, { 200, 100 }, { }, output, &error));
        QVERIFY(output.isEmpty());
    }
};

QTEST_GUILESS_MAIN(CanvasCoordinatesTests)
#include "canvas_coordinates_tests.moc"
