#include "live2dstruct/cartworkpsdrecovery.h"
#include "live2dstruct/cartworksourceimport.h"
#include "psd/cpsdreader.h"
#include "std/cexportcontrol.h"
#include "psd_original_fixture.h"
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
}

class ArtworkPsdRecoveryTests : public QObject
{
    Q_OBJECT
private slots:
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
