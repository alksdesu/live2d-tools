#include "live2dstruct/cruntimepackage.h"
#include "live2dstruct/cartworkpackage.h"
#include "live2dstruct/cmocdeformerorder.h"
#include "live2dstruct/cparameterresourceedits.h"
#include "deformer_order_fixture.h"
#include "std/cmodeljson.h"
#include "std/cmodelresourcepath.h"
#include "std/cexportcontrol.h"
#include "live2dstruct/csectiontable.h"
#include <QtEndian>
#include <QtTest>
#include <QCryptographicHash>
#include <QTemporaryDir>
#include <QDir>
#include <QFile>
#include <QBuffer>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <deque>

using namespace live2dstruct;
namespace {
QByteArray read(const QString& path) { QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray(); }
bool write(const QString& path, const QByteArray& bytes) { QFile file(path); return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(); }
QByteArray digest(const QByteArray& bytes) { return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256); }
QByteArray json(const QJsonObject& root) { return QJsonDocument(root).toJson(QJsonDocument::Indented); }
QByteArray png(QRgb color, QSize size = {4, 4})
{
    QImage image(size, QImage::Format_ARGB32); image.fill(color);
    QByteArray bytes; QBuffer buffer(&bytes);
    return buffer.open(QIODevice::WriteOnly) && image.save(&buffer, "PNG") ? bytes : QByteArray();
}
QByteArray entry(const CRuntimePackage::Prepared& prepared, const QString& name)
{
    for (const auto& file : prepared.entries) if (file.relativePath == name) return file.bytes;
    return {};
}
CRuntimePackage::Request request(const QString& path)
{
    CRuntimePackage::Request result; result.modelJsonPath = path;
    const auto bytes = read(path); result.modelSettingsDigest = digest(bytes);
    const auto files = QJsonDocument::fromJson(bytes).object()["FileReferences"].toObject();
    result.mocBytes = read(CModelResourcePath::Resolve(QFileInfo(path).absolutePath(), files["Moc"].toString()));
    result.sourceMocDigest = digest(result.mocBytes); return result;
}
struct Fixture {
    QTemporaryDir directory;
    QString path;
    QJsonObject root;
    CModelDocument document;
    Fixture()
    {
        const auto home = QString(TEST_RESOURCES) + "/Mark";
        const auto files = QJsonDocument::fromJson(read(home + "/Mark.model3.json")).object()["FileReferences"].toObject();
        path = directory.filePath(QStringLiteral("模型.model3.json"));
        root = {{"Version", 3}, {"Layout", QJsonObject{{"CenterX", 0}}}, {"Custom", "kept"},
            {"FileReferences", QJsonObject{{"Moc", home + "/" + files["Moc"].toString()},
                {"Textures", QJsonArray{home + "/" + files["Textures"].toArray().first().toString()}}}}};
        document.LoadBytes(read(home + "/" + files["Moc"].toString()), {});
        save();
    }
    bool save() const { return write(path, json(root)); }
    void resource(const QString& field, const QString& name, const QByteArray& bytes)
    {
        write(directory.filePath(name), bytes); auto files = root["FileReferences"].toObject(); files[field] = name; root["FileReferences"] = files; save();
    }
};

bool artworkRequest(Fixture& fixture, CRuntimePackage::Request& input, QString* error)
{
    using Artwork = CModelArtwork;
    const auto image = png(qRgba(21, 71, 81, 94), {6, 5});
    const auto original = png(qRgba(17, 39, 2, 91), {4, 4});
    const auto path = fixture.directory.filePath("original.png");
    if (!write(path, original)) return false;
    auto files = fixture.root["FileReferences"].toObject(); files["Textures"] = QJsonArray{"original.png"};
    fixture.root["FileReferences"] = files; if (!fixture.save()) return false;
    input = request(fixture.path); CTextureAssets::Source source;
    if (!CTextureAssets::MakeSource(path, original, source, error)) return false;
    input.textureSources = CTextureAssets::Catalog{{0, source}};
    CTextureWorkspace::State workspace;
    if (!CTextureWorkspace::Initialize(*input.textureSources, {}, workspace, error)) return false;
    workspace.atlases.insert(1, {"generated-source", "生成图集", -1, {6, 5}, image});
    input.textureWorkspace = workspace;
    CModelDocument document; if (!document.LoadBytes(input.mocBytes, {}, error)) return false;
    CModelDrawableMaterial material;
    if (!document.CaptureDrawableMaterial("ArmR", material, error)) return false;
    material.textureIndex = 1;
    if (!document.SetDrawableMaterials({material}, error) || !document.BuildRuntimeMoc(input.mocBytes, error)) return false;
    Artwork::State state; state.images.insert(digest(image), {{6, 5}, image});
    state.images.insert(digest(original), {{4, 4}, original});
    state.families.insert("source", {"source", "原稿", Artwork::SourceKind::Png, "latest"});
    Artwork::Revision revision; revision.id = "old"; revision.familyId = "source";
    revision.path = fixture.directory.filePath("artwork.png"); revision.fileName = "artwork.png";
    revision.sourceBytes = image; revision.sourceDigest = digest(image); revision.canvasSize = {6, 5};
    revision.iccProfile = QByteArray("saved ICC bytes");
    Artwork::Layer layer; layer.id = "pixels"; layer.name = "原图层"; layer.sourceIndex = 0;
    layer.bounds = {0, 0, 6, 5}; layer.imageDigest = digest(image); revision.layers = {layer};
    state.revisions.insert(revision.id, revision);
    revision.id = "latest"; revision.sourceBytes = original; revision.sourceDigest = digest(original);
    revision.canvasSize = {4, 4}; revision.layers[0].imageDigest = digest(original); revision.layers[0].bounds = {0, 0, 4, 4};
    state.revisions.insert(revision.id, revision);
    state.materials.insert("material", {"material", "材质", "source", "old", "pixels"});
    state.placements.insert("placement", {"placement", "material", "generated-source", {}, {0, 0, 6, 5}, 1});
    Artwork::Binding binding; binding.meshId = material.id; binding.placementId = "placement"; binding.sampledMaterial = material;
    for (int i = 0; i < material.uvs.size(); i += 2) binding.familyCoordinates.append({double(material.uvs[i]) * 6, double(material.uvs[i + 1]) * 5});
    if (!Artwork::TopologyDigest(material.uvs.size() / 2, document.DrawableIndices(material.id), binding.topologyDigest, error)) return false;
    state.bindings.insert(material.id, binding);
    if (!Artwork::Freeze(state, state, error)) return false;
    input.artwork = state; return true;
}

bool publish(const CRuntimePackage::Prepared& prepared, const QString& directory, QString* error)
{
    CDirectoryExport::Options options; options.protectedPaths = prepared.protectedPaths;
    CDirectoryExport::Result result;
    return CDirectoryExport::Publish(directory, prepared.entries, options, result, nullptr, error) && result.committed;
}

bool loadArtworkPackage(const QString& modelPath, CModelDocument& document, CArtworkPackage::Loaded& loaded, QString* error)
{
    const auto settings = read(modelPath); const auto files = QJsonDocument::fromJson(settings).object()["FileReferences"].toObject();
    const auto directory = QFileInfo(modelPath).absoluteDir();
    if (!document.Load(directory.filePath(files["Moc"].toString()), error)) return false;
    CTextureAssets::Catalog sources; const auto textures = files["Textures"].toArray();
    for (int i = 0; i < textures.size(); ++i) if (!textures[i].toString().isEmpty()) {
        const auto path = directory.filePath(textures[i].toString()); CTextureAssets::Source source;
        if (!CTextureAssets::MakeSource(path, read(path), source, error)) return false;
        sources.insert(i, source);
    }
    return document.InitializeTextureWorkspace(sources, {}, error) && CArtworkPackage::Load(modelPath, settings, document, loaded, nullptr, error);
}

struct Borrowed {
    std::deque<QByteArray> bytes;
    std::deque<QString> strings;
    QByteArray borrow(const QByteArray& value) {
        bytes.emplace_back(value.constData(), value.size()); auto& last = bytes.back();
        return QByteArray::fromRawData(last.constData(), last.size());
    }
    QString borrow(const QString& value) {
        strings.emplace_back(value.constData(), value.size()); auto& last = strings.back();
        return QString::fromRawData(last.constData(), last.size());
    }
    QSet<QString> borrow(const QSet<QString>& values) { QSet<QString> result; for (const auto& value : values) result.insert(borrow(value)); return result; }
    void corrupt() {
        for (auto& value : bytes) std::fill(value.data(), value.data() + value.size(), 'x');
        for (auto& value : strings) std::fill(value.data(), value.data() + value.size(), QChar('x'));
    }
};

template<class Map, class Transform>
void borrowMap(Map& values, Borrowed& storage, Transform transform)
{
    Map result;
    for (auto it = values.cbegin(); it != values.cend(); ++it) { auto value = it.value(); transform(value); result.insert(storage.borrow(it.key()), value); }
    values = std::move(result);
}

void borrowRequest(CRuntimePackage::Request& input, Borrowed& storage)
{
    const auto string = [&](QString& value) { value = storage.borrow(value); };
    const auto bytes = [&](QByteArray& value) { value = storage.borrow(value); };
    string(input.modelJsonPath); string(input.registeredUserDataPath); string(input.timelineGroup); string(input.timelineSoundPath);
    bytes(input.modelSettingsDigest); bytes(input.sourceMocDigest); bytes(input.mocBytes); bytes(input.loadedModelSettingsBytes);
    for (auto* value : {&input.physics, &input.pose, &input.userData, &input.timeline}) if (*value) bytes(**value);
    borrowMap(input.expectedSourceDigests, storage, bytes);
    auto& references = input.references; string(references.modelPath); bytes(references.modelSource);
    references.modelSuppressedIds = storage.borrow(references.modelSuppressedIds); references.breath = storage.borrow(references.breath); references.mouseFollow = storage.borrow(references.mouseFollow);
    borrowMap(references.resources, storage, [&](auto& resource) { bytes(resource.source); resource.suppressedIds = storage.borrow(resource.suppressedIds); });
    borrowMap(input.parameterMetadata, storage, [&](auto& value) { string(value.displayName); });
    for (auto& node : input.parameterOrganization.nodes) { string(node.id); string(node.name); string(node.parentFolder); string(node.color); }
    for (auto& pair : input.parameterOrganization.pairs) { string(pair.first); string(pair.second); }
    bytes(input.geometryObjectPairs.sourceDigest);
    for (auto& pair : input.geometryObjectPairs.pairs) {
        string(pair.id);
        for (auto* endpoint : {&pair.first, &pair.second}) { string(endpoint->object.id); for (auto& axis : endpoint->axes) string(axis.parameterId); }
    }
    if (input.textureSources) for (auto& source : *input.textureSources) { string(source.path); bytes(source.png); bytes(source.digest); }
    for (auto& png : input.textureState) bytes(png);
    if (input.textureWorkspace) {
        bytes(input.textureWorkspace->sourceFingerprint);
        for (auto& atlas : input.textureWorkspace->atlases) { string(atlas.id); string(atlas.name); bytes(atlas.png); }
    }
    if (input.artwork) {
        auto& state = *input.artwork;
        borrowMap(state.images, storage, [&](auto& image) { bytes(image.png); });
        borrowMap(state.families, storage, [&](auto& family) { string(family.id); string(family.name); string(family.currentRevision); });
        borrowMap(state.revisions, storage, [&](auto& revision) {
            string(revision.id); string(revision.familyId); string(revision.path); string(revision.fileName);
            bytes(revision.sourceDigest); bytes(revision.sourceBytes); bytes(revision.iccProfile);
            for (auto& layer : revision.layers) {
                string(layer.id); string(layer.name); bytes(layer.blendMode); bytes(layer.imageDigest);
                for (auto& diagnostic : layer.diagnostics) string(diagnostic);
            }
        });
        borrowMap(state.materials, storage, [&](auto& material) { string(material.id); string(material.name); string(material.familyId); string(material.revisionId); string(material.layerId); string(material.missingInRevision); });
        borrowMap(state.placements, storage, [&](auto& placement) { string(placement.id); string(placement.materialId); string(placement.atlasId); });
        borrowMap(state.bindings, storage, [&](auto& binding) { string(binding.meshId); string(binding.placementId); string(binding.sampledMaterial.id); string(binding.detachedReason); bytes(binding.topologyDigest); });
    }
}
}
class RuntimePackageTests : public QObject
{
    Q_OBJECT
private slots:
    void artworkAttachmentsAreExplicitAndPreserveEveryRevision()
    {
        Fixture fixture; CRuntimePackage::Request input; QString error;
        QVERIFY2(artworkRequest(fixture, input, &error), qPrintable(error));
        const auto artwork = *input.artwork; input.artwork.reset();
        CRuntimePackage::Prepared ordinary, attached;
        QVERIFY2(CRuntimePackage::Prepare(input, ordinary, nullptr, &error), qPrintable(error));
        const auto manifest = CArtworkPackage::ManifestFile(ordinary.modelFile);
        QVERIFY(entry(ordinary, manifest).isEmpty());
        input.artwork = artwork;
        QVERIFY2(CRuntimePackage::Prepare(input, attached, nullptr, &error), qPrintable(error));
        QCOMPARE(attached.model, ordinary.model); QCOMPARE(entry(attached, attached.modelFile), entry(ordinary, ordinary.modelFile));
        for (const auto& file : ordinary.entries) QCOMPARE(entry(attached, file.relativePath), file.bytes);
        QVERIFY(!entry(attached, manifest).isEmpty());
        const auto index = QJsonDocument::fromJson(entry(attached, manifest)).object();
        QCOMPARE(index["artwork"].toObject()["revisions"].toObject().size(), 2);
        QCOMPARE(index["artwork"].toObject()["materials"].toObject()["material"].toObject()["revisionId"].toString(), QString("old"));
        QVERIFY(!index["artwork"].toObject()["families"].toObject().isEmpty());
        for (const auto& revision : artwork.revisions) {
            QVERIFY(attached.protectedPaths.contains(revision.path));
            QVERIFY(index["objects"].toObject().contains(QString::fromLatin1(revision.sourceDigest.toHex())));
        }
        qint64 total = 0; QSet<QString> paths;
        for (const auto& file : attached.entries) { total += file.bytes.size(); QVERIFY(!paths.contains(file.relativePath)); paths.insert(file.relativePath); }
        QCOMPARE(attached.totalBytes, total); QVERIFY(attached.totalBytes > ordinary.totalBytes);
        QTemporaryDir target; QVERIFY2(publish(attached, target.filePath("package"), &error), qPrintable(error));
        CModelDocument document; CArtworkPackage::Loaded loaded;
        QVERIFY2(loadArtworkPackage(target.filePath("package/") + attached.modelFile, document, loaded, &error), qPrintable(error));
        QVERIFY(loaded.present); QCOMPARE(loaded.state.artwork.revisions.size(), 2);
        QCOMPARE(loaded.state.artwork.revisions["old"].sourceBytes, artwork.revisions["old"].sourceBytes);
        QCOMPARE(loaded.state.artwork.materials, artwork.materials);
        QCOMPARE(loaded.state.artwork.bindings, artwork.bindings);
        QCOMPARE(loaded.state.artwork.placements["placement"].atlasId, QString("source:1"));
    }

    void artworkPackageReexportsAfterGeneratedAtlasesBecomeSources()
    {
        Fixture fixture; CRuntimePackage::Request input; QString error;
        QVERIFY2(artworkRequest(fixture, input, &error), qPrintable(error));
        const auto original = *input.artwork; QTemporaryDir target;
        for (int cycle = 0; cycle < 3; ++cycle) {
            CRuntimePackage::Prepared output; QVERIFY2(CRuntimePackage::Prepare(input, output, nullptr, &error), qPrintable(error));
            const auto directory = target.filePath(QString::number(cycle));
            QVERIFY2(publish(output, directory, &error), qPrintable(error));
            CModelDocument document; CArtworkPackage::Loaded loaded;
            const auto modelPath = directory + '/' + output.modelFile;
            QVERIFY2(loadArtworkPackage(modelPath, document, loaded, &error), qPrintable(error)); QVERIFY(loaded.present);
            QCOMPARE(document.TextureWorkspace().atlases[1].sourceSlot, 1);
            QVERIFY(document.TextureWorkspace().atlases[1].png.isEmpty());
            QCOMPARE(loaded.state.artwork.bindings, original.bindings);
            QCOMPARE(loaded.state.artwork.revisions["old"].sourceBytes, original.revisions["old"].sourceBytes);
            QCOMPARE(loaded.state.artwork.revisions["latest"].sourceBytes, original.revisions["latest"].sourceBytes);
            QVERIFY(document.SetTextureWorkspace(loaded.state.workspace, &error)); QVERIFY(document.SetArtwork(loaded.state.artwork, &error));
            input = request(modelPath); input.textureSources = document.TextureSources(); input.textureWorkspace = document.TextureWorkspace(); input.artwork = document.Artwork();
        }
    }

    void artworkSnapshotsRemainExportableWithoutDiskFiles_data()
    {
        QTest::addColumn<int>("kind"); QTest::newRow("missing") << 0; QTest::newRow("changed") << 1;
        QTest::newRow("old-expected") << 2; QTest::newRow("latest-expected") << 3; QTest::newRow("bad-expected") << 4;
    }
    void artworkSnapshotsRemainExportableWithoutDiskFiles()
    {
        QFETCH(int, kind); Fixture fixture; CRuntimePackage::Request input; QString error;
        QVERIFY2(artworkRequest(fixture, input, &error), qPrintable(error));
        const auto path = input.artwork->revisions["old"].path;
        if (kind == 1) QVERIFY(write(path, "changed externally"));
        if (kind >= 2) input.expectedSourceDigests[path] = kind == 4 ? QByteArray(32, 'x') : input.artwork->revisions[kind == 2 ? "old" : "latest"].sourceDigest;
        CRuntimePackage::Prepared output; output.totalBytes = 71;
        QCOMPARE(CRuntimePackage::Prepare(input, output, nullptr, &error), kind != 4);
        if (kind == 4) { QCOMPARE(output.totalBytes, 71); QVERIFY(output.entries.isEmpty()); return; }
        QVERIFY(output.protectedPaths.contains(path)); int snapshots = 0;
        for (const auto& source : output.sources) if (source.path == path) ++snapshots;
        QCOMPARE(snapshots, 2); QVERIFY(!entry(output, CArtworkPackage::ManifestFile(output.modelFile)).isEmpty());
        if (kind == 1) QCOMPARE(read(path), QByteArray("changed externally")); else QVERIFY(!QFileInfo::exists(path));
        QVERIFY(!publish(output, fixture.directory.path(), &error));
    }

    void invalidArtworkAndCombinedBudgetsPreserveOutput_data()
    {
        QTest::addColumn<int>("kind");
        const char* names[]{"no-sources", "no-workspace", "bad-original", "stale-uv", "missing-atlas", "total-budget", "file-budget", "count-budget", "metadata-budget"};
        for (int i = 0; i < 9; ++i) QTest::newRow(names[i]) << i;
    }
    void invalidArtworkAndCombinedBudgetsPreserveOutput()
    {
        QFETCH(int, kind); Fixture fixture; CRuntimePackage::Request input; QString error;
        QVERIFY2(artworkRequest(fixture, input, &error), qPrintable(error));
        CRuntimePackage::Prepared expected; QVERIFY2(CRuntimePackage::Prepare(input, expected, nullptr, &error), qPrintable(error));
        if (kind == 0) input.textureSources.reset();
        if (kind == 1) input.textureWorkspace.reset();
        if (kind == 2) input.artwork->revisions["old"].sourceBytes = "corrupted";
        if (kind == 3) input.artwork->bindings["ArmR"].sampledMaterial.uvs[0] += .01f;
        if (kind == 4) input.artwork->placements["placement"].atlasId = "absent";
        if (kind == 5) input.maximumTotalBytes = expected.totalBytes - 1;
        if (kind == 6) input.maximumFileBytes = input.mocBytes.size() - 1;
        if (kind == 7) input.maximumFiles = expected.entries.size() - 1;
        if (kind == 8) input.artwork->families["source"].name = QString(50000, 'x');
        CRuntimePackage::Prepared output; output.modelFile = "kept"; output.totalBytes = 71; output.entries = {{"held", "owned"}};
        QVERIFY(!CRuntimePackage::Prepare(input, output, nullptr, &error)); QVERIFY(!error.isEmpty());
        QCOMPARE(output.modelFile, QString("kept")); QCOMPARE(output.totalBytes, 71); QCOMPARE(output.entries.size(), 1); QCOMPARE(output.entries.first().bytes, QByteArray("owned"));
    }

    void artworkCancellationAndExactOutputBudget()
    {
        Fixture fixture; CRuntimePackage::Request input; QString error;
        QVERIFY2(artworkRequest(fixture, input, &error), qPrintable(error));
        CRuntimePackage::Prepared expected, output; QVERIFY2(CRuntimePackage::Prepare(input, expected, nullptr, &error), qPrintable(error));
        input.maximumTotalBytes = expected.totalBytes; input.maximumFiles = expected.entries.size();
        QVERIFY2(CRuntimePackage::Prepare(input, output, nullptr, &error), qPrintable(error)); QCOMPARE(output.totalBytes, expected.totalBytes);
        for (int cancelAt : {1, 4}) {
            CExportControl control; int reached = 0;
            control.SetHandler([&](const CExportControl::Progress& progress, QString*) {
                if (progress.detail == QStringLiteral("收集作者来源快照") && ++reached == cancelAt) control.Cancel(); return true;
            });
            QVERIFY(!CRuntimePackage::Prepare(input, output, &control, &error)); QCOMPARE(reached, cancelAt); QVERIFY(control.WasCanceled());
            QCOMPARE(output.totalBytes, expected.totalBytes); QCOMPARE(output.model, expected.model); QCOMPARE(output.entries.size(), expected.entries.size());
        }
    }

    void attachmentAnchorsUseNormalizedRuntimeMoc()
    {
        Fixture fixture; CRuntimePackage::Request input; QString error;
        QVERIFY2(artworkRequest(fixture, input, &error), qPrintable(error));
        auto reader = CReader::FromBytes(input.mocBytes); const CSectionTable table(reader);
        input.mocBytes = DeformerOrderFixture::permute(input.mocBytes, DeformerOrderFixture::reverseOrder(table));
        QVERIFY(!input.mocBytes.isEmpty()); QByteArray normalized;
        QVERIFY2(CMocDeformerOrder::Normalize(input.mocBytes, normalized, &error), qPrintable(error)); QVERIFY(normalized != input.mocBytes);
        CRuntimePackage::Prepared output; QVERIFY2(CRuntimePackage::Prepare(input, output, nullptr, &error), qPrintable(error));
        const auto files = output.model["FileReferences"].toObject(); QCOMPARE(entry(output, files["Moc"].toString()), normalized);
        const auto index = QJsonDocument::fromJson(entry(output, CArtworkPackage::ManifestFile(output.modelFile))).object();
        QCOMPARE(index["moc"].toObject()["sha256"].toString(), QString::fromLatin1(digest(normalized).toHex()));
        QCOMPARE(index["model"].toObject()["sha256"].toString(), QString::fromLatin1(digest(entry(output, output.modelFile)).toHex()));
        QTemporaryDir target; QVERIFY2(publish(output, target.filePath("model"), &error), qPrintable(error));
        CModelDocument document; CArtworkPackage::Loaded loaded;
        QVERIFY2(loadArtworkPackage(target.filePath("model/") + output.modelFile, document, loaded, &error), qPrintable(error));
        CModelMaterialState materials; QVERIFY(document.CaptureDrawableMaterials(materials, &error)); QCOMPARE(loaded.state.materials, materials);
    }

    void borrowedRequestIsOwnedBeforeAnyProgressCallback_data()
    {
        QTest::addColumn<bool>("withArtwork"); QTest::newRow("legacy-texture-overrides") << false; QTest::newRow("authoring-workspace") << true;
    }
    void borrowedRequestIsOwnedBeforeAnyProgressCallback()
    {
        QFETCH(bool, withArtwork); Fixture fixture; CRuntimePackage::Request input; QString error;
        QVERIFY2(artworkRequest(fixture, input, &error), qPrintable(error));
        const auto expression = json({{"Type", "Live2D Expression"}, {"Parameters", QJsonArray{QJsonObject{{"Id", "ParamAngleX"}, {"Value", .3}, {"Blend", "Add"}}}}});
        QVERIFY(write(fixture.directory.filePath("expression.exp3.json"), expression));
        auto files = fixture.root["FileReferences"].toObject(); files["Expressions"] = QJsonArray{QJsonObject{{"Name", "expression"}, {"File", "expression.exp3.json"}}}; fixture.root["FileReferences"] = files;
        fixture.root["Groups"] = QJsonArray{QJsonObject{{"Name", "EyeBlink"}, {"Target", "Parameter"}, {"Ids", QJsonArray{"ParamAngleX"}}}}; QVERIFY(fixture.save());
        input.loadedModelSettingsBytes = read(fixture.path); input.modelSettingsDigest = digest(input.loadedModelSettingsBytes);
        CParameterResourceEdits::Prepared references;
        QVERIFY2(CParameterResourceEdits::Prepare(fixture.path, {}, {"ParamAngleX"}, references, &error), qPrintable(error)); input.references = references.state;
        input.references.breath = {"ParamAngleX"}; input.references.mouseFollow = {"ParamAngleY"};
        input.expectedSourceDigests[fixture.directory.filePath("expression.exp3.json")] = digest(expression);
        input.expectedSourceDigests[input.textureSources->first().path] = input.textureSources->first().digest;
        input.parameterMetadata["ParamAngleX"].displayName = "Owned display name";
        input.parameterOrganization.configured = true; input.parameterOrganization.nodes.append({CParameterOrganization::Node::Folder, "folder", {}, "Owned folder", "#123456"});
        for (const auto& parameter : fixture.document.Parameters()) input.parameterOrganization.nodes.append({CParameterOrganization::Node::Parameter, parameter.id, "folder", {}, {}});
        input.parameterOrganization.pairs = {{"ParamAngleX", "ParamAngleY", true, true}};
        CGeometryObjectPairs::Pair pair; pair.id = "review-pair"; pair.requiresReview = true;
        pair.first.object = {CGeometryObjectPairs::Type::ArtMesh, "ArmR"}; pair.first.vertexCount = 3; pair.first.triangles = {0, 1, 2};
        pair.first.keyformCount = 1; pair.first.axes = {{0, "ParamAngleX", {0.f}, 0}};
        pair.second = pair.first; pair.second.object.id = "ArmL"; pair.firstVertexToSecondVertex = {0, 1, 2}; pair.axes = {{0, 0, {0}}};
        input.geometryObjectPairs = {input.sourceMocDigest, {pair}};
        input.physics = json({{"Version", 3}, {"Meta", QJsonObject{}}, {"PhysicsSettings", QJsonArray{}}});
        input.pose = json({{"Version", 3}, {"Groups", QJsonArray{}}});
        input.userData = json({{"Version", 3}, {"UserData", QJsonArray{}}});
        input.registeredUserDataPath = fixture.directory.filePath("unused.userdata3.json");
        input.timeline = read(QString(TEST_RESOURCES) + "/Mark/motions/mark_m01.motion3.json"); input.timelineGroup = "Owned group";
        input.timelineSoundPath = fixture.directory.filePath("sound.wav"); QVERIFY(write(input.timelineSoundPath, "RIFFowned"));
        if (!withArtwork) {
            input.artwork.reset(); input.textureWorkspace.reset(); input.mocBytes = fixture.document.SourceBytes();
            input.textureState = {{0, png(qRgba(4, 7, 21, 199))}};
        }
        CRuntimePackage::Prepared expected, output;
        QVERIFY2(CRuntimePackage::Prepare(input, expected, nullptr, &error), qPrintable(error));
        Borrowed storage; borrowRequest(input, storage); CExportControl control; bool called = false;
        auto& borrowedPair = input.geometryObjectPairs.pairs[0];
        auto* vertices = borrowedPair.firstVertexToSecondVertex.data(); auto* triangles = borrowedPair.first.triangles.data();
        auto* keys = borrowedPair.first.axes[0].keys.data(); auto* mapping = borrowedPair.axes[0].firstKeyToSecondKey.data();
        control.SetHandler([&](const CExportControl::Progress&, QString*) {
            if (!called) { called = true; storage.corrupt(); vertices[0] = -1; triangles[0] = 99; keys[0] = 123; mapping[0] = -1; input = {}; } return true;
        });
        QVERIFY2(CRuntimePackage::Prepare(input, output, &control, &error), qPrintable(error)); QVERIFY(called);
        QCOMPARE(output.model, expected.model); QCOMPARE(output.totalBytes, expected.totalBytes); QCOMPARE(output.entries.size(), expected.entries.size());
        QCOMPARE(output.notes, expected.notes); QCOMPARE(output.protectedPaths, expected.protectedPaths);
        for (const auto& file : expected.entries) QCOMPARE(entry(output, file.relativePath), file.bytes);
    }

    void artworkHistoryDoesNotWeakenCurrentTextureChecks()
    {
        Fixture fixture; CRuntimePackage::Request input; QString error;
        QVERIFY2(artworkRequest(fixture, input, &error), qPrintable(error));
        const auto path = input.textureSources->first().path;
        input.artwork->revisions["old"].path = path;
        input.expectedSourceDigests[path] = input.artwork->revisions["old"].sourceDigest;
        CRuntimePackage::Prepared output;
        QVERIFY(!CRuntimePackage::Prepare(input, output, nullptr, &error)); QVERIFY(!error.isEmpty()); QVERIFY(output.entries.isEmpty());
        input.expectedSourceDigests[path] = input.textureSources->first().digest;
        QVERIFY2(CRuntimePackage::Prepare(input, output, nullptr, &error), qPrintable(error));
    }

    void oversizedBorrowedInputIsRejectedBeforeProgress()
    {
        Fixture fixture; auto input = request(fixture.path); Borrowed storage; input.mocBytes = storage.borrow(input.mocBytes);
        input.maximumFileBytes = input.mocBytes.size() - 1; CExportControl control; int callbacks = 0;
        control.SetHandler([&](const CExportControl::Progress&, QString*) { ++callbacks; return true; });
        CRuntimePackage::Prepared output; QString error;
        QVERIFY(!CRuntimePackage::Prepare(input, output, &control, &error)); QCOMPARE(callbacks, 0); QVERIFY(output.entries.isEmpty());
    }

    void generatedAtlasPackageReopensWithSparseSlots()
    {
        Fixture fixture; const auto originalPng = png(qRgba(21, 31, 41, 51), {3, 2});
        const auto path = fixture.directory.filePath("original.png"); QVERIFY(write(path, originalPng));
        auto files = fixture.root["FileReferences"].toObject(); files["Textures"] = QJsonArray{"original.png", "", "", "original.png"};
        fixture.root["FileReferences"] = files; QVERIFY(fixture.save());
        CTextureAssets::Source source; QString error; QVERIFY(CTextureAssets::MakeSource(path, originalPng, source, &error));
        auto input = request(fixture.path); input.textureSources = CTextureAssets::Catalog{{0, source}, {3, source}};
        CTextureWorkspace::State workspace; QVERIFY(CTextureWorkspace::Initialize(*input.textureSources, {}, workspace, &error));
        workspace.atlases[0].size = {6, 5}; workspace.atlases[0].png = png(qRgba(9, 41, 12, 63), {6, 5});
        workspace.atlases.insert(4, {"generated-atlas", "新增图集", -1, {7, 9}, png(qRgba(29, 11, 4, 0), {7, 9})});
        input.textureWorkspace = workspace;
        CModelDocument changed; QVERIFY(changed.LoadBytes(input.mocBytes, "package-source", &error));
        CModelMaterialState materials; QVERIFY(changed.CaptureDrawableMaterials(materials, &error)); QVERIFY(!materials.isEmpty());
        const auto id = materials.first().id; auto material = materials.first(); material.textureIndex = 4;
        QVERIFY(changed.SetDrawableMaterials({material}, &error)); QVERIFY(changed.BuildRuntimeMoc(input.mocBytes, &error));
        CRuntimePackage::Prepared prepared; QVERIFY2(CRuntimePackage::Prepare(input, prepared, nullptr, &error), qPrintable(error));
        const auto references = prepared.model["FileReferences"].toObject(); const auto textures = references["Textures"].toArray();
        QCOMPARE(textures.size(), 5); QCOMPARE(textures[1].toString(), QString()); QCOMPARE(textures[2].toString(), QString());
        QCOMPARE(entry(prepared, textures[0].toString()), workspace.atlases[0].png);
        QCOMPARE(entry(prepared, textures[3].toString()), originalPng); QCOMPARE(entry(prepared, textures[4].toString()), workspace.atlases[4].png);
        QVERIFY(prepared.protectedPaths.contains(path)); QCOMPARE(read(path), originalPng);
        QTemporaryDir output; QVERIFY(output.isValid());
        for (const auto& value : prepared.entries) {
            const auto destination = output.filePath(value.relativePath); QVERIFY(QDir().mkpath(QFileInfo(destination).absolutePath()));
            QVERIFY(write(destination, value.bytes));
        }
        CModelDocument reopened; QVERIFY(reopened.Load(output.filePath(references["Moc"].toString()), &error));
        CModelDrawableMaterial actual; QVERIFY(reopened.CaptureDrawableMaterial(id, actual, &error)); QCOMPARE(actual, material);
        CTextureAssets::Catalog newSources;
        for (int slot = 0; slot < textures.size(); ++slot) if (!textures[slot].toString().isEmpty()) {
            const auto filename = output.filePath(textures[slot].toString()); CTextureAssets::Source exported;
            QVERIFY(CTextureAssets::MakeSource(filename, read(filename), exported, &error)); newSources.insert(slot, exported);
        }
        QVERIFY(reopened.InitializeTextureWorkspace(newSources, {}, &error)); QCOMPARE(reopened.TextureSources()[4].size, QSize(7, 9));
        QVERIFY(reopened.TextureWorkspace().atlases[4].png.isEmpty()); QCOMPARE(reopened.TextureWorkspace().atlases[4].sourceSlot, 4);
        input.textureState = {{0, png(qRgb(2, 4, 8), {3, 2})}}; const auto held = prepared.model;
        QVERIFY(!CRuntimePackage::Prepare(input, prepared, nullptr, &error)); QCOMPARE(prepared.model, held);
    }
    void drawableTextureSlotsMustResolve_data()
    {
        QTest::addColumn<int>("kind");
        QTest::newRow("past-declared-slots") << 0;
        QTest::newRow("empty-declared-slot") << 1;
        QTest::newRow("textures-field-absent") << 2;
        QTest::newRow("catalog-empty-slot") << 3;
    }
    void drawableTextureSlotsMustResolve()
    {
        QFETCH(int, kind);
        Fixture fixture;
        auto files = fixture.root["FileReferences"].toObject();
        const auto original = files["Textures"].toArray().first();
        if (kind == 1 || kind == 3) files["Textures"] = QJsonArray{original, ""};
        if (kind == 2) files.remove("Textures");
        fixture.root["FileReferences"] = files;
        QVERIFY(fixture.save());
        auto input = request(fixture.path);
        if (kind != 2) {
            auto reader = CReader::FromBytes(input.mocBytes); CSectionTable table(reader);
            QVERIFY(table.IsValid()); QVERIFY(!table.art_meshes.isEmpty());
            qToLittleEndian<qint32>(1, input.mocBytes.data() + table.sectionOffsetTable->art_meshes->texture_nos);
        }
        if (kind == 3) {
            CTextureAssets::Source source; QString detail;
            QVERIFY(CTextureAssets::MakeSource(original.toString(), read(original.toString()), source, &detail));
            input.textureSources = CTextureAssets::Catalog{{0, source}};
        }
        CRuntimePackage::Prepared output; output.model = {{"kept", true}};
        QString error;
        QVERIFY(!CRuntimePackage::Prepare(input, output, nullptr, &error));
        QVERIFY(error.contains(QStringLiteral("纹理槽")));
        QCOMPARE(output.model, (QJsonObject{{"kept", true}}));
        QVERIFY(output.entries.isEmpty());
    }

    void textureSnapshotsSurviveDiskChangesAndKeepOriginalIdentity_data()
    {
        QTest::addColumn<bool>("remove"); QTest::newRow("changed") << false; QTest::newRow("deleted") << true;
    }
    void textureSnapshotsSurviveDiskChangesAndKeepOriginalIdentity()
    {
        QFETCH(bool, remove); Fixture fixture;
        const auto original = png(qRgba(91, 20, 31, 74)), replacement = png(qRgba(2, 194, 83, 49));
        const auto path = fixture.directory.filePath("original.png"); QVERIFY(write(path, original));
        auto files = fixture.root["FileReferences"].toObject(); files["Textures"] = QJsonArray{"original.png"};
        fixture.root["FileReferences"] = files; QVERIFY(fixture.save());
        CTextureAssets::Source source; QString error; QVERIFY(CTextureAssets::MakeSource(path, original, source, &error));
        auto input = request(fixture.path); input.textureSources = CTextureAssets::Catalog{{0, source}};
        input.textureState = {{0, replacement}}; input.expectedSourceDigests[path] = source.digest;
        if (remove) QVERIFY(QFile::remove(path)); else QVERIFY(write(path, "external non-PNG content"));
        CRuntimePackage::Prepared prepared;
        QVERIFY2(CRuntimePackage::Prepare(input, prepared, nullptr, &error), qPrintable(error));
        const auto output = prepared.model["FileReferences"].toObject()["Textures"].toArray().first().toString();
        QCOMPARE(entry(prepared, output), replacement); QVERIFY(output.endsWith(".png"));
        QVERIFY(prepared.protectedPaths.contains(path)); int matched = 0;
        for (const auto& captured : prepared.sources) if (captured.path == path) {
            ++matched; QCOMPARE(captured.digest, digest(original)); QCOMPARE(captured.size, original.size());
        }
        QCOMPARE(matched, 1); QCOMPARE(input.textureSources->value(0), source);
        QCOMPARE(remove ? QByteArray() : read(path), remove ? QByteArray() : QByteArray("external non-PNG content"));
        CDirectoryExport::Options options; options.protectedPaths = prepared.protectedPaths;
        CDirectoryExport::Result published;
        QVERIFY(!CDirectoryExport::Publish(fixture.directory.path(), prepared.entries, options, published, nullptr, &error));
    }

    void repeatedSourcePathsAllowIndependentSlotsAndEmptyHoles()
    {
        Fixture fixture; const auto original = png(qRgba(12, 61, 42, 18));
        const auto first = png(qRgba(118, 20, 7, 27)), last = png(qRgba(14, 19, 229, 54));
        const auto path = fixture.directory.filePath("shared.png");
        auto files = fixture.root["FileReferences"].toObject(); files["Textures"] = QJsonArray{"shared.png", "", "./shared.png"};
        fixture.root["FileReferences"] = files; QVERIFY(fixture.save());
        CTextureAssets::Source source; QString error; QVERIFY(CTextureAssets::MakeSource(path, original, source, &error));
        auto input = request(fixture.path); input.textureSources = CTextureAssets::Catalog{{0, source}, {2, source}};
        input.textureState = {{0, first}, {2, last}}; input.expectedSourceDigests[path] = source.digest;
        CRuntimePackage::Prepared prepared; QVERIFY2(CRuntimePackage::Prepare(input, prepared, nullptr, &error), qPrintable(error));
        const auto textures = prepared.model["FileReferences"].toObject()["Textures"].toArray(); QCOMPARE(textures.size(), 3);
        QCOMPARE(textures[1].toString(), QString()); QVERIFY(textures[0] != textures[2]);
        QCOMPARE(entry(prepared, textures[0].toString()), first); QCOMPARE(entry(prepared, textures[2].toString()), last);
        int sources = 0; for (const auto& value : prepared.sources) sources += value.path == path;
        QCOMPARE(sources, 1); QVERIFY(!QFileInfo::exists(path));
    }

    void invalidTextureSnapshotsPreservePreparedOutput_data()
    {
        QTest::addColumn<int>("kind");
        const char* rows[]{"missing-slot", "extra-slot", "empty-slot", "different-path", "wrong-digest", "wrong-size",
            "invalid-source", "different-dimensions", "unknown-state-slot", "state-without-catalog", "effective-digest", "same-path-different-source"};
        for (int i = 0; i < 12; ++i) QTest::newRow(rows[i]) << i;
    }
    void invalidTextureSnapshotsPreservePreparedOutput()
    {
        QFETCH(int, kind); Fixture fixture; const auto original = png(qRgba(12, 91, 42, 98)), replacement = png(qRgba(129, 71, 24, 57));
        const auto path = fixture.directory.filePath("original.png");
        auto files = fixture.root["FileReferences"].toObject(); files["Textures"] = QJsonArray{"original.png", ""};
        if (kind == 11) files["Textures"] = QJsonArray{"original.png", "original.png"};
        fixture.root["FileReferences"] = files; QVERIFY(fixture.save());
        CTextureAssets::Source source; QString error; QVERIFY(CTextureAssets::MakeSource(path, original, source, &error));
        auto input = request(fixture.path); input.textureSources = CTextureAssets::Catalog{{0, source}};
        input.textureState = {{0, replacement}};
        if (kind == 0) input.textureSources->clear();
        if (kind == 1) (*input.textureSources)[2] = source;
        if (kind == 2) (*input.textureSources)[1] = source;
        if (kind == 3) (*input.textureSources)[0].path = fixture.directory.filePath("other.png");
        if (kind == 4) (*input.textureSources)[0].digest = QByteArray(32, 'x');
        if (kind == 5) (*input.textureSources)[0].size = QSize(3, 4);
        if (kind == 6) { (*input.textureSources)[0].png = "invalid"; (*input.textureSources)[0].digest = digest("invalid"); }
        if (kind == 7) input.textureState[0] = png(qRgba(1, 2, 3, 4), {5, 4});
        if (kind == 8) input.textureState[3] = replacement;
        if (kind == 9) input.textureSources.reset();
        if (kind == 10) input.expectedSourceDigests[path] = digest(replacement);
        if (kind == 11) { CTextureAssets::Source other; QVERIFY(CTextureAssets::MakeSource(path, replacement, other, &error)); (*input.textureSources)[1] = other; }
        CRuntimePackage::Prepared prepared; prepared.model = {{"preserved", true}}; prepared.entries.append(CDirectoryExport::Entry{"keep", "original"});
        prepared.protectedPaths = {"protected"}; prepared.totalBytes = 8;
        QVERIFY(!CRuntimePackage::Prepare(input, prepared, nullptr, &error)); QVERIFY(!error.isEmpty());
        QCOMPARE(prepared.model, QJsonObject({{"preserved", true}})); QCOMPARE(prepared.entries.size(), 1);
        QCOMPARE(prepared.entries[0].relativePath, QString("keep")); QCOMPARE(prepared.entries[0].bytes, QByteArray("original"));
        QCOMPARE(prepared.protectedPaths, QStringList{"protected"}); QCOMPARE(prepared.totalBytes, 8);
    }

    void textureSnapshotCallbacksCaptureRequestAndCancelAtomically()
    {
        Fixture fixture; const auto original = png(qRgba(91, 20, 31, 74)), replacement = png(qRgba(2, 194, 83, 49));
        const auto path = fixture.directory.filePath("original.png");
        auto files = fixture.root["FileReferences"].toObject(); files["Textures"] = QJsonArray{"original.png"};
        fixture.root["FileReferences"] = files; QVERIFY(fixture.save());
        CTextureAssets::Source source; QString error; QVERIFY(CTextureAssets::MakeSource(path, original, source, &error));
        auto input = request(fixture.path); input.textureSources = CTextureAssets::Catalog{{0, source}}; input.textureState = {{0, replacement}};
        const auto saved = input; CExportControl control; bool called = false;
        QVERIFY(control.SetHandler([&](const CExportControl::Progress&, QString*) {
            if (!called) { called = true; input = {}; } return true;
        }));
        CRuntimePackage::Prepared prepared; QVERIFY2(CRuntimePackage::Prepare(input, prepared, &control, &error), qPrintable(error)); QVERIFY(called);
        const auto output = prepared.model["FileReferences"].toObject()["Textures"].toArray().first().toString(); QCOMPARE(entry(prepared, output), replacement);
        const auto accepted = prepared.model; const auto total = prepared.totalBytes;
        CExportControl canceled; bool reached = false;
        QVERIFY(canceled.SetHandler([&](const CExportControl::Progress& progress, QString*) {
            if (progress.detail.startsWith(QStringLiteral("收集纹理快照"))) { reached = true; canceled.Cancel(); }
            return true;
        }));
        QVERIFY(!CRuntimePackage::Prepare(saved, prepared, &canceled, &error)); QVERIFY(reached); QVERIFY(canceled.WasCanceled());
        QCOMPARE(prepared.model, accepted); QCOMPARE(prepared.totalBytes, total); QCOMPARE(entry(prepared, output), replacement);
    }

    void textureSnapshotBudgetsPreservePreparedOutput_data()
    {
        QTest::addColumn<int>("kind"); QTest::newRow("source-file") << 0; QTest::newRow("effective-file") << 1;
        QTest::newRow("total") << 2; QTest::newRow("file-count") << 3;
    }
    void textureSnapshotBudgetsPreservePreparedOutput()
    {
        QFETCH(int, kind); Fixture fixture; auto original = png(qRgba(91, 20, 31, 74));
        auto replacement = png(qRgba(2, 194, 83, 49)); const auto path = fixture.directory.filePath("original.png");
        auto files = fixture.root["FileReferences"].toObject(); files["Textures"] = QJsonArray{"original.png"};
        fixture.root["FileReferences"] = files; QVERIFY(fixture.save());
        auto input = request(fixture.path); const auto limit = input.mocBytes.size() + read(fixture.path).size();
        if (kind == 0) original.append(QByteArray(limit, 'x'));
        if (kind == 1) replacement.append(QByteArray(limit, 'y'));
        CTextureAssets::Source source; QString error; QVERIFY(CTextureAssets::MakeSource(path, original, source, &error));
        input.textureSources = CTextureAssets::Catalog{{0, source}}; input.textureState = {{0, replacement}};
        if (kind < 2) input.maximumFileBytes = limit;
        if (kind == 2) input.maximumTotalBytes = limit;
        if (kind == 3) input.maximumFiles = 2;
        CRuntimePackage::Prepared prepared; prepared.model = {{"preserved", true}};
        QVERIFY(!CRuntimePackage::Prepare(input, prepared, nullptr, &error)); QVERIFY(!error.isEmpty());
        QCOMPARE(prepared.model, QJsonObject({{"preserved", true}})); QVERIFY(prepared.entries.isEmpty());
    }

    void textureSnapshotsKeepOtherSourceChecks()
    {
        Fixture fixture; const auto original = png(qRgba(91, 20, 31, 74)); const auto path = fixture.directory.filePath("original.png");
        auto files = fixture.root["FileReferences"].toObject(); files["Textures"] = QJsonArray{"original.png"};
        fixture.root["FileReferences"] = files; QVERIFY(fixture.save());
        const auto names = json({{"Version", 3}, {"Parameters", QJsonArray{}}}); fixture.resource("DisplayInfo", "names.json", names);
        CTextureAssets::Source source; QString error; QVERIFY(CTextureAssets::MakeSource(path, original, source, &error));
        auto input = request(fixture.path); input.textureSources = CTextureAssets::Catalog{{0, source}};
        input.expectedSourceDigests[fixture.directory.filePath("names.json")] = digest(names);
        QVERIFY(write(fixture.directory.filePath("names.json"), json({{"Version", 3}, {"Custom", true}})));
        CRuntimePackage::Prepared prepared; QVERIFY(!CRuntimePackage::Prepare(input, prepared, nullptr, &error));
        QVERIFY(error.contains("names.json")); QVERIFY(prepared.entries.isEmpty());
    }

    void newlySavedUserDataRegistrationKeepsOtherSourceChecks_data()
    {
        QTest::addColumn<int>("index");
        QTest::newRow("registered") << 0; QTest::newRow("unrelated-change") << 1;
        QTest::newRow("missing-loaded-digest") << 2; QTest::newRow("wrong-loaded-digest") << 3;
        QTest::newRow("wrong-registration-path") << 4;
    }
    void newlySavedUserDataRegistrationKeepsOtherSourceChecks()
    {
        QFETCH(int, index); Fixture fixture; auto input = request(fixture.path);
        input.loadedModelSettingsBytes = read(fixture.path);
        const auto data = json({{"Version", 3}, {"UserData", QJsonArray{QJsonObject{{"Target", "ArtMesh"}, {"Id", fixture.document.Drawables().first().id}, {"Value", "saved"}}}}});
        fixture.resource("UserData", "created.userdata3.json", data);
        input.registeredUserDataPath = fixture.directory.filePath("created.userdata3.json");
        input.expectedSourceDigests[input.registeredUserDataPath] = digest(data); input.userData = data;
        if (index == 1) { fixture.root["Custom"] = "external change"; QVERIFY(fixture.save()); }
        if (index == 2) input.expectedSourceDigests.clear();
        if (index == 3) input.expectedSourceDigests[input.registeredUserDataPath] = QByteArray(32, 'x');
        if (index == 4) input.registeredUserDataPath = fixture.directory.filePath("other.json");
        CRuntimePackage::Prepared prepared; QString error;
        QCOMPARE(CRuntimePackage::Prepare(input, prepared, nullptr, &error), index == 0);
        if (index != 0) { QVERIFY(!error.isEmpty()); QVERIFY(prepared.entries.isEmpty()); }
        else QCOMPARE(entry(prepared, prepared.model["FileReferences"].toObject()["UserData"].toString()), data);
    }
    void updatedLoadedManifestCanPackageOldReferenceStateWithoutChangingHistory()
    {
        Fixture fixture; const QString id = "ParamAngleX";
        fixture.root["Groups"] = QJsonArray{QJsonObject{{"Name", "EyeBlink"}, {"Target", "Parameter"}, {"Ids", QJsonArray{id}}}};
        QVERIFY(fixture.save());
        CParameterResourceEdits::Prepared references; QString error;
        QVERIFY(CParameterResourceEdits::Prepare(fixture.path, {}, {id}, references, &error));
        const auto oldSource = references.state.modelSource;
        const auto data = json({{"Version", 3}, {"UserData", QJsonArray{QJsonObject{{"Target", "ArtMesh"},
            {"Id", fixture.document.Drawables().first().id}, {"Value", "saved"}}}}});
        fixture.resource("UserData", "created.userdata3.json", data);
        auto input = request(fixture.path); input.loadedModelSettingsBytes = read(fixture.path); input.references = references.state;
        CMocParameterStructureBuilder::Result deleted;
        QVERIFY(CMocParameterStructureBuilder::Delete(input.mocBytes, {id, 0}, deleted, &error)); input.mocBytes = deleted.bytes;
        CRuntimePackage::Prepared prepared;
        QVERIFY2(CRuntimePackage::Prepare(input, prepared, nullptr, &error), qPrintable(error));
        QCOMPARE(input.references.modelSource, oldSource);
        QCOMPARE(prepared.model["Groups"].toArray().first().toObject()["Ids"].toArray(), QJsonArray{});
        QCOMPARE(entry(prepared, prepared.model["FileReferences"].toObject()["UserData"].toString()), data);
        const auto acceptedModel = prepared.model;
        QMap<QString, QByteArray> acceptedEntries;
        for (const auto& file : prepared.entries) acceptedEntries.insert(file.relativePath, file.bytes);
        fixture.root["Custom"] = "external change"; QVERIFY(fixture.save());
        input.loadedModelSettingsBytes = read(fixture.path); input.modelSettingsDigest = digest(input.loadedModelSettingsBytes);
        QVERIFY(!CRuntimePackage::Prepare(input, prepared, nullptr, &error));
        QVERIFY(error.contains(QStringLiteral("引用状态"))); QCOMPARE(prepared.model, acceptedModel); QCOMPARE(prepared.entries.size(), acceptedEntries.size());
        for (const auto& file : prepared.entries) QCOMPARE(file.bytes, acceptedEntries.value(file.relativePath));
    }

    void optionalEmptyResourcesRemainPortable_data()
    {
        QTest::addColumn<int>("index");
        QTest::newRow("omitted") << 0; QTest::newRow("empty-list") << 1; QTest::newRow("empty-slot") << 2;
    }
    void optionalEmptyResourcesRemainPortable()
    {
        QFETCH(int, index); Fixture fixture;
        auto files = fixture.root["FileReferences"].toObject();
        auto moc = read(files["Moc"].toString());
        auto reader = CReader::FromBytes(moc); CSectionTable table(reader); QVERIFY(table.IsValid());
        for (int mesh = 0; mesh < table.art_meshes.size(); ++mesh) {
            qToLittleEndian<qint32>(0, moc.data() + table.sectionOffsetTable->art_meshes->vertex_counts + mesh * 4);
            qToLittleEndian<qint32>(0, moc.data() + table.sectionOffsetTable->art_meshes->position_index_sources_counts + mesh * 4);
        }
        const auto emptyMoc = fixture.directory.filePath("empty.moc3"); QVERIFY(write(emptyMoc, moc)); files["Moc"] = emptyMoc;
        if (index == 0) files.remove("Textures");
        else files["Textures"] = index == 1 ? QJsonArray{} : QJsonArray{""};
        files["Expressions"] = QJsonArray{QJsonObject{{"Name", "empty"}, {"File", ""}}};
        files["Motions"] = QJsonObject{{"Empty", QJsonArray{QJsonObject{{"File", ""}, {"Sound", ""}}}}};
        fixture.root["FileReferences"] = files; QVERIFY(fixture.save());
        CRuntimePackage::Prepared prepared; QString error;
        QVERIFY2(CRuntimePackage::Prepare(request(fixture.path), prepared, nullptr, &error), qPrintable(error));
        const auto result = prepared.model["FileReferences"].toObject();
        QCOMPARE(result.value("Textures"), files.value("Textures")); QCOMPARE(result["Expressions"], files["Expressions"]); QCOMPARE(result["Motions"], files["Motions"]);
    }
    void deletedParametersAreRemovedFromLinkedResources()
    {
        Fixture fixture; const QString id = "ParamAngleX";
        fixture.resource("DisplayInfo", "names.json", json({{"Version", 3}, {"Parameters", QJsonArray{QJsonObject{{"Id", id}, {"Name", "removed"}}}},
            {"Parts", QJsonArray{QJsonObject{{"Id", id}, {"Name", "part retained"}}}}, {"CombinedParameters", QJsonArray{QJsonArray{id, "ParamAngleY"}}}}));
        const auto expression = json({{"Type", "Live2D Expression"}, {"Parameters", QJsonArray{QJsonObject{{"Id", id}, {"Value", 1}, {"Blend", "Add"}}, QJsonObject{{"Id", "HostVirtual"}, {"Value", 2}, {"Blend", "Add"}}}}});
        QVERIFY(write(fixture.directory.filePath("face.exp3.json"), expression));
        QVERIFY(write(fixture.directory.filePath("move.motion3.json"), read(QString(TEST_RESOURCES) + "/Mark/motions/mark_m01.motion3.json")));
        auto files = fixture.root["FileReferences"].toObject();
        files["Expressions"] = QJsonArray{QJsonObject{{"Name", "face"}, {"File", "face.exp3.json"}}};
        files["Motions"] = QJsonObject{{"Idle", QJsonArray{QJsonObject{{"File", "move.motion3.json"}}}}};
        fixture.root["FileReferences"] = files; fixture.root["Groups"] = QJsonArray{QJsonObject{{"Target", "Parameter"}, {"Name", "LipSync"}, {"Ids", QJsonArray{id}}}}; QVERIFY(fixture.save());
        auto input = request(fixture.path); CMocParameterStructureBuilder::Result deleted; QString error;
        QVERIFY2(CMocParameterStructureBuilder::Delete(input.mocBytes, {id, 0}, deleted, &error), qPrintable(error)); input.mocBytes = deleted.bytes;
        CParameterResourceEdits::Prepared references;
        QVERIFY2(CParameterResourceEdits::Prepare(fixture.path, {}, {id}, references, &error), qPrintable(error)); input.references = references.state;
        CRuntimePackage::Prepared prepared;
        QVERIFY2(CRuntimePackage::Prepare(input, prepared, nullptr, &error), qPrintable(error));
        const auto result = prepared.model["FileReferences"].toObject();
        const auto names = QJsonDocument::fromJson(entry(prepared, result["DisplayInfo"].toString())).object();
        QVERIFY(names["Parameters"].toArray().isEmpty()); QVERIFY(names["CombinedParameters"].toArray().isEmpty()); QCOMPARE(names["Parts"].toArray().size(), 1);
        const auto face = QJsonDocument::fromJson(entry(prepared, result["Expressions"].toArray().first().toObject()["File"].toString())).object();
        QCOMPARE(face["Parameters"].toArray().size(), 1); QCOMPARE(face["Parameters"].toArray().first().toObject()["Id"].toString(), "HostVirtual");
        const auto motion = QJsonDocument::fromJson(entry(prepared, result["Motions"].toObject()["Idle"].toArray().first().toObject()["File"].toString())).object();
        for (const auto& curve : motion["Curves"].toArray()) QVERIFY(curve.toObject()["Target"] != "Parameter" || curve.toObject()["Id"] != id);
        QCOMPARE(read(fixture.directory.filePath("face.exp3.json")), expression);
    }
    void motionSyncChecksModelParameterNamespace_data()
    {
        QTest::addColumn<int>("index");
        QTest::newRow("cubism-parameter") << 0; QTest::newRow("mapping-target") << 1; QTest::newRow("audio-namespace") << 2;
    }
    void motionSyncChecksModelParameterNamespace()
    {
        QFETCH(int, index); Fixture fixture;
        QJsonObject setting{{"Id", "Setting1"}, {"CubismParameters", QJsonArray{}}, {"Mappings", QJsonArray{}}, {"AudioParameters", QJsonArray{QJsonObject{{"Id", "ParamAngleX"}}}}};
        if (index == 0) setting["CubismParameters"] = QJsonArray{QJsonObject{{"Id", "ParamAngleX"}}};
        if (index == 1) setting["Mappings"] = QJsonArray{QJsonObject{{"Id", "A"}, {"Targets", QJsonArray{QJsonObject{{"Id", "ParamAngleX"}, {"Value", 1}}}}}};
        if (index == 2) setting["Mappings"] = QJsonArray{QJsonObject{{"Id", "ParamAngleX"}, {"Targets", QJsonArray{QJsonObject{{"Id", "ParamAngleY"}, {"Value", 1}}}}}};
        fixture.resource("MotionSync", "sync.json", json({{"Version", 1}, {"Settings", QJsonArray{setting}}}));
        auto input = request(fixture.path); CMocParameterStructureBuilder::Result deleted; QString error;
        QVERIFY2(CMocParameterStructureBuilder::Delete(input.mocBytes, {"ParamAngleX", 0}, deleted, &error), qPrintable(error)); input.mocBytes = deleted.bytes;
        CRuntimePackage::Prepared prepared;
        QCOMPARE(CRuntimePackage::Prepare(input, prepared, nullptr, &error), index == 2);
        if (index != 2) { QVERIFY(error.contains("MotionSync")); QVERIFY(prepared.entries.isEmpty()); }
    }
    void originalModelsCanMoveWithoutSourcePaths_data()
    {
        QTest::addColumn<QString>("name");
        for (const auto& name : {"Mark", "Natori", "Rice", "Wanko"}) QTest::newRow(name) << QString::fromLatin1(name);
    }
    void originalModelsCanMoveWithoutSourcePaths()
    {
        QFETCH(QString, name); const auto source = QString(TEST_RESOURCES) + "/" + name + "/" + name + ".model3.json";
        auto input = request(source); CRuntimePackage::Prepared prepared; QString error;
        QVERIFY2(CRuntimePackage::Prepare(input, prepared, nullptr, &error), qPrintable(error));
        QVERIFY(prepared.entries.size() > 3); QTemporaryDir temporary;
        CDirectoryExport::Options options; options.protectedPaths = prepared.protectedPaths;
        CDirectoryExport::Result published;
        QVERIFY2(CDirectoryExport::Publish(temporary.filePath("package"), prepared.entries, options, published, nullptr, &error), qPrintable(error));
        QVERIFY(published.committed); QVERIFY(QDir().rename(temporary.filePath("package"), temporary.filePath(QStringLiteral("搬移后的 模型"))));
        CModelJson model; model.Parse(temporary.filePath(QStringLiteral("搬移后的 模型/")) + prepared.modelFile); QVERIFY(model.IsOpen());
        for (const auto& path : model.getAllZipFiles()) { QVERIFY(QFileInfo::exists(path)); QVERIFY(path.startsWith(temporary.path())); }
        CModelDocument loaded; QVERIFY2(loaded.LoadBytes(read(model.getMocFileName()), {}, &error), qPrintable(error));
        for (const auto& captured : prepared.sources) QCOMPARE(digest(read(captured.path)), captured.digest);
        QCOMPARE(loaded.SourceBytes(), input.mocBytes);
    }
    void collectsExtensionsAudioAndDraftsWithoutWritingSource()
    {
        Fixture fixture; QVERIFY(fixture.document.IsLoaded());
        const auto part = fixture.document.Parts().first().id; const auto mesh = fixture.document.Drawables().first().id;
        fixture.resource("Pose", QStringLiteral("姿势.pose3.json"), json({{"Version", 3}, {"Groups", QJsonArray{QJsonArray{QJsonObject{{"Id", part}, {"Custom", 7}}}}}}));
        fixture.resource("UserData", "tags.userdata3.json", json({{"Version", 3}, {"UserData", QJsonArray{QJsonObject{{"Target", "ArtMesh"}, {"Id", mesh}, {"Value", "tag"}}}}}));
        fixture.resource("Physics", "draft.physics3.json", json({{"Version", 3}, {"Meta", QJsonObject{{"VertexCount", 900}, {"Custom", true}}}, {"PhysicsSettings", QJsonArray{}}}));
        fixture.resource("MotionSync", "speech.motionsync3.json", json({{"Version", 1}, {"Settings", QJsonArray{}}, {"Custom", "sync"}}));
        const auto motion = read(QString(TEST_RESOURCES) + "/Mark/motions/mark_m01.motion3.json");
        QVERIFY(!motion.isEmpty()); QVERIFY(write(fixture.directory.filePath("move.motion3.json"), motion));
        QVERIFY(write(fixture.directory.filePath(QStringLiteral("音声.音")), "RIFFfixture"));
        auto files = fixture.root["FileReferences"].toObject();
        files["Motions"] = QJsonObject{{"Edited", QJsonArray{QJsonObject{{"File", "move.motion3.json"}, {"Sound", QStringLiteral("音声.音")}, {"MotionSync", "SettingsName"}, {"FadeInTime", 0.25}}}}};
        files["Expressions"] = QJsonArray{QJsonObject{{"Name", "smile"}, {"File", "smile.exp3.json"}}};
        QVERIFY(write(fixture.directory.filePath("smile.exp3.json"), json({{"Type", "Live2D Expression"}, {"Parameters", QJsonArray{}}})));
        fixture.root["FileReferences"] = files; QVERIFY(fixture.save());
        auto input = request(fixture.path); input.timeline = motion; input.timelineSoundPath = fixture.directory.filePath(QStringLiteral("音声.音"));
        input.physics = json({{"Version", 3}, {"Meta", QJsonObject{{"TotalInputCount", -1}, {"Custom", "draft"}}}, {"PhysicsSettings", QJsonArray{}}});
        CRuntimePackage::Prepared prepared; QString error;
        QVERIFY2(CRuntimePackage::Prepare(input, prepared, nullptr, &error), qPrintable(error));
        const auto result = prepared.model["FileReferences"].toObject(); const auto motions = result["Motions"].toObject();
        QVERIFY(motions.contains("Edited_2")); const auto first = motions["Edited"].toArray().first().toObject();
        QCOMPARE(first["MotionSync"].toString(), "SettingsName"); QCOMPARE(first["FadeInTime"].toDouble(), .25);
        QCOMPARE(first["File"], motions["Edited_2"].toArray().first().toObject()["File"]);
        QVERIFY(first["Sound"].toString().endsWith(".bin")); QCOMPARE(entry(prepared, first["Sound"].toString()), "RIFFfixture");
        QCOMPARE(QJsonDocument::fromJson(entry(prepared, result["Physics"].toString())).object()["Meta"].toObject()["TotalInputCount"].toInt(), 0);
        QCOMPARE(QJsonDocument::fromJson(entry(prepared, result["Physics"].toString())).object()["Meta"].toObject()["Custom"].toString(), "draft");
        QCOMPARE(entry(prepared, result["Pose"].toString()), read(fixture.directory.filePath(QStringLiteral("姿势.pose3.json"))));
        QCOMPARE(prepared.model["Custom"].toString(), "kept");
        for (const auto& source : prepared.sources) QCOMPARE(digest(read(source.path)), source.digest);
    }
    void sparseDisplayInfoPreservesOriginalBytesAndFriendlyNames()
    {
        Fixture fixture; const auto parameter = fixture.document.Parameters().first();
        const auto original = QByteArray("  ") + json({{"Version", 3}, {"Parameters", QJsonArray{QJsonObject{{"Id", parameter.id}, {"Name", "Friendly name"}, {"Custom", true}}}}, {"Other", 1}});
        fixture.resource("DisplayInfo", "names.cdi3.json", original);
        auto input = request(fixture.path); CRuntimePackage::Prepared prepared; QString error;
        QVERIFY2(CRuntimePackage::Prepare(input, prepared, nullptr, &error), qPrintable(error));
        QCOMPARE(entry(prepared, prepared.model["FileReferences"].toObject()["DisplayInfo"].toString()), original);
        CModelParameterMetadata metadata; metadata.displayName = parameter.id; input.parameterMetadata.insert(parameter.id, metadata);
        QVERIFY2(CRuntimePackage::Prepare(input, prepared, nullptr, &error), qPrintable(error));
        QCOMPARE(entry(prepared, prepared.model["FileReferences"].toObject()["DisplayInfo"].toString()), original);
        metadata.displayName = QStringLiteral("新名称"); input.parameterMetadata[parameter.id] = metadata;
        QVERIFY2(CRuntimePackage::Prepare(input, prepared, nullptr, &error), qPrintable(error));
        const auto root = QJsonDocument::fromJson(entry(prepared, prepared.model["FileReferences"].toObject()["DisplayInfo"].toString())).object();
        QCOMPARE(root["Parameters"].toArray().size(), 1); QCOMPARE(root["Parameters"].toArray().first().toObject()["Name"].toString(), QStringLiteral("新名称")); QCOMPARE(root["Other"].toInt(), 1);
    }
    void rejectsInvalidSourcesWithoutChangingOutput_data()
    {
        QTest::addColumn<int>("index"); for (int i = 0; i < 10; ++i) QTest::newRow(qPrintable(QString::number(i))) << i;
    }
    void rejectsInvalidSourcesWithoutChangingOutput()
    {
        QFETCH(int, index); Fixture fixture; auto files = fixture.root["FileReferences"].toObject();
        if (index == 0) files["FutureFile"] = "external.bin";
        if (index == 1) files["Pose"] = "missing.json";
        if (index == 2) { fixture.resource("Pose", "pose.json", json({{"Groups", QJsonArray{QJsonArray{QJsonObject{{"Id", "missing"}}}}}})); files = fixture.root["FileReferences"].toObject(); }
        if (index == 3) { fixture.resource("UserData", "tag.json", json({{"Version", 3}, {"UserData", QJsonArray{QJsonObject{{"Target", "ArtMesh"}, {"Id", "missing"}, {"Value", "tag"}}}}})); files = fixture.root["FileReferences"].toObject(); }
        fixture.root["FileReferences"] = files; QVERIFY(fixture.save()); auto input = request(fixture.path);
        if (index == 4) input.modelSettingsDigest = QByteArray(32, 'x');
        if (index == 5) input.sourceMocDigest = QByteArray(32, 'x');
        if (index == 6) input.expectedSourceDigests[files["Textures"].toArray().first().toString()] = QByteArray(32, 'x');
        if (index == 7) input.expectedSourceDigests[fixture.directory.filePath("not-linked")] = QByteArray(32, 'x');
        if (index == 8) input.maximumFileBytes = 16;
        if (index == 9) input.maximumFiles = 1;
        CRuntimePackage::Prepared prepared; prepared.modelFile = "unchanged"; prepared.totalBytes = 47; QString error;
        QVERIFY(!CRuntimePackage::Prepare(input, prepared, nullptr, &error)); QVERIFY(!error.isEmpty()); QCOMPARE(prepared.modelFile, "unchanged"); QCOMPARE(prepared.totalBytes, 47);
    }
    void cancellationLeavesNoPreparedResultOrWrites()
    {
        Fixture fixture; auto input = request(fixture.path); CExportControl control;
        control.SetHandler([](const CExportControl::Progress&, QString*) { return false; });
        CRuntimePackage::Prepared prepared; QString error;
        QVERIFY(!CRuntimePackage::Prepare(input, prepared, &control, &error)); QVERIFY(!error.isEmpty()); QVERIFY(prepared.entries.isEmpty()); QCOMPARE(read(fixture.path), json(fixture.root));
    }
    void detectsSourceMutationDuringPreparation()
    {
        Fixture fixture; auto input = request(fixture.path); bool changed = false;
        CExportControl control; control.SetHandler([&](const CExportControl::Progress& progress, QString*) {
            if (!changed && progress.detail.startsWith(QStringLiteral("准备 model/"))) { changed = true; write(fixture.path, json(fixture.root) + " "); }
            return true;
        });
        CRuntimePackage::Prepared prepared; QString error;
        QVERIFY(!CRuntimePackage::Prepare(input, prepared, &control, &error)); QVERIFY(changed); QVERIFY(!error.isEmpty()); QVERIFY(prepared.entries.isEmpty());
    }

    void finalProgressCannotInvalidateSourceVerification()
    {
        Fixture fixture; CRuntimePackage::Request input; QString error;
        QVERIFY2(artworkRequest(fixture, input, &error), qPrintable(error));
        CExportControl control; bool changed = false;
        control.SetHandler([&](const CExportControl::Progress& progress, QString*) {
            if (progress.detail == QStringLiteral("模型包准备完成")) { changed = true; write(fixture.path, json(fixture.root) + " "); }
            return true;
        });
        CRuntimePackage::Prepared output; output.totalBytes = 93;
        QVERIFY(!CRuntimePackage::Prepare(input, output, &control, &error)); QVERIFY(changed); QVERIFY(!error.isEmpty());
        QCOMPARE(output.totalBytes, 93); QVERIFY(output.entries.isEmpty());
    }
};
QTEST_GUILESS_MAIN(RuntimePackageTests)
#include "runtime_package_tests.moc"
