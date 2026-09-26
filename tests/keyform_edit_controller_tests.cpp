#include "live2dstruct/ckeyformeditcontroller.h"
#include "live2dstruct/cmodelsession.h"
#include "live2dstruct/csectiontable.h"

#include <QtTest>
#include <QtEndian>
#include <functional>
#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

using namespace live2dstruct;

namespace {
QString sample(const QString& name)
{
    return QString(TEST_RESOURCES) + '/' + name + '/' + name + ".moc3";
}

QMap<QString, float> values(const CModelDocument& document)
{
    QMap<QString, float> result;
    for (const auto& parameter : document.Parameters()) result.insert(parameter.id, document.ParameterValue(parameter.id));
    return result;
}

struct Target
{
    QString id;
    int index = -1;
    QMap<QString, float> pose;
};

bool findTarget(const CModelDocument& document, Target& result, QString* error)
{
    for (int index = 0; index < document.Drawables().size(); ++index) {
        const auto& mesh = document.Drawables()[index];
        if (mesh.keyforms.size() < 2 || mesh.vertexCount < 3 || mesh.keyforms[0].positions == mesh.keyforms[1].positions) continue;
        auto pose = values(document);
        QMap<QString, float> corner;
        if (!document.KeyformParameterValues(mesh.id, "artmesh", 0, corner, error)) continue;
        for (auto it = corner.cbegin(); it != corner.cend(); ++it) pose.insert(it.key(), it.value());
        CKeyformMappingContext mapping;
        if (!mapping.Build(document, mesh.id, 0, pose, error)) continue;
        result = {mesh.id, index, pose};
        return true;
    }
    return false;
}

bool mappedVertices(const CModelDocument& document, const CKeyformEditController& controller,
                    QVector<float>& output, QString* error = nullptr)
{
    // The mapping supplies controlled runtime input, not a Cubism Core oracle.
    CKeyformMappingContext mapping;
    return mapping.Build(document, controller.Target(), controller.Keyform(), controller.Pose(), error) &&
           mapping.ToModel(document.DrawableKeyformPositions(controller.Target(), controller.Keyform()), output, error);
}

CModelEdit changed(const CModelDocument& document, const QString& id, int keyform, float delta = 0.01f)
{
    const auto before = document.DrawableKeyformPositions(id, keyform);
    auto after = before;
    if (!after.isEmpty()) after[0] += delta;
    return {id, "artmesh", CModelEdit::KeyformPositions, QVariant::fromValue(before), QVariant::fromValue(after), keyform, false};
}

CModelEdit materialChange(const CModelDocument& document, const QString& id, CModelEdit::Field field)
{
    const auto mesh = *std::find_if(document.Drawables().cbegin(), document.Drawables().cend(),
        [&](const auto& drawable) { return drawable.id == id; });
    if (field == CModelEdit::Visibility) return {id, "artmesh", field, mesh.visible, !mesh.visible};
    if (field == CModelEdit::Opacity) {
        const float before = document.DrawableOpacity(mesh);
        return {id, "artmesh", field, before, before == 0.25f ? 0.75f : 0.25f};
    }
    if (field == CModelEdit::DrawOrder) {
        const int before = qRound(document.DrawableDrawOrder(mesh));
        return {id, "artmesh", field, before, (before + 1) % 1001};
    }
    if (field == CModelEdit::MeshIndices) {
        const auto before = document.DrawableIndices(id);
        auto after = before;
        if (after.size() >= 3) std::swap(after[0], after[1]);
        return {id, "artmesh", field, QVariant::fromValue(before), QVariant::fromValue(after)};
    }
    QMap<QString, bool> before, after;
    const auto state = document.VisibilityState();
    for (const auto& type : {QString("part"), QString("artmesh"), QString("warp"), QString("rotation")}) {
        for (auto it = state.cbegin(); it != state.cend(); ++it) if (it.key().startsWith(type + '|')) {
            before.insert(it.key(), it.value()); after.insert(it.key(), !it.value()); break;
        }
    }
    return {"selection", "objects", CModelEdit::VisibilityValues, QVariant::fromValue(before), QVariant::fromValue(after), -1, false};
}

QByteArray content(const CModelDocument& document)
{
    CModelSession session;
    session.sourceMoc = document.SourcePath();
    session.sourceDigest = document.SourceDigest();
    session.parameters = values(document);
    session.parameterMetadata = document.ParameterMetadataOverrides();
    session.drawableKeyformPositions = document.DrawableKeyformPositionOverrides();
    session.drawableIndices = document.DrawableIndexOverrides();
    session.drawableOpacity = document.DrawableOpacityOverrides();
    session.drawableDrawOrder = document.DrawableDrawOrderOverrides();
    session.drawableMaskIndices = document.DrawableMaskIndexOverrides();
    session.drawableMaskInverted = document.DrawableMaskInvertedOverrides();
    session.deformerParentIndices = document.DeformerParentOverrides();
    session.deformerRotationKeyforms = document.DeformerRotationKeyformOverrides();
    session.deformerWarpKeyforms = document.DeformerWarpKeyformOverrides();
    for (const auto& key : document.LockedKeys()) session.locked.insert(key, true);
    for (const auto& part : document.Parts()) session.visibility.insert("part|" + part.id, part.visible);
    for (const auto& mesh : document.Drawables()) session.visibility.insert("artmesh|" + mesh.id, mesh.visible);
    for (const auto& deformer : document.Deformers()) {
        const QString type = deformer.type == CModelDeformer::Warp ? "warp|" : "rotation|";
        session.visibility.insert(type + deformer.id, deformer.visible);
    }
    return session.ToJson();
}

bool sameEdit(const CModelEdit& left, const CModelEdit& right)
{
    return left.name == right.name && left.type == right.type && left.field == right.field &&
           left.index == right.index && left.before == right.before && left.after == right.after && left.coalesce == right.coalesce;
}

struct Snapshot
{
    QByteArray content;
    QByteArray source;
    quint64 revision;
    bool undoAvailable, redoAvailable;
    CModelEdit undo, redo;
};

Snapshot capture(const CModelDocument& document)
{
    Snapshot result{content(document), document.SourceBytes(), document.Revision(), false, false, {}, {}};
    result.undoAvailable = document.PeekUndoEdit(&result.undo);
    result.redoAvailable = document.PeekRedoEdit(&result.redo);
    return result;
}

struct ControlledPreview
{
    bool reject = false;
    bool throwPublish = false;
    bool runtimeMaterials = false;
    bool rejectEnd = false;
    bool throwEnd = false;
    int attempts = 0;
    int accepted = 0;
    int ended = 0;
    bool editing = false;
    QByteArray publishedBytes;
    QMap<QString, float> pose;
    std::function<void()> onPublish;

    void connect(CKeyformEditController& controller)
    {
        controller.SetPublisher([this](const CModelDocument& candidate, const QMap<QString, float>& values,
                                        bool editMode, QString* error) {
            ++attempts;
            if (onPublish) onPublish();
            if (throwPublish) throw std::runtime_error("controlled publisher exception");
            if (reject) {
                if (error) *error = "controlled runtime rejection";
                return false;
            }
            QByteArray bytes;
            if (!(runtimeMaterials ? candidate.BuildRuntimeMoc(bytes, error) : candidate.BuildPreviewMoc(bytes, error))) return false;
            publishedBytes = std::move(bytes);
            pose = values;
            editing = editMode;
            ++accepted;
            return true;
        }, [this](QString* error) {
            if (throwEnd) throw std::runtime_error("controlled end exception");
            if (rejectEnd) {
                if (error) *error = "controlled end rejection";
                return false;
            }
            ++ended;
            editing = false;
            return true;
        });
    }
};

bool seedHistory(CModelDocument& document, const QString& id)
{
    return document.ExecuteEdit(changed(document, id, 0, 0.005f)) &&
           document.ExecuteEdit(changed(document, id, 1, 0.007f)) && document.UndoEdit();
}

int historyCount(CModelDocument document)
{
    int count = 0;
    while (document.UndoEdit()) ++count;
    return count;
}

void put(QByteArray& bytes, int offset, int value)
{
    qToLittleEndian(qint32(value), bytes.data() + offset);
}

int appendColumn(QByteArray& bytes, int slot, int value)
{
    const int offset = bytes.size();
    bytes.append(QByteArray(4, '\0'));
    put(bytes, slot, offset);
    put(bytes, offset, value);
    return offset;
}
}

class KeyformEditControllerTests : public QObject
{
    Q_OBJECT
    CModelDocument baseline;
    Target target;

    void unchanged(const CModelDocument& document, const Snapshot& before, bool revision = true)
    {
        QCOMPARE(content(document), before.content);
        QCOMPARE(document.SourceBytes(), before.source);
        if (revision) QCOMPARE(document.Revision(), before.revision);
        CModelEdit edit;
        QCOMPARE(document.PeekUndoEdit(&edit), before.undoAvailable);
        if (before.undoAvailable) QVERIFY(sameEdit(edit, before.undo));
        QCOMPARE(document.PeekRedoEdit(&edit), before.redoAvailable);
        if (before.redoAvailable) QVERIFY(sameEdit(edit, before.redo));
    }

    void start(CModelDocument& document, CKeyformEditController& controller, ControlledPreview& preview)
    {
        controller.SetDocument(&document);
        preview.connect(controller);
        QString error;
        QVERIFY2(controller.Activate(target.id, 0, values(document), &error), qPrintable(error));
        QVERIFY(controller.IsActive());
    }

    void begin(CModelDocument& document, CKeyformEditController& controller)
    {
        QVector<float> world;
        QString error;
        QVERIFY2(mappedVertices(document, controller, world, &error), qPrintable(error));
        QVERIFY2(controller.BeginDrag({0}, world, &error), qPrintable(error));
    }

private slots:
    void initTestCase()
    {
        QString error;
        QVERIFY2(baseline.Load(sample("Mark"), &error), qPrintable(error));
        QVERIFY2(findTarget(baseline, target, &error), qPrintable(error));
        QVERIFY(baseline.Drawables()[target.index].parentPartIndex >= 0);
        QVERIFY(baseline.Drawables()[target.index].parentDeformerIndex >= 0);
    }

    void materialPublicationIsAtomicAndKeepsEditorMode_data()
    {
        QTest::addColumn<int>("field"); QTest::addColumn<bool>("active");
        for (int field : {int(CModelEdit::Visibility), int(CModelEdit::VisibilityValues), int(CModelEdit::Opacity),
                          int(CModelEdit::DrawOrder), int(CModelEdit::MeshIndices)})
            for (bool active : {false, true}) QTest::newRow(qPrintable(QString("%1-%2").arg(field).arg(active))) << field << active;
    }
    void materialPublicationIsAtomicAndKeepsEditorMode()
    {
        QFETCH(int, field); QFETCH(bool, active);
        CModelDocument document = baseline; CKeyformEditController controller; ControlledPreview preview;
        preview.runtimeMaterials = true; controller.SetDocument(&document); preview.connect(controller);
        QString error;
        if (active) QVERIFY2(controller.Activate(target.id, 1, values(document), &error), qPrintable(error));
        const auto pose = controller.Pose(); const auto selected = controller.Target(); const int keyform = controller.Keyform();
        const auto before = capture(document);
        const auto command = materialChange(document, target.id, CModelEdit::Field(field));
        QVERIFY(CKeyformEditController::Handles(command)); QVERIFY(command.before != command.after);
        preview.reject = true;
        QVERIFY(!controller.Submit(command, &error)); QVERIFY(!error.isEmpty()); unchanged(document, before);
        preview.reject = false; preview.throwPublish = true;
        QVERIFY(!controller.Submit(command, &error)); QVERIFY(!error.isEmpty()); unchanged(document, before);
        QCOMPARE(controller.IsActive(), active); QCOMPARE(controller.Target(), selected); QCOMPARE(controller.Keyform(), keyform); QCOMPARE(controller.Pose(), pose);
        preview.throwPublish = false;
        bool reentryRejected = false;
        preview.onPublish = [&] { reentryRejected = !controller.Submit(command); };
        QVERIFY2(controller.Submit(command, &error), qPrintable(error)); QVERIFY(reentryRejected); preview.onPublish = {};
        QCOMPARE(preview.editing, active); QCOMPARE(preview.pose, pose); QCOMPARE(preview.ended, 0);
        QCOMPARE(controller.IsActive(), active); QCOMPARE(controller.Target(), selected); QCOMPARE(controller.Keyform(), keyform); QCOMPARE(controller.Pose(), pose);
        QVERIFY(content(document) != before.content); QCOMPARE(document.SourceBytes(), before.source);
        const auto accepted = capture(document); const auto published = preview.publishedBytes;
        CModelEdit restored; preview.reject = true;
        QVERIFY(!controller.History(true, restored, &error)); unchanged(document, accepted); QCOMPARE(preview.publishedBytes, published);
        preview.reject = false;
        QVERIFY2(controller.History(true, restored, &error), qPrintable(error)); QCOMPARE(content(document), before.content);
        const auto undone = capture(document); preview.throwPublish = true;
        QVERIFY(!controller.History(false, restored, &error)); unchanged(document, undone);
        preview.throwPublish = false;
        QVERIFY2(controller.History(false, restored, &error), qPrintable(error)); QCOMPARE(content(document), accepted.content);
        QCOMPARE(controller.IsActive(), active); QCOMPARE(controller.Keyform(), keyform); QCOMPARE(controller.Pose(), pose); QCOMPARE(preview.ended, 0);
    }

    void materialInputsRejectStaleTypesAndInvalidValuesBeforePublication()
    {
        CModelDocument document = baseline; CKeyformEditController controller; ControlledPreview preview;
        controller.SetDocument(&document); preview.connect(controller); const auto before = capture(document);
        QVector<CModelEdit> rejected;
        for (auto field : {CModelEdit::Visibility, CModelEdit::Opacity, CModelEdit::DrawOrder, CModelEdit::MeshIndices}) {
            auto command = materialChange(document, target.id, field);
            command.before = command.after; command.after = materialChange(document, target.id, field).before;
            rejected.append(command);
            command = materialChange(document, target.id, field); command.type = "part"; rejected.append(command);
        }
        auto visibility = materialChange(document, target.id, CModelEdit::Visibility); visibility.before = "true"; rejected.append(visibility);
        auto opacity = materialChange(document, target.id, CModelEdit::Opacity); opacity.after = std::numeric_limits<float>::quiet_NaN(); rejected.append(opacity);
        opacity.after = 1.5f; rejected.append(opacity); opacity.after = 0.5; rejected.append(opacity);
        auto order = materialChange(document, target.id, CModelEdit::DrawOrder); order.after = 1001; rejected.append(order);
        order.after = 1.25; rejected.append(order);
        auto topology = materialChange(document, target.id, CModelEdit::MeshIndices); topology.after = QVariant::fromValue(QVector<quint16>{0, 1}); rejected.append(topology);
        auto batch = materialChange(document, target.id, CModelEdit::VisibilityValues);
        auto next = batch.after.value<QMap<QString, bool>>(); next.insert("part|missing", false); batch.after = QVariant::fromValue(next); rejected.append(batch);
        for (const auto& command : rejected) {
            QString error; QVERIFY(!controller.Submit(command, &error)); QVERIFY(!error.isEmpty()); unchanged(document, before);
        }
        QCOMPARE(preview.attempts, 0); QVERIFY(!controller.IsActive());
    }

    void materialLocksCoverParentsAndEntireVisibilityBatch()
    {
        for (const auto& kind : {QString("artmesh"), QString("part"), QString("deformer")}) {
            CModelDocument document = baseline; CKeyformEditController controller; ControlledPreview preview;
            const auto mesh = document.Drawables()[target.index];
            QString type = kind, id = target.id;
            if (kind == "part") id = document.Parts()[mesh.parentPartIndex].id;
            if (kind == "deformer") {
                const auto deformer = document.Deformers()[mesh.parentDeformerIndex];
                type = deformer.type == CModelDeformer::Warp ? "warp" : "rotation"; id = deformer.id;
            }
            document.SetLocked(type, id, true); const auto before = capture(document);
            controller.SetDocument(&document); preview.connect(controller);
            for (auto field : {CModelEdit::Visibility, CModelEdit::Opacity, CModelEdit::DrawOrder, CModelEdit::MeshIndices}) {
                QVERIFY(!controller.Submit(materialChange(document, target.id, field))); unchanged(document, before);
            }
            auto state = document.VisibilityState();
            QMap<QString, bool> previous{{"artmesh|" + target.id, state.value("artmesh|" + target.id)},
                {"part|" + document.Parts().first().id, state.value("part|" + document.Parts().first().id)}};
            auto next = previous; for (auto it = next.begin(); it != next.end(); ++it) it.value() = !it.value();
            CModelEdit batch{"selection", "objects", CModelEdit::VisibilityValues, QVariant::fromValue(previous), QVariant::fromValue(next), -1, false};
            QVERIFY(!controller.Submit(batch)); unchanged(document, before); QCOMPARE(preview.attempts, 0);
        }
    }

    void materialPartVisibilityChecksAncestorLocks()
    {
        auto bytes = baseline.SourceBytes(); CReader reader = CReader::FromBytes(bytes); CSectionTable table(reader);
        QVERIFY(table.IsValid()); QVERIFY(table.parts.size() >= 2);
        const int offset = table.sectionOffsetTable->parts->parent_part_indices;
        for (int index = 0; index < table.parts.size(); ++index) put(bytes, offset + index * 4, -1);
        put(bytes, offset + 4, 0);
        CModelDocument document; QString error; QVERIFY(document.LoadBytes(bytes, "part-parent.moc3", &error));
        const auto part = document.Parts()[1];
        CKeyformEditController controller; ControlledPreview preview; controller.SetDocument(&document); preview.connect(controller);
        const CModelEdit command{part.id, "part", CModelEdit::Visibility, part.visible, !part.visible};
        document.SetLocked("part", document.Parts()[0].id, true); const auto before = capture(document);
        QVERIFY(!controller.Submit(command)); unchanged(document, before); QCOMPARE(preview.attempts, 0);
        document.SetLocked("part", document.Parts()[0].id, false);
        QVERIFY2(controller.Submit(command, &error), qPrintable(error)); QCOMPARE(document.Parts()[1].visible, !part.visible);
        QVERIFY(!controller.IsActive());
    }

    void batchVisibilityValidatesSharedAncestorsBeforeChangingAnyTarget()
    {
        auto bytes = baseline.SourceBytes(); CReader reader = CReader::FromBytes(bytes); CSectionTable table(reader);
        QVERIFY(table.IsValid()); QVERIFY(table.parts.size() >= 2);
        const int offset = table.sectionOffsetTable->parts->parent_part_indices;
        put(bytes, offset, -1);
        for (int index = 1; index < table.parts.size(); ++index) put(bytes, offset + index * 4, index - 1);
        CModelDocument document; QString error; QVERIFY(document.LoadBytes(bytes, "part-chain.moc3", &error));
        auto before = document.VisibilityState(); before.remove("part|" + document.Parts().first().id);
        auto after = before; for (auto it = after.begin(); it != after.end(); ++it) it.value() = !it.value();
        QVERIFY(before.size() > 1);
        const CModelEdit command{"selection", "objects", CModelEdit::VisibilityValues,
            QVariant::fromValue(before), QVariant::fromValue(after), -1, false};
        CKeyformEditController controller; ControlledPreview preview; controller.SetDocument(&document); preview.connect(controller);
        document.SetLocked("part", document.Parts().first().id, true); const auto original = capture(document);
        QVERIFY(!controller.Submit(command, &error)); unchanged(document, original); QCOMPARE(preview.attempts, 0);
        document.SetLocked("part", document.Parts().first().id, false);
        QVERIFY2(controller.Submit(command, &error), qPrintable(error));
        const auto accepted = document.VisibilityState();
        for (auto it = after.cbegin(); it != after.cend(); ++it) QCOMPARE(accepted.value(it.key()), it.value());
        CModelEdit undo; QVERIFY(controller.History(true, undo));
        const auto restored = document.VisibilityState();
        for (auto it = before.cbegin(); it != before.cend(); ++it) QCOMPARE(restored.value(it.key()), it.value());
    }

    void materialCoalescingPreservesDocumentHistorySemantics()
    {
        for (bool coalesce : {false, true}) {
            CModelDocument document = baseline; CKeyformEditController controller; ControlledPreview preview;
            controller.SetDocument(&document); preview.connect(controller);
            const float original = document.DrawableOpacity(document.Drawables()[target.index]);
            CModelEdit first{target.id, "artmesh", CModelEdit::Opacity, original, 0.8f, -1, coalesce};
            CModelEdit second{target.id, "artmesh", CModelEdit::Opacity, 0.8f, 0.6f, -1, coalesce};
            QVERIFY(controller.Submit(first)); QVERIFY(controller.Submit(second));
            QCOMPARE(historyCount(document), coalesce ? 1 : 2); CModelEdit edit;
            QVERIFY(controller.History(true, edit));
            QCOMPARE(document.DrawableOpacity(document.Drawables()[target.index]), coalesce ? original : 0.8f);
            if (!coalesce) QVERIFY(controller.History(true, edit));
            QCOMPARE(document.DrawableOpacity(document.Drawables()[target.index]), original); QVERIFY(!document.CanUndoEdit());
            QVERIFY(document.DrawableOpacityOverrides().isEmpty()); QCOMPARE(preview.ended, 0);
        }
    }

    void activationUsesTheSelectedObjectsCornerWithoutEditingTheDocument_data()
    {
        QTest::addColumn<QString>("model");
        for (const char* model : {"Mark", "Rice", "Natori", "Wanko"}) QTest::newRow(model) << QString::fromLatin1(model);
    }

    void activationUsesTheSelectedObjectsCornerWithoutEditingTheDocument()
    {
        QFETCH(QString, model);
        CModelDocument document;
        Target selected;
        QString error;
        QVERIFY2(document.Load(sample(model), &error), qPrintable(error));
        QVERIFY2(findTarget(document, selected, &error), qPrintable(error));
        const auto before = capture(document);
        CKeyformEditController controller;
        ControlledPreview preview;
        controller.SetDocument(&document);
        preview.connect(controller);
        auto current = values(document);
        for (const auto& parameter : document.Parameters()) current[parameter.id] = parameter.minimum;
        QMap<QString, float> corner;
        QVERIFY(document.KeyformParameterValues(selected.id, "artmesh", 1, corner));
        auto expected = current;
        for (auto it = corner.cbegin(); it != corner.cend(); ++it) expected.insert(it.key(), it.value());
        QVERIFY2(controller.Activate(selected.id, 1, current, &error), qPrintable(error));
        QCOMPARE(controller.Target(), selected.id);
        QCOMPARE(controller.Keyform(), 1);
        QCOMPARE(controller.Pose(), expected);
        QCOMPARE(preview.pose, expected);
        QVERIFY(preview.editing);
        QCOMPARE(preview.accepted, 1);
        unchanged(document, before);
        preview.reject = true;
        QVERIFY(!controller.Activate(selected.id, 0, current, &error));
        QCOMPARE(controller.Keyform(), 1);
        QCOMPARE(controller.Pose(), expected);
        unchanged(document, before);
        preview.reject = false;
        preview.throwPublish = true;
        QVERIFY(!controller.Activate(selected.id, 0, current, &error));
        QCOMPARE(controller.Keyform(), 1);
        QVERIFY(!error.isEmpty());
        unchanged(document, before);
    }

    void failedSubmitKeepsBothHistoryBranches_data()
    {
        QTest::addColumn<bool>("exception");
        QTest::newRow("false") << false;
        QTest::newRow("exception") << true;
    }

    void failedSubmitKeepsBothHistoryBranches()
    {
        QFETCH(bool, exception);
        CModelDocument document = baseline;
        QVERIFY(seedHistory(document, target.id));
        CKeyformEditController controller;
        ControlledPreview preview;
        start(document, controller, preview);
        const auto before = capture(document);
        preview.reject = !exception;
        preview.throwPublish = exception;
        QString error;
        QVERIFY(!controller.Submit(changed(document, target.id, 0), &error));
        QVERIFY(!error.isEmpty());
        unchanged(document, before);
        QVERIFY(document.UndoEdit());
        QCOMPARE(document.DrawableKeyformPositions(target.id, 0), baseline.DrawableKeyformPositions(target.id, 0));
        QVERIFY(document.RedoEdit());
        QVERIFY(document.RedoEdit());
        QVERIFY(!document.CanRedoEdit());
    }

    void editingAnotherFormOfTheSameMeshKeepsTheSelectedPose()
    {
        CModelDocument document = baseline;
        CKeyformEditController controller;
        ControlledPreview preview;
        start(document, controller, preview);
        const auto selectedPose = controller.Pose();
        const auto selectedPositions = document.DrawableKeyformPositions(target.id, 0);
        auto command = changed(document, target.id, 1);
        command.after = QVariant::fromValue(selectedPositions);
        QVERIFY(command.before != command.after);
        QVERIFY(controller.Submit(command));
        QCOMPARE(controller.Keyform(), 0);
        QCOMPARE(controller.Target(), target.id);
        QCOMPARE(controller.Pose(), selectedPose);
        QCOMPARE(preview.pose, selectedPose);
        QCOMPARE(document.DrawableKeyformPositions(target.id, 0), selectedPositions);
        QCOMPARE(document.DrawableKeyformPositions(target.id, 1), command.after.value<QVector<float>>());
        CModelDocument published;
        QVERIFY(published.LoadBytes(preview.publishedBytes, "controlled-preview"));
        QCOMPARE(published.DrawableKeyformPositions(target.id, 1), command.after.value<QVector<float>>());
        QCOMPARE(historyCount(document), 1);
    }

    void dragSamplesUseAbsoluteDisplacementAndSeparateGestures()
    {
        CModelDocument document = baseline;
        CKeyformEditController controller;
        ControlledPreview preview;
        start(document, controller, preview);
        CKeyformMappingContext mapping;
        QVERIFY(mapping.Build(document, target.id, 0, controller.Pose()));
        const auto local = document.DrawableKeyformPositions(target.id, 0);
        QVector<float> world;
        QVERIFY(mapping.ToModel(local, world));
        QVERIFY(controller.BeginDrag({0}, world));
        QVERIFY(controller.DragBy({0.001, -0.001}));
        QVERIFY(controller.DragBy({0.002, -0.002}));
        auto absoluteWorld = world;
        absoluteWorld[0] = float(double(world[0]) + 0.002);
        absoluteWorld[1] = float(double(world[1]) - 0.002);
        QVector<float> absoluteLocal;
        QVERIFY(mapping.FromModel(absoluteWorld, absoluteLocal));
        auto expected = local;
        expected[0] = absoluteLocal[0];
        expected[1] = absoluteLocal[1];
        QCOMPARE(document.DrawableKeyformPositions(target.id, 0), expected);
        QCOMPARE(historyCount(document), 1);
        const int attempts = preview.attempts;
        QVERIFY(controller.DragBy({0.002, -0.002}));
        QCOMPARE(preview.attempts, attempts);
        controller.FinishDrag();
        QVERIFY(!controller.IsDragging());
        const auto firstDrag = document.DrawableKeyformPositions(target.id, 0);
        begin(document, controller);
        QVERIFY(controller.DragBy({0.001, 0.001}));
        controller.FinishDrag();
        QCOMPARE(historyCount(document), 2);
        const auto secondDrag = document.DrawableKeyformPositions(target.id, 0);
        CModelEdit history;
        QVERIFY(controller.History(true, history));
        QCOMPARE(document.DrawableKeyformPositions(target.id, 0), firstDrag);
        QVERIFY(controller.History(true, history));
        QCOMPARE(document.DrawableKeyformPositions(target.id, 0), local);
        QVERIFY(controller.History(false, history));
        QVERIFY(controller.History(false, history));
        QCOMPARE(document.DrawableKeyformPositions(target.id, 0), secondDrag);
    }

    void rejectedDragSamplesKeepTheLastAcceptedState()
    {
        CModelDocument document = baseline;
        CKeyformEditController controller;
        ControlledPreview preview;
        start(document, controller, preview);
        begin(document, controller);
        QVERIFY(controller.DragBy({0.001, 0.001}));
        const auto accepted = capture(document);
        preview.reject = true;
        QString error;
        QVERIFY(!controller.DragBy({0.002, 0.002}, &error));
        QVERIFY(controller.IsDragging());
        unchanged(document, accepted);
        QVERIFY(!controller.CancelDrag(&error));
        QVERIFY(controller.IsDragging());
        unchanged(document, accepted);
        preview.reject = false;
        QVERIFY(controller.DragBy({0.003, 0.003}, &error));
        QCOMPARE(historyCount(document), 1);
        const auto next = capture(document);
        QVERIFY(!controller.DragBy({std::numeric_limits<double>::infinity(), 0}, &error));
        unchanged(document, next);
        controller.FinishDrag();
    }

    void switchingOrEndingRequiresExplicitlyFinishingTheDrag()
    {
        CModelDocument document = baseline;
        CKeyformEditController controller;
        ControlledPreview preview;
        start(document, controller, preview);
        begin(document, controller);
        QVERIFY(controller.DragBy({0.001, 0.001}));
        const auto accepted = capture(document);
        const auto pose = controller.Pose();
        const int attempts = preview.attempts;
        QVERIFY(!controller.Activate(target.id, 1, values(document)));
        QVERIFY(!controller.End());
        QVERIFY(controller.IsDragging());
        QVERIFY(controller.IsActive());
        QCOMPARE(controller.Keyform(), 0);
        QCOMPARE(controller.Pose(), pose);
        QCOMPARE(preview.attempts, attempts);
        QCOMPARE(preview.ended, 0);
        unchanged(document, accepted);
        controller.FinishDrag();
        QVERIFY(controller.End());
        unchanged(document, accepted);
    }

    void returningToOriginOrCancelingRestoresTheRedoBranch_data()
    {
        QTest::addColumn<bool>("cancel");
        QTest::newRow("return-to-origin") << false;
        QTest::newRow("cancel") << true;
    }

    void returningToOriginOrCancelingRestoresTheRedoBranch()
    {
        QFETCH(bool, cancel);
        CModelDocument document = baseline;
        QVERIFY(seedHistory(document, target.id));
        CKeyformEditController controller;
        ControlledPreview preview;
        start(document, controller, preview);
        const auto before = capture(document);
        begin(document, controller);
        QVERIFY(controller.DragBy({0.001, 0.001}));
        QVERIFY(!document.CanRedoEdit());
        if (cancel) QVERIFY(controller.CancelDrag());
        else {
            QVERIFY(controller.DragBy({0, 0}));
            QVERIFY(controller.IsDragging());
            controller.FinishDrag();
        }
        QVERIFY(!controller.IsDragging());
        unchanged(document, before, false);
        QVERIFY(document.Revision() > before.revision);
        CModelEdit redo;
        QVERIFY(controller.History(false, redo));
        QVERIFY(sameEdit(redo, before.redo));
        QCOMPARE(document.DrawableKeyformPositions(redo.name, redo.index), redo.after.value<QVector<float>>());
    }

    void lockedObjectsAndAncestorsBlockEdits_data()
    {
        QTest::addColumn<QString>("kind");
        QTest::newRow("mesh") << QStringLiteral("artmesh");
        QTest::newRow("part") << QStringLiteral("part");
        QTest::newRow("deformer") << QStringLiteral("deformer");
    }

    void lockedObjectsAndAncestorsBlockEdits()
    {
        QFETCH(QString, kind);
        CModelDocument document = baseline;
        QString id = target.id;
        if (kind == "part") id = document.Parts()[document.Drawables()[target.index].parentPartIndex].id;
        if (kind == "deformer") {
            const auto& deformer = document.Deformers()[document.Drawables()[target.index].parentDeformerIndex];
            id = deformer.id;
            kind = deformer.type == CModelDeformer::Warp ? "warp" : "rotation";
        }
        document.SetLocked(kind, id, true);
        const auto locked = capture(document);
        CKeyformEditController controller;
        ControlledPreview preview;
        controller.SetDocument(&document);
        preview.connect(controller);
        QVERIFY(!controller.Activate(target.id, 0, values(document)));
        QVERIFY(!controller.Submit(changed(document, target.id, 0)));
        QCOMPARE(preview.attempts, 0);
        unchanged(document, locked);
        document.SetLocked(kind, id, false);
        QVERIFY(controller.Activate(target.id, 0, values(document)));
        begin(document, controller);
        document.SetLocked(kind, id, true);
        const auto relocked = capture(document);
        QVERIFY(!controller.DragBy({0.001, 0.001}));
        unchanged(document, relocked);
        controller.FinishDrag();
    }

    void staleDragDoesNotOverwriteLaterChanges()
    {
        CModelDocument document = baseline;
        CKeyformEditController controller;
        ControlledPreview preview;
        start(document, controller, preview);
        begin(document, controller);
        QVERIFY(document.SetDrawableOpacity(target.id, 0.4f));
        const auto changedState = capture(document);
        const int attempts = preview.attempts;
        QVERIFY(!controller.DragBy({0.001, 0.001}));
        QVERIFY(!controller.CancelDrag());
        QCOMPARE(preview.attempts, attempts);
        unchanged(document, changedState);
        controller.FinishDrag();
    }

    void wrongRuntimeVerticesAndInvalidSelectionsDoNotStartDragging()
    {
        CModelDocument document = baseline;
        CKeyformEditController controller;
        ControlledPreview preview;
        start(document, controller, preview);
        QVector<float> world;
        QVERIFY(mappedVertices(document, controller, world));
        const auto before = capture(document);
        auto wrong = world;
        wrong[0] += 1;
        QVERIFY(!controller.BeginDrag({0}, wrong));
        wrong[0] = std::numeric_limits<float>::quiet_NaN();
        QVERIFY(!controller.BeginDrag({0}, wrong));
        QVERIFY(!controller.BeginDrag({0}, world.mid(0, 2)));
        QVERIFY(!controller.BeginDrag({}, world));
        QVERIFY(!controller.BeginDrag({0, 0}, world));
        QVERIFY(!controller.BeginDrag({-1}, world));
        QVERIFY(!controller.BeginDrag({int(world.size() / 2)}, world));
        QVERIFY(!controller.IsDragging());
        unchanged(document, before);
    }

    void glueAffectedMeshesCannotStartCanvasEditing()
    {
        QByteArray bytes = baseline.SourceBytes();
        CReader reader = CReader::FromBytes(bytes);
        CSectionTable table(reader);
        QVERIFY(table.IsValid());
        put(bytes, table.sectionOffsetTable->count_info->count_info_offset + 20 * 4, 1);
        for (int slot : {0x1a4, 0x1ac, 0x1b0, 0x1b4, 0x1b8, 0x1bc, 0x1c0, 0x1c4})
            appendColumn(bytes, slot, slot == 0x1ac ? -1 : (slot == 0x1b8 || slot == 0x1bc ? target.index : 0));
        const int idOffset = bytes.size();
        bytes.append(QByteArray("Glue").leftJustified(64, '\0'));
        put(bytes, 0x1a8, idOffset);
        CModelDocument document;
        QString error;
        QVERIFY2(document.LoadBytes(bytes, "glue-model", &error), qPrintable(error));
        CKeyformEditController controller;
        ControlledPreview preview;
        start(document, controller, preview);
        CKeyformMappingContext baselineMapping;
        QVERIFY(baselineMapping.Build(baseline, target.id, 0, controller.Pose()));
        QVector<float> unchangedWorld;
        QVERIFY(baselineMapping.ToModel(baseline.DrawableKeyformPositions(target.id, 0), unchangedWorld));
        const auto before = capture(document);
        QVERIFY(!controller.BeginDrag({0}, unchangedWorld, &error));
        QVERIFY(error.contains("Glue"));
        QVERIFY(!controller.IsDragging());
        unchanged(document, before);
    }

    void failedHistoryDoesNotAdvanceOrPublishItsOutputCommand()
    {
        CModelDocument document = baseline;
        CKeyformEditController controller;
        ControlledPreview preview;
        start(document, controller, preview);
        QVERIFY(controller.Submit(changed(document, target.id, 0)));
        const auto beforeUndo = capture(document);
        CModelEdit sentinel{"sentinel", "part", CModelEdit::Visibility, true, false, -1, false};
        CModelEdit output = sentinel;
        preview.reject = true;
        QVERIFY(!controller.History(true, output));
        QVERIFY(sameEdit(output, sentinel));
        unchanged(document, beforeUndo);
        preview.reject = false;
        QVERIFY(controller.History(true, output));
        const auto beforeRedo = capture(document);
        output = sentinel;
        preview.reject = true;
        QVERIFY(!controller.History(false, output));
        QVERIFY(sameEdit(output, sentinel));
        unchanged(document, beforeRedo);
    }

    void changedSourceRequiresExplicitReattachment()
    {
        CModelDocument document = baseline;
        CKeyformEditController controller;
        ControlledPreview preview;
        start(document, controller, preview);
        QVERIFY(document.Load(sample("Wanko")));
        const auto before = capture(document);
        const int attempts = preview.attempts;
        QVERIFY(!controller.Submit(changed(baseline, target.id, 0)));
        QVERIFY(!controller.Activate(target.id, 0, {}));
        CModelEdit output;
        QVERIFY(!controller.History(true, output));
        QCOMPARE(preview.attempts, attempts);
        unchanged(document, before);
    }

    void nestedControllerActionsCannotPublishOrResetAnActiveDrag()
    {
        CModelDocument document = baseline, other = baseline;
        CKeyformEditController controller;
        ControlledPreview preview;
        start(document, controller, preview);
        begin(document, controller);
        const auto otherBefore = capture(other);
        bool invoked = false;
        bool rejected = false;
        int nestedPublisherCalls = 0;
        preview.onPublish = [&] {
            invoked = true;
            CModelEdit output;
            rejected = !controller.Activate(target.id, 1, {}) &&
                       !controller.Submit(changed(document, target.id, 1)) &&
                       !controller.History(true, output) && !controller.End();
            controller.SetDocument(&other);
            controller.FinishDrag();
            controller.SetPublisher([&](const CModelDocument&, const QMap<QString, float>&, bool, QString*) {
                ++nestedPublisherCalls;
                return false;
            }, {});
        };
        QString error;
        QVERIFY2(controller.DragBy({0.001, 0.001}, &error), qPrintable(error));
        QVERIFY(invoked);
        QVERIFY(rejected);
        QVERIFY(controller.IsDragging());
        QCOMPARE(controller.Target(), target.id);
        QCOMPARE(controller.Keyform(), 0);
        unchanged(other, otherBefore);
        QCOMPARE(historyCount(document), 1);
        preview.onPublish = {};
        QVERIFY(controller.CancelDrag());
        QCOMPARE(nestedPublisherCalls, 0);
    }

    void publisherSideChangesCannotBeOverwritten_data()
    {
        QTest::addColumn<bool>("changeSource");
        QTest::addColumn<bool>("activate");
        QTest::newRow("submit-revision") << false << false;
        QTest::newRow("submit-source") << true << false;
        QTest::newRow("activate-revision") << false << true;
        QTest::newRow("activate-source") << true << true;
    }

    void publisherSideChangesCannotBeOverwritten()
    {
        QFETCH(bool, changeSource);
        QFETCH(bool, activate);
        CModelDocument document = baseline;
        CKeyformEditController controller;
        ControlledPreview preview;
        start(document, controller, preview);
        const auto originalPose = controller.Pose();
        const auto command = changed(document, target.id, 0);
        bool modified = false;
        Snapshot concurrent;
        preview.onPublish = [&] {
            modified = changeSource ? document.Load(sample("Wanko")) : document.SetDrawableOpacity(target.id, 0.45f);
            concurrent = capture(document);
        };
        QString error;
        const bool accepted = activate ? controller.Activate(target.id, 1, values(document), &error)
                                       : controller.Submit(command, &error);
        QVERIFY(modified);
        QVERIFY(!accepted);
        QVERIFY(!error.isEmpty());
        unchanged(document, concurrent);
        QCOMPARE(controller.Keyform(), 0);
        QCOMPARE(controller.Pose(), originalPose);
    }

    void restoringCandidatePublishesOnlyAfterRuntimeAcceptance()
    {
        CModelDocument document = baseline;
        QVERIFY(seedHistory(document, target.id));
        CModelSession session;
        session.sourceDigest = baseline.SourceDigest();
        auto restoredPositions = baseline.DrawableKeyformPositions(target.id, 1);
        restoredPositions[1] += 0.03f;
        session.drawableKeyformPositions[target.id] = {{}, restoredPositions};
        CModelDocument candidate;
        QString error;
        QVERIFY2(session.RestoreDocument(document, candidate, &error), qPrintable(error));
        CKeyformEditController controller;
        ControlledPreview preview;
        controller.SetDocument(&document);
        preview.connect(controller);
        const auto before = capture(document);
        preview.reject = true;
        QVERIFY(!controller.Restore(candidate, &error));
        unchanged(document, before);
        preview.reject = false;
        QVERIFY2(controller.Restore(candidate, &error), qPrintable(error));
        QCOMPARE(document.DrawableKeyformPositions(target.id, 1), restoredPositions);
        QCOMPARE(document.DrawableKeyformPositions(target.id, 0), baseline.DrawableKeyformPositions(target.id, 0));
        QVERIFY(!document.CanUndoEdit());
        QVERIFY(!document.CanRedoEdit());
        QVERIFY(!preview.editing);
        QVERIFY(!controller.IsActive());
        CModelDocument unrelated;
        QVERIFY(unrelated.Load(sample("Wanko")));
        const auto accepted = capture(document);
        QVERIFY(!controller.Restore(unrelated, &error));
        unchanged(document, accepted);
        QVERIFY(controller.Activate(target.id, 0, values(document)));
        QVERIFY(!controller.Restore(candidate, &error));
    }

    void firstEditPublishesOnceAndKeepsTheSelectedCorner()
    {
        CModelDocument document = baseline;
        CKeyformEditController controller;
        ControlledPreview preview;
        controller.SetDocument(&document);
        preview.connect(controller);
        const auto before = capture(document);
        auto current = values(document);
        for (const auto& parameter : document.Parameters()) current[parameter.id] = parameter.minimum;
        QMap<QString, float> corner;
        QVERIFY(document.KeyformParameterValues(target.id, "artmesh", 0, corner));
        auto expected = current;
        for (auto it = corner.cbegin(); it != corner.cend(); ++it) expected[it.key()] = it.value();
        const auto edit = changed(document, target.id, 1);
        QString error;
        preview.reject = true;
        QVERIFY(!controller.Submit(edit, current, 0, &error));
        QCOMPARE(preview.attempts, 1);
        QCOMPARE(preview.accepted, 0);
        QVERIFY(!controller.IsActive());
        unchanged(document, before);
        preview.reject = false;
        QVERIFY2(controller.Submit(edit, current, 0, &error), qPrintable(error));
        QCOMPARE(preview.attempts, 2);
        QCOMPARE(preview.accepted, 1);
        QCOMPARE(controller.Keyform(), 0);
        QCOMPARE(controller.Pose(), expected);
        QCOMPARE(preview.pose, expected);
        QCOMPARE(document.DrawableKeyformPositions(target.id, 0), baseline.DrawableKeyformPositions(target.id, 0));
        QCOMPARE(document.DrawableKeyformPositions(target.id, 1), edit.after.value<QVector<float>>());
        QVERIFY(document.CanUndoEdit());
        QVERIFY(!document.CanRedoEdit());
    }

    void endingPreviewFailureKeepsTheEditSession_data()
    {
        QTest::addColumn<bool>("exception");
        QTest::newRow("false") << false;
        QTest::newRow("exception") << true;
    }

    void endingPreviewFailureKeepsTheEditSession()
    {
        QFETCH(bool, exception);
        CModelDocument document = baseline;
        CKeyformEditController controller;
        ControlledPreview preview;
        start(document, controller, preview);
        const auto before = capture(document);
        const auto pose = controller.Pose();
        preview.rejectEnd = !exception;
        preview.throwEnd = exception;
        QString error;
        QVERIFY(!controller.End(&error));
        QVERIFY(!error.isEmpty());
        QVERIFY(controller.IsActive());
        QCOMPARE(controller.Pose(), pose);
        unchanged(document, before);
        preview.rejectEnd = false;
        preview.throwEnd = false;
        QVERIFY(controller.End(&error));
        QVERIFY(!controller.IsActive());
        QVERIFY(controller.Pose().isEmpty());
        QCOMPARE(preview.ended, 1);
        unchanged(document, before);
    }
};

QTEST_GUILESS_MAIN(KeyformEditControllerTests)
#include "keyform_edit_controller_tests.moc"
