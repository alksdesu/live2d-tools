#include "live2dstruct/cwarpgridresampler.h"
#include "std/cexportcontrol.h"
#include <QtTest>
#include <cmath>
#include <cstring>
#include <limits>

using Sampler = live2dstruct::CWarpGridResampler;

namespace {
QVector<float> regular(int rows, int columns)
{
    QVector<float> result;
    for (int row = 0; row <= rows; ++row)
        for (int column = 0; column <= columns; ++column)
            result << float(column) / columns << float(row) / rows;
    return result;
}
}

class WarpGridResamplerTests : public QObject
{
    Q_OBJECT
private slots:
    void quadAndTriangularSamplingHaveDistinctCenters()
    {
        const QVector<float> source { 0, 0, 2, 0, 0, 2, 4, 4 };
        Sampler::Plan triangle, quad;
        QVERIFY(Sampler::Prepare(1, 1, false, 2, 2, triangle));
        QVERIFY(Sampler::Prepare(1, 1, true, 2, 2, quad));
        QVector<float> a, b;
        QVERIFY(Sampler::Resample(triangle, source, a));
        QVERIFY(Sampler::Resample(quad, source, b));
        QCOMPARE(a.mid(8, 2), QVector<float>({ 1, 1 }));
        QCOMPARE(b.mid(8, 2), QVector<float>({ 1.5f, 1.5f }));
        QCOMPARE(a.last(), 4.f);
        QCOMPARE(b.last(), 4.f);
    }

    void nonSquareSamplingKeepsRowMajorCoordinates()
    {
        Sampler::Plan plan;
        QVERIFY(Sampler::Prepare(2, 3, true, 7, 5, plan));
        QVector<float> result;
        QVERIFY(Sampler::Resample(plan, regular(2, 3), result));
        QCOMPARE(result.size(), 8 * 6 * 2);
        for (int row = 0; row <= 7; ++row)
            for (int column = 0; column <= 5; ++column) {
                const int index = (row * 6 + column) * 2;
                QVERIFY(std::abs(result[index] - double(column) / 5) < 1e-7);
                QVERIFY(std::abs(result[index + 1] - double(row) / 7) < 1e-7);
            }
    }

    void survivingNodesKeepExactFloatBits()
    {
        Sampler::Plan plan;
        QVERIFY(Sampler::Prepare(2, 2, true, 4, 6, plan));
        auto source = regular(2, 2);
        source[0] = -0.f;
        source[5] = std::nextafter(1.f, 2.f);
        QVector<float> result;
        QVERIFY(Sampler::Resample(plan, source, result));
        QCOMPARE(plan.OldToNew().size(), 9);
        for (int index = 0; index < 9; ++index) {
            const int mapped = plan.OldToNew()[index];
            QCOMPARE(mapped, (index / 3) * 2 * 7 + (index % 3) * 3);
            QVERIFY(std::memcmp(source.constData() + index * 2, result.constData() + mapped * 2, 8) == 0);
        }
    }

    void unchangedGridIsAnExactOwnedCopy()
    {
        Sampler::Plan plan;
        QVERIFY(Sampler::Prepare(3, 5, false, 3, 5, plan));
        QVERIFY(plan.IsUnchanged());
        auto source = regular(3, 5);
        source[0] = -0.f;
        QVector<float> result;
        QVERIFY(Sampler::Resample(plan, source, result));
        QVERIFY(result.constData() != source.constData());
        QVERIFY(std::memcmp(source.constData(), result.constData(), source.size() * sizeof(float)) == 0);
    }

    void reductionDoesNotInventSurvivingNodeIdentities()
    {
        Sampler::Plan plan;
        QVERIFY(Sampler::Prepare(4, 6, false, 2, 3, plan));
        for (int row = 0; row <= 4; ++row)
            for (int column = 0; column <= 6; ++column)
                QCOMPARE(plan.OldToNew()[row * 7 + column], row % 2 || column % 2 ? -1 : (row / 2) * 4 + column / 2);
    }

    void exactInteriorClassification_data()
    {
        QTest::addColumn<bool>("quad");
        QTest::addColumn<int>("rows");
        QTest::addColumn<int>("columns");
        QTest::addColumn<bool>("exact");
        QTest::newRow("unchanged") << false << 2 << 3 << true;
        QTest::newRow("triangle-uniform") << false << 4 << 6 << true;
        QTest::newRow("triangle-anisotropic") << false << 4 << 9 << false;
        QTest::newRow("quad-anisotropic") << true << 4 << 9 << true;
        QTest::newRow("quad-nonaligned") << true << 3 << 4 << false;
        QTest::newRow("reduction") << true << 1 << 1 << false;
    }
    void exactInteriorClassification()
    {
        QFETCH(bool, quad);
        QFETCH(int, rows);
        QFETCH(int, columns);
        QFETCH(bool, exact);
        Sampler::Plan plan;
        QVERIFY(Sampler::Prepare(2, 3, quad, rows, columns, plan));
        QCOMPARE(plan.ExactInteriorSubdivision(), exact);
    }

    void generatedWeightsAreFiniteNormalizedAndInRange()
    {
        for (bool quad : { false, true }) {
            Sampler::Plan plan;
            QVERIFY(Sampler::Prepare(17, 23, quad, 31, 29, plan));
            for (const auto& influence : plan.Influences()) {
                QVERIFY(influence.count >= 1 && influence.count <= 4);
                double sum = 0;
                for (int term = 0; term < influence.count; ++term) {
                    QVERIFY(influence.indices[term] >= 0 && influence.indices[term] < 18 * 24);
                    QVERIFY(std::isfinite(influence.weights[term]) && influence.weights[term] > 0);
                    sum += influence.weights[term];
                }
                QVERIFY(std::abs(sum - 1) < 1e-14);
            }
        }
    }

    void blendDeltasUseTheSameLinearWeights()
    {
        Sampler::Plan plan;
        QVERIFY(Sampler::Prepare(1, 1, true, 4, 4, plan));
        const QVector<float> shape { 3, 1, 4, 2, 5, 4, 8, 9 };
        const QVector<float> delta { -2, 1, 0, -3, 2, -1, -4, 3 };
        auto combined = shape;
        for (int index = 0; index < combined.size(); ++index)
            combined[index] += delta[index] * .5f;
        QVector<float> a, b, c;
        QVERIFY(Sampler::Resample(plan, shape, a));
        QVERIFY(Sampler::Resample(plan, delta, b));
        QVERIFY(Sampler::Resample(plan, combined, c));
        for (int index = 0; index < c.size(); ++index)
            QCOMPARE(c[index], a[index] + b[index] * .5f);
    }

    void foldedAndExtremeFiniteSourcesCanBeSampled()
    {
        Sampler::Plan plan;
        QVERIFY(Sampler::Prepare(1, 1, true, 3, 3, plan));
        QVector<float> result;
        QVERIFY(Sampler::Resample(plan, { 0, 0, 1, 1, 1, 0, 0, 1 }, result));
        const float extreme = std::numeric_limits<float>::max();
        QVERIFY(Sampler::Resample(
            plan, { extreme, -extreme, extreme, extreme, -extreme, -extreme, -extreme, extreme }, result));
        for (float value : result)
            QVERIFY(std::isfinite(value));
    }

    void rationalWeightsPreserveExactLargeCancellation()
    {
        Sampler::Plan plan;
        QVERIFY(Sampler::Prepare(1, 1, true, 3, 3, plan));
        const float extreme = std::numeric_limits<float>::max();
        QVector<float> result;
        QVERIFY(Sampler::Resample(plan, { extreme, 0, -extreme, 0, -extreme, 0, 0, 0 }, result));
        qInfo() << "rational zero at (1/3,1/3):" << result[10];
        QVERIFY(result[10] == 0.f);
    }

    void triangularRationalWeightsPreserveExactLargeCancellation()
    {
        Sampler::Plan plan;
        QVERIFY(Sampler::Prepare(1, 1, false, 5, 3, plan));
        const float extreme = std::numeric_limits<float>::max();
        const float twoThirds = std::ldexp(float(11184810), 104);
        QVector<float> result;
        QVERIFY(Sampler::Resample(plan, { 0, 0, -twoThirds, 0, 0, 0, extreme, 0 }, result));
        QVERIFY(result[34] == 0.f);
    }

    void smallTermsSurviveLargeCancellation()
    {
        Sampler::Plan plan;
        QVERIFY(Sampler::Prepare(1, 1, true, 2, 2, plan));
        const float extreme = std::numeric_limits<float>::max();
        QVector<float> result;
        QVERIFY(Sampler::Resample(plan, { extreme, 0, 1, 0, -extreme, 0, 0, 0 }, result));
        QVERIFY(result[8] == .25f);
    }

    void invalidLayoutsKeepPreviousPlan_data()
    {
        QTest::addColumn<int>("rows");
        QTest::addColumn<int>("columns");
        QTest::newRow("zero") << 0 << 2;
        QTest::newRow("negative") << 2 << -1;
        QTest::newRow("dimension") << 1001 << 1;
        QTest::newRow("coordinate-pool") << 1000 << 499;
        QTest::newRow("integer-overflow") << std::numeric_limits<int>::max() << std::numeric_limits<int>::max();
    }
    void invalidLayoutsKeepPreviousPlan()
    {
        QFETCH(int, rows);
        QFETCH(int, columns);
        Sampler::Plan plan;
        QVERIFY(Sampler::Prepare(1, 1, true, 2, 3, plan));
        QString error;
        QVERIFY(!Sampler::Prepare(1, 1, true, rows, columns, plan, &error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(plan.Rows(), 2);
        QCOMPARE(plan.Columns(), 3);
        QVERIFY(!Sampler::Prepare(rows, columns, true, 1, 1, plan));
        QCOMPARE(plan.Rows(), 2);
    }

    void maximumDimensionCanUseSmallOtherAxis()
    {
        Sampler::Plan plan;
        QVERIFY(Sampler::Prepare(1, 1000, true, 1000, 1, plan));
        QCOMPARE(plan.Influences().size(), 2002);
    }

    void invalidCoordinatesKeepPreviousOutput()
    {
        Sampler::Plan plan;
        QVERIFY(Sampler::Prepare(1, 1, true, 2, 2, plan));
        QVector<float> output { 42 };
        QVERIFY(!Sampler::Resample(plan, { 0, 1 }, output));
        QCOMPARE(output, QVector<float>({ 42 }));
        auto source = regular(1, 1);
        source[2] = std::numeric_limits<float>::quiet_NaN();
        QVERIFY(!Sampler::Resample(plan, source, output));
        QCOMPARE(output, QVector<float>({ 42 }));
        QVERIFY(!Sampler::Resample({ }, regular(1, 1), output));
        QCOMPARE(output, QVector<float>({ 42 }));
    }

    void cancellationKeepsPlanAndOutput()
    {
        Sampler::Plan plan;
        QVERIFY(Sampler::Prepare(1, 1, true, 2, 2, plan));
        CExportControl control;
        int calls = 0;
        QVERIFY(control.SetHandler([&](const auto&, QString*) {
            if (++calls == 3)
                control.Cancel();
            return true;
        }));
        QVERIFY(!Sampler::Prepare(10, 10, true, 100, 100, plan, nullptr, &control));
        QCOMPARE(plan.Rows(), 2);
        QVERIFY(control.WasCanceled());
        CExportControl second;
        QVERIFY(second.SetHandler([&](const auto&, QString*) {
            second.Cancel();
            return true;
        }));
        QVector<float> output { 42 };
        QVERIFY(!Sampler::Resample(plan, regular(1, 1), output, nullptr, &second));
        QCOMPARE(output, QVector<float>({ 42 }));
        QVERIFY(second.WasCanceled());
    }

    void callbacksCannotChangeFrozenInputs()
    {
        Sampler::Plan plan;
        QVERIFY(Sampler::Prepare(1, 1, true, 2, 2, plan));
        auto source = regular(1, 1);
        auto* alias = source.data();
        CExportControl control;
        bool called = false;
        QVERIFY(control.SetHandler([&](const auto&, QString*) {
            if (!called) {
                called = true;
                alias[0] = 123;
                plan = { };
            }
            return true;
        }));
        QVector<float> output;
        QVERIFY(Sampler::Resample(plan, source, output, nullptr, &control));
        QCOMPARE(output, regular(2, 2));
        QVERIFY(called);
        QVERIFY(!plan.IsValid());
    }
};
QTEST_GUILESS_MAIN(WarpGridResamplerTests)
#include "warp_grid_resampler_tests.moc"
