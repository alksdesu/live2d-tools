#include "live2dstruct/cmodeldocument.h"
#include "live2dstruct/cmodelsession.h"
#include "live2dstruct/cmocmeshstructurebuilder.h"
#include "deformer_replay_fixture.h"
#include <QtTest>
#include <QJsonDocument>
#include <QJsonArray>
#include <QTemporaryDir>

using namespace live2dstruct;
using Raw = CMocDeformerStructureBuilder;
using Parameters = CMocParameterStructureBuilder;
using Meshes = CMocMeshStructureBuilder;
namespace Fixture = DeformerReplayFixture;
namespace {
QByteArray preview(const CModelDocument& document)
{
    QByteArray bytes;
    document.BuildPreviewMoc(bytes);
    return bytes;
}
CDeformerPreserveRequest anchored(const CModelDocument& document,
                                  decltype(CDeformerStructureRequest::operation) operation)
{
    return {std::move(operation), document.SourceDigest(), QCryptographicHash::hash(preview(document), QCryptographicHash::Sha256)};
}
Raw::InsertRequest insertion(const CModelDocument& document, bool warp, const QString& id = "DocumentControl")
{
    auto result = Fixture::rootMesh(preview(document), warp);
    result.id = id;
    return result;
}
CModelSession capture(const CModelDocument& document)
{
    CModelSession result;
    result.sourceMoc = document.SourcePath(); result.sourceDigest = document.SourceDigest();
    result.structure = document.ParameterStructure(); result.meshStructure = document.HasMeshStructure();
    result.deformerStructure = document.HasDeformerStructure();
    if (result.deformerStructure) result.sourceDeformerIds = document.SourceDeformerIds();
    result.version = result.deformerStructure ? 6 : result.meshStructure ? 5 : result.structure.columns.isEmpty() && result.structure.counts.isEmpty() ? 1 : 2;
    for (auto it = document.ParameterValues().cbegin(); it != document.ParameterValues().cend(); ++it) result.parameters.insert(it.key(), it.value());
    result.parameterMetadata = document.ParameterMetadataOverrides(); result.visibility = document.VisibilityState();
    for (const auto& key : document.LockedKeys()) result.locked.insert(key, true);
    result.drawableOpacity = document.DrawableOpacityOverrides(); result.drawableDrawOrder = document.DrawableDrawOrderOverrides();
    result.drawableKeyformPositions = document.DrawableKeyformPositionOverrides(); result.drawableIndices = document.DrawableIndexOverrides();
    result.drawableMaskIndices = document.DrawableMaskIndexOverrides(); result.drawableMaskInverted = document.DrawableMaskInvertedOverrides();
    result.deformerParentIndices = document.DeformerParentOverrides(); result.deformerRotationKeyforms = document.DeformerRotationKeyformOverrides();
    result.deformerWarpKeyforms = document.DeformerWarpKeyformOverrides();
    return result;
}
Meshes::MeshEdit identity(const CModelDocument& document, const QString& id)
{
    Meshes::MeshEdit result; result.artMeshId = id;
    for (const auto& mesh : document.Drawables()) if (mesh.id == id) {
        for (int index = 0; index < mesh.vertexCount; ++index) { result.vertices.append({{index},{1}}); result.oldToNew.append(index); }
        result.indices = document.DrawableIndices(id); break;
    }
    return result;
}
QString typeOf(const CModelDeformer& node) { return node.type == CModelDeformer::Warp ? "warp" : "rotation"; }
QString key(const Raw::Owner& owner)
{
    return QString(owner.type == Raw::Owner::ArtMesh ? "artmesh|" : owner.type == Raw::Owner::Warp ? "warp|" : "rotation|") + owner.id;
}
}

class DeformerStructureDocumentTests : public QObject
{
    Q_OBJECT
    CModelDocument m_original;

    void verifyRestored(const CModelDocument& expected)
    {
        const auto session = capture(expected); const auto bytes = session.ToJson(); QVERIFY(!bytes.isEmpty());
        CModelSession decoded; QString error;
        QVERIFY2(CModelSession::FromJson(bytes, &decoded, &error), qPrintable(error));
        QCOMPARE(decoded.deformerStructure, expected.HasDeformerStructure());
        QCOMPARE(decoded.sourceDeformerIds, expected.HasDeformerStructure() ? expected.SourceDeformerIds() : QSet<QString>{});
        for (const CModelDocument* loaded : {static_cast<const CModelDocument*>(&m_original), &expected}) {
            CModelDocument restored;
            QVERIFY2(decoded.RestoreDocument(*loaded, restored, &error), qPrintable(error));
            QCOMPARE(restored.AuthoringStructureState(), expected.AuthoringStructureState());
            QCOMPARE(restored.DeformerStructureState(), expected.DeformerStructureState());
            QCOMPARE(restored.SourceDeformerIds(), expected.SourceDeformerIds());
            QCOMPARE(restored.SourceBytes(), m_original.SourceBytes()); QCOMPARE(restored.SourceDigest(), m_original.SourceDigest());
            QCOMPARE(preview(restored), preview(expected));
            QByteArray actual, wanted; QVERIFY(restored.BuildRuntimeMoc(actual, &error)); QVERIFY(expected.BuildRuntimeMoc(wanted, &error)); QCOMPARE(actual, wanted);
            QVERIFY(!restored.CanUndoEdit()); QVERIFY(!restored.CanRedoEdit());
        }
    }
    void assertUnchanged(const CModelDocument& document, const QVariantMap& state, quint64 revision,
                         const QByteArray& moc, bool undo, bool redo)
    {
        QCOMPARE(document.AuthoringStructureState(), state); QCOMPARE(document.Revision(), revision);
        QCOMPARE(preview(document), moc); QCOMPARE(document.CanUndoEdit(), undo); QCOMPARE(document.CanRedoEdit(), redo);
    }

private slots:
    void initTestCase()
    {
        QString error; QVERIFY2(m_original.Load(QString(TEST_RESOURCES) + "/Mark/Mark.moc3", &error), qPrintable(error));
    }
    void insertDeleteHistoryAndVersionSixSession_data()
    {
        QTest::addColumn<bool>("warp"); QTest::newRow("warp") << true; QTest::newRow("rotation") << false;
    }
    void insertDeleteHistoryAndVersionSixSession()
    {
        QFETCH(bool, warp); auto document = m_original; const auto request = insertion(document, warp);
        QVERIFY(!request.children.isEmpty()); QString error; CModelEdit add;
        const auto start = document.AuthoringStructureState(); const auto revision = document.Revision();
        QVERIFY2(document.MakeDeformerStructureEdit(anchored(document, request), add, &error), qPrintable(error));
        QCOMPARE(document.AuthoringStructureState(), start); QCOMPARE(document.Revision(), revision); QVERIFY(!document.CanUndoEdit());
        QCOMPARE(add.field, CModelEdit::DeformerStructure); QVERIFY(add.deformerRequest); QVERIFY(!add.coalesce);
        QVERIFY(document.ExecuteEdit(add)); QVERIFY(document.HasDeformerStructure());
        QCOMPARE(document.Deformers().size(), m_original.Deformers().size() + 1);
        QCOMPARE(document.SourceDeformerIds(), m_original.SourceDeformerIds()); QVERIFY(!document.SourceDeformerIds().contains(request.id));
        const auto added = document.AuthoringStructureState(); verifyRestored(document);
        const auto session = capture(document); const auto json = QJsonDocument::fromJson(session.ToJson()).object();
        QCOMPARE(json["version"].toInt(), 6); QVERIFY(json["deformerStructure"].toBool()); QVERIFY(json["sourceDeformerIds"].isArray());
        QTemporaryDir directory; QVERIFY(directory.isValid());
        QVERIFY2(session.Save(directory.filePath("deformer.session"), &error), qPrintable(error));
        CModelSession reopened; QVERIFY(CModelSession::Load(directory.filePath("deformer.session"), &reopened, &error));
        QCOMPARE(reopened.structure, session.structure); QCOMPARE(reopened.sourceDeformerIds, session.sourceDeformerIds);
        CModelEdit remove; QVERIFY2(document.MakeDeformerStructureEdit(anchored(document, Raw::RemoveRequest{request.id}), remove, &error), qPrintable(error));
        QVERIFY(document.ExecuteEdit(remove)); const auto removed = document.AuthoringStructureState();
        QCOMPARE(document.Deformers().size(), m_original.Deformers().size()); verifyRestored(document);
        QVERIFY(document.UndoEdit()); QCOMPARE(document.AuthoringStructureState(), added);
        QVERIFY(document.UndoEdit()); QCOMPARE(document.AuthoringStructureState(), start); QVERIFY(!document.HasDeformerStructure()); QVERIFY(!document.CanUndoEdit());
        QVERIFY(document.RedoEdit()); QCOMPARE(document.AuthoringStructureState(), added);
        QVERIFY(document.RedoEdit()); QCOMPARE(document.AuthoringStructureState(), removed); QVERIFY(!document.CanRedoEdit());
        QCOMPARE(document.SourceBytes(), m_original.SourceBytes()); QCOMPARE(document.SourceDigest(), m_original.SourceDigest());
    }

    void parameterMeshAndScalarEditsShareOneHistory()
    {
        auto document = m_original; QString error;
        QVector<QVariantMap> states{document.AuthoringStructureState()}; QVector<QByteArray> previews{preview(document)};
        const auto remember = [&] { states.append(document.AuthoringStructureState()); previews.append(preview(document)); };
        const QString mesh = document.Drawables().front().id;
        CModelEdit change{mesh, "artmesh", CModelEdit::Opacity, 1.f, .625f, -1, false};
        change.before = document.DrawableOpacity(document.Drawables().front()); QVERIFY(document.ExecuteEdit(change)); remember();
        const auto position = document.DrawableKeyformPositions(mesh, 0); auto shifted = position; shifted[0] += .125f;
        change = {mesh, "artmesh", CModelEdit::KeyformPositions, QVariant::fromValue(position), QVariant::fromValue(shifted), 0, false};
        QVERIFY(document.ExecuteEdit(change)); remember();
        const auto request = insertion(document, false);
        QVERIFY2(document.MakeDeformerStructureEdit(anchored(document, request), change, &error), qPrintable(error)); QVERIFY(document.ExecuteEdit(change)); remember();
        Parameters::CreateRequest parameter; parameter.parameter.id = "ParamWithDeformer";
        QVERIFY2(document.MakeCreateParameterEdit(parameter, QStringLiteral("结构参数"), change, &error), qPrintable(error)); QVERIFY(document.ExecuteEdit(change)); remember();
        QVERIFY2(document.MakeBindParameterEdit(parameter.parameter.id, {{{Parameters::Owner::ArtMesh, mesh},{-1,1}}}, change, &error), qPrintable(error));
        QVERIFY(document.ExecuteEdit(change)); remember();
        auto geometry = identity(document, mesh); geometry.vertices.append({{0,1},{.5,.5}});
        Meshes::Request meshRequest; meshRequest.meshes.append(geometry);
        QVERIFY2(document.MakeMeshStructureEdit(meshRequest, change, &error), qPrintable(error)); QVERIFY(document.ExecuteEdit(change)); remember();
        QVERIFY(document.HasMeshStructure()); QVERIFY(document.HasDeformerStructure());
        QVERIFY2(document.MakeDeformerStructureEdit(anchored(document, Raw::RemoveRequest{request.id}), change, &error), qPrintable(error));
        QVERIFY(document.ExecuteEdit(change)); remember(); verifyRestored(document);
        QCOMPARE(document.DrawableOpacity(document.Drawables().front()), .625f);
        for (int index = states.size() - 1; index > 0; --index) {
            QVERIFY2(document.UndoEdit(), qPrintable(QString("undo %1").arg(index)));
            QCOMPARE(document.AuthoringStructureState(), states[index - 1]); QCOMPARE(preview(document), previews[index - 1]);
        }
        QVERIFY(!document.CanUndoEdit());
        for (int index = 1; index < states.size(); ++index) {
            QVERIFY2(document.RedoEdit(), qPrintable(QString("redo %1").arg(index)));
            QCOMPARE(document.AuthoringStructureState(), states[index]); QCOMPARE(preview(document), previews[index]);
        }
        QVERIFY(!document.CanRedoEdit()); verifyRestored(document);
    }

    void legacySessionClearsDirectoryFlagAndSourceIdentity_data()
    {
        QTest::addColumn<int>("version"); for (int version = 1; version <= 6; ++version) QTest::newRow(qPrintable(QString::number(version))) << version;
    }
    void legacySessionClearsDirectoryFlagAndSourceIdentity()
    {
        QFETCH(int, version); auto document = m_original; CModelEdit edit; QString error;
        QVERIFY(document.MakeDeformerStructureEdit(anchored(document, insertion(document, false)), edit, &error)); QVERIFY(document.ExecuteEdit(edit));
        auto old = capture(m_original); old.version = version;
        CModelSession parsed; QVERIFY2(CModelSession::FromJson(old.ToJson(), &parsed, &error), qPrintable(error));
        QVERIFY(parsed.RestoreDocument(document, document, &error));
        QVERIFY(!document.HasDeformerStructure()); QCOMPARE(document.SourceDeformerIds(), m_original.SourceDeformerIds());
        QCOMPARE(document.AuthoringStructureState(), m_original.AuthoringStructureState());
        QCOMPARE(document.Deformers().size(), m_original.Deformers().size()); QVERIFY(!document.CanUndoEdit());
    }

    void sourceIdentitySurvivesDeleteAndRecreateSameId()
    {
        const auto source = Fixture::fixedRotation(m_original.SourceBytes(), "Rotation7", 1, 0);
        QVERIFY(!source.isEmpty()); CModelDocument document; QString error;
        QVERIFY(document.LoadBytes(source, "fixed-rotation.moc3", &error));
        const auto sourceIds = document.SourceDeformerIds(); QVERIFY(sourceIds.contains("Rotation7"));
        CModelEdit edit; QVERIFY2(document.MakeDeformerStructureEdit(anchored(document, Raw::RemoveRequest{"Rotation7"}), edit, &error), qPrintable(error));
        QVERIFY(document.ExecuteEdit(edit)); QVERIFY(!document.SourceDeformerIds().contains("Rotation7"));
        const auto removed = document.AuthoringStructureState();
        Raw::InsertRequest raw; raw.id = "Rotation7"; raw.shape = Raw::Rotation{};
        QVERIFY2(document.MakeDeformerStructureEdit(anchored(document, raw), edit, &error), qPrintable(error)); QVERIFY(document.ExecuteEdit(edit));
        QVERIFY(!document.SourceDeformerIds().contains("Rotation7"));
        auto session = capture(document); CModelSession decoded; QVERIFY(CModelSession::FromJson(session.ToJson(), &decoded, &error));
        CModelDocument restored; QVERIFY2(decoded.RestoreDocument(document, restored, &error), qPrintable(error));
        QVERIFY(!restored.SourceDeformerIds().contains("Rotation7")); QCOMPARE(restored.AuthoringStructureState(), document.AuthoringStructureState());
        QVERIFY(document.UndoEdit()); QCOMPARE(document.AuthoringStructureState(), removed);
        QVERIFY(document.UndoEdit()); QCOMPARE(document.SourceDeformerIds(), sourceIds); QVERIFY(!document.HasDeformerStructure());
        QVERIFY(document.RedoEdit()); QVERIFY(document.RedoEdit()); QVERIFY(!document.SourceDeformerIds().contains("Rotation7"));
    }

    void staleAndTamperedCommandsAreAtomic_data()
    {
        QTest::addColumn<int>("fault");
        const char* names[]{"source-digest","authoring-digest","stale-command","missing-request","wrong-name","wrong-type","extra-parameter-request","scalar-side-effect","source-identity-side-effect","changed-operation","parameter-field-spoof","mesh-field-spoof"};
        for (int index = 0; index < int(std::size(names)); ++index) QTest::newRow(names[index]) << index;
    }
    void staleAndTamperedCommandsAreAtomic()
    {
        QFETCH(int, fault); auto document = m_original; QString error; CModelEdit edit;
        auto request = anchored(document, insertion(document, false));
        if (fault == 0) request.sourceDigest.fill('x');
        if (fault == 1) request.expectedAuthoringDigest.fill('x');
        if (fault < 2) {
            edit.name = "sentinel";
            const auto state = document.AuthoringStructureState(); const auto revision = document.Revision(); const auto moc = preview(document);
            QVERIFY(!document.MakeDeformerStructureEdit(request, edit, &error)); QVERIFY(!error.isEmpty()); QCOMPARE(edit.name, QString("sentinel"));
            assertUnchanged(document, state, revision, moc, false, false); return;
        }
        QVERIFY2(document.MakeDeformerStructureEdit(request, edit, &error), qPrintable(error));
        if (fault == 2) QVERIFY(document.SetDrawableOpacity(document.Drawables().front().id, .3f));
        if (fault == 3) edit.deformerRequest.reset();
        if (fault == 4) edit.name = "Other";
        if (fault == 5) edit.type = "parameter";
        if (fault == 6) edit.parameterChange = CModelEdit::ParameterChange::Create;
        if (fault == 7) {
            auto state = edit.after.toMap(); auto opacity = state["opacity"].value<QHash<QString,float>>(); opacity[document.Drawables().front().id] = .2f;
            state["opacity"] = QVariant::fromValue(opacity); edit.after = state;
        }
        if (fault == 8) { auto state = edit.after.toMap(); state["sourceDeformerIds"] = QVariant::fromValue(QSet<QString>{}); edit.after = state; }
        if (fault == 9) { auto changed = *edit.deformerRequest; std::get<Raw::InsertRequest>(changed.operation).shape = Raw::Warp{}; edit.deformerRequest = std::make_shared<const CDeformerPreserveRequest>(changed); }
        if (fault == 10 || fault == 11) {
            edit.deformerRequest.reset(); edit.field = fault == 10 ? CModelEdit::ParameterStructure : CModelEdit::MeshStructure;
            edit.type = fault == 10 ? "parameter" : "mesh";
        }
        const auto state = document.AuthoringStructureState(); const auto revision = document.Revision(); const auto moc = preview(document);
        QVERIFY(!document.ExecuteEdit(edit)); assertUnchanged(document, state, revision, moc, false, false);
    }

    void locksAndRemovedVisibilityAreHandled()
    {
        auto document = m_original; const auto request = insertion(document, false); QVERIFY(!request.children.isEmpty());
        const auto ownerKey = key(request.children.front()); const int separator = ownerKey.indexOf('|');
        document.SetLocked(ownerKey.left(separator), ownerKey.mid(separator + 1), true);
        CModelEdit edit; edit.name = "keep"; QString error;
        QVERIFY(!document.MakeDeformerStructureEdit(anchored(document, request), edit, &error)); QCOMPARE(edit.name, QString("keep"));
        document.ClearLocked();
        if (request.partIndex >= 0) {
            document.SetLocked("part", document.Parts()[request.partIndex].id, true);
            QVERIFY(!document.MakeDeformerStructureEdit(anchored(document, request), edit, &error)); document.ClearLocked();
        }
        const QString unaffected = document.Drawables().back().id; document.SetLocked("artmesh", unaffected, true);
        QVERIFY2(document.MakeDeformerStructureEdit(anchored(document, request), edit, &error), qPrintable(error)); QVERIFY(document.ExecuteEdit(edit));
        QVERIFY(document.SetDeformerVisible(request.id, false));
        document.SetLocked("rotation", request.id, true);
        QVERIFY(!document.MakeDeformerStructureEdit(anchored(document, Raw::RemoveRequest{request.id}), edit, &error));
        document.SetLocked("rotation", request.id, false);
        const auto before = document.AuthoringStructureState();
        QVERIFY2(document.MakeDeformerStructureEdit(anchored(document, Raw::RemoveRequest{request.id}), edit, &error), qPrintable(error)); QVERIFY(document.ExecuteEdit(edit));
        QVERIFY(!document.VisibilityState().contains("rotation|" + request.id)); QVERIFY(!document.LockedKeys().contains("rotation|" + request.id));
        QVERIFY(document.IsLocked("artmesh", unaffected)); QVERIFY(document.UndoEdit()); QCOMPARE(document.AuthoringStructureState(), before);
        QVERIFY(!document.VisibilityState().value("rotation|" + request.id, true)); QVERIFY(document.RedoEdit());
    }

    void versionSixMarkersAndIdentityAreValidated_data()
    {
        QTest::addColumn<int>("fault"); for (int fault = 0; fault < 10; ++fault) QTest::newRow(qPrintable(QString::number(fault))) << fault;
    }
    void versionSixMarkersAndIdentityAreValidated()
    {
        QFETCH(int, fault); auto document = m_original; CModelEdit edit; QString error;
        QVERIFY(document.MakeDeformerStructureEdit(anchored(document, insertion(document, false)), edit, &error)); QVERIFY(document.ExecuteEdit(edit));
        auto json = QJsonDocument::fromJson(capture(document).ToJson()).object();
        if (fault == 0) json.remove("deformerStructure");
        if (fault == 1) json["deformerStructure"] = "true";
        if (fault == 2) json.remove("sourceDeformerIds");
        if (fault == 3) json["sourceDeformerIds"] = QJsonArray{1};
        if (fault == 4) json["sourceDeformerIds"] = QJsonArray{"Rotation7","Rotation7"};
        if (fault == 5) json["sourceDeformerIds"] = QJsonArray{""};
        if (fault == 6) json["deformerStructure"] = false;
        if (fault == 7) json["version"] = 5;
        if (fault == 8) json["sourceDeformerIds"] = QJsonArray{"NotFromSource"};
        if (fault == 9) json["sourceDeformerIds"] = QJsonArray{"DocumentControl"};
        CModelSession decoded; decoded.sourceMoc = "sentinel";
        if (fault < 8) { QVERIFY(!CModelSession::FromJson(QJsonDocument(json).toJson(), &decoded, &error)); QCOMPARE(decoded.sourceMoc, QString("sentinel")); return; }
        QVERIFY(CModelSession::FromJson(QJsonDocument(json).toJson(), &decoded, &error));
        const auto state = document.AuthoringStructureState(); const auto revision = document.Revision(); const auto moc = preview(document);
        QVERIFY(!decoded.RestoreDocument(document, document, &error)); assertUnchanged(document, state, revision, moc, true, false);
    }

    void invalidStructureSetterPreservesHistory_data()
    {
        QTest::addColumn<int>("fault");
        const char* names[]{"unknown-source-id","new-node-as-source","source-type","source-part","mesh-id"};
        for (int index = 0; index < int(std::size(names)); ++index) QTest::newRow(names[index]) << index;
    }
    void invalidStructureSetterPreservesHistory()
    {
        QFETCH(int, fault); auto edited = m_original; CModelEdit edit; QString error;
        QVERIFY(edited.MakeDeformerStructureEdit(anchored(edited, insertion(edited, false)), edit, &error)); QVERIFY(edited.ExecuteEdit(edit));
        auto structure = edited.ParameterStructure(); auto sourceIds = edited.SourceDeformerIds();
        if (fault == 0) sourceIds.insert("Missing");
        if (fault == 1) sourceIds.insert("DocumentControl");
        if (fault == 2) qToLittleEndian(qint32(0), structure.columns[0x84].data());
        if (fault == 3) qToLittleEndian(qint32(-1), structure.columns[0x7c].data());
        if (fault == 4) {
            auto names = Fixture::RawColumns(m_original.SourceBytes()).column("art_meshes.ids");
            names.replace(0, 64, QByteArray("ChangedMesh").leftJustified(64, '\0'));
            structure.columns[0xc4] = names;
        }
        auto target = m_original; const auto mesh = target.Drawables().front(); const auto parameter = target.Parameters().front();
        const float oldValue = target.ParameterValue(parameter.id);
        const float nextValue = oldValue == parameter.maximum ? parameter.minimum : parameter.maximum;
        QVERIFY(target.ExecuteEdit({mesh.id, "artmesh", CModelEdit::Opacity, target.DrawableOpacity(mesh), .25f, -1, false}));
        QVERIFY(target.ExecuteEdit({parameter.id, "parameter", CModelEdit::ParameterValue, oldValue, nextValue, -1, false}));
        QVERIFY(target.UndoEdit());
        const auto state = target.AuthoringStructureState(); const auto revision = target.Revision(); const auto bytes = preview(target);
        QVERIFY(!target.SetDeformerStructure(structure, false, sourceIds, &error)); QVERIFY(!error.isEmpty());
        assertUnchanged(target, state, revision, bytes, true, true);
        QVERIFY(target.RedoEdit()); QCOMPARE(target.ParameterValue(parameter.id), nextValue);
        QVERIFY(target.UndoEdit()); QCOMPARE(target.AuthoringStructureState(), state);
        QVERIFY(target.UndoEdit()); QCOMPARE(target.AuthoringStructureState(), m_original.AuthoringStructureState());
    }

    void structuralSessionUpgradesStoredVersionWithoutDroppingIdentity()
    {
        auto document = m_original; CModelEdit edit; QString error;
        QVERIFY(document.MakeDeformerStructureEdit(anchored(document, insertion(document, false)), edit, &error)); QVERIFY(document.ExecuteEdit(edit));
        for (int version = 1; version <= 6; ++version) {
            auto session = capture(document); session.version = version;
            const auto bytes = session.ToJson(); QVERIFY(!bytes.isEmpty());
            QCOMPARE(QJsonDocument::fromJson(bytes).object()["version"].toInt(), 6); QCOMPARE(session.version, version);
            CModelSession decoded; QVERIFY(CModelSession::FromJson(bytes, &decoded, &error));
            QVERIFY(decoded.deformerStructure); QCOMPARE(decoded.sourceDeformerIds, document.SourceDeformerIds());
            QCOMPARE(decoded.structure, document.ParameterStructure());
        }
    }

    void removalRenumbersSourceParentIdentities()
    {
        const auto bytes = Fixture::fixedRotation(m_original.SourceBytes(), "Rotation7", 1, 0);
        CModelDocument document; QString error; QVERIFY(document.LoadBytes(bytes, "renumber.moc3", &error));
        const auto before = document.Deformers(); int removed = -1;
        for (int index = 0; index < before.size(); ++index) if (before[index].id == "Rotation7") removed = index;
        QVERIFY(removed >= 0); CModelEdit edit;
        QVERIFY2(document.MakeDeformerStructureEdit(anchored(document, Raw::RemoveRequest{"Rotation7"}), edit, &error), qPrintable(error)); QVERIFY(document.ExecuteEdit(edit));
        QHash<QString,int> indices; for (int index = 0; index < document.Deformers().size(); ++index) indices.insert(document.Deformers()[index].id, index);
        int shifted = 0;
        for (const auto& node : before) {
            if (node.id == "Rotation7") continue;
            const int parent = node.parentDeformerIndex;
            const int expected = parent < 0 || parent == removed ? -1 : indices.value(before[parent].id, -2);
            QCOMPARE(document.SourceDeformerParentIndex(node.id), expected);
            if (parent > removed) ++shifted;
        }
        QVERIFY(shifted > 0); const auto session = capture(document); CModelDocument restored;
        QVERIFY2(session.RestoreDocument(document, restored, &error), qPrintable(error));
        for (const auto& node : document.Deformers()) QCOMPARE(restored.SourceDeformerParentIndex(node.id), document.SourceDeformerParentIndex(node.id));
    }
};

QTEST_GUILESS_MAIN(DeformerStructureDocumentTests)
#include "deformer_structure_document_tests.moc"
