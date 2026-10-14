#include "geometry_pair_planner_fixture.h"
#include "real_core_model_fixture.h"
#include "live2dstruct/csectionvalidation.h"
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
    void fullFramePairingMatchesIndependentCoreOracle_data()
    {
        QTest::addColumn<int>("type"); QTest::addColumn<bool>("reverse");
        for (int type : {int(Geometry::Warp), int(Geometry::Rotation)}) for (bool reverse : {false, true})
            QTest::newRow(qPrintable(QString("%1-%2").arg(type).arg(reverse))) << type << reverse;
    }
    void fullFramePairingMatchesIndependentCoreOracle()
    {
        QFETCH(int, type); QFETCH(bool, reverse);
        Fixture fixture; QString error;
        QVERIFY2(make(Geometry::Type(type), fixture, &error, true, true), qPrintable(error));
        CModelDocument document; QVERIFY2(document.LoadBytes(fixture.bytes, "full-frame-core.moc3", &error), qPrintable(error));
        QVERIFY2(document.SetGeometryObjectPairs({document.SourceDigest(), {fixture.pair}}, &error), qPrintable(error));
        const auto source = reverse ? fixture.pair.second : fixture.pair.first;
        const auto target = reverse ? fixture.pair.first : fixture.pair.second;
        const int sourceKey = reverse ? 3 : 0, targetKey = reverse ? 0 : 3;
        const QString sourceWitness = reverse ? fixture.secondWitness : fixture.firstWitness;
        const QString targetWitness = reverse ? fixture.firstWitness : fixture.secondWitness;
        const auto original = document;
        const auto originalState = document.AuthoringStructureState();
        auto requestedWarp = document.DeformerWarpKeyforms(source.object.id);
        auto requestedRotation = document.DeformerRotationKeyforms(source.object.id);
        const auto targetWarp = document.DeformerWarpKeyforms(target.object.id);
        const auto targetRotation = document.DeformerRotationKeyforms(target.object.id);
        CModelEdit edit{source.object.id, Pairs::TypeName(source.object.type), CModelEdit::WarpKeyforms, {}, {}, sourceKey, false};
        if (type == Geometry::Warp) {
            edit.before = QVariant::fromValue(requestedWarp);
            for (int index = 0; index < requestedWarp[sourceKey].positions.size(); ++index)
                requestedWarp[sourceKey].positions[index] += index % 2 ? -.017f : .013f;
            requestedWarp[sourceKey].opacity = .37f;
            edit.after = QVariant::fromValue(requestedWarp);
        } else {
            edit.field = CModelEdit::RotationKeyforms;
            edit.before = QVariant::fromValue(requestedRotation);
            requestedRotation[sourceKey].originX += .013f; requestedRotation[sourceKey].originY -= .017f;
            requestedRotation[sourceKey].opacity = .37f;
            edit.after = QVariant::fromValue(requestedRotation);
        }
        const auto patchFrame = [&](QByteArray& bytes, const QString& id, int key,
                                    const QVector<CModelWarpKeyform>& warps,
                                    const QVector<CModelRotationKeyform>& rotations, bool includeOpacity) {
            auto reader = CReader::FromBytes(bytes); CSectionTable table(reader);
            if (!table.IsValid()) return false;
            const auto& offsets = *table.sectionOffsetTable;
            const auto number = [&](int offset, float value) {
                quint32 bits; std::memcpy(&bits, &value, sizeof(bits)); qToLittleEndian(bits, bytes.data() + offset);
            };
            const auto integer = [&](int offset, bool value) { qToLittleEndian(qint32(value), bytes.data() + offset); };
            for (const auto* deformer : table.deformers) if (deformer->ids == id) {
                const int specific = deformer->specific_sources_indices;
                if (type == Geometry::Warp) {
                    if (key >= warps.size() || deformer->types != 0) return false;
                    const auto* owner = table.warp_deformers[specific];
                    if (key >= owner->keyform_sources_counts || warps[key].positions.size() != owner->vertex_counts * 2) return false;
                    DeformerReplayFixture::RawColumns raw(bytes);
                    const auto span = raw.spans.value("keyform_positions.xys");
                    if (span.slot < 0 || span.countIndex < 0) return false;
                    const int oldCount = raw.count("keyform_positions.xys");
                    QByteArray coordinates = raw.column("keyform_positions.xys");
                    for (float value : warps[key].positions) {
                        quint32 bits; std::memcpy(&bits, &value, sizeof(bits));
                        char encoded[4]; qToLittleEndian(bits, encoded); coordinates.append(encoded, 4);
                    }
                    raw.state.columns.insert(span.slot, coordinates);
                    raw.state.counts.insert(span.countIndex, coordinates.size() / 4);
                    const int row = owner->keyform_sources_begin_indices + key;
                    raw.set("warp_deformer_keyforms.keyform_position_sources_begin_indices", row, oldCount);
                    if (includeOpacity) raw.setFloat("warp_deformer_keyforms.opacities", row, warps[key].opacity);
                    QString rebuildError; bytes = raw.encode(&rebuildError);
                    return !bytes.isEmpty() && rebuildError.isEmpty();
                } else {
                    if (key >= rotations.size() || deformer->types != 1) return false;
                    const auto* owner = table.rotation_deformers[specific];
                    if (key >= owner->keyform_sources_counts) return false;
                    const int row = owner->keyform_sources_begin_indices + key; const auto& form = rotations[key];
                    const auto* fields = offsets.rotation_deformer_keyforms;
                    number(fields->angles + row * 4, form.angle);
                    number(fields->origin_x + row * 4, form.originX); number(fields->origin_y + row * 4, form.originY);
                    number(fields->scales + row * 4, form.scale);
                    integer(fields->is_reflect_x + row * 4, form.reflectX); integer(fields->is_reflect_y + row * 4, form.reflectY);
                    if (includeOpacity) number(fields->opacities + row * 4, form.opacity);
                }
                return true;
            }
            return false;
        };
        CKeyformEditController controller; controller.SetDocument(&document);
        QByteArray published; int publications = 0;
        controller.SetPublisher([&](const CModelDocument& next, const auto&, bool, QString* detail) {
            ++publications; return next.BuildRuntimeMoc(published, detail);
        }, [](QString*) { return true; });
        QVERIFY2(controller.Activate(source.object.id, edit.type, sourceKey, {}, &error), qPrintable(error));
        const auto sourcePose = controller.Pose();
        QMap<QString, float> targetPose = sourcePose, corner;
        QVERIFY(document.KeyformParameterValues(target.object.id, edit.type, targetKey, corner, &error));
        for (auto it = corner.cbegin(); it != corner.cend(); ++it) targetPose[it.key()] = it.value();
        CoreModel beforeSource, beforeTarget, requestedSource;
        QVERIFY(beforeSource.load(fixture.bytes)); setPose(beforeSource, sourcePose);
        QVERIFY(beforeTarget.load(fixture.bytes)); setPose(beforeTarget, targetPose);
        const auto sourceBefore = positions(beforeSource, sourceWitness), targetBefore = positions(beforeTarget, targetWitness);
        QVERIFY(sourceBefore.size() >= 4); QVERIFY(targetBefore.size() >= 4);
        QByteArray oracle = fixture.bytes;
        QVERIFY(patchFrame(oracle, source.object.id, sourceKey, requestedWarp, requestedRotation, true));
        QVERIFY(requestedSource.load(oracle)); setPose(requestedSource, sourcePose);
        const auto sourceExpected = positions(requestedSource, sourceWitness);
        const QPointF desiredDelta = mirror(sourceExpected[0] - sourceBefore[0]);
        QVERIFY(std::hypot(desiredDelta.x(), desiredDelta.y()) > .001);
        QVector<QPointF> parentAxes;
        for (int axis = 0; axis < 2; ++axis) {
            auto warpProbe = targetWarp; auto rotationProbe = targetRotation;
            if (type == Geometry::Warp) {
                for (int index = axis; index < warpProbe[targetKey].positions.size(); index += 2)
                    warpProbe[targetKey].positions[index] += 1;
            } else if (axis == 0) rotationProbe[targetKey].originX += 1;
            else rotationProbe[targetKey].originY += 1;
            QByteArray probe = fixture.bytes;
            QVERIFY(patchFrame(probe, target.object.id, targetKey, warpProbe, rotationProbe, false));
            CoreModel core; QVERIFY(core.load(probe)); setPose(core, targetPose);
            const auto evaluated = positions(core, targetWitness); QCOMPARE(evaluated.size(), targetBefore.size());
            parentAxes.append(evaluated[0] - targetBefore[0]); QVERIFY(core.intact());
        }
        const auto cross = [](QPointF a, QPointF b) { return a.x() * b.y() - a.y() * b.x(); };
        const double determinant = cross(parentAxes[0], parentAxes[1]); QVERIFY(std::abs(determinant) > .001);
        const QPointF localDelta(cross(desiredDelta, parentAxes[1]) / determinant,
                                 cross(parentAxes[0], desiredDelta) / determinant);
        auto expectedWarp = targetWarp; auto expectedRotation = targetRotation;
        if (type == Geometry::Warp) {
            for (int index = 0; index < expectedWarp[targetKey].positions.size(); ++index)
                expectedWarp[targetKey].positions[index] += float(index % 2 ? localDelta.y() : localDelta.x());
        } else {
            expectedRotation[targetKey].originX += float(localDelta.x());
            expectedRotation[targetKey].originY += float(localDelta.y());
        }
        QVERIFY(patchFrame(oracle, target.object.id, targetKey, expectedWarp, expectedRotation, false));
        const int beforePublications = publications;
        QVERIFY2(controller.Submit(edit, sourcePose, sourceKey, &error), qPrintable(error));
        QCOMPARE(publications, beforePublications + 1);
        if (type == Geometry::Warp) {
            QCOMPARE(document.DeformerWarpKeyforms(source.object.id), requestedWarp);
            const auto actual = document.DeformerWarpKeyforms(target.object.id);
            for (int key = 0; key < actual.size(); ++key) QCOMPARE(actual[key].opacity, targetWarp[key].opacity);
        } else {
            QCOMPARE(document.DeformerRotationKeyforms(source.object.id), requestedRotation);
            const auto actual = document.DeformerRotationKeyforms(target.object.id);
            for (int key = 0; key < actual.size(); ++key) QCOMPARE(actual[key].opacity, targetRotation[key].opacity);
        }
        CoreModel changedSource, changedTarget;
        QVERIFY(changedSource.load(published)); setPose(changedSource, sourcePose);
        QVERIFY(changedTarget.load(published)); setPose(changedTarget, targetPose);
        const auto targetAfter = positions(changedTarget, targetWitness);
        for (int index = 0; index < targetBefore.size(); ++index)
            QVERIFY(near(targetAfter[index], targetBefore[index] + desiredDelta));
        const auto opacity = [](CoreModel& core, const QString& id) {
            for (int index = 0; index < csmGetDrawableCount(core.value); ++index)
                if (QString::fromUtf8(csmGetDrawableIds(core.value)[index]) == id) return csmGetDrawableOpacities(core.value)[index];
            return -1.0f;
        };
        QVERIFY(opacity(beforeSource, sourceWitness) > 0);
        const float priorOpacity = type == Geometry::Warp ? original.DeformerWarpKeyforms(source.object.id)[sourceKey].opacity
                                                        : original.DeformerRotationKeyforms(source.object.id)[sourceKey].opacity;
        QVERIFY(priorOpacity > 0);
        QVERIFY(std::abs(opacity(changedSource, sourceWitness) - opacity(beforeSource, sourceWitness) * (.37f / priorOpacity)) < .00001f);
        QCOMPARE(opacity(changedTarget, targetWitness), opacity(beforeTarget, targetWitness));
        const auto parity = [&](const QByteArray& actualBytes, const QByteArray& expectedBytes) {
            CoreModel actual, expected;
            if (!actual.load(actualBytes) || !expected.load(expectedBytes)) return false;
            QVector<QMap<QString, float>> checks{sourcePose, targetPose};
            for (const auto& endpoint : {source, target}) for (int key = 0; key < endpoint.keyformCount; ++key) {
                QMap<QString, float> keys, values = sourcePose;
                if (!original.KeyformParameterValues(endpoint.object.id, edit.type, key, keys, &error)) return false;
                for (auto it = keys.cbegin(); it != keys.cend(); ++it) values[it.key()] = it.value();
                checks.append(values);
            }
            auto middle = sourcePose; middle["PairPrimary"] = 0; middle["PairShared"] = .25f; checks.append(middle);
            for (const auto& values : checks) {
                setPose(actual, values); setPose(expected, values);
                error = difference(frame(actual.value), frame(expected.value));
                if (!::changed(frame(actual.value), frame(expected.value)).isEmpty()) return false;
            }
            return actual.intact() && expected.intact();
        };
        QVERIFY2(parity(published, oracle), qPrintable(error));
        CModelEdit history;
        QVERIFY2(controller.History(true, history, &error), qPrintable(error));
        QCOMPARE(document.AuthoringStructureState(), originalState); QVERIFY(!document.CanUndoEdit()); QVERIFY(document.CanRedoEdit());
        QVERIFY2(parity(published, fixture.bytes), qPrintable(error));
        QVERIFY2(controller.History(false, history, &error), qPrintable(error));
        QVERIFY2(parity(published, oracle), qPrintable(error));
        QVERIFY(beforeSource.intact()); QVERIFY(beforeTarget.intact()); QVERIFY(requestedSource.intact());
        QVERIFY(changedSource.intact()); QVERIFY(changedTarget.intact());
    }


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
