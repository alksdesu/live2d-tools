#include "live2dstruct/cartworkpsdrecovery.h"
#include "live2dstruct/cartworksourceimport.h"
#include "psd/cpsdreader.h"
#include "psd/cpsdsourcewriter.h"
#include "psd_binary_reader.h"
#include "std/cexportcontrol.h"
#include "psd_original_fixture.h"
#include "cmo_artwork_source_fixture.h"
#include "cmo3/ccmo3project.h"
#include <QBuffer>
#include <QCryptographicHash>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

using Artwork = live2dstruct::CModelArtwork;
using Recovery = live2dstruct::CArtworkPsdRecovery;
namespace {
QByteArray digest(const QByteArray& bytes) { return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256); }
Artwork::State fixture()
{
    const auto bytes = psd_original_fixture::fixture();
    CPsdSnapshot snapshot;
    if (!CPsdReader::ReadBytes(bytes, snapshot)) return {};
    snapshot.canonicalPath = "/missing/original.psd";
    live2dstruct::CArtworkSourceImport::SourceCandidate candidate;
    if (!live2dstruct::CArtworkSourceImport::Prepare(snapshot, "family", "old", Artwork::ColorPolicy::PreserveValues, {}, candidate)) return {};
    Artwork::State state;
    state.images = candidate.images;
    state.families.insert("family", {"family", "来源家族", Artwork::SourceKind::Psd, "current"});
    for (const QString& id : {QString("old"), QString("current"), QString("unused"), QString("policy")}) {
        auto revision = candidate.revision;
        revision.id = id;
        if (id == "policy") { revision.colorPolicy = Artwork::ColorPolicy::ConvertToSRgb; revision.documentToFamily.translate(27, -81); }
        state.revisions.insert(id, revision);
    }
    for (const QString& revision : {QString("old"), QString("current")}) {
        Artwork::Material material;
        material.id = "material-" + revision; material.name = material.id;
        material.familyId = "family"; material.revisionId = revision; material.layerId = "record:0";
        state.materials.insert(material.id, material);
        Artwork::Placement placement;
        placement.id = "placement-" + revision; placement.materialId = material.id; placement.atlasId = "atlas";
        placement.sourceDomain = QRectF(-1, -2, 2, 2);
        state.placements.insert(placement.id, placement);
    }
    for (int index = 0; index < 4; ++index) {
        Artwork::Binding binding;
        binding.meshId = QString("mesh-%1").arg(index);
        binding.placementId = index < 3 ? "placement-old" : "placement-current";
        binding.familyCoordinates = {{-1,-2},{1,-2},{-1,0}};
        binding.sampledMaterial = {binding.meshId, 0, {0,0,1,0,0,1}};
        binding.topologyDigest = QByteArray(32, 'x');
        if (index == 2) { binding.status = Artwork::BindingStatus::Detached; binding.detachedReason = "手工纹理修改"; }
        state.bindings.insert(binding.meshId, binding);
    }
    return state;
}
const CPsdSourceCatalog::Document* revision(const CPsdSourceCatalog& catalog, const QString& id)
{
    for (const auto& entry : catalog.documents) if (entry.origin.revisionId == id) return &entry;
    return nullptr;
}
Artwork::State cmoFixture(const QByteArray& bytes, QString* error)
{
    cmo3::CCmo3Project project;
    if (!cmo3::CCmo3Project::OpenBytes(bytes, "/offline/native.cmo3", project, error) || !project.Artwork()) return {};
    Artwork::State result;
    for (const auto& entry : project.Artwork()->documents) {
        const auto* document = entry.Reconstructed(); if (!document) return {};
        Artwork::Revision revision; revision.id = "revision:" + document->id; revision.familyId = "family:" + document->id;
        revision.path = project.SourcePath(); revision.fileName = "native.cmo3"; revision.sourceBytes = project.Bytes(); revision.sourceDigest = project.SourceDigest();
        revision.sourceDocumentId = document->id; revision.canvasSize = document->canvasSize; revision.iccProfile = document->iccProfile;
        for (const auto& source : document->layers) {
            Artwork::Layer layer; layer.id = source.id; layer.name = source.name; layer.parentIndex = source.parentIndex;
            layer.sourceIndex = revision.layers.size(); layer.kind = source.kind == CPsdSourceLayer::Kind::Group ? Artwork::LayerKind::Group : Artwork::LayerKind::Raster;
            layer.bounds = source.bounds; layer.visible = source.visible; layer.clipping = source.clipping; layer.opacity = source.opacity;
            layer.blendMode = source.blendMode; layer.externalId = source.externalId;
            if (!source.png.isEmpty()) { layer.imageDigest = digest(source.png); result.images.insert(layer.imageDigest, {source.bounds.size(), source.png}); }
            revision.layers.append(std::move(layer));
        }
        result.families.insert(revision.familyId, {revision.familyId, document->name, Artwork::SourceKind::Cmo3, revision.id});
        result.revisions.insert(revision.id, std::move(revision));
    }
    return result;
}
}

class ArtworkPsdRecoveryTests : public QObject
{
    Q_OBJECT
private slots:
    void savedCmoDocumentsRecoverNativeTreesFromOneEmbeddedArchive()
    {
        const auto bytes = cmo_artwork_source_fixture::archive(); QVERIFY(!bytes.isEmpty()); QString error;
        auto state = cmoFixture(bytes, &error); QVERIFY2(!state.IsEmpty(), qPrintable(error));
        QCOMPARE(state.families.size(), 2); QCOMPARE(state.images.size(), 1);
        auto alternate = state.revisions.first(); alternate.id = "alternate-policy"; alternate.colorPolicy = Artwork::ColorPolicy::ConvertToSRgb;
        alternate.documentToFamily = QTransform(2, 0, 0, 3, 100, -90); state.revisions.insert(alternate.id, alternate);
        QImage working(2, 1, QImage::Format_RGBA8888); working.fill(Qt::green); QByteArray png; QBuffer buffer(&png);
        QVERIFY(buffer.open(QIODevice::WriteOnly)); QVERIFY(working.save(&buffer, "PNG")); state.images.clear(); state.images.insert(digest(png), {working.size(), png});
        for (auto& value : state.revisions) for (auto& layer : value.layers) {
            layer.name = "伪造工作名称"; if (!layer.imageDigest.isEmpty()) layer.imageDigest = digest(png);
        }
        int reads = 0; CExportControl control; QString previousDetail;
        QVERIFY(control.SetHandler([&](const CExportControl::Progress& progress, QString*) {
            if (progress.detail == QStringLiteral("读取 CAFF 资源") && previousDetail != progress.detail) ++reads;
            previousDetail = progress.detail; return true;
        }));
        CPsdSourceCatalog catalog; QVERIFY2(Recovery::Prepare(state, catalog, &error, &control), qPrintable(error));
        QCOMPARE(reads, 1); QCOMPARE(catalog.documents.size(), 3); QCOMPARE(catalog.protectedPaths, QStringList{"/offline/native.cmo3"});
        for (const auto& entry : catalog.documents) {
            QVERIFY(entry.Reconstructed()); QVERIFY(!entry.Original()); QVERIFY(entry.diagnostics.isEmpty());
            QCOMPARE(entry.origin.sourceVersion, QString("502020000")); QCOMPARE(entry.origin.sourceDigest, digest(bytes));
            const auto& doc = *entry.Reconstructed(); QCOMPARE(doc.id, state.revisions[entry.origin.revisionId].sourceDocumentId);
            QCOMPARE(doc.name, QString("同名文档")); QCOMPARE(doc.layers.size(), 3); QVERIFY(!doc.iccProfile.isEmpty());
            QCOMPARE(doc.layers[0].name, QString("原始分组")); QCOMPARE(doc.layers[1].parentIndex, 0); QCOMPARE(doc.layers[2].parentIndex, 0);
            QCOMPARE(doc.layers[1].png, cmo_artwork_source_fixture::pixels()); QVERIFY(doc.layers[2].png.isEmpty());
            QCOMPARE(doc.layers[1].bounds, QRect(-3, 4, 2, 1)); QCOMPARE(doc.layers[1].opacity, quint8(127)); QVERIFY(!doc.layers[1].visible);
        }
    }
    void savedCmoRestoresAReadablePsdWithOriginalPixelsAndEmptyLayers()
    {
        QString error; const auto state = cmoFixture(cmo_artwork_source_fixture::archive(), &error);
        QVERIFY2(!state.IsEmpty(), qPrintable(error)); CPsdSourceCatalog catalog;
        QVERIFY2(Recovery::Prepare(state, catalog, &error), qPrintable(error)); QTemporaryDir directory;
        for (const auto& entry : catalog.documents) {
            const auto* source = entry.Reconstructed(); QVERIFY(source);
            const auto path = directory.filePath(source->id + ".psd");
            QVERIFY2(CPsdSourceWriter::Write(path, *source, nullptr, &error), qPrintable(error));
            CPsdSnapshot restored; QVERIFY2(CPsdReader::ReadFile(path, restored, &error), qPrintable(error));
            QCOMPARE(restored.canvasSize, source->canvasSize); QCOMPARE(restored.iccProfile, source->iccProfile);
            QCOMPARE(restored.displayOrder.size(), source->layers.size()); QFile file(path); QVERIFY(file.open(QIODevice::ReadOnly));
            PsdBinary::Document binary; QVERIFY2(PsdBinary::Parse(file.readAll(), binary, &error), qPrintable(error));
            for (int index = 0; index < source->layers.size(); ++index) {
                const auto& expected = source->layers[index]; const int record = restored.displayOrder[index];
                const auto& actual = restored.layers[record];
                QCOMPARE(actual.name, expected.name); QCOMPARE(actual.bounds, expected.bounds);
                QCOMPARE(actual.parentIndex, expected.parentIndex < 0 ? -1 : restored.displayOrder[expected.parentIndex]);
                QCOMPARE(actual.visible, expected.visible); QCOMPARE(actual.opacity, expected.opacity);
                QCOMPARE(actual.blendMode, expected.blendMode); QCOMPARE(actual.clipping, expected.clipping);
                QCOMPARE(actual.layerId, expected.externalId);
                if (!expected.png.isEmpty()) {
                    QImage original; QVERIFY(original.loadFromData(expected.png, "PNG"));
                    const auto expectedPixels = original.convertToFormat(QImage::Format_RGBA8888);
                    const auto actualPixels = actual.image.convertToFormat(QImage::Format_RGBA8888);
                    QCOMPARE(actualPixels.size(), expectedPixels.size());
                    for (int y = 0; y < expectedPixels.height(); ++y)
                        QCOMPARE(QByteArray(reinterpret_cast<const char*>(actualPixels.constScanLine(y)), actualPixels.width() * 4),
                                 QByteArray(reinterpret_cast<const char*>(expectedPixels.constScanLine(y)), expectedPixels.width() * 4));
                } else if (expected.kind == CPsdSourceLayer::Kind::Raster) {
                    QVERIFY(actual.image.isNull()); QCOMPARE(binary.layers[record].channels.size(), 4);
                    for (const auto& channel : binary.layers[record].channels) { QCOMPARE(channel.length, quint32(2)); QVERIFY(channel.pixels.isEmpty()); }
                }
            }
        }
    }
    void cmoMissingDocumentAndBrokenArchiveRemainVisibleAlongsideValidSources()
    {
        QString error; auto state = cmoFixture(cmo_artwork_source_fixture::archive(), &error); QVERIFY2(!state.IsEmpty(), qPrintable(error));
        auto missing = state.revisions.first(); missing.id = "missing-document"; missing.sourceDocumentId = "not-in-archive";
        state.revisions.insert(missing.id, missing);
        auto corrupt = missing; corrupt.id = "broken-archive"; corrupt.sourceBytes = "not a CMO"; corrupt.sourceDigest = digest(corrupt.sourceBytes);
        state.revisions.insert(corrupt.id, corrupt);
        CPsdSourceCatalog result; QVERIFY2(Recovery::Prepare(state, result, &error), qPrintable(error)); QCOMPARE(result.documents.size(), 4);
        for (const auto& id : {"missing-document", "broken-archive"}) {
            const auto* entry = revision(result, id); QVERIFY(entry && entry->Reconstructed());
            QCOMPARE(entry->diagnostics.size(), 1); QCOMPARE(entry->diagnostics[0].severity, CPsdSourceDiagnostic::Severity::Error);
        }
        const auto* valid = revision(result, "revision:d0-guid"); QVERIFY(valid && valid->diagnostics.isEmpty()); QVERIFY(error.isEmpty());
    }
    void cmoRecoveryFreezesInputAndKeepsPerSourceVersions()
    {
        QString error; auto state = cmoFixture(cmo_artwork_source_fixture::archive(1), &error); QVERIFY2(!state.IsEmpty(), qPrintable(error));
        const auto nextBytes = cmo_artwork_source_fixture::archive(1, "503000000");
        auto next = state.revisions.first(); next.id = "new"; next.sourceBytes = nextBytes; next.sourceDigest = digest(nextBytes); state.revisions.insert(next.id, next);
        state.families.begin()->currentRevision = next.id;
        CExportControl control; QVERIFY(control.SetHandler([&](const auto&, QString*) { state = {}; return true; }));
        CPsdSourceCatalog result; QVERIFY2(Recovery::Prepare(state, result, &error, &control), qPrintable(error));
        QCOMPARE(result.documents.size(), 2); QCOMPARE(revision(result, "new")->origin.sourceVersion, QString("503000000"));
        QCOMPARE(revision(result, "revision:d0-guid")->origin.sourceVersion, QString("502020000"));
        QVERIFY(revision(result, "new")->origin.currentRevision); QVERIFY(!revision(result, "revision:d0-guid")->origin.currentRevision);
        auto again = cmoFixture(nextBytes, &error); CExportControl canceled;
        QVERIFY(canceled.SetHandler([&](const CExportControl::Progress& progress, QString*) { if (progress.phase == CExportControl::Phase::Extracting) canceled.Cancel(); return true; }));
        result.sourcePath = "retained"; QVERIFY(!Recovery::Prepare(again, result, &error, &canceled)); QCOMPARE(result.sourcePath, QString("retained"));
    }
    void allRevisionsRemainDistinctAndCountsFollowPinnedMaterials()
    {
        const auto source = fixture(); QVERIFY(!source.IsEmpty()); QString error;
        QVERIFY2(Artwork::Validate(source, nullptr, &error), qPrintable(error));
        CPsdSourceCatalog catalog;
        QVERIFY2(Recovery::Prepare(source, catalog, &error), qPrintable(error));
        QCOMPARE(catalog.documents.size(), 4);
        const auto* old = revision(catalog, "old"); const auto* current = revision(catalog, "current");
        const auto* unused = revision(catalog, "unused"); const auto* policy = revision(catalog, "policy");
        QVERIFY(old && current && unused && policy);
        QVERIFY(!old->origin.currentRevision); QVERIFY(current->origin.currentRevision);
        QCOMPARE(old->origin.linkedMeshes, 2); QCOMPARE(old->origin.detachedMeshes, 1);
        QCOMPARE(current->origin.linkedMeshes, 1); QCOMPARE(current->origin.detachedMeshes, 0);
        QCOMPARE(unused->origin.linkedMeshes, 0); QCOMPARE(unused->origin.detachedMeshes, 0);
        QVERIFY(!old->Reconstructed()); QVERIFY(old->Original()->IsValid());
        QCOMPARE(old->Original()->Directory(), policy->Original()->Directory());
        QCOMPARE(old->Original()->Directory(), unused->Original()->Directory());
        QCOMPARE(old->Original()->Directory()->bytes, source.revisions["old"].sourceBytes);
        QCOMPARE(catalog.protectedPaths, QStringList{"/missing/original.psd"});
    }
    void publicLayerMetadataAndRegistrationCannotForgeTheOriginalDirectory()
    {
        auto state = fixture();
        for (auto& item : state.revisions) {
            item.layers[0].name = "伪造名称"; item.layers[0].visible = true; item.layers[0].sourceIndex = 91;
            item.layers[0].externalId = 999; item.documentToFamily = QTransform(2, 0, 0, 3, 55, -90);
        }
        QString error; QVERIFY2(Artwork::Validate(state, nullptr, &error), qPrintable(error));
        CPsdSourceCatalog catalog; QVERIFY2(Recovery::Prepare(state, catalog, &error), qPrintable(error));
        for (const auto& entry : catalog.documents) {
            const auto* source = entry.Original()->Directory(); QVERIFY(source);
            QCOMPARE(source->layers[0].name, QStringLiteral("原始图层😀")); QVERIFY(!source->layers[0].visible);
            QCOMPARE(source->layers[0].recordIndex, 0); QCOMPARE(source->layers[0].layerId, std::optional<quint32>(0xf1234567));
            QCOMPARE(source->layers[0].bounds, QRect(-1, -2, 2, 2));
        }
    }
    void malformedRevisionIsDisabledWithoutBlockingValidOriginals()
    {
        auto state = fixture();
        state.revisions["unused"].sourceBytes = "not a PSD";
        state.revisions["unused"].sourceDigest = digest("not a PSD");
        CPsdSourceCatalog catalog; QString error;
        QVERIFY2(Recovery::Prepare(state, catalog, &error), qPrintable(error));
        QCOMPARE(catalog.documents.size(), 4);
        const auto* bad = revision(catalog, "unused"); QVERIFY(bad && bad->Original()); QVERIFY(!bad->Original()->IsValid());
        QCOMPARE(bad->diagnostics.size(), 1); QCOMPARE(bad->diagnostics[0].severity, CPsdSourceDiagnostic::Severity::Error);
        QVERIFY(revision(catalog, "old")->Original()->IsValid()); QVERIFY(error.isEmpty());
    }
    void libraryPngPixelsCannotReplaceRawSourceBytes()
    {
        auto state = fixture();
        const auto bytes = state.revisions["old"].sourceBytes;
        QImage image(2, 2, QImage::Format_RGBA8888); image.fill(Qt::green);
        QByteArray png; QBuffer buffer(&png); QVERIFY(buffer.open(QIODevice::WriteOnly)); QVERIFY(image.save(&buffer, "PNG"));
        const auto imageDigest = digest(png);
        state.images.clear(); state.images.insert(imageDigest, {image.size(), png});
        for (auto& item : state.revisions) { item.layers[0].imageDigest = imageDigest; item.colorPolicy = Artwork::ColorPolicy::ConvertToSRgb; }
        CPsdSourceCatalog result; QString error;
        QVERIFY2(Recovery::Prepare(state, result, &error), qPrintable(error));
        for (const auto& entry : result.documents) QCOMPARE(entry.Original()->Directory()->bytes, bytes);
    }
    void unsupportedEffectsArePreservedRatherThanPromotedToIntegrityErrors()
    {
        auto state = fixture(); const auto bytes = psd_original_fixture::fixture(3, true);
        for (auto& item : state.revisions) { item.sourceBytes = bytes; item.sourceDigest = digest(bytes); }
        CPsdSourceCatalog catalog; QString error; QVERIFY2(Recovery::Prepare(state, catalog, &error), qPrintable(error));
        for (const auto& entry : catalog.documents) {
            QVERIFY(entry.Original()->IsValid()); QVERIFY(entry.diagnostics.isEmpty());
            QCOMPARE(entry.Original()->Directory()->bytes, bytes);
            QVERIFY(!entry.Original()->Directory()->layers[0].diagnostics.isEmpty());
        }
    }
    void frozenStateSurvivesCallerMutationAndMissingSourcePaths()
    {
        auto state = fixture(); const auto originalBytes = state.revisions["old"].sourceBytes;
        CExportControl control; bool called = false;
        QVERIFY(control.SetHandler([&](const auto&, QString*) { called = true; state = {}; return true; }));
        CPsdSourceCatalog catalog; QString error;
        QVERIFY2(Recovery::Prepare(state, catalog, &error, &control), qPrintable(error)); QVERIFY(called);
        QTemporaryDir dir; const auto output = dir.filePath("restored.psd");
        QVERIFY2(revision(catalog, "old")->Original()->Write(output, nullptr, &error), qPrintable(error));
        QFile file(output); QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), originalBytes);
    }
    void globalCancellationAndInvalidStateDoNotReplaceCatalog()
    {
        const auto state = fixture(); CPsdSourceCatalog result; result.sourcePath = "retained";
        CExportControl control; QVERIFY(control.SetHandler([&](const auto&, QString*) { control.Cancel(); return true; }));
        QString error; QVERIFY(!Recovery::Prepare(state, result, &error, &control)); QVERIFY(control.WasCanceled());
        QCOMPARE(result.sourcePath, QString("retained"));
        auto invalid = state; invalid.materials["material-old"].revisionId = "missing";
        QVERIFY(!Recovery::Prepare(invalid, result, &error)); QCOMPARE(result.sourcePath, QString("retained"));
    }
    void pngFamiliesAreNotPresentedAsOriginalPsd()
    {
        auto state = fixture(); state.families["family"].kind = Artwork::SourceKind::Png;
        CPsdSourceCatalog result; QString error; QVERIFY2(Recovery::Prepare(state, result, &error), qPrintable(error));
        QVERIFY(result.documents.isEmpty());
    }
    void authoringArchiveRelocationDoesNotChangeRecoverableBytes()
    {
        const auto state = fixture(); QJsonObject json; QString error;
        QVERIFY(Artwork::ToJson(state, json, &error));
        auto revisions = json["revisions"].toObject();
        for (auto it = revisions.begin(); it != revisions.end(); ++it) { auto value = it.value().toObject(); value["path"] = "/moved/model/authoring/artwork/objects/" + value["sourceDigest"].toString() + ".psd"; it.value() = value; }
        json["revisions"] = revisions;
        Artwork::State restored; QVERIFY2(Artwork::FromJson(json, restored, &error), qPrintable(error));
        CPsdSourceCatalog catalog; QVERIFY2(Recovery::Prepare(restored, catalog, &error), qPrintable(error));
        QCOMPARE(revision(catalog, "old")->Original()->Directory()->bytes, state.revisions["old"].sourceBytes);
        QVERIFY(revision(catalog, "old")->origin.sourcePath.startsWith("/moved/"));
    }
};
QTEST_GUILESS_MAIN(ArtworkPsdRecoveryTests)
#include "artwork_psd_recovery_tests.moc"
