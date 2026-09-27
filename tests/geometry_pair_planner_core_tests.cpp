#include "geometry_pair_planner_fixture.h"
#include "real_core_model_fixture.h"
#include "live2dstruct/cdeformereditcontext.h"
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
