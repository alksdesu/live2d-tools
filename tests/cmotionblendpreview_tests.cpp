#include "live2dstruct/cmotionblendpreview.h"

#include <QtTest>
#include <cmath>
#include <limits>

class CMotionBlendPreviewTests : public QObject
{
    Q_OBJECT
private slots:
    void globalSineFadesUseBaseline()
    {
        CMotionTimeline timeline;
        QVERIFY(timeline.AddPoint("A", 0, 10));
        QVERIFY(timeline.SetTiming(30, 4));
        const QMap<QString, float> baseline{{"A", 2}, {"Unchanged", 17}};
        const auto evaluate = [&](double time) { return CMotionBlendPreview::Evaluate(timeline, time, baseline, {}, {}); };
        QCOMPARE(evaluate(0).parameters.value("A"), 2.0f);
        QVERIFY(qAbs(evaluate(0.5).parameters.value("A") - 6.0f) < 0.00001f);
        QCOMPARE(evaluate(1).parameters.value("A"), 10.0f);
        QCOMPARE(evaluate(2).parameters.value("A"), 10.0f);
        QVERIFY(qAbs(evaluate(3.5).parameters.value("A") - 6.0f) < 0.00001f);
        QCOMPARE(evaluate(4).parameters.value("A"), 2.0f);
        QVERIFY(!evaluate(2).parameters.contains("Unchanged"));
        QCOMPARE(evaluate(-8).parameters, evaluate(0).parameters);
        QCOMPARE(evaluate(9).parameters, evaluate(4).parameters);
        QVERIFY(timeline.SetFadeTimes({2.0, 4.0}));
        const double expected = 2.0 + 8.0 * 0.5 * (0.5 + std::sqrt(0.5) / 2.0);
        QVERIFY(qAbs(evaluate(1).parameters.value("A") - expected) < 0.00001);
    }

    void perCurveFadesOverrideEachDirection()
    {
        CMotionTimeline timeline;
        for (const auto& id : {"Inherited", "InstantIn", "InstantOut", "OwnBoth", "Negative"})
            QVERIFY(timeline.AddPoint(QString::fromLatin1(id), 0, 8));
        QVERIFY(timeline.SetTiming(30, 4));
        QVERIFY(timeline.SetFadeTimes({2.0, 4.0}));
        QVERIFY(timeline.SetTrackFadeTimes({CMotionTarget::Parameter, "InstantIn"}, {0.0, std::nullopt}));
        QVERIFY(timeline.SetTrackFadeTimes({CMotionTarget::Parameter, "InstantOut"}, {std::nullopt, 0.0}));
        QVERIFY(timeline.SetTrackFadeTimes({CMotionTarget::Parameter, "OwnBoth"}, {4.0, 2.0}));
        QVERIFY(timeline.SetTrackFadeTimes({CMotionTarget::Parameter, "Negative"}, {-3.0, -9.0}));
        const auto values = CMotionBlendPreview::Evaluate(timeline, 1.0, {}, {}, {}).parameters;
        const double inheritedOut = 0.5 + std::sqrt(0.5) / 2.0;
        QVERIFY(qAbs(values.value("Inherited") - 4.0 * inheritedOut) < 0.00001);
        QVERIFY(qAbs(values.value("InstantIn") - 8.0 * inheritedOut) < 0.00001);
        QCOMPARE(values.value("InstantOut"), 4.0f);
        QVERIFY(qAbs(values.value("OwnBoth") - 8.0 * (0.5 - std::sqrt(0.5) / 2.0)) < 0.00001);
        QCOMPARE(values.value("Negative"), values.value("Inherited"));
        QVERIFY(timeline.SetFadeTimes({-2.0, -4.0}));
        const auto negativeGlobal = CMotionBlendPreview::Evaluate(timeline, 0.5, {}, {}, {}).parameters;
        QCOMPARE(negativeGlobal.value("Inherited"), 4.0f);
        QCOMPARE(negativeGlobal.value("InstantIn"), 8.0f);
        QVERIFY(timeline.SetFadeTimes({0.0, 0.0}));
        const auto instant = CMotionBlendPreview::Evaluate(timeline, 0, {}, {}, {}).parameters;
        QCOMPARE(instant.value("Inherited"), 8.0f);
        QCOMPARE(instant.value("Negative"), 8.0f);
    }

    void loopRepeatsFadeInWithoutFadeOut()
    {
        CMotionTimeline timeline;
        QVERIFY(timeline.AddPoint("A", 0, 8));
        QVERIFY(timeline.AddPoint("A", 2, 16));
        QVERIFY(timeline.SetFadeTimes({1.0, 10.0}));
        const auto evaluate = [&](double time) {
            return CMotionBlendPreview::Evaluate(timeline, time, {{"A", 2}}, {}, {}, true).parameters.value("A");
        };
        QCOMPARE(evaluate(0), 2.0f);
        QCOMPARE(evaluate(0.5), 6.0f);
        QCOMPARE(evaluate(1), 12.0f);
        QCOMPARE(evaluate(1.5), 14.0f);
        QCOMPARE(evaluate(2), 2.0f);
        QCOMPARE(evaluate(2.5), 6.0f);
        QCOMPARE(evaluate(20.5), 6.0f);
        QVERIFY(timeline.SetTrackFadeTimes({CMotionTarget::Parameter, "A"}, {0.0, 50.0}));
        QCOMPARE(evaluate(2), 8.0f);
        QCOMPARE(evaluate(2.5), 10.0f);
    }

    void explicitAndFallbackEffectsUseCorrectOrder()
    {
        CMotionTimeline timeline;
        QVERIFY(timeline.AddPoint({CMotionTarget::Model, "EyeBlink"}, 0, 0.5f));
        QVERIFY(timeline.AddPoint({CMotionTarget::Model, "LipSync"}, 0, 0.2f));
        QVERIFY(timeline.AddPoint("Explicit", 0, 0.8f));
        QVERIFY(timeline.SetTiming(30, 4));
        const QSet<QString> eyeIds{"Explicit", "Eye", "Both"};
        const QSet<QString> lipIds{"Explicit", "Mouth", "Both"};
        const QMap<QString, float> baseline{{"Explicit", 0.1f}, {"Eye", 1}, {"Mouth", 0.8f}, {"Both", 1}};
        const auto frame = CMotionBlendPreview::Evaluate(timeline, 0.5, baseline, eyeIds, lipIds);
        QVERIFY(qAbs(frame.parameters.value("Explicit") - 0.35f) < 0.00001f);
        QCOMPARE(frame.parameters.value("Eye"), 0.75f);
        QCOMPARE(frame.parameters.value("Mouth"), 0.5f);
        QVERIFY(qAbs(frame.parameters.value("Both") - 0.475f) < 0.00001f);
        QVERIFY(!frame.model.contains("EyeBlink"));
        QVERIFY(!frame.model.contains("LipSync"));
        QCOMPARE(frame.ResolveParameters(eyeIds, lipIds), frame.parameters);
        QVERIFY(timeline.SetTrackFadeTimes({CMotionTarget::Parameter, "Explicit"}, {0.0, 0.0}));
        const auto overrides = CMotionBlendPreview::Evaluate(timeline, 0.5, baseline, eyeIds, lipIds);
        QVERIFY(qAbs(overrides.parameters.value("Explicit") - 0.6f) < 0.00001f);
        QCOMPARE(overrides.parameters.value("Eye"), frame.parameters.value("Eye"));
    }

    void modelAndPartCurvesStayIndependentAndRaw()
    {
        CMotionTimeline timeline;
        QVERIFY(timeline.AddPoint({CMotionTarget::Model, "Opacity"}, 0, 0.25f));
        QVERIFY(timeline.AddPoint({CMotionTarget::Model, "OtherEffect"}, 0, 0.75f));
        QVERIFY(timeline.AddPoint({CMotionTarget::PartOpacity, "Opacity"}, 0, 0.4f));
        QVERIFY(timeline.AddPoint({CMotionTarget::Parameter, "Opacity"}, 0, 0.9f));
        QVERIFY(timeline.SetTiming(30, 3));
        QVERIFY(timeline.SetTrackFadeTimes({CMotionTarget::Model, "Opacity"}, {50.0, 50.0}));
        QVERIFY(timeline.SetTrackFadeTimes({CMotionTarget::PartOpacity, "Opacity"}, {50.0, 50.0}));
        const auto frame = CMotionBlendPreview::Evaluate(timeline, 0, {{"Opacity", 0.1f}}, {}, {});
        QCOMPARE(frame.parameters.value("Opacity"), 0.1f);
        QCOMPARE(frame.model.value("Opacity"), 0.25f);
        QCOMPARE(frame.model.value("OtherEffect"), 0.75f);
        QCOMPARE(frame.parts.value("Opacity"), 0.4f);
    }

    void evaluationDoesNotAccumulateOrMutateInputs()
    {
        CMotionTimeline timeline;
        QVERIFY(timeline.AddPoint("A", 0, 10));
        QVERIFY(timeline.SetTiming(30, 4));
        const auto originalTimeline = timeline;
        const QMap<QString, float> baseline{{"A", 2}};
        const auto first = CMotionBlendPreview::Evaluate(timeline, 0.5, baseline, {}, {});
        for (int i = 0; i < 100; ++i)
            QCOMPARE(CMotionBlendPreview::Evaluate(timeline, 0.5, baseline, {}, {}).parameters, first.parameters);
        CMotionBlendPreview::Evaluate(timeline, 2, baseline, {}, {});
        QCOMPARE(CMotionBlendPreview::Evaluate(timeline, 0.5, baseline, {}, {}).parameters, first.parameters);
        QCOMPARE(baseline.value("A"), 2.0f);
        QVERIFY(timeline.HasSameContent(originalTimeline));
    }

    void finiteBoundsAndInvalidInputs()
    {
        CMotionTimeline timeline;
        const float maximum = std::numeric_limits<float>::max();
        QVERIFY(timeline.AddPoint({CMotionTarget::Model, "EyeBlink"}, 0, maximum));
        QVERIFY(timeline.AddPoint({CMotionTarget::Parameter, "A"}, 0, maximum));
        QVERIFY(timeline.AddPoint({CMotionTarget::Parameter, "B"}, 0, -maximum));
        QVERIFY(timeline.SetTiming(30, 4));
        const auto frame = CMotionBlendPreview::Evaluate(timeline, 1, {{"A", -maximum}, {"B", maximum}}, {"A", "B"}, {});
        QVERIFY(frame.IsFinite());
        QCOMPARE(frame.parameters.value("A"), maximum);
        QCOMPARE(frame.parameters.value("B"), -maximum);
        CMotionTimeline finiteTarget;
        QVERIFY(finiteTarget.AddPoint("A", 0, 1));
        QVERIFY(finiteTarget.SetFadeTimes({0.0, 0.0}));
        QCOMPARE(CMotionBlendPreview::Evaluate(finiteTarget, 0, {{"A", maximum}}, {}, {}).parameters.value("A"), 1.0f);
        QVERIFY(CMotionBlendPreview::Evaluate(timeline, std::numeric_limits<double>::infinity(), {}, {}, {}).IsEmpty());
        QVERIFY(CMotionBlendPreview::Evaluate(timeline, std::numeric_limits<double>::quiet_NaN(), {}, {}, {}).IsEmpty());
        QVERIFY(CMotionBlendPreview::Evaluate(timeline, 1, {{"A", std::numeric_limits<float>::quiet_NaN()}}, {}, {}).IsEmpty());
        QVERIFY(CMotionBlendPreview::Evaluate(timeline, 1, {}, {""}, {}).IsEmpty());
        QVERIFY(CMotionBlendPreview::Evaluate(timeline, 1, {{QString(QChar::Null), 1}}, {}, {}).IsEmpty());
        QVERIFY(CMotionBlendPreview::Evaluate(timeline, std::numeric_limits<double>::max(), {}, {}, {}, true).IsFinite());
        const auto atStart = CMotionBlendPreview::Evaluate(timeline, 0, {{"A", 3}, {"B", 4}}, {"A", "B"}, {});
        QCOMPARE(atStart.parameters.value("A"), 3.0f);
        QCOMPARE(atStart.parameters.value("B"), 4.0f);
    }

    void zeroDurationHasNoFadeOut()
    {
        CMotionTimeline timeline;
        QVERIFY(timeline.AddPoint("A", 0, 6));
        QVERIFY(timeline.SetFadeTimes({0.0, 20.0}));
        QCOMPARE(CMotionBlendPreview::Evaluate(timeline, 0, {{"A", 1}}, {}, {}).parameters.value("A"), 6.0f);
        QCOMPARE(CMotionBlendPreview::Evaluate(timeline, 50, {{"A", 1}}, {}, {}).parameters.value("A"), 6.0f);
        QCOMPARE(CMotionBlendPreview::Evaluate(timeline, 50, {{"A", 1}}, {}, {}, true).parameters.value("A"), 6.0f);
        QVERIFY(CMotionBlendPreview::Evaluate(CMotionTimeline(), 1, {{"A", 1}}, {}, {}).IsEmpty());
    }
};

QTEST_GUILESS_MAIN(CMotionBlendPreviewTests)
#include "cmotionblendpreview_tests.moc"
