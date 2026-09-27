#include "geometry_pair_planner_fixture.h"
#include "geometry_object_pairs_document_fixture.h"
#include "live2dstruct/ckeyformeditcontroller.h"
#include <QtTest>
#include <cmath>
#include <stdexcept>

using namespace GeometryPairData;

namespace {
struct Preview {
    int calls = 0, accepted = 0;
    bool reject = false, throwing = false;
    QByteArray bytes;
    QMap<QString, float> pose;
    void connect(CKeyformEditController& controller) {
        controller.SetPublisher([this](const CModelDocument& document, const QMap<QString, float>& values, bool, QString* error) {
            ++calls;
            if (throwing) throw std::runtime_error("controlled pair publisher exception");
            if (reject) { if (error) *error = "controlled pair publisher rejection"; return false; }
            QByteArray candidate;
            if (!document.BuildRuntimeMoc(candidate, error)) return false;
            bytes = std::move(candidate); pose = values; ++accepted; return true;
        }, [](QString*) { return true; });
    }
};

Geometry frame(const CModelDocument& document, const Pairs::Object& object, int key)
{
    Geometry result; document.CaptureGeometryKeyform(object.type, object.id, key, result); return result;
}

CModelGeometryState geometry(const CModelDocument& document, const Pairs::Pair& pair)
{
    CModelGeometryState result;
    for (const auto* endpoint : {&pair.first, &pair.second})
        for (int key = 0; key < endpoint->keyformCount; ++key) result.append(frame(document, endpoint->object, key));
    return result;
}

QMap<QString, float> pose(const CModelDocument& document, const Pairs::Object& object, int key)
{
    QMap<QString, float> result, corner;
    for (const auto& parameter : document.Parameters()) result.insert(parameter.id, document.ParameterValue(parameter.id));
    if (!document.KeyformParameterValues(object.id, Pairs::TypeName(object.type), key, corner)) return {};
    for (auto it = corner.cbegin(); it != corner.cend(); ++it) result[it.key()] = it.value();
    return result;
}

bool world(const CModelDocument& document, const Geometry& value, QVector<float>& result, QString* error = nullptr)
{
    const auto values = pose(document, {value.type, value.id}, value.keyform);
    if (value.type == Geometry::ArtMesh) {
        CKeyformMappingContext mapping;
        return mapping.Build(document, value.id, value.keyform, values, error) && mapping.ToModel(std::get<QVector<float>>(value.value), result, error);
    }
    CDeformerEditContext mapping;
    if (!mapping.Build(document, value.id, value.TypeName(), value.keyform, values, error)) return false;
    if (value.type == Geometry::Rotation) { result = mapping.ModelPoints(); return true; }
    return mapping.ToModel(std::get<QVector<float>>(value.value), result, error);
}

CModelEdit legacy(const CModelDocument& document, const Pairs::Object& object, const QVector<int>& keys = {0}, bool opacity = false)
{
    CModelEdit edit; edit.name = object.id; edit.type = Pairs::TypeName(object.type); edit.index = keys.first(); edit.coalesce = false;
    if (object.type == Geometry::ArtMesh) {
        const auto before = document.DrawableKeyformPositions(object.id, keys.first()); auto after = before;
        after[0] += .013f; after[1] -= .017f;
        edit.field = CModelEdit::KeyformPositions; edit.before = QVariant::fromValue(before); edit.after = QVariant::fromValue(after);
    } else if (object.type == Geometry::Warp) {
        const auto before = document.DeformerWarpKeyforms(object.id); auto after = before;
        for (int key : keys) {
            if (opacity) after[key].opacity = before[key].opacity == .4f ? .7f : .4f;
            else { after[key].positions[0] += .013f; after[key].positions[1] -= .017f; }
        }
        edit.field = CModelEdit::WarpKeyforms; edit.before = QVariant::fromValue(before); edit.after = QVariant::fromValue(after);
    } else {
        const auto before = document.DeformerRotationKeyforms(object.id); auto after = before;
        for (int key : keys) {
            if (opacity) after[key].opacity = before[key].opacity == .4f ? .7f : .4f;
            else { after[key].originX += .013f; after[key].angle += 13; after[key].scale *= 1.1f; }
        }
        edit.field = CModelEdit::RotationKeyforms; edit.before = QVariant::fromValue(before); edit.after = QVariant::fromValue(after);
    }
    return edit;
}

bool begin(CKeyformEditController& controller, const CModelDocument& document, int handle, QString* error)
{
    if (handle == 0) {
        QVector<float> points;
        return world(document, frame(document, {Geometry::ArtMesh, controller.Target()}, controller.Keyform()), points, error) &&
            controller.BeginDrag({0}, points, error);
    }
    if (handle == 1) return controller.BeginDeformerDrag(CKeyformEditController::DeformerHandle::WarpPoints, {0}, error);
    return controller.BeginDeformerDrag(handle == 2 ? CKeyformEditController::DeformerHandle::RotationOrigin :
                                                    CKeyformEditController::DeformerHandle::RotationAngle, {}, error);
}

bool move(CKeyformEditController& controller, int handle, int sample, QString* error)
{
    const QPointF delta = sample == 0 ? QPointF{} : sample == 1 ? QPointF(.013, -.009) : QPointF(.028, .015);
    if (handle == 0) return controller.DragBy(delta, error);
    return controller.MoveDeformerDrag(handle == 3 ? QPointF{} : delta, handle == 3 ? (sample == 1 ? 13 : sample == 2 ? -9 : 0) : 0, error);
}

bool seedRedo(CModelDocument& document, const Pairs::Object& source)
{
    const auto before = frame(document, source, 0); const auto after = changed(before); CModelEdit edit;
    return document.MakeGeometryEdit({before}, {after}, before, edit) && document.ExecuteEdit(edit) && document.UndoEdit();
}
}

class GeometryPairControllerTests : public QObject
{
    Q_OBJECT
    QMap<int, CModelDocument> documents;
private slots:
    void initTestCase()
    {
        for (auto type : {Geometry::ArtMesh, Geometry::Warp, Geometry::Rotation}) {
            Fixture fixture; QString error; QVERIFY2(make(type, fixture, &error, true, true), qPrintable(error));
            CModelDocument document; QVERIFY2(document.LoadBytes(fixture.bytes, "paired-controller.moc3", &error), qPrintable(error));
            QVERIFY2(document.SetGeometryObjectPairs({document.SourceDigest(), {fixture.pair}}, &error), qPrintable(error));
            documents.insert(int(type), std::move(document));
        }
    }

    void legacySubmitWritesBothSidesWithOnePublication_data()
    {
        QTest::addColumn<int>("type"); QTest::addColumn<bool>("reverse");
        for (int type = 0; type < 3; ++type) for (bool reverse : {false, true})
            QTest::newRow(qPrintable(QString("%1-%2").arg(type).arg(reverse))) << type << reverse;
    }
    void legacySubmitWritesBothSidesWithOnePublication()
    {
        QFETCH(int, type); QFETCH(bool, reverse); auto document = documents[type]; const auto pair = document.GeometryObjectPairs().pairs[0];
        const auto source = reverse ? pair.second.object : pair.first.object;
        const auto target = reverse ? pair.first.object : pair.second.object;
        const int sourceKey = reverse ? 3 : 0, targetKey = reverse ? 0 : 3;
        const auto before = document; const auto initial = document.AuthoringStructureState();
        CKeyformEditController controller; controller.SetDocument(&document); Preview preview; preview.connect(controller); QString error;
        QVERIFY2(controller.Submit(legacy(document, source, {sourceKey}), {}, sourceKey, &error), qPrintable(error));
        QCOMPARE(preview.calls, 1); QCOMPARE(preview.accepted, 1);
        QCOMPARE(GeometryBatchData::historyCount(document), 1);
        CModelEdit history; QVERIFY(document.PeekUndoEdit(&history)); QCOMPARE(history.field, CModelEdit::GeometryKeyforms);
        const auto entries = history.after.value<CModelGeometryState>(); QCOMPARE(entries.size(), 2);
        QCOMPARE(entries[0].keyform, sourceKey); QCOMPARE(entries[1].keyform, targetKey);
        QCOMPARE(controller.Target(), source.id); QCOMPARE(controller.Keyform(), sourceKey);
        QCOMPARE(controller.Pose().value("PairPrimary"), reverse ? 1.0f : -1.0f); QCOMPARE(preview.pose, controller.Pose());
        QVector<float> sourceBefore, sourceAfter, targetBefore, targetAfter;
        QVERIFY(world(before, frame(before, source, sourceKey), sourceBefore));
        QVERIFY(world(document, frame(document, source, sourceKey), sourceAfter));
        QVERIFY(world(before, frame(before, target, targetKey), targetBefore));
        QVERIFY(world(document, frame(document, target, targetKey), targetAfter));
        for (int index = 0; index < sourceBefore.size(); ++index) {
            const double delta = double(sourceAfter[index]) - sourceBefore[index];
            const double mirrored = index % 2 ? delta : -delta;
            const double actual = double(targetAfter[index]) - targetBefore[index];
            QVERIFY2(std::abs(actual - mirrored) < 0.00003, qPrintable(QString("delta %1: %2 vs %3").arg(index).arg(actual, 0, 'g', 12).arg(mirrored, 0, 'g', 12)));
        }
        if (type == Geometry::Rotation) {
            const auto resolved = [&](const CModelDocument& model, const Pairs::Object& object, int key, CRotationFrameGeometry::Frame& value) {
                CDeformerEditContext mapping;
                return mapping.Build(model, object.id, "rotation", key, pose(model, object, key)) && mapping.CurrentRotationFrame(value);
            };
            CRotationFrameGeometry::Frame sourceA, sourceB, targetA, targetB;
            QVERIFY(resolved(before, source, sourceKey, sourceA)); QVERIFY(resolved(document, source, sourceKey, sourceB));
            QVERIFY(resolved(before, target, targetKey, targetA)); QVERIFY(resolved(document, target, targetKey, targetB));
            const auto angle = [](QPointF a, QPointF b) {
                return std::atan2(a.x() * b.y() - a.y() * b.x(), QPointF::dotProduct(a, b));
            };
            QVERIFY(std::abs(angle(sourceA.xAxis, sourceB.xAxis) + angle(targetA.xAxis, targetB.xAxis)) < .00001);
            QVERIFY(std::abs(targetB.scale / targetA.scale - sourceB.scale / sourceA.scale) < .00001);
        }
        for (int key = 0; key < pair.second.keyformCount; ++key)
            if (key != targetKey) QCOMPARE(frame(document, target, key), frame(before, target, key));
        const auto accepted = document.AuthoringStructureState();
        QVERIFY(controller.History(true, history, &error)); QCOMPARE(preview.calls, 2); QCOMPARE(document.AuthoringStructureState(), initial);
        QVERIFY(controller.History(false, history, &error)); QCOMPARE(preview.calls, 3); QCOMPARE(document.AuthoringStructureState(), accepted);
    }

    void multipleSourceKeyformsAreMergedIntoOneCommand_data()
    {
        QTest::addColumn<int>("type"); QTest::newRow("warp") << int(Geometry::Warp); QTest::newRow("rotation") << int(Geometry::Rotation);
    }
    void multipleSourceKeyformsAreMergedIntoOneCommand()
    {
        QFETCH(int, type); auto document = documents[type]; const auto before = document; const auto pair = document.GeometryObjectPairs().pairs[0];
        CKeyformEditController controller; controller.SetDocument(&document); Preview preview; preview.connect(controller); QString error;
        QVERIFY2(controller.Submit(legacy(document, pair.first.object, {0, 5}), {}, 0, &error), qPrintable(error));
        QCOMPARE(preview.calls, 1); CModelEdit command; QVERIFY(document.PeekUndoEdit(&command));
        QCOMPARE(command.field, CModelEdit::GeometryKeyforms); QCOMPARE(command.after.value<CModelGeometryState>().size(), 4);
        QVERIFY(frame(document, pair.second.object, 3) != frame(before, pair.second.object, 3));
        QVERIFY(frame(document, pair.second.object, 2) != frame(before, pair.second.object, 2));
        for (int key : {0, 1, 4, 5}) QCOMPARE(frame(document, pair.second.object, key), frame(before, pair.second.object, key));
        QCOMPARE(GeometryBatchData::historyCount(document), 1); QVERIFY(controller.History(true, command, &error));
        QCOMPARE(document.AuthoringStructureState(), before.AuthoringStructureState());
    }

    void dragUsesFrozenStartAndRestoresRedo_data()
    {
        QTest::addColumn<int>("handle");
        for (int handle = 0; handle < 4; ++handle) QTest::newRow(qPrintable(QString::number(handle))) << handle;
    }
    void dragUsesFrozenStartAndRestoresRedo()
    {
        QFETCH(int, handle); auto document = documents[std::min(handle, 2)]; const auto pair = document.GeometryObjectPairs().pairs[0];
        QVERIFY(seedRedo(document, pair.first.object)); const auto baseline = document.AuthoringStructureState();
        CModelEdit originalRedo; QVERIFY(document.PeekRedoEdit(&originalRedo));
        CKeyformEditController controller; controller.SetDocument(&document); Preview preview; preview.connect(controller); QString error;
        QVERIFY(controller.Activate(pair.first.object.id, Pairs::TypeName(pair.first.object.type), 0, {}, &error));
        QVERIFY2(begin(controller, document, handle, &error), qPrintable(error));
        QVERIFY2(move(controller, handle, 1, &error), qPrintable(error)); const auto first = document.AuthoringStructureState();
        QVERIFY2(move(controller, handle, 2, &error), qPrintable(error)); QVERIFY(document.AuthoringStructureState() != first);
        QVERIFY2(move(controller, handle, 1, &error), qPrintable(error)); QCOMPARE(document.AuthoringStructureState(), first);
        QCOMPARE(GeometryBatchData::historyCount(document), 1);
        QVERIFY2(move(controller, handle, 0, &error), qPrintable(error)); QCOMPARE(document.AuthoringStructureState(), baseline);
        QVERIFY(document.CanRedoEdit()); QVERIFY(!document.CanUndoEdit()); QVERIFY(!controller.DragHasChanges());
        CModelEdit redo; QVERIFY(document.PeekRedoEdit(&redo)); QCOMPARE(redo.before, originalRedo.before); QCOMPARE(redo.after, originalRedo.after);
        QVERIFY(controller.CancelDrag(&error)); QVERIFY(!controller.IsDragging());
        QVERIFY(begin(controller, document, handle, &error)); QVERIFY(move(controller, handle, 2, &error));
        QVERIFY(controller.CancelDrag(&error)); QCOMPARE(document.AuthoringStructureState(), baseline); QVERIFY(document.CanRedoEdit());
    }

    void laterKeyformMappingFailureDoesNotCommitEarlierPair_data() { multipleSourceKeyformsAreMergedIntoOneCommand_data(); }
    void laterKeyformMappingFailureDoesNotCommitEarlierPair()
    {
        QFETCH(int, type); auto document = documents[type]; const auto pair = document.GeometryObjectPairs().pairs[0]; QString error;
        CModelEdit bind;
        QVERIFY2(document.MakeBindParameterEdit("PairPrimary", {{{CMocParameterStructureBuilder::Owner::Rotation, "PairParentB"}, {-1, 1}}}, bind, &error), qPrintable(error));
        QVERIFY(document.ExecuteEdit(bind));
        auto parent = document.DeformerRotationKeyforms("PairParentB"); QCOMPARE(parent.size(), 2); parent[0].scale = 0;
        QVERIFY(document.SetDeformerRotationKeyforms("PairParentB", parent)); document.ClearEditHistory();
        auto firstOnly = document; CKeyformEditController firstController; firstController.SetDocument(&firstOnly);
        Preview firstPreview; firstPreview.connect(firstController);
        QVERIFY2(firstController.Submit(legacy(firstOnly, pair.first.object, {0}), {}, 0, &error), qPrintable(error));
        QCOMPARE(firstPreview.calls, 1);
        CKeyformEditController controller; controller.SetDocument(&document); Preview preview; preview.connect(controller);
        const auto baseline = document.AuthoringStructureState(); const auto revision = document.Revision();
        QVERIFY(!controller.Submit(legacy(document, pair.first.object, {0, 5}), {}, 0, &error)); QVERIFY(!error.isEmpty());
        QCOMPARE(preview.calls, 0); QCOMPARE(document.AuthoringStructureState(), baseline); QCOMPARE(document.Revision(), revision);
        QVERIFY(!document.CanUndoEdit()); QVERIFY(!controller.IsActive());
    }

    void failedPublisherLeavesBothSidesAndRedoIntact_data()
    {
        QTest::addColumn<int>("type"); QTest::addColumn<bool>("throwing");
        for (int type = 0; type < 3; ++type) for (bool throwing : {false, true})
            QTest::newRow(qPrintable(QString("%1-%2").arg(type).arg(throwing))) << type << throwing;
    }
    void failedPublisherLeavesBothSidesAndRedoIntact()
    {
        QFETCH(int, type); QFETCH(bool, throwing); auto document = documents[type]; const auto pair = document.GeometryObjectPairs().pairs[0];
        QVERIFY(seedRedo(document, pair.first.object)); CKeyformEditController controller; controller.SetDocument(&document);
        Preview preview; preview.connect(controller); QString error;
        QVERIFY(controller.Activate(pair.first.object.id, Pairs::TypeName(pair.first.object.type), 0, {}, &error));
        const auto baseline = document.AuthoringStructureState(); const auto revision = document.Revision(); const auto bytes = preview.bytes;
        preview.reject = !throwing; preview.throwing = throwing;
        QVERIFY(!controller.Submit(legacy(document, pair.first.object), {}, 0, &error)); QVERIFY(!error.isEmpty());
        QCOMPARE(preview.calls, 2); QCOMPARE(preview.bytes, bytes); QCOMPARE(document.AuthoringStructureState(), baseline);
        QCOMPARE(document.Revision(), revision); QVERIFY(!document.CanUndoEdit()); QVERIFY(document.CanRedoEdit());
        QCOMPARE(controller.Target(), pair.first.object.id); QCOMPARE(controller.Keyform(), 0);
    }

    void targetLockReviewAndDisabledPairHaveDistinctBehavior_data()
    {
        QTest::addColumn<int>("failure");
        QTest::newRow("target-lock") << 0; QTest::newRow("review") << 1; QTest::newRow("disabled") << 2;
    }
    void targetLockReviewAndDisabledPairHaveDistinctBehavior()
    {
        QFETCH(int, failure); auto document = documents[int(Geometry::ArtMesh)]; auto state = document.GeometryObjectPairs(); const auto pair = state.pairs[0];
        if (failure == 0) document.SetLocked("artmesh", pair.second.object.id, true);
        if (failure == 1) { state.pairs[0].requiresReview = true; QVERIFY(document.SetGeometryObjectPairs(state)); }
        if (failure == 2) { state.pairs[0].enabled = false; QVERIFY(document.SetGeometryObjectPairs(state)); }
        const auto baseline = document.AuthoringStructureState(); const auto target = frame(document, pair.second.object, 3);
        CKeyformEditController controller; controller.SetDocument(&document); Preview preview; preview.connect(controller); QString error;
        QCOMPARE(controller.Submit(legacy(document, pair.first.object), {}, 0, &error), failure == 2);
        QCOMPARE(frame(document, pair.second.object, 3), target);
        if (failure != 2) { QCOMPARE(preview.calls, 0); QCOMPARE(document.AuthoringStructureState(), baseline); }
        else { QCOMPARE(preview.calls, 1); CModelEdit edit; QVERIFY(document.PeekUndoEdit(&edit)); QCOMPARE(edit.field, CModelEdit::KeyformPositions); }
    }

    void opacityOnlyStaysLocalAndMixedEditsAreRejected_data() { multipleSourceKeyformsAreMergedIntoOneCommand_data(); }
    void opacityOnlyStaysLocalAndMixedEditsAreRejected()
    {
        QFETCH(int, type); auto document = documents[type]; const auto pair = document.GeometryObjectPairs().pairs[0];
        const auto original = document; CKeyformEditController controller; controller.SetDocument(&document); Preview preview; preview.connect(controller); QString error;
        QVERIFY2(controller.Submit(legacy(document, pair.first.object, {0}, true), {}, 0, &error), qPrintable(error));
        QCOMPARE(preview.calls, 1); QCOMPARE(geometry(document, pair), geometry(original, pair));
        if (type == Geometry::Warp) {
            QCOMPARE(document.DeformerWarpKeyforms(pair.second.object.id), original.DeformerWarpKeyforms(pair.second.object.id));
            QCOMPARE(document.DeformerWarpKeyforms(pair.first.object.id)[0].opacity, .4f);
        } else {
            QCOMPARE(document.DeformerRotationKeyforms(pair.second.object.id), original.DeformerRotationKeyforms(pair.second.object.id));
            QCOMPARE(document.DeformerRotationKeyforms(pair.first.object.id)[0].opacity, .4f);
        }
        auto mixed = legacy(document, pair.first.object);
        if (type == Geometry::Warp) { auto after = mixed.after.value<QVector<CModelWarpKeyform>>(); after[0].opacity = .8f; mixed.after = QVariant::fromValue(after); }
        else { auto after = mixed.after.value<QVector<CModelRotationKeyform>>(); after[0].opacity = .8f; mixed.after = QVariant::fromValue(after); }
        const auto baseline = document.AuthoringStructureState(); QVERIFY(!controller.Submit(mixed, {}, 0, &error));
        QVERIFY2(error.contains(QStringLiteral("透明度")), qPrintable(error)); QCOMPARE(preview.calls, 1); QCOMPARE(document.AuthoringStructureState(), baseline);
    }

    void failedDragDoesNotReplaceLastAcceptedSample_data() { dragUsesFrozenStartAndRestoresRedo_data(); }
    void failedDragDoesNotReplaceLastAcceptedSample()
    {
        QFETCH(int, handle); auto document = documents[std::min(handle, 2)]; const auto pair = document.GeometryObjectPairs().pairs[0];
        CKeyformEditController controller; controller.SetDocument(&document); Preview preview; preview.connect(controller); QString error;
        QVERIFY(controller.Activate(pair.first.object.id, Pairs::TypeName(pair.first.object.type), 0, {}, &error));
        QVERIFY(begin(controller, document, handle, &error)); QVERIFY(move(controller, handle, 1, &error));
        const auto accepted = document.AuthoringStructureState(); const auto bytes = preview.bytes;
        preview.reject = true; QVERIFY(!move(controller, handle, 2, &error));
        QCOMPARE(document.AuthoringStructureState(), accepted); QCOMPARE(preview.bytes, bytes); QVERIFY(controller.IsDragging());
        preview.reject = false; QVERIFY(move(controller, handle, 2, &error)); QVERIFY(document.AuthoringStructureState() != accepted);
        QVERIFY(controller.CancelDrag(&error)); QVERIFY(!document.CanUndoEdit());
    }

    void lockedObjectsCanBePreviewedWithoutBecomingEditable_data()
    {
        QTest::addColumn<int>("type"); QTest::addColumn<int>("scope");
        for (int type = 0; type < 3; ++type) for (int scope = 0; scope < 3; ++scope)
            QTest::newRow(qPrintable(QString("%1-%2").arg(type).arg(scope))) << type << scope;
    }
    void lockedObjectsCanBePreviewedWithoutBecomingEditable()
    {
        QFETCH(int, type); QFETCH(int, scope); auto document = documents[type]; const auto pair = document.GeometryObjectPairs().pairs[0];
        auto target = pair.first.object; QString lockType = Pairs::TypeName(target.type), lockId = target.id; QString error;
        if (scope == 1) {
            int parent = -1;
            for (int index = 0; index < document.Deformers().size(); ++index)
                if (document.Deformers()[index].id == "PairParentB") parent = index;
            QVERIFY(parent >= 0);
            QVERIFY(document.SetDeformerParentIndex(type == Geometry::ArtMesh ? QString("PairParentA") : target.id, parent));
            lockType = "rotation"; lockId = "PairParentB";
        } else if (scope == 2) {
            bool found = false;
            const auto select = [&](const Pairs::Object& object, int part) {
                if (part < 0 || part >= document.Parts().size()) return false;
                Geometry value; QVector<float> points;
                if (!document.CaptureGeometryKeyform(object.type, object.id, 0, value) || !world(document, value, points) || points.isEmpty()) return false;
                target = object; lockType = "part"; lockId = document.Parts()[part].id; return true;
            };
            if (type == Geometry::ArtMesh) {
                for (const auto& mesh : document.Drawables())
                    if (select({Geometry::ArtMesh, mesh.id}, mesh.parentPartIndex)) { found = true; break; }
            } else for (const auto& deformer : document.Deformers()) {
                const auto kind = deformer.type == CModelDeformer::Warp ? Geometry::Warp : Geometry::Rotation;
                if (int(kind) == type && select({kind, deformer.id}, deformer.parentPartIndex)) { found = true; break; }
            }
            QVERIFY(found);
        }
        QVector<float> runtime; QVERIFY2(world(document, frame(document, target, 0), runtime, &error), qPrintable(error));
        QVERIFY(seedRedo(document, target)); document.SetLocked(lockType, lockId, true);
        const auto baseline = document.AuthoringStructureState(); const auto revision = document.Revision();
        CModelEdit originalRedo; QVERIFY(document.PeekRedoEdit(&originalRedo)); QVERIFY(!document.CanUndoEdit());
        CKeyformEditController controller; controller.SetDocument(&document); ::Preview published; published.connect(controller);
        QVERIFY(!controller.Activate(target.id, Pairs::TypeName(target.type), 0, {}, &error));
        QVERIFY(error.contains(QStringLiteral("锁定"))); QCOMPARE(published.calls, 0); QVERIFY(!controller.IsActive());
        QVERIFY2(controller.Preview(target.id, Pairs::TypeName(target.type), 0, {}, &error), qPrintable(error));
        QCOMPARE(published.calls, 1); QCOMPARE(published.accepted, 1); QCOMPARE(controller.Target(), target.id); QCOMPARE(controller.Keyform(), 0);
        QCOMPARE(controller.Type(), Pairs::TypeName(target.type)); QCOMPARE(published.pose, pose(document, target, 0));
        const auto displayedPose = controller.Pose(); const auto displayedBytes = published.bytes;
        QVERIFY(!controller.Submit(legacy(document, target), {}, 0, &error)); QVERIFY(error.contains(QStringLiteral("锁定")));
        if (type == Geometry::ArtMesh) QVERIFY(!controller.BeginDrag({0}, runtime, &error));
        else QVERIFY(!controller.BeginDeformerDrag(type == Geometry::Warp ? CKeyformEditController::DeformerHandle::WarpPoints :
                         CKeyformEditController::DeformerHandle::RotationOrigin, type == Geometry::Warp ? QVector<int>{0} : QVector<int>{}, &error));
        QVERIFY(error.contains(QStringLiteral("锁定"))); QVERIFY(!controller.IsDragging()); QCOMPARE(published.calls, 1);
        QVERIFY(!controller.Activate(target.id, Pairs::TypeName(target.type), 0, {}, &error)); QCOMPARE(published.calls, 1);
        const auto other = target == pair.first.object ? pair.second.object : pair.first.object;
        QVERIFY(other.id != target.id);
        for (bool throwing : {false, true}) {
            published.reject = !throwing; published.throwing = throwing;
            QVERIFY(!controller.Preview(other.id, Pairs::TypeName(other.type), 0, {}, &error));
            QCOMPARE(controller.Target(), target.id); QCOMPARE(controller.Pose(), displayedPose); QCOMPARE(published.bytes, displayedBytes);
            CKeyformEditController inactive; inactive.SetDocument(&document); ::Preview failed; failed.reject = !throwing; failed.throwing = throwing; failed.connect(inactive);
            QVERIFY(!inactive.Preview(target.id, Pairs::TypeName(target.type), 0, {}, &error)); QVERIFY(!inactive.IsActive());
            QVERIFY(inactive.Target().isEmpty()); QCOMPARE(failed.calls, 1); QCOMPARE(failed.accepted, 0);
        }
        QCOMPARE(published.calls, 3); QCOMPARE(published.accepted, 1); QCOMPARE(document.AuthoringStructureState(), baseline);
        QCOMPARE(document.Revision(), revision); QVERIFY(!document.CanUndoEdit()); QVERIFY(document.CanRedoEdit());
        CModelEdit redo; QVERIFY(document.PeekRedoEdit(&redo)); QCOMPARE(redo.before, originalRedo.before); QCOMPARE(redo.after, originalRedo.after);
    }

    void sharedSpecificEqualWritesUseTheBatchPath()
    {
        CModelDocument document; QString error; QVERIFY(GeometryPairsData::load(document, true, &error));
        QVERIFY(document.SetGeometryObjectPairs(GeometryPairsData::paired(document, true)));
        CKeyformEditController controller; controller.SetDocument(&document); Preview preview; preview.connect(controller);
        auto before = document.DeformerRotationKeyforms("Root"), after = before; after[0].originY += .25f;
        CModelEdit edit{"Root", "rotation", CModelEdit::RotationKeyforms, QVariant::fromValue(before), QVariant::fromValue(after), 0, false};
        QVERIFY2(controller.Submit(edit, &error), qPrintable(error)); QCOMPARE(preview.calls, 1);
        QCOMPARE(document.DeformerRotationKeyforms("Root"), document.DeformerRotationKeyforms("Twin"));
        before = document.DeformerRotationKeyforms("Root"); after = before; after[0].originX += .25f;
        edit.before = QVariant::fromValue(before); edit.after = QVariant::fromValue(after);
        const auto baseline = document.AuthoringStructureState(); QVERIFY(!controller.Submit(edit, &error));
        QVERIFY2(error.contains(QStringLiteral("共享")), qPrintable(error)); QCOMPARE(preview.calls, 1); QCOMPARE(document.AuthoringStructureState(), baseline);
    }
};

QTEST_GUILESS_MAIN(GeometryPairControllerTests)
#include "geometry_pair_controller_tests.moc"
