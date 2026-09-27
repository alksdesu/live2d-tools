#include "live2dstruct/ccanvasgeometry.h"
#include "live2dstruct/csectiontable.h"
#include <QtTest>
#include <limits>

using namespace live2dstruct;

class CanvasGeometryInverseTests : public QObject
{
    Q_OBJECT
private slots:
    void pixelOriginAndScaleRoundTrip()
    {
        CReader reader(QString(TEST_RESOURCES) + "/Mark/Mark.moc3");
        CSectionTable table(reader);
        QVERIFY(table.IsValid());
        auto& canvas = *table.canvas_info;
        canvas.origin_x = 713.25f; canvas.origin_y = -39.5f; canvas.pixels_per_unit = 1200;
        for (const QPointF model : {QPointF(), QPointF(-0.5, 0.2), QPointF(19, -21)}) {
            QPointF pixels, restored;
            QVERIFY(CCanvasGeometry::ModelToPixels(&canvas, model, pixels));
            QVERIFY(CCanvasGeometry::PixelsToModel(&canvas, pixels, restored));
            QCOMPARE(restored, model);
        }
        QPointF origin;
        QVERIFY(CCanvasGeometry::PixelsToModel(&canvas, {713.25, -39.5}, origin));
        QCOMPARE(origin, QPointF());
        QVERIFY(CCanvasGeometry::PixelsToModel(&canvas, {1913.25, -1239.5}, origin));
        QCOMPARE(origin, QPointF(1, 1));
    }

    void invalidInputAndOverflowPreserveOutput()
    {
        CReader reader(QString(TEST_RESOURCES) + "/Mark/Mark.moc3");
        CSectionTable table(reader);
        QVERIFY(table.IsValid());
        auto& canvas = *table.canvas_info;
        QPointF output(7, 9);
        QVERIFY(!CCanvasGeometry::PixelsToModel(nullptr, {}, output));
        QVERIFY(!CCanvasGeometry::PixelsToModel(&canvas, {std::numeric_limits<double>::infinity(), 0}, output));
        QCOMPARE(output, QPointF(7, 9));
        canvas.pixels_per_unit = 0;
        QVERIFY(!CCanvasGeometry::PixelsToModel(&canvas, {}, output));
        QCOMPARE(output, QPointF(7, 9));
        canvas.pixels_per_unit = std::numeric_limits<float>::denorm_min();
        QVERIFY(!CCanvasGeometry::PixelsToModel(&canvas, {std::numeric_limits<double>::max(), 0}, output));
        QCOMPARE(output, QPointF(7, 9));
    }
};

QTEST_GUILESS_MAIN(CanvasGeometryInverseTests)
#include "canvas_geometry_inverse_tests.moc"
