#include "live2dstruct/cmodelsession.h"
#include "live2dstruct/ctextureeditcontroller.h"
#include "live2dstruct/cmocmeshstructurebuilder.h"
#include "deformer_replay_fixture.h"
#include <QtTest>
#include <QBuffer>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <limits>

using namespace live2dstruct;
using Assets = CTextureAssets;
using Raw = CMocDeformerStructureBuilder;
namespace {
QByteArray png(QRgb color, QSize size = {4, 3})
{
    QImage image(size, QImage::Format_RGBA8888); image.fill(QColor::fromRgba(color));
    QBuffer buffer;
    return buffer.open(QIODevice::WriteOnly) && image.save(&buffer, "PNG") ? buffer.data() : QByteArray();
}
QByteArray preview(const CModelDocument& document)
{
    QByteArray bytes; document.BuildPreviewMoc(bytes); return bytes;
}
CModelSession capture(const CModelDocument& document, const Assets::Draft& textures = {})
{
    CModelSession result; result.sourceMoc = document.SourcePath(); result.sourceDigest = document.SourceDigest();
    result.structure = document.ParameterStructure(); result.meshStructure = document.HasMeshStructure();
    result.deformerStructure = document.HasDeformerStructure();
    if (result.deformerStructure) result.sourceDeformerIds = document.SourceDeformerIds();
    result.textures = textures;
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
bool appendPoint(CModelDocument& document, const QString& id, QString* error)
{
    CMocMeshStructureBuilder::MeshEdit mesh; mesh.artMeshId = id;
    for (const auto& source : document.Drawables()) if (source.id == id) {
        for (int index = 0; index < source.vertexCount; ++index) { mesh.vertices.append({{index}, {1}}); mesh.oldToNew.append(index); }
        mesh.indices = document.DrawableIndices(id); break;
    }
    mesh.vertices.append({{0, 1}, {.25, .75}});
    CMocMeshStructureBuilder::Request request; request.meshes.append(mesh); CModelEdit edit;
    return document.MakeMeshStructureEdit(request, edit, error) && document.ExecuteEdit(edit);
}
struct EditorState {
    Assets::Catalog sources;
    Assets::State current;
    quint64 revision;
    int slot;
    bool dirty, undo, redo;
    explicit EditorState(const CTextureEditController& editor)
        : sources(editor.Sources()), current(editor.Current()), revision(editor.Revision()), slot(editor.LastChangedSlot()),
          dirty(editor.IsDirty()), undo(editor.CanUndo()), redo(editor.CanRedo()) {}
    bool matches(const CTextureEditController& editor) const {
        return sources == editor.Sources() && current == editor.Current() && revision == editor.Revision() &&
            slot == editor.LastChangedSlot() && dirty == editor.IsDirty() && undo == editor.CanUndo() && redo == editor.CanRedo();
    }
};
}

class TextureSessionTests : public QObject
{
    Q_OBJECT
    CModelDocument m_original;
    QTemporaryDir m_directory;
    Assets::Catalog m_sources;
    Assets::State m_state;
    Assets::Draft m_draft;

    void verifyDocument(const CModelDocument& actual, const CModelDocument& expected)
    {
        QCOMPARE(actual.SourceBytes(), expected.SourceBytes()); QCOMPARE(actual.SourceDigest(), expected.SourceDigest());
        QCOMPARE(actual.AuthoringStructureState(), expected.AuthoringStructureState()); QCOMPARE(preview(actual), preview(expected));
        QCOMPARE(actual.VisibilityState(), expected.VisibilityState()); QCOMPARE(actual.LockedKeys(), expected.LockedKeys());
        QCOMPARE(actual.ParameterMetadataOverrides(), expected.ParameterMetadataOverrides());
        QCOMPARE(actual.HasMeshStructure(), expected.HasMeshStructure()); QCOMPARE(actual.HasDeformerStructure(), expected.HasDeformerStructure());
    }
    bool mixedDocument(int kind, CModelDocument& document, QString* error)
    {
        document = m_original; const auto mesh = document.Drawables().first().id; CModelEdit edit;
        if (kind >= 2) {
            const auto bytes = preview(document); auto request = DeformerReplayFixture::rootMesh(bytes, kind == 3 || kind == 5);
            request.id = "SessionTextureControl";
            if (!document.MakeDeformerStructureEdit({request, document.SourceDigest(), QCryptographicHash::hash(bytes, QCryptographicHash::Sha256)}, edit, error) ||
                !document.ExecuteEdit(edit)) return false;
        }
        if ((kind == 1 || kind >= 4) && !appendPoint(document, mesh, error)) return false;
        CMocParameterStructureBuilder::CreateRequest parameter; parameter.parameter.id = "ParamTextureSession";
        if (!document.MakeCreateParameterEdit(parameter, QStringLiteral("会话参数"), edit, error) || !document.ExecuteEdit(edit)) return false;
        edit = {mesh, "artmesh", CModelEdit::Opacity, document.DrawableOpacity(document.Drawables().first()), .625f, -1, false};
        if (!document.ExecuteEdit(edit)) return false;
        document.SetDrawableVisible(mesh, false);
        document.SetLocked("artmesh", mesh, true);
        return true;
    }
private slots:
    void initTestCase()
    {
        QVERIFY(m_directory.isValid()); QString error;
        QVERIFY2(m_original.Load(QString(TEST_RESOURCES) + "/Mark/Mark.moc3", &error), qPrintable(error));
        Assets::Source source; QVERIFY(Assets::MakeSource(m_directory.filePath("shared.png"), png(qRgba(30, 71, 94, 81)), source, &error));
        m_sources = {{0, source}, {2, source}};
        m_state = {{2, png(qRgba(211, 17, 59, 47))}};
        QVERIFY(Assets::CaptureDraft(m_sources, m_state, m_draft, &error));
    }

    void versionSevenCombinesTexturesWithAuthoringState_data()
    {
        QTest::addColumn<int>("kind");
        const char* names[]{"parameter-properties", "mesh", "rotation", "warp", "rotation-mesh", "warp-mesh"};
        for (int kind = 0; kind < 6; ++kind) QTest::newRow(names[kind]) << kind;
    }
    void versionSevenCombinesTexturesWithAuthoringState()
    {
        QFETCH(int, kind); CModelDocument expected; QString error; QVERIFY2(mixedDocument(kind, expected, &error), qPrintable(error));
        auto session = capture(expected, m_draft); const auto bytes = session.ToJson(); QVERIFY(!bytes.isEmpty());
        const auto json = QJsonDocument::fromJson(bytes).object(); QCOMPARE(json["version"].toInt(), 7);
        QCOMPARE(json["meshStructure"].toBool(), expected.HasMeshStructure()); QCOMPARE(json["deformerStructure"].toBool(), expected.HasDeformerStructure());
        CModelSession decoded; QVERIFY2(CModelSession::FromJson(bytes, &decoded, &error), qPrintable(error)); QCOMPARE(decoded.textures, m_draft);
        QCOMPARE(decoded.sourceDeformerIds, expected.HasDeformerStructure() ? expected.SourceDeformerIds() : QSet<QString>{});
        CTextureEditController editor; QVERIFY(editor.Initialize(m_sources)); int calls = 0;
        editor.SetPublisher([&](const auto&, QString*) { ++calls; return true; });
        QVERIFY(editor.Replace(0, png(qRgba(15, 91, 83, 40)))); const EditorState held(editor);
        CTextureEditController candidate; QVERIFY(editor.PrepareRestore(decoded.textures, candidate, &error));
        QCOMPARE(candidate.Current(), m_state); QVERIFY(!candidate.IsDirty()); QVERIFY(!candidate.CanUndo()); QVERIFY(!candidate.CanRedo());
        QVERIFY(held.matches(editor)); QCOMPARE(calls, 1);
        for (const auto* loaded : {&m_original, &expected}) {
            CModelDocument restored; QVERIFY2(decoded.RestoreDocument(*loaded, restored, &error), qPrintable(error));
            verifyDocument(restored, expected); QVERIFY(!restored.CanUndoEdit()); QVERIFY(!restored.CanRedoEdit());
        }
        const auto path = m_directory.filePath(QString("mixed-%1.session").arg(kind)); QVERIFY2(session.Save(path, &error), qPrintable(error));
        CModelSession reopened; QVERIFY(CModelSession::Load(path, &reopened, &error)); QCOMPARE(reopened.ToJson(), bytes);
        editor.Swap(candidate); QCOMPARE(editor.Current(), m_state); QVERIFY(!editor.IsDirty()); QCOMPARE(calls, 1);
        QVERIFY(!QFileInfo::exists(m_sources[0].path));
    }

    void oldSessionsClearTextureDraftsAndHistory_data()
    {
        QTest::addColumn<int>("version"); for (int version = 1; version <= 6; ++version) QTest::newRow(qPrintable(QString::number(version))) << version;
    }
    void oldSessionsClearTextureDraftsAndHistory()
    {
        QFETCH(int, version); auto old = capture(m_original); old.version = version;
        const auto bytes = old.ToJson(); QVERIFY(!bytes.isEmpty()); QVERIFY(!QJsonDocument::fromJson(bytes).object().contains("textures"));
        auto decoded = capture(m_original, m_draft); QVERIFY(CModelSession::FromJson(bytes, &decoded));
        QCOMPARE(decoded.version, version); QVERIFY(decoded.textures.isEmpty());
        CTextureEditController editor; QVERIFY(editor.Initialize(m_sources)); int calls = 0;
        editor.SetPublisher([&](const auto&, QString*) { ++calls; return true; });
        QVERIFY(editor.Replace(2, m_state[2])); editor.MarkSaved();
        QVERIFY(editor.Replace(0, png(qRgba(43, 15, 91, 8)))); QVERIFY(editor.IsDirty()); QVERIFY(editor.CanUndo());
        const EditorState before(editor); CTextureEditController prepared;
        QVERIFY(editor.PrepareRestore(decoded.textures, prepared)); QVERIFY(before.matches(editor)); QCOMPARE(calls, 2);
        QVERIFY(prepared.Current().isEmpty()); QVERIFY(!prepared.IsDirty()); QVERIFY(!prepared.CanUndo()); QVERIFY(!prepared.CanRedo());
        CModelDocument restored; QVERIFY(decoded.RestoreDocument(m_original, restored)); verifyDocument(restored, m_original);
        editor.Swap(prepared); QVERIFY(editor.Current().isEmpty()); QCOMPARE(Assets::Effective(editor.Sources(), editor.Current()), Assets::Effective(m_sources, {}));
        QCOMPARE(calls, 2);
    }

    void malformedVersionSevenJsonKeepsSession_data()
    {
        QTest::addColumn<int>("kind");
        const char* names[]{"missing-textures", "wrong-textures-type", "missing-model-path", "missing-model-digest", "future-version",
            "old-version-with-textures", "missing-mesh-flag", "missing-deformer-flag", "wrong-source-ids", "texture-null-row"};
        for (int kind = 0; kind < 10; ++kind) QTest::newRow(names[kind]) << kind;
    }
    void malformedVersionSevenJsonKeepsSession()
    {
        QFETCH(int, kind); const auto saved = capture(m_original, m_draft).ToJson(); auto root = QJsonDocument::fromJson(saved).object();
        if (kind == 0) root.remove("textures"); if (kind == 1) root["textures"] = QJsonObject{};
        if (kind == 2) root.remove("sourceMoc"); if (kind == 3) root.remove("sourceDigest");
        if (kind == 4) root["version"] = 14; if (kind == 5) root["version"] = 6;
        if (kind == 6) root.remove("meshStructure"); if (kind == 7) root.remove("deformerStructure");
        if (kind == 8) root["sourceDeformerIds"] = "invalid"; if (kind == 9) root["textures"] = QJsonArray{QJsonValue::Null};
        CModelSession parsed; QVERIFY(CModelSession::FromJson(saved, &parsed)); QString error;
        QVERIFY(!CModelSession::FromJson(QJsonDocument(root).toJson(), &parsed, &error)); QVERIFY(!error.isEmpty()); QCOMPARE(parsed.ToJson(), saved);
    }

    void malformedDirectStateCannotSerializeOrReplaceDocument_data()
    {
        QTest::addColumn<int>("kind");
        const char* names[]{"invalid-slot", "relative-source", "invalid-source-digest", "invalid-png", "size-mismatch", "missing-model-path", "invalid-model-digest", "future-version"};
        for (int kind = 0; kind < 8; ++kind) QTest::newRow(names[kind]) << kind;
    }
    void malformedDirectStateCannotSerializeOrReplaceDocument()
    {
        QFETCH(int, kind); auto session = capture(m_original, m_draft); session.version = 7;
        if (kind == 0) { const auto value = session.textures.take(2); session.textures[-1] = value; }
        if (kind == 1) session.textures[2].sourcePath = "relative.png";
        if (kind == 2) session.textures[2].sourceDigest = "invalid";
        if (kind == 3) session.textures[2].png = "invalid";
        if (kind == 4) session.textures[2].size = {5, 3};
        if (kind == 5) session.sourceMoc.clear(); if (kind == 6) session.sourceDigest.clear(); if (kind == 7) session.version = 14;
        QVERIFY(session.ToJson().isEmpty());
        CModelDocument result; QString error; QVERIFY(mixedDocument(4, result, &error));
        const auto before = result; const auto revision = result.Revision();
        QVERIFY(!session.RestoreDocument(m_original, result, &error)); QVERIFY(!error.isEmpty());
        verifyDocument(result, before); QCOMPARE(result.Revision(), revision); QCOMPARE(result.CanUndoEdit(), before.CanUndoEdit());
        QVERIFY(!session.Save(m_directory.filePath("invalid.session"), &error));
    }

    void staleTextureAnchorsPreserveCurrentAndPreparedControllers_data()
    {
        QTest::addColumn<int>("kind"); QTest::newRow("path") << 0; QTest::newRow("digest") << 1;
        QTest::newRow("size") << 2; QTest::newRow("missing-slot") << 3;
    }
    void staleTextureAnchorsPreserveCurrentAndPreparedControllers()
    {
        QFETCH(int, kind); auto sources = m_sources; Assets::Source changed; QString error;
        if (kind == 0) sources[2].path = m_directory.filePath("different.png");
        if (kind == 1 || kind == 2) {
            QVERIFY(Assets::MakeSource(sources[2].path, png(qRgba(18, 13, 12, 11), kind == 2 ? QSize(5, 3) : QSize(4, 3)), changed, &error));
            sources[0] = sources[2] = changed;
        }
        if (kind == 3) sources.remove(2);
        CTextureEditController editor; QVERIFY(editor.Initialize(sources)); int calls = 0;
        editor.SetPublisher([&](const auto&, QString*) { ++calls; return true; });
        QVERIFY(editor.Replace(0, png(qRgba(51, 91, 33, 42), sources[0].size))); editor.MarkSaved();
        CTextureEditController prepared; QVERIFY(prepared.Initialize(m_sources, m_state));
        const EditorState currentBefore(editor), preparedBefore(prepared);
        auto session = capture(m_original, m_draft); session.version = 7; CModelDocument document = m_original; const auto revision = document.Revision();
        CModelDocument documentCandidate; QVERIFY(session.RestoreDocument(document, documentCandidate, &error));
        QVERIFY(!editor.PrepareRestore(session.textures, prepared, &error)); QVERIFY(!error.isEmpty());
        QVERIFY(currentBefore.matches(editor)); QVERIFY(preparedBefore.matches(prepared)); QCOMPARE(calls, 1);
        verifyDocument(document, m_original); QCOMPARE(document.Revision(), revision);
    }

    void versionSevenCannotHideStructuralCapabilities_data()
    {
        QTest::addColumn<bool>("mesh"); QTest::newRow("mesh-flag") << true; QTest::newRow("deformer-flag") << false;
    }
    void versionSevenCannotHideStructuralCapabilities()
    {
        QFETCH(bool, mesh); CModelDocument edited; QString error; QVERIFY(mixedDocument(4, edited, &error));
        CModelSession session; QVERIFY(CModelSession::FromJson(capture(edited, m_draft).ToJson(), &session, &error));
        if (mesh) session.meshStructure = false;
        else { session.deformerStructure = false; session.sourceDeformerIds.clear(); }
        CModelDocument result = m_original; const auto revision = result.Revision();
        QVERIFY(!session.RestoreDocument(m_original, result, &error)); QVERIFY(!error.isEmpty());
        verifyDocument(result, m_original); QCOMPARE(result.Revision(), revision);
        CTextureEditController editor; QVERIFY(editor.Initialize(m_sources)); CTextureEditController prepared;
        QVERIFY(editor.PrepareRestore(session.textures, prepared)); QCOMPARE(prepared.Current(), m_state);
        QVERIFY(editor.Current().isEmpty());
    }

    void rejectedModelRestoreKeepsPreparedTextureState()
    {
        CTextureEditController editor; QVERIFY(editor.Initialize(m_sources, m_state));
        CTextureEditController prepared; QVERIFY(editor.PrepareRestore(m_draft, prepared)); const EditorState held(prepared), current(editor);
        auto session = capture(m_original, m_draft); session.version = 7; session.sourceDigest = QByteArray(32, 'x');
        CModelDocument result; QString error; QVERIFY(mixedDocument(5, result, &error)); const auto before = result; const auto revision = result.Revision();
        QVERIFY(!session.RestoreDocument(m_original, result, &error)); QVERIFY(!error.isEmpty());
        verifyDocument(result, before); QCOMPARE(result.Revision(), revision); QVERIFY(held.matches(prepared)); QVERIFY(current.matches(editor));
    }

    void preparedRestoreRetainsPublisherAndRejectsWithoutCommitting()
    {
        CTextureEditController editor; QVERIFY(editor.Initialize(m_sources)); int calls = 0; bool reject = true;
        editor.SetPublisher([&](const auto&, QString* error) { ++calls; if (reject && error) *error = "rejected"; return !reject; });
        CTextureEditController prepared; QVERIFY(editor.PrepareRestore(m_draft, prepared)); QCOMPARE(calls, 0);
        const EditorState held(prepared), current(editor); QString error;
        QVERIFY(!prepared.Replace(0, png(qRgba(44, 37, 29, 21)), &error)); QCOMPARE(error, QString("rejected"));
        QVERIFY(held.matches(prepared)); QVERIFY(current.matches(editor)); QCOMPARE(calls, 1);
        reject = false; QVERIFY(prepared.Replace(0, png(qRgba(44, 37, 29, 21)), &error)); QCOMPARE(calls, 2);
        QVERIFY(current.matches(editor)); QVERIFY(prepared.IsDirty()); QVERIFY(prepared.CanUndo());
    }
};
QTEST_GUILESS_MAIN(TextureSessionTests)
#include "texture_session_tests.moc"
