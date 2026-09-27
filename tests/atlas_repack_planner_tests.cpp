#include "live2dstruct/catlasrepackplanner.h"
#include "live2dstruct/cartworkreconciler.h"
#include "std/cexportcontrol.h"
#include "std/cpngimage.h"
#include <QtTest>
#include <QBuffer>
#include <QTransform>
#include <array>
#include <cmath>
#include <cstring>

using namespace live2dstruct;
using Planner = CAtlasRepackPlanner;
namespace {
QImage pattern(QSize size, int seed)
{
    QImage image(size, QImage::Format_RGBA8888);
    for (int y = 0; y < size.height(); ++y) for (int x = 0; x < size.width(); ++x) {
        auto* value = image.scanLine(y) + x * 4;
        value[0] = uchar((seed + x * 17 + y * 3) % 256);
        value[1] = uchar((seed * 3 + x * 7 + y * 29) % 256);
        value[2] = uchar((seed * 13 + x * 31 + y * 11) % 256);
        value[3] = (x + y) % 3 ? uchar((seed + x * 41 + y * 23) % 256) : 0;
    }
    return image;
}
QByteArray png(const QImage& image)
{
    QBuffer buffer; if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "PNG")) return {}; return buffer.data();
}
QByteArray pixel(const QImage& image, int x, int y)
{
    return QByteArray(reinterpret_cast<const char*>(image.constScanLine(y) + x * 4), 4);
}
QByteArray rgba(const QImage& image)
{
    QByteArray bytes;
    for (int y = 0; y < image.height(); ++y) bytes.append(reinterpret_cast<const char*>(image.constScanLine(y)), image.width() * 4);
    return bytes;
}
int modulo(int coordinate, int period) { return (coordinate % period + period) % period; }
std::array<double, 4> bilinear(const QImage& image, QPointF coordinate)
{
    const auto x = coordinate.x() - .5, y = coordinate.y() - .5;
    const int left = int(std::floor(x)), top = int(std::floor(y)); const auto fx = x - left, fy = y - top;
    std::array<double, 4> result{};
    for (int dy = 0; dy < 2; ++dy) for (int dx = 0; dx < 2; ++dx) {
        const auto* p = image.constScanLine(modulo(top + dy, image.height())) + modulo(left + dx, image.width()) * 4;
        const auto weight = (dx ? fx : 1 - fx) * (dy ? fy : 1 - fy);
        for (int channel = 0; channel < 4; ++channel) result[channel] += weight * p[channel];
    }
    return result;
}
Planner::Mesh mesh(const QString& id, int slot, const QVector<QPointF>& points, QSize size, bool editable = true)
{
    Planner::Mesh result; result.material.id = id; result.material.textureIndex = slot; result.editable = editable;
    if (!editable) result.reason = QStringLiteral("对象已锁定：") + id;
    for (const auto& point : points) result.material.uvs << float(point.x() / size.width()) << float(point.y() / size.height());
    if (points.size() >= 3) result.triangles = {0, 1, 2};
    return result;
}
Planner::Snapshot snapshot(QSize sourceSize = {8, 4}, QSize destinationSize = {13, 9})
{
    Planner::Snapshot result;
    result.sourceDigest = QByteArray(32, 'a'); result.topologyDigest = QByteArray(32, 'b'); result.documentRevision = 19;
    for (int slot = 0; slot < 2; ++slot) {
        CTextureAssets::Source source;
        CTextureAssets::MakeSource(QString("/tmp/repack-planner-source-%1.png").arg(slot), png(pattern(slot ? destinationSize : sourceSize, slot ? 93 : 11)), source);
        result.sources.insert(slot, source);
    }
    CTextureWorkspace::Initialize(result.sources, {}, result.workspace);
    result.meshes = {mesh("A", 0, {{1.25, 1.25}, {4.75, 1.25}, {4.75, 2.75}, {1.25, 2.75}, {2.25, 2.5}}, sourceSize)};
    result.meshes[0].triangles = {0, 1, 2, 0, 2, 3};
    return result;
}
void bindArtwork(Planner::Snapshot& source)
{
    auto& artwork = source.artwork; const auto image = source.sources[0];
    artwork.images.insert(image.digest, {image.size, image.png});
    artwork.families.insert("family", {"family", "原图集", CModelArtwork::SourceKind::AtlasSnapshot, "revision"});
    CModelArtwork::Revision revision; revision.id = "revision"; revision.familyId = "family";
    revision.sourceDigest = image.digest; revision.sourceBytes = image.png; revision.canvasSize = image.size;
    CModelArtwork::Layer layer; layer.id = "layer"; layer.bounds = QRect(QPoint(), image.size); layer.imageDigest = image.digest;
    revision.layers = {layer}; artwork.revisions.insert(revision.id, revision);
    artwork.materials.insert("material", {"material", "", "family", "revision", "layer", CModelArtwork::Boundary::Repeat, false, {}});
    artwork.placements.insert("placement", {"placement", "material", source.workspace.atlases[0].id, QTransform(), QRectF(QPointF(), image.size), 1, CModelArtwork::Filter::ExactTexels});
    artwork.placements.insert("unused-original", {"unused-original", "material", source.workspace.atlases[1].id, QTransform(), QRectF(QPointF(), image.size), 1, CModelArtwork::Filter::ExactTexels});
    for (const auto& mesh : source.meshes) if (mesh.material.textureIndex == 0) {
        CModelArtwork::Binding binding; binding.meshId = mesh.material.id; binding.placementId = "placement"; binding.sampledMaterial = mesh.material;
        for (int i = 0; i < mesh.material.uvs.size(); i += 2)
            binding.familyCoordinates.append({double(mesh.material.uvs[i]) * image.size.width(), double(mesh.material.uvs[i + 1]) * image.size.height()});
        CModelArtwork::TopologyDigest(mesh.material.uvs.size() / 2, mesh.triangles, binding.topologyDigest);
        artwork.bindings.insert(binding.meshId, binding);
    }
}
bool equal(const Planner::Plan& first, const Planner::Plan& second)
{
    return first.state == second.state && first.pixelCoordinates == second.pixelCoordinates && first.images == second.images &&
        first.changedMeshes == second.changedMeshes && first.changedSlots == second.changedSlots && first.warnings == second.warnings && first.token == second.token;
}
Planner::Plan sentinel(const Planner::Plan& initial)
{
    auto result = initial; result.warnings = {QStringLiteral("sentinel")}; result.changedSlots = {777}; return result;
}
QTransform forward(QRectF source, QPoint destination, int quarterTurns)
{
    const QSizeF target = quarterTurns % 2 ? QSizeF(source.height(), source.width()) : source.size();
    QTransform result;
    result.translate(destination.x() + target.width() / 2, destination.y() + target.height() / 2);
    result.rotate(quarterTurns * 90);
    result.translate(-source.center().x(), -source.center().y());
    return result;
}
QImage movedImage(const QImage& source, const QImage& target, QRect sourceArea, const QTransform& transform)
{
    auto result = target;
    for (int y = sourceArea.y(); y < sourceArea.y() + sourceArea.height(); ++y)
        for (int x = sourceArea.x(); x < sourceArea.x() + sourceArea.width(); ++x) {
            const auto position = transform.map(QPointF(x + .5, y + .5));
            std::memcpy(result.scanLine(int(std::floor(position.y()))) + int(std::floor(position.x())) * 4,
                        source.constScanLine(modulo(y, source.height())) + modulo(x, source.width()) * 4, 4);
        }
    return result;
}
void decodedMatches(const Planner::Snapshot& source, const Planner::Plan& plan)
{
    CTextureAssets::State bytes; QString error;
    QVERIFY2(CTextureWorkspace::Effective(source.sources, plan.state.workspace, bytes, &error), qPrintable(error));
    for (auto it = plan.images.cbegin(); it != plan.images.cend(); ++it) {
        QImage decoded; QVERIFY2(CPngImage::Decode(bytes.value(it.key()), decoded, &error), qPrintable(error));
        QCOMPARE(decoded.format(), QImage::Format_RGBA8888); QCOMPARE(decoded.size(), it->size()); QCOMPARE(rgba(decoded), rgba(*it));
    }
}
}

class AtlasRepackPlannerTests : public QObject
{
    Q_OBJECT
private slots:
    void artworkSharedPlacementSurvivesPartialRotationAndReturningToSource()
    {
        auto source = snapshot(); auto other = source.meshes[0]; other.material.id = "Shared"; other.editable = false; source.meshes.append(other); bindArtwork(source);
        Planner::Plan original, moved, returned; QString error; QVERIFY2(Planner::Initialize(source, original, &error), qPrintable(error));
        QVERIFY2(Planner::Move(source, original, {"A"}, 1, {2, 1}, 1, 1, moved, &error), qPrintable(error));
        const auto& art = moved.state.artwork; QCOMPARE(art.bindings["A"].status, CModelArtwork::BindingStatus::Linked);
        QCOMPARE(art.bindings["A"].familyCoordinates, source.artwork.bindings["A"].familyCoordinates);
        QVERIFY(art.bindings["A"].placementId != art.bindings["Shared"].placementId);
        QVERIFY(art.bindings["Shared"] == source.artwork.bindings["Shared"]);
        QVERIFY2(Planner::Move(source, moved, {"A"}, 0, {0, 0}, 3, 1, returned, &error), qPrintable(error));
        QVERIFY(returned.state.artwork == source.artwork); QCOMPARE(returned.state.materials, original.state.materials);
    }
    void artworkResizingAndGeneratedSlotRoundTripRestoreIdentity()
    {
        auto source = snapshot(); bindArtwork(source); Planner::Plan original, resized, restored, added, moved, returned, removed; QString error;
        QVERIFY(Planner::Initialize(source, original));
        QVERIFY2(Planner::ResizeAtlas(source, original, 0, {13, 9}, resized, &error), qPrintable(error));
        QCOMPARE(resized.state.artwork.bindings["A"].familyCoordinates, original.state.artwork.bindings["A"].familyCoordinates);
        QVERIFY(resized.state.artwork.placements == source.artwork.placements);
        QVERIFY2(Planner::ResizeAtlas(source, resized, 0, {8, 4}, restored, &error), qPrintable(error)); QVERIFY(restored.state.artwork == source.artwork);
        QVERIFY(Planner::AddAtlas(source, restored, "暂存", {8, 8}, added));
        QVERIFY2(Planner::Move(source, added, {"A"}, 2, {0, 0}, 1, 1, moved, &error), qPrintable(error));
        QVERIFY2(Planner::Move(source, moved, {"A"}, 0, {0, 0}, 3, 1, returned, &error), qPrintable(error));
        QVERIFY(Planner::RemoveAtlas(source, returned, 2, removed)); QVERIFY(removed.state == original.state);
    }
    void artworkPackKeepsSharedSourceAndIndependentUnusedPlacement()
    {
        auto source = snapshot(); auto other = source.meshes[0]; other.material.id = "Shared"; source.meshes.append(other); bindArtwork(source);
        Planner::Plan initial, added, packed; QString error; QVERIFY(Planner::Initialize(source, initial)); QVERIFY(Planner::AddAtlas(source, initial, "打包", {8, 8}, added));
        QVERIFY2(Planner::Pack(source, added, {"A", "Shared"}, {2}, 1, true, packed, &error), qPrintable(error));
        const auto& artwork = packed.state.artwork;
        QCOMPARE(artwork.bindings["A"].placementId, artwork.bindings["Shared"].placementId);
        QVERIFY(artwork.placements["unused-original"] == source.artwork.placements["unused-original"]);
        QVERIFY(artwork.revisions == source.artwork.revisions); QVERIFY(artwork.images == source.artwork.images);
        QCOMPARE(artwork.bindings["A"].status, CModelArtwork::BindingStatus::Linked);
    }
    void artworkSealOwnsBorrowedSourceAndDetectsMetadataTampering()
    {
        auto source = snapshot(); bindArtwork(source); QString name = "borrowed name"; auto* raw = name.data();
        source.artwork.families["family"].name = QString::fromRawData(raw, name.size());
        auto trusted = source; QVERIFY(CModelArtwork::Freeze(source.artwork, trusted.artwork));
        Planner::Plan initial; QVERIFY(Planner::Initialize(source, initial)); raw[0] = 'X';
        QVERIFY(!Planner::Validate(source, initial)); QVERIFY(Planner::Validate(trusted, initial)); QVERIFY(initial.state.artwork == trusted.artwork);
        auto corrupt = initial; corrupt.state.artwork.bindings["A"].familyCoordinates[0].rx() += .01;
        QVERIFY(!Planner::Validate(trusted, corrupt)); corrupt = initial; corrupt.state.artwork.families["family"].name = "changed";
        QVERIFY(!Planner::Validate(trusted, corrupt));
        Planner::Plan other; QVERIFY(Planner::Initialize(source, other)); QVERIFY(other.token != initial.token);
    }
    void removingUnusedAtlasKeepsOriginalHistoricalPlacement()
    {
        auto source = snapshot(); source.workspace.atlases.insert(2, {"generated-original", "空图集", -1, {8, 8}, png(pattern({8, 8}, 31))});
        bindArtwork(source); source.artwork.placements["unused-original"].atlasId = "generated-original";
        Planner::Plan initial, removed; QVERIFY(Planner::Initialize(source, initial)); QVERIFY(Planner::RemoveAtlas(source, initial, 2, removed));
        QVERIFY(removed.state.artwork == source.artwork); QVERIFY(Planner::Validate(source, removed));
    }
    void rectangularRgbaRotationsUseTexelCentersAndPreserveUnusedVertex_data()
    {
        QTest::addColumn<int>("turns"); for (int turns = 0; turns < 4; ++turns) QTest::newRow(qPrintable(QString::number(turns))) << turns;
    }
    void rectangularRgbaRotationsUseTexelCentersAndPreserveUnusedVertex()
    {
        QFETCH(int, turns); const auto source = snapshot(); const auto origins = source.sources;
        Planner::Plan base, result; QString error; QVERIFY2(Planner::Initialize(source, base, &error), qPrintable(error));
        QVERIFY2(Planner::Move(source, base, {"A"}, 1, {2, 1}, turns, 1, result, &error), qPrintable(error));
        const QRect area(0, 0, 6, 4); const auto transform = forward(area, {2, 1}, turns);
        const auto expected = movedImage(base.images[0], base.images[1], area, transform);
        QCOMPARE(rgba(result.images[1]), rgba(expected)); QCOMPARE(rgba(result.images[0]), rgba(base.images[0])); QCOMPARE(source.sources, origins);
        QCOMPARE(result.state.materials[0].textureIndex, 1); QCOMPARE(result.pixelCoordinates["A"].size(), 5);
        for (int index = 0; index < base.pixelCoordinates["A"].size(); ++index) {
            const auto mapped = transform.map(base.pixelCoordinates["A"][index]); QCOMPARE(result.pixelCoordinates["A"][index], mapped);
            QCOMPARE(result.state.materials[0].uvs[index * 2], float(mapped.x() / 13));
            QCOMPARE(result.state.materials[0].uvs[index * 2 + 1], float(mapped.y() / 9));
        }
        for (int y = 1; y <= 4; ++y) for (int x = 1; x <= 7; ++x) {
            const QPointF point(1.25 + x * .4, 1.25 + y * .2);
            const auto before = bilinear(base.images[0], point), after = bilinear(result.images[1], transform.map(point));
            for (int channel = 0; channel < 4; ++channel) QVERIFY(std::abs(before[channel] - after[channel]) < 1e-9);
        }
        const auto zeroAlpha = transform.map(QPointF(.5, .5));
        QCOMPARE(pixel(result.images[1], int(zeroAlpha.x()), int(zeroAlpha.y())), pixel(base.images[0], 0, 0));
        QVERIFY(pixel(base.images[0], 0, 0).left(3) != QByteArray(3, '\0'));
        decodedMatches(source, result);
    }
    void overlappingCopyReadsTheEntireOldSourceImage()
    {
        auto source = snapshot({16, 8}, {13, 9}); Planner::Plan base, result; QVERIFY(Planner::Initialize(source, base));
        QVERIFY(Planner::Move(source, base, {"A"}, 0, {2, 1}, 0, 1, result));
        const auto expected = movedImage(base.images[0], base.images[0], {0, 0, 6, 4}, forward({0, 0, 6, 4}, {2, 1}, 0));
        QCOMPARE(rgba(result.images[0]), rgba(expected)); decodedMatches(source, result);
    }
    void unusedUvOutlierIsIncludedInTheCopiedPixelDomain()
    {
        auto source = snapshot({16, 8}, {16, 8}); source.meshes[0].material.uvs[8] = 10.5f / 16;
        Planner::Plan initial, moved; QVERIFY(Planner::Initialize(source, initial)); QVERIFY(Planner::Move(source, initial, {"A"}, 1, {1, 1}, 0, 1, moved));
        const QRect area(0, 0, 12, 4); const auto transform = forward(area, {1, 1}, 0);
        QCOMPARE(rgba(moved.images[1]), rgba(movedImage(initial.images[0], initial.images[1], area, transform)));
        QCOMPARE(moved.pixelCoordinates["A"][4], transform.map(initial.pixelCoordinates["A"][4]));
    }
    void negativeUvCopiesRepeatedPixelsWithoutClamping()
    {
        auto source = snapshot(); source.meshes = {mesh("A", 0, {{-1.5, .5}, {1.5, .5}, {-1.5, 2.5}}, {8, 4})};
        Planner::Plan base, result; QVERIFY(Planner::Initialize(source, base)); QString error;
        QVERIFY2(Planner::Move(source, base, {"A"}, 1, {2, 1}, 1, 1, result, &error), qPrintable(error));
        const QRect area(-3, -1, 6, 5); const auto matrix = forward(area, {2, 1}, 1);
        QCOMPARE(rgba(result.images[1]), rgba(movedImage(base.images[0], base.images[1], area, matrix)));
        for (const auto point : {QPointF(-1.5, .5), QPointF(-.2, 1.4), QPointF(1.25, .8)}) {
            const auto before = bilinear(base.images[0], point), after = bilinear(result.images[1], matrix.map(point));
            for (int channel = 0; channel < 4; ++channel) QVERIFY(std::abs(before[channel] - after[channel]) < 1e-9);
        }
        decodedMatches(source, result);
    }
    void unselectedRepeatSamplingBlocksConflictingDestinationPixels()
    {
        auto source = snapshot(); source.meshes.append(mesh("Protected", 1, {{14.5, 1.5}, {15.5, 1.5}, {14.5, 2.5}}, {13, 9}, false));
        Planner::Plan base; QVERIFY(Planner::Initialize(source, base)); auto output = sentinel(base); const auto unchanged = output; QString error;
        QVERIFY(!Planner::Move(source, base, {"A"}, 1, {0, 0}, 0, 1, output, &error)); QVERIFY(error.contains("Protected")); QVERIFY(equal(output, unchanged));
        Planner::Plan free;
        QVERIFY2(Planner::Move(source, base, {"A"}, 1, {6, 4}, 0, 1, free, &error), qPrintable(error));
        const auto oldSamples = base.state.materials[1]; QCOMPARE(free.state.materials[1], oldSamples);
        for (const auto point : base.pixelCoordinates["Protected"])
            QCOMPARE(bilinear(free.images[1], point), bilinear(base.images[1], point));
    }
    void unchangedSharedSourceRemainsAvailableAfterCrossSlotCopy()
    {
        auto source = snapshot(); auto shared = source.meshes[0]; shared.material.id = "Shared"; shared.editable = false; source.meshes.append(shared);
        Planner::Plan base, moved; QVERIFY(Planner::Initialize(source, base)); QVERIFY(Planner::Move(source, base, {"A"}, 1, {2, 1}, 0, 1, moved));
        QCOMPARE(moved.state.materials[1], base.state.materials[1]); QCOMPARE(rgba(moved.images[0]), rgba(base.images[0]));
        QCOMPARE(moved.changedMeshes, QSet<QString>{"A"}); decodedMatches(source, moved);
    }
    void sameProtectedRgbaCanBeCopiedWithoutChangingLockedSampling()
    {
        auto source = snapshot({8, 4}, {8, 4}); source.sources[1] = source.sources[0];
        QVERIFY(CTextureWorkspace::Initialize(source.sources, {}, source.workspace));
        auto shared = source.meshes[0]; shared.material.id = "Protected"; shared.material.textureIndex = 1; shared.editable = false; source.meshes.append(shared);
        Planner::Plan base, moved; QVERIFY(Planner::Initialize(source, base)); QString error;
        QVERIFY2(Planner::Move(source, base, {"A"}, 1, {0, 0}, 0, 1, moved, &error), qPrintable(error));
        QCOMPARE(rgba(moved.images[1]), rgba(base.images[1])); QCOMPARE(moved.state.materials[1], base.state.materials[1]);
    }
    void resizePreservesPixelPositionsAndDoesNotAccumulateFloatRoundoff()
    {
        const auto source = snapshot(); Planner::Plan original, current; QVERIFY(Planner::Initialize(source, original)); current = original;
        for (int cycle = 0; cycle < 24; ++cycle) {
            Planner::Plan resized, restored; QString error;
            QVERIFY2(Planner::ResizeAtlas(source, current, 0, {13, 9}, resized, &error), qPrintable(error));
            QCOMPARE(resized.pixelCoordinates, original.pixelCoordinates);
            for (int y = 0; y < 4; ++y) for (int x = 0; x < 8; ++x) QCOMPARE(pixel(resized.images[0], x, y), pixel(original.images[0], x, y));
            QVERIFY2(Planner::ResizeAtlas(source, resized, 0, {8, 4}, restored, &error), qPrintable(error));
            QCOMPARE(restored.state.materials, original.state.materials); QCOMPARE(restored.pixelCoordinates, original.pixelCoordinates);
            QCOMPARE(rgba(restored.images[0]), rgba(original.images[0])); current = std::move(restored);
        }
        QCOMPARE(current.state.workspace, original.state.workspace); decodedMatches(source, current);
    }
    void resizeExpansionPreservesNegativeAndBoundaryRepeatSamples()
    {
        auto source = snapshot(); source.meshes = {mesh("A", 0, {{-.5, .5}, {1.5, .5}, {-.5, 2.5}}, {8, 4})};
        Planner::Plan original, expanded; QVERIFY(Planner::Initialize(source, original)); QString error;
        QVERIFY2(Planner::ResizeAtlas(source, original, 0, {12, 8}, expanded, &error), qPrintable(error));
        for (int y = -1; y < 4; ++y) for (int x = -2; x < 3; ++x) {
            const QPointF point(x + .65, y + .73); const auto before = bilinear(original.images[0], point), after = bilinear(expanded.images[0], point);
            if (point.x() >= -.5 && point.x() <= 1.5 && point.y() >= .5 && point.y() <= 2.5)
                for (int channel = 0; channel < 4; ++channel) QVERIFY(std::abs(before[channel] - after[channel]) < 1e-9);
        }
        QCOMPARE(pixel(expanded.images[0], 11, 0), pixel(original.images[0], 7, 0)); decodedMatches(source, expanded);
    }
    void returningToBaselinePreservesItsAcceptedPngBytes()
    {
        auto source = snapshot(); auto image = pattern({8, 4}, 11); image.setText("Material note", "keep author metadata");
        const auto replacement = png(image); QVERIFY(replacement != source.sources[0].png);
        QVERIFY(CTextureWorkspace::Initialize(source.sources, {{0, replacement}}, source.workspace));
        Planner::Plan initial, expanded, restored; QVERIFY(Planner::Initialize(source, initial));
        QVERIFY(Planner::ResizeAtlas(source, initial, 0, {13, 9}, expanded));
        QVERIFY(Planner::ResizeAtlas(source, expanded, 0, {8, 4}, restored));
        QCOMPARE(rgba(restored.images[0]), rgba(initial.images[0]));
        QCOMPARE(restored.state.workspace.atlases[0].png, replacement); QCOMPARE(restored.state, initial.state);
    }
    void shrinkCannotFoldConflictingSupportOrLoseUvVertices()
    {
        auto source = snapshot(); source.meshes = {mesh("A", 0, {{.5, .5}, {4.5, .5}, {.5, 2.5}}, {8, 4})};
        Planner::Plan original; QVERIFY(Planner::Initialize(source, original)); auto output = sentinel(original); const auto unchanged = output; QString error;
        QVERIFY(!Planner::ResizeAtlas(source, original, 0, {4, 4}, output, &error)); QVERIFY(equal(output, unchanged));
        QVERIFY(!Planner::ResizeAtlas(source, original, 0, {5, 4}, output, &error)); QVERIFY(equal(output, unchanged));
        QVERIFY(!error.isEmpty());
    }
    void sameSizeLockedResizeIsNoopButActualResizeIsRejected()
    {
        auto source = snapshot(); source.meshes[0].editable = false; source.meshes[0].reason = "locked A";
        Planner::Plan initial, same; QVERIFY(Planner::Initialize(source, initial)); QVERIFY(Planner::ResizeAtlas(source, initial, 0, {8, 4}, same)); QVERIFY(equal(initial, same));
        auto output = sentinel(initial); const auto unchanged = output; QString error;
        QVERIFY(!Planner::ResizeAtlas(source, initial, 0, {12, 8}, output, &error)); QVERIFY(equal(output, unchanged)); QVERIFY(error.contains("A"));
    }
    void quarterTurnsRemainExactlyReversibleInPixelSpace()
    {
        auto source = snapshot({16, 8}, {13, 9}); Planner::Plan initial, current; QVERIFY(Planner::Initialize(source, initial)); current = initial;
        for (int index = 0; index < 4; ++index) {
            Planner::Plan next; QVERIFY(Planner::Move(source, current, {"A"}, 0, {0, 0}, 1, 1, next)); current = std::move(next);
        }
        QCOMPARE(current.pixelCoordinates, initial.pixelCoordinates); QCOMPARE(current.state.materials, initial.state.materials);
        for (int y = 0; y < 4; ++y) for (int x = 0; x < 6; ++x) QCOMPARE(pixel(current.images[0], x, y), pixel(initial.images[0], x, y));
    }
    void packOverlappingMeshesAsOnePatchWithDeterministicPlacement()
    {
        auto source = snapshot({16, 8}, {16, 8});
        source.meshes.append(mesh("B", 0, {{3.25, 1.25}, {5.75, 1.25}, {3.25, 2.75}}, {16, 8}));
        source.meshes.append(mesh("C", 0, {{10.25, 4.25}, {12.75, 4.25}, {10.25, 5.75}}, {16, 8}));
        Planner::Plan initial, first, second; QVERIFY(Planner::Initialize(source, initial)); QString error;
        QVERIFY2(Planner::Pack(source, initial, {"A", "B", "C"}, {1}, 1, true, first, &error), qPrintable(error));
        QVERIFY2(Planner::Pack(source, initial, {"C", "B", "A"}, {1}, 1, true, second, &error), qPrintable(error));
        QCOMPARE(first.state, second.state); QCOMPARE(first.pixelCoordinates, second.pixelCoordinates); QCOMPARE(rgba(first.images[1]), rgba(second.images[1]));
        const auto oldDifference = initial.pixelCoordinates["B"][0] - initial.pixelCoordinates["A"][0];
        const auto newDifference = first.pixelCoordinates["B"][0] - first.pixelCoordinates["A"][0];
        QCOMPARE(std::hypot(newDifference.x(), newDifference.y()), std::hypot(oldDifference.x(), oldDifference.y()));
        QVERIFY(newDifference == oldDifference || newDifference == QPointF(-oldDifference.y(), oldDifference.x()));
        for (const auto& value : first.state.materials) for (int index = 0; index < first.pixelCoordinates[value.id].size(); ++index) {
            const auto before = bilinear(initial.images[0], initial.pixelCoordinates[value.id][index]);
            const auto after = bilinear(first.images[1], first.pixelCoordinates[value.id][index]);
            for (int channel = 0; channel < 4; ++channel) QVERIFY(std::abs(before[channel] - after[channel]) < 1e-9);
        }
        decodedMatches(source, first);
    }
    void packRequiresAllTargetReferencesAndPreservesOutputWhenTooSmall()
    {
        auto source = snapshot({8, 4}, {4, 3}); Planner::Plan initial; QVERIFY(Planner::Initialize(source, initial));
        auto output = sentinel(initial); const auto unchanged = output; QString error;
        QVERIFY(!Planner::Pack(source, initial, {"A"}, {1}, 1, true, output, &error)); QVERIFY(equal(output, unchanged));
        source = snapshot(); source.meshes.append(mesh("Protected", 1, {{8.25, 5.25}, {10.25, 5.25}, {8.25, 7.25}}, {13, 9}, false));
        QVERIFY(Planner::Initialize(source, initial)); output = sentinel(initial); const auto held = output;
        QVERIFY(!Planner::Pack(source, initial, {"A"}, {1}, 1, false, output, &error)); QVERIFY(equal(output, held)); QVERIFY(error.contains("Protected"));
    }
    void cancellationAfterRenderingStartsDoesNotModifyInputOrOutput()
    {
        const auto source = snapshot(); Planner::Plan initial; QVERIFY(Planner::Initialize(source, initial)); const auto held = initial;
        auto output = sentinel(initial); const auto unchanged = output; CExportControl control; int checkpoints = 0;
        control.SetHandler([&](const auto&, QString*) { if (++checkpoints == 4) control.Cancel(); return true; }); QString error;
        QVERIFY(!Planner::Move(source, initial, {"A"}, 1, {2, 1}, 1, 1, output, &error, &control));
        QVERIFY(checkpoints >= 4); QVERIFY(control.WasCanceled()); QVERIFY(equal(initial, held)); QVERIFY(equal(output, unchanged));
    }
    void forgedImageMustNotBeAcceptedAsMatchingThePng()
    {
        const auto source = snapshot(); Planner::Plan initial; QVERIFY(Planner::Initialize(source, initial)); auto forged = initial;
        forged.images[0].scanLine(0)[0] ^= 1; QString error;
        QVERIFY2(!Planner::Validate(source, forged, &error), "Validate accepted an image that disagrees with the persisted PNG");
        auto output = sentinel(initial); const auto unchanged = output;
        QVERIFY(!Planner::RenameAtlas(source, forged, 0, "renamed", output, &error)); QVERIFY(equal(output, unchanged));
    }
    void trustedCopiesWorkButReconstructedOrSubFloatTamperedPlansDoNot()
    {
        const auto source = snapshot(); Planner::Plan initial; QVERIFY(Planner::Initialize(source, initial));
        const auto copy = initial; QVERIFY(Planner::Validate(source, copy));
        Planner::Plan reconstructed; reconstructed.state = initial.state; reconstructed.images = initial.images;
        reconstructed.pixelCoordinates = initial.pixelCoordinates; reconstructed.changedMeshes = initial.changedMeshes;
        reconstructed.changedSlots = initial.changedSlots; reconstructed.warnings = initial.warnings; reconstructed.token = initial.token;
        QVERIFY(!Planner::Validate(source, reconstructed));
        auto modified = initial; auto& x = modified.pixelCoordinates["A"][0].rx(); x = std::nextafter(x, std::numeric_limits<double>::infinity());
        QCOMPARE(float(x / 8), initial.state.materials[0].uvs[0]); QVERIFY(!Planner::Validate(source, modified));
    }
    void publicBoundsRejectsAStalePlanWithoutReplacingOutputs()
    {
        const auto source = snapshot(); Planner::Plan initial; QVERIFY(Planner::Initialize(source, initial)); auto stale = source; ++stale.documentRevision;
        QRect bounds(7, 8, 9, 10); int slot = 77; QString error;
        QVERIFY(!Planner::SelectionBounds(stale, initial, {"A"}, 1, bounds, slot, &error)); QCOMPARE(bounds, QRect(7, 8, 9, 10)); QCOMPARE(slot, 77);
        QVERIFY(!error.isEmpty());
    }
    void validationFreezesBorrowedSourceBytesAndAtlasNames()
    {
        auto source = snapshot(); QByteArray bytes = source.sources[0].png;
        source.sources[0].png = QByteArray::fromRawData(bytes.constData(), bytes.size());
        auto independent = source; independent.sources[0].png = QByteArray(bytes.constData(), bytes.size());
        Planner::Plan initial; QVERIFY(Planner::Initialize(source, initial)); bytes[bytes.size() / 2] ^= 1;
        QVERIFY(!Planner::Validate(source, initial)); QVERIFY(Planner::Validate(independent, initial));
        QChar characters[] = {QChar('N'), QChar('a'), QChar('m'), QChar('e')};
        const auto name = QString::fromRawData(characters, 4); Planner::Plan renamed;
        QVERIFY(Planner::RenameAtlas(independent, initial, 0, name, renamed)); characters[0] = QChar('X');
        QCOMPARE(renamed.state.workspace.atlases[0].name, QString("Name")); QVERIFY(Planner::Validate(independent, renamed));
    }
    void originalSignedZeroSurvivesResizeAndRestoration()
    {
        auto source = snapshot(); source.meshes[0].material.uvs[0] = -0.0f;
        Planner::Plan initial, grown, restored; QVERIFY(Planner::Initialize(source, initial)); QVERIFY(std::signbit(initial.pixelCoordinates["A"][0].x()));
        QString error; QVERIFY2(Planner::ResizeAtlas(source, initial, 0, {13, 9}, grown, &error), qPrintable(error));
        QVERIFY2(Planner::ResizeAtlas(source, grown, 0, {8, 4}, restored, &error), qPrintable(error));
        QVERIFY(std::signbit(restored.state.materials[0].uvs[0])); QVERIFY(std::signbit(restored.pixelCoordinates["A"][0].x()));
        QCOMPARE(restored.state.materials, initial.state.materials); QVERIFY(Planner::Validate(source, restored));
    }
    void mergedOverlappingDomainsCannotOverflowIntegerBounds()
    {
        auto source = snapshot();
        source.meshes = {mesh("A", 0, {{-1500000000, 1}, {.5, 1}, {.5, 2}}, {8, 4}),
                         mesh("B", 0, {{-.5, 1}, {1500000000, 1}, {-.5, 2}}, {8, 4})};
        Planner::Plan initial; QVERIFY(Planner::Initialize(source, initial)); auto output = sentinel(initial); const auto unchanged = output; QString error;
        QVERIFY(!Planner::Pack(source, initial, {"A", "B"}, {1}, 1, false, output, &error)); QVERIFY(equal(output, unchanged));
        QVERIFY(error.contains(QStringLiteral("整数")));
    }
    void forgedDirectoryPixelsUvAndTokenPreserveOutput_data()
    {
        QTest::addColumn<int>("fault"); for (int fault = 0; fault < 7; ++fault) QTest::newRow(qPrintable(QString::number(fault))) << fault;
    }
    void forgedDirectoryPixelsUvAndTokenPreserveOutput()
    {
        QFETCH(int, fault); const auto source = snapshot(); Planner::Plan initial; QVERIFY(Planner::Initialize(source, initial)); auto forged = initial;
        if (fault == 0) forged.token[0] ^= 1;
        if (fault == 1) forged.images.remove(1);
        if (fault == 2) forged.pixelCoordinates["A"][0].rx() += 1;
        if (fault == 3) forged.state.materials[0].uvs[0] += .1f;
        if (fault == 4) forged.state.materials[0].id = "unknown";
        if (fault == 5) forged.state.workspace.atlases[1].size = {2, 3};
        if (fault == 6) forged.images[0] = forged.images[0].convertToFormat(QImage::Format_ARGB32);
        auto output = sentinel(initial); const auto unchanged = output; QString error;
        QVERIFY(!Planner::Move(source, forged, {"A"}, 1, {2, 1}, 0, 1, output, &error)); QVERIFY(equal(output, unchanged)); QVERIFY(!error.isEmpty());
    }
    void totalPixelBudgetAppliesAfterInitialization_data()
    {
        QTest::addColumn<bool>("resize"); QTest::newRow("add") << false; QTest::newRow("resize") << true;
    }
    void totalPixelBudgetAppliesAfterInitialization()
    {
        QFETCH(bool, resize); auto source = snapshot(); source.limits.maximumPixels = 8 * 4 + 13 * 9;
        Planner::Plan initial; QVERIFY(Planner::Initialize(source, initial)); auto output = sentinel(initial); const auto unchanged = output; QString error;
        const bool accepted = resize ? Planner::ResizeAtlas(source, initial, 0, {9, 4}, output, &error)
                                     : Planner::AddAtlas(source, initial, "new", {2, 2}, output, &error);
        QVERIFY2(!accepted, "The sum of distinct decoded atlas pixels exceeded maximumPixels"); QVERIFY(equal(output, unchanged));
    }
    void initializationAndLargeOperationsRespectResourceBudgets_data()
    {
        QTest::addColumn<int>("operation"); QTest::newRow("add-clear") << 0; QTest::newRow("resize-copy") << 1; QTest::newRow("move-copy") << 2;
    }
    void initializationAndLargeOperationsRespectResourceBudgets()
    {
        QFETCH(int, operation); auto source = snapshot(); source.meshes.clear(); source.limits.maximumOperations = 1;
        if (operation == 2) source.meshes = snapshot().meshes;
        Planner::Plan initial; QVERIFY(Planner::Initialize(source, initial)); auto output = sentinel(initial); const auto unchanged = output; QString error;
        bool accepted = false;
        if (operation == 0) accepted = Planner::AddAtlas(source, initial, "new", {4, 4}, output, &error);
        if (operation == 1) accepted = Planner::ResizeAtlas(source, initial, 0, {9, 4}, output, &error);
        if (operation == 2) accepted = Planner::Move(source, initial, {"A"}, 1, {2, 1}, 0, 1, output, &error);
        QVERIFY2(!accepted, "Pixel initialization or copying bypassed the operation budget"); QVERIFY(equal(output, unchanged));
    }
    void packSharesOneBudgetForPlanningClearingAndCopying()
    {
        auto source = snapshot(); source.limits.maximumOperations = 13 * 9 + 6 * 4;
        Planner::Plan initial; QVERIFY(Planner::Initialize(source, initial)); auto output = sentinel(initial); const auto unchanged = output; QString error;
        QVERIFY2(!Planner::Pack(source, initial, {"A"}, {1}, 1, false, output, &error), "Packing spent separate full budgets on planning and rendering");
        QVERIFY(equal(output, unchanged));
    }
    void callerLimitsCannotDisableTheHardAllocationBound()
    {
        auto source = snapshot(); source.limits.maximumDimension = 100000;
        source.limits.maximumPixels = source.limits.maximumOperations = std::numeric_limits<qint64>::max();
        Planner::Plan initial; QVERIFY(Planner::Initialize(source, initial)); auto output = sentinel(initial); const auto unchanged = output;
        QVERIFY(!Planner::AddAtlas(source, initial, "too large", {50000, 50000}, output)); QVERIFY(equal(output, unchanged));
    }
};
QTEST_GUILESS_MAIN(AtlasRepackPlannerTests)
#include "atlas_repack_planner_tests.moc"
