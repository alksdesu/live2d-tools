#include "live2dstruct/conionskin.h"
#include <QtTest>
#include <cmath>
#include <limits>

using live2dstruct::COnionSkin;

class OnionSkinTests : public QObject
{
    Q_OBJECT
    static CMotionTimeline timeline()
    {
        CMotionTimeline value;
        value.AddPoint(QStringLiteral("A"), 0, 0);
        value.AddPoint(QStringLiteral("A"), 2, 20);
        value.SetTiming(10, 2);
        return value;
    }
    static COnionSkin::Request request()
    {
        COnionSkin::Request result;
        result.settings.enabled = true;
        result.time = 1;
        COnionSkin::CaptureTimeline(timeline(), result.timeline);
        return result;
    }
    static COnionSkin::Base base()
    {
        COnionSkin::Base result;
        result.parameters = {{QStringLiteral("A"), 3}, {QStringLiteral("Untouched"), 7}};
        result.parts = {{QStringLiteral("Part"), .7f}};
        result.modelOpacity = .8f;
        return result;
    }
private slots:
    void settingsLimits_data()
    {
        QTest::addColumn<int>("before"); QTest::addColumn<int>("after");
        QTest::addColumn<int>("step"); QTest::addColumn<float>("opacity"); QTest::addColumn<bool>("valid");
        QTest::newRow("empty") << 0 << 0 << 1 << 0.f << true;
        QTest::newRow("maximum") << 4 << 4 << 500 << 1.f << true;
        QTest::newRow("negative") << -1 << 1 << 1 << .5f << false;
        QTest::newRow("too-many") << 1 << 5 << 1 << .5f << false;
        QTest::newRow("no-step") << 1 << 1 << 0 << .5f << false;
        QTest::newRow("opacity-high") << 1 << 1 << 1 << 1.01f << false;
        QTest::newRow("opacity-nan") << 1 << 1 << 1 << std::numeric_limits<float>::quiet_NaN() << false;
    }
    void settingsLimits()
    {
        QFETCH(int, before); QFETCH(int, after); QFETCH(int, step); QFETCH(float, opacity); QFETCH(bool, valid);
        COnionSkin::Settings settings;
        settings.before = before; settings.after = after; settings.stepFrames = step; settings.beforeOpacity = opacity;
        QCOMPARE(COnionSkin::Validate(settings), valid);
        settings.beforeOpacity = .25f; settings.afterOpacity = opacity;
        QCOMPARE(COnionSkin::Validate(settings), valid);
    }
    void samplesKeepSubframesAndPaintFarFirst()
    {
        auto input = request(); input.time = 1.025; input.settings.before = 2; input.settings.after = 2;
        COnionSkin::Plan plan;
        QVERIFY(COnionSkin::Build(input, base(), plan));
        QCOMPARE(plan.samples.size(), 4);
        QCOMPARE(plan.samples[0].offset, -2); QCOMPARE(plan.samples[1].offset, 2);
        QCOMPARE(plan.samples[2].offset, -1); QCOMPARE(plan.samples[3].offset, 1);
        QVERIFY(std::abs(plan.samples[0].time - .825) < 1e-12);
        for (const auto& sample : plan.samples) {
            QVERIFY(std::abs(sample.frame.parameters.value("A") - sample.time * 10) < 1e-5);
            QCOMPARE(sample.frame.parameters.value("Untouched"), 7.f);
            QCOMPARE(sample.frame.parts.value("Part"), .7f);
            QCOMPARE(sample.frame.model.value("Opacity"), .8f);
        }
    }
    void endpointsOmitInsteadOfRepeating()
    {
        auto input = request(); input.time = 0; input.settings.before = 4; input.settings.after = 4;
        COnionSkin::Plan plan;
        QVERIFY(COnionSkin::Build(input, base(), plan));
        QCOMPARE(plan.samples.size(), 4); QCOMPARE(plan.omitted, 4);
        for (const auto& sample : plan.samples) QVERIFY(sample.time > 0);
        input.time = 2;
        QVERIFY(COnionSkin::Build(input, base(), plan));
        QCOMPARE(plan.samples.size(), 4); QCOMPARE(plan.omitted, 4);
        for (const auto& sample : plan.samples) QVERIFY(sample.time < 2);
    }
    void loopsDeduplicateAndPreferNearest()
    {
        auto input = request(); input.loop = true; input.time = 0;
        input.settings.before = 4; input.settings.after = 4; input.settings.stepFrames = 10;
        COnionSkin::Plan plan;
        QVERIFY(COnionSkin::Build(input, base(), plan));
        QCOMPARE(plan.samples.size(), 1); QCOMPARE(plan.omitted, 7);
        QCOMPARE(plan.samples.first().time, 1.); QCOMPARE(plan.samples.first().offset, -1);
    }
    void zeroDurationAndNoTracksAreEmpty()
    {
        CMotionTimeline single; QVERIFY(single.AddPoint(QStringLiteral("A"), 0, 4));
        auto input = request(); input.time = 0;
        QVERIFY(COnionSkin::CaptureTimeline(single, input.timeline));
        COnionSkin::Plan plan;
        QVERIFY(COnionSkin::Build(input, base(), plan)); QVERIFY(plan.samples.isEmpty());
        CMotionTimeline empty;
        QVERIFY(COnionSkin::CaptureTimeline(empty, input.timeline));
        QVERIFY(COnionSkin::Build(input, base(), plan)); QVERIFY(plan.samples.isEmpty());
    }
    void transportSuspensionDoesNotRequireModel()
    {
        auto input = request(); input.timeline = {};
        COnionSkin::Plan plan;
        input.settings.enabled = false; QVERIFY(COnionSkin::Build(input, {}, plan));
        input.settings.enabled = true; input.recording = true; QVERIFY(COnionSkin::Build(input, {}, plan));
        input.recording = false; input.suspended = true; QVERIFY(COnionSkin::Build(input, {}, plan));
        input.suspended = false; input.playing = true; QVERIFY(COnionSkin::Build(input, {}, plan));
        input.settings.duringPlayback = true; QVERIFY(!COnionSkin::Build(input, {}, plan));
        QVERIFY(plan.samples.isEmpty());
    }
    void effectsAppliedOnceAndUnknownTracksIgnored()
    {
        auto source = timeline();
        QVERIFY(source.AddPoint({CMotionTarget::Model, "EyeBlink"}, 0, .5f));
        QVERIFY(source.AddPoint({CMotionTarget::Model, "LipSync"}, 0, .2f));
        QVERIFY(source.AddPoint({CMotionTarget::Model, "Opacity"}, 0, 2));
        QVERIFY(source.AddPoint({CMotionTarget::PartOpacity, "Part"}, 0, -2));
        QVERIFY(source.AddPoint({CMotionTarget::PartOpacity, "UnknownPart"}, 0, 1));
        QVERIFY(source.AddPoint(QStringLiteral("Unknown"), 0, 45));
        QVERIFY(source.SetFadeTimes({0., 0.}));
        auto input = request(); QVERIFY(COnionSkin::CaptureTimeline(source, input.timeline));
        auto baseline = base(); baseline.eyeBlinkIds = {"A", "UnknownEffect"}; baseline.lipSyncIds = {"A"};
        COnionSkin::Plan plan, faded;
        QVERIFY(COnionSkin::Build(input, baseline, plan));
        input.useFades = true; QVERIFY(COnionSkin::Build(input, baseline, faded));
        for (int i = 0; i < plan.samples.size(); ++i) {
            const auto& sample = plan.samples[i];
            QVERIFY(std::abs(sample.frame.parameters.value("A") - (sample.time * 5 + .2)) < 1e-5);
            QCOMPARE(sample.frame.parameters.size(), 2); QCOMPARE(sample.frame.parts.size(), 1);
            QCOMPARE(sample.frame.parts.value("Part"), 0.f); QCOMPARE(sample.frame.model.value("Opacity"), 1.f);
            QCOMPARE(sample.frame.parameters, faded.samples[i].frame.parameters);
            QVERIFY(!sample.frame.model.contains("EyeBlink"));
        }
    }
    void fadesAndHiddenPartsUseStableBase()
    {
        auto source = timeline(); QVERIFY(source.SetFadeTimes({1., 1.}));
        auto input = request(); input.time = .6; input.useFades = true;
        QVERIFY(COnionSkin::CaptureTimeline(source, input.timeline));
        auto baseline = base(); baseline.hiddenParts.insert("Part");
        COnionSkin::Plan plan;
        QVERIFY(COnionSkin::Build(input, baseline, plan));
        QCOMPARE(plan.samples[0].time, .5);
        QVERIFY(std::abs(plan.samples[0].frame.parameters.value("A") - 4.f) < 1e-5);
        QCOMPARE(plan.samples[0].frame.parts.value("Part"), 0.f);
        COnionSkin::Plan repeated; QVERIFY(COnionSkin::Build(input, baseline, repeated));
        QCOMPARE(plan.fingerprint, repeated.fingerprint);
    }
    void fadesDistinguishTimesSharingOneFloatSample()
    {
        CMotionTimeline source; QVERIFY(source.AddPoint(QStringLiteral("A"), 0, 10));
        QVERIFY(source.SetTiming(100, 100000000)); QVERIFY(source.SetFadeTimes({0., 1.}));
        auto input = request(); input.time = 99999999.5; input.useFades = true;
        QVERIFY(COnionSkin::CaptureTimeline(source, input.timeline));
        auto baseline = base(); baseline.parameters["A"] = 0;
        COnionSkin::Plan plan; QVERIFY(COnionSkin::Build(input, baseline, plan));
        QCOMPARE(plan.samples.size(), 2); QCOMPARE(plan.omitted, 0);
        QCOMPARE(float(plan.samples[0].time), float(plan.samples[1].time));
        QVERIFY(plan.samples[0].frame.parameters.value("A") > 5);
        QVERIFY(plan.samples[1].frame.parameters.value("A") < 5);
        for (const auto& sample : plan.samples) {
            const double remaining = source.Timing().duration - sample.time;
            const double expected = 5 * (1 - std::cos(remaining * std::acos(-1.)));
            QVERIFY(std::abs(sample.frame.parameters.value("A") - expected) < 1e-5);
        }
        input.useFades = false; QVERIFY(COnionSkin::Build(input, baseline, plan));
        QVERIFY(plan.samples.isEmpty()); QCOMPARE(plan.omitted, 2);
    }
    void frozenTimelineAndBaseOwnBorrowedStorage()
    {
        QChar borrowed[] = {QChar('A')};
        const auto id = QString::fromRawData(borrowed, 1);
        CMotionTimeline source; QVERIFY(source.AddPoint(id, 0, 1)); QVERIFY(source.AddPoint(id, 2, 3));
        QVERIFY(source.SetTiming(10, 2));
        auto input = request(); QVERIFY(COnionSkin::CaptureTimeline(source, input.timeline));
        auto baseline = base(); baseline.parameters = {{id, 10}};
        COnionSkin::Plan plan; QVERIFY(COnionSkin::Build(input, baseline, plan));
        const auto hash = input.timeline.Fingerprint();
        borrowed[0] = QChar('B');
        QCOMPARE(input.timeline.Fingerprint(), hash);
        QVERIFY(plan.samples.first().frame.parameters.contains("A"));
        QVERIFY(!plan.samples.first().frame.parameters.contains("B"));
        QVERIFY(COnionSkin::Build(input, base(), plan));
        QVERIFY(std::abs(plan.samples.first().frame.parameters.value("A") - 1.9f) < 1e-5);
    }
    void failedBuildPreservesAcceptedPlan()
    {
        auto input = request(); COnionSkin::Plan plan;
        QVERIFY(COnionSkin::Build(input, base(), plan)); const auto hash = plan.fingerprint;
        input.time = std::numeric_limits<double>::quiet_NaN();
        QVERIFY(!COnionSkin::Build(input, base(), plan)); QCOMPARE(plan.fingerprint, hash);
        input.time = 1; auto baseline = base(); baseline.parameters["A"] = std::numeric_limits<float>::infinity();
        QVERIFY(!COnionSkin::Build(input, baseline, plan)); QCOMPARE(plan.fingerprint, hash);
        input.time = 3; QVERIFY(!COnionSkin::Build(input, base(), plan)); QCOMPARE(plan.fingerprint, hash);
    }
    void snapshotRetainsCurveEvaluationWithoutRecordingCallback()
    {
        const auto lifetime = std::make_shared<int>(0);
        std::weak_ptr<int> held;
        auto input = request();
        {
            auto callbackOwner = std::make_shared<int>(0); held = callbackOwner;
            CMotionTimeline source([callbackOwner, lifetime] { ++*lifetime; return qint64(0); });
            QVERIFY(source.AddPoint(QStringLiteral("A"), 0, 0));
            QVERIFY(source.AddPoint(QStringLiteral("A"), 2, 20));
            QVERIFY(source.SetTiming(10, 2));
            CMotionTimeline::BatchEdit edit;
            edit.segments.append({{{CMotionTarget::Parameter, "A"}, 1}, 1, {{2. / 3, 0}, {4. / 3, 20}}});
            CMotionTimeline::BatchResult changed; QVERIFY(source.EditPoints(edit, changed));
            QVERIFY(COnionSkin::CaptureTimeline(source, input.timeline));
        }
        QVERIFY(held.expired()); QCOMPARE(*lifetime, 0);
        COnionSkin::Plan plan; QVERIFY(COnionSkin::Build(input, base(), plan));
        for (const auto& sample : plan.samples) {
            const double u = static_cast<float>(sample.time) / 2.;
            QVERIFY(std::abs(sample.frame.parameters.value("A") - 20 * (3 * u * u - 2 * u * u * u)) < 1e-5);
        }
        auto stepped = timeline(); QVERIFY(stepped.SetSegmentType(QStringLiteral("A"), 1, 2));
        QVERIFY(COnionSkin::CaptureTimeline(stepped, input.timeline));
        QVERIFY(COnionSkin::Build(input, base(), plan));
        for (const auto& sample : plan.samples) QCOMPARE(sample.frame.parameters.value("A"), 0.f);
        QVERIFY(stepped.SetSegmentType(QStringLiteral("A"), 1, 3));
        QVERIFY(COnionSkin::CaptureTimeline(stepped, input.timeline));
        QVERIFY(COnionSkin::Build(input, base(), plan));
        for (const auto& sample : plan.samples) QCOMPARE(sample.frame.parameters.value("A"), 20.f);
    }
    void renderInputsInvalidateFingerprint()
    {
        auto input = request(); auto baseline = base(); COnionSkin::Plan plan;
        QVERIFY(COnionSkin::Build(input, baseline, plan)); auto hash = plan.fingerprint;
        ++input.sourceRevision; QVERIFY(COnionSkin::Build(input, baseline, plan)); QVERIFY(hash != plan.fingerprint);
        hash = plan.fingerprint; input.settings.beforeOpacity = .5f;
        QVERIFY(COnionSkin::Build(input, baseline, plan)); QVERIFY(hash != plan.fingerprint);
        hash = plan.fingerprint; baseline.parameters["Untouched"] = 9;
        QVERIFY(COnionSkin::Build(input, baseline, plan)); QVERIFY(hash != plan.fingerprint);
    }
    void resolutionUsesPhysicalPixelsAndBudget()
    {
        QSize size;
        QVERIFY(COnionSkin::SelectResolution({2880, 1800}, 16384, 16 * 1024 * 1024, size));
        QCOMPARE(size, QSize(2880, 1800));
        QVERIFY(COnionSkin::SelectResolution({5760, 3600}, 16384, 16 * 1024 * 1024, size));
        QVERIFY(size.width() < 5760); QVERIFY(qint64(size.width()) * size.height() * 12 + 16 * 1024 * 1024 <= COnionSkin::MaximumGpuBytes);
        QVERIFY(std::abs(double(size.width()) / size.height() - 1.6) < .002);
        QVERIFY(COnionSkin::SelectResolution({4000, 2000}, 1024, 0, size)); QCOMPARE(size, QSize(1024, 512));
        const auto previous = size;
        QVERIFY(!COnionSkin::SelectResolution({4000, 2000}, 1024, COnionSkin::MaximumGpuBytes, size)); QCOMPARE(size, previous);
        QVERIFY(!COnionSkin::SelectResolution({0, 20}, 1024, 0, size)); QCOMPARE(size, previous);
        QVERIFY(!COnionSkin::SelectResolution({10, 20}, 0, 0, size)); QCOMPARE(size, previous);
        QVERIFY(COnionSkin::SelectResolution({INT_MAX, INT_MAX}, 16384, 0, size));
        QVERIFY(qint64(size.width()) * size.height() * 12 <= COnionSkin::MaximumGpuBytes);
        QVERIFY(COnionSkin::SelectResolution({1, 1}, 1, COnionSkin::MaximumGpuBytes - 12, size));
        QCOMPARE(size, QSize(1, 1));
        QVERIFY(!COnionSkin::SelectResolution({1, 1}, 1, COnionSkin::MaximumGpuBytes - 11, size));
    }
};
QTEST_GUILESS_MAIN(OnionSkinTests)
#include "onion_skin_tests.moc"
