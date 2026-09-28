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
    void callbacksCannotTemporarilyRaiseTheCapturedOperationBudget_data()
    {
        QTest::addColumn<int>("restoreAt"); for (int index : {6, 7, 8}) QTest::newRow(qPrintable(QString::number(index))) << index;
    }
    void callbacksCannotTemporarilyRaiseTheCapturedOperationBudget()
    {
        QFETCH(int, restoreAt); auto source = snapshot({16, 12}, {24, 20}); source.limits.maximumOperations = 64;
        const auto originalSource = source; Planner::Plan initial; QVERIFY(Planner::Initialize(source, initial));
        auto output = sentinel(initial); const auto unchanged = output; CExportControl control; int calls = 0;
        QVERIFY(control.SetHandler([&](const auto&, QString*) {
            if (++calls == 1) source.limits.maximumOperations = 100000;
            if (calls == restoreAt) source.limits.maximumOperations = 64;
            return true;
        }));
        QString error; QVERIFY(!Planner::Move(source, initial, {"A"}, 1, {2, 1}, 0, 1, output, &error, &control));
        QVERIFY(calls > 0); QVERIFY(equal(output, unchanged)); QVERIFY2(error.contains(QStringLiteral("预算")), qPrintable(error));
        QVERIFY(Planner::Validate(originalSource, initial));
    }
    void callbacksCannotReplaceSelectedMeshIds()
    {
        auto source = snapshot({16, 12}, {24, 20}); source.meshes.append(mesh("B", 0, {{10.25, 5.25}, {12.75, 5.25}, {10.25, 7.75}}, {16, 12}));
        Planner::Plan initial, expected, actual; QVERIFY(Planner::Initialize(source, initial));
        QVERIFY(Planner::Move(source, initial, {"A"}, 1, {2, 1}, 0, 1, expected));
        QStringList ids{"A"}; CExportControl control;
        QVERIFY(control.SetHandler([&](const auto&, QString*) { ids = {"B"}; return true; }));
        QVERIFY(Planner::Move(source, initial, ids, 1, {2, 1}, 0, 1, actual, nullptr, &control));
        QVERIFY(equal(actual, expected)); QCOMPARE(actual.changedMeshes, QSet<QString>{"A"});
    }
    void operationsCaptureTheirSourceAndPlanBeforeCallbacks_data()
    {
        QTest::addColumn<int>("operation"); QTest::addColumn<bool>("aliased");
        for (int operation = 0; operation < 6; ++operation) for (bool aliased : {false, true})
            QTest::newRow(qPrintable(QStringLiteral("%1-%2").arg(operation).arg(aliased))) << operation << aliased;
    }
    void operationsCaptureTheirSourceAndPlanBeforeCallbacks()
    {
        QFETCH(int, operation); QFETCH(bool, aliased); auto source = snapshot({16, 12}, {24, 20});
        if (operation == 4) source.workspace.atlases.insert(2, {"generated", "unused", -1, {4, 4}, png(pattern({4, 4}, 3))});
        const auto trusted = source; Planner::Plan input, expected, output; QVERIFY(Planner::Initialize(source, input));
        const auto invoke = [&](Planner::Plan& result, CExportControl* control) {
            if (operation == 0) return Planner::Move(source, input, {"A"}, 1, {2, 1}, 0, 1, result, nullptr, control);
            if (operation == 1) return Planner::Pack(source, input, {"A"}, {1}, 1, false, result, nullptr, control);
            if (operation == 2) return Planner::ResizeAtlas(source, input, 0, {24, 20}, result, nullptr, control);
            if (operation == 3) return Planner::AddAtlas(source, input, "new", {4, 4}, result, nullptr, control);
            if (operation == 4) return Planner::RemoveAtlas(source, input, 2, result, nullptr, control);
            return Planner::Initialize(source, result, nullptr, control);
        };
        QVERIFY(invoke(expected, nullptr)); CExportControl control; int calls = 0;
        QVERIFY(control.SetHandler([&](const auto&, QString*) { ++calls; source = {}; input = {}; return true; }));
        auto& actual = aliased ? input : output; QVERIFY(invoke(actual, &control)); QVERIFY(calls > 0);
        QVERIFY(Planner::Validate(trusted, actual)); auto compared = actual;
        if (operation == 3) compared.state.workspace.atlases[2].id = expected.state.workspace.atlases[2].id;
        QVERIFY(equal(compared, expected));
    }
    void initializationOwnsBorrowedInputBeforeItsFirstCallback()
    {
        const auto trusted = snapshot(); auto source = trusted; Planner::Plan expected, actual; QVERIFY(Planner::Initialize(trusted, expected));
        QByteArray bytes = source.sources[0].png; auto* rawBytes = bytes.data();
        source.sources[0].png = QByteArray::fromRawData(rawBytes, bytes.size());
        QString name = source.workspace.atlases[0].name; auto* rawName = name.data();
        source.workspace.atlases[0].name = QString::fromRawData(rawName, name.size());
        QString id = source.meshes[0].material.id; auto* rawId = id.data(); source.meshes[0].material.id = QString::fromRawData(rawId, id.size());
        QByteArray digest = source.sourceDigest; auto* rawDigest = digest.data(); source.sourceDigest = QByteArray::fromRawData(rawDigest, digest.size());
        CExportControl control; int calls = 0;
        QVERIFY(control.SetHandler([&](const auto&, QString*) {
            if (++calls == 1) { rawBytes[0] = 0; rawName[0] = 'X'; rawId[0] = 'B'; rawDigest[0] = 'z'; source.limits.maximumPixels = 1; }
            return true;
        }));
        QString error; QVERIFY2(Planner::Initialize(source, actual, &error, &control), qPrintable(error));
        QVERIFY(calls > 0); QVERIFY(equal(actual, expected)); QVERIFY(Planner::Validate(trusted, actual)); QVERIFY(!Planner::Validate(source, actual));
    }
    void borrowedIdsAndTargetSlotsAreOwnedBeforeCallbacks_data()
    {
        QTest::addColumn<bool>("pack"); QTest::newRow("move") << false; QTest::newRow("pack") << true;
    }
    void borrowedIdsAndTargetSlotsAreOwnedBeforeCallbacks()
    {
        QFETCH(bool, pack); auto source = snapshot({16, 12}, {24, 20}); source.meshes.append(mesh("B", 0, {{10.25, 5.25}, {12.75, 5.25}, {10.25, 7.75}}, {16, 12}));
        Planner::Plan initial, expected, actual; QVERIFY(Planner::Initialize(source, initial));
        QVERIFY(pack ? Planner::Pack(source, initial, {"A"}, {1}, 1, false, expected)
                     : Planner::Move(source, initial, {"A"}, 1, {2, 1}, 0, 1, expected));
        QString storage = "A"; auto* rawId = storage.data(); QStringList ids{QString::fromRawData(rawId, storage.size())};
        QVector<int> targets{1}; auto* rawTarget = targets.data(); CExportControl control;
        QVERIFY(control.SetHandler([&](const auto&, QString*) { rawId[0] = 'B'; rawTarget[0] = 0; return true; }));
        QVERIFY(pack ? Planner::Pack(source, initial, ids, targets, 1, false, actual, nullptr, &control)
                     : Planner::Move(source, initial, ids, 1, {2, 1}, 0, 1, actual, nullptr, &control));
        QVERIFY(equal(actual, expected)); QVERIFY(Planner::Validate(source, actual));
    }
    void addedAtlasOwnsItsBorrowedTrimmedNameBeforeCallbacks()
    {
        const auto source = snapshot(); Planner::Plan initial, actual; QVERIFY(Planner::Initialize(source, initial));
        QString storage = "  Original  "; auto* raw = storage.data(); const auto name = QString::fromRawData(raw, storage.size());
        CExportControl control; QVERIFY(control.SetHandler([&](const auto&, QString*) { raw[2] = 'X'; return true; }));
        QVERIFY(Planner::AddAtlas(source, initial, name, {4, 4}, actual, nullptr, &control));
        QCOMPARE(actual.state.workspace.atlases[2].name, QStringLiteral("Original")); QVERIFY(Planner::Validate(source, actual));
    }
    void cancellationKeepsAliasedInputPlanUnchanged_data()
    {
        QTest::addColumn<int>("operation"); for (int operation = 0; operation < 5; ++operation) QTest::newRow(qPrintable(QString::number(operation))) << operation;
    }
    void cancellationKeepsAliasedInputPlanUnchanged()
    {
        QFETCH(int, operation); auto source = snapshot();
        source.workspace.atlases.insert(2, {"generated", "unused", -1, {4, 4}, png(pattern({4, 4}, 3))});
        Planner::Plan input; QVERIFY(Planner::Initialize(source, input)); const auto unchanged = input;
        CExportControl control; QVERIFY(control.SetHandler([&](const auto&, QString*) { control.Cancel(); return true; }));
        bool accepted;
        if (operation == 0) accepted = Planner::Move(source, input, {"A"}, 1, {2, 1}, 0, 1, input, nullptr, &control);
        else if (operation == 1) accepted = Planner::Pack(source, input, {"A"}, {1}, 1, false, input, nullptr, &control);
        else if (operation == 2) accepted = Planner::ResizeAtlas(source, input, 0, {13, 9}, input, nullptr, &control);
        else if (operation == 3) accepted = Planner::AddAtlas(source, input, "new", {4, 4}, input, nullptr, &control);
        else accepted = Planner::RemoveAtlas(source, input, 2, input, nullptr, &control);
        QVERIFY(!accepted); QVERIFY(control.WasCanceled()); QVERIFY(equal(input, unchanged)); QVERIFY(Planner::Validate(source, input));
    }
    void invalidArgumentCountsAndNamesFailBeforeCallbacks()
    {
        const auto source = snapshot(); Planner::Plan initial; QVERIFY(Planner::Initialize(source, initial)); auto output = sentinel(initial); const auto unchanged = output;
        CExportControl control; int calls = 0; QVERIFY(control.SetHandler([&](const auto&, QString*) { ++calls; return true; }));
        const QStringList tooMany(1000, QStringLiteral("A"));
        QVERIFY(!Planner::Move(source, initial, tooMany, 1, {2, 1}, 0, 1, output, nullptr, &control));
        QVERIFY(!Planner::Pack(source, initial, {"A"}, QVector<int>(1000, 1), 1, false, output, nullptr, &control));
        QVERIFY(!Planner::AddAtlas(source, initial, QString(257, 'X'), {4, 4}, output, nullptr, &control));
        QCOMPARE(calls, 0); QVERIFY(equal(output, unchanged));
        const QString name = QString(300, ' ') + QString(256, 'N') + QString(300, ' ');
        QVERIFY(Planner::AddAtlas(source, initial, name, {4, 4}, output)); QCOMPARE(output.state.workspace.atlases[2].name, QString(256, 'N'));
    }
    void movingArtworkPreservesEditablePixelsOutsideCurrentUvs_data()
    {
        QTest::addColumn<int>("turns"); QTest::newRow("translation") << 0; QTest::newRow("quarter-turn") << 1;
    }
    void movingArtworkPreservesEditablePixelsOutsideCurrentUvs()
    {
        QFETCH(int, turns); auto source = snapshot({16, 12}, {24, 20}); bindArtwork(source);
        source.artwork.placements["placement"].sourceDomain = QRectF(1, 1, 10, 8);
        Planner::Plan initial, moved; QString error; QVERIFY2(Planner::Initialize(source, initial, &error), qPrintable(error));
        QVERIFY2(Planner::Move(source, initial, {"A"}, 1, {2, 1}, turns, 1, moved, &error), qPrintable(error));
        const auto& placement = moved.state.artwork.placements[moved.state.artwork.bindings["A"].placementId];
        QCOMPARE(placement.sourceDomain, source.artwork.placements["placement"].sourceDomain);
        const QPointF familyPoint(9.5, 7.5), target = placement.familyToAtlas.map(familyPoint);
        auto materials = moved.state.materials; materials[0].uvs[0] = float(target.x() / 24); materials[0].uvs[1] = float(target.y() / 20);
        CModelArtwork::State adjusted; CArtworkReconciler::Triangles triangles{{"A", source.meshes[0].triangles}};
        QVERIFY2(CArtworkReconciler::ManualAppearance(source.sources, moved.state, moved.state.workspace, materials,
                                                     triangles, adjusted, &error), qPrintable(error));
        QCOMPARE(adjusted.bindings["A"].status, CModelArtwork::BindingStatus::Linked);
        QCOMPARE(pixel(moved.images[1], int(std::floor(target.x())), int(std::floor(target.y()))), pixel(initial.images[0], 9, 7));
        const auto expected = movedImage(initial.images[0], initial.images[1], QRect(0, 0, 12, 10), forward(QRect(0, 0, 12, 10), {2, 1}, turns));
        QCOMPARE(rgba(moved.images[1]), rgba(expected));
    }
    void artworkSelectionUsesMappedDomainAndExistingGutter()
    {
        auto source = snapshot({16, 12}, {24, 20}); bindArtwork(source);
        auto& placement = source.artwork.placements["placement"];
        placement.sourceDomain = QRectF(1, 1, 10, 10); placement.gutter = 5;
        placement.familyToAtlas = QTransform(0, 1, -1, 0, 12, 0);
        const auto inverse = placement.familyToAtlas.inverted();
        for (auto& point : source.artwork.bindings["A"].familyCoordinates) point = inverse.map(point);
        Planner::Plan initial; QString error; QVERIFY2(Planner::Initialize(source, initial, &error), qPrintable(error));
        QRect bounds; int slot = -1;
        QVERIFY2(Planner::SelectionBounds(source, initial, {"A"}, 2, bounds, slot, &error), qPrintable(error));
        QCOMPARE(bounds, QRect(-4, -4, 20, 20)); QCOMPARE(slot, 0);
    }
    void detachedAndUnlinkedArtworkRetainUvOnlyBounds_data()
    {
        QTest::addColumn<bool>("detached"); QTest::newRow("unlinked") << false; QTest::newRow("detached") << true;
    }
    void detachedAndUnlinkedArtworkRetainUvOnlyBounds()
    {
        QFETCH(bool, detached); auto source = snapshot({16, 12}, {24, 20});
        if (detached) {
            bindArtwork(source); auto& binding = source.artwork.bindings["A"];
            binding.status = CModelArtwork::BindingStatus::Detached; binding.detachedReason = "manual";
            source.artwork.placements["placement"].gutter = 5;
        }
        Planner::Plan initial, moved; QVERIFY(Planner::Initialize(source, initial)); QRect bounds; int slot = -1;
        QVERIFY(Planner::SelectionBounds(source, initial, {"A"}, 1, bounds, slot)); QCOMPARE(bounds, QRect(0, 0, 6, 4));
        QVERIFY(Planner::Move(source, initial, {"A"}, 1, {2, 1}, 0, 1, moved));
        QCOMPARE(rgba(moved.images[1]), rgba(movedImage(initial.images[0], initial.images[1], bounds, forward(bounds, {2, 1}, 0))));
    }
    void packingArtworkCopiesItsCompleteEditableDomain()
    {
        auto source = snapshot({16, 12}, {24, 20}); auto shared = source.meshes[0]; shared.material.id = "Shared";
        source.meshes.append(shared); bindArtwork(source);
        source.artwork.placements["placement"].sourceDomain = QRectF(1, 1, 10, 8);
        Planner::Plan initial, packed; QString error; QVERIFY(Planner::Initialize(source, initial));
        QVERIFY2(Planner::Pack(source, initial, {"A"}, {1}, 1, true, packed, &error), qPrintable(error));
        const auto& placement = packed.state.artwork.placements[packed.state.artwork.bindings["A"].placementId];
        for (int y = 0; y < 10; ++y) for (int x = 0; x < 12; ++x) {
            const auto target = placement.familyToAtlas.map(QPointF(x + .5, y + .5));
            QCOMPARE(pixel(packed.images[1], int(std::floor(target.x())), int(std::floor(target.y()))), pixel(initial.images[0], x, y));
        }
        QCOMPARE(placement.sourceDomain, source.artwork.placements["placement"].sourceDomain);
        QCOMPARE(packed.state.artwork.bindings["A"].status, CModelArtwork::BindingStatus::Linked);
        QVERIFY(packed.state.artwork.bindings["A"].placementId != packed.state.artwork.bindings["Shared"].placementId);
        QVERIFY(packed.state.artwork.bindings["Shared"] == initial.state.artwork.bindings["Shared"]);
        QCOMPARE(rgba(packed.images[0]), rgba(initial.images[0]));
    }
    void unselectedArtworkProtectsDomainOutsideItsUvs_data()
    {
        QTest::addColumn<bool>("emptyUvs"); QTest::newRow("existing-vertices") << false; QTest::newRow("no-vertices") << true;
    }
    void unselectedArtworkProtectsDomainOutsideItsUvs()
    {
        QFETCH(bool, emptyUvs); auto source = snapshot({16, 12}, {24, 20}); source.meshes[0].material.id = "Protected";
        if (emptyUvs) { source.meshes[0].material.uvs.clear(); source.meshes[0].triangles.clear(); }
        source.meshes.append(mesh("Mover", 1, {{1.25, 1.25}, {2.75, 1.25}, {1.25, 2.75}}, {24, 20}));
        bindArtwork(source); source.artwork.placements["placement"].sourceDomain = QRectF(1, 1, 10, 8);
        Planner::Plan initial; QString error; QVERIFY2(Planner::Initialize(source, initial, &error), qPrintable(error));
        auto output = sentinel(initial); const auto unchanged = output;
        QVERIFY(!Planner::Move(source, initial, {"Mover"}, 0, {8, 6}, 0, 1, output, &error));
        QVERIFY(equal(output, unchanged)); QVERIFY2(error.contains("Protected"), qPrintable(error));
    }
    void selectedGutterDoesNotExpandUnselectedProtection_data()
    {
        QTest::addColumn<bool>("linked"); QTest::newRow("uv-only") << false; QTest::newRow("linked-domain") << true;
    }
    void selectedGutterDoesNotExpandUnselectedProtection()
    {
        QFETCH(bool, linked); auto source = snapshot({64, 48}, {64, 48});
        source.meshes = {mesh("Protected", 0, {{27.25, 3.25}, {30.75, 3.25}, {27.25, 5.75}}, {64, 48}),
                         mesh("Mover", 1, {{1.25, 1.25}, {4.75, 1.25}, {1.25, 2.75}}, {64, 48})};
        if (linked) { bindArtwork(source); source.artwork.placements["placement"].sourceDomain = QRectF(27, 2, 5, 5); }
        Planner::Plan initial, moved; QString error; QVERIFY(Planner::Initialize(source, initial));
        QRect patch; int slot = -1; QVERIFY(Planner::SelectionBounds(source, initial, {"Mover"}, 8, patch, slot));
        QCOMPARE(patch, QRect(-7, -7, 20, 18)); QCOMPARE(slot, 1);
        QVERIFY2(Planner::Move(source, initial, {"Mover"}, 0, {0, 0}, 0, 8, moved, &error), qPrintable(error));
        QCOMPARE(rgba(moved.images[0]), rgba(movedImage(initial.images[1], initial.images[0], patch, forward(patch, {0, 0}, 0))));
        for (int y = 1; y < 8; ++y) for (int x = 26; x < 33; ++x) QCOMPARE(pixel(moved.images[0], x, y), pixel(initial.images[0], x, y));
        if (linked) {
            QVERIFY(moved.state.artwork.bindings["Protected"] == initial.state.artwork.bindings["Protected"]);
            QVERIFY(moved.state.artwork.placements == initial.state.artwork.placements);
        }
    }
    void smallerSelectedGutterPreservesUnselectedArtworkGutter()
    {
        auto source = snapshot({64, 48}, {64, 48});
        source.meshes = {mesh("Protected", 0, {{27.25, 3.25}, {30.75, 3.25}, {27.25, 5.75}}, {64, 48}),
                         mesh("Mover", 1, {{1.25, 1.25}, {4.75, 1.25}, {1.25, 2.75}}, {64, 48})};
        bindArtwork(source); source.artwork.placements["placement"].sourceDomain = QRectF(27, 2, 5, 5);
        source.artwork.placements["placement"].gutter = 5;
        Planner::Plan initial; QVERIFY(Planner::Initialize(source, initial)); auto output = sentinel(initial); const auto unchanged = output; QString error;
        QVERIFY(!Planner::Move(source, initial, {"Mover"}, 0, {20, 3}, 0, 1, output, &error));
        QVERIFY(equal(output, unchanged)); QVERIFY2(error.contains("Protected"), qPrintable(error));
    }
    void resizingArtworkPreservesRepeatedDomainPixels()
    {
        auto source = snapshot({16, 12}, {24, 20}); bindArtwork(source);
        source.artwork.placements["placement"].sourceDomain = QRectF(1, 1, 18, 12);
        Planner::Plan initial, grown; QString error; QVERIFY(Planner::Initialize(source, initial));
        QVERIFY2(Planner::ResizeAtlas(source, initial, 0, {24, 20}, grown, &error), qPrintable(error));
        for (int y = 0; y < 14; ++y) for (int x = 0; x < 20; ++x)
            QCOMPARE(pixel(grown.images[0], x, y), pixel(initial.images[0], x % 16, y % 12));
        QVERIFY(grown.state.artwork.placements == initial.state.artwork.placements);
        QCOMPARE(grown.state.artwork.bindings["A"].status, CModelArtwork::BindingStatus::Linked);
    }
    void shrinkingArtworkRejectsDomainCollisionsBeyondCurrentUvs()
    {
        auto source = snapshot({16, 12}, {24, 20}); bindArtwork(source);
        source.artwork.placements["placement"].sourceDomain = QRectF(1, 1, 10, 8);
        Planner::Plan initial; QVERIFY(Planner::Initialize(source, initial)); auto output = sentinel(initial); const auto unchanged = output; QString error;
        QVERIFY(!Planner::ResizeAtlas(source, initial, 0, {8, 6}, output, &error));
        QVERIFY(equal(output, unchanged)); QVERIFY2(error.contains(QStringLiteral("重复采样")), qPrintable(error));
    }
    void completeArtworkDomainSharesOperationBudget_data()
    {
        QTest::addColumn<int>("operation"); QTest::newRow("move") << 0; QTest::newRow("pack") << 1; QTest::newRow("resize") << 2;
    }
    void completeArtworkDomainSharesOperationBudget()
    {
        QFETCH(int, operation); auto source = snapshot({16, 12}, {24, 20}); bindArtwork(source);
        source.artwork.placements["placement"].sourceDomain = QRectF(1, 1, 10, 8);
        source.limits.maximumOperations = operation == 2 ? 1220 : 550;
        Planner::Plan initial; QVERIFY(Planner::Initialize(source, initial)); auto output = sentinel(initial); const auto unchanged = output; QString error;
        bool accepted;
        if (operation == 0) accepted = Planner::Move(source, initial, {"A"}, 1, {2, 1}, 0, 1, output, &error);
        else if (operation == 1) accepted = Planner::Pack(source, initial, {"A"}, {1}, 1, true, output, &error);
        else accepted = Planner::ResizeAtlas(source, initial, 0, {24, 20}, output, &error);
        QVERIFY(!accepted); QVERIFY(equal(output, unchanged)); QVERIFY2(error.contains(QStringLiteral("预算")), qPrintable(error));
    }
    void sharedArtworkDomainIsBudgetedOnceForSelection()
    {
        auto source = snapshot({16, 12}, {24, 20}); const auto original = source.meshes[0]; source.meshes.clear(); QStringList ids;
        for (int index = 0; index < 128; ++index) {
            auto shared = original; shared.material.id = QStringLiteral("Shared%1").arg(index);
            ids.append(shared.material.id); source.meshes.append(shared);
        }
        bindArtwork(source); source.artwork.placements["placement"].sourceDomain = QRectF(1, 1, 10, 8);
        source.limits.maximumOperations = 128 * (original.material.uvs.size() / 2 + 1) + 4;
        Planner::Plan initial; QString error; QVERIFY(Planner::Initialize(source, initial)); QRect bounds; int slot = -1;
        QVERIFY2(Planner::SelectionBounds(source, initial, ids, 1, bounds, slot, &error), qPrintable(error));
        QCOMPARE(bounds, QRect(0, 0, 12, 10)); QCOMPARE(slot, 0);
    }
    void completeArtworkDomainCannotBeSilentlyCroppedToTarget_data()
    {
        QTest::addColumn<bool>("pack"); QTest::newRow("move") << false; QTest::newRow("pack") << true;
    }
    void completeArtworkDomainCannotBeSilentlyCroppedToTarget()
    {
        QFETCH(bool, pack); auto source = snapshot({16, 12}, {8, 8}); bindArtwork(source);
        source.artwork.placements["placement"].sourceDomain = QRectF(1, 1, 10, 8);
        Planner::Plan initial; QVERIFY(Planner::Initialize(source, initial)); auto output = sentinel(initial); const auto unchanged = output; QString error;
        const bool accepted = pack ? Planner::Pack(source, initial, {"A"}, {1}, 1, true, output, &error)
                                   : Planner::Move(source, initial, {"A"}, 1, {0, 0}, 0, 1, output, &error);
        QVERIFY(!accepted); QVERIFY(equal(output, unchanged)); QVERIFY2(error.contains(QStringLiteral("无法放入")), qPrintable(error));
    }
    void artworkSharedPlacementSurvivesPartialRotationAndReturningToSource()
    {
        auto source = snapshot({16, 12}, {24, 20}); auto other = source.meshes[0]; other.material.id = "Shared"; other.editable = false; source.meshes.append(other); bindArtwork(source);
        source.artwork.placements["placement"].sourceDomain = QRectF(1, 1, 10, 8);
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
        auto source = snapshot({16, 12}, {24, 20}); bindArtwork(source); Planner::Plan original, resized, restored, added, moved, returned, removed; QString error;
        source.artwork.placements["placement"].sourceDomain = QRectF(1, 1, 10, 8);
        QVERIFY(Planner::Initialize(source, original));
        QVERIFY2(Planner::ResizeAtlas(source, original, 0, {24, 20}, resized, &error), qPrintable(error));
        QCOMPARE(resized.state.artwork.bindings["A"].familyCoordinates, original.state.artwork.bindings["A"].familyCoordinates);
        QVERIFY(resized.state.artwork.placements == source.artwork.placements);
        QVERIFY2(Planner::ResizeAtlas(source, resized, 0, {16, 12}, restored, &error), qPrintable(error)); QVERIFY(restored.state.artwork == source.artwork);
        QVERIFY(Planner::AddAtlas(source, restored, "暂存", {24, 24}, added));
        QVERIFY2(Planner::Move(source, added, {"A"}, 2, {0, 0}, 1, 1, moved, &error), qPrintable(error));
        QVERIFY2(Planner::Move(source, moved, {"A"}, 0, {0, 0}, 3, 1, returned, &error), qPrintable(error));
        QVERIFY(Planner::RemoveAtlas(source, returned, 2, removed)); QVERIFY(removed.state == original.state);
    }
    void artworkPackKeepsSharedSourceAndIndependentUnusedPlacement()
    {
        auto source = snapshot(); auto other = source.meshes[0]; other.material.id = "Shared"; source.meshes.append(other); bindArtwork(source);
        Planner::Plan initial, added, packed; QString error; QVERIFY(Planner::Initialize(source, initial)); QVERIFY(Planner::AddAtlas(source, initial, "打包", {16, 16}, added));
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
