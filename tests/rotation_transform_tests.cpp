#include "live2dstruct/coor/crotationcoortype.h"

#include <QtTest>
#include <QTransform>
#include <algorithm>
#include <cmath>
#include <limits>

using live2dstruct::CRotationCoorType;
using live2dstruct::CRotationNodeKeyform;

namespace {
bool Near(float actual, double expected)
{
    return std::abs(actual - expected) <= 0.00001 * std::max(1.0, std::abs(expected));
}

QByteArray Bytes(const QVector<float>& values)
{
    return QByteArray(reinterpret_cast<const char*>(values.constData()), values.size() * sizeof(float));
}
}

class RotationTransformTests : public QObject
{
    Q_OBJECT
private slots:
    void onlyExactIdentityIsEligibleForSkipping()
    {
        CRotationNodeKeyform key;
        CRotationCoorType identity(0, key);
        QVERIFY(identity.IsIdentity());
        key.Setangles(360);
        QVERIFY(CRotationCoorType(0, key).IsIdentity());
        key.Setangles(0.000001f);
        QVERIFY(!CRotationCoorType(0, key).IsIdentity());
        key.Setangles(0);
        key.Setscales(std::nextafter(1.0f, 2.0f));
        QVERIFY(!CRotationCoorType(0, key).IsIdentity());
        key.Setscales(0);
        QVERIFY(!CRotationCoorType(0, key).IsIdentity());
        key.Setscales(-1);
        key.Setis_reflect_x(true);
        QVERIFY(!CRotationCoorType(0, key).IsIdentity());
        key.Setis_reflect_y(true);
        QVERIFY(CRotationCoorType(0, key).IsIdentity());
        key.Setorigin_x(std::numeric_limits<float>::denorm_min());
        QVERIFY(!CRotationCoorType(0, key).IsIdentity());
        key.Setorigin_x(std::numeric_limits<float>::infinity());
        QVERIFY(!CRotationCoorType(0, key).IsIdentity());
        QVERIFY(!CRotationCoorType(std::numeric_limits<float>::quiet_NaN(), key).IsIdentity());
        float values[]{2, -3};
        QVERIFY(identity.transformLocalToCanvas(values, values, 1, 0, 2));
        QCOMPARE(values[0], 2.0f);
        QCOMPARE(values[1], -3.0f);
    }

    void defaultsAndExplicitReflectionAreCopied()
    {
        CRotationNodeKeyform defaults;
        QCOMPARE(defaults.Getopacities(), 1.0f);
        QCOMPARE(defaults.Getangles(), 0.0f);
        QCOMPARE(defaults.Getscales(), 1.0f);
        QCOMPARE(defaults.Getorigin_x(), 0.0f);
        QCOMPARE(defaults.Getorigin_y(), 0.0f);
        QVERIFY(!defaults.Getis_reflect_x());
        QVERIFY(!defaults.Getis_reflect_y());
        CRotationCoorType identity(0, defaults);
        QVector<float> input{2, -3}, output(2, 99);
        QVERIFY(identity.transformLocalToCanvas(input.data(), output.data(), 1, 0, 2));
        QCOMPARE(output, input);
        QVERIFY(identity.transformCanvasToLocal(output.data(), output.data(), 1));
        QCOMPARE(output, input);

        CRotationNodeKeyform explicitKey(0.25f, 20, 3, 4, -2, true, false);
        CRotationNodeKeyform copied = explicitKey;
        CRotationNodeKeyform assigned;
        assigned = explicitKey;
        explicitKey.Setis_reflect_x(false);
        explicitKey.Setis_reflect_y(true);
        explicitKey.Setorigin_x(7);
        QCOMPARE(copied.Getopacities(), 0.25f);
        QVERIFY(copied.Getis_reflect_x());
        QVERIFY(!copied.Getis_reflect_y());
        QVERIFY(assigned.Getis_reflect_x());
        QVERIFY(!assigned.Getis_reflect_y());
        QCOMPARE(copied.Getorigin_x(), 3.0f);
        CRotationCoorType transform(70, copied);
        QCOMPARE(transform.GetAngle_forRotation(), 90.0);
        QCOMPARE(transform.GetCalculatedTotalScale(), -2.0f);
        QVERIFY(transform.GetisReflectX());
        QVERIFY(!transform.GetisReflectY());
        copied.Setis_reflect_x(false);
        QVERIFY(transform.GetisReflectX());
    }

    void quarterTurnReflectionAndNegativeScaleRoundTrip()
    {
        for (bool reflectX : {false, true}) {
            for (bool reflectY : {false, true}) {
                for (float scale : {0.25f, 2.0f, -2.0f}) {
                    CRotationNodeKeyform key(1, 55, 3, -4, scale, reflectX, reflectY);
                    CRotationCoorType transform(35, key);
                    QVector<float> local{2, -1, -3, 4}, canvas(4, 0), restored(4, 0);
                    QVERIFY(transform.transformLocalToCanvas(local.data(), canvas.data(), 2, 0, 2));
                    const float signX = reflectX ? -1.0f : 1.0f;
                    const float signY = reflectY ? -1.0f : 1.0f;
                    QVERIFY(Near(canvas[0], 3 + scale * signY));
                    QVERIFY(Near(canvas[1], -4 + 2 * scale * signX));
                    QVERIFY(Near(canvas[2], 3 - 4 * scale * signY));
                    QVERIFY(Near(canvas[3], -4 - 3 * scale * signX));
                    QVERIFY(transform.transformCanvasToLocal(canvas.data(), restored.data(), 2));
                    for (int i = 0; i < local.size(); ++i) QVERIFY(Near(restored[i], local[i]));
                }
            }
        }
    }

    void arbitraryAnglesMatchDegreeTransform()
    {
        for (float angle : {-450.0f, -31.25f, 0.0f, 180.0f, 360.0f, 720.0f}) {
            CRotationNodeKeyform key(1, angle, -3, 5, 1.25f, true, false);
            CRotationCoorType transform(12.5f, key);
            QTransform expected;
            expected.translate(-3, 5);
            expected.rotate(double(angle) + 12.5);
            expected.scale(-1.25, 1.25);
            QVector<float> local{2, -1, -3, 4}, canvas(4, 0);
            QVERIFY(transform.transformLocalToCanvas(local.data(), canvas.data(), 2, 0, 2));
            for (int i = 0; i < local.size(); i += 2) {
                const QPointF point = expected.map(QPointF(local[i], local[i + 1]));
                QVERIFY(Near(canvas[i], point.x()));
                QVERIFY(Near(canvas[i + 1], point.y()));
            }
            QVERIFY(transform.transformCanvasToLocal(canvas.data(), canvas.data(), 2));
            for (int i = 0; i < local.size(); ++i) QVERIFY(Near(canvas[i], local[i]));
        }
    }

    void offsetAndStrideOnlyWriteRequestedPoints()
    {
        CRotationNodeKeyform key(1, 90, 3, -4, 2, false, false);
        CRotationCoorType transform(0, key);
        QVector<float> source{91, 92, 93, 94, 95, 2, -1, 96, 97, -3, 4};
        QVector<float> destination(source.size(), 88);
        QVERIFY(transform.transformLocalToCanvas(source.data(), destination.data(), 2, 5, 4));
        for (int index : {0, 1, 2, 3, 4, 7, 8}) QCOMPARE(destination[index], 88.0f);
        QVERIFY(Near(destination[5], 5));
        QVERIFY(Near(destination[6], 0));
        QVERIFY(Near(destination[9], -5));
        QVERIFY(Near(destination[10], -10));
        const auto original = source;
        QVERIFY(transform.transformLocalToCanvas(source.data(), source.data(), 2, 5, 4));
        for (int index : {0, 1, 2, 3, 4, 7, 8}) QCOMPARE(source[index], original[index]);
        for (int index : {5, 6, 9, 10}) QVERIFY(Near(source[index], destination[index]));
    }

    void partiallyOverlappingBuffersUseOriginalInput()
    {
        CRotationNodeKeyform key(1, 90, 3, -4, 2, false, true);
        CRotationCoorType transform(0, key);
        QVector<float> local{2, -1, -3, 4}, canvas(4, 0);
        QVERIFY(transform.transformLocalToCanvas(local.data(), canvas.data(), 2, 0, 2));
        for (bool inverse : {false, true}) {
            const auto& input = inverse ? canvas : local;
            const auto& expected = inverse ? local : canvas;
            for (int shift : {-1, 0, 1}) {
                QVector<float> shared(6, 77);
                const int sourceStart = shift < 0 ? 1 : 0;
                const int destinationStart = sourceStart + shift;
                for (int i = 0; i < input.size(); ++i) shared[sourceStart + i] = input[i];
                const bool success = inverse
                    ? transform.transformCanvasToLocal(shared.data() + sourceStart, shared.data() + destinationStart, 2)
                    : transform.transformLocalToCanvas(shared.data() + sourceStart, shared.data() + destinationStart, 2, 0, 2);
                QVERIFY(success);
                for (int i = 0; i < expected.size(); ++i) QVERIFY(Near(shared[destinationStart + i], expected[i]));
                QCOMPARE(shared[5], 77.0f);
            }
        }
    }

    void zeroScaleHasNoInverse()
    {
        CRotationNodeKeyform key(1, 90, 3, -4, 0, true, true);
        CRotationCoorType transform(0, key);
        QVector<float> source{2, -1, -3, 4}, destination(4, 88);
        QVERIFY(transform.transformLocalToCanvas(source.data(), destination.data(), 2, 0, 2));
        QCOMPARE(destination, (QVector<float>{3, -4, 3, -4}));
        destination.fill(88);
        QVERIFY(!transform.transformCanvasToLocal(source.data(), destination.data(), 2));
        QCOMPARE(destination, QVector<float>(4, 88));
    }

    void failedBatchLeavesAllOutputUntouched()
    {
        CRotationNodeKeyform key(1, 0, 0, 0, 2, false, false);
        CRotationCoorType transform(0, key);
        const float infinity = std::numeric_limits<float>::infinity();
        const float nan = std::numeric_limits<float>::quiet_NaN();
        const float maximum = std::numeric_limits<float>::max();
        for (float invalid : {infinity, nan, maximum}) {
            QVector<float> source{2, 3, invalid, 4}, output(4, 88);
            QVERIFY(!transform.transformLocalToCanvas(source.data(), output.data(), 2, 0, 2));
            QCOMPARE(output, QVector<float>(4, 88));
            source.append(77);
            const auto original = Bytes(source);
            QVERIFY(!transform.transformLocalToCanvas(source.data(), source.data() + 1, 2, 0, 2));
            QCOMPARE(Bytes(source), original);
        }
        key.Setscales(0.5f);
        CRotationCoorType small(0, key);
        for (float invalid : {infinity, nan, maximum}) {
            QVector<float> source{2, 3, invalid, 4}, output(4, 88);
            QVERIFY(!small.transformCanvasToLocal(source.data(), output.data(), 2));
            QCOMPARE(output, QVector<float>(4, 88));
            source.append(77);
            const auto original = Bytes(source);
            QVERIFY(!small.transformCanvasToLocal(source.data(), source.data() + 1, 2));
            QCOMPARE(Bytes(source), original);
        }
    }

    void invalidArgumentsAndParametersAreRejected()
    {
        CRotationNodeKeyform key;
        CRotationCoorType transform(0, key);
        QVector<float> source{2, 3}, output(2, 88);
        QVERIFY(transform.transformLocalToCanvas(nullptr, nullptr, 0, 0, 2));
        QVERIFY(transform.transformCanvasToLocal(nullptr, nullptr, 0));
        for (const auto& arguments : {QVector<int>{-1, 0, 2}, QVector<int>{1000001, 0, 2},
                                      QVector<int>{1, -1, 2}, QVector<int>{1, 0, 1},
                                      QVector<int>{1, std::numeric_limits<int>::max(), 2},
                                      QVector<int>{2, 0, std::numeric_limits<int>::max()}}) {
            QVERIFY(!transform.transformLocalToCanvas(source.data(), output.data(), arguments[0], arguments[1], arguments[2]));
            QCOMPARE(output, QVector<float>(2, 88));
        }
        QVERIFY(!transform.transformLocalToCanvas(nullptr, output.data(), 1, 0, 2));
        QVERIFY(!transform.transformLocalToCanvas(source.data(), nullptr, 1, 0, 2));
        QVERIFY(!transform.transformCanvasToLocal(nullptr, output.data(), 1));
        QVERIFY(!transform.transformCanvasToLocal(source.data(), nullptr, 1));
        QVERIFY(!transform.transformCanvasToLocal(source.data(), output.data(), -1));
        QVERIFY(!transform.transformCanvasToLocal(source.data(), output.data(), 1000001));
        const float invalid = std::numeric_limits<float>::infinity();
        for (int field = 0; field < 6; ++field) {
            CRotationNodeKeyform candidate;
            if (field == 1) candidate.Setangles(invalid);
            if (field == 2) candidate.Setscales(invalid);
            if (field == 3) candidate.Setorigin_x(invalid);
            if (field == 4) candidate.Setorigin_y(invalid);
            if (field == 5) candidate.GetPosition().clear();
            CRotationCoorType rejected(field == 0 ? invalid : 0, candidate);
            QVERIFY(!rejected.transformLocalToCanvas(source.data(), output.data(), 1, 0, 2));
            QVERIFY(!rejected.transformCanvasToLocal(source.data(), output.data(), 1));
            QCOMPARE(output, QVector<float>(2, 88));
        }
    }

    void finiteExtremeAnglesAndScalesRemainUsable()
    {
        const float maximum = std::numeric_limits<float>::max();
        CRotationNodeKeyform key(1, maximum, 0, 0, 1, false, false);
        CRotationCoorType largeAngle(maximum, key);
        QVERIFY(std::isfinite(largeAngle.GetAngle_forRotation()));
        QVector<float> source{1, 0}, output(2, 0);
        QVERIFY(largeAngle.transformLocalToCanvas(source.data(), output.data(), 1, 0, 2));
        QVERIFY(largeAngle.transformCanvasToLocal(output.data(), output.data(), 1));
        QVERIFY(Near(output[0], 1));
        QVERIFY(Near(output[1], 0));
        const float tiny = std::numeric_limits<float>::denorm_min() * 16;
        key.Setangles(0);
        key.Setscales(tiny);
        CRotationCoorType tinyScale(0, key);
        QVERIFY(tinyScale.transformLocalToCanvas(source.data(), output.data(), 1, 0, 2));
        QCOMPARE(output[0], tiny);
        QVERIFY(tinyScale.transformCanvasToLocal(output.data(), output.data(), 1));
        QCOMPARE(output, source);
    }
};

QTEST_GUILESS_MAIN(RotationTransformTests)
#include "rotation_transform_tests.moc"
