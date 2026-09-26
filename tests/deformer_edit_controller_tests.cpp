#include "live2dstruct/ckeyformeditcontroller.h"
#include "live2dstruct/cmodelsession.h"
#include "deformer_snapshot_fixture.h"
#include <QtTest>
#include <functional>
#include <limits>
#include <stdexcept>

using namespace live2dstruct;
using namespace DeformerSnapshotData;
using Handle = CKeyformEditController::DeformerHandle;

namespace {
struct Preview
{
    bool reject = false, throwing = false;
    int attempts = 0, accepted = 0;
    QByteArray bytes;
    QMap<QString, float> pose;
    std::function<void()> callback;
    void Connect(CKeyformEditController& controller)
    {
        controller.SetPublisher([this](const CModelDocument& candidate, const QMap<QString, float>& values,
                                      bool editing, QString* error) {
            ++attempts;
            if (callback) callback();
            if (throwing) throw std::runtime_error("publisher failure");
            if (reject || !editing) { if (error) *error = QStringLiteral("预览拒绝候选"); return false; }
            QByteArray prepared;
            if (!candidate.BuildPreviewMoc(prepared, error)) return false;
            bytes.swap(prepared); pose = values; ++accepted;
            return true;
        }, [](QString*) { return true; });
    }
};

int historyCount(CModelDocument document)
{
    int result = 0;
    while (document.UndoEdit()) ++result;
    return result;
}

QByteArray content(const CModelDocument& document)
{
    CModelSession session;
    session.sourceMoc = document.SourcePath(); session.sourceDigest = document.SourceDigest();
    session.parameters = pose(document);
    session.deformerWarpKeyforms = document.DeformerWarpKeyformOverrides();
    session.deformerRotationKeyforms = document.DeformerRotationKeyformOverrides();
    session.drawableKeyformPositions = document.DrawableKeyformPositionOverrides();
    session.drawableOpacity = document.DrawableOpacityOverrides();
    session.deformerParentIndices = document.DeformerParentOverrides();
    session.visibility = document.VisibilityState();
    for (const auto& key : document.LockedKeys()) session.locked[key] = true;
    return session.ToJson();
}

struct Snapshot
{
    QByteArray content, preview;
    quint64 revision = 0;
    int history = 0;
    bool undo = false, redo = false;
    CModelEdit undoEdit, redoEdit;
};
Snapshot snapshot(const CModelDocument& document)
{
    Snapshot result;
    result.content = content(document); document.BuildPreviewMoc(result.preview);
    result.revision = document.Revision(); result.history = historyCount(document);
    result.undo = document.PeekUndoEdit(&result.undoEdit); result.redo = document.PeekRedoEdit(&result.redoEdit);
    return result;
}
bool sameCommand(const CModelEdit& a, const CModelEdit& b)
{
    return a.name == b.name && a.type == b.type && a.field == b.field && a.index == b.index &&
        a.before == b.before && a.after == b.after && a.coalesce == b.coalesce;
}

void bindWarp(Fixture& fixture)
{
    fixture.BindRoot(false);
    const auto& o = *fixture.table.sectionOffsetTable;
    fixture.column(o.deformers->keyform_binding_sources_indices, 0, -1);
    fixture.column(o.deformers->keyform_binding_sources_indices, 1, 0);
    fixture.column(o.rotation_deformers->keyform_sources_counts, 0, 1);
    fixture.column(o.warp_deformers->keyform_sources_counts, 0, 2);
    fixture.count(7, 2);
    real(fixture.bytes, o.warp_deformer_keyforms->opacities + 4, 1);
    fixture.column(o.warp_deformer_keyforms->keyform_position_sources_begin_indices, 1, 12);
}

bool seedHistory(CModelDocument& document)
{
    for (float angle : {95.0f, 100.0f}) {
        auto before = document.DeformerRotationKeyforms("Root"), after = before;
        after[0].angle = angle;
        if (!document.ExecuteEdit({"Root", "rotation", CModelEdit::RotationKeyforms,
            QVariant::fromValue(before), QVariant::fromValue(after), 0, false})) return false;
    }
    return document.UndoEdit();
}
}

class DeformerEditControllerTests : public QObject
{
    Q_OBJECT
    void unchanged(const CModelDocument& document, const Snapshot& state, bool checkRevision = true)
    {
        QCOMPARE(content(document), state.content);
        QByteArray bytes; QVERIFY(document.BuildPreviewMoc(bytes)); QCOMPARE(bytes, state.preview);
        if (checkRevision) QCOMPARE(document.Revision(), state.revision);
        QCOMPARE(historyCount(document), state.history);
        CModelEdit command;
        QCOMPARE(document.PeekUndoEdit(&command), state.undo);
        if (state.undo) QVERIFY(sameCommand(command, state.undoEdit));
        QCOMPARE(document.PeekRedoEdit(&command), state.redo);
        if (state.redo) QVERIFY(sameCommand(command, state.redoEdit));
    }
    void activate(CModelDocument& document, CKeyformEditController& controller, Preview& preview,
                  const QString& id, const QString& type, int keyform = 0)
    {
        controller.SetDocument(&document); preview.Connect(controller);
        QString error;
        QVERIFY2(controller.Activate(id, type, keyform, pose(document), &error), qPrintable(error));
        QCOMPARE(controller.Target(), id); QCOMPARE(controller.Type(), type); QCOMPARE(controller.Keyform(), keyform);
        QVERIFY(!controller.DragHasChanges());
    }
    void begin(CKeyformEditController& controller, int kind)
    {
        QString error;
        QVERIFY2(controller.BeginDeformerDrag(kind == 0 ? Handle::WarpPoints : kind == 1 ? Handle::RotationOrigin : Handle::RotationAngle,
                    kind == 0 ? QVector<int>{1, 3} : QVector<int>{}, &error), qPrintable(error));
        QVERIFY(!controller.DragHasChanges());
    }
    bool move(CKeyformEditController& controller, int kind, double amount, QString* error = nullptr)
    {
        return controller.MoveDeformerDrag(kind == 2 ? QPointF() : QPointF(amount, -amount), kind == 2 ? amount : 0, error);
    }
private slots:
    void meshChangeQueryUsesTheSameNetGeometryContract()
    {
        Fixture fixture; CModelDocument document; QVERIFY(document.LoadBytes(fixture.bytes, "mesh-query"));
        CKeyformEditController controller; QVERIFY(!controller.DragHasChanges()); Preview preview;
        activate(document, controller, preview, "Mesh", "artmesh");
        CKeyformMappingContext mapping; QString error;
        QVERIFY(mapping.Build(document, "Mesh", 0, controller.Pose(), &error));
        QVector<float> world;
        QVERIFY(mapping.ToModel(document.DrawableKeyformPositions("Mesh", 0), world));
        QVERIFY(controller.BeginDrag({0}, world)); QVERIFY(!controller.DragHasChanges());
        const auto initial = snapshot(document);
        QVERIFY(controller.DragBy({})); QVERIFY(!controller.DragHasChanges()); unchanged(document, initial);
        preview.reject = true; QVERIFY(!controller.DragBy({0.01, 0.02})); QVERIFY(!controller.DragHasChanges());
        preview.reject = false; QVERIFY(controller.DragBy({0.01, 0.02})); QVERIFY(controller.DragHasChanges());
        preview.reject = true; QVERIFY(!controller.DragBy({0.02, 0.03})); QVERIFY(controller.DragHasChanges());
        QVERIFY(!controller.CancelDrag()); QVERIFY(controller.DragHasChanges());
        preview.reject = false; QVERIFY(controller.DragBy({})); QVERIFY(!controller.DragHasChanges());
        unchanged(document, initial, false);
        QVERIFY(controller.DragBy({0.02, 0.03})); QVERIFY(controller.DragHasChanges());
        QVERIFY(controller.CancelDrag()); QVERIFY(!controller.DragHasChanges()); unchanged(document, initial, false);
        QVERIFY(controller.BeginDrag({0}, world)); QVERIFY(controller.DragBy({0.01, 0.02})); QVERIFY(controller.DragHasChanges());
        controller.FinishDrag(); QVERIFY(!controller.DragHasChanges());
        controller.SetDocument(nullptr); QVERIFY(!controller.DragHasChanges());
    }
    void selectedWarpPointsUseAbsoluteDisplacementAndOneHistoryEntry()
    {
        Fixture fixture; bindWarp(fixture);
        CModelDocument document; QVERIFY(document.LoadBytes(fixture.bytes, "warp"));
        CKeyformEditController controller; Preview preview; activate(document, controller, preview, "Warp", "warp", 1);
        const auto original = document.DeformerWarpKeyforms("Warp"); const auto originalPose = controller.Pose();
        const auto root = document.DeformerRotationKeyforms("Root"); const auto mesh = document.DrawableKeyformPositions("Mesh", 0);
        QVERIFY(controller.BeginDeformerDrag(Handle::WarpPoints, {3, 1}));
        QVERIFY(controller.MoveDeformerDrag({0.1, -0.2}, 0));
        QVERIFY(controller.MoveDeformerDrag({0.3, 0.4}, 0));
        QVERIFY(controller.DragHasChanges());
        const auto current = document.DeformerWarpKeyforms("Warp");
        QCOMPARE(current[0], original[0]);
        for (int index = 0; index < 4; ++index) {
            const bool selected = index == 1 || index == 3;
            const double x = original[1].positions[index * 2] - (selected ? 0.4 : 0);
            const double y = original[1].positions[index * 2 + 1] - (selected ? 0.3 : 0);
            QVERIFY(std::abs(current[1].positions[index * 2] - x) < 0.00002);
            QVERIFY(std::abs(current[1].positions[index * 2 + 1] - y) < 0.00002);
        }
        QCOMPARE(document.DeformerRotationKeyforms("Root"), root); QCOMPARE(document.DrawableKeyformPositions("Mesh", 0), mesh);
        QCOMPARE(controller.Pose(), originalPose); QCOMPARE(preview.pose, originalPose); QCOMPARE(historyCount(document), 1);
        const auto revision = document.Revision(); const int attempts = preview.attempts;
        QVERIFY(controller.MoveDeformerDrag({0.3, 0.4}, 0)); QCOMPARE(document.Revision(), revision); QCOMPARE(preview.attempts, attempts);
        controller.FinishDrag(); QVERIFY(!controller.DragHasChanges()); CModelEdit undo;
        QVERIFY(controller.History(true, undo)); QCOMPARE(document.DeformerWarpKeyforms("Warp"), original);
        QVERIFY(!document.CanUndoEdit()); QVERIFY(controller.History(false, undo)); QCOMPARE(document.DeformerWarpKeyforms("Warp"), current);
    }
    void rotationOriginAndAngleOnlyChangeTheSelectedForm_data()
    {
        QTest::addColumn<bool>("angle"); QTest::newRow("origin") << false; QTest::newRow("angle") << true;
    }
    void rotationOriginAndAngleOnlyChangeTheSelectedForm()
    {
        QFETCH(bool, angle);
        Fixture fixture; fixture.BindRoot(false);
        CModelDocument document; QVERIFY(document.LoadBytes(fixture.bytes, "rotation"));
        CKeyformEditController controller; Preview preview; activate(document, controller, preview, "Root", "rotation", 1);
        const auto before = document.DeformerRotationKeyforms("Root"); const auto frozen = controller.Pose();
        QVERIFY(controller.BeginDeformerDrag(angle ? Handle::RotationAngle : Handle::RotationOrigin, {}));
        QVERIFY(controller.MoveDeformerDrag(angle ? QPointF() : QPointF(0.1, -0.2), angle ? 350 : 0));
        QVERIFY(controller.MoveDeformerDrag(angle ? QPointF() : QPointF(0.3, 0.4), angle ? 380 : 0));
        auto expected = before;
        if (angle) expected[1].angle += 380;
        else { expected[1].originX += 0.3f; expected[1].originY -= 0.4f; }
        QCOMPARE(document.DeformerRotationKeyforms("Root"), expected);
        QVERIFY(controller.DragHasChanges());
        QCOMPARE(controller.Pose(), frozen); QCOMPARE(preview.pose, frozen); QCOMPARE(historyCount(document), 1);
        controller.FinishDrag(); QVERIFY(!controller.DragHasChanges()); CModelEdit edit; QVERIFY(controller.History(true, edit));
        QCOMPARE(document.DeformerRotationKeyforms("Root"), before);
        QVERIFY(controller.History(false, edit)); QCOMPARE(document.DeformerRotationKeyforms("Root"), expected);
    }
    void publisherRejectionThrowAndCancellationKeepTheLastAcceptedState_data()
    {
        QTest::addColumn<int>("kind");
        QTest::newRow("warp") << 0; QTest::newRow("origin") << 1; QTest::newRow("angle") << 2;
    }
    void publisherRejectionThrowAndCancellationKeepTheLastAcceptedState()
    {
        QFETCH(int, kind);
        Fixture fixture; CModelDocument document; QVERIFY(document.LoadBytes(fixture.bytes, "reject"));
        CKeyformEditController controller; Preview preview;
        activate(document, controller, preview, kind == 0 ? "Warp" : "Child", kind == 0 ? "warp" : "rotation");
        const auto before = snapshot(document); const auto frozen = controller.Pose(); begin(controller, kind);
        QVERIFY(move(controller, kind, 0.02)); QVERIFY(controller.DragHasChanges()); const auto accepted = snapshot(document); const auto published = preview.bytes;
        QString error; preview.reject = true;
        QVERIFY(!move(controller, kind, 0.03, &error)); QVERIFY(!error.isEmpty()); QVERIFY(controller.DragHasChanges()); unchanged(document, accepted); QCOMPARE(preview.bytes, published);
        QVERIFY(!controller.CancelDrag(&error)); QVERIFY(controller.IsDragging()); QVERIFY(controller.DragHasChanges()); unchanged(document, accepted);
        preview.reject = false; preview.throwing = true;
        QVERIFY(!move(controller, kind, 0.04, &error)); unchanged(document, accepted);
        QVERIFY(!controller.CancelDrag(&error)); QVERIFY(controller.IsDragging()); QVERIFY(controller.DragHasChanges()); unchanged(document, accepted);
        preview.throwing = false; QVERIFY2(move(controller, kind, 0.05, &error), qPrintable(error));
        QCOMPARE(historyCount(document), 1); QCOMPARE(controller.Pose(), frozen);
        QVERIFY(controller.CancelDrag(&error)); QVERIFY(!controller.IsDragging()); QVERIFY(!controller.DragHasChanges()); unchanged(document, before, false);
        QCOMPARE(preview.bytes, before.preview); QCOMPARE(preview.pose, frozen);
        begin(controller, kind); QVERIFY(move(controller, kind, 0.01)); controller.FinishDrag(); QCOMPARE(historyCount(document), 1);
    }
    void cancellationAndReturnToStartRestoreTheRedoBranch_data()
    {
        QTest::addColumn<int>("kind"); QTest::addColumn<bool>("cancel");
        for (int kind : {0, 1, 2}) for (bool cancel : {false, true})
            QTest::newRow(qPrintable(QString("%1-%2").arg(kind).arg(cancel))) << kind << cancel;
    }
    void cancellationAndReturnToStartRestoreTheRedoBranch()
    {
        QFETCH(int, kind); QFETCH(bool, cancel);
        Fixture fixture; CModelDocument document; QVERIFY(document.LoadBytes(fixture.bytes, "history")); QVERIFY(seedHistory(document));
        CKeyformEditController controller; Preview preview;
        activate(document, controller, preview, kind == 0 ? "Warp" : kind == 2 ? "Root" : "Child", kind == 0 ? "warp" : "rotation");
        const auto before = snapshot(document); QVERIFY(before.redo); begin(controller, kind);
        QVERIFY(move(controller, kind, 0.02)); QVERIFY(!document.CanRedoEdit()); QCOMPARE(historyCount(document), before.history + 1);
        if (cancel) QVERIFY(controller.CancelDrag());
        else { QVERIFY(move(controller, kind, 0)); QVERIFY(controller.IsDragging()); QVERIFY(!controller.DragHasChanges()); controller.FinishDrag(); }
        QVERIFY(!controller.DragHasChanges());
        unchanged(document, before, false);
        CModelEdit command; QVERIFY(controller.History(false, command)); QVERIFY(sameCommand(command, before.redoEdit));
    }
    void staleRevisionAndSourceNeverOverwriteLaterState_data()
    {
        QTest::addColumn<int>("kind"); QTest::addColumn<QString>("mutation");
        for (int kind : {0, 1, 2}) for (const char* mutation : {"parameter", "ancestor", "locked", "reload"})
            QTest::newRow(qPrintable(QString("%1-%2").arg(kind).arg(mutation))) << kind << QString::fromLatin1(mutation);
    }
    void staleRevisionAndSourceNeverOverwriteLaterState()
    {
        QFETCH(int, kind); QFETCH(QString, mutation);
        Fixture fixture; CModelDocument document; QVERIFY(document.LoadBytes(fixture.bytes, "stale"));
        CKeyformEditController controller; Preview preview;
        activate(document, controller, preview, kind == 0 ? "Warp" : "Child", kind == 0 ? "warp" : "rotation");
        begin(controller, kind); QVERIFY(move(controller, kind, 0.02));
        if (mutation == "parameter") QVERIFY(document.SetParameterValue("P", 0.5f));
        if (mutation == "ancestor") { auto forms = document.DeformerRotationKeyforms("Root"); forms[0].angle += 10; QVERIFY(document.SetDeformerRotationKeyforms("Root", forms)); }
        if (mutation == "locked") document.SetLocked("rotation", "Root", true);
        if (mutation == "reload") {
            real(fixture.bytes, fixture.table.sectionOffsetTable->rotation_deformer_keyforms->angles, 45);
            QVERIFY(document.LoadBytes(fixture.bytes, "replacement"));
        }
        QCOMPARE(controller.DragHasChanges(), mutation != "reload");
        const auto changed = snapshot(document); const auto frozen = controller.Pose(); const int attempts = preview.attempts;
        QString error; QVERIFY(!move(controller, kind, 0.03, &error)); QVERIFY(!error.isEmpty());
        QVERIFY(!controller.CancelDrag(&error)); QCOMPARE(preview.attempts, attempts); unchanged(document, changed);
        QCOMPARE(controller.Pose(), frozen); controller.FinishDrag(); QVERIFY(!controller.IsDragging());
    }
    void activeGestureAndPublicationRejectReentrantOperations()
    {
        Fixture fixture; CModelDocument document, other;
        QVERIFY(document.LoadBytes(fixture.bytes, "guard")); QVERIFY(other.LoadBytes(fixture.bytes, "other"));
        CKeyformEditController controller; Preview preview; activate(document, controller, preview, "Warp", "warp");
        begin(controller, 0); const auto frozen = controller.Pose(); const int attempts = preview.attempts;
        CModelEdit command;
        QVERIFY(!controller.Activate("Child", "rotation", 0, {{"P", 1}})); QVERIFY(!controller.End());
        QVERIFY(!controller.History(true, command)); QVERIFY(!controller.BeginDeformerDrag(Handle::WarpPoints, {0}));
        QCOMPARE(preview.attempts, attempts); QCOMPARE(controller.Pose(), frozen);
        bool visited = false;
        preview.callback = [&] {
            visited = true; QVERIFY(!controller.CancelDrag()); QVERIFY(!move(controller, 0, 0.5));
            QVERIFY(!controller.BeginDeformerDrag(Handle::WarpPoints, {0}));
            QVERIFY(!controller.Activate("Child", "rotation", 0, {{"P", 1}}));
            controller.FinishDrag(); controller.SetDocument(&other);
            QVERIFY(controller.IsDragging()); QCOMPARE(controller.Target(), QString("Warp"));
        };
        QVERIFY(move(controller, 0, 0.02)); QVERIFY(visited); QCOMPARE(historyCount(document), 1); QCOMPARE(historyCount(other), 0);
        preview.callback = {}; QVERIFY(controller.CancelDrag()); QCOMPARE(historyCount(document), 0);
    }
    void distinctGesturesRemainDistinctUndoEntries_data()
    {
        QTest::addColumn<int>("kind");
        QTest::newRow("warp") << 0; QTest::newRow("origin") << 1; QTest::newRow("angle") << 2;
    }
    void distinctGesturesRemainDistinctUndoEntries()
    {
        QFETCH(int, kind);
        Fixture fixture; CModelDocument document; QVERIFY(document.LoadBytes(fixture.bytes, "two-gestures"));
        CKeyformEditController controller; Preview preview;
        activate(document, controller, preview, kind == 0 ? "Warp" : "Child", kind == 0 ? "warp" : "rotation");
        const auto initial = content(document);
        begin(controller, kind); QVERIFY(move(controller, kind, 0.02)); controller.FinishDrag();
        const auto first = content(document);
        begin(controller, kind); QVERIFY(move(controller, kind, 0.03)); controller.FinishDrag();
        const auto second = content(document); QVERIFY(second != first); QCOMPARE(historyCount(document), 2);
        CModelEdit edit;
        QVERIFY(controller.History(true, edit)); QCOMPARE(content(document), first);
        QVERIFY(controller.History(true, edit)); QCOMPARE(content(document), initial);
        QVERIFY(controller.History(false, edit)); QCOMPARE(content(document), first);
        QVERIFY(controller.History(false, edit)); QCOMPARE(content(document), second);
    }
    void ancestorPoseOutsideItsKeysCannotBeginDrag()
    {
        Fixture fixture; fixture.BindRoot(false);
        const auto& o = *fixture.table.sectionOffsetTable;
        real(fixture.bytes, o.keys->values, 0.25f); real(fixture.bytes, o.keys->values + 4, 0.75f);
        CModelDocument document; QVERIFY(document.LoadBytes(fixture.bytes, "ancestor-coverage"));
        CKeyformEditController controller; Preview preview;
        activate(document, controller, preview, "Child", "rotation");
        const auto before = snapshot(document); const int attempts = preview.attempts;
        QString error; QVERIFY(!controller.BeginDeformerDrag(Handle::RotationOrigin, {}, &error));
        QVERIFY(error.contains(QStringLiteral("覆盖"))); QVERIFY(!controller.IsDragging());
        unchanged(document, before); QCOMPARE(preview.attempts, attempts);
        QVERIFY(controller.Activate("Child", "rotation", 0, {{"P", 0.5f}}, &error));
        QVERIFY2(controller.BeginDeformerDrag(Handle::RotationOrigin, {}, &error), qPrintable(error));
        QVERIFY(controller.MoveDeformerDrag({0.01, 0.02}, 0, &error));
        QCOMPARE(controller.Pose().value("P"), 0.5f); QVERIFY(controller.CancelDrag());
    }
    void publicationRevisionConflictPreservesTheInterveningDocumentEdit()
    {
        Fixture fixture; CModelDocument document; QVERIFY(document.LoadBytes(fixture.bytes, "publication-conflict"));
        CKeyformEditController controller; Preview preview; activate(document, controller, preview, "Warp", "warp");
        begin(controller, 0); const auto original = document.DeformerWarpKeyforms("Warp");
        preview.callback = [&] { QVERIFY(document.SetParameterValue("P", 0.5f)); };
        QString error; QVERIFY(!move(controller, 0, 0.02, &error));
        QVERIFY(error.contains(QStringLiteral("文档已改变"))); QCOMPARE(document.DeformerWarpKeyforms("Warp"), original);
        QCOMPARE(document.ParameterValue("P"), 0.5f); QCOMPARE(historyCount(document), 0);
        preview.callback = {}; QVERIFY(!controller.CancelDrag(&error)); controller.FinishDrag();
    }
    void invalidSelectionAndHandleDoNotStartDrag_data()
    {
        QTest::addColumn<QString>("type"); QTest::addColumn<int>("handle"); QTest::addColumn<QVector<int>>("selected");
        QTest::newRow("empty") << QString("warp") << 0 << QVector<int>{};
        QTest::newRow("duplicate") << QString("warp") << 0 << QVector<int>{1, 1};
        QTest::newRow("negative") << QString("warp") << 0 << QVector<int>{-1};
        QTest::newRow("outside") << QString("warp") << 0 << QVector<int>{4};
        QTest::newRow("rotation-on-warp") << QString("warp") << 1 << QVector<int>{};
        QTest::newRow("warp-on-rotation") << QString("rotation") << 0 << QVector<int>{0};
        QTest::newRow("rotation-selection") << QString("rotation") << 1 << QVector<int>{0};
    }
    void invalidSelectionAndHandleDoNotStartDrag()
    {
        QFETCH(QString, type); QFETCH(int, handle); QFETCH(QVector<int>, selected);
        Fixture fixture; CModelDocument document; QVERIFY(document.LoadBytes(fixture.bytes, "selection"));
        CKeyformEditController controller; Preview preview; activate(document, controller, preview, type == "warp" ? "Warp" : "Child", type);
        const auto before = snapshot(document); const int attempts = preview.attempts;
        QVERIFY(!controller.BeginDeformerDrag(static_cast<Handle>(handle), selected)); QVERIFY(!controller.IsDragging());
        unchanged(document, before); QCOMPARE(preview.attempts, attempts);
    }
    void invalidMovementDoesNotPublish_data()
    {
        QTest::addColumn<int>("kind"); QTest::addColumn<QPointF>("delta"); QTest::addColumn<double>("angle");
        const double inf = std::numeric_limits<double>::infinity(), huge = std::numeric_limits<double>::max();
        for (int kind : {0, 1, 2}) {
            QTest::newRow(qPrintable(QString("%1-nan").arg(kind))) << kind << QPointF(std::numeric_limits<double>::quiet_NaN(), 0) << 0.0;
            QTest::newRow(qPrintable(QString("%1-infinity").arg(kind))) << kind << QPointF() << inf;
            QTest::newRow(qPrintable(QString("%1-wrong-component").arg(kind))) << kind << (kind == 2 ? QPointF(1, 0) : QPointF()) << (kind == 2 ? 0.0 : 1.0);
            QTest::newRow(qPrintable(QString("%1-overflow").arg(kind))) << kind << (kind == 2 ? QPointF() : QPointF(huge, 0)) << (kind == 2 ? huge : 0.0);
        }
    }
    void invalidMovementDoesNotPublish()
    {
        QFETCH(int, kind); QFETCH(QPointF, delta); QFETCH(double, angle);
        Fixture fixture; CModelDocument document; QVERIFY(document.LoadBytes(fixture.bytes, "invalid-move"));
        CKeyformEditController controller; Preview preview; activate(document, controller, preview, kind == 0 ? "Warp" : "Child", kind == 0 ? "warp" : "rotation");
        begin(controller, kind); const auto before = snapshot(document); const int attempts = preview.attempts;
        QString error; QVERIFY(!controller.MoveDeformerDrag(delta, angle, &error)); QVERIFY(!error.isEmpty());
        unchanged(document, before); QCOMPARE(preview.attempts, attempts); QVERIFY(controller.IsDragging()); QVERIFY(!controller.DragHasChanges()); QVERIFY(controller.CancelDrag());
    }
    void reflectedSignedParentUsesParentInverse_data()
    {
        QTest::addColumn<float>("scale"); QTest::addColumn<int>("reflection");
        for (float scale : {-2.0f, 2.0f}) for (int reflection : {0, 1, 2, 3})
            QTest::newRow(qPrintable(QString("%1-%2").arg(scale).arg(reflection))) << scale << reflection;
    }
    void reflectedSignedParentUsesParentInverse()
    {
        QFETCH(float, scale); QFETCH(int, reflection);
        Fixture fixture; const auto* o = fixture.table.sectionOffsetTable->rotation_deformer_keyforms;
        real(fixture.bytes, o->scales, scale); fixture.column(o->is_reflect_x, 0, reflection & 1); fixture.column(o->is_reflect_y, 0, (reflection >> 1) & 1);
        CModelDocument document; QVERIFY(document.LoadBytes(fixture.bytes, "reflected"));
        CKeyformEditController controller; Preview preview; activate(document, controller, preview, "Warp", "warp");
        const auto before = document.DeformerWarpKeyforms("Warp");
        QVERIFY(controller.BeginDeformerDrag(Handle::WarpPoints, {1})); QVERIFY(controller.MoveDeformerDrag({0.2, -0.3}, 0));
        const auto after = document.DeformerWarpKeyforms("Warp");
        QVERIFY(std::abs(after[0].positions[2] - before[0].positions[2] - 0.3 / (scale * (reflection & 1 ? -1 : 1))) < 0.00002);
        QVERIFY(std::abs(after[0].positions[3] - before[0].positions[3] + 0.2 / (scale * (reflection & 2 ? -1 : 1))) < 0.00002);
        controller.FinishDrag();
    }
    void zeroOwnScaleAllowsOriginAndAngleButCollapsedParentDoesNotPublish()
    {
        Fixture fixture; const auto* o = fixture.table.sectionOffsetTable->rotation_deformer_keyforms;
        real(fixture.bytes, o->scales + 4, 0);
        CModelDocument document; QVERIFY(document.LoadBytes(fixture.bytes, "zero-own"));
        CKeyformEditController controller; Preview preview; activate(document, controller, preview, "Child", "rotation");
        QVERIFY(controller.BeginDeformerDrag(Handle::RotationAngle, {})); QVERIFY(controller.MoveDeformerDrag({}, 45)); controller.FinishDrag();
        QCOMPARE(document.DeformerRotationKeyforms("Child")[0].scale, 0.0f);
        QVERIFY(controller.BeginDeformerDrag(Handle::RotationOrigin, {})); QVERIFY(controller.MoveDeformerDrag({0.02, 0.03}, 0)); controller.FinishDrag();
        QCOMPARE(document.DeformerRotationKeyforms("Child")[0].scale, 0.0f);
        real(fixture.bytes, o->scales, 0); QVERIFY(document.LoadBytes(fixture.bytes, "zero-parent"));
        activate(document, controller, preview, "Child", "rotation"); const auto before = snapshot(document);
        QVERIFY(!controller.BeginDeformerDrag(Handle::RotationOrigin, {})); unchanged(document, before);
        activate(document, controller, preview, "Warp", "warp"); const int attempts = preview.attempts;
        QString error;
        if (controller.BeginDeformerDrag(Handle::WarpPoints, {0}, &error)) {
            QVERIFY(!controller.MoveDeformerDrag({0.01, 0}, 0, &error)); QVERIFY(!error.isEmpty()); QVERIFY(controller.CancelDrag());
        }
        QCOMPARE(preview.attempts, attempts); unchanged(document, before);
    }
};
QTEST_GUILESS_MAIN(DeformerEditControllerTests)
#include "deformer_edit_controller_tests.moc"
