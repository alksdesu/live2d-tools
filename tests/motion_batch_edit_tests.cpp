#include "live2dstruct/cmotiontimeline.h"
#include <QtTest>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QFile>
#include <cmath>
#include <limits>

namespace {
const CMotionTrack parameter{CMotionTarget::Parameter, "Shared"};
const CMotionTrack model{CMotionTarget::Model, "Shared"};
const CMotionTrack part{CMotionTarget::PartOpacity, "Shared"};
QJsonObject curve(const CMotionTrack& track, const QJsonArray& segments)
{
    return {{"Target", track.TargetName()}, {"Id", track.id}, {"Segments", segments},
        {"FadeInTime", .25}, {"FadeOutTime", -1}, {"CustomCurve", "retained"}};
}
bool load(CMotionTimeline& timeline, const QJsonArray& curves, bool restricted = false)
{
    const QJsonObject motion{{"Version", 3}, {"CustomRoot", "retained"},
        {"Meta", QJsonObject{{"Fps", 29.9700000000003}, {"Duration", 5}, {"Loop", true},
            {"AreBeziersRestricted", restricted}, {"FadeInTime", .5}, {"FadeOutTime", .75}, {"CustomMeta", 17}}},
        {"UserData", QJsonArray{QJsonObject{{"Time", 3.5}, {"Value", QStringLiteral("事件🌱")}, {"CustomEvent", true}}}},
        {"Curves", curves}};
    return timeline.RestoreState({{"version", 1}, {"motion", motion}, {"playhead", 0}});
}
QJsonObject state(const CMotionTimeline& timeline)
{
    QJsonObject result; timeline.SaveState(&result, timeline.Loop(), 0); return result;
}
CMotionTimeline::BatchResult sentinel()
{
    CMotionTimeline::BatchResult result; result.oldToNew = {{{CMotionTarget::Model, "sentinel"}, {9}}};
    result.inserted = {{{CMotionTarget::PartOpacity, "sentinel"}, 8}}; result.changed = true; return result;
}
bool sameResult(const CMotionTimeline::BatchResult& left, const CMotionTimeline::BatchResult& right)
{
    return left.oldToNew == right.oldToNew && left.inserted == right.inserted && left.changed == right.changed;
}
}

class MotionBatchEditTests : public QObject
{
    Q_OBJECT
private slots:
    void referencesKeepTypedIdentityAndHash()
    {
        const CMotionPointRef p{parameter, 1}, m{model, 1}, p2{parameter, 2};
        QSet<CMotionPointRef> selected{p, m, p2, p}; QCOMPARE(selected.size(), 3);
        QVERIFY(p != m); QVERIFY(p < p2); QVERIFY(m < p);
        const QVariant value = QVariant::fromValue(CMotionPointSelection{p, m});
        QCOMPARE(value.value<CMotionPointSelection>(), CMotionPointSelection({p, m}));
    }

    void simultaneousMoveAllowsVacatedTimesAndMapsEveryTrack()
    {
        CMotionTimeline timeline;
        QVERIFY(load(timeline, {curve(parameter, {0, 0, 0, 1, 1, 0, 2, 2}),
            curve(model, {0, 1, 2, 2, 0}), curve(part, {0, 1, 3, 2, .5})}));
        const auto before = timeline; CMotionTimeline::BatchEdit edit;
        edit.updates = {{{parameter, 0}, {1, 0}}, {{parameter, 1}, {2, 1}}, {{parameter, 2}, {3, 2}}};
        CMotionTimeline::BatchResult result; QString error;
        QVERIFY2(timeline.EditPoints(edit, result, &error), qPrintable(error)); QVERIFY(result.changed);
        QCOMPARE(timeline.Tracks()[parameter], QVector<QPointF>({{1, 0}, {2, 1}, {3, 2}}));
        QCOMPARE(result.oldToNew[parameter], QVector<int>({0, 1, 2}));
        QCOMPARE(result.oldToNew[model], QVector<int>({0, 1})); QCOMPARE(result.oldToNew[part], QVector<int>({0, 1}));
        QCOMPARE(timeline.Tracks()[model], before.Tracks()[model]); QCOMPARE(timeline.Tracks()[part], before.Tracks()[part]);
        QCOMPARE(timeline.Events(), before.Events()); QCOMPARE(timeline.Timing().fps, before.Timing().fps);
        QCOMPARE(timeline.Timing().duration, before.Timing().duration); QCOMPARE(timeline.FadeTimes().fadeIn, before.FadeTimes().fadeIn);
        const auto oldRoot = state(before)["motion"].toObject(), root = state(timeline)["motion"].toObject();
        QCOMPARE(root["CustomRoot"], oldRoot["CustomRoot"]); QCOMPARE(root["Meta"], oldRoot["Meta"]);
        for (const auto& entry : root["Curves"].toArray()) {
            QCOMPARE(entry.toObject()["CustomCurve"].toString(), QString("retained"));
            QCOMPARE(entry.toObject()["FadeInTime"].toDouble(), .25); QCOMPARE(entry.toObject()["FadeOutTime"].toDouble(), -1.0);
        }
    }

    void reorderAndMixedInsertionDeletionReturnStableReferences()
    {
        CMotionTimeline timeline; QVERIFY(load(timeline, {curve(parameter, {0, 0, 2, 1, 1, 3, 2, 2, 0, 3, 3})}));
        CMotionTimeline::BatchEdit edit;
        edit.updates = {{{parameter, 3}, {.5, 30}}}; edit.removes = {{parameter, 1}};
        edit.insertions = {{parameter, {1.5, 15}, 2, {}, {}}, {part, {1, .75}, 0, {}, {}}};
        CMotionTimeline::BatchResult result; QVERIFY(timeline.EditPoints(edit, result));
        QCOMPARE(timeline.Tracks()[parameter], QVector<QPointF>({{0, 0}, {.5, 30}, {1.5, 15}, {2, 2}}));
        QCOMPARE(result.oldToNew[parameter], QVector<int>({0, -1, 3, 1}));
        QCOMPARE(result.inserted, CMotionPointSelection({{parameter, 2}, {part, 0}}));
        QCOMPARE(timeline.SegmentType(parameter, 2), 2); QCOMPARE(timeline.SegmentType(parameter, 3), 3);
        QCOMPARE(timeline.SegmentType(part, 0), 0); QVERIFY(timeline.BezierControls(part, 0).isEmpty());
    }

    void translateAndScaleBezierHandlesFromOriginalEndpoints_data()
    {
        QTest::addColumn<bool>("restricted"); QTest::newRow("unrestricted") << false; QTest::newRow("restricted") << true;
    }
    void translateAndScaleBezierHandlesFromOriginalEndpoints()
    {
        QFETCH(bool, restricted); CMotionTimeline timeline;
        QVERIFY(load(timeline, {curve(parameter, {0, 0, 1, .2, .4, .7, .8, 1, 1, 1, 1.3, 1.4, 1.8, 1.6, 2, 2})}, restricted));
        const auto original = timeline; CMotionTimeline::BatchEdit edit;
        for (int index = 0; index < 3; ++index) {
            const auto point = timeline.Tracks()[parameter][index]; edit.updates.append({{parameter, index}, {point.x() * 2 + 1, point.y() + 3}});
        }
        CMotionTimeline::BatchResult result; QVERIFY(timeline.EditPoints(edit, result));
        for (int index = 1; index < 3; ++index) {
            const auto old = original.BezierControls(parameter, index), actual = timeline.BezierControls(parameter, index);
            QCOMPARE(actual.size(), 2);
            for (int handle = 0; handle < 2; ++handle) {
                QVERIFY(std::abs(actual[handle].x() - (old[handle].x() * 2 + 1)) < 1e-12);
                QVERIFY(std::abs(actual[handle].y() - (old[handle].y() + 3)) < 1e-12);
            }
        }
        for (int sample = 0; sample <= 20; ++sample) {
            const float time = sample / 10.f;
            QVERIFY(std::abs(timeline.Evaluate(parameter, time * 2 + 1) - (original.Evaluate(parameter, time) + 3)) < 1e-5f);
        }
    }

    void unselectedNeighborKeepsPositionAndRetargetsOnlyItsAdjacentSegment()
    {
        CMotionTimeline timeline; QVERIFY(load(timeline, {curve(parameter,
            {0, 0, 1, .2, .4, .7, .8, 1, 1, 1, 1.3, 1.4, 1.8, 1.6, 2, 2, 1, 2.4, 2.5, 2.7, 2.8, 3, 3})}));
        const auto original = timeline; CMotionTimeline::BatchEdit edit; edit.updates = {{{parameter, 1}, {1.5, 2}}};
        CMotionTimeline::BatchResult result; QVERIFY(timeline.EditPoints(edit, result));
        QCOMPARE(timeline.Tracks()[parameter][2], original.Tracks()[parameter][2]);
        QCOMPARE(timeline.Tracks()[parameter][3], original.Tracks()[parameter][3]);
        QCOMPARE(timeline.BezierControls(parameter, 3), original.BezierControls(parameter, 3));
        const auto controls = timeline.BezierControls(parameter, 2); QCOMPARE(controls.size(), 2);
        QVERIFY(std::abs(controls[0].x() - 1.65) < 1e-12); QVERIFY(std::abs(controls[0].y() - 2.4) < 1e-12);
        QVERIFY(std::abs(controls[1].x() - 1.9) < 1e-12); QCOMPARE(controls[1].y(), 1.6);
    }

    void pastedSegmentRetargetsToActualMergedPredecessor()
    {
        CMotionTimeline timeline; QVERIFY(load(timeline, {curve(parameter, {0, 0, 0, 1, 4, 0, 3, 3})}));
        CMotionTimeline::BatchEdit edit;
        edit.insertions = {{parameter, {2, 2}, 1, {{.5, .5}, {1.5, 1.5}}, QPointF(0, 0)}};
        CMotionTimeline::BatchResult result; QVERIFY(timeline.EditPoints(edit, result));
        QCOMPARE(result.inserted, CMotionPointSelection({{parameter, 2}}));
        QCOMPARE(timeline.BezierControls(parameter, 2), QVector<QPointF>({{1.25, 4.5}, {1.75, 1.5}}));
        QCOMPARE(timeline.Tracks()[parameter][1], QPointF(1, 4)); QCOMPARE(timeline.Tracks()[parameter][3], QPointF(3, 3));
    }

    void deletionBridgesBezierAndKeepsDurationEvenWithoutMeta()
    {
        CMotionTimeline timeline; QVERIFY(timeline.AddPoint("A", 0, 0)); QVERIFY(timeline.AddPoint("A", 1, 1)); QVERIFY(timeline.AddPoint("A", 2, 2));
        const CMotionTrack a{CMotionTarget::Parameter, "A"}; QVERIFY(timeline.SetSegmentType(a, 2, 1));
        CMotionTimeline::BatchEdit edit; edit.removes = {{a, 1}}; CMotionTimeline::BatchResult result;
        QVERIFY(timeline.EditPoints(edit, result)); QCOMPARE(result.oldToNew[a], QVector<int>({0, -1, 1}));
        const auto controls = timeline.BezierControls(a, 1);
        QVERIFY(std::abs(controls[0].x() - 2.0 / 3) < 1e-12); QVERIFY(std::abs(controls[0].y() - 1.0 / 3) < 1e-12);
        edit.removes = {{a, 1}}; QVERIFY(timeline.EditPoints(edit, result)); QCOMPARE(timeline.Timing().duration, 2.0);
        edit.removes = {{a, 0}}; QVERIFY(timeline.EditPoints(edit, result)); QVERIFY(!timeline.HasCurves()); QCOMPARE(timeline.Timing().duration, 2.0);
        QVERIFY(!state(timeline).isEmpty());
    }

    void rejectedBatchPreservesTimelineAndOutput_data()
    {
        QTest::addColumn<int>("kind"); const char* names[]{"unknown", "negative-index", "duplicate-update", "duplicate-remove", "update-remove",
            "selected-collision", "unselected-collision", "float-collision", "negative-time", "nonfinite-value", "invalid-target", "invalid-type",
            "controls-without-start", "one-control", "controls-for-linear", "start-after-end", "outside-control", "nonfinite-control"};
        for (int kind = 0; kind < 18; ++kind) QTest::newRow(names[kind]) << kind;
    }
    void rejectedBatchPreservesTimelineAndOutput()
    {
        QFETCH(int, kind); CMotionTimeline timeline; QVERIFY(load(timeline, {curve(parameter, {0, 0, 0, 1, 1, 0, 2, 2})}));
        const auto original = timeline; const auto saved = state(timeline); CMotionTimeline::BatchEdit edit;
        if (kind == 0) edit.updates = {{{part, 0}, {3, 0}}};
        if (kind == 1) edit.removes = {{parameter, -1}};
        if (kind == 2) edit.updates = {{{parameter, 0}, {.2, 0}}, {{parameter, 0}, {.3, 0}}};
        if (kind == 3) edit.removes = {{parameter, 0}, {parameter, 0}};
        if (kind == 4) { edit.updates = {{{parameter, 0}, {.2, 0}}}; edit.removes = {{parameter, 0}}; }
        if (kind == 5) edit.updates = {{{parameter, 0}, {.5, 0}}, {{parameter, 1}, {.5, 1}}};
        if (kind == 6) edit.updates = {{{parameter, 0}, {1, 0}}};
        if (kind == 7) edit.insertions = {{parameter, {1.000000001, 3}}};
        if (kind == 8) edit.updates = {{{parameter, 0}, {-1, 0}}};
        if (kind == 9) edit.updates = {{{parameter, 0}, {0, std::numeric_limits<double>::infinity()}}};
        if (kind == 10) edit.insertions = {{{static_cast<CMotionTarget>(99), "A"}, {3, 3}}};
        if (kind >= 11) {
            CMotionTimeline::PointInsertion insertion{parameter, {3, 3}, 1, {{2.2, 2.2}, {2.7, 2.7}}, QPointF(2, 2)};
            if (kind == 11) insertion.segmentType = 4;
            if (kind == 12) insertion.segmentStart.reset();
            if (kind == 13) insertion.controls.resize(1);
            if (kind == 14) insertion.segmentType = 0;
            if (kind == 15) insertion.segmentStart = QPointF(4, 4);
            if (kind == 16) insertion.controls[0].setX(1);
            if (kind == 17) insertion.controls[0].setY(std::numeric_limits<double>::quiet_NaN());
            edit.insertions = {insertion};
        }
        auto result = sentinel(); const auto held = result; QString error;
        QVERIFY(!timeline.EditPoints(edit, result, &error)); QVERIFY(!error.isEmpty());
        QVERIFY(timeline.HasSameContent(original)); QCOMPARE(state(timeline), saved); QVERIFY(sameResult(result, held));
    }

    void noOpDoesNotMaterializeHiddenBezierDefaultsOrSourceMetadata()
    {
        CMotionTimeline timeline; QVERIFY(load(timeline, {curve(parameter, {0, 0, 1, 4, .5, 5, .8, 1, 1})}, true));
        const auto original = timeline; CMotionTimeline::BatchEdit edit; edit.updates = {{{parameter, 1}, {1, 1}}};
        CMotionTimeline::BatchResult result; QVERIFY(timeline.EditPoints(edit, result)); QVERIFY(!result.changed); QVERIFY(timeline.HasSameContent(original));
        QCOMPARE(result.oldToNew[parameter], QVector<int>({0, 1}));
        QVERIFY(timeline.EditPoints({}, result)); QVERIFY(!result.changed); QVERIFY(timeline.HasSameContent(original));
        CMotionTimeline linear; QVERIFY(linear.AddPoint("A", 0, 0)); QVERIFY(linear.AddPoint("A", 1, 1));
        QVERIFY(linear.SetSegmentType("A", 1, 1)); const auto implicit = linear;
        edit.updates = {{{{CMotionTarget::Parameter, "A"}, 1}, {1, 1}}};
        QVERIFY(linear.EditPoints(edit, result)); QVERIFY(!result.changed); QVERIFY(linear.HasSameContent(implicit));
    }

    void noOpReplacementCanReturnNewSelectionWithoutHistory()
    {
        CMotionTimeline timeline; QVERIFY(timeline.AddPoint("A", 0, 0)); QVERIFY(timeline.AddPoint("A", 1, 1)); const auto before = timeline;
        const CMotionTrack a{CMotionTarget::Parameter, "A"}; CMotionTimeline::BatchEdit edit;
        edit.removes = {{a, 1}}; edit.insertions = {{a, {1, 1}}}; CMotionTimeline::BatchResult result;
        QVERIFY(timeline.EditPoints(edit, result)); QVERIFY(!result.changed); QVERIFY(timeline.HasSameContent(before));
        QCOMPARE(result.oldToNew[a], QVector<int>({0, -1})); QCOMPARE(result.inserted, CMotionPointSelection({{a, 1}}));
    }

    void controlOverflowRejectsInsteadOfFlatteningCurve()
    {
        const double maximum = std::numeric_limits<float>::max(); CMotionTimeline timeline;
        QVERIFY(load(timeline, {curve(parameter, {0, 0, 1, .25, maximum, .75, maximum, 1, 0})}));
        const auto before = timeline; CMotionTimeline::BatchEdit edit; edit.updates = {{{parameter, 0}, {0, maximum}}};
        auto result = sentinel(); const auto held = result; QString error;
        QVERIFY(!timeline.EditPoints(edit, result, &error)); QVERIFY(!error.isEmpty());
        QVERIFY(timeline.HasSameContent(before)); QVERIFY(sameResult(result, held));
    }

    void restrictedConversionPreservesShapeAndMetadataAcrossAllTargets()
    {
        CMotionTimeline timeline; QJsonArray curves;
        for (const auto& track : {parameter, model, part}) curves.append(curve(track, {0, 0, 1, 3, .8, 4, .2, 1, 1, 2, 2, .5}));
        QVERIFY(load(timeline, curves, true)); const auto before = timeline; const auto oldState = state(timeline);
        QVERIFY(timeline.AreBeziersRestricted());
        const auto effective = timeline.BezierControls(parameter, 1);
        QCOMPARE(effective, QVector<QPointF>({{1.0 / 3, .8}, {2.0 / 3, .2}}));
        QString error; QVERIFY2(timeline.ConvertToUnrestricted(&error), qPrintable(error)); QVERIFY(!timeline.AreBeziersRestricted());
        for (const auto& track : {parameter, model, part}) {
            QCOMPARE(timeline.BezierControls(track, 1), effective);
            QCOMPARE(timeline.Tracks()[track], before.Tracks()[track]); QCOMPARE(timeline.SegmentType(track, 2), 2);
            for (int sample = 0; sample <= 20; ++sample) QVERIFY(std::abs(timeline.Evaluate(track, sample / 10.f) - before.Evaluate(track, sample / 10.f)) < 1e-5f);
        }
        auto expected = oldState["motion"].toObject(); auto actual = state(timeline)["motion"].toObject();
        auto meta = expected["Meta"].toObject(); meta["AreBeziersRestricted"] = false; expected["Meta"] = meta;
        expected["Curves"] = actual["Curves"]; QCOMPARE(actual, expected);
        const auto converted = timeline; QVERIFY(timeline.ConvertToUnrestricted(&error)); QVERIFY(timeline.HasSameContent(converted));
        QByteArray bytes; QVERIFY(timeline.ToJson(bytes, timeline.Fps(), timeline.Loop(), &error));
        CMotionTimeline restored; const auto exported = QJsonDocument::fromJson(bytes).object();
        QVERIFY(restored.RestoreState({{"version", 1}, {"motion", exported}, {"playhead", 0}}, nullptr, &error));
        QCOMPARE(restored.BezierControls(parameter, 1), effective); QCOMPARE(exported["Meta"].toObject()["TotalPointCount"].toInt(), 15);
    }

    void explicitSegmentEditsUseFinalFrameAndAllowCrossedHandles()
    {
        CMotionTimeline timeline; QVERIFY(load(timeline, {curve(parameter, {0, 0, 0, 1, 1, 0, 2, 2})}));
        CMotionTimeline::BatchEdit edit; edit.updates = {{{parameter, 1}, {1.5, 3}}};
        edit.segments = {{{parameter, 1}, 1, {{1.3, 4}, {.2, -2}}}, {{parameter, 2}, 2, {}}};
        CMotionTimeline::BatchResult result; QString error; QVERIFY2(timeline.EditPoints(edit, result, &error), qPrintable(error));
        QVERIFY(result.changed); QCOMPARE(timeline.BezierControls(parameter, 1), QVector<QPointF>({{1.3, 4}, {.2, -2}}));
        QCOMPARE(timeline.SegmentType(parameter, 2), 2); QVERIFY(timeline.BezierControls(parameter, 2).isEmpty());
        edit = {}; edit.segments = {{{parameter, 1}, 1, {}}}; QVERIFY(timeline.EditPoints(edit, result));
        QCOMPARE(timeline.BezierControls(parameter, 1), QVector<QPointF>({{.5, 1}, {1, 2}}));
        const auto before = timeline; QVERIFY(timeline.EditPoints(edit, result)); QVERIFY(!result.changed); QVERIFY(timeline.HasSameContent(before));
    }

    void restrictedExplicitHandlesPreserveIgnoredTimesAndMode()
    {
        CMotionTimeline timeline; QVERIFY(load(timeline, {curve(parameter, {0, 0, 1, 4, .5, 5, .8, 1, 1})}, true));
        CMotionTimeline::BatchEdit edit; edit.segments = {{{parameter, 1}, 1, {{-9, 2}, {8, -1}}}};
        CMotionTimeline::BatchResult result; QVERIFY(timeline.EditPoints(edit, result)); QVERIFY(timeline.AreBeziersRestricted());
        QCOMPARE(timeline.BezierControls(parameter, 1), QVector<QPointF>({{1.0 / 3, 2}, {2.0 / 3, -1}}));
        const auto segments = state(timeline)["motion"].toObject()["Curves"].toArray()[0].toObject()["Segments"].toArray();
        QCOMPARE(segments[3].toDouble(), 4.0); QCOMPARE(segments[5].toDouble(), 5.0);
        QCOMPARE(segments[4].toDouble(), 2.0); QCOMPARE(segments[6].toDouble(), -1.0);
        edit.segments[0].controls.clear(); QVERIFY(timeline.EditPoints(edit, result));
        const auto reset = state(timeline)["motion"].toObject()["Curves"].toArray()[0].toObject()["Segments"].toArray();
        QCOMPARE(reset[3].toDouble(), 4.0); QCOMPARE(reset[5].toDouble(), 5.0);
        QCOMPARE(reset[4].toDouble(), 1.0 / 3); QCOMPARE(reset[6].toDouble(), 2.0 / 3);
    }

    void invalidSegmentEditsPreserveContentAndOutput_data()
    {
        QTest::addColumn<int>("kind"); const char* names[]{"first", "unknown", "duplicate", "removed", "bad-type", "one-handle", "linear-handles", "outside-final-range", "nonfinite"};
        for (int kind = 0; kind < 9; ++kind) QTest::newRow(names[kind]) << kind;
    }
    void invalidSegmentEditsPreserveContentAndOutput()
    {
        QFETCH(int, kind); CMotionTimeline timeline; QVERIFY(load(timeline, {curve(parameter, {0, 0, 0, 1, 1})}));
        const auto before = timeline; CMotionTimeline::BatchEdit edit;
        edit.segments = {{{parameter, 1}, 1, {{.2, .3}, {.8, .7}}}};
        if (kind == 0) edit.segments[0].ref.index = 0;
        if (kind == 1) edit.segments[0].ref.track = part;
        if (kind == 2) edit.segments.append(edit.segments.first());
        if (kind == 3) edit.removes = {{parameter, 1}};
        if (kind == 4) edit.segments[0].type = 9;
        if (kind == 5) edit.segments[0].controls.resize(1);
        if (kind == 6) edit.segments[0].type = 0;
        if (kind == 7) { edit.updates = {{{parameter, 1}, {.5, 1}}}; }
        if (kind == 8) edit.segments[0].controls[0].setY(std::numeric_limits<double>::infinity());
        auto result = sentinel(); const auto held = result; QString error;
        QVERIFY(!timeline.EditPoints(edit, result, &error)); QVERIFY(!error.isEmpty());
        QVERIFY(timeline.HasSameContent(before)); QVERIFY(sameResult(result, held));
    }

    void singlePointPathsRejectHandleOverflow_data()
    {
        QTest::addColumn<int>("kind"); QTest::newRow("update") << 0; QTest::newRow("remove") << 1; QTest::newRow("insert") << 2;
    }
    void singlePointPathsRejectHandleOverflow()
    {
        QFETCH(int, kind); const double maximum = std::numeric_limits<float>::max(); CMotionTimeline timeline;
        const QJsonArray points = kind == 1 ? QJsonArray{0, maximum, 0, 1, 0, 1, 1.2, maximum, 1.8, maximum, 2, 0}
            : QJsonArray{0, 0, 1, .25, maximum, .75, maximum, 1, 0};
        QVERIFY(load(timeline, {curve(parameter, points)})); const auto before = timeline;
        const bool accepted = kind == 0 ? timeline.UpdatePoint(parameter, 0, 0, maximum)
            : kind == 1 ? timeline.RemovePoint(parameter, 1) : timeline.InsertPoint(parameter, .5, maximum);
        QVERIFY(!accepted); QVERIFY(timeline.HasSameContent(before));
    }

    void explicitHandlesCanReplaceAnOtherwiseOverflowingRetarget()
    {
        const double maximum = std::numeric_limits<float>::max(); CMotionTimeline timeline;
        QVERIFY(load(timeline, {curve(parameter, {0, 0, 1, .25, maximum, .75, maximum, 1, 0})}));
        CMotionTimeline::BatchEdit edit; edit.updates = {{{parameter, 0}, {0, maximum}}};
        edit.segments = {{{parameter, 1}, 1, {{.25, 1}, {.75, 1}}}};
        CMotionTimeline::BatchResult result; QVERIFY(timeline.EditPoints(edit, result));
        QCOMPARE(timeline.BezierControls(parameter, 1), QVector<QPointF>({{.25, 1}, {.75, 1}}));
    }

    void archivedTracksKeepShapeAcrossBezierModes_data()
    {
        QTest::addColumn<bool>("archivedRestricted"); QTest::addColumn<bool>("destinationRestricted");
        QTest::newRow("restricted-to-free") << true << false;
        QTest::newRow("free-to-restricted") << false << true;
        QTest::newRow("restricted-to-restricted") << true << true;
        QTest::newRow("free-to-free") << false << false;
    }
    void archivedTracksKeepShapeAcrossBezierModes()
    {
        QFETCH(bool, archivedRestricted); QFETCH(bool, destinationRestricted);
        const QJsonArray archivedPoints = archivedRestricted ? QJsonArray{0, 0, 1, 3, .9, 4, .1, 1, 1, 2, 2, 0}
            : QJsonArray{0, 0, 1, .8, .9, .2, .1, 1, 1, 2, 2, 0};
        CMotionTimeline source; QVERIFY(load(source, {curve(parameter, archivedPoints)}, archivedRestricted));
        const auto original = source; CMotionTimeline::ParameterTracks archive; QVERIFY(source.RemoveParameterTracks({parameter.id}, archive));
        const CMotionTrack other{CMotionTarget::Parameter, "Other"}; QJsonArray current;
        for (const auto& track : {other, model, part}) current.append(curve(track, destinationRestricted
            ? QJsonArray{0, 1, 1, 3, .25, 4, 1.25, 1, .5} : QJsonArray{0, 1, 1, .2, .25, .8, 1.25, 1, .5}));
        CMotionTimeline timeline; QVERIFY(load(timeline, current, destinationRestricted)); const auto before = timeline; QString error;
        QVERIFY2(timeline.RestoreParameterTracks(archive, &error), qPrintable(error));
        QCOMPARE(timeline.AreBeziersRestricted(), archivedRestricted && destinationRestricted);
        QCOMPARE(timeline.Tracks()[parameter], original.Tracks()[parameter]); QCOMPARE(timeline.Events(), before.Events());
        QCOMPARE(timeline.Timing().fps, before.Timing().fps); QCOMPARE(timeline.Timing().duration, before.Timing().duration);
        QCOMPARE(timeline.TrackFadeTimes(parameter).fadeIn, original.TrackFadeTimes(parameter).fadeIn);
        for (int sample = 0; sample <= 40; ++sample) {
            const float time = sample / 20.f;
            QVERIFY(std::abs(timeline.Evaluate(parameter, time) - original.Evaluate(parameter, time)) < 1e-5f);
            for (const auto& track : {other, model, part}) {
                QCOMPARE(timeline.Tracks()[track], before.Tracks()[track]);
                QVERIFY(std::abs(timeline.Evaluate(track, time) - before.Evaluate(track, time)) < 1e-5f);
            }
        }
        CMotionTimeline::ParameterTracks repeated; const auto restored = timeline;
        QVERIFY(timeline.RemoveParameterTracks({parameter.id}, repeated)); QVERIFY(timeline.RestoreParameterTracks(repeated));
        QCOMPARE(state(timeline), state(restored));
        QByteArray bytes; QVERIFY(timeline.ToJson(bytes, timeline.Fps(), timeline.Loop(), &error));
        const auto root = QJsonDocument::fromJson(bytes).object(); bool found = false;
        for (const auto& value : root["Curves"].toArray()) {
            const auto row = value.toObject();
            if (row["Target"] == "Parameter" && row["Id"] == parameter.id) {
                found = true; QCOMPARE(row["CustomCurve"].toString(), QString("retained")); QCOMPARE(row["FadeOutTime"].toDouble(), -1.0);
            }
        }
        QVERIFY(found);
    }

    void emptyAndLinearArchivesDoNotConvertRestrictedDestination()
    {
        CMotionTimeline source; QVERIFY(source.AddPoint("Linear", 0, 0)); QVERIFY(source.AddPoint("Linear", 1, 1));
        CMotionTimeline::ParameterTracks archive; QVERIFY(source.RemoveParameterTracks({"Linear"}, archive));
        CMotionTimeline timeline; QVERIFY(load(timeline, {curve(model, {0, 0, 1, 4, .2, 5, .8, 1, 1})}, true));
        const auto before = timeline; CMotionTimeline::ParameterTracks empty; QVERIFY(timeline.RestoreParameterTracks(empty));
        QVERIFY(timeline.HasSameContent(before)); QVERIFY(timeline.RestoreParameterTracks(archive)); QVERIFY(timeline.AreBeziersRestricted());
        QCOMPARE(timeline.BezierControls(model, 1), before.BezierControls(model, 1));
    }

    void archivedModeRestoreFailuresPreserveSourceAndRecordingClock_data()
    {
        QTest::addColumn<int>("kind"); QTest::newRow("duplicate") << 0; QTest::newRow("short-duration") << 1;
        QTest::newRow("point-budget") << 2;
    }
    void archivedModeRestoreFailuresPreserveSourceAndRecordingClock()
    {
        QFETCH(int, kind); CMotionTimeline source;
        QVERIFY(load(source, {curve(parameter, {0, 0, 1, .8, .9, .2, .1, 4, 1})}));
        CMotionTimeline::ParameterTracks archive; QVERIFY(source.RemoveParameterTracks({parameter.id}, archive));
        qint64 now = 0; CMotionTimeline timeline([&] { return now; });
        QVERIFY(load(timeline, {curve(model, {0, 0, 1, 4, .2, 5, .8, 1, 1})}, true));
        if (kind == 0) QVERIFY(timeline.AddPoint(parameter, 0, 9));
        if (kind == 1) { QVERIFY(timeline.SetEvents({})); QVERIFY(timeline.SetTiming(30, 1)); }
        if (kind == 2) {
            QVector<QPointF> points(CMotionTimeline::MaximumPointCount - 3);
            for (int index = 0; index < points.size(); ++index) points[index] = QPointF(index, .5);
            QVERIFY(timeline.ReplaceParameterSamples({{"Dense", points}}));
        }
        timeline.StartRecording(); now = 500; QVERIFY(timeline.PauseRecordingClock()); const auto before = timeline;
        const auto saved = state(timeline); QString error; QVERIFY(!timeline.RestoreParameterTracks(archive, &error)); QVERIFY(!error.isEmpty());
        QVERIFY(timeline.HasSameContent(before)); QCOMPARE(state(timeline), saved); QVERIFY(timeline.AreBeziersRestricted());
        QVERIFY(timeline.IsRecording()); QVERIFY(timeline.IsRecordingClockPaused());
        CMotionTimeline valid; QVERIFY(load(valid, {curve(model, {0, 0, 1, 4, .2, 5, .8, 1, 1})}, true));
        QVERIFY(valid.RestoreParameterTracks(archive)); QVERIFY(!valid.AreBeziersRestricted());
    }

    void archivedFreeTracksConvertCurrentRestrictedTracksDuringRecording()
    {
        CMotionTimeline source; QVERIFY(load(source, {curve(parameter, {0, 0, 1, .8, .9, .2, .1, 1, 1})}));
        CMotionTimeline::ParameterTracks archive; QVERIFY(source.RemoveParameterTracks({parameter.id}, archive));
        qint64 now = 0; CMotionTimeline timeline([&] { return now; });
        QVERIFY(load(timeline, {curve(model, {0, 0, 1, 4, .2, 5, .8, 1, 1})}, true));
        const auto before = timeline; timeline.StartRecording(); now = 1000; QVERIFY(timeline.PauseRecordingClock());
        QVERIFY(timeline.RestoreParameterTracks(archive)); QVERIFY(!timeline.AreBeziersRestricted());
        QVERIFY(timeline.IsRecording()); QVERIFY(timeline.IsRecordingClockPaused());
        for (int sample = 0; sample <= 10; ++sample)
            QVERIFY(std::abs(timeline.Evaluate(model, sample / 10.f) - before.Evaluate(model, sample / 10.f)) < 1e-5f);
        now = 3000; QVERIFY(timeline.ResumeRecordingClock()); now = 3500; timeline.Record("Recorded", 2);
        QCOMPARE((timeline.Tracks()[{CMotionTarget::Parameter, "Recorded"}].last().x()), 6.5);
    }

    void lateFirstKeyMatchesSdkSegmentBoundaries_data()
    {
        QTest::addColumn<int>("target"); QTest::addColumn<int>("mode");
        for (int target = 0; target < 3; ++target) for (int mode = 0; mode < 5; ++mode)
            QTest::newRow(qPrintable(QString("target-%1-mode-%2").arg(target).arg(mode))) << target << mode;
    }
    void lateFirstKeyMatchesSdkSegmentBoundaries()
    {
        QFETCH(int, target); QFETCH(int, mode); const CMotionTrack track{static_cast<CMotionTarget>(target), "Late"};
        const bool bezier = mode == 1 || mode == 4;
        QJsonArray points{1, 0, bezier ? 1 : mode};
        if (bezier) { points.append(1.2); points.append(.8); points.append(1.8); points.append(.2); }
        for (const double number : {2., 1., 0., 3., 0., 2., 4., .5}) points.append(number);
        CMotionTimeline timeline; QVERIFY(load(timeline, {curve(track, points)}, mode == 1));
        for (float time : {0.f, .5f}) QCOMPARE(timeline.Evaluate(track, time), mode == 3 || mode == 4 ? 1.f : 0.f);
        QCOMPARE(timeline.Evaluate(track, 1), mode == 3 ? 1.f : mode == 4 ? 1.05259801e-6f : 0.f);
        QVERIFY(std::abs(timeline.Evaluate(track, 1.5f) - (mode == 2 ? 0.f : mode == 3 ? 1.f : .5f)) < 1e-6f);
        for (const auto& sample : QVector<QPointF>{{2, 1}, {2.5, .5}, {3, 0}, {3.5, 0}, {4, .5}, {5, .5}})
            QCOMPARE(timeline.Evaluate(track, sample.x()), float(sample.y()));
        QByteArray bytes; QVERIFY(timeline.ToJson(bytes, timeline.Fps(), timeline.Loop()));
        CMotionTimeline restored; QVERIFY(restored.RestoreState({{"version", 1}, {"motion", QJsonDocument::fromJson(bytes).object()}, {"playhead", 0}}));
        for (float time : {0.f, .5f, 1.f, 1.5f, 2.f, 2.5f, 3.f, 3.5f, 4.f, 5.f})
            QCOMPARE(restored.Evaluate(track, time), timeline.Evaluate(track, time));
    }

    void recordingRejectsBatchAndConversionWithoutChangingClock()
    {
        qint64 now = 0; CMotionTimeline timeline([&] { return now; });
        QVERIFY(load(timeline, {curve(parameter, {0, 0, 1, .2, .3, .8, .7, 1, 1})}, true));
        timeline.StartRecording(); now = 1000; QVERIFY(timeline.PauseRecordingClock()); const auto before = timeline;
        CMotionTimeline::BatchEdit edit; edit.updates = {{{parameter, 0}, {.1, 1}}}; auto result = sentinel(); const auto held = result; QString error;
        QVERIFY(!timeline.EditPoints(edit, result, &error)); QVERIFY(!timeline.ConvertToUnrestricted(&error));
        QVERIFY(timeline.HasSameContent(before)); QVERIFY(timeline.IsRecording()); QVERIFY(timeline.IsRecordingClockPaused()); QVERIFY(sameResult(result, held));
        now = 3000; QVERIFY(timeline.ResumeRecordingClock()); now = 3500; timeline.Record("Recorded", 2);
        QCOMPARE((timeline.Tracks()[{CMotionTarget::Parameter, "Recorded"}].last().x()), 6.5);
    }

    void budgetsRejectEncodedSizeAndCountAtomically()
    {
        CMotionTimeline timeline; QVERIFY(timeline.AddPoint("A", 0, 0)); const auto before = timeline;
        CMotionTimeline::BatchEdit edit; edit.insertions = {{{CMotionTarget::Parameter, QString(17 * 1024 * 1024, 'x')}, {1, 1}}};
        auto result = sentinel(); const auto held = result; QString error;
        QVERIFY(!timeline.EditPoints(edit, result, &error)); QVERIFY(!error.isEmpty()); QVERIFY(timeline.HasSameContent(before)); QVERIFY(sameResult(result, held));
        QVector<QPointF> points; points.reserve(CMotionTimeline::MaximumPointCount);
        for (int index = 0; index < CMotionTimeline::MaximumPointCount; ++index) points.append({double(index), .5});
        QVERIFY(timeline.ReplaceParameterSamples({{"A", points}}, &error)); const auto dense = timeline;
        edit.insertions = {{{CMotionTarget::Parameter, "A"}, {100001, 1}}};
        QVERIFY(!timeline.EditPoints(edit, result, &error)); QVERIFY(timeline.HasSameContent(dense)); QVERIFY(sameResult(result, held));
        edit.removes = {{{CMotionTarget::Parameter, "A"}, 0}};
        QVERIFY2(timeline.EditPoints(edit, result, &error), qPrintable(error)); QCOMPARE(timeline.TotalPointCount(), CMotionTimeline::MaximumPointCount);
        QCOMPARE((result.oldToNew[{CMotionTarget::Parameter, "A"}].first()), -1);
        QCOMPARE(result.inserted, CMotionPointSelection({{{CMotionTarget::Parameter, "A"}, CMotionTimeline::MaximumPointCount - 1}}));
    }
};
QTEST_GUILESS_MAIN(MotionBatchEditTests)
#include "motion_batch_edit_tests.moc"
