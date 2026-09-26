#include "live2dstruct/ckeyformeditcontroller.h"
#include "live2dstruct/cdeformerpreserverequest.h"
#include <QtTest>
#include <QCryptographicHash>
#include <stdexcept>
#include <limits>

using namespace live2dstruct;
using Builder = CMocDeformerStructureBuilder;
using Controller = CKeyformEditController;

namespace {
QMap<QString, float> values(const CModelDocument& document)
{
    QMap<QString, float> result;
    for (const auto& parameter : document.Parameters()) result.insert(parameter.id, document.ParameterValue(parameter.id));
    return result;
}
CDeformerPreserveRequest requestFor(const CModelDocument& document,
                                  std::variant<Builder::InsertRequest, Builder::RemoveRequest> operation)
{
    QByteArray bytes;
    if (!document.BuildPreviewMoc(bytes)) return {};
    return {std::move(operation), document.SourceDigest(), QCryptographicHash::hash(bytes, QCryptographicHash::Sha256)};
}
bool insert(const CModelDocument& document, const QString& id, CModelEdit& edit, QString* error,
            bool warp = false, QVector<Builder::Owner> children = {}, int part = -1, QString parent = {})
{
    Builder::InsertRequest value;
    value.id = id; value.partIndex = part; value.parentId = parent; value.children = std::move(children);
    if (warp) value.shape = Builder::Warp{};
    else value.shape = Builder::Rotation{};
    return document.MakeDeformerStructureEdit(requestFor(document, value), edit, error);
}
bool remove(const CModelDocument& document, const QString& id, CModelEdit& edit, QString* error)
{
    return document.MakeDeformerStructureEdit(requestFor(document, Builder::RemoveRequest{id}), edit, error);
}
bool same(const CModelEditRuntimeState& a, const CModelEditRuntimeState& b)
{
    return a.before == b.before && a.after == b.after && a.beforeEditor == b.beforeEditor && a.afterEditor == b.afterEditor;
}
struct Runtime
{
    QByteArray bytes;
    Controller::EditorState editor;
    bool reject = false, throwing = false, dirtyFailure = false;
    int attempts = 0, accepted = 0, ended = 0, combined = 0, meshes = 0;
    bool restoredSession = false;
    std::function<void()> during;

    void connect(Controller& controller)
    {
        controller.SetPublisher([this](const CModelDocument& document, const auto& pose, bool editing, QString* error) {
            QByteArray candidate;
            if (reject || !document.BuildRuntimeMoc(candidate, error)) return false;
            bytes.swap(candidate);
            if (!editing) editor = {};
            else editor.pose = pose;
            return true;
        }, [this](QString*) { ++ended; if (reject) return false; editor = {}; return true; });
        controller.SetDeformerPublisher([this](const CModelDocument& document, const CModelEdit& command,
                                               bool forward, QString* error) {
            ++attempts;
            Controller::EditorState planned;
            if (!Controller::ReadDeformerEditorState(command, forward, planned, error)) return false;
            if (during) during();
            if (dirtyFailure) {
                command.runtime->before = QStringLiteral("bad-before"); command.runtime->after = QStringLiteral("bad-after");
                command.runtime->beforeEditor = QStringLiteral("bad-before-editor");
                command.runtime->afterEditor = QStringLiteral("bad-after-editor");
            }
            if (throwing) throw std::runtime_error("publisher failure");
            if (reject) { if (error) *error = "publisher rejected"; return false; }
            QByteArray candidate;
            if (!document.BuildRuntimeMoc(candidate, error)) return false;
            (forward ? command.runtime->before : command.runtime->after) = bytes;
            (forward ? command.runtime->after : command.runtime->before) = candidate;
            bytes.swap(candidate); editor = std::move(planned); ++accepted;
            return true;
        });
        controller.SetMeshPublisher([this](const CModelDocument& document, const CModelEdit&, bool, QString* error) {
            ++meshes;
            if (reject) return false;
            QByteArray candidate;
            if (!document.BuildRuntimeMoc(candidate, error)) return false;
            bytes.swap(candidate);
            return true;
        });
        controller.SetStructurePublisher([this](const CModelDocument& document, const CModelEdit&, bool,
                                                bool restore, const auto&, bool keep, auto& pose, QString* error) {
            ++combined;
            if (throwing) throw std::runtime_error("combined publisher failure");
            if (reject) { if (error) *error = "combined publisher rejected"; return false; }
            QByteArray candidate;
            if (!document.BuildRuntimeMoc(candidate, error)) return false;
            if (!keep) editor = {};
            if (pose.isEmpty()) pose = values(document);
            bytes.swap(candidate); restoredSession = restore;
            return true;
        });
    }
};
}

class DeformerStructureControllerTests : public QObject
{
    Q_OBJECT
    CModelDocument m_original;
private slots:
    void initTestCase()
    {
        QString error;
        QVERIFY2(m_original.Load(QString(TEST_RESOURCES) + "/Mark/Mark.moc3", &error), qPrintable(error));
    }

    void publicationFailureRetainsDocumentHistoryAndEditor_data()
    {
        QTest::addColumn<int>("fault");
        QTest::newRow("missing-publisher") << 0;
        QTest::newRow("false") << 1;
        QTest::newRow("throw") << 2;
    }
    void publicationFailureRetainsDocumentHistoryAndEditor()
    {
        QFETCH(int, fault);
        CModelDocument document = m_original; Controller controller; controller.SetDocument(&document);
        Runtime runtime; runtime.connect(controller); QString error; CModelEdit edit;
        QVERIFY(insert(document, "First", edit, &error)); QVERIFY(controller.Submit(edit, &error));
        QVERIFY(controller.Activate("First", "rotation", 0, values(document), &error));
        QVERIFY(insert(document, "Second", edit, &error)); QVERIFY(controller.Submit(edit, &error));
        CModelEdit history; QVERIFY(controller.History(true, history, &error));
        const auto before = document.AuthoringStructureState(); const auto revision = document.Revision();
        const auto bytes = runtime.bytes; const auto pose = controller.Pose();
        QVERIFY(insert(document, "Third", edit, &error));
        if (fault == 0) controller.SetDeformerPublisher({});
        runtime.reject = fault == 1; runtime.throwing = fault == 2; runtime.dirtyFailure = true;
        QVERIFY(!controller.Submit(edit, &error)); QVERIFY(!error.isEmpty());
        QCOMPARE(document.AuthoringStructureState(), before); QCOMPARE(document.Revision(), revision);
        QCOMPARE(runtime.bytes, bytes); QCOMPARE(controller.Target(), QString("First")); QCOMPARE(controller.Pose(), pose);
        QVERIFY(document.CanUndoEdit() && document.CanRedoEdit()); QCOMPARE(runtime.ended, 0);
    }

    void activeDeletedTargetRestoresFromHistory_data()
    {
        QTest::addColumn<bool>("warp"); QTest::newRow("rotation") << false; QTest::newRow("warp") << true;
    }
    void activeDeletedTargetRestoresFromHistory()
    {
        QFETCH(bool, warp);
        CModelDocument document = m_original; Controller controller; controller.SetDocument(&document);
        Runtime runtime; runtime.connect(controller); QString error; CModelEdit edit;
        QVERIFY(insert(document, "Selected", edit, &error, warp)); QVERIFY(controller.Submit(edit, &error));
        const QString type = warp ? "warp" : "rotation";
        QVERIFY(controller.Activate("Selected", type, 0, values(document), &error));
        const auto pose = controller.Pose(); const auto before = document.AuthoringStructureState();
        QVERIFY(remove(document, "Selected", edit, &error));
        runtime.reject = true;
        QVERIFY(!controller.Submit(edit, &error)); QVERIFY(controller.IsActive()); QCOMPARE(controller.Pose(), pose);
        QCOMPARE(document.AuthoringStructureState(), before); QCOMPARE(runtime.ended, 0);
        runtime.reject = false;
        QVERIFY2(controller.Submit(edit, &error), qPrintable(error));
        QVERIFY(!controller.IsActive()); QVERIFY(controller.Pose().isEmpty()); QCOMPARE(controller.Keyform(), -1);
        QVERIFY(runtime.editor.target.isEmpty()); QCOMPARE(runtime.ended, 0);
        CModelEdit history;
        QVERIFY2(controller.History(true, history, &error), qPrintable(error));
        QCOMPARE(controller.Target(), QString("Selected")); QCOMPARE(controller.Type(), type);
        QCOMPARE(controller.Keyform(), 0); QCOMPARE(controller.Pose(), pose); QCOMPARE(runtime.editor.target, controller.Target());
        QCOMPARE(document.AuthoringStructureState(), before);
        QVERIFY(controller.History(false, history, &error)); QVERIFY(!controller.IsActive()); QCOMPARE(runtime.ended, 0);
    }

    void insertionKeepsActiveObjectAndUndoCanRestoreNewSelection()
    {
        CModelDocument document = m_original; Controller controller; controller.SetDocument(&document);
        Runtime runtime; runtime.connect(controller); QString error; CModelEdit edit;
        QVERIFY(insert(document, "First", edit, &error)); QVERIFY(controller.Submit(edit, &error));
        QVERIFY(controller.Activate("First", "rotation", 0, values(document), &error));
        const auto pose = controller.Pose();
        QVERIFY(insert(document, "Second", edit, &error)); QVERIFY(controller.Submit(edit, &error));
        QCOMPARE(controller.Target(), QString("First")); QCOMPARE(controller.Pose(), pose);
        QCOMPARE(runtime.editor.target, QString("First"));
        QVERIFY(controller.Activate("Second", "rotation", 0, pose, &error));
        CModelEdit history;
        QVERIFY(controller.History(true, history, &error)); QVERIFY(!controller.IsActive());
        QVERIFY2(controller.History(false, history, &error), qPrintable(error));
        QCOMPARE(controller.Target(), QString("Second")); QCOMPARE(controller.Pose(), pose); QCOMPARE(runtime.ended, 0);
    }

    void historyFailuresRollBackAllSidecars_data()
    {
        QTest::addColumn<bool>("undo"); QTest::addColumn<bool>("throwing");
        for (bool undo : {false, true}) for (bool throwing : {false, true})
            QTest::newRow(qPrintable(QString("undo%1-throw%2").arg(undo).arg(throwing))) << undo << throwing;
    }
    void historyFailuresRollBackAllSidecars()
    {
        QFETCH(bool, undo); QFETCH(bool, throwing);
        CModelDocument document = m_original; Controller controller; controller.SetDocument(&document);
        Runtime runtime; runtime.connect(controller); QString error; CModelEdit edit;
        QVERIFY(insert(document, "Node", edit, &error)); QVERIFY(controller.Submit(edit, &error));
        if (!undo) QVERIFY(controller.History(true, edit, &error));
        QVERIFY(undo ? document.PeekUndoEdit(&edit) : document.PeekRedoEdit(&edit));
        QVERIFY(edit.runtime); const auto sidecar = *edit.runtime;
        const auto before = document.AuthoringStructureState(); const auto revision = document.Revision(); const auto bytes = runtime.bytes;
        runtime.dirtyFailure = true; runtime.throwing = throwing; runtime.reject = !throwing;
        CModelEdit output; output.name = "keep";
        QVERIFY(!controller.History(undo, output, &error)); QCOMPARE(output.name, QString("keep"));
        QCOMPARE(document.AuthoringStructureState(), before); QCOMPARE(document.Revision(), revision); QCOMPARE(runtime.bytes, bytes);
        QVERIFY(undo ? document.PeekUndoEdit(&edit) : document.PeekRedoEdit(&edit)); QVERIFY(same(*edit.runtime, sidecar));
        runtime.dirtyFailure = runtime.throwing = runtime.reject = false;
        QVERIFY2(controller.History(undo, output, &error), qPrintable(error));
    }

    void archivedEditorCorruptionIsRejectedBeforePublication_data()
    {
        QTest::addColumn<int>("kind"); for (int kind = 0; kind < 6; ++kind) QTest::newRow(qPrintable(QString::number(kind))) << kind;
    }
    void archivedEditorCorruptionIsRejectedBeforePublication()
    {
        QFETCH(int, kind);
        CModelDocument document = m_original; Controller controller; controller.SetDocument(&document);
        Runtime runtime; runtime.connect(controller); QString error; CModelEdit edit;
        QVERIFY(insert(document, "Selected", edit, &error)); QVERIFY(controller.Submit(edit, &error));
        QVERIFY(controller.Activate("Selected", "rotation", 0, values(document), &error));
        QVERIFY(remove(document, "Selected", edit, &error)); QVERIFY(controller.Submit(edit, &error));
        QVERIFY(document.PeekUndoEdit(&edit)); auto original = edit.runtime->beforeEditor;
        auto value = original.toMap();
        if (kind == 0) value["type"] = "part";
        if (kind == 1) value["keyform"] = 999;
        if (kind == 2) value["target"] = "missing";
        if (kind == 3) value["pose"] = QVariant::fromValue(QMap<QString, float>{});
        if (kind == 4) value["extra"] = true;
        if (kind == 5) { auto pose = value["pose"].value<QMap<QString,float>>(); pose.begin().value() = std::numeric_limits<float>::quiet_NaN(); value["pose"] = QVariant::fromValue(pose); }
        edit.runtime->beforeEditor = value;
        const auto before = document.AuthoringStructureState(); const int attempts = runtime.attempts;
        CModelEdit output; output.name = "keep";
        QVERIFY(!controller.History(true, output, &error)); QCOMPARE(output.name, QString("keep"));
        QCOMPARE(runtime.attempts, attempts); QCOMPARE(document.AuthoringStructureState(), before); QVERIFY(!controller.IsActive());
        edit.runtime->beforeEditor = original;
        QVERIFY2(controller.History(true, output, &error), qPrintable(error));
    }

    void publicationBlocksReentryAndPublisherReplacement()
    {
        CModelDocument document = m_original, other = m_original; Controller controller; controller.SetDocument(&document);
        Runtime runtime; runtime.connect(controller); QString error; CModelEdit edit;
        QVERIFY(insert(document, "Node", edit, &error));
        bool submitted = true, undone = true, restored = true, ended = true; int replacement = 0;
        runtime.during = [&] {
            CModelEdit output; QString nested;
            submitted = controller.Submit(edit, &nested); undone = controller.History(true, output, &nested);
            restored = controller.Restore(other, &nested); ended = controller.End(&nested);
            controller.SetDocument(&other);
            controller.SetDeformerPublisher([&](const auto&, const auto&, bool, QString*) { ++replacement; return false; });
        };
        QVERIFY2(controller.Submit(edit, &error), qPrintable(error));
        QVERIFY(!submitted && !undone && !restored && !ended); QCOMPARE(replacement, 0);
        QVERIFY(document.HasDeformerStructure()); QVERIFY(!other.HasDeformerStructure());
        runtime.during = {};
        QVERIFY(insert(document, "Next", edit, &error)); QVERIFY(controller.Submit(edit, &error)); QCOMPARE(replacement, 0);
    }

    void runtimeStateRollbackAlsoCoversRevisionChanges()
    {
        CModelDocument document = m_original; Controller controller; controller.SetDocument(&document);
        Runtime runtime; runtime.connect(controller); QString error; CModelEdit edit;
        QVERIFY(insert(document, "Node", edit, &error)); QVERIFY(controller.Submit(edit, &error));
        QVERIFY(document.PeekUndoEdit(&edit)); const auto sidecar = *edit.runtime;
        runtime.during = [&] { document.SetParameterValue(document.Parameters().first().id, .25f); };
        CModelEdit output; output.name = "keep";
        QVERIFY(!controller.History(true, output, &error)); QCOMPARE(output.name, QString("keep"));
        QVERIFY(document.HasDeformerStructure()); QVERIFY(document.CanUndoEdit()); QVERIFY(!document.CanRedoEdit());
        QVERIFY(document.PeekUndoEdit(&edit)); QVERIFY(same(*edit.runtime, sidecar));
    }

    void restorePublishesCombinedStateOnceAndFailureKeepsTheEditor()
    {
        CModelDocument document = m_original; Controller controller; controller.SetDocument(&document);
        Runtime runtime; runtime.connect(controller); QString error; CModelEdit edit;
        QVERIFY(insert(document, "Selected", edit, &error)); QVERIFY(controller.Submit(edit, &error));
        QVERIFY(controller.Activate("Selected", "rotation", 0, values(document), &error));
        const auto before = document.AuthoringStructureState(); const auto pose = controller.Pose();
        runtime.reject = true;
        QVERIFY(!controller.Restore(m_original, &error)); QCOMPARE(document.AuthoringStructureState(), before);
        QVERIFY(controller.IsActive()); QCOMPARE(controller.Pose(), pose); QCOMPARE(runtime.ended, 0);
        runtime.reject = false;
        const int calls = runtime.combined;
        QVERIFY(controller.Restore(m_original, &error)); QCOMPARE(runtime.combined, calls + 1); QVERIFY(runtime.restoredSession);
        QVERIFY(!document.HasDeformerStructure()); QVERIFY(!controller.IsActive()); QCOMPARE(runtime.ended, 0);
        CModelDocument withStructure = document;
        QVERIFY(insert(withStructure, "Restored", edit, &error)); QVERIFY(withStructure.ExecuteEdit(edit));
        QVERIFY(controller.Restore(withStructure, &error)); QCOMPARE(runtime.combined, calls + 2);
        QVERIFY(document.HasDeformerStructure()); QVERIFY(!controller.IsActive());
    }

    void mixedDeformerParameterAndMeshHistoryUsesOnePublisherPerStep()
    {
        CModelDocument document = m_original; Controller controller; controller.SetDocument(&document);
        Runtime runtime; runtime.connect(controller); QString error; CModelEdit edit;
        const auto original = document.AuthoringStructureState();
        QVERIFY(insert(document, "First", edit, &error)); QVERIFY(controller.Submit(edit, &error));
        CMocParameterStructureBuilder::CreateRequest parameter; parameter.parameter.id = "MixedParameter";
        QVERIFY(document.MakeCreateParameterEdit(parameter, {}, edit, &error)); QVERIFY(controller.Submit(edit, &error));
        const auto& sourceMesh = document.Drawables().first();
        CMocMeshStructureBuilder::MeshEdit mesh;
        mesh.artMeshId = sourceMesh.id; mesh.indices = document.DrawableIndices(sourceMesh.id);
        for (int index = 0; index < sourceMesh.vertexCount; ++index) {
            mesh.vertices.append({{index}, {1}}); mesh.oldToNew.append(index);
        }
        mesh.vertices.append({{0}, {1}});
        CMocMeshStructureBuilder::Request geometry; geometry.meshes.append(mesh);
        QVERIFY2(document.MakeMeshStructureEdit(geometry, edit, &error), qPrintable(error));
        QVERIFY(controller.Submit(edit, &error));
        QVERIFY(insert(document, "Last", edit, &error)); QVERIFY(controller.Submit(edit, &error));
        const auto final = document.AuthoringStructureState();
        const QList<CModelEdit::Field> backward{CModelEdit::DeformerStructure, CModelEdit::MeshStructure,
                                               CModelEdit::ParameterStructure, CModelEdit::DeformerStructure};
        for (auto field : backward) { QVERIFY2(controller.History(true, edit, &error), qPrintable(error)); QCOMPARE(edit.field, field); }
        QCOMPARE(document.AuthoringStructureState(), original);
        for (auto field = backward.crbegin(); field != backward.crend(); ++field) {
            QVERIFY2(controller.History(false, edit, &error), qPrintable(error)); QCOMPARE(edit.field, *field);
        }
        QCOMPARE(document.AuthoringStructureState(), final);
        QCOMPARE(runtime.accepted, 6); QCOMPARE(runtime.meshes, 3); QCOMPARE(runtime.combined, 3); QCOMPARE(runtime.ended, 0);
    }

    void locksAreCheckedAgainBeforePublication()
    {
        CModelDocument document = m_original; Controller controller; controller.SetDocument(&document);
        Runtime runtime; runtime.connect(controller); QString error; CModelEdit edit;
        QVERIFY(insert(document, "Selected", edit, &error)); QVERIFY(controller.Submit(edit, &error));
        QVERIFY(remove(document, "Selected", edit, &error));
        document.SetLocked("rotation", "Selected", true);
        const auto before = document.AuthoringStructureState(); const int attempts = runtime.attempts;
        QVERIFY(!controller.Submit(edit, &error)); QCOMPARE(runtime.attempts, attempts);
        QCOMPARE(document.AuthoringStructureState(), before);
        CModelEdit output; output.name = "keep";
        QVERIFY(!remove(document, "Selected", output, &error)); QCOMPARE(output.name, QString("keep"));
    }

    void removalDoesNotConfuseArtMeshWithTheSameId()
    {
        CModelDocument document = m_original; Controller controller; controller.SetDocument(&document);
        Runtime runtime; runtime.connect(controller); QString error; CModelEdit edit;
        QString id; int keyform = -1;
        for (const auto& mesh : document.Drawables()) if (mesh.keyforms.size() >= 3) {
            id = mesh.id; keyform = mesh.keyforms.size() - 1; break;
        }
        QVERIFY(!id.isEmpty());
        QVERIFY2(controller.Activate(id, "artmesh", keyform, values(document), &error), qPrintable(error));
        const auto pose = controller.Pose();
        QVERIFY2(insert(document, id, edit, &error), qPrintable(error));
        QVERIFY(controller.Submit(edit, &error));
        QCOMPARE(controller.Target(), id); QCOMPARE(controller.Type(), QString("artmesh"));
        QVERIFY(remove(document, id, edit, &error)); QVERIFY(controller.Submit(edit, &error));
        QCOMPARE(controller.Target(), id); QCOMPARE(controller.Type(), QString("artmesh"));
        QCOMPARE(controller.Pose(), pose); QCOMPARE(controller.Keyform(), keyform);
        QCOMPARE(runtime.editor.type, QString("artmesh")); QCOMPARE(runtime.editor.keyform, keyform);
    }

    void draggingPreventsStructureAndHistoryPublication()
    {
        CModelDocument document = m_original; Controller controller; controller.SetDocument(&document);
        Runtime runtime; runtime.connect(controller); QString error; CModelEdit edit;
        QVERIFY(insert(document, "Selected", edit, &error)); QVERIFY(controller.Submit(edit, &error));
        QVERIFY(controller.Activate("Selected", "rotation", 0, values(document), &error));
        QVERIFY(insert(document, "Next", edit, &error));
        QVERIFY2(controller.BeginDeformerDrag(Controller::DeformerHandle::RotationOrigin, {}, &error), qPrintable(error));
        const auto before = document.AuthoringStructureState(); const int attempts = runtime.attempts;
        CModelEdit output;
        QVERIFY(!controller.Submit(edit, &error)); QVERIFY(!controller.History(true, output, &error));
        QVERIFY(!controller.Restore(m_original, &error)); QVERIFY(controller.IsDragging());
        QCOMPARE(document.AuthoringStructureState(), before); QCOMPARE(runtime.attempts, attempts);
        controller.FinishDrag();
        QVERIFY(controller.Submit(edit, &error)); QCOMPARE(controller.Target(), QString("Selected"));
    }

    void malformedAndStaleRequestsFailBeforePublisher_data()
    {
        QTest::addColumn<int>("kind"); for (int kind = 0; kind < 4; ++kind) QTest::newRow(qPrintable(QString::number(kind))) << kind;
    }
    void malformedAndStaleRequestsFailBeforePublisher()
    {
        QFETCH(int, kind);
        CModelDocument document = m_original; Controller controller; controller.SetDocument(&document);
        Runtime runtime; runtime.connect(controller); QString error; CModelEdit edit;
        QVERIFY(insert(document, "Node", edit, &error));
        if (kind == 0) edit.deformerRequest.reset();
        if (kind == 1) edit.field = CModelEdit::MeshStructure;
        if (kind == 2) {
            auto request = *edit.deformerRequest;
            request.sourceDigest = QByteArray(32, 'X');
            edit.deformerRequest = std::make_shared<const CDeformerPreserveRequest>(std::move(request));
        }
        if (kind == 3) QVERIFY(document.SetParameterValue(document.Parameters().first().id, .25f));
        const auto before = document.AuthoringStructureState();
        QVERIFY(!controller.Submit(edit, &error)); QVERIFY(!error.isEmpty());
        QCOMPARE(runtime.attempts, 0); QCOMPARE(document.AuthoringStructureState(), before); QVERIFY(!document.CanUndoEdit());
    }

    void readerRejectsWrongFieldAndPreservesItsOutput()
    {
        Controller::EditorState result{"keep", "warp", 3, {{"P", .5f}}}; QString error;
        CModelEdit command; command.runtime = std::make_shared<CModelEditRuntimeState>();
        QVERIFY(!Controller::ReadDeformerEditorState(command, true, result, &error));
        QCOMPARE(result.target, QString("keep")); QCOMPARE(result.keyform, 3);
        command.field = CModelEdit::DeformerStructure;
        command.runtime->afterEditor = QVariantMap{{"target", QString()}, {"type", QString()}, {"keyform", -1},
            {"pose", QVariant::fromValue(QMap<QString,float>{})}};
        QVERIFY(Controller::ReadDeformerEditorState(command, true, result, &error));
        QVERIFY(result.target.isEmpty()); QCOMPARE(result.keyform, -1); QVERIFY(result.pose.isEmpty());
    }
};
QTEST_GUILESS_MAIN(DeformerStructureControllerTests)
#include "deformer_structure_controller_tests.moc"
