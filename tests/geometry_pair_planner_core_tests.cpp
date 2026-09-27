#include "geometry_pair_planner_fixture.h"
#include "real_core_model_fixture.h"
#include "live2dstruct/cdeformereditcontext.h"
#include "live2dstruct/ckeyformeditcontroller.h"
#include <QtTest>

using namespace GeometryPairData;
namespace {
void setPose(CoreModel& core, const QMap<QString, float>& pose)
{
    QVector<float> values;
    for (int index = 0; index < csmGetParameterCount(core.value); ++index)
        values.append(pose.value(QString::fromUtf8(csmGetParameterIds(core.value)[index])));
    core.pose(values);
}
QVector<QPointF> positions(CoreModel& core, const QString& id)
{
    for (int index = 0; index < csmGetDrawableCount(core.value); ++index) {
        if (QString::fromUtf8(csmGetDrawableIds(core.value)[index]) != id) continue;
        QVector<QPointF> result;
        for (int vertex = 0; vertex < csmGetDrawableVertexCounts(core.value)[index]; ++vertex) {
            const auto value = csmGetDrawableVertexPositions(core.value)[index][vertex]; result.append({value.X, value.Y});
        }
        return result;
    }
    return {};
}
bool near(QPointF actual, QPointF expected)
{
    return std::hypot(actual.x() - expected.x(), actual.y() - expected.y()) < 0.00003;
}
QPointF mirror(QPointF value) { return {-value.x(), value.y()}; }
}

class GeometryPairPlannerCoreTests : public QObject
{
    Q_OBJECT
private slots:
    void weightedControllerMatchesCoreDisplacement_data()
    {
        QTest::addColumn<int>("type"); QTest::addColumn<bool>("reverse");
        for (int type : {int(Geometry::ArtMesh), int(Geometry::Warp)}) for (bool reverse : {false, true})
            QTest::newRow(qPrintable(QString("%1-%2").arg(type).arg(reverse))) << type << reverse;
    }
    void weightedControllerMatchesCoreDisplacement()
    {
        QFETCH(int, type); QFETCH(bool, reverse);
        Fixture fixture; QString error;
        QVERIFY2(make(Geometry::Type(type), fixture, &error, true, true), qPrintable(error));
        if (type == Geometry::ArtMesh) QVERIFY2(reindexTarget(fixture, &error), qPrintable(error));
        CModelDocument document; QVERIFY(document.LoadBytes(fixture.bytes, "weighted-core.moc3", &error));
        QVERIFY(document.SetGeometryObjectPairs({document.SourceDigest(), {fixture.pair}}, &error));
        const auto pair = fixture.pair;
        const auto source = reverse ? pair.second : pair.first;
        const auto target = reverse ? pair.first : pair.second;
        const int sourceKey = reverse ? 3 : 0, targetKey = reverse ? 0 : 3;
        CKeyformEditController controller; controller.SetDocument(&document);
        QByteArray published;
        controller.SetPublisher([&](const CModelDocument& next, const auto&, bool, QString* detail) {
            return next.BuildRuntimeMoc(published, detail);
        }, [](QString*) { return true; });
        QVERIFY2(controller.Activate(source.object.id, Pairs::TypeName(source.object.type), sourceKey, {}, &error), qPrintable(error));
        QMap<QString, float> targetPose = controller.Pose(), corner;
        QVERIFY(document.KeyformParameterValues(target.object.id, Pairs::TypeName(target.object.type), targetKey, corner, &error));
        for (auto it = corner.cbegin(); it != corner.cend(); ++it) targetPose[it.key()] = it.value();
        CoreModel sourceCore, targetCore;
        QVERIFY(sourceCore.load(published)); setPose(sourceCore, controller.Pose());
        QVERIFY(targetCore.load(published)); setPose(targetCore, targetPose);
        const QString sourceId = reverse ? fixture.secondWitness : fixture.firstWitness;
        const QString targetId = reverse ? fixture.firstWitness : fixture.secondWitness;
        const auto before = positions(sourceCore, sourceId), targetBefore = positions(targetCore, targetId);
        QVERIFY(before.size() >= 4);
        QVector<double> weights(type == Geometry::ArtMesh ? before.size() : 4, 0);
        weights[0] = .2; weights[2] = .6;
        QVector<float> runtime;
        for (auto point : before) { runtime.append(float(point.x())); runtime.append(float(point.y())); }
        QVERIFY2(type == Geometry::ArtMesh ? controller.BeginWeightedDrag(weights, runtime, &error) :
                                            controller.BeginWeightedWarpDrag(weights, &error), qPrintable(error));
        const QPointF delta(.025, -.015);
        QVERIFY2(type == Geometry::ArtMesh ? controller.DragBy(delta, &error) : controller.MoveDeformerDrag(delta, 0, &error), qPrintable(error));
        CoreModel changedSource, changedTarget;
        QVERIFY(changedSource.load(published)); setPose(changedSource, controller.Pose());
        QVERIFY(changedTarget.load(published)); setPose(changedTarget, targetPose);
        const auto after = positions(changedSource, sourceId), targetAfter = positions(changedTarget, targetId);
        for (int i = 0; i < weights.size(); ++i) {
            const int paired = type == Geometry::ArtMesh ? (reverse ? pair.firstVertexToSecondVertex.indexOf(i) : pair.firstVertexToSecondVertex[i]) : i;
            QVERIFY2(near(after[i], before[i] + delta * weights[i]), qPrintable(QString("source vertex %1").arg(i)));
            QVERIFY2(near(targetAfter[paired], targetBefore[paired] + mirror(delta * weights[i])), qPrintable(QString("target vertex %1").arg(i)));
        }
        QVERIFY(controller.CancelDrag(&error));
        CoreModel cancelled; QVERIFY(cancelled.load(published)); setPose(cancelled, controller.Pose());
        QCOMPARE(positions(cancelled, sourceId), before);
        QVERIFY(sourceCore.intact()); QVERIFY(targetCore.intact()); QVERIFY(changedSource.intact()); QVERIFY(changedTarget.intact()); QVERIFY(cancelled.intact());
    }
    void mirrorAndSynchronizationMatchCore_data()
    {
        QTest::addColumn<int>("type"); QTest::addColumn<bool>("forward"); QTest::addColumn<bool>("synchronize");
        for (int type : {0, 1, 2}) for (bool forward : {false, true}) for (bool synchronize : {false, true})
            QTest::newRow(qPrintable(QString::number(type) + (forward ? "-forward" : "-reverse") + (synchronize ? "-sync" : "-copy")))
                << type << forward << synchronize;
    }
    void mirrorAndSynchronizationMatchCore()
    {
        QFETCH(int, type); QFETCH(bool, forward); QFETCH(bool, synchronize);
        Fixture fixture; QString error;
        QVERIFY2(make(Geometry::Type(type), fixture, &error, true, true), qPrintable(error));
        if (type == Geometry::ArtMesh) QVERIFY2(reindexTarget(fixture, &error), qPrintable(error));
        CModelDocument document; QVERIFY(document.LoadBytes(fixture.bytes, "core-pair.moc3", &error));
        auto pair = fixture.pair;
        if (type == Geometry::Rotation)
            QVERIFY2(CGeometryPairPlanner::PrepareRotationCorrespondence(document, pair, 0, 3, {}, pair, &error), qPrintable(error));
        Geometry source;
        const auto& sourceEndpoint = forward ? pair.first : pair.second;
        const auto& targetEndpoint = forward ? pair.second : pair.first;
        QVERIFY(document.CaptureGeometryKeyform(Geometry::Type(type), sourceEndpoint.object.id, 0, source));
        CGeometryPairPlanner::Result plan;
        QVERIFY2(synchronize ? CGeometryPairPlanner::PrepareSynchronized(document, pair, forward, source, changed(source), {}, plan, &error) :
            CGeometryPairPlanner::PrepareMirror(document, pair, forward, 0, {}, plan, &error), qPrintable(error));
        QVERIFY(plan.changed);
        CoreModel sourceCore, targetBeforeCore;
        QVERIFY(sourceCore.load(fixture.bytes)); setPose(sourceCore, plan.sourcePose);
        QVERIFY(targetBeforeCore.load(fixture.bytes)); setPose(targetBeforeCore, plan.targetPose);
        const QString sourceWitness = forward ? fixture.firstWitness : fixture.secondWitness;
        const QString targetWitness = forward ? fixture.secondWitness : fixture.firstWitness;
        const auto beforeSource = positions(sourceCore, sourceWitness), beforeTarget = positions(targetBeforeCore, targetWitness);
        QVERIFY(beforeSource.size() >= 4); QVERIFY(beforeTarget.size() == beforeSource.size());
        const auto originalSource = document.AuthoringStructureState();
        QVERIFY(document.ExecuteEdit(plan.edit));
        QByteArray bytes; QVERIFY2(document.BuildRuntimeMoc(bytes, &error), qPrintable(error));
        CoreModel afterSourceCore, afterTargetCore;
        QVERIFY(afterSourceCore.load(bytes)); setPose(afterSourceCore, plan.sourcePose);
        QVERIFY(afterTargetCore.load(bytes)); setPose(afterTargetCore, plan.targetPose);
        const auto afterSource = positions(afterSourceCore, sourceWitness), afterTarget = positions(afterTargetCore, targetWitness);
        QCOMPARE(frame(sourceCore.value).definition, frame(afterSourceCore.value).definition);
        QCOMPARE(frame(targetBeforeCore.value).definition, frame(afterTargetCore.value).definition);
        QSet<int> allowed;
        for (int index = 0; index < csmGetDrawableCount(sourceCore.value); ++index) {
            const QString id = QString::fromUtf8(csmGetDrawableIds(sourceCore.value)[index]);
            if (id == sourceWitness || id == targetWitness) allowed.insert(index);
        }
        for (const auto index : ::changed(frame(sourceCore.value), frame(afterSourceCore.value))) QVERIFY(allowed.contains(index));
        for (const auto index : ::changed(frame(targetBeforeCore.value), frame(afterTargetCore.value))) QVERIFY(allowed.contains(index));
        if (type != Geometry::Rotation) {
            const int count = type == Geometry::ArtMesh ? beforeSource.size() : 4;
            for (int vertex = 0; vertex < count; ++vertex) {
                const int targetVertex = type == Geometry::ArtMesh ? (forward ? pair.firstVertexToSecondVertex[vertex] : pair.firstVertexToSecondVertex.indexOf(vertex)) : vertex;
                const QPointF expected = synchronize ? beforeTarget[targetVertex] + mirror(afterSource[vertex] - beforeSource[vertex]) : mirror(beforeSource[vertex]);
                QVERIFY2(near(afterTarget[targetVertex], expected), qPrintable(QString("vertex %1 actual %2,%3 expected %4,%5")
                    .arg(vertex).arg(afterTarget[targetVertex].x()).arg(afterTarget[targetVertex].y()).arg(expected.x()).arg(expected.y())));
            }
            if (!synchronize) QCOMPARE(afterSource, beforeSource);
        } else if (!synchronize) {
            const QPointF x = beforeSource[1] - beforeSource[0], y = beforeSource[2] - beforeSource[0];
            const QPointF lx = forward ? pair.rotationLocalX : QPointF(pair.rotationLocalX.x(), pair.rotationLocalY.x());
            const QPointF ly = forward ? pair.rotationLocalY : QPointF(pair.rotationLocalX.y(), pair.rotationLocalY.y());
            const QVector<QPointF> expected{mirror(beforeSource[0]), mirror(beforeSource[0] + x * lx.x() + y * lx.y()),
                                           mirror(beforeSource[0] + x * ly.x() + y * ly.y())};
            for (int vertex = 0; vertex < 3; ++vertex) QVERIFY(near(afterTarget[vertex], expected[vertex]));
        } else {
            const QPointF oldX = beforeSource[1] - beforeSource[0], oldY = beforeSource[2] - beforeSource[0];
            const QPointF newX = afterSource[1] - afterSource[0], newY = afterSource[2] - afterSource[0];
            const double scaleSquared = QPointF::dotProduct(oldX, oldX);
            const QPointF origin = beforeTarget[0] + mirror(afterSource[0] - beforeSource[0]);
            QVERIFY(near(afterTarget[0], origin));
            for (int vertex = 1; vertex < 3; ++vertex) {
                const QPointF delta = mirror(beforeTarget[vertex] - beforeTarget[0]);
                const QPointF expected = origin + mirror(newX * (QPointF::dotProduct(oldX, delta) / scaleSquared) +
                                                        newY * (QPointF::dotProduct(oldY, delta) / scaleSquared));
                QVERIFY2(near(afterTarget[vertex], expected), qPrintable(error));
            }
        }
        QVERIFY(document.UndoEdit()); QCOMPARE(document.AuthoringStructureState(), originalSource);
        QVERIFY(document.RedoEdit());
        QVERIFY(sourceCore.intact()); QVERIFY(targetBeforeCore.intact()); QVERIFY(afterSourceCore.intact()); QVERIFY(afterTargetCore.intact());
    }

    void reversedNonQuadControlsDoNotClaimInteriorConjugacy()
    {
        Fixture fixture; QString error;
        QVERIFY2(make(Geometry::Warp, fixture, &error, false, false, true, false, false), qPrintable(error));
        CModelDocument document; QVERIFY(document.LoadBytes(fixture.bytes, "nonquad.moc3"));
        CGeometryPairPlanner::Result plan;
        QVERIFY2(CGeometryPairPlanner::PrepareMirror(document, fixture.pair, true, 0, {}, plan, &error), qPrintable(error));
        QVERIFY(plan.changed); QVERIFY(plan.diagnostics.join(' ').contains(QStringLiteral("控制点")));
        QVERIFY(plan.diagnostics.join(' ').contains(QStringLiteral("固定三角")));
        CoreModel before; QVERIFY(before.load(fixture.bytes)); setPose(before, plan.sourcePose);
        const auto source = positions(before, fixture.firstWitness); QVERIFY(source.size() >= 5);
        QVERIFY(document.ExecuteEdit(plan.edit)); QByteArray bytes; QVERIFY(document.BuildRuntimeMoc(bytes, &error));
        CoreModel after; QVERIFY(after.load(bytes)); setPose(after, plan.targetPose);
        const auto target = positions(after, fixture.secondWitness); QVERIFY(target.size() >= 5);
        for (int vertex = 0; vertex < 4; ++vertex) QVERIFY(near(target[vertex], mirror(source[vertex])));
        QVERIFY(near(source[4], {.2, -.3}));
        QVERIFY(near(target[4], {-.4, -.5}));
        QVERIFY(!near(target[4], mirror(source[4])));
        QVERIFY(before.intact()); QVERIFY(after.intact());
    }
};

QTEST_GUILESS_MAIN(GeometryPairPlannerCoreTests)
#include "geometry_pair_planner_core_tests.moc"
