#include "live2dstruct/cmodeldocument.h"
#include "live2dstruct/cmodelsession.h"
#include "live2dstruct/cmodelsavefingerprint.h"
#include "live2dstruct/catlasrepackplanner.h"
#include "deformer_replay_fixture.h"
#include <QtTest>
#include <QBuffer>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLineF>
#include <QTemporaryDir>
#include <cmath>

using namespace live2dstruct;
namespace {
using Artwork = CModelArtwork;
using Planner = CAtlasRepackPlanner;
using Meshes = CMocMeshStructureBuilder;
using Parameters = CMocParameterStructureBuilder;
QByteArray png(QSize size = QSize(64, 32), QColor color = QColor(31, 61, 91, 177)) {
    QImage image(size, QImage::Format_RGBA8888); image.fill(color); QBuffer buffer;
    if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "PNG")) return {}; return buffer.data();
}
QByteArray hash(const QByteArray& value) { return QCryptographicHash::hash(value, QCryptographicHash::Sha256); }
QByteArray moc(const CModelDocument& document) { QByteArray result; document.BuildRuntimeMoc(result); return result; }
QByteArray saved(const CModelDocument& document) { QByteArray result; CModelSaveFingerprint::Capture(document, result); return result; }
CModelAtlasState state(const CModelDocument& document) { CModelAtlasState result; document.CaptureAtlasState(result); return result; }
bool execute(CModelDocument& document, const CModelAtlasState& next, QString* error) {
    CModelAtlasState before; CModelEdit edit;
    return document.CaptureAtlasState(before, error) && document.MakeAtlasEdit(before, next, edit, error) && document.ExecuteEdit(edit);
}
CModelSession session(const CModelDocument& document) {
    CModelSession value; value.version = 12; value.sourceMoc = document.SourcePath(); value.sourceDigest = document.SourceDigest();
    value.structure = document.ParameterStructure(); value.meshStructure = document.HasMeshStructure(); value.materialStructure = document.HasMaterialStructure();
    value.drawableMaterials = document.DrawableMaterialOverrides(); value.deformerStructure = document.HasDeformerStructure();
    if (value.deformerStructure) value.sourceDeformerIds = document.SourceDeformerIds();
    for (auto it = document.ParameterValues().cbegin(); it != document.ParameterValues().cend(); ++it) value.parameters.insert(it.key(), it.value());
    value.parameterMetadata = document.ParameterMetadataOverrides(); value.parameterOrganization = document.ParameterOrganization();
    value.geometryObjectPairs = document.GeometryObjectPairs(); value.visibility = document.VisibilityState();
    for (const auto& key : document.LockedKeys()) value.locked.insert(key, true);
    value.drawableOpacity = document.DrawableOpacityOverrides(); value.drawableDrawOrder = document.DrawableDrawOrderOverrides();
    value.drawableKeyformPositions = document.DrawableKeyformPositionOverrides(); value.drawableIndices = document.DrawableIndexOverrides();
    value.drawableMaskIndices = document.DrawableMaskIndexOverrides(); value.drawableMaskInverted = document.DrawableMaskInvertedOverrides();
    value.deformerParentIndices = document.DeformerParentOverrides(); value.deformerRotationKeyforms = document.DeformerRotationKeyformOverrides();
    value.deformerWarpKeyforms = document.DeformerWarpKeyformOverrides(); value.artwork = document.Artwork();
    CTextureWorkspace::Draft workspace;
    if (CTextureWorkspace::CaptureDraft(document.TextureSources(), document.TextureWorkspace(), workspace)) value.textureWorkspace = workspace;
    return value;
}
struct Fixture {
    QTemporaryDir directory;
    CModelDocument document;
    QString meshId, error;
    int slot = -1;
    bool initialize() {
        if (!directory.isValid() || !document.Load(QString(TEST_RESOURCES) + "/Mark/Mark.moc3", &error)) return false;
        CModelMaterialState materials; if (!document.CaptureDrawableMaterials(materials, &error)) return false;
        CTextureAssets::Catalog sources; const auto bytes = png();
        for (const auto& material : materials) if (!material.uvs.isEmpty() && !sources.contains(material.textureIndex)) {
            const auto path = directory.filePath(QString("texture-%1.png").arg(material.textureIndex)); QFile file(path);
            if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) return false; file.close();
            CTextureAssets::Source source; if (!CTextureAssets::MakeSource(path, bytes, source, &error)) return false;
            sources.insert(material.textureIndex, source);
        }
        if (!document.InitializeTextureWorkspace(sources, {}, &error)) return false;
        for (const auto& mesh : document.Drawables()) if (mesh.vertexCount >= 3 && mesh.indices.size() >= 6 && !mesh.keyforms.isEmpty()) {
            CModelDrawableMaterial material; if (!document.CaptureDrawableMaterial(mesh.id, material, &error)) return false;
            meshId = mesh.id; slot = material.textureIndex; break;
        }
        document.ClearEditHistory(); return !meshId.isEmpty();
    }
    Artwork::State library(bool linked, QSize size = QSize(64, 32)) {
        Artwork::State result; const auto bytes = png(size); const auto digest = hash(bytes);
        const auto path = directory.filePath("source.png"); QFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) return {}; file.close();
        result.images.insert(digest, {size, bytes});
        result.families.insert("family", {"family", "Original PNG", Artwork::SourceKind::Png, "revision-one"});
        Artwork::Revision revision; revision.id = "revision-one"; revision.familyId = "family"; revision.path = path; revision.fileName = "source.png";
        revision.sourceDigest = digest; revision.sourceBytes = bytes; revision.canvasSize = size;
        Artwork::Layer layer; layer.id = "png-layer"; layer.name = "Original pixels"; layer.sourceIndex = 0; layer.bounds = QRect(QPoint(), size); layer.imageDigest = digest;
        revision.layers = {layer}; result.revisions.insert(revision.id, revision);
        if (!linked) return result;
        result.materials.insert("material", {"material", "Source", "family", revision.id, layer.id, Artwork::Boundary::Repeat, true, {}});
        result.placements.insert("placement", {"placement", "material", document.TextureWorkspace().atlases[slot].id,
            QTransform(), QRectF(QPointF(), size), 1, Artwork::Filter::ExactTexels});
        Artwork::Binding binding; binding.meshId = meshId; binding.placementId = "placement";
        document.CaptureDrawableMaterial(meshId, binding.sampledMaterial, &error);
        const auto atlasSize = document.TextureWorkspace().atlases[slot].size;
        for (int index = 0; index < binding.sampledMaterial.uvs.size(); index += 2)
            binding.familyCoordinates.append({double(binding.sampledMaterial.uvs[index]) * atlasSize.width(), double(binding.sampledMaterial.uvs[index + 1]) * atlasSize.height()});
        Artwork::TopologyDigest(binding.familyCoordinates.size(), document.DrawableIndices(meshId), binding.topologyDigest, &error);
        result.bindings.insert(meshId, binding); return result;
    }
    bool attach(QSize size = QSize(64, 32)) {
        auto next = state(document); next.artwork = library(true, size);
        if (next.artwork.IsEmpty() || !execute(document, next, &error)) return false;
        document.ClearEditHistory(); return true;
    }
    Planner::Snapshot snapshot() const {
        Planner::Snapshot value; value.sourceDigest = document.SourceDigest(); value.documentRevision = document.Revision(); value.topologyDigest = hash(moc(document));
        value.sources = document.TextureSources(); value.workspace = document.TextureWorkspace(); value.artwork = document.Artwork();
        const auto current = state(document);
        for (const auto& material : current.materials) value.meshes.append({material, document.DrawableIndices(material.id), true, {}});
        return value;
    }
};
Meshes::MeshEdit identity(const CModelDocument& document, const QString& id) {
    Meshes::MeshEdit result; result.artMeshId = id; CModelDrawableMaterial material; document.CaptureDrawableMaterial(id, material);
    for (int index = 0; index < material.uvs.size() / 2; ++index) { result.vertices.append({{index}, {1}}); result.oldToNew.append(index); }
    result.indices = document.DrawableIndices(id); return result;
}
}
class ArtworkDocumentTests : public QObject {
    Q_OBJECT
private slots:
    void libraryImportIsOneDirtyHistoryEntryWithoutChangingMoc() {
        Fixture f; QVERIFY2(f.initialize(), qPrintable(f.error)); const auto original = state(f.document); const auto originalMoc = moc(f.document);
        const auto baseline = saved(f.document); QVERIFY(!baseline.isEmpty()); auto next = original; next.artwork = f.library(false);
        QVERIFY2(execute(f.document, next, &f.error), qPrintable(f.error)); QVERIFY(f.document.CanUndoEdit()); QVERIFY(saved(f.document) != baseline);
        QCOMPARE(moc(f.document), originalMoc); QVERIFY(f.document.Artwork() == next.artwork);
        CModelEdit edit; QVERIFY(f.document.PeekUndoEdit(&edit)); QCOMPARE(edit.field, CModelEdit::AtlasLayout);
        QVERIFY(f.document.UndoEdit()); QVERIFY(state(f.document) == original); QCOMPARE(saved(f.document), baseline); QVERIFY(!f.document.CanUndoEdit());
        QVERIFY(f.document.RedoEdit()); QVERIFY(state(f.document) == next); QCOMPARE(moc(f.document), originalMoc);
    }
    void linkedArtworkFollowsAtlasPlansAndReturnsToItsOriginalPlacement() {
        Fixture f; QVERIFY2(f.initialize() && f.attach(), qPrintable(f.error)); const auto initial = state(f.document); const auto sources = f.document.TextureSources();
        const auto snapshot = f.snapshot(); Planner::Plan original, added, moved, resized, packed, returned, removed;
        QVERIFY2(Planner::Initialize(snapshot, original, &f.error), qPrintable(f.error));
        QRect originalBounds; int originalSlot = -1;
        QVERIFY2(Planner::SelectionBounds(snapshot, original, {f.meshId}, 1, originalBounds, originalSlot, &f.error), qPrintable(f.error));
        QVERIFY2(Planner::AddAtlas(snapshot, original, "Destination", {256, 256}, added, &f.error), qPrintable(f.error));
        const int target = added.state.workspace.atlases.lastKey();
        QVERIFY2(Planner::Move(snapshot, added, {f.meshId}, target, {10, 12}, 1, 1, moved, &f.error), qPrintable(f.error));
        QVERIFY2(execute(f.document, moved.state, &f.error), qPrintable(f.error));
        QCOMPARE(f.document.Artwork().bindings[f.meshId].status, Artwork::BindingStatus::Linked);
        QCOMPARE(f.document.Artwork().bindings[f.meshId].familyCoordinates, initial.artwork.bindings[f.meshId].familyCoordinates);
        QVERIFY2(Planner::ResizeAtlas(snapshot, moved, target, {320, 288}, resized, &f.error), qPrintable(f.error));
        QVERIFY2(execute(f.document, resized.state, &f.error), qPrintable(f.error));
        QVERIFY2(Planner::Pack(snapshot, resized, {f.meshId}, {target}, 1, true, packed, &f.error), qPrintable(f.error));
        QVERIFY2(execute(f.document, packed.state, &f.error), qPrintable(f.error));
        QVERIFY2(Planner::Move(snapshot, moved, {f.meshId}, originalSlot, originalBounds.topLeft(), 3, 1, returned, &f.error), qPrintable(f.error));
        QVERIFY2(Planner::RemoveAtlas(snapshot, returned, target, removed, &f.error), qPrintable(f.error));
        QVERIFY2(execute(f.document, removed.state, &f.error), qPrintable(f.error));
        QVERIFY(f.document.Artwork() == initial.artwork); QCOMPARE(f.document.TextureSources(), sources);
        QCOMPARE(f.document.Artwork().revisions["revision-one"].sourceBytes, initial.artwork.revisions["revision-one"].sourceBytes);
        while (f.document.CanUndoEdit()) QVERIFY(f.document.UndoEdit()); QVERIFY(state(f.document) == initial);
        while (f.document.CanRedoEdit()) QVERIFY(f.document.RedoEdit()); QVERIFY(f.document.Artwork() == initial.artwork);
    }
    void manualPngAndUvChangesPreserveOrDetachLinksAndUndoRestoresThem() {
        Fixture f; QVERIFY2(f.initialize() && f.attach(), qPrintable(f.error)); const auto initial = state(f.document);
        auto changed = initial; changed.workspace.atlases[f.slot].png = png({64, 32}, Qt::red);
        QVERIFY2(execute(f.document, changed, &f.error), qPrintable(f.error));
        QCOMPARE(f.document.Artwork().bindings[f.meshId].status, Artwork::BindingStatus::Detached);
        QVERIFY(!f.document.Artwork().bindings[f.meshId].detachedReason.isEmpty()); QVERIFY(f.document.UndoEdit()); QVERIFY(f.document.Artwork() == initial.artwork);
        CModelDrawableMaterial before; QVERIFY(f.document.CaptureDrawableMaterial(f.meshId, before)); auto after = before; after.uvs[0] = .25f; after.uvs[1] = .25f;
        CModelEdit edit; QVERIFY(f.document.MakeMaterialEdit({before}, {after}, edit, &f.error)); QVERIFY(f.document.ExecuteEdit(edit));
        QCOMPARE(f.document.Artwork().bindings[f.meshId].status, Artwork::BindingStatus::Linked);
        QCOMPARE(f.document.Artwork().bindings[f.meshId].familyCoordinates[0], QPointF(16, 8));
        QVERIFY(f.document.UndoEdit()); QVERIFY(f.document.Artwork() == initial.artwork);
        after = before; after.uvs[0] = 2.0f;
        QVERIFY(f.document.MakeMaterialEdit({before}, {after}, edit, &f.error)); QVERIFY(f.document.ExecuteEdit(edit));
        QCOMPARE(f.document.Artwork().bindings[f.meshId].status, Artwork::BindingStatus::Detached); QVERIFY(f.document.UndoEdit());
        changed = state(f.document); const int slot = changed.workspace.atlases.lastKey() + 1;
        changed.workspace.atlases.insert(slot, {"other-atlas", "Other", -1, {64, 32}, png()});
        for (auto& material : changed.materials) if (material.id == f.meshId) material.textureIndex = slot;
        QVERIFY2(execute(f.document, changed, &f.error), qPrintable(f.error)); QCOMPARE(f.document.Artwork().bindings[f.meshId].status, Artwork::BindingStatus::Detached);
        QVERIFY(f.document.UndoEdit()); QVERIFY(f.document.Artwork() == initial.artwork);
    }
    void topologyAndInsertedVerticesMigrateCoordinatesAndAllKeyforms() {
        Fixture f; QVERIFY2(f.initialize() && f.attach(), qPrintable(f.error)); const auto initial = f.document.Artwork();
        auto indices = f.document.DrawableIndices(f.meshId), shorter = indices; shorter.resize(shorter.size() - 3);
        CModelEdit topology{f.meshId, "artmesh", CModelEdit::MeshIndices, QVariant::fromValue(indices), QVariant::fromValue(shorter), -1, false};
        QVERIFY(f.document.ExecuteEdit(topology)); QCOMPARE(f.document.Artwork().bindings[f.meshId].familyCoordinates, initial.bindings[f.meshId].familyCoordinates);
        QByteArray digest; QVERIFY(Artwork::TopologyDigest(initial.bindings[f.meshId].familyCoordinates.size(), shorter, digest)); QCOMPARE(f.document.Artwork().bindings[f.meshId].topologyDigest, digest);
        QVERIFY(f.document.UndoEdit()); QVERIFY(f.document.Artwork() == initial);
        QVector<QVector<float>> forms;
        for (const auto& mesh : f.document.Drawables()) if (mesh.id == f.meshId)
            for (int key = 0; key < mesh.keyforms.size(); ++key) forms.append(f.document.DrawableKeyformPositions(f.meshId, key));
        auto mesh = identity(f.document, f.meshId); mesh.vertices.append({{0, 1}, {.25, .75}}); Meshes::Request request; request.meshes.append(mesh); CModelEdit edit;
        QVERIFY2(f.document.MakeMeshStructureEdit(request, edit, &f.error), qPrintable(f.error)); QVERIFY(f.document.ExecuteEdit(edit));
        const auto expected = initial.bindings[f.meshId].familyCoordinates[0] * .25 + initial.bindings[f.meshId].familyCoordinates[1] * .75;
        const auto& points = f.document.Artwork().bindings[f.meshId].familyCoordinates;
        QCOMPARE(points.size(), initial.bindings[f.meshId].familyCoordinates.size() + 1); QVERIFY(QLineF(points.last(), expected).length() < 1e-8);
        for (int key = 0; key < forms.size(); ++key) {
            const auto next = f.document.DrawableKeyformPositions(f.meshId, key); QCOMPARE(next.size(), forms[key].size() + 2);
            QCOMPARE(next.mid(0, forms[key].size()), forms[key]);
            for (int axis = 0; axis < 2; ++axis) QVERIFY(std::abs(next[forms[key].size() + axis] - (forms[key][axis] * .25 + forms[key][axis + 2] * .75)) < 1e-5);
        }
        const auto expanded = f.document.Artwork(); QVERIFY(f.document.UndoEdit()); QVERIFY(f.document.Artwork() == initial);
        QVERIFY(f.document.RedoEdit()); QVERIFY(f.document.Artwork() == expanded);
    }
    void sessionsRetainOldRevisionPinsAndRejectBadReferencesAtomically() {
        Fixture f; QVERIFY2(f.initialize() && f.attach(), qPrintable(f.error)); const auto baseline = f.document;
        auto next = state(f.document); auto revision = next.artwork.revisions["revision-one"];
        revision.id = "revision-two"; revision.sourceBytes = png({64, 32}, Qt::blue); revision.sourceDigest = hash(revision.sourceBytes);
        revision.layers[0].imageDigest = revision.sourceDigest; next.artwork.images.insert(revision.sourceDigest, {{64, 32}, revision.sourceBytes});
        next.artwork.revisions.insert(revision.id, revision); next.artwork.families["family"].currentRevision = revision.id;
        QVERIFY2(execute(f.document, next, &f.error), qPrintable(f.error));
        const auto captured = session(f.document); const auto json = captured.ToJson(); QVERIFY(!json.isEmpty()); QCOMPARE(QJsonDocument::fromJson(json).object()["version"].toInt(), 12);
        CModelSession parsed; QVERIFY2(CModelSession::FromJson(json, &parsed, &f.error), qPrintable(f.error));
        QCOMPARE(parsed.artwork.materials["material"].revisionId, QString("revision-one")); QCOMPARE(parsed.artwork.families["family"].currentRevision, QString("revision-two"));
        CModelDocument restored; QVERIFY2(parsed.RestoreDocument(baseline, restored, &f.error), qPrintable(f.error));
        QVERIFY(state(restored) == state(f.document)); QCOMPARE(moc(restored), moc(f.document)); QVERIFY(!restored.CanUndoEdit());
        auto broken = parsed; broken.artwork.materials["material"].layerId = "missing"; auto sentinel = baseline;
        QVERIFY(!broken.RestoreDocument(f.document, sentinel, &f.error)); QVERIFY(state(sentinel) == state(baseline));
        auto root = QJsonDocument::fromJson(json).object(); auto artwork = root["artwork"].toObject(); auto materials = artwork["materials"].toObject(); QVERIFY(materials.contains("material"));
        auto row = materials["material"].toObject(); row["layerId"] = "missing"; materials["material"] = row; artwork["materials"] = materials; root["artwork"] = artwork;
        auto unchanged = parsed; QVERIFY(!CModelSession::FromJson(QJsonDocument(root).toJson(), &unchanged, &f.error)); QVERIFY(unchanged.artwork == parsed.artwork);
    }
    void allLegacySessionVersionsRestoreWithoutInventingSourceLinks() {
        Fixture f; QVERIFY2(f.initialize(), qPrintable(f.error)); const auto original = f.document; QVERIFY2(f.attach(), qPrintable(f.error));
        for (int version = 1; version <= 11; ++version) {
            auto old = session(original); old.version = version; old.artwork = {};
            if (version < 11) old.textureWorkspace.reset();
            const auto bytes = old.ToJson(); QVERIFY2(!bytes.isEmpty(), qPrintable(QString::number(version)));
            CModelSession parsed; QVERIFY2(CModelSession::FromJson(bytes, &parsed, &f.error), qPrintable(f.error)); QCOMPARE(parsed.version, version);
            CModelDocument restored; QVERIFY2(parsed.RestoreDocument(f.document, restored, &f.error), qPrintable(QString("v%1: %2").arg(version).arg(f.error)));
            QVERIFY(restored.Artwork().IsEmpty()); QCOMPARE(moc(restored), moc(original));
        }
    }
    void parameterAndDeformerStructureEditsKeepSourcesAndRejectInjectedArtworkChanges() {
        Fixture f; QVERIFY2(f.initialize() && f.attach(), qPrintable(f.error)); const auto artwork = f.document.Artwork(); QString error; CModelEdit edit;
        Parameters::CreateRequest create; create.parameter.id = "ParamArtwork";
        QVERIFY2(f.document.MakeCreateParameterEdit(create, "Artwork parameter", edit, &error), qPrintable(error));
        auto injected = edit; auto after = injected.after.toMap(); auto forged = after["artwork"].value<Artwork::State>();
        forged.families["family"].name = "Injected"; after["artwork"] = QVariant::fromValue(forged); injected.after = after;
        QVERIFY(!f.document.ExecuteEdit(injected)); QVERIFY(f.document.Artwork() == artwork);
        QVERIFY(f.document.ExecuteEdit(edit)); QVERIFY(f.document.Artwork() == artwork);
        QVERIFY2(f.document.MakeBindParameterEdit(create.parameter.id, {{{Parameters::Owner::ArtMesh, f.meshId}, {-1, 0, 1}}}, edit, &error), qPrintable(error));
        QVERIFY(f.document.ExecuteEdit(edit)); QVERIFY(f.document.Artwork() == artwork);
        QVERIFY2(f.document.MakeDeleteParameterEdit(Parameters::DeleteRequest{create.parameter.id, 0}, edit, &error), qPrintable(error)); QVERIFY(f.document.ExecuteEdit(edit)); QVERIFY(f.document.Artwork() == artwork);
        const auto bytes = moc(f.document); auto add = DeformerReplayFixture::rootMesh(bytes, false); add.id = "ArtworkRotation"; QVERIFY(!add.children.isEmpty());
        CDeformerPreserveRequest request{add, f.document.SourceDigest(), hash(bytes)};
        QVERIFY2(f.document.MakeDeformerStructureEdit(request, edit, &error), qPrintable(error)); QVERIFY(f.document.ExecuteEdit(edit)); QVERIFY(f.document.Artwork() == artwork);
        QByteArray textureFingerprint;
        QVERIFY(CTextureWorkspace::Fingerprint(f.document.TextureSources(), f.document.TextureWorkspace(), textureFingerprint, &error));
        QCOMPARE(f.document.TextureWorkspaceFingerprint(), textureFingerprint);
        const auto captured = session(f.document); CModelSession parsed; QVERIFY(CModelSession::FromJson(captured.ToJson(), &parsed, &error));
        CModelDocument restored; QVERIFY2(parsed.RestoreDocument(f.document, restored, &error), qPrintable(error)); QVERIFY(restored.Artwork() == artwork);
        while (f.document.CanUndoEdit()) { QVERIFY(f.document.UndoEdit()); QVERIFY(f.document.Artwork() == artwork); }
        while (f.document.CanRedoEdit()) { QVERIFY(f.document.RedoEdit()); QVERIFY(f.document.Artwork() == artwork); }
    }
    void sharedSourceSurvivesRepeatedUvMetadataHistoryWithMeasuredCost() {
        Fixture f; QVERIFY2(f.initialize() && f.attach({2048, 2048}), qPrintable(f.error)); const auto source = f.document.Artwork().revisions["revision-one"].sourceBytes;
        QElapsedTimer timer; timer.start();
        for (int iteration = 0; iteration < 6; ++iteration) {
            CModelDrawableMaterial before; QVERIFY(f.document.CaptureDrawableMaterial(f.meshId, before)); auto after = before;
            after.uvs[0] = .25f + iteration * .01f; CModelEdit edit; QVERIFY(f.document.MakeMaterialEdit({before}, {after}, edit, &f.error)); QVERIFY(f.document.ExecuteEdit(edit));
            auto metadata = state(f.document); metadata.artwork.families["family"].name = QString("Source %1").arg(iteration);
            QVERIFY2(execute(f.document, metadata, &f.error), qPrintable(f.error)); QCOMPARE(f.document.Artwork().images.size(), 1);
            QCOMPARE(f.document.Artwork().revisions["revision-one"].sourceBytes, source);
        }
        qInfo().noquote() << QString("shared_source_rgba_bytes=16777216 edits=12 elapsed_ms=%1").arg(timer.elapsed());
        for (int index = 0; index < 12; ++index) QVERIFY(f.document.UndoEdit()); QVERIFY(!f.document.CanUndoEdit());
        for (int index = 0; index < 12; ++index) QVERIFY(f.document.RedoEdit()); QVERIFY(!f.document.CanRedoEdit());
        QCOMPARE(f.document.Artwork().revisions["revision-one"].sourceBytes, source);
    }
};
QTEST_GUILESS_MAIN(ArtworkDocumentTests)
#include "artwork_document_tests.moc"
