#include "live2dstruct/cmodeldocument.h"
#include "live2dstruct/cmodelsession.h"
#include "deformer_replay_fixture.h"
#include <QtTest>
#include <QJsonDocument>
#include <QJsonArray>
#include <QTemporaryDir>

using namespace live2dstruct;
using Organization = CParameterOrganization;
namespace {
QStringList ids(const CModelDocument& document)
{
    QStringList result; for (const auto& parameter : document.Parameters()) result.append(parameter.id); return result;
}
Organization::State organization(const CModelDocument& document)
{
    Organization::State state; state.configured = true;
    state.nodes.append({Organization::Node::Folder, "face", {}, "Face", "#aaBBcc"});
    const auto parameters = ids(document);
    for (int index = 0; index < parameters.size(); ++index)
        state.nodes.append({Organization::Node::Parameter, parameters[index], index < 2 ? "face" : QString(), {}, index == 0 ? "#010203" : QString()});
    if (parameters.size() >= 2) state.pairs.append({parameters[0], parameters[1], true, true});
    return state;
}
QByteArray preview(const CModelDocument& document)
{
    QByteArray bytes; document.BuildPreviewMoc(bytes); return bytes;
}
CModelSession capture(const CModelDocument& document)
{
    CModelSession session; session.sourceMoc = document.SourcePath(); session.sourceDigest = document.SourceDigest();
    session.structure = document.ParameterStructure(); session.meshStructure = document.HasMeshStructure();
    session.deformerStructure = document.HasDeformerStructure();
    if (session.deformerStructure) session.sourceDeformerIds = document.SourceDeformerIds();
    session.parameterOrganization = document.ParameterOrganization();
    session.parameterMetadata = document.ParameterMetadataOverrides();
    for (auto value = document.ParameterValues().cbegin(); value != document.ParameterValues().cend(); ++value) session.parameters.insert(value.key(), value.value());
    session.visibility = document.VisibilityState(); for (const auto& lock : document.LockedKeys()) session.locked[lock] = true;
    session.drawableOpacity = document.DrawableOpacityOverrides(); session.drawableDrawOrder = document.DrawableDrawOrderOverrides();
    session.drawableKeyformPositions = document.DrawableKeyformPositionOverrides(); session.drawableIndices = document.DrawableIndexOverrides();
    session.drawableMaskIndices = document.DrawableMaskIndexOverrides(); session.drawableMaskInverted = document.DrawableMaskInvertedOverrides();
    session.deformerParentIndices = document.DeformerParentOverrides(); session.deformerRotationKeyforms = document.DeformerRotationKeyformOverrides();
    session.deformerWarpKeyforms = document.DeformerWarpKeyformOverrides(); return session;
}
}
class ParameterOrganizationDocumentTests : public QObject
{
    Q_OBJECT
    CModelDocument m_original;
private slots:
    void initTestCase()
    {
        QString error; QVERIFY2(m_original.Load(QString(TEST_RESOURCES) + "/Mark/Mark.moc3", &error), qPrintable(error));
    }
    void organizationHasIndependentUndoAndLeavesMocBytesUntouched()
    {
        auto document = m_original; const auto before = preview(document); QByteArray runtimeBefore;
        QVERIFY(document.BuildRuntimeMoc(runtimeBefore)); const auto revision = document.Revision();
        auto state = organization(document); CModelEdit edit; QString error;
        QVERIFY(document.MakeParameterOrganizationEdit(state, edit, &error)); QCOMPARE(document.Revision(), revision);
        QCOMPARE(edit.field, CModelEdit::ParameterOrganization); QVERIFY(!edit.coalesce); QVERIFY(!edit.runtime);
        QVERIFY(document.ExecuteEdit(edit)); QCOMPARE(document.ParameterOrganization().nodes[0].color, QString("#AABBCC"));
        const auto accepted = document.ParameterOrganization(); QCOMPARE(preview(document), before);
        QByteArray runtime; QVERIFY(document.BuildRuntimeMoc(runtime)); QCOMPARE(runtime, runtimeBefore);
        QCOMPARE(document.SourceBytes(), m_original.SourceBytes()); QVERIFY(document.UndoEdit()); QVERIFY(!document.ParameterOrganization().configured);
        QVERIFY(document.RedoEdit()); QCOMPARE(document.ParameterOrganization(), accepted);
        QVERIFY(document.MakeParameterOrganizationEdit(state, edit)); QVERIFY(!document.ExecuteEdit(edit));
        state = document.ParameterOrganization(); std::swap(state.nodes[1], state.nodes[2]); state.nodes[0].name = "Renamed";
        QVERIFY(document.MakeParameterOrganizationEdit(state, edit)); QVERIFY(document.ExecuteEdit(edit));
        QVERIFY(document.UndoEdit()); QCOMPARE(document.ParameterOrganization(), accepted); QVERIFY(document.UndoEdit());
        QVERIFY(!document.CanUndoEdit()); QVERIFY(!document.ParameterOrganization().configured);
    }

    void rejectedAndStaleEditsKeepDocumentAndHistory()
    {
        auto document = m_original; CModelEdit edit; QVERIFY(document.MakeParameterOrganizationEdit(organization(document), edit));
        const auto before = document.AuthoringStructureState(); const auto revision = document.Revision();
        auto invalid = edit; auto state = invalid.after.value<Organization::State>(); state.nodes.removeLast(); invalid.after = QVariant::fromValue(state);
        QVERIFY(!document.ExecuteEdit(invalid)); QCOMPARE(document.AuthoringStructureState(), before); QCOMPARE(document.Revision(), revision); QVERIFY(!document.CanUndoEdit());
        QVERIFY(document.ExecuteEdit(edit)); const auto accepted = document.AuthoringStructureState();
        QVERIFY(!document.ExecuteEdit(edit)); QCOMPARE(document.AuthoringStructureState(), accepted);
        invalid = {}; invalid.after = "keep"; state.configured = false;
        QVERIFY(!document.MakeParameterOrganizationEdit(state, invalid)); QCOMPARE(invalid.after.toString(), QString("keep"));
        state = document.ParameterOrganization(); state.nodes[0].name = "external"; QVERIFY(document.SetParameterOrganization(state));
        QVERIFY(!document.UndoEdit()); QCOMPARE(document.ParameterOrganization(), state);
    }

    void deleteRecreateAndUndoRestoreExactOrganization()
    {
        auto document = m_original; CModelEdit edit; QString error;
        CMocParameterStructureBuilder::CreateRequest request; request.parameter.id = "OrganizationCreated";
        QVERIFY(document.MakeCreateParameterEdit(request, "Created", edit, &error)); QVERIFY(document.ExecuteEdit(edit));
        auto state = organization(document); state.pairs.clear(); state.pairs.append({request.parameter.id, document.Parameters()[0].id, true, true});
        state.nodes.last().parentFolder = "face"; state.nodes.last().color = "#112233";
        QVERIFY(document.MakeParameterOrganizationEdit(state, edit)); QVERIFY(document.ExecuteEdit(edit));
        const auto configured = document.ParameterOrganization(); const auto withParameter = document.AuthoringStructureState();
        QVERIFY2(document.MakeDeleteParameterEdit(request.parameter.id, edit, &error), qPrintable(error)); QVERIFY(document.ExecuteEdit(edit));
        QVERIFY(document.ParameterOrganization().configured); QVERIFY(document.ParameterOrganization().pairs.isEmpty());
        const auto deleted = document.AuthoringStructureState();
        QVERIFY(document.MakeCreateParameterEdit(request, "Recreated", edit, &error)); QVERIFY(document.ExecuteEdit(edit));
        const auto recreated = document.ParameterOrganization(); QVERIFY(recreated.pairs.isEmpty());
        QCOMPARE(recreated.nodes.last().id, request.parameter.id); QVERIFY(recreated.nodes.last().parentFolder.isEmpty()); QVERIFY(recreated.nodes.last().color.isEmpty());
        QVERIFY(document.UndoEdit()); QCOMPARE(document.AuthoringStructureState(), deleted);
        QVERIFY(document.UndoEdit()); QCOMPARE(document.AuthoringStructureState(), withParameter); QCOMPARE(document.ParameterOrganization(), configured);
        QVERIFY(document.RedoEdit()); QVERIFY(document.RedoEdit()); QCOMPARE(document.ParameterOrganization(), recreated);
    }

    void structureOperationsAndSessionPreserveOrganization_data()
    {
        QTest::addColumn<int>("kind"); QTest::newRow("parameter") << 0; QTest::newRow("mesh") << 1; QTest::newRow("deformer") << 2;
    }
    void structureOperationsAndSessionPreserveOrganization()
    {
        QFETCH(int, kind); auto document = m_original; QString error; CModelEdit edit;
        QVERIFY(document.MakeParameterOrganizationEdit(organization(document), edit)); QVERIFY(document.ExecuteEdit(edit));
        const auto before = document.ParameterOrganization();
        if (kind == 0) {
            CMocParameterStructureBuilder::CreateRequest request; request.parameter.id = "OrgAdded";
            QVERIFY(document.MakeCreateParameterEdit(request, {}, edit, &error));
        } else if (kind == 1) {
            const auto& source = document.Drawables().first(); CMocMeshStructureBuilder::MeshEdit mesh; mesh.artMeshId = source.id;
            for (int index = 0; index < source.vertexCount; ++index) { mesh.vertices.append({{index}, {1}}); mesh.oldToNew.append(index); }
            mesh.vertices.append({{0, 1}, {.5, .5}}); mesh.indices = document.DrawableIndices(source.id);
            CMocMeshStructureBuilder::Request request; request.meshes.append(mesh);
            QVERIFY2(document.MakeMeshStructureEdit(request, edit, &error), qPrintable(error));
        } else {
            const auto bytes = preview(document); auto request = DeformerReplayFixture::rootMesh(bytes, true); request.id = "OrganizationWarp";
            QVERIFY2(document.MakeDeformerStructureEdit({request, document.SourceDigest(), QCryptographicHash::hash(bytes, QCryptographicHash::Sha256)}, edit, &error), qPrintable(error));
        }
        QVERIFY2(document.ExecuteEdit(edit), qPrintable(error));
        if (kind == 0) { QCOMPARE(document.ParameterOrganization().nodes.size(), before.nodes.size() + 1); QVERIFY(document.ParameterOrganization().nodes.last().parentFolder.isEmpty()); }
        else QCOMPARE(document.ParameterOrganization(), before);
        const auto accepted = document.AuthoringStructureState(); const auto json = capture(document).ToJson(); QVERIFY(!json.isEmpty());
        QCOMPARE(QJsonDocument::fromJson(json).object()["version"].toInt(), 8); CModelSession decoded;
        QVERIFY2(CModelSession::FromJson(json, &decoded, &error), qPrintable(error));
        for (const auto* loaded : {&m_original, &document}) {
            CModelDocument restored; QVERIFY2(decoded.RestoreDocument(*loaded, restored, &error), qPrintable(error));
            QCOMPARE(restored.AuthoringStructureState(), accepted); QCOMPARE(preview(restored), preview(document)); QVERIFY(!restored.CanUndoEdit());
        }
        QVERIFY(document.UndoEdit()); QCOMPARE(document.ParameterOrganization(), before); QVERIFY(document.RedoEdit()); QCOMPARE(document.AuthoringStructureState(), accepted);
    }

    void oldSessionsClearOrganization_data()
    {
        QTest::addColumn<int>("version"); for (int version = 1; version <= 7; ++version) QTest::newRow(qPrintable(QString::number(version))) << version;
    }
    void oldSessionsClearOrganization()
    {
        QFETCH(int, version); auto current = m_original; QVERIFY(current.SetParameterOrganization(organization(current)));
        auto session = capture(m_original); session.version = version; const auto bytes = session.ToJson(); QVERIFY(!bytes.isEmpty());
        QVERIFY(!QJsonDocument::fromJson(bytes).object().contains("parameterOrganization")); CModelSession decoded;
        decoded.parameterOrganization = current.ParameterOrganization(); QVERIFY(CModelSession::FromJson(bytes, &decoded)); QVERIFY(!decoded.parameterOrganization.configured);
        CModelDocument restored; QVERIFY(decoded.RestoreDocument(current, restored)); QVERIFY(!restored.ParameterOrganization().configured); QCOMPARE(preview(restored), preview(m_original));
    }

    void versionEightRejectsMalformedStateWithoutReplacingTargets_data()
    {
        QTest::addColumn<int>("kind"); const char* names[]{"missing-field", "wrong-field", "old-version", "future-version", "missing-digest", "null-path", "missing-parameter", "wrong-source"};
        for (int kind = 0; kind < 8; ++kind) QTest::newRow(names[kind]) << kind;
    }
    void versionEightRejectsMalformedStateWithoutReplacingTargets()
    {
        QFETCH(int, kind); auto current = m_original; QVERIFY(current.SetParameterOrganization(organization(current)));
        const auto goodBytes = capture(current).ToJson(); auto root = QJsonDocument::fromJson(goodBytes).object();
        if (kind == 0) root.remove("parameterOrganization"); if (kind == 1) root["parameterOrganization"] = QJsonArray{};
        if (kind == 2) root["version"] = 7; if (kind == 3) root["version"] = 10;
        if (kind == 4) root.remove("sourceDigest"); if (kind == 5) root["sourceMoc"] = QString("bad") + QChar::Null;
        if (kind == 6) { auto org = root["parameterOrganization"].toObject(); auto nodes = org["nodes"].toArray(); nodes.removeLast(); org["nodes"] = nodes; root["parameterOrganization"] = org; }
        if (kind == 7) root["sourceDigest"] = QString(64, '1');
        CModelSession decoded; QVERIFY(CModelSession::FromJson(goodBytes, &decoded)); QString error;
        if (kind < 6) { QVERIFY(!CModelSession::FromJson(QJsonDocument(root).toJson(), &decoded, &error)); QCOMPARE(decoded.ToJson(), goodBytes); }
        else {
            QVERIFY(CModelSession::FromJson(QJsonDocument(root).toJson(), &decoded, &error));
            auto restored = current; const auto before = restored.AuthoringStructureState(); const auto revision = restored.Revision();
            QVERIFY(!decoded.RestoreDocument(current, restored, &error)); QCOMPARE(restored.AuthoringStructureState(), before); QCOMPARE(restored.Revision(), revision);
        }
        QVERIFY(!error.isEmpty());
    }

    void inMemorySourcesKeepOrganizationAcrossStructureHistory()
    {
        CModelDocument document; QVERIFY(document.LoadBytes(m_original.SourceBytes(), {}));
        QVERIFY(document.SetParameterOrganization(organization(document))); const auto before = document.ParameterOrganization();
        CMocParameterStructureBuilder::CreateRequest request; request.parameter.id = "MemoryAdded"; CModelEdit edit; QString error;
        QVERIFY(document.MakeCreateParameterEdit(request, {}, edit, &error)); QVERIFY(document.ExecuteEdit(edit)); QVERIFY(document.UndoEdit());
        QCOMPARE(document.ParameterOrganization(), before);
    }
};
QTEST_GUILESS_MAIN(ParameterOrganizationDocumentTests)
#include "parameter_organization_document_tests.moc"
