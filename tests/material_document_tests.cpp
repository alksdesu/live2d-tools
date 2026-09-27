#include "geometry_object_pairs_document_fixture.h"
#include "live2dstruct/cmocmaterialbuilder.h"
#include "live2dstruct/cmodelsavefingerprint.h"
#include <QtTest>
#include <QJsonDocument>
#include <QJsonArray>
#include <QBuffer>
#include <QImage>
#include <limits>
using namespace GeometryPairsData;
namespace {
QByteArray atlasPng(QSize size, QRgb color = qRgba(17, 29, 43, 127))
{
    QImage image(size, QImage::Format_RGBA8888); image.fill(QColor::fromRgba(color));
    QBuffer output; if (!output.open(QIODevice::WriteOnly) || !image.save(&output, "PNG")) return {};
    return output.data();
}
CTextureAssets::Catalog atlasSources()
{
    CTextureAssets::Source source; if (!CTextureAssets::MakeSource("/tmp/atlas-material-source.png", atlasPng({3, 2}), source)) return {};
    return {{0, source}};
}
CModelSession atlasSession(const CModelDocument& document)
{
    auto session = capture(document); session.version = 11; session.materialStructure = document.HasMaterialStructure();
    session.drawableMaterials = document.DrawableMaterialOverrides(); CTextureWorkspace::Draft draft;
    if (CTextureWorkspace::CaptureDraft(document.TextureSources(), document.TextureWorkspace(), draft)) session.textureWorkspace = draft;
    return session;
}
CModelSession materialSession(const CModelDocument& document)
{
    auto session = capture(document); session.materialStructure = document.HasMaterialStructure();
    session.drawableMaterials = document.DrawableMaterialOverrides(); return session;
}
CModelDrawableMaterial material(const CModelDocument& document, const QString& id)
{
    CModelDrawableMaterial value; document.CaptureDrawableMaterial(id, value); return value;
}
bool change(CModelDocument& document, QString* error = nullptr)
{
    auto before = material(document, "Mesh"), after = before; after.uvs[0] = .25f; after.textureIndex = 2;
    CModelEdit edit; return document.MakeMaterialEdit({before}, {after}, edit, error) && document.ExecuteEdit(edit);
}
}
class MaterialDocumentTests : public QObject
{
    Q_OBJECT
private slots:
    void sessionsRestoreGeneratedIdentityAcrossCompleteWorkspaces()
    {
        CModelDocument document; QVERIFY(load(document)); const auto sources = atlasSources(); QVERIFY(document.InitializeTextureWorkspace(sources));
        CModelAtlasState base; QVERIFY(document.CaptureAtlasState(base)); auto a = base;
        a.workspace.atlases.insert(1, {"atlas-A", "图集 A", -1, {5, 7}, atlasPng({5, 7}, qRgb(5, 11, 17))});
        CModelEdit edit; QString error; QVERIFY(document.MakeAtlasEdit(base, a, edit, &error)); QVERIFY(document.ExecuteEdit(edit));
        const auto savedA = atlasSession(document).ToJson(); auto b = a; b.workspace.atlases[1].id = "atlas-B";
        b.workspace.atlases[1].name = "图集 B"; b.workspace.atlases[1].png = atlasPng({5, 7}, qRgb(23, 29, 31));
        QVERIFY2(document.MakeAtlasEdit(a, b, edit, &error), qPrintable(error)); QVERIFY(document.ExecuteEdit(edit));
        const auto savedB = atlasSession(document).ToJson(); CModelSession sessionA, sessionB;
        QVERIFY(CModelSession::FromJson(savedA, &sessionA, &error)); QVERIFY(CModelSession::FromJson(savedB, &sessionB, &error));
        CModelDocument restoredA; QVERIFY2(sessionA.RestoreDocument(document, restoredA, &error), qPrintable(error));
        CModelAtlasState actual; QVERIFY(restoredA.CaptureAtlasState(actual)); QCOMPARE(actual, a); QCOMPARE(restoredA.TextureSources(), sources);
        QVERIFY(!restoredA.CanUndoEdit()); CModelDocument restoredB;
        QVERIFY2(sessionB.RestoreDocument(restoredA, restoredB, &error), qPrintable(error));
        QVERIFY(restoredB.CaptureAtlasState(actual)); QCOMPARE(actual, b); QCOMPARE(restoredB.TextureSources(), sources);
        QVERIFY(document.SetTextureWorkspace(a.workspace, &error)); QCOMPARE(document.TextureWorkspace(), a.workspace);
        QCOMPARE(document.TextureSources(), sources);
    }
    void identicalAtlasPixelsShareOwnedHistoryStorage()
    {
        CModelDocument document; QVERIFY(load(document)); auto sources = atlasSources(); sources[3] = sources[0];
        QVERIFY(document.InitializeTextureWorkspace(sources));
        QVERIFY(document.TextureSources()[0].png.constData() == document.TextureSources()[3].png.constData());
        CModelAtlasState before; QVERIFY(document.CaptureAtlasState(before)); auto first = before;
        first.workspace.atlases.insert(4, {"generated-first", "首张图集", -1, {5, 7}, atlasPng({5, 7})});
        CModelEdit edit; QVERIFY(document.MakeAtlasEdit(before, first, edit)); QVERIFY(document.ExecuteEdit(edit));
        CModelAtlasState current; QVERIFY(document.CaptureAtlasState(current)); const auto* stored = current.workspace.atlases[4].png.constData();
        auto second = current; second.workspace.atlases[5] = second.workspace.atlases[4]; second.workspace.atlases[5].id = "generated-second";
        QVERIFY(document.MakeAtlasEdit(current, second, edit)); QVERIFY(document.ExecuteEdit(edit));
        QVERIFY(document.TextureWorkspace().atlases[4].png.constData() == stored);
        QVERIFY(document.TextureWorkspace().atlases[5].png.constData() == stored);
        QVERIFY(document.UndoEdit()); QVERIFY(document.RedoEdit()); QVERIFY(document.TextureWorkspace().atlases[5].png.constData() == stored);
    }
    void atlasAndMaterialsShareOneExactHistory()
    {
        CModelDocument document; QVERIFY(load(document)); const auto sources = atlasSources();
        QVERIFY(document.InitializeTextureWorkspace(sources)); QVERIFY(document.SetGeometryObjectPairs(paired(document)));
        const auto initial = document.AuthoringStructureState(); const auto pairState = document.GeometryObjectPairs();
        const auto positions = document.DrawableKeyformPositionOverrides(); const auto indices = document.DrawableIndices("Mesh");
        QByteArray initialSave; QVERIFY(CModelSaveFingerprint::Capture(document, initialSave));
        CModelAtlasState before; QVERIFY(document.CaptureAtlasState(before)); auto after = before;
        after.workspace.atlases.insert(1, {"new-atlas", "新图集", -1, {6, 5}, atlasPng({6, 5})});
        after.workspace.atlases[0].png = atlasPng({3, 2}, qRgba(60, 4, 9, 0));
        for (auto& value : after.materials) if (value.id == "Mesh") { value.textureIndex = 1; value.uvs[0] = -.0f; }
        CModelEdit edit; QString error; QVERIFY2(document.MakeAtlasEdit(before, after, edit, &error), qPrintable(error));
        QVERIFY(document.ExecuteEdit(edit)); QVERIFY(!document.ExecuteEdit(edit));
        QCOMPARE(document.TextureSources(), sources); QCOMPARE(document.TextureWorkspace(), after.workspace);
        QCOMPARE(document.GeometryObjectPairs(), pairState); QCOMPARE(document.DrawableKeyformPositionOverrides(), positions);
        QCOMPARE(document.DrawableIndices("Mesh"), indices);
        QByteArray runtime; QVERIFY(document.BuildRuntimeMoc(runtime, &error)); CModelDocument compiled;
        QVERIFY(compiled.LoadBytes(runtime, "atlas-preview", &error)); QCOMPARE(material(compiled, "Mesh").textureIndex, 1);
        const auto changed = document.AuthoringStructureState(); QByteArray changedSave; QVERIFY(CModelSaveFingerprint::Capture(document, changedSave)); QVERIFY(changedSave != initialSave);
        QVERIFY(document.UndoEdit()); QCOMPARE(document.AuthoringStructureState(), initial);
        QByteArray restoredSave; QVERIFY(CModelSaveFingerprint::Capture(document, restoredSave)); QCOMPARE(restoredSave, initialSave);
        QVERIFY(document.DrawableMaterialOverrides().isEmpty()); QVERIFY(document.RedoEdit()); QCOMPARE(document.AuthoringStructureState(), changed);
    }
    void atlasRejectsLocksMissingSlotsAndStaleStorage()
    {
        CModelDocument document; QVERIFY(load(document)); QVERIFY(document.InitializeTextureWorkspace(atlasSources()));
        CModelAtlasState before; QVERIFY(document.CaptureAtlasState(before)); auto after = before;
        after.workspace.atlases.insert(1, {"new-atlas", "新图集", -1, {6, 5}, atlasPng({6, 5})});
        for (auto& value : after.materials) if (value.id == "Mesh") value.textureIndex = 1;
        CModelEdit edit; QString error; document.SetLocked("rotation", "Root", true);
        QVERIFY(!document.MakeAtlasEdit(before, after, edit, &error)); document.SetLocked("rotation", "Root", false);
        QVERIFY(document.MakeAtlasEdit(before, after, edit, &error)); QVERIFY(document.ExecuteEdit(edit));
        CModelAtlasState current; QVERIFY(document.CaptureAtlasState(current)); auto removal = current; removal.workspace.atlases.remove(1);
        QVERIFY(!document.MakeAtlasEdit(current, removal, edit, &error));
        auto unknown = material(document, "Mesh"); unknown.textureIndex = 8; QVERIFY(!document.SetDrawableMaterials({unknown}, &error));
        auto invalid = after; invalid.workspace.atlases[0].id = "replacement"; QVERIFY(!document.MakeAtlasEdit(current, invalid, edit, &error));
        auto external = document.TextureWorkspace(); external.atlases[1].name = "changed"; QVERIFY(document.SetTextureWorkspace(external));
        QVERIFY(!document.UndoEdit()); QCOMPARE(document.TextureWorkspace(), external);
    }
    void workspaceSurvivesTopologyAndSessionRestoration()
    {
        CModelDocument document; QVERIFY(load(document)); QVERIFY(document.InitializeTextureWorkspace(atlasSources()));
        CModelAtlasState before; QVERIFY(document.CaptureAtlasState(before)); auto after = before;
        after.workspace.atlases.insert(1, {"new-atlas", "新图集", -1, {7, 5}, atlasPng({7, 5})});
        for (auto& value : after.materials) if (value.id == "Mesh") value.textureIndex = 1;
        CModelEdit edit; QString error; QVERIFY(document.MakeAtlasEdit(before, after, edit, &error)); QVERIFY(document.ExecuteEdit(edit));
        const auto atlasState = document.AuthoringStructureState();
        QVERIFY2(document.MakeMeshStructureEdit(reorder(document), edit, &error), qPrintable(error)); QVERIFY(document.ExecuteEdit(edit));
        QCOMPARE(document.TextureWorkspace(), after.workspace); const auto reordered = document.AuthoringStructureState();
        QVERIFY(document.UndoEdit()); QCOMPARE(document.AuthoringStructureState(), atlasState); QVERIFY(document.RedoEdit()); QCOMPARE(document.AuthoringStructureState(), reordered);
        auto session = atlasSession(document); CModelSession parsed; const auto json = session.ToJson(); QVERIFY(!json.isEmpty());
        QVERIFY2(CModelSession::FromJson(json, &parsed, &error), qPrintable(error)); QCOMPARE(parsed.version, 11);
        CModelDocument restored; QVERIFY2(parsed.RestoreDocument(document, restored, &error), qPrintable(error));
        QCOMPARE(restored.AuthoringStructureState(), document.AuthoringStructureState()); QCOMPARE(restored.TextureSources(), document.TextureSources());
        CModelDocument unbound; QVERIFY(load(unbound)); QVERIFY(!parsed.RestoreDocument(unbound, restored, &error));
        QCOMPARE(restored.AuthoringStructureState(), document.AuthoringStructureState());
    }
    void oldSessionsMigratePngOverridesIntoDocument()
    {
        for (int version = 1; version <= 10; ++version) {
            CModelDocument document; QVERIFY(load(document)); const auto sources = atlasSources(); QVERIFY(document.InitializeTextureWorkspace(sources));
            auto session = capture(document); session.version = version;
            if (version >= 7) QVERIFY(CTextureAssets::CaptureDraft(sources, {{0, atlasPng({3, 2}, qRgb(12, 19, 61))}}, session.textures));
            CModelSession parsed; QString error; QVERIFY2(CModelSession::FromJson(session.ToJson(), &parsed, &error), qPrintable(error));
            CModelDocument restored; QVERIFY2(parsed.RestoreDocument(document, restored, &error), qPrintable(error));
            QVERIFY(restored.HasTextureWorkspace()); CTextureAssets::State pngs; QVERIFY(CTextureWorkspace::Effective(restored.TextureSources(), restored.TextureWorkspace(), pngs));
            QCOMPARE(pngs[0], version >= 7 ? session.textures[0].png : sources[0].png); QVERIFY(!restored.PeekUndoEdit());
            QCOMPARE(restored.TextureSources(), sources);
        }
    }
    void workspaceSessionRejectsMixedAndMissingFields()
    {
        CModelDocument document; QVERIFY(load(document)); QVERIFY(document.InitializeTextureWorkspace(atlasSources()));
        const auto session = atlasSession(document); const auto valid = QJsonDocument::fromJson(session.ToJson()).object();
        for (int fault = 0; fault < 5; ++fault) {
            auto json = valid;
            if (fault == 0) json.remove("textureWorkspace");
            if (fault == 1) json["version"] = 10;
            if (fault == 2) json["textureWorkspace"] = QJsonArray{};
            if (fault == 3) { auto workspace = json["textureWorkspace"].toObject(); workspace["sourceFingerprint"] = QString(64, 'a'); json["textureWorkspace"] = workspace; }
            if (fault == 4) { auto mixed = session; QVERIFY(CTextureAssets::CaptureDraft(document.TextureSources(), {{0, atlasPng({3, 2}, qRgb(1, 2, 3))}}, mixed.textures)); QVERIFY(mixed.ToJson().isEmpty()); continue; }
            CModelSession parsed; parsed.sourceMoc = "sentinel"; QString error;
            QVERIFY(!CModelSession::FromJson(QJsonDocument(json).toJson(), &parsed, &error)); QCOMPARE(parsed.sourceMoc, QString("sentinel"));
        }
    }
    void highUnsignedIndicesSurviveDocumentLoading()
    {
        DeformerSnapshotData::Fixture source; const auto& offsets = *source.table.sectionOffsetTable;
        source.column(offsets.art_meshes->vertex_counts, 0, 65536);
        source.appendColumn(0x178, QVector<int>(65536 * 2, 0)); source.count(15, 65536 * 2);
        source.appendColumn(0x15c, QVector<int>(65536 * 4, 0)); source.count(10, 65536 * 4);
        source.column(offsets.art_mesh_keyforms->keyform_position_sources_begin_indices, 0, 0);
        source.column(offsets.art_mesh_keyforms->keyform_position_sources_begin_indices, 1, 65536 * 2);
        const QVector<quint16> expected{32768, 65534, 65535};
        for (int index = 0; index < 3; ++index) qToLittleEndian(expected[index], source.bytes.data() + offsets.position_indices->indices + index * 2);
        CModelDocument document; QString error; QVERIFY2(document.LoadBytes(source.bytes, "unsigned-mesh", &error), qPrintable(error));
        QCOMPARE(document.DrawableIndices("Mesh"), expected); QCOMPARE(document.Drawables()[0].indices, expected);
        for (const auto& diagnostic : document.Validate()) QVERIFY(diagnostic.code != QStringLiteral("index_range"));
        CModelDrawableMaterial value; QVERIFY(document.CaptureDrawableMaterial("Mesh", value, &error)); QCOMPARE(value.uvs.size(), 65536 * 2);
    }
    void atomicHistoryAndExactPresence()
    {
        CModelDocument document; QVERIFY(load(document)); QVERIFY(document.SetGeometryObjectPairs(paired(document)));
        const auto initial = document.AuthoringStructureState(); const auto source = document.SourceBytes(); const auto pairs = document.GeometryObjectPairs();
        const auto original = material(document, "Mesh"); const auto sibling = material(document, "Twin"); QString error;
        QByteArray saved, changedFingerprint, restoredFingerprint; QVERIFY(CModelSaveFingerprint::Capture(document, saved, &error));
        QVERIFY2(change(document, &error), qPrintable(error)); const auto edited = document.AuthoringStructureState();
        QCOMPARE(document.GeometryObjectPairs(), pairs); QCOMPARE(material(document, "Twin"), sibling);
        QVERIFY(CModelSaveFingerprint::Capture(document, changedFingerprint, &error)); QVERIFY(changedFingerprint != saved);
        QByteArray preview, runtime; QVERIFY(document.BuildPreviewMoc(preview, &error)); QVERIFY(document.BuildRuntimeMoc(runtime, &error));
        CModelDocument compiled; QVERIFY(compiled.LoadBytes(preview, "preview", &error));
        QCOMPARE(material(compiled, "Mesh"), material(document, "Mesh")); QCOMPARE(document.SourceBytes(), source);
        QVERIFY(document.UndoEdit()); QCOMPARE(document.AuthoringStructureState(), initial); QCOMPARE(material(document, "Mesh"), original);
        QVERIFY(CModelSaveFingerprint::Capture(document, restoredFingerprint, &error)); QCOMPARE(restoredFingerprint, saved);
        QVERIFY(document.DrawableMaterialOverrides().isEmpty()); QVERIFY(document.RedoEdit()); QCOMPARE(document.AuthoringStructureState(), edited);
        QVERIFY(document.SetDrawableMaterials({original})); QVERIFY(document.DrawableMaterialOverrides().isEmpty());
    }
    void noOpDoesNotCreateOverrideOrHistory()
    {
        CModelDocument document; QVERIFY(load(document)); const auto before = document.AuthoringStructureState(); const auto revision = document.Revision();
        CModelEdit edit; const auto value = material(document, "Mesh"); QVERIFY(document.MakeMaterialEdit({value}, {value}, edit));
        QVERIFY(!document.ExecuteEdit(edit)); QCOMPARE(document.AuthoringStructureState(), before); QCOMPARE(document.Revision(), revision);
        QVERIFY(document.SetDrawableMaterials({value})); QCOMPARE(document.Revision(), revision);
    }
    void invalidBatchPreservesDocument_data()
    {
        QTest::addColumn<int>("fault"); for (int fault = 0; fault < 9; ++fault) QTest::newRow(qPrintable(QString::number(fault))) << fault;
    }
    void invalidBatchPreservesDocument()
    {
        QFETCH(int, fault); CModelDocument document; QVERIFY(load(document)); const auto state = document.AuthoringStructureState();
        const auto first = material(document, "Mesh"), second = material(document, "Twin"); auto nextFirst = first, nextSecond = second;
        nextFirst.uvs[0] += .1f; nextSecond.uvs[0] += .2f;
        if (fault == 0) nextSecond.id = "missing";
        if (fault == 1) nextSecond.id = first.id;
        if (fault == 2) nextSecond.textureIndex = -1;
        if (fault == 3) nextSecond.uvs.removeLast();
        if (fault == 4) nextSecond.uvs[0] = std::numeric_limits<float>::quiet_NaN();
        if (fault == 5) nextSecond.uvs[0] = std::numeric_limits<float>::infinity();
        if (fault == 6) nextSecond.uvs.clear();
        if (fault == 7) nextSecond.uvs += nextSecond.uvs;
        if (fault == 8) nextSecond.id = QString(QChar::Null);
        CModelEdit edit; edit.name = "sentinel"; QString error;
        QVERIFY(!document.MakeMaterialEdit({first, second}, {nextFirst, nextSecond}, edit, &error)); QVERIFY(!error.isEmpty());
        QCOMPARE(edit.name, QString("sentinel")); QCOMPARE(document.AuthoringStructureState(), state);
        QVERIFY(!document.SetDrawableMaterials({nextFirst, nextSecond}, &error)); QCOMPARE(document.AuthoringStructureState(), state);
    }
    void lockedUnchangedRowsAndStaleChangedRows()
    {
        CModelDocument document; QVERIFY(load(document)); auto first = material(document, "Mesh"), second = material(document, "Twin");
        document.SetLocked("artmesh", "Twin", true); auto changed = first; changed.uvs[0] += .25f;
        CModelEdit edit; QString error; QVERIFY2(document.MakeMaterialEdit({first, second}, {changed, second}, edit, &error), qPrintable(error));
        document.SetLocked("artmesh", "Mesh", true); QVERIFY(!document.ExecuteEdit(edit)); document.SetLocked("artmesh", "Mesh", false);
        QVERIFY(document.ExecuteEdit(edit)); QVERIFY(!document.ExecuteEdit(edit)); QVERIFY(document.UndoEdit());
        document.SetLocked("rotation", "Root", true); QVERIFY(!document.MakeMaterialEdit({first}, {changed}, edit, &error));
    }
    void lockedPartsOnDeformerAncestors()
    {
        DeformerSnapshotData::Fixture source; const auto& offsets = *source.table.sectionOffsetTable;
        source.count(0, 2);
        for (int index = 0; index < 2; ++index) {
            source.id(offsets.parts->ids, index, index ? "PartChild" : "PartRoot");
            source.column(offsets.parts->keyform_binding_sources_indices, index, -1);
            source.column(offsets.parts->keyform_sources_begin_indices, index, 0);
            source.column(offsets.parts->keyform_sources_counts, index, 0);
            source.column(offsets.parts->is_visible, index, 1); source.column(offsets.parts->is_enabled, index, 1);
            source.column(offsets.parts->parent_part_indices, index, index - 1);
        }
        source.column(offsets.deformers->parent_part_indices, 1, 1);
        CModelDocument document; QString error; QVERIFY(document.LoadBytes(source.bytes, "material-parts", &error));
        const auto before = material(document, "Mesh"); auto after = before; after.uvs[0] += .25f;
        document.SetLocked("part", "PartRoot", true); CModelEdit edit;
        QVERIFY(!document.MakeMaterialEdit({before}, {after}, edit, &error)); QVERIFY(error.contains("PartRoot"));
        QVERIFY(document.SetDeformerParentIndex("Child", -1)); QVERIFY(document.MakeMaterialEdit({before}, {after}, edit, &error));
        QVERIFY(document.SetDeformerParentIndex("Child", 1)); QVERIFY(!document.ExecuteEdit(edit));
    }
    void signedZeroAndSubnormalRoundTrip()
    {
        CModelDocument document; QVERIFY(load(document)); auto original = material(document, "Mesh"), next = original;
        next.uvs[0] = -0.0f; next.uvs[2] = std::numeric_limits<float>::denorm_min();
        CModelEdit edit; QVERIFY(document.MakeMaterialEdit({original}, {next}, edit)); QVERIFY(document.ExecuteEdit(edit));
        const auto state = document.AuthoringStructureState(); auto session = materialSession(document); const auto json = session.ToJson();
        QCOMPARE(QJsonDocument::fromJson(json).object().value("version").toInt(), 10);
        CModelSession decoded; QString error; QVERIFY2(CModelSession::FromJson(json, &decoded, &error), qPrintable(error));
        CModelDocument restored; QVERIFY2(decoded.RestoreDocument(document, restored, &error), qPrintable(error));
        QCOMPARE(material(restored, "Mesh"), next); QCOMPARE(restored.AuthoringStructureState(), state);
        QVERIFY(document.UndoEdit()); QVERIFY(document.DrawableMaterialOverrides().isEmpty()); QVERIFY(document.RedoEdit()); QCOMPARE(material(document, "Mesh"), next);
    }
    void structureBakesMaterialsAndUndoRestoresOverrides_data()
    {
        QTest::addColumn<int>("operation"); for (int operation = 0; operation < 4; ++operation) QTest::newRow(qPrintable(QString::number(operation))) << operation;
    }
    void structureBakesMaterialsAndUndoRestoresOverrides()
    {
        QFETCH(int, operation); CModelDocument document; QString error; QVERIFY(load(document, operation == 3, &error)); QVERIFY(change(document, &error));
        const auto before = document.AuthoringStructureState(); const auto oldMaterial = material(document, "Mesh"); CModelEdit edit;
        if (operation == 0) QVERIFY2(document.MakeMeshStructureEdit(reorder(document, true), edit, &error), qPrintable(error));
        if (operation == 1) {
            auto request = reorder(document, true); request.meshes[0].vertices.append({{0, 1}, {.25, .75}});
            QVERIFY2(document.MakeMeshStructureEdit(request, edit, &error), qPrintable(error));
        }
        if (operation == 2) {
            CMocParameterStructureBuilder::CreateRequest request; request.parameter.id = "MaterialTest";
            request.parameter.minimum = -1; request.parameter.maximum = 1; request.parameter.defaultValue = 0;
            QVERIFY2(document.MakeCreateParameterEdit(request, {}, edit, &error), qPrintable(error));
        }
        if (operation == 3) QVERIFY2(document.MakeDeformerStructureEdit(removal(document, "Twin"), edit, &error), qPrintable(error));
        QVERIFY2(document.ExecuteEdit(edit), "structure application"); QVERIFY(document.HasMaterialStructure()); QVERIFY(document.DrawableMaterialOverrides().isEmpty());
        const auto after = document.AuthoringStructureState(); const auto current = material(document, "Mesh"); QCOMPARE(current.textureIndex, oldMaterial.textureIndex);
        if (operation < 2) {
            QCOMPARE(current.uvs[0], oldMaterial.uvs[2]); QCOMPARE(current.uvs[1], oldMaterial.uvs[3]);
            if (operation == 1) QCOMPARE(current.uvs[6], float(.25 * oldMaterial.uvs[0] + .75 * oldMaterial.uvs[2]));
        } else QCOMPARE(current, oldMaterial);
        CModelSession parsed; QVERIFY2(CModelSession::FromJson(materialSession(document).ToJson(), &parsed, &error), qPrintable(error));
        CModelDocument restored; QVERIFY2(parsed.RestoreDocument(document, restored, &error), qPrintable(error)); QCOMPARE(restored.AuthoringStructureState(), after);
        QVERIFY(document.UndoEdit()); QCOMPARE(document.AuthoringStructureState(), before); QVERIFY(document.RedoEdit()); QCOMPARE(document.AuthoringStructureState(), after);
    }
    void materialStructureCannotEnableDeformerDefinitionChanges()
    {
        CModelDocument document; QString error; QVERIFY(load(document)); QVERIFY(change(document));
        auto session = materialSession(document); session.version = 10; session.materialStructure = true;
        auto reader = CReader::FromBytes(document.SourceBytes()); CSectionTable table(reader); QVERIFY(table.IsValid());
        auto angles = document.SourceBytes().mid(table.sectionOffsetTable->rotation_deformers->base_angles, table.rotation_deformers.size() * 4);
        DeformerSnapshotData::real(angles, 0, table.rotation_deformers[0]->base_angles + 1.0f);
        session.structure.columns.insert(0xb0, angles);
        auto output = document; const auto before = output.AuthoringStructureState();
        QVERIFY(!session.RestoreDocument(document, output, &error)); QVERIFY(!error.isEmpty()); QCOMPARE(output.AuthoringStructureState(), before);
    }
    void legacyVersionsDefaultToNoMaterials_data()
    {
        QTest::addColumn<int>("version"); for (int version = 1; version <= 9; ++version) QTest::newRow(qPrintable(QString::number(version))) << version;
    }
    void legacyVersionsDefaultToNoMaterials()
    {
        QFETCH(int, version); CModelDocument document; QVERIFY(load(document)); auto session = materialSession(document); session.version = version;
        CModelSession parsed; QString error; QVERIFY(CModelSession::FromJson(session.ToJson(), &parsed, &error));
        QVERIFY(!parsed.materialStructure); QVERIFY(parsed.drawableMaterials.isEmpty()); QVERIFY(change(document));
        CModelDocument restored; QVERIFY2(parsed.RestoreDocument(document, restored, &error), qPrintable(error)); QVERIFY(restored.DrawableMaterialOverrides().isEmpty());
    }
    void malformedSessionsPreserveOutput_data()
    {
        QTest::addColumn<int>("fault"); for (int fault = 0; fault < 8; ++fault) QTest::newRow(qPrintable(QString::number(fault))) << fault;
    }
    void malformedSessionsPreserveOutput()
    {
        QFETCH(int, fault); CModelDocument document; QVERIFY(load(document)); QVERIFY(change(document));
        auto json = QJsonDocument::fromJson(materialSession(document).ToJson()).object();
        if (fault == 0) json["version"] = 13;
        if (fault == 1) json["version"] = 9;
        if (fault == 2) json.remove("materialStructure");
        if (fault == 3) json["drawableMaterials"] = false;
        if (fault == 4) json["sourceDigest"] = QString();
        auto materials = json["drawableMaterials"].toObject(); auto row = materials["Mesh"].toObject();
        if (fault == 5) row["uvs"] = "00000080";
        if (fault == 6) row["textureIndex"] = .5;
        if (fault == 7) row["uvs"] = "0000807f00000000";
        if (fault >= 5) { materials["Mesh"] = row; json["drawableMaterials"] = materials; }
        CModelSession output; output.sourceMoc = "sentinel"; QString error;
        QVERIFY(!CModelSession::FromJson(QJsonDocument(json).toJson(), &output, &error)); QVERIFY(!error.isEmpty()); QCOMPARE(output.sourceMoc, QString("sentinel"));
    }
};
QTEST_GUILESS_MAIN(MaterialDocumentTests)
#include "material_document_tests.moc"
