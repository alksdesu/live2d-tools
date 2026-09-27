#include "live2dstruct/crotationframegeometry.h"
#include "live2dstruct/coor/ccoordinateframe.h"
#include "live2dstruct/keyform/crotationnodekeyform.h"
#include <QtTest>
#include <cmath>
#include <limits>

using namespace live2dstruct;

class RotationFrameTests : public QObject
{
    Q_OBJECT
private slots:
    void reflectionPreservesScaleAndIsAnInvolution_data()
    {
        QTest::addColumn<QPointF>("normal");
        QTest::newRow("horizontal") << QPointF(1, 0);
        QTest::newRow("vertical") << QPointF(0, 1);
        QTest::newRow("diagonal") << QPointF(3, -4);
        QTest::newRow("large-normal") << QPointF(std::numeric_limits<double>::max(), std::numeric_limits<double>::max());
        QTest::newRow("small-normal") << QPointF(std::numeric_limits<double>::denorm_min(), 0);
    }

    void reflectionPreservesScaleAndIsAnInvolution()
    {
        QFETCH(QPointF, normal);
        const CRotationFrameGeometry::Frame original{{0.8, -0.4}, {0.6, 0.8}, {-0.8, 0.6}, 0.7};
        CRotationFrameGeometry::Frame mirrored, restored;
        QString error;
        QVERIFY2(CRotationFrameGeometry::Mirror(original, {0.2, 0.3}, normal, mirrored, &error), qPrintable(error));
        QCOMPARE(mirrored.scale, original.scale);
        QVERIFY(mirrored.xAxis.x() * mirrored.yAxis.y() - mirrored.xAxis.y() * mirrored.yAxis.x() < 0);
        QVERIFY(CRotationFrameGeometry::Mirror(mirrored, {0.2, 0.3}, normal, restored, &error));
        QVERIFY2(CRotationFrameGeometry::Matches(restored, original, &error), qPrintable(error));
    }

    void verticalMirrorReflectsOriginAndBothDirections()
    {
        CRotationFrameGeometry::Frame actual;
        QVERIFY(CRotationFrameGeometry::Mirror({{3, 4}, {1, 0}, {0, 1}, 0}, {2, 9}, {1, 0}, actual));
        QCOMPARE(actual.origin, QPointF(1, 4));
        QCOMPARE(actual.xAxis, QPointF(-1, 0));
        QCOMPARE(actual.yAxis, QPointF(0, 1));
        QCOMPARE(actual.scale, 0.0);
    }

    void movingTheLinePointAlongItsTangentPreservesTheReflection()
    {
        const CRotationFrameGeometry::Frame original{{3, 0.4}, {1, 0}, {0, 1}, 1};
        CRotationFrameGeometry::Frame actual;
        QVERIFY(CRotationFrameGeometry::Mirror(original, {2, 1e20}, {1, 0}, actual));
        QCOMPARE(actual.origin.x(), 1.0);
        QCOMPARE(actual.origin.y(), original.origin.y());
    }

    void rejectedFramesAndLinesPreserveOutput()
    {
        using Frame = CRotationFrameGeometry::Frame;
        const double nan = std::numeric_limits<double>::quiet_NaN();
        const QVector<Frame> invalid{
            {{nan, 0}, {1, 0}, {0, 1}, 1}, {{0, 0}, {2, 0}, {0, 1}, 1},
            {{0, 0}, {1, 0}, {1, 0}, 1}, {{0, 0}, {1, 0}, {0, 1}, -1},
            {{0, 0}, {1, 0}, {0, 1}, nan}
        };
        const Frame sentinel{{7, 8}, {-1, 0}, {0, 1}, 2};
        for (const Frame& value : invalid) {
            Frame output = sentinel;
            QString error;
            QVERIFY(!CRotationFrameGeometry::Mirror(value, {}, {1, 0}, output, &error));
            QVERIFY(!error.isEmpty());
            QCOMPARE(output.origin, sentinel.origin); QCOMPARE(output.xAxis, sentinel.xAxis);
            QCOMPARE(output.yAxis, sentinel.yAxis); QCOMPARE(output.scale, sentinel.scale);
        }
        for (const QPointF normal : {QPointF(), QPointF(nan, 1)}) {
            Frame output = sentinel;
            QVERIFY(!CRotationFrameGeometry::Mirror(Frame{}, {}, normal, output));
            QCOMPARE(output.origin, sentinel.origin); QCOMPARE(output.scale, sentinel.scale);
        }
        Frame output = sentinel;
        QVERIFY(!CRotationFrameGeometry::Mirror(Frame{}, {nan, 0}, {1, 0}, output));
        QCOMPARE(output.origin, sentinel.origin);
    }

    void resolvedRotationSharesFactoryResults()
    {
        std::shared_ptr<const CCoordinateFrame> parent;
        CRotationNodeKeyform parentKey(1, 31, 0.2f, 0.4f, -0.7f, true, false);
        CCoordinateFrame::RotationBasis ignored;
        QVERIFY(CCoordinateFrame::CreateRotation(17, parentKey, {}, parent, ignored));
        for (float scale : {-2.0f, -0.0f, 0.0f, 2.0f}) for (int reflection = 0; reflection < 4; ++reflection) {
            CRotationNodeKeyform key(1, 771, -0.2f, 0.3f, scale, reflection & 1, reflection & 2);
            CCoordinateFrame::ResolvedRotation resolved;
            QVERIFY(CCoordinateFrame::ResolveRotation(-19, key, parent, resolved));
            std::shared_ptr<const CCoordinateFrame> frame;
            CCoordinateFrame::RotationBasis basis;
            QVERIFY(CCoordinateFrame::CreateRotation(-19, key, parent, frame, basis));
            QCOMPARE(basis.origin, resolved.basis.origin);
            QCOMPARE(basis.xAxis, resolved.basis.xAxis);
            QCOMPARE(basis.yAxis, resolved.basis.yAxis);
            QCOMPARE(resolved.parentSignedScale, -0.7f);
            QCOMPARE(resolved.signedScale, float(double(scale) * -0.7f));
            std::vector<float> points;
            QVERIFY(frame->MapToCanvas({0, 0, 1, 0, 0, 1}, points));
            const double magnitude = std::abs(double(resolved.signedScale));
            for (int vertex = 0; vertex < 3; ++vertex) {
                const QPointF expected = basis.origin + (vertex == 1 ? basis.xAxis * magnitude :
                    vertex == 2 ? basis.yAxis * magnitude : QPointF());
                QVERIFY(std::hypot(expected.x() - points[vertex * 2], expected.y() - points[vertex * 2 + 1]) < 0.000001);
            }
        }
    }

    void collapsedParentLeavesResolvedOutputUntouched()
    {
        std::shared_ptr<const CCoordinateFrame> parent;
        QVERIFY(CCoordinateFrame::CreateWarp(1, 1, {2, 3, 2, 3, 2, 3, 2, 3}, false, {}, parent));
        CRotationNodeKeyform key(1, 0, 0.2f, 0.4f, 1, false, false);
        CCoordinateFrame::ResolvedRotation output{{{7, 8}, {1, 2}, {3, 4}}, 5, 6, 7, 8};
        QVERIFY(!CCoordinateFrame::ResolveRotation(0, key, parent, output));
        QCOMPARE(output.basis.origin, QPointF(7, 8));
        QCOMPARE(output.signedScale, 6.0f);
        QCOMPARE(output.parentAngle, 7.0);
        QCOMPARE(output.parentSignedScale, 8.0f);
    }
};

QTEST_GUILESS_MAIN(RotationFrameTests)
#include "rotation_frame_tests.moc"
