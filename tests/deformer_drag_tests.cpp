#include "live2d/cdeformerdrag.h"

#include <QtTest>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace {
QVector<CDeformerDrag::Parameter> parameters(int count)
{
    QVector<CDeformerDrag::Parameter> values;
    for (int index = 0; index < count; ++index) values.append({QString::number(index), -1, 1, 0});
    return values;
}

struct ProbeRuntime
{
    QVector<float> original, current, saved{0.125f, 0.375f};
    CDeformerDrag::Vertices baseline;
    QVector<QVector<int>> affected;
    QVector<QVector<float>> inputs;
    QString failure;
    int restores = 0;
    bool isolatedSamples = true;

    ProbeRuntime(int parameterCount, int drawableCount)
        : original(parameterCount, 0), current(original),
          baseline(drawableCount, QVector<float>{0, 0, 1, 1}), affected(parameterCount)
    {}

    bool Evaluate(const QVector<float>& pose, CDeformerDrag::Vertices& vertices, QString* error)
    {
        current = pose;
        inputs.append(pose);
        vertices = baseline;
        int changes = 0;
        for (int parameter = 0; parameter < pose.size(); ++parameter) {
            if (pose[parameter] == original[parameter]) continue;
            ++changes;
            for (int drawable : affected[parameter]) vertices[drawable][2] = 2;
        }
        isolatedSamples = isolatedSamples && changes <= 1;
        if (failure.startsWith("restore")) return true;
        const bool inject = failure.startsWith("baseline") ? inputs.size() == 1 : inputs.size() == 3;
        if (!inject) return true;
        if (failure.endsWith("false")) { *error = "controlled evaluator failure"; return false; }
        if (failure.endsWith("exception")) throw std::runtime_error("controlled evaluator exception");
        if (failure.endsWith("int")) throw 7;
        if (failure.endsWith("nan")) vertices[0][0] = std::numeric_limits<float>::quiet_NaN();
        if (failure.endsWith("infinity")) vertices[0][0] = std::numeric_limits<float>::infinity();
        if (failure.endsWith("odd")) vertices[0].append(1);
        if (failure.endsWith("count")) vertices.removeLast();
        if (failure.endsWith("topology")) vertices[0].append(QVector<float>{2, 2});
        return true;
    }

    bool Restore(QString* error)
    {
        ++restores;
        current = original;
        if (failure == "restore-false") { *error = "controlled restore failure"; return false; }
        if (failure == "restore-exception") throw std::runtime_error("controlled restore exception");
        if (failure == "restore-int") throw 9;
        return true;
    }
};
}

class DeformerDragTests : public QObject
{
    Q_OBJECT
private slots:
    void finiteRangesHaveDefinedEndpointsAndMidpoints_data()
    {
        QTest::addColumn<float>("input");
        QTest::addColumn<float>("minimum");
        QTest::addColumn<float>("maximum");
        QTest::addColumn<float>("expected");
        const float limit = std::numeric_limits<float>::max();
        QTest::newRow("left") << -1.0f << -30.0f << 30.0f << -30.0f;
        QTest::newRow("right") << 1.0f << -30.0f << 30.0f << 30.0f;
        QTest::newRow("center") << 0.0f << -30.0f << 30.0f << 0.0f;
        QTest::newRow("quarter") << -0.5f << -30.0f << 30.0f << -15.0f;
        QTest::newRow("asymmetric") << 0.0f << -2.0f << 6.0f << 2.0f;
        QTest::newRow("wide-center") << 0.0f << -limit << limit << 0.0f;
        QTest::newRow("wide-quarter") << -0.5f << -limit << limit << -limit * 0.5f;
        QTest::newRow("wide-three-quarters") << 0.5f << -limit << limit << limit * 0.5f;
        QTest::newRow("large-positive") << 0.0f << limit * 0.5f << limit << limit * 0.75f;
        QTest::newRow("large-negative") << 0.0f << -limit << -limit * 0.5f << -limit * 0.75f;
        QTest::newRow("fixed") << limit << limit << limit << limit;
        QTest::newRow("far-left") << -limit << -limit << limit << -limit;
        QTest::newRow("far-right") << limit << -limit << limit << limit;
        QTest::newRow("smallest-step-midpoint") << 0.0f << 1.0f << std::nextafter(1.0f, 2.0f) << 1.0f;
    }

    void finiteRangesHaveDefinedEndpointsAndMidpoints()
    {
        QFETCH(float, input);
        QFETCH(float, minimum);
        QFETCH(float, maximum);
        QFETCH(float, expected);
        float output = 123;
        QVERIFY(CDeformerDrag::MapValue(input, minimum, maximum, output));
        QVERIFY(std::isfinite(output));
        QCOMPARE(output, expected);
        QVERIFY(output >= minimum && output <= maximum);
    }

    void invalidMappingDoesNotWriteOutput()
    {
        const float nan = std::numeric_limits<float>::quiet_NaN();
        const float infinity = std::numeric_limits<float>::infinity();
        for (const auto& values : {QVector<float>{nan, 0, 1}, {infinity, 0, 1}, {0, nan, 1},
                                   {0, 0, infinity}, {0, 1, 0}}) {
            float output = 123;
            QVERIFY(!CDeformerDrag::MapValue(values[0], values[1], values[2], output));
            QCOMPARE(output, 123.0f);
        }
    }

    void bindingAndStopNeverLeaveOldActiveValues()
    {
        const float nan = std::numeric_limits<float>::quiet_NaN();
        CDeformerDrag drag;
        QVERIFY(drag.SetParameters({"X", "invalid", "Y", "truncated"}, {-30, nan, -2, 0}, {30, 1, 6}, {0, 0, 2}));
        QCOMPARE(drag.Ids(), (QStringList{"X", "Y"}));
        QVERIFY(!drag.IsActive());
        QVERIFY(drag.Values().isEmpty());
        QVERIFY(drag.Apply(0.5f, -0.5f));
        QCOMPARE(drag.Values(), (QList<float>{15, 0}));
        const auto accepted = drag.Values();
        QVERIFY(!drag.Apply(0, nan));
        QVERIFY(drag.IsActive());
        QCOMPARE(drag.Values(), accepted);
        drag.Stop();
        QVERIFY(!drag.IsActive());
        QVERIFY(drag.Values().isEmpty());
        QCOMPARE(accepted, (QList<float>{15, 0}));
        QCOMPARE(drag.Ids(), (QStringList{"X", "Y"}));
        QVERIFY(drag.Apply(1, -1));
        QVERIFY(drag.SetParameters({"A", "B", "C", "D"}, {0, 0, 0, 4}, {2, 2, 2, 4}, {0, 0, 0, 4}));
        QVERIFY(!drag.IsActive());
        QVERIFY(drag.Values().isEmpty());
        QVERIFY(drag.Apply(-1, 1));
        QCOMPARE(drag.Values(), (QList<float>{0, 2, 0, 4}));
        QVERIFY(drag.SetParameters({}, {}, {}, {}));
        QVERIFY(drag.Ids().isEmpty());
        QVERIFY(drag.Values().isEmpty());
        QVERIFY(!drag.Apply(0, 0));
        QVERIFY(!drag.IsActive());
    }

    void probeUsesAllVerticesAndPublishesAfterRestore()
    {
        CDeformerDrag drag;
        QVERIFY(drag.SetParameters({"old"}, {-1}, {1}, {0}));
        QVERIFY(drag.Apply(0.5f, 0));
        ProbeRuntime runtime(2, 2);
        runtime.affected = {{0}, {1}};
        const auto saved = runtime.saved;
        bool unpublishedDuringRestore = false;
        QVERIFY(drag.ProbeParameters(parameters(2), runtime.original, 2, {0},
            [&](const auto& pose, auto& vertices, QString* error) { return runtime.Evaluate(pose, vertices, error); },
            [&](QString* error) {
                unpublishedDuringRestore = drag.IsActive() && drag.Ids() == QStringList{"old"};
                return runtime.Restore(error);
            }));
        QVERIFY(unpublishedDuringRestore);
        QCOMPARE(drag.Ids(), (QStringList{"0"}));
        QVERIFY(!drag.IsActive());
        QVERIFY(drag.Values().isEmpty());
        QCOMPARE(runtime.restores, 1);
        QCOMPARE(runtime.current, runtime.original);
        QCOMPARE(runtime.saved, saved);
        QVERIFY(runtime.isolatedSamples);
        QCOMPARE(runtime.inputs.size(), 3);
        QCOMPARE(runtime.baseline[0][0], 0.0f);
    }

    void probeRetainsLocalPreferenceAndStableFourParameterLimit_data()
    {
        QTest::addColumn<bool>("local");
        QTest::newRow("local") << true;
        QTest::newRow("global-fallback") << false;
    }

    void probeRetainsLocalPreferenceAndStableFourParameterLimit()
    {
        QFETCH(bool, local);
        ProbeRuntime runtime(local ? 8 : 5, local ? 6 : 3);
        runtime.affected = local ? QVector<QVector<int>>{{0, 1, 2}, {0}, {0, 2}, {0, 3}, {0, 1, 2, 3, 4, 5}, {4}, {0}, {0}}
                                 : QVector<QVector<int>>{{0, 1, 2}, {0, 1}, {0, 2}, {0, 1, 2}, {0, 1, 2}};
        QVector<int> found{99};
        QVERIFY(CDeformerDrag::Probe(parameters(runtime.original.size()), runtime.original, runtime.baseline.size(), {0},
            [&](const auto& pose, auto& vertices, QString* error) { return runtime.Evaluate(pose, vertices, error); },
            [&](QString* error) { return runtime.Restore(error); }, found));
        QCOMPARE(found, local ? (QVector<int>{1, 6, 7, 2}) : (QVector<int>{1, 2, 0, 3}));
        QVERIFY(runtime.isolatedSamples);
        QCOMPARE(runtime.current, runtime.original);
        QCOMPARE(runtime.restores, 1);
    }

    void extremeMidpointAndExactOutOfRangeRestoration()
    {
        const float limit = std::numeric_limits<float>::max();
        auto definitions = parameters(3);
        definitions[0] = {"0", limit * 0.5f, limit, limit};
        definitions[2] = {"2", 7, 7, 7};
        ProbeRuntime runtime(3, 2);
        runtime.original = runtime.current = {limit, 2, 7};
        runtime.affected = {{0}, {0}, {0}};
        runtime.baseline[0][2] = -limit;
        QVector<int> found;
        QVERIFY(CDeformerDrag::Probe(definitions, runtime.original, 2, {0},
            [&](const auto& pose, auto& vertices, QString* error) {
                const bool result = runtime.Evaluate(pose, vertices, error);
                if (pose != runtime.original) vertices[0][2] = limit;
                return result;
            }, [&](QString* error) { return runtime.Restore(error); }, found));
        QCOMPARE(runtime.inputs.size(), 3);
        QCOMPARE(runtime.inputs[1][0], limit * 0.5f);
        QCOMPARE(runtime.inputs[2], (QVector<float>{limit, -1, 7}));
        QCOMPARE(runtime.current, (QVector<float>{limit, 2, 7}));
        QCOMPARE(found, (QVector<int>{0, 1}));
    }

    void probeRejectsBadInputsBeforeInvokingCallbacks_data()
    {
        QTest::addColumn<QString>("invalid");
        for (const char* value : {"range", "nan-range", "nan-current", "duplicate-id", "current-count", "target", "duplicate-target", "empty-target"})
            QTest::newRow(value) << QString::fromLatin1(value);
    }

    void probeRejectsBadInputsBeforeInvokingCallbacks()
    {
        QFETCH(QString, invalid);
        auto definitions = parameters(2);
        QVector<float> current{0, 0};
        QVector<int> targets{0}, found{99};
        if (invalid == "range") definitions[0].minimum = 2;
        if (invalid == "nan-range") definitions[0].maximum = std::numeric_limits<float>::quiet_NaN();
        if (invalid == "nan-current") current[1] = std::numeric_limits<float>::infinity();
        if (invalid == "duplicate-id") definitions[1].id = definitions[0].id;
        if (invalid == "current-count") current.removeLast();
        if (invalid == "target") targets = {2};
        if (invalid == "duplicate-target") targets = {0, 0};
        if (invalid == "empty-target") targets.clear();
        int calls = 0;
        QVERIFY(!CDeformerDrag::Probe(definitions, current, 2, targets,
            [&](const auto&, auto&, QString*) { ++calls; return true; }, [&](QString*) { ++calls; return true; }, found));
        QCOMPARE(calls, 0);
        QCOMPARE(found, (QVector<int>{99}));
    }

    void failuresRestorePoseAndDoNotPublishPartialBindings_data()
    {
        QTest::addColumn<QString>("failure");
        for (const char* value : {"baseline-false", "baseline-exception", "baseline-int", "baseline-nan", "sample-false",
            "sample-exception", "sample-int", "sample-infinity", "sample-odd", "sample-count", "sample-topology",
            "restore-false", "restore-exception", "restore-int"}) QTest::newRow(value) << QString::fromLatin1(value);
    }

    void failuresRestorePoseAndDoNotPublishPartialBindings()
    {
        QFETCH(QString, failure);
        CDeformerDrag drag;
        QVERIFY(drag.SetParameters({"old"}, {-1}, {1}, {0}));
        QVERIFY(drag.Apply(0.5f, 0));
        const auto before = drag.Values();
        ProbeRuntime runtime(2, 2);
        runtime.affected = {{0}, {0}};
        runtime.failure = failure;
        const auto saved = runtime.saved;
        QString error;
        QVERIFY(!drag.ProbeParameters(parameters(2), runtime.original, 2, {0},
            [&](const auto& pose, auto& vertices, QString* message) { return runtime.Evaluate(pose, vertices, message); },
            [&](QString* message) { return runtime.Restore(message); }, &error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(runtime.restores, 1);
        QCOMPARE(runtime.current, runtime.original);
        QCOMPARE(runtime.saved, saved);
        QCOMPARE(drag.Ids(), (QStringList{"old"}));
        QCOMPARE(drag.Values(), before);
        QVERIFY(drag.IsActive());
    }
};

QTEST_GUILESS_MAIN(DeformerDragTests)
#include "deformer_drag_tests.moc"
