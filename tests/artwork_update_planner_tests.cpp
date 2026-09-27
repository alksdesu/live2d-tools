#include "live2dstruct/cartworkupdateplanner.h"
#include "live2dstruct/cartworkreconciler.h"
#include "std/cexportcontrol.h"
#include <QtTest>
#include <QBuffer>
#include <QCryptographicHash>
#include <QImage>
#include <cmath>
#include <limits>

using namespace live2dstruct;
using Planner = CArtworkUpdatePlanner;
using Artwork = CModelArtwork;
namespace {
QByteArray hash(const QByteArray& bytes) { return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256); }
QImage pattern(QRect bounds)
{
    QImage image(bounds.size(), QImage::Format_RGBA8888);
    for (int y = 0; y < image.height(); ++y) for (int x = 0; x < image.width(); ++x) {
        auto* pixel = image.scanLine(y) + x * 4; const int dx = x + bounds.x(), dy = y + bounds.y();
        pixel[0] = uchar(80 + dx * 9); pixel[1] = uchar(90 + dy * 13); pixel[2] = uchar(120 + dx * 3 + dy * 2); pixel[3] = 255;
    }
    return image;
}
QByteArray png(const QImage& image)
{ QBuffer buffer; buffer.open(QIODevice::WriteOnly); image.save(&buffer, "PNG"); return buffer.data(); }
QByteArray pixel(const QImage& image, QPointF point)
{ return QByteArray(reinterpret_cast<const char*>(image.constScanLine(int(std::floor(point.y()))) + int(std::floor(point.x())) * 4), 4); }
Planner::Snapshot snapshot()
{
    Planner::Snapshot result; result.sourceDigest = hash("model source"); result.documentRevision = 7;
    CTextureAssets::Source source; CTextureAssets::MakeSource("/tmp/artwork-update.png", png(pattern({0, 0, 8, 8})), source);
    result.sources.insert(0, source); CTextureWorkspace::Initialize(result.sources, {}, result.baseline.workspace);
    for (const QString& id : {QString("A"), QString("B")}) {
        CAtlasRepackPlanner::Mesh mesh; mesh.material = {id, 0, {.125f, .125f, .375f, .125f, .375f, .375f, .125f, .375f}};
        mesh.triangles = {0, 1, 2, 0, 2, 3}; result.meshes.append(mesh); result.baseline.materials.append(mesh.material);
    }
    return result;
}
CArtworkSourceImport::SourceCandidate incoming(QString id = "revision", QRect bounds = {0, 0, 4, 4})
{
    CArtworkSourceImport::SourceCandidate result; auto& revision = result.revision; revision.id = id; revision.familyId = "family";
    revision.path = "/tmp/source.psd"; revision.fileName = "source.psd"; revision.sourceBytes = "original PSD bytes " + id.toUtf8(); revision.sourceDigest = hash(revision.sourceBytes);
    revision.canvasSize = {16, 16}; Artwork::Layer layer; layer.id = "record:0"; layer.name = "图层"; layer.bounds = bounds; layer.externalId = 17;
    if (!bounds.isEmpty()) { const auto pixels = png(pattern(bounds)); layer.imageDigest = hash(pixels); result.images.insert(layer.imageDigest, {bounds.size(), pixels}); }
    revision.layers = {layer}; result.diagnostics = {"来源像素已冻结"}; return result;
}
Planner::Request request(const QStringList& ids = {"A"}, bool confirmed = true)
{
    Planner::Request result; result.incoming = incoming(); result.familyName = "原稿";
    for (const auto& id : ids) result.decisions.append({id, Planner::Action::Associate, "record:0", QTransform(), confirmed, true});
    return result;
}
Planner::Snapshot accept(const Planner::Snapshot& before, const Planner::Plan& plan)
{
    auto result = before; result.baseline = plan.state; ++result.documentRevision;
    for (auto& mesh : result.meshes) for (const auto& material : result.baseline.materials) if (material.id == mesh.material.id) mesh.material = material;
    return result;
}
Artwork::Placement placement(const Planner::Plan& plan, const QString& id = "A")
{ return plan.state.artwork.placements[plan.state.artwork.bindings[id].placementId]; }
}

class ArtworkUpdatePlannerTests : public QObject
{
    Q_OBJECT
private slots:
    void firstAssociationKeepsUnselectedPixelsAndNeedsSealedConfirmation()
    {
        const auto source = snapshot(); const auto held = source; auto edit = request({"A"}, false); Planner::Plan plan; QString error;
        QVERIFY2(Planner::Build(source, edit, plan, &error), qPrintable(error)); QVERIFY(Planner::Validate(source, plan));
        QVERIFY(!Planner::Validate(source, plan, &error, true)); QCOMPARE(plan.unconfirmed, QSet<QString>{"A"});
        auto tampered = plan; tampered.unconfirmed.clear(); QVERIFY(!Planner::Validate(source, tampered, &error, true));
        QCOMPARE(plan.state.materials[1], source.baseline.materials[1]); QCOMPARE(plan.state.workspace.atlases[0], source.baseline.workspace.atlases[0]);
        QCOMPARE(source.baseline, held.baseline); QCOMPARE(source.sources, held.sources); QCOMPARE(plan.state.workspace.atlases.size(), 2);
        edit.decisions[0].confirmed = true; Planner::Plan confirmed; QVERIFY(Planner::Build(source, edit, confirmed)); QVERIFY(Planner::Validate(source, confirmed, &error, true));
        QCOMPARE(confirmed.state, plan.state); QVERIFY(confirmed.token != plan.token);
    }
    void negativeLayerBoundsAndRevisionAlignmentUseExplicitPointOrder()
    {
        const auto source = snapshot(); auto edit = request(); edit.incoming = incoming("aligned", {-2, -1, 4, 4});
        edit.decisions[0].atlasToDocument = QTransform::fromTranslate(-2, -1);
        edit.incoming.revision.documentToFamily = QTransform(2, 0, 0, 2, 10, 20);
        Planner::Plan plan; QString error; QVERIFY2(Planner::Build(source, edit, plan, &error), qPrintable(error));
        const auto& points = plan.state.artwork.bindings["A"].familyCoordinates;
        QCOMPARE(points[0], QPointF(8, 20)); QCOMPARE(points[1], QPointF(12, 20));
        QCOMPARE(placement(plan).familyToAtlas.m11(), .5); QCOMPARE(placement(plan).familyToAtlas.m22(), .5);
        QCOMPARE(plan.images[1].size(), QSize(6, 6));
        const auto mapped = placement(plan).familyToAtlas.map(QPointF(9, 21));
        QCOMPARE(pixel(plan.images[1], mapped), pixel(pattern({-2, -1, 4, 4}), {1.5, 1.5}));
    }
    void expandedBoundsKeepOldCoordinatesAndPixelDensity()
    {
        const auto source = snapshot(); Planner::Plan first; QVERIFY(Planner::Build(source, request(), first));
        const auto before = accept(source, first); auto edit = request(); edit.incoming = incoming("expanded", {-2, -1, 8, 6});
        edit.decisions[0].action = Planner::Action::Update; edit.decisions[0].confirmed = false;
        Planner::Plan updated; QString error; QVERIFY2(Planner::Build(before, edit, updated, &error), qPrintable(error));
        QVERIFY(updated.unconfirmed.isEmpty()); QVERIFY(Planner::Validate(before, updated, &error, true));
        QCOMPARE(updated.state.artwork.bindings["A"].familyCoordinates, first.state.artwork.bindings["A"].familyCoordinates);
        QCOMPARE(placement(updated).familyToAtlas.m11(), placement(first).familyToAtlas.m11());
        QCOMPARE(updated.images[2].size(), QSize(10, 8));
        QCOMPARE(pixel(updated.images[2], placement(updated).familyToAtlas.map(QPointF(1.5, 1.5))), pixel(first.images[1], placement(first).familyToAtlas.map(QPointF(1.5, 1.5))));
        QVERIFY(updated.state.artwork.revisions.contains("revision")); QVERIFY(updated.state.artwork.revisions.contains("expanded"));
        QVERIFY(updated.warnings.join('\n').contains("非透明源像素中心")); QCOMPARE(updated.state.workspace.atlases[1], first.state.workspace.atlases[1]);
    }
    void partialUpdateForksSharedMaterialAndPlacement()
    {
        const auto source = snapshot(); Planner::Plan first; QVERIFY(Planner::Build(source, request({"A", "B"}), first));
        QCOMPARE(first.state.artwork.bindings["A"].placementId, first.state.artwork.bindings["B"].placementId); QCOMPARE(first.images.size(), 2);
        const auto before = accept(source, first); auto edit = request(); edit.incoming = incoming("new"); edit.decisions[0].action = Planner::Action::Update;
        Planner::Plan updated; QVERIFY(Planner::Build(before, edit, updated));
        QVERIFY(updated.state.artwork.bindings["B"] == first.state.artwork.bindings["B"]); QCOMPARE(updated.state.materials[1], first.state.materials[1]);
        const auto a = updated.state.artwork.placements.value(updated.state.artwork.bindings["A"].placementId);
        const auto b = updated.state.artwork.placements.value(updated.state.artwork.bindings["B"].placementId);
        QVERIFY(a.id != b.id); QVERIFY(a.materialId != b.materialId); QCOMPARE(updated.state.workspace.atlases[1], first.state.workspace.atlases[1]);
    }
    void sourceOnlyKeepAndMissingCandidateDoNotChangeSampling()
    {
        const auto source = snapshot(); Planner::Plan first; QVERIFY(Planner::Build(source, request(), first)); const auto before = accept(source, first);
        auto edit = request(); edit.incoming = incoming("missing"); edit.incoming.revision.layers.clear(); edit.incoming.images.clear(); edit.decisions[0].action = Planner::Action::Keep;
        Planner::Plan kept; QVERIFY(Planner::Build(before, edit, kept)); QCOMPARE(kept.state.materials, first.state.materials); QCOMPARE(kept.state.workspace, first.state.workspace);
        QVERIFY(kept.state.artwork.bindings == first.state.artwork.bindings); QCOMPARE(kept.state.artwork.families["family"].currentRevision, QString("missing"));
        edit.decisions[0].action = Planner::Action::Update; edit.decisions[0].layerId.clear(); Planner::Plan missing;
        QVERIFY(Planner::Build(before, edit, missing)); QVERIFY(missing.impacts[0].notes.join('\n').contains("保留旧素材"));
        edit.decisions.clear(); Planner::Plan library; QVERIFY(Planner::Build(source, edit, library)); QVERIFY(library.state.artwork.bindings.isEmpty());
        QCOMPARE(library.state.materials, source.baseline.materials); QCOMPARE(library.images.size(), 1);
    }
    void existingRevisionMayBeReusedButNeverRewritten()
    {
        const auto source = snapshot(); const auto original = request(); Planner::Plan first; QVERIFY(Planner::Build(source, original, first));
        const auto before = accept(source, first); auto same = original; same.decisions.clear(); same.familyName = "改名";
        Planner::Plan renamed; QVERIFY(Planner::Build(before, same, renamed)); QCOMPARE(renamed.state.artwork.revisions.size(), 1); QCOMPARE(renamed.state.artwork.families["family"].name, QString("改名"));
        same.incoming.revision.path = "/different.psd"; auto held = renamed; QString error;
        QVERIFY(!Planner::Build(before, same, renamed, &error)); QCOMPARE(renamed.state, held.state); QVERIFY(error.contains("改写历史"));
    }
    void arbitraryCalibrationReadsOriginalPixelsAndKeepsOldSourceImmutable()
    {
        const auto source = snapshot(); auto edit = request(); edit.decisions[0].atlasToDocument = QTransform::fromScale(2, 2);
        Planner::Plan half, again; QString error; QVERIFY2(Planner::Build(source, edit, half, &error), qPrintable(error));
        QCOMPARE(placement(half).filter, Artwork::Filter::LinearLightPremultiplied); QCOMPARE(placement(half).familyToAtlas.m11(), .5);
        QVERIFY(Planner::Build(source, edit, again)); QCOMPARE(again.state, half.state); QCOMPARE(again.token, half.token);
        QCOMPARE(half.state.artwork.images, edit.incoming.images);
        QCOMPARE(half.state.artwork.revisions["revision"].sourceBytes, edit.incoming.revision.sourceBytes);
    }
    void rotatedNonSquareSourceKeepsPixelCentersAndTransparentRgb()
    {
        const auto source = snapshot(); auto edit = request(); edit.incoming = incoming("rotated", {0, 0, 4, 6});
        auto pixels = pattern({0, 0, 4, 6}); auto* first = pixels.scanLine(0); first[0] = 17; first[1] = 39; first[2] = 91; first[3] = 0;
        const auto bytes = png(pixels); edit.incoming.images.clear(); edit.incoming.images.insert(hash(bytes), {pixels.size(), bytes}); edit.incoming.revision.layers[0].imageDigest = hash(bytes);
        edit.decisions[0].atlasToDocument = QTransform(0, 1, -1, 0, 4, 0);
        Planner::Plan plan; QString error; QVERIFY2(Planner::Build(source, edit, plan, &error), qPrintable(error));
        QCOMPARE(plan.images.last().size(), QSize(8, 6)); QCOMPARE(placement(plan).filter, Artwork::Filter::ExactTexels);
        QCOMPARE(plan.state.artwork.bindings["A"].familyCoordinates[0], QPointF(3, 1));
        for (const QPointF point : {QPointF(.5, .5), QPointF(2.5, 4.5)})
            QCOMPARE(pixel(plan.images.last(), placement(plan).familyToAtlas.map(point)), pixel(pixels, point));
    }
    void emptyLayerClearsAppearanceWithoutCreatingFakeSourceImage()
    {
        const auto source = snapshot(); Planner::Plan first; QVERIFY(Planner::Build(source, request(), first)); const auto before = accept(source, first);
        auto edit = request(); edit.incoming = incoming("empty", {-2, -3, 0, 0}); edit.decisions[0].action = Planner::Action::Update;
        Planner::Plan plan; QString error; QVERIFY2(Planner::Build(before, edit, plan, &error), qPrintable(error));
        QCOMPARE(plan.state.artwork.images.size(), first.state.artwork.images.size()); QVERIFY(plan.state.artwork.revisions["empty"].layers[0].imageDigest.isEmpty());
        const auto image = plan.images.last(); for (int y = 0; y < image.height(); ++y) for (int x = 0; x < image.width(); ++x) QCOMPARE(image.pixelColor(x, y).alpha(), 0);
        QCOMPARE(plan.state.artwork.bindings["A"].familyCoordinates, first.state.artwork.bindings["A"].familyCoordinates);
    }
    void opacityPolicyIsAppliedOnceWithoutChangingSource()
    {
        const auto source = snapshot(); auto edit = request(); edit.incoming.revision.layers[0].opacity = 128;
        Planner::Plan applied, raw; QVERIFY(Planner::Build(source, edit, applied));
        const auto point = placement(applied).familyToAtlas.map(QPointF(1.5, 1.5)); QCOMPARE(uchar(pixel(applied.images.last(), point)[3]), 128);
        edit.decisions[0].applyLayerOpacity = false; QVERIFY(Planner::Build(source, edit, raw)); QCOMPARE(uchar(pixel(raw.images.last(), point)[3]), 255);
        QCOMPARE(applied.state.artwork.images, raw.state.artwork.images);
    }
    void largeCpuSourceMayBeReducedIntoAGpuSizedAtlas()
    {
        auto source = snapshot(); source.limits.maximumDimension = 8; auto edit = request(); edit.incoming = incoming("wide", {0, 0, 20, 2});
        edit.decisions[0].atlasToDocument = QTransform::fromScale(10, 1); Planner::Plan plan; QString error;
        QVERIFY2(Planner::Build(source, edit, plan, &error), qPrintable(error));
        QVERIFY(plan.images.last().width() <= 8); QVERIFY(plan.images.last().height() <= 8); QCOMPARE(edit.incoming.images.first().size.width(), 20);
    }
    void repeatedAtlasSourceCoverageIncludesWrappedTriangles()
    {
        auto source = snapshot(); auto edit = request(); edit.incoming.kind = Artwork::SourceKind::AtlasSnapshot; const auto& incoming = edit.incoming;
        source.baseline.artwork.images = incoming.images; source.baseline.artwork.revisions.insert(incoming.revision.id, incoming.revision);
        source.baseline.artwork.families.insert("family", {"family", "原图集", Artwork::SourceKind::AtlasSnapshot, "revision"});
        source.baseline.materials[0].uvs = {0, 0, .5f, 0, .5f, .5f, 0, .5f}; source.meshes[0].material = source.baseline.materials[0];
        edit.decisions[0].atlasToDocument = QTransform::fromTranslate(4, 0); Planner::Plan plan; QString error;
        QVERIFY2(Planner::Build(source, edit, plan, &error), qPrintable(error));
        QCOMPARE(plan.state.artwork.families["family"].kind, Artwork::SourceKind::AtlasSnapshot);
        QVERIFY(!plan.warnings.join('\n').contains("非透明源像素中心"));
        edit.incoming.kind = Artwork::SourceKind::Psd; auto held = plan;
        QVERIFY(!Planner::Build(source, edit, plan)); QCOMPARE(plan.state, held.state);
    }
    void mismatchedExternalIdentityRequiresConfirmation()
    {
        const auto source = snapshot(); Planner::Plan first; QVERIFY(Planner::Build(source, request(), first)); const auto before = accept(source, first);
        auto edit = request(); edit.incoming = incoming("other-id"); edit.incoming.revision.layers[0].externalId = 99; edit.decisions[0].action = Planner::Action::Update; edit.decisions[0].confirmed = false;
        Planner::Plan preview; QVERIFY(Planner::Build(before, edit, preview)); QCOMPARE(preview.unconfirmed, QSet<QString>{"A"}); QVERIFY(!Planner::Validate(before, preview, nullptr, true));
        edit.decisions[0].confirmed = true; QVERIFY(Planner::Build(before, edit, preview)); QVERIFY(Planner::Validate(before, preview, nullptr, true));
        edit.decisions[0].confirmed = false; edit.incoming.revision.layers[0].externalId = 17;
        auto duplicate = edit.incoming.revision.layers[0]; duplicate.id = "record:1"; edit.incoming.revision.layers.append(duplicate);
        QVERIFY(Planner::Build(before, edit, preview)); QCOMPARE(preview.unconfirmed, QSet<QString>{"A"});
        edit.incoming.revision.familyId = "another-family"; QVERIFY(!Planner::Build(before, edit, preview));
    }
    void detachPreservesTheVisibleAtlasAndRejectsLaterAutomaticUpdate()
    {
        const auto source = snapshot(); Planner::Plan first; auto edit = request(); QVERIFY(Planner::Build(source, edit, first)); const auto before = accept(source, first);
        edit.decisions[0].action = Planner::Action::Detach; Planner::Plan detached; QVERIFY(Planner::Build(before, edit, detached));
        QCOMPARE(detached.state.workspace, first.state.workspace); QCOMPARE(detached.state.materials, first.state.materials);
        QCOMPARE(detached.state.artwork.bindings["A"].status, Artwork::BindingStatus::Detached);
        const auto after = accept(before, detached); edit.decisions[0].action = Planner::Action::Update;
        QVERIFY(!Planner::Build(after, edit, detached));
    }
    void borrowedInputsAreFrozenBeforeSourceCallbacks()
    {
        auto source = snapshot(); auto edit = request(); QByteArray sourceId = source.sourceDigest, bytes = edit.incoming.revision.sourceBytes;
        auto* idMemory = sourceId.data(); auto* sourceMemory = bytes.data(); QString name = "borrowed layer"; auto* nameMemory = name.data();
        source.sourceDigest = QByteArray::fromRawData(idMemory, sourceId.size());
        edit.incoming.revision.sourceBytes = QByteArray::fromRawData(sourceMemory, bytes.size());
        edit.incoming.revision.layers[0].name = QString::fromRawData(nameMemory, name.size());
        auto trusted = source; trusted.sourceDigest = QByteArray(idMemory, sourceId.size());
        const QByteArray original(sourceMemory, bytes.size()); Planner::Plan plan;
        CExportControl control; bool changed = false;
        control.SetHandler([&](const auto&, QString*) {
            if (!changed) { changed = true; idMemory[0] ^= 1; sourceMemory[0] ^= 1; nameMemory[0] = 'X'; }
            return true;
        });
        QString error; QVERIFY2(Planner::Build(source, edit, plan, &error, &control), qPrintable(error)); QVERIFY(changed);
        QVERIFY(!Planner::Validate(source, plan)); QVERIFY(Planner::Validate(trusted, plan));
        QCOMPARE(plan.state.artwork.revisions["revision"].sourceBytes, original);
        QCOMPARE(plan.state.artwork.revisions["revision"].layers[0].name, QString("borrowed layer"));
    }
    void invalidRequestsLeaveTheResultUntouched_data()
    {
        QTest::addColumn<int>("fault"); for (int i = 0; i < 12; ++i) QTest::newRow(qPrintable(QString::number(i))) << i;
    }
    void invalidRequestsLeaveTheResultUntouched()
    {
        QFETCH(int, fault); auto source = snapshot(); auto edit = request(); Planner::Plan output; QVERIFY(Planner::Build(source, edit, output)); const auto held = output;
        if (fault == 0) source.meshes[0].editable = false;
        if (fault == 1) edit.decisions[0].layerId = "missing";
        if (fault == 2) edit.decisions.append(edit.decisions[0]);
        if (fault == 3) edit.decisions[0].action = Planner::Action::Update;
        if (fault == 4) edit.incoming.revision.layers[0].pixelEquivalent = false;
        if (fault == 5) edit.decisions[0].atlasToDocument = QTransform(1, 2, 2, 4, 0, 0);
        if (fault == 6) source.limits.maximumDimension = 2;
        if (fault == 7) source.limits.maximumPixels = 10;
        if (fault == 8) source.limits.maximumOperations = 1;
        if (fault == 9) edit.incoming.revision.sourceDigest = hash("wrong");
        if (fault == 10) source.meshes[0].triangles = {0, 1, 99};
        if (fault == 11) source.baseline.materials[0].textureIndex = 99;
        QString error; QVERIFY(!Planner::Build(source, edit, output, &error)); QCOMPARE(output.state, held.state); QCOMPARE(output.token, held.token); QVERIFY(!error.isEmpty());
    }
    void cancellationAndSealProtectTheWholeCandidate()
    {
        auto source = snapshot(); auto edit = request(); Planner::Plan plan; QVERIFY(Planner::Build(source, edit, plan)); const auto held = plan;
        CExportControl control; control.SetHandler([](const auto&, QString*) { return false; }); QVERIFY(!Planner::Build(source, edit, plan, nullptr, &control)); QCOMPARE(plan.state, held.state);
        auto tampered = plan; tampered.state.artwork.bindings["A"].familyCoordinates[0].rx() += 1; QVERIFY(!Planner::Validate(source, tampered));
        tampered = plan; tampered.images.last().fill(Qt::red); QVERIFY(!Planner::Validate(source, tampered));
        tampered = plan; tampered.impacts[0].newSlot = 100; QVERIFY(!Planner::Validate(source, tampered));
        ++source.documentRevision; QVERIFY(!Planner::Validate(source, plan));
    }
};
QTEST_GUILESS_MAIN(ArtworkUpdatePlannerTests)
#include "artwork_update_planner_tests.moc"
