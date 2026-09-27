#include "live2dstruct/cartworkreconciler.h"
#include <QtTest>
#include <QBuffer>
#include <QCryptographicHash>
#include <QImage>
#include <cmath>
#include <limits>

using namespace live2dstruct;
using Artwork = CModelArtwork;
using Reconciler = CArtworkReconciler;
namespace {
QByteArray png(QSize size = {8, 8}, QRgb color = qRgba(41, 73, 109, 127), bool metadata = false)
{
    QImage image(size, QImage::Format_ARGB32); image.fill(color); if (metadata) image.setText("note", "same pixels");
    QBuffer buffer; buffer.open(QIODevice::WriteOnly); image.save(&buffer, "PNG"); return buffer.data();
}
struct Fixture {
    CTextureAssets::Catalog sources;
    CModelAtlasState state;
    Reconciler::Triangles triangles;
};
Fixture fixture()
{
    Fixture result;
    for (int slot = 0; slot < 2; ++slot) {
        CTextureAssets::Source source; CTextureAssets::MakeSource(QString("/tmp/artwork-%1.png").arg(slot), png(), source);
        result.sources.insert(slot, source);
    }
    CTextureWorkspace::Initialize(result.sources, {}, result.state.workspace);
    auto& artwork = result.state.artwork; const auto& source = result.sources[0];
    artwork.images.insert(source.digest, {source.size, source.png});
    artwork.families.insert("family", {"family", "素材", Artwork::SourceKind::AtlasSnapshot, "revision"});
    Artwork::Revision revision; revision.id = "revision"; revision.familyId = "family"; revision.canvasSize = source.size;
    revision.sourceBytes = source.png; revision.sourceDigest = source.digest;
    Artwork::Layer layer; layer.id = "layer"; layer.bounds = {0, 0, 8, 8}; layer.imageDigest = source.digest; revision.layers = {layer};
    artwork.revisions.insert(revision.id, revision);
    artwork.materials.insert("material", {"material", "", "family", "revision", "layer", Artwork::Boundary::Repeat, false, {}});
    artwork.placements.insert("placement", {"placement", "material", "source:0", QTransform(), {0, 0, 8, 8}, 1, Artwork::Filter::ExactTexels});
    artwork.placements.insert("unused-original", {"unused-original", "material", "source:1", QTransform(), {0, 0, 8, 8}, 1, Artwork::Filter::ExactTexels});
    for (const QString& id : {QString("A"), QString("B")}) {
        CModelDrawableMaterial material{id, 0, {.125f, .125f, .375f, .125f, .125f, .375f}};
        result.state.materials.append(material); result.triangles.insert(id, {0, 1, 2});
        Artwork::Binding binding; binding.meshId = id; binding.placementId = "placement";
        binding.familyCoordinates = {{1, 1}, {3, 1}, {1, 3}}; binding.sampledMaterial = material;
        Artwork::TopologyDigest(3, {0, 1, 2}, binding.topologyDigest); artwork.bindings.insert(id, binding);
    }
    return result;
}
CModelMaterialState move(const CModelMaterialState& before, const QStringList& ids, int slot, QPointF delta)
{
    auto result = before;
    for (auto& value : result) if (ids.contains(value.id)) {
        value.textureIndex = slot;
        for (int i = 0; i < value.uvs.size(); i += 2) { value.uvs[i] += float(delta.x() / 8); value.uvs[i + 1] += float(delta.y() / 8); }
    }
    return result;
}
}
class ArtworkReconcilerTests : public QObject
{
    Q_OBJECT
private slots:
    void validatesStableAtlasAndExactSampledMaterial_data()
    {
        QTest::addColumn<int>("fault"); for (int i = 0; i < 8; ++i) QTest::newRow(qPrintable(QString::number(i))) << i;
    }
    void validatesStableAtlasAndExactSampledMaterial()
    {
        QFETCH(int, fault); auto f = fixture(); QVERIFY(Reconciler::ValidateLinks(f.state.artwork, f.state.workspace, f.state.materials, f.triangles));
        if (fault == 0) f.state.artwork.bindings["A"].familyCoordinates[0].rx() += .01;
        if (fault == 1) f.state.workspace.atlases[0].id = "other";
        if (fault == 2) f.state.materials[0].uvs[0] += .01f;
        if (fault == 3) f.triangles["A"] = {0, 2, 1};
        if (fault == 4) f.state.materials.removeFirst();
        if (fault == 5) f.triangles.remove("A");
        if (fault == 6) f.state.artwork.bindings["A"].topologyDigest = QByteArray(32, 'x');
        if (fault == 7) f.state.workspace.atlases[1].id = f.state.workspace.atlases[0].id;
        QString error; QVERIFY(!Reconciler::ValidateLinks(f.state.artwork, f.state.workspace, f.state.materials, f.triangles, &error)); QVERIFY(!error.isEmpty());
    }
    void detachedRecordsSurviveCurrentShapeChanges()
    {
        auto f = fixture(); auto& binding = f.state.artwork.bindings["A"]; binding.status = Artwork::BindingStatus::Detached; binding.detachedReason = "保留旧形状";
        f.state.materials[0].uvs = {0, 0}; f.state.materials[0].textureIndex = 1; f.triangles["A"].clear();
        QVERIFY(Reconciler::ValidateLinks(f.state.artwork, f.state.workspace, f.state.materials, f.triangles));
        f.state.materials.removeFirst(); f.triangles.remove("A"); QVERIFY(!Reconciler::ValidateLinks(f.state.artwork, f.state.workspace, f.state.materials, f.triangles));
    }
    void manualUvWithinDomainUpdatesCoordinatesAndTopologyOnly()
    {
        const auto f = fixture(); auto materials = f.state.materials; materials[0].uvs[0] = .25f; auto triangles = f.triangles; triangles["A"] = {0, 2, 1};
        Artwork::State result; QString error;
        QVERIFY2(Reconciler::ManualAppearance(f.sources, f.state, f.state.workspace, materials, triangles, result, &error), qPrintable(error));
        QCOMPARE(result.bindings["A"].familyCoordinates[0], QPointF(2, 1)); QCOMPARE(result.bindings["A"].sampledMaterial, materials[0]);
        QVERIFY(result.bindings["B"] == f.state.artwork.bindings["B"]); QVERIFY(result.placements == f.state.artwork.placements);
        QByteArray hash; QVERIFY(Artwork::TopologyDigest(3, triangles["A"], hash)); QCOMPARE(result.bindings["A"].topologyDigest, hash);
        QVERIFY(Reconciler::ValidateLinks(result, f.state.workspace, materials, triangles));
    }
    void manualChangeDetachesConservatively_data()
    {
        QTest::addColumn<int>("fault"); for (int i = 0; i < 5; ++i) QTest::newRow(qPrintable(QString::number(i))) << i;
    }
    void manualChangeDetachesConservatively()
    {
        QFETCH(int, fault); const auto f = fixture(); auto workspace = f.state.workspace; auto materials = f.state.materials;
        if (fault == 0) workspace.atlases[0].png = png({8, 8}, qRgba(4, 5, 6, 0));
        if (fault == 1) { workspace.atlases[0].size = {16, 8}; workspace.atlases[0].png = png({16, 8}); }
        if (fault == 2) materials[0].uvs[0] = 1.125f;
        if (fault == 3) materials[0].textureIndex = 1;
        if (fault == 4) materials[0].uvs << .5f << .5f;
        Artwork::State result; QString error; QVERIFY2(Reconciler::ManualAppearance(f.sources, f.state, workspace, materials, f.triangles, result, &error), qPrintable(error));
        QCOMPARE(result.bindings["A"].status, Artwork::BindingStatus::Detached); QVERIFY(!result.bindings["A"].detachedReason.isEmpty());
        QCOMPARE(result.bindings["A"].sampledMaterial, f.state.materials[0]); QCOMPARE(result.bindings["A"].familyCoordinates, f.state.artwork.bindings["A"].familyCoordinates);
        QCOMPARE(result.bindings["B"].status, fault <= 1 ? Artwork::BindingStatus::Detached : Artwork::BindingStatus::Linked);
    }
    void equivalentPngEncodingPreservesLinkedSource()
    {
        const auto f = fixture(); auto workspace = f.state.workspace; workspace.atlases[0].png = png({8, 8}, qRgba(41, 73, 109, 127), true);
        QVERIFY(workspace.atlases[0].png != f.sources[0].png); Artwork::State result;
        QVERIFY(Reconciler::ManualAppearance(f.sources, f.state, workspace, f.state.materials, f.triangles, result)); QVERIFY(result == f.state.artwork);
    }
    void sourceBoundarySnappingRequiresIdenticalFloatUv()
    {
        auto f = fixture(); f.state.artwork.placements["placement"].sourceDomain.setWidth(2.4);
        for (auto& material : f.state.materials) {
            material.uvs[2] = .25f; f.state.artwork.bindings[material.id].sampledMaterial = material;
            f.state.artwork.bindings[material.id].familyCoordinates[1].setX(2);
        }
        auto material = f.state.materials; material[0].uvs[0] = .3f; Artwork::State result;
        QVERIFY(Reconciler::ManualAppearance(f.sources, f.state, f.state.workspace, material, f.triangles, result));
        QCOMPARE(result.bindings["A"].status, Artwork::BindingStatus::Linked); QCOMPARE(result.bindings["A"].familyCoordinates[0].x(), 2.4);
        material[0].uvs[0] = std::nextafter(.3f, 1.f);
        QVERIFY(Reconciler::ManualAppearance(f.sources, f.state, f.state.workspace, material, f.triangles, result));
        QCOMPARE(result.bindings["A"].status, Artwork::BindingStatus::Detached);
    }
    void resultOwnsBorrowedSourceAndReplacementMaterial()
    {
        auto f = fixture(); QByteArray bytes = f.state.artwork.revisions["revision"].sourceBytes; auto* raw = bytes.data();
        f.state.artwork.revisions["revision"].sourceBytes = QByteArray::fromRawData(raw, bytes.size());
        QString id = "A"; auto* characters = id.data(); auto materials = f.state.materials;
        materials[0].id = QString::fromRawData(characters, id.size()); materials[0].uvs[0] = .25f;
        Artwork::State result; QVERIFY(Reconciler::ManualAppearance(f.sources, f.state, f.state.workspace, materials, f.triangles, result));
        const auto held = result; raw[0] ^= 1; characters[0] = 'X'; QVERIFY(result == held);
        QCOMPARE(result.bindings["A"].sampledMaterial.id, QString("A")); QVERIFY(Artwork::Validate(result));
    }
    void vertexSourcesInterpolateAndReorderWithoutLosingOldCoordinates()
    {
        const auto f = fixture(); CMocMeshStructureBuilder::MeshEdit edit;
        edit.artMeshId = "A"; edit.vertices = {{{2}, {1}}, {{0}, {1}}, {{0, 1, 2}, {.2, .3, .5}}};
        edit.oldToNew = {1, -1, 0}; edit.indices = {0, 1, 2};
        CModelDrawableMaterial material{"A", 0, {.125f, .375f, .125f, .125f, .2f, .25f}}; Artwork::State result; QString error;
        QVERIFY2(Reconciler::RemapMesh(f.state.artwork, edit, material, edit.indices, result, &error), qPrintable(error));
        QCOMPARE(result.bindings["A"].familyCoordinates[0], QPointF(1, 3)); QCOMPARE(result.bindings["A"].familyCoordinates[1], QPointF(1, 1));
        QCOMPARE(result.bindings["A"].familyCoordinates[2], QPointF(1.6, 2));
        QVERIFY(result.bindings["B"] == f.state.artwork.bindings["B"]);
        edit.vertices[0].weights[0] += 1e-10; QVERIFY(Reconciler::RemapMesh(f.state.artwork, edit, material, edit.indices, result));
    }
    void invalidVertexSourceKeepsResult_data()
    {
        QTest::addColumn<int>("fault"); for (int i = 0; i < 7; ++i) QTest::newRow(qPrintable(QString::number(i))) << i;
    }
    void invalidVertexSourceKeepsResult()
    {
        QFETCH(int, fault); const auto f = fixture(); CMocMeshStructureBuilder::MeshEdit edit{"A", {{{0}, {1}}, {{1}, {1}}, {{2}, {1}}}, {0, 1, 2}, {0, 1, 2}};
        auto material = f.state.materials[0];
        if (fault == 0) edit.vertices[0].weights[0] = -.5;
        if (fault == 1) edit.vertices[0].indices[0] = 4;
        if (fault == 2) edit.vertices[0].weights[0] = .9;
        if (fault == 3) edit.oldToNew[1] = 0;
        if (fault == 4) material.uvs[0] = .126f;
        if (fault == 5) material.textureIndex = 1;
        if (fault == 6) edit.indices = {1, 2, 0};
        auto result = f.state.artwork; QVERIFY(!Reconciler::RemapMesh(f.state.artwork, edit, material, {0, 1, 2}, result)); QVERIFY(result == f.state.artwork);
    }
    void partialSharedMovementForksThenRestoresOriginalIdentity()
    {
        const auto f = fixture(); auto after = f.state; after.materials = move(after.materials, {"A"}, 1, {2, 1});
        const QMap<QString, Reconciler::Rebase> forward{{"A", {QTransform::fromTranslate(2, 1), 1, 1, {0, 0, 4, 4}}}};
        QString error; QVERIFY2(Reconciler::RebasePlacements(f.state.artwork, f.state, after.workspace, after.materials, f.triangles, forward, after.artwork, &error), qPrintable(error));
        QVERIFY(after.artwork.bindings["A"].placementId != after.artwork.bindings["B"].placementId);
        QCOMPARE(after.artwork.bindings["A"].familyCoordinates, f.state.artwork.bindings["A"].familyCoordinates);
        QVERIFY(after.artwork.bindings["B"] == f.state.artwork.bindings["B"]); QVERIFY(after.artwork.placements["placement"] == f.state.artwork.placements["placement"]);
        Artwork::State returned; const QMap<QString, Reconciler::Rebase> backward{{"A", {QTransform::fromTranslate(-2, -1), 0, 1, {2, 1, 4, 4}}}};
        QVERIFY2(Reconciler::RebasePlacements(f.state.artwork, after, f.state.workspace, f.state.materials, f.triangles, backward, returned, &error), qPrintable(error));
        QVERIFY(returned == f.state.artwork);
    }
    void returningOneOfSharedMovedMeshesProtectsTheOther()
    {
        const auto f = fixture(); auto moved = f.state; moved.materials = move(moved.materials, {"A", "B"}, 1, {2, 1});
        QMap<QString, Reconciler::Rebase> forward{{"A", {QTransform::fromTranslate(2, 1), 1, 1, {0, 0, 4, 4}}}, {"B", {QTransform::fromTranslate(2, 1), 1, 1, {0, 0, 4, 4}}}};
        QVERIFY(Reconciler::RebasePlacements(f.state.artwork, f.state, moved.workspace, moved.materials, f.triangles, forward, moved.artwork));
        auto partial = moved; partial.materials = move(partial.materials, {"A"}, 0, {-2, -1});
        QMap<QString, Reconciler::Rebase> backward{{"A", {QTransform::fromTranslate(-2, -1), 0, 1, {2, 1, 4, 4}}}};
        QString error; QVERIFY2(Reconciler::RebasePlacements(f.state.artwork, moved, partial.workspace, partial.materials, f.triangles, backward, partial.artwork, &error), qPrintable(error));
        QCOMPARE(partial.artwork.bindings["B"].sampledMaterial, moved.materials[1]);
        QVERIFY(partial.artwork.bindings["A"].placementId != partial.artwork.bindings["B"].placementId);
        backward.insert("B", backward.take("A")); Artwork::State returned;
        QVERIFY2(Reconciler::RebasePlacements(f.state.artwork, partial, f.state.workspace, f.state.materials, f.triangles, backward, returned, &error), qPrintable(error));
        QVERIFY(returned == f.state.artwork);
    }
    void noGuessingMissingTransformsOrChangedTopology()
    {
        const auto f = fixture(); const auto changed = move(f.state.materials, {"A"}, 1, {2, 1}); auto result = f.state.artwork;
        QVERIFY(!Reconciler::RebasePlacements(f.state.artwork, f.state, f.state.workspace, changed, f.triangles, {}, result)); QVERIFY(result == f.state.artwork);
        auto triangles = f.triangles; triangles["A"] = {0, 2, 1};
        QVERIFY(!Reconciler::RebasePlacements(f.state.artwork, f.state, f.state.workspace, f.state.materials, triangles, {}, result));
        auto source = f.state.artwork; source.families["family"].name = "changed";
        QVERIFY(!Reconciler::RebasePlacements(source, f.state, f.state.workspace, f.state.materials, f.triangles, {}, result));
    }
};
QTEST_GUILESS_MAIN(ArtworkReconcilerTests)
#include "artwork_reconciler_tests.moc"
