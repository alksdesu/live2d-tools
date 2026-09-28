#include "live2dstruct/cartworkpackage.h"
#include "live2dstruct/cmocmaterialbuilder.h"
#include "std/cexportcontrol.h"
#include <QtTest>
#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>

using namespace live2dstruct;
namespace {
using Package = CArtworkPackage;
using Artwork = CModelArtwork;
QByteArray hash(const QByteArray& bytes) { return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256); }
QByteArray png() {
    QImage image(64, 32, QImage::Format_RGBA8888); image.fill(QColor(47, 79, 113, 183)); image.setPixelColor(0, 0, QColor(41, 53, 71, 0));
    QBuffer buffer; buffer.open(QIODevice::WriteOnly); image.save(&buffer, "PNG"); return buffer.data();
}
bool write(const QString& path, const QByteArray& data) {
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) return false;
    QFile file(path); return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
}
QByteArray read(const QString& path) { QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray(); }
struct Fixture {
    QTemporaryDir temporary;
    CModelDocument model;
    Package::Request request;
    QString error, mesh;
    bool initialize(bool sparse = false) {
        if (!temporary.isValid() || !model.Load(QString(TEST_RESOURCES) + "/Mark/Mark.moc3", &error)) return false;
        const auto bytes = png(); CTextureAssets::Source source;
        const auto sourcePath = temporary.filePath("old/source.png");
        if (!write(sourcePath, bytes) || !CTextureAssets::MakeSource(sourcePath, bytes, source, &error)) return false;
        request.sources.insert(0, source);
        if (sparse) request.sources.insert(2, source);
        if (!CTextureWorkspace::Initialize(request.sources, {}, request.state.workspace, &error) || !model.CaptureDrawableMaterials(request.state.materials, &error)) return false;
        const int slot = sparse ? 3 : 1;
        request.state.workspace.atlases.insert(slot, {"generated:A", "原画图集", -1, source.size, source.png});
        auto material = request.state.materials.first(); mesh = material.id; material.textureIndex = slot; request.state.materials[0] = material;
        if (!CMocMaterialBuilder::Build(model.SourceBytes(), {{mesh, material}}, request.mocBytes, &error)) return false;
        QJsonArray textures; textures.append("textures/atlas.png"); if (sparse) { textures.append(""); textures.append("textures/atlas.png"); }
        textures.append("textures/generated.png");
        request.modelFile = "model.model3.json";
        request.modelSettingsBytes = QJsonDocument(QJsonObject{{"Version", 3}, {"FileReferences", QJsonObject{{"Moc", "model/edited.moc3"}, {"Textures", textures}}}}).toJson(QJsonDocument::Compact);
        auto& artwork = request.state.artwork; const auto digest = hash(bytes);
        artwork.images.insert(digest, {source.size, bytes});
        artwork.families.insert("family", {"family", "PNG来源", Artwork::SourceKind::Png, "revision2"});
        Artwork::Revision revision; revision.id = "revision1"; revision.familyId = "family"; revision.path = temporary.filePath("missing/source.png");
        revision.fileName = "source.png"; revision.sourceBytes = bytes; revision.sourceDigest = digest; revision.canvasSize = source.size;
        Artwork::Layer layer; layer.id = "layer"; layer.name = "源层"; layer.bounds = QRect(QPoint(), source.size); layer.imageDigest = digest; layer.sourceIndex = 0;
        revision.layers.append(layer); artwork.revisions.insert(revision.id, revision); revision.id = "revision2"; artwork.revisions.insert(revision.id, revision);
        artwork.materials.insert("material", {"material", "源", "family", "revision1", "layer", Artwork::Boundary::Repeat, true, {}});
        artwork.placements.insert("placement", {"placement", "material", "generated:A", QTransform(), QRectF(QPointF(), source.size), 1, Artwork::Filter::ExactTexels});
        artwork.placements.insert("unused", {"unused", "material", "source:1", QTransform(), QRectF(QPointF(), source.size), 1, Artwork::Filter::ExactTexels});
        Artwork::Binding binding; binding.meshId = mesh; binding.placementId = "placement"; binding.sampledMaterial = material;
        for (int i = 0; i < material.uvs.size(); i += 2) binding.familyCoordinates.append({double(material.uvs[i]) * source.size.width(), double(material.uvs[i+1]) * source.size.height()});
        if (!Artwork::TopologyDigest(binding.familyCoordinates.size(), model.DrawableIndices(mesh), binding.topologyDigest, &error)) return false;
        artwork.bindings.insert(mesh, binding);
        return true;
    }
    bool publish(const QString& directory, const Package::Prepared& prepared) {
        if (!write(directory + '/' + request.modelFile, request.modelSettingsBytes) || !write(directory + "/model/edited.moc3", request.mocBytes)) return false;
        CTextureAssets::State pngs; if (!CTextureWorkspace::Effective(request.sources, request.state.workspace, pngs, &error)) return false;
        const auto textures = QJsonDocument::fromJson(request.modelSettingsBytes).object()["FileReferences"].toObject()["Textures"].toArray();
        for (int slot = 0; slot < textures.size(); ++slot) if (!textures[slot].toString().isEmpty() && !write(directory + '/' + textures[slot].toString(), pngs[slot])) return false;
        for (const auto& entry : prepared.entries) if (!write(directory + '/' + entry.relativePath, entry.bytes)) return false;
        return true;
    }
    bool loaded(const QString& directory, CModelDocument& result) {
        if (!result.Load(directory + "/model/edited.moc3", &error)) return false;
        CTextureAssets::Catalog sources; const auto textures = QJsonDocument::fromJson(request.modelSettingsBytes).object()["FileReferences"].toObject()["Textures"].toArray();
        for (int slot = 0; slot < textures.size(); ++slot) if (!textures[slot].toString().isEmpty()) {
            const auto path = directory + '/' + textures[slot].toString(); CTextureAssets::Source source;
            if (!CTextureAssets::MakeSource(path, read(path), source, &error)) return false; sources.insert(slot, source);
        }
        return result.InitializeTextureWorkspace(sources, {}, &error);
    }
};
}
class ArtworkPackageTests : public QObject {
    Q_OBJECT
private slots:
    void cmoDocumentsShareOneOriginalBlobAcrossPortableRevisions() {
        Fixture f; QVERIFY2(f.initialize(), qPrintable(f.error)); auto& artwork = f.request.state.artwork;
        const QByteArray raw("CAFF retained original archive bytes"); const auto digest = hash(raw);
        artwork.families["family"].kind = Artwork::SourceKind::Cmo3;
        for (auto& revision : artwork.revisions) {
            revision.sourceBytes = raw; revision.sourceDigest = digest; revision.sourceDocumentId = "document-one";
            revision.fileName = "source.cmo3"; revision.path = "/offline/source.cmo3";
        }
        auto other = artwork.revisions["revision1"]; other.id = "other-revision"; other.familyId = "other-family"; other.sourceDocumentId = "document-two";
        artwork.revisions.insert(other.id, other); artwork.families.insert(other.familyId, {other.familyId, "第二份原稿", Artwork::SourceKind::Cmo3, other.id});
        Package::Prepared prepared; QVERIFY2(Package::Prepare(f.request, prepared, nullptr, &f.error), qPrintable(f.error));
        const auto index = QJsonDocument::fromJson(prepared.entries.last().bytes).object();
        QCOMPARE(index["version"].toInt(), 1); QCOMPARE(index["artwork"].toObject()["version"].toInt(), 2);
        QCOMPARE(index["objects"].toObject().size(), 2);
        const auto object = index["objects"].toObject()[QString::fromLatin1(digest.toHex())].toObject();
        QVERIFY(object["path"].toString().endsWith(".bin"));
        const auto first = f.temporary.filePath("first"), moved = f.temporary.filePath("moved");
        QVERIFY(f.publish(first, prepared)); QVERIFY(QDir().rename(first, moved)); CModelDocument model; QVERIFY(f.loaded(moved, model));
        Package::Loaded loaded; QVERIFY2(Package::Load(moved + '/' + f.request.modelFile, f.request.modelSettingsBytes, model, loaded, nullptr, &f.error), qPrintable(f.error));
        QCOMPARE(loaded.state.artwork.revisions["revision1"].sourceDocumentId, QString("document-one"));
        QCOMPARE(loaded.state.artwork.revisions["other-revision"].sourceDocumentId, QString("document-two"));
        for (const auto& revision : loaded.state.artwork.revisions) {
            QCOMPARE(revision.sourceBytes, raw); QCOMPARE(revision.sourceDigest, digest); QVERIFY(revision.path.startsWith(moved + '/'));
        }
    }
    void archiveSharesExactSessionMetadataAndOwnsBlobs() {
        Fixture f; QVERIFY(f.initialize()); Artwork::Archive archive; QJsonObject json;
        QVERIFY2(Artwork::EncodeArchive(f.request.state.artwork, archive, &f.error), qPrintable(f.error)); QVERIFY(Artwork::ToJson(f.request.state.artwork, json, &f.error));
        const auto blobs = json.take("blobs").toObject(); QCOMPARE(json, archive.metadata); QCOMPARE(blobs.size(), archive.blobs.size());
        for (auto it = archive.blobs.cbegin(); it != archive.blobs.cend(); ++it) QCOMPARE(blobs[QString::fromLatin1(it.key().toHex())].toString().toLatin1(), it->toBase64());
        Artwork::State decoded; QVERIFY(Artwork::DecodeArchive(archive, decoded, &f.error)); QVERIFY(decoded == f.request.state.artwork);
        auto bad = archive; bad.metadata["unknown"] = true; QVERIFY(!Artwork::DecodeArchive(bad, decoded, &f.error)); QVERIFY(decoded == f.request.state.artwork);
        bad = archive; bad.blobs.begin().value()[0] ^= 1; QVERIFY(!Artwork::DecodeArchive(bad, decoded, &f.error)); QVERIFY(decoded == f.request.state.artwork);
        QByteArray mutableBytes = png(); auto borrowed = archive; borrowed.blobs.begin().value() = QByteArray::fromRawData(mutableBytes.data(), mutableBytes.size());
        QVERIFY(Artwork::DecodeArchive(borrowed, decoded, &f.error)); mutableBytes[0] ^= 1; QVERIFY(decoded == f.request.state.artwork);
    }
    void portableRoundtripReanchorsGeneratedSharedAndSparseSlots_data() { QTest::addColumn<bool>("sparse"); QTest::newRow("dense") << false; QTest::newRow("sparse-shared-png") << true; }
    void portableRoundtripReanchorsGeneratedSharedAndSparseSlots() {
        QFETCH(bool, sparse); Fixture f; QVERIFY2(f.initialize(sparse), qPrintable(f.error)); Package::Prepared prepared;
        QVERIFY2(Package::Prepare(f.request, prepared, nullptr, &f.error), qPrintable(f.error)); QCOMPARE(prepared.manifestFile, QString("model.model3.json.artwork.json")); QCOMPARE(prepared.entries.size(), 2);
        const auto index = QJsonDocument::fromJson(prepared.entries.last().bytes).object(); QVERIFY(!prepared.entries.last().bytes.contains(f.temporary.path().toUtf8()));
        QCOMPARE(index["objects"].toObject().size(), 1); const auto first = f.temporary.filePath("first"), moved = f.temporary.filePath("moved");
        QVERIFY(f.publish(first, prepared)); QVERIFY(QDir().rename(first, moved)); CModelDocument loaded; QVERIFY2(f.loaded(moved, loaded), qPrintable(f.error));
        Package::Loaded value; QVERIFY2(Package::Load(moved + '/' + f.request.modelFile, f.request.modelSettingsBytes, loaded, value, nullptr, &f.error), qPrintable(f.error)); QVERIFY(value.present);
        const int slot = sparse ? 3 : 1; QCOMPARE(value.state.artwork.placements["placement"].atlasId, QString("source:%1").arg(slot));
        QVERIFY(value.state.artwork.placements["unused"].atlasId.startsWith("unavailable:artwork-atlas:"));
        QCOMPARE(value.state.workspace.atlases[slot].name, QString("原画图集")); QCOMPARE(value.state.artwork.materials["material"].revisionId, QString("revision1"));
        QCOMPARE(value.state.artwork.bindings[f.mesh].familyCoordinates, f.request.state.artwork.bindings[f.mesh].familyCoordinates);
        QCOMPARE(value.state.artwork.revisions["revision1"].sourceBytes, f.request.state.artwork.revisions["revision1"].sourceBytes);
        QVERIFY(value.state.artwork.revisions["revision1"].path.startsWith(moved + '/'));
        Package::Request next{f.request.modelFile, f.request.modelSettingsBytes, f.request.mocBytes, loaded.TextureSources(), value.state, {}}; Package::Prepared again;
        QVERIFY2(Package::Prepare(next, again, nullptr, &f.error), qPrintable(f.error));
        for (const auto& entry : again.entries) QVERIFY(write(moved + '/' + entry.relativePath, entry.bytes));
        Package::Loaded twice; QVERIFY2(Package::Load(moved + '/' + f.request.modelFile, f.request.modelSettingsBytes, loaded, twice, nullptr, &f.error), qPrintable(f.error)); QVERIFY(twice.state == value.state);
    }
    void detachedAndSourceOnlyRemainInactive_data() { QTest::addColumn<bool>("sourceOnly"); QTest::newRow("detached") << false; QTest::newRow("library-only") << true; }
    void detachedAndSourceOnlyRemainInactive() {
        QFETCH(bool, sourceOnly); Fixture f; QVERIFY(f.initialize());
        if (sourceOnly) { f.request.state.artwork.bindings.clear(); f.request.state.artwork.placements.clear(); f.request.state.artwork.materials.clear(); }
        else { auto& binding = f.request.state.artwork.bindings[f.mesh]; binding.status = Artwork::BindingStatus::Detached; binding.detachedReason = "manual"; binding.placementId = "unused"; }
        Package::Prepared prepared; QVERIFY2(Package::Prepare(f.request, prepared, nullptr, &f.error), qPrintable(f.error)); const auto dir = f.temporary.filePath("output"); QVERIFY(f.publish(dir, prepared));
        CModelDocument loaded; QVERIFY(f.loaded(dir, loaded)); Package::Loaded result;
        QVERIFY2(Package::Load(dir + '/' + f.request.modelFile, f.request.modelSettingsBytes, loaded, result, nullptr, &f.error), qPrintable(f.error));
        if (sourceOnly) QVERIFY(result.state.artwork.bindings.isEmpty());
        else { QCOMPARE(result.state.artwork.bindings[f.mesh].status, Artwork::BindingStatus::Detached); QVERIFY(result.state.artwork.placements["unused"].atlasId != "source:1"); }
    }
    void malformedAttachmentIsAtomic_data() {
        QTest::addColumn<QString>("change");
        for (const auto& name : {"unknown", "version", "anchor", "path", "revision-path", "revision-type", "blob", "missing", "model", "png", "symlink", "directory-link"}) QTest::newRow(name) << QString(name);
    }
    void malformedAttachmentIsAtomic() {
        QFETCH(QString, change); Fixture f; QVERIFY(f.initialize()); Package::Prepared prepared; QVERIFY(Package::Prepare(f.request, prepared, nullptr, &f.error));
        const auto dir = f.temporary.filePath("output"); QVERIFY(f.publish(dir, prepared)); CModelDocument loaded; QVERIFY(f.loaded(dir, loaded));
        const auto manifestPath = dir + '/' + prepared.manifestFile; auto root = QJsonDocument::fromJson(read(manifestPath)).object(); const auto key = root["objects"].toObject().keys().first(); const auto blobPath = root["objects"].toObject()[key].toObject()["path"].toString();
        if (change == "unknown") root["extra"] = 1;
        if (change == "version") root["version"] = 2;
        if (change == "anchor") { auto model = root["moc"].toObject(); model["sha256"] = QString(64, '0'); root["moc"] = model; }
        if (change == "path") { auto objects = root["objects"].toObject(); auto blob = objects[key].toObject(); blob["path"] = "../source.png"; objects[key] = blob; root["objects"] = objects; }
        if (change == "revision-path" || change == "revision-type") { auto artwork = root["artwork"].toObject(); auto revisions = artwork["revisions"].toObject(); auto rev = revisions["revision1"].toObject(); rev["path"] = "/tmp/evil.png"; revisions["revision1"] = rev; artwork["revisions"] = change == "revision-type" ? QJsonValue(QJsonArray{}) : QJsonValue(revisions); root["artwork"] = artwork; }
        QVERIFY(write(manifestPath, QJsonDocument(root).toJson(QJsonDocument::Compact)));
        if (change == "blob") { auto bytes = read(dir + '/' + blobPath); bytes[0] ^= 1; QVERIFY(write(dir + '/' + blobPath, bytes)); }
        if (change == "missing") QVERIFY(QFile::remove(dir + '/' + blobPath));
        if (change == "model") QVERIFY(write(dir + '/' + f.request.modelFile, f.request.modelSettingsBytes + ' '));
        if (change == "png") { auto bytes = png(); bytes[30] ^= 1; QVERIFY(write(dir + "/textures/generated.png", bytes)); }
        if (change == "symlink") { const auto external = f.temporary.filePath("external.png"); QVERIFY(write(external, read(dir + '/' + blobPath))); QVERIFY(QFile::remove(dir + '/' + blobPath)); QVERIFY(QFile::link(external, dir + '/' + blobPath)); }
        if (change == "directory-link") { const auto external = f.temporary.filePath("external-directory"); QVERIFY(QDir().rename(dir + "/authoring", external)); QVERIFY(QFile::link(external, dir + "/authoring")); }
        Package::Loaded result; result.present = true; result.notes = {"sentinel"};
        QVERIFY(!Package::Load(dir + '/' + f.request.modelFile, f.request.modelSettingsBytes, loaded, result, nullptr, &f.error)); QVERIFY(!f.error.isEmpty()); QCOMPARE(result.notes, QStringList{"sentinel"}); QVERIFY(result.present);
    }
    void missingAttachmentIsOptionalAndLimitsAndCancellationAreAtomic() {
        Fixture f; QVERIFY(f.initialize()); Package::Prepared prepared; QVERIFY(Package::Prepare(f.request, prepared, nullptr, &f.error)); const auto dir = f.temporary.filePath("output"); QVERIFY(f.publish(dir, prepared)); CModelDocument loaded; QVERIFY(f.loaded(dir, loaded));
        Package::Loaded result; result.notes = {"sentinel"}; Package::Limits small; small.maximumIndexBytes = 1;
        QVERIFY(!Package::Load(dir + '/' + f.request.modelFile, f.request.modelSettingsBytes, loaded, small, result, nullptr, &f.error)); QCOMPARE(result.notes, QStringList{"sentinel"});
        CExportControl canceled; canceled.Cancel(); QVERIFY(!Package::Load(dir + '/' + f.request.modelFile, f.request.modelSettingsBytes, loaded, result, &canceled, &f.error)); QCOMPARE(result.notes, QStringList{"sentinel"});
        QVERIFY(QFile::remove(dir + '/' + prepared.manifestFile)); QVERIFY(Package::Load(dir + '/' + f.request.modelFile, f.request.modelSettingsBytes, loaded, result, nullptr, &f.error)); QVERIFY(!result.present); QVERIFY(result.notes.isEmpty());
        auto limited = f.request; limited.limits.maximumFileBytes = 1; Package::Prepared unchanged; unchanged.manifestFile = "sentinel"; QVERIFY(!Package::Prepare(limited, unchanged, nullptr, &f.error)); QCOMPARE(unchanged.manifestFile, QString("sentinel"));
    }
    void rootAliasesPreserveLogicalSourcesWithoutAllowingInteriorLinks() {
        Fixture f; QVERIFY(f.initialize()); Package::Prepared prepared; QVERIFY(Package::Prepare(f.request, prepared, nullptr, &f.error));
        const auto dir = f.temporary.filePath("real"), alias = f.temporary.filePath("alias"); QVERIFY(f.publish(dir, prepared)); QVERIFY(QFile::link(dir, alias));
        CModelDocument loaded; QVERIFY(f.loaded(alias, loaded)); QVERIFY(loaded.TextureSources().first().path.startsWith(alias + '/'));
        Package::Loaded result; QVERIFY2(Package::Load(alias + '/' + f.request.modelFile, f.request.modelSettingsBytes, loaded, result, nullptr, &f.error), qPrintable(f.error));
        QVERIFY(result.present); QVERIFY(result.state.artwork.revisions.first().path.startsWith(QFileInfo(dir).canonicalFilePath() + '/'));
        const auto baseline = result.state; const auto textures = f.temporary.filePath("external-textures"); QVERIFY(QDir().rename(dir + "/textures", textures)); QVERIFY(QFile::link(textures, dir + "/textures"));
        QVERIFY(!Package::Load(alias + '/' + f.request.modelFile, f.request.modelSettingsBytes, loaded, result, nullptr, &f.error)); QVERIFY(result.state == baseline);
    }
    void resourcePathConflictsAreRejectedBeforePublishing_data() {
        QTest::addColumn<QString>("name"); QTest::addColumn<QString>("mocPath");
        QTest::newRow("identical-file") << "model.model3.json" << "model.model3.json";
        QTest::newRow("case-alias") << "model.model3.json" << "MODEL.MODEL3.JSON";
        QTest::newRow("unicode-alias") << QString::fromUtf8("é.json") << QString::fromUtf8("e\u0301.json");
        QTest::newRow("file-as-directory") << "model.model3.json" << "model.model3.json/moc";
        QTest::newRow("directory-as-file") << "model.model3.json" << "textures";
    }
    void resourcePathConflictsAreRejectedBeforePublishing() {
        QFETCH(QString, name); QFETCH(QString, mocPath); Fixture f; QVERIFY(f.initialize()); f.request.modelFile = name;
        auto root = QJsonDocument::fromJson(f.request.modelSettingsBytes).object(); auto files = root["FileReferences"].toObject(); files["Moc"] = mocPath; root["FileReferences"] = files; f.request.modelSettingsBytes = QJsonDocument(root).toJson();
        Package::Prepared result; result.manifestFile = "sentinel"; QVERIFY(!Package::Prepare(f.request, result, nullptr, &f.error)); QVERIFY(f.error.contains("冲突")); QCOMPARE(result.manifestFile, QString("sentinel"));
    }
    void knownSnapshotSizeRejectsGrownFilesBeforeReadingTheirContent_data() {
        QTest::addColumn<QString>("kind"); QTest::newRow("model") << "model"; QTest::newRow("moc") << "moc"; QTest::newRow("png") << "png"; QTest::newRow("object") << "object";
    }
    void knownSnapshotSizeRejectsGrownFilesBeforeReadingTheirContent() {
        QFETCH(QString, kind); Fixture f; QVERIFY(f.initialize()); Package::Prepared prepared; QVERIFY(Package::Prepare(f.request, prepared, nullptr, &f.error));
        const auto dir = f.temporary.filePath("output"); QVERIFY(f.publish(dir, prepared)); CModelDocument loaded; QVERIFY(f.loaded(dir, loaded));
        const auto relative = kind == "model" ? f.request.modelFile : kind == "moc" ? QString("model/edited.moc3") : kind == "png" ? QString("textures/atlas.png") : prepared.entries.first().relativePath;
        QVERIFY(write(dir + '/' + relative, read(dir + '/' + relative) + QByteArray(4 * 1024 * 1024, 'x')));
        CExportControl control; int callbacks = 0; QVERIFY(control.SetHandler([&](const CExportControl::Progress&, QString*) { ++callbacks; return true; }));
        Package::Loaded result; result.notes = {"sentinel"}; QVERIFY(!Package::Load(dir + '/' + f.request.modelFile, f.request.modelSettingsBytes, loaded, result, &control, &f.error));
        QVERIFY(callbacks <= 5); QCOMPARE(result.notes, QStringList{"sentinel"}); QVERIFY(!f.error.isEmpty());
    }
    void overLimitInputsFailBeforeCallbacksAndCopies() {
        Fixture f; QVERIFY(f.initialize()); auto request = f.request; request.state.materials.first().uvs.resize(CModelMaterialEdit::MaximumValues + 2);
        CExportControl control; int callbacks = 0; QVERIFY(control.SetHandler([&](const CExportControl::Progress&, QString*) { ++callbacks; return true; }));
        Package::Prepared result; result.manifestFile = "sentinel"; QVERIFY(!Package::Prepare(request, result, &control, &f.error)); QCOMPARE(callbacks, 0); QCOMPARE(result.manifestFile, QString("sentinel"));
        request = f.request; request.modelSettingsBytes = QByteArray(16 * 1024 * 1024 + 1, ' '); QVERIFY(!Package::Prepare(request, result, &control, &f.error)); QCOMPARE(callbacks, 0);
        Package::Loaded loaded; loaded.notes = {"sentinel"}; QVERIFY(!Package::Load(f.temporary.filePath("model.json"), request.modelSettingsBytes, f.model, loaded, &control, &f.error)); QCOMPARE(callbacks, 0); QCOMPARE(loaded.notes, QStringList{"sentinel"});
    }
    void exactResourceBoundariesAndUnusualModelName() {
        Fixture f; QVERIFY(f.initialize(true)); f.request.modelFile = "custom.json"; Package::Prepared prepared;
        QVERIFY2(Package::Prepare(f.request, prepared, nullptr, &f.error), qPrintable(f.error)); QCOMPARE(prepared.manifestFile, QString("custom.json.artwork.json"));
        qint64 expectedOutput = 0, maximumFile = 0;
        for (const auto& entry : prepared.entries) { expectedOutput += entry.bytes.size(); maximumFile = std::max(maximumFile, qint64(entry.bytes.size())); }
        QCOMPARE(prepared.totalBytes, expectedOutput);
        auto request = f.request; request.limits.maximumTotalBytes = expectedOutput; request.limits.maximumFiles = prepared.entries.size();
        Package::Prepared exact; QVERIFY2(Package::Prepare(request, exact, nullptr, &f.error), qPrintable(f.error));
        request.limits.maximumTotalBytes--; QVERIFY(!Package::Prepare(request, exact, nullptr, &f.error)); QCOMPARE(exact.totalBytes, expectedOutput);
        request = f.request; request.limits.maximumFiles = prepared.entries.size() - 1; QVERIFY(!Package::Prepare(request, exact, nullptr, &f.error));
        const auto dir = f.temporary.filePath("output"); QVERIFY(f.publish(dir, prepared)); CModelDocument loaded; QVERIFY(f.loaded(dir, loaded));
        QSet<QString> uniqueFiles{prepared.manifestFile, f.request.modelFile, "model/edited.moc3", "textures/atlas.png", "textures/generated.png"};
        for (const auto& entry : prepared.entries) uniqueFiles.insert(entry.relativePath);
        qint64 inputBytes = 0; for (const auto& file : uniqueFiles) inputBytes += read(dir + '/' + file).size();
        Package::Limits limits; limits.maximumTotalBytes = inputBytes; limits.maximumFiles = uniqueFiles.size(); limits.maximumIndexBytes = read(dir + '/' + prepared.manifestFile).size();
        Package::Loaded result; QVERIFY2(Package::Load(dir + '/' + f.request.modelFile, f.request.modelSettingsBytes, loaded, limits, result, nullptr, &f.error), qPrintable(f.error));
        const auto baseline = result.state; limits.maximumTotalBytes--; QVERIFY(!Package::Load(dir + '/' + f.request.modelFile, f.request.modelSettingsBytes, loaded, limits, result, nullptr, &f.error)); QVERIFY(result.state == baseline);
        limits.maximumTotalBytes++; limits.maximumFiles--; QVERIFY(!Package::Load(dir + '/' + f.request.modelFile, f.request.modelSettingsBytes, loaded, limits, result, nullptr, &f.error)); QVERIFY(result.state == baseline);
        limits.maximumFiles++; limits.maximumIndexBytes--; QVERIFY(!Package::Load(dir + '/' + f.request.modelFile, f.request.modelSettingsBytes, loaded, limits, result, nullptr, &f.error)); QVERIFY(result.state == baseline);
        QVERIFY(QFile::remove(dir + '/' + prepared.manifestFile)); QVERIFY(Package::Load(dir + '/' + f.request.modelFile, f.request.modelSettingsBytes, loaded, result, nullptr, &f.error)); QVERIFY(!result.present);
    }
    void lateFileMutationAndCancellationCannotPublishPartialState() {
        Fixture f; QVERIFY(f.initialize()); Package::Prepared prepared; QVERIFY(Package::Prepare(f.request, prepared, nullptr, &f.error)); const auto dir = f.temporary.filePath("output"); QVERIFY(f.publish(dir, prepared));
        CModelDocument loaded; QVERIFY(f.loaded(dir, loaded)); int checkpoints = 0; CExportControl counting;
        QVERIFY(counting.SetHandler([&](const CExportControl::Progress&, QString*) { ++checkpoints; return true; }));
        Package::Loaded result; QVERIFY(Package::Load(dir + '/' + f.request.modelFile, f.request.modelSettingsBytes, loaded, result, &counting, &f.error)); QVERIFY(checkpoints > 5);
        const auto baseline = result.state; const auto blobPath = dir + '/' + prepared.entries.first().relativePath; const auto original = read(blobPath); int count = 0; CExportControl mutation;
        QVERIFY(mutation.SetHandler([&](const CExportControl::Progress&, QString*) { if (++count == checkpoints) { auto bytes = original; bytes[0] ^= 1; return write(blobPath, bytes); } return true; }));
        QVERIFY(!Package::Load(dir + '/' + f.request.modelFile, f.request.modelSettingsBytes, loaded, result, &mutation, &f.error)); QCOMPARE(count, checkpoints); QVERIFY(result.state == baseline); QVERIFY(f.error.contains("最终复核"));
        QVERIFY(write(blobPath, original)); count = 0; CExportControl cancellation;
        QVERIFY(cancellation.SetHandler([&](const CExportControl::Progress&, QString*) { if (++count == checkpoints - 1) cancellation.Cancel(); return true; }));
        QVERIFY(!Package::Load(dir + '/' + f.request.modelFile, f.request.modelSettingsBytes, loaded, result, &cancellation, &f.error)); QVERIFY(cancellation.WasCanceled()); QVERIFY(result.state == baseline);
    }
    void borrowedInputsAndReentrantMutationAreFrozen() {
        Fixture f; QVERIFY(f.initialize()); auto request = f.request; QByteArray borrowed = request.state.artwork.images.first().png;
        request.state.artwork.images.begin()->png = QByteArray::fromRawData(borrowed.data(), borrowed.size()); request.state.artwork.revisions.begin()->sourceBytes = QByteArray::fromRawData(borrowed.data(), borrowed.size());
        bool changed = false; CExportControl control; QVERIFY(control.SetHandler([&](const CExportControl::Progress&, QString*) { if (!changed) { changed = true; borrowed[0] ^= 1; request.modelFile = "wrong"; request.state.materials.clear(); request.sources.clear(); } return true; }));
        Package::Prepared prepared; QVERIFY2(Package::Prepare(request, prepared, &control, &f.error), qPrintable(f.error)); QVERIFY(changed); QCOMPARE(prepared.manifestFile, QString("model.model3.json.artwork.json"));
        const auto dir = f.temporary.filePath("output"); QVERIFY(f.publish(dir, prepared)); CModelDocument loaded; QVERIFY(f.loaded(dir, loaded));
        bool replaced = false; CExportControl reader; QVERIFY(reader.SetHandler([&](const CExportControl::Progress&, QString*) { if (!replaced) { replaced = true; return write(dir + "/model/edited.moc3", QByteArray("changed")); } return true; }));
        Package::Loaded value; value.notes = {"sentinel"}; QVERIFY(!Package::Load(dir + '/' + f.request.modelFile, f.request.modelSettingsBytes, loaded, value, &reader, &f.error)); QCOMPARE(value.notes, QStringList{"sentinel"});
    }
};
QTEST_GUILESS_MAIN(ArtworkPackageTests)
#include "artwork_package_tests.moc"
