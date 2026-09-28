#include "live2dstruct/cartworksourceimport.h"
#include "live2dstruct/cpsdlayermatcher.h"
#include "live2dstruct/cartworksampler.h"
#include "psd/cpsdreader.h"
#include "std/cpngimage.h"
#include "std/cexportcontrol.h"
#include <QtTest>
#include <QColorSpace>
#include <QtEndian>
using namespace live2dstruct;
namespace {
QByteArray u16(quint16 value) { char data[2]; qToBigEndian(value, data); return QByteArray(data, 2); }
QByteArray u32(quint32 value) { char data[4]; qToBigEndian(value, data); return QByteArray(data, 4); }
QByteArray block(const QByteArray& value) { return u32(value.size()) + value; }
QByteArray tag(const QByteArray& key, const QByteArray& value) { return "8BIM" + key + block(value); }
struct Layer { QString name = "Paint"; int id = 1, kind = 0; bool pixels = true, visible = true; QByteArray extra; quint8 opacity = 255; bool mask = false, extraChannel = false; };
QByteArray psd(const QVector<Layer>& layers = {Layer()}, const QByteArray& icc = {}) {
    QByteArray records, data;
    for (const auto& layer : layers) {
        const bool empty = layer.kind != 0;
        records += u32(empty ? 0 : -5) + u32(empty ? 0 : -3) + u32(empty ? 0 : -3) + u32(empty ? 0 : -1);
        const int count = empty || !layer.pixels ? 0 : 4 + int(layer.mask) + int(layer.extraChannel); records += u16(count);
        const QVector<QByteArray> planes{QByteArray::fromHex("804020ff"), QByteArray::fromHex("408020ff"), QByteArray::fromHex("208040ff"), QByteArray::fromHex("004080ff")};
        for (int c = 0; c < count; ++c) {
            const int channel = c < 3 ? c : c == 3 ? -1 : c == 4 && layer.mask ? -2 : 4;
            records += u16(channel) + u32(6); data += u16(0) + (c < 4 ? planes[c] : QByteArray(4, 0));
        }
        records += "8BIMnorm"; records += char(layer.opacity); records += char(0); records += char(layer.visible ? 0 : 2); records += char(0);
        QByteArray name = u32(layer.name.size()); for (QChar c : layer.name) name += u16(c.unicode());
        const auto mask = layer.mask ? u32(-5) + u32(-3) + u32(-3) + u32(-1) + QByteArray(4, 0) : QByteArray();
        QByteArray extra = block(mask) + u32(0) + QByteArray(4, 0) + tag("luni", name);
        if (layer.id >= 0) extra += tag("lyid", u32(layer.id));
        if (layer.kind) extra += tag("lsct", u32(layer.kind));
        records += block(extra + layer.extra);
    }
    QByteArray info = u16(layers.size()) + records + data; if (info.size() % 2) info += char(0);
    QByteArray resources;
    if (!icc.isEmpty()) resources = QByteArray("8BIM") + u16(1039) + QByteArray(2, 0) + block(icc) + QByteArray(icc.size() % 2, 0);
    return QByteArray("8BPS") + u16(1) + QByteArray(6, 0) + u16(3) + u32(2) + u32(2) + u16(8) + u16(3) +
        u32(0) + block(resources) + block(block(info) + u32(0)) + u16(0) + QByteArray(12, 0);
}
CArtworkSourceImport::SourceCandidate prepare(const QByteArray& bytes, CModelArtwork::ColorPolicy policy = CModelArtwork::ColorPolicy::PreserveValues) {
    CPsdSnapshot snapshot; snapshot.bytes = bytes; snapshot.canonicalPath = "/missing/original.psd";
    CArtworkSourceImport::SourceCandidate result; QString error;
    if (!CArtworkSourceImport::Prepare(snapshot, "family", "revision", policy, QTransform(), result, &error)) qFatal("%s", qPrintable(error));
    return result;
}
QImage image(const CArtworkSourceImport::SourceCandidate& source, int index = 0) {
    QImage result; CPngImage::Decode(source.images.value(source.revision.layers[index].imageDigest).png, result); return result;
}
}
class ArtworkSourceImportTests : public QObject {
    Q_OBJECT
private slots:
    void invalidSealedDocumentDoesNotReplaceCandidateOrInvokeCallbacks() {
        const auto before = prepare(psd()); auto result = before; CPsdReadDocument source;
        CExportControl control; bool called = false; QString error;
        QVERIFY(control.SetHandler([&](const auto&, QString*) { called = true; return true; }));
        QVERIFY(!CArtworkSourceImport::Prepare(source, "family", "next", CModelArtwork::ColorPolicy::PreserveValues,
                                               {}, result, &error, &control));
        QVERIFY(!error.isEmpty()); QVERIFY(!called); QVERIFY(result.revision == before.revision); QCOMPARE(result.images, before.images);
    }
    void sealedDocumentOwnsIdentityDuringCallbackReplacement() {
        CPsdReadDocument source, replacement; QString error;
        const auto bytes = psd();
        QVERIFY(CPsdReader::OpenBytes(bytes, source, &error));
        QVERIFY(CPsdReader::OpenBytes(psd({Layer{"Other"}}), replacement, &error));
        QChar familyData[]{'f', 'a', 'm'}, revisionData[]{'r', 'e', 'v'};
        const auto family = QString::fromRawData(familyData, 3), revision = QString::fromRawData(revisionData, 3);
        QTransform mapping; mapping.translate(13, -7); const auto expectedMapping = mapping;
        CExportControl control; bool replaced = false;
        QVERIFY(control.SetHandler([&](const auto&, QString*) {
            if (!replaced) { replaced = true; source = replacement; familyData[0] = 'X'; revisionData[0] = 'Y'; mapping.reset(); }
            return true;
        }));
        CArtworkSourceImport::SourceCandidate result;
        QVERIFY2(CArtworkSourceImport::Prepare(source, family, revision, CModelArtwork::ColorPolicy::PreserveValues,
                                              mapping, result, &error, &control), qPrintable(error));
        QVERIFY(replaced); QCOMPARE(result.revision.familyId, QString("fam")); QCOMPARE(result.revision.id, QString("rev"));
        QCOMPARE(result.revision.sourceBytes, bytes); QCOMPARE(result.revision.documentToFamily, expectedMapping);
        QCOMPARE(result.revision.layers[0].name, QString("Paint")); QCOMPARE(image(result).pixel(1, 1), qRgba(255, 255, 255, 255));
    }
    void sealedFileAndLegacySnapshotKeepCanonicalIdentityAfterRemoval() {
        QTemporaryDir folder; QVERIFY(folder.isValid()); const auto path = folder.filePath("source.psd");
        const auto bytes = psd(); QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write(bytes), bytes.size()); file.close();
        CPsdReadDocument source; QString error; QVERIFY(CPsdReader::OpenFile(path, source, &error));
        QVERIFY(source.Directory()); const auto canonical = source.Directory()->canonicalPath; QVERIFY(!canonical.isEmpty());
        QVERIFY(QFile::remove(path));
        CArtworkSourceImport::SourceCandidate sealed, legacy;
        QVERIFY(CArtworkSourceImport::Prepare(source, "family", "revision", CModelArtwork::ColorPolicy::PreserveValues, {}, sealed, &error));
        CPsdSnapshot snapshot; snapshot.bytes = bytes; snapshot.canonicalPath = canonical;
        QVERIFY(CArtworkSourceImport::Prepare(snapshot, "family", "revision", CModelArtwork::ColorPolicy::PreserveValues, {}, legacy, &error));
        QVERIFY(sealed.revision == legacy.revision); QCOMPARE(sealed.images, legacy.images);
        QCOMPARE(sealed.revision.path, canonical); QCOMPARE(sealed.revision.fileName, QString("source.psd"));
    }
    void canceledStreamingImportDoesNotPublishPartialLayers() {
        CPsdReadDocument source; QString error;
        QVERIFY(CPsdReader::OpenBytes(psd({Layer{"first", 1}, Layer{"second", 2}}), source, &error));
        const auto before = prepare(psd()); auto result = before;
        CExportControl control; bool canceledBetweenLayers = false;
        QVERIFY(control.SetHandler([&](const CExportControl::Progress& progress, QString*) {
            if (progress.phase == CExportControl::Phase::Preparing && progress.current == 1 &&
                progress.total == 2 && progress.detail == QStringLiteral("first")) {
                canceledBetweenLayers = true; control.Cancel();
            }
            return true;
        }));
        QVERIFY(!CArtworkSourceImport::Prepare(source, "family", "next", CModelArtwork::ColorPolicy::PreserveValues, {}, result, &error, &control));
        QVERIFY(canceledBetweenLayers); QVERIFY(control.WasCanceled()); QVERIFY(result.revision == before.revision); QCOMPARE(result.images, before.images);
    }
    void streamingPreservesUnappliedMaskAndExtraChannelPixels_data() {
        QTest::addColumn<bool>("mask"); QTest::addColumn<bool>("extra");
        QTest::newRow("mask") << true << false;
        QTest::newRow("extra-channel") << false << true;
        QTest::newRow("both") << true << true;
    }
    void streamingPreservesUnappliedMaskAndExtraChannelPixels() {
        QFETCH(bool, mask); QFETCH(bool, extra); Layer layer; layer.mask = mask; layer.extraChannel = extra;
        CPsdReadDocument source; QString error; QVERIFY2(CPsdReader::OpenBytes(psd({layer}), source, &error), qPrintable(error));
        CArtworkSourceImport::SourceCandidate result;
        QVERIFY2(CArtworkSourceImport::Prepare(source, "family", "revision", CModelArtwork::ColorPolicy::PreserveValues, {}, result, &error), qPrintable(error));
        QVERIFY(!result.revision.layers[0].pixelEquivalent); QVERIFY(!result.revision.layers[0].imageDigest.isEmpty());
        QCOMPARE(image(result), image(prepare(psd())));
        const auto diagnostics = result.revision.layers[0].diagnostics.join('\n');
        if (mask) QVERIFY(diagnostics.contains("mask:"));
        QVERIFY(diagnostics.contains("extra-channel:"));
    }
    void colorRecipeCanReuseSealedOriginalAndReturnToExactPreservedPixels() {
        CPsdReadDocument source; QString error;
        QVERIFY(CPsdReader::OpenBytes(psd({Layer()}, QColorSpace(QColorSpace::SRgbLinear).iccProfile()), source, &error));
        CArtworkSourceImport::SourceCandidate preserved, converted, restored;
        QVERIFY(CArtworkSourceImport::Prepare(source, "family", "preserve", CModelArtwork::ColorPolicy::PreserveValues, {}, preserved, &error));
        QVERIFY(CArtworkSourceImport::Prepare(source, "family", "convert", CModelArtwork::ColorPolicy::ConvertToSRgb, {}, converted, &error));
        QVERIFY(CArtworkSourceImport::Prepare(source, "family", "restore", CModelArtwork::ColorPolicy::PreserveValues, {}, restored, &error));
        QCOMPARE(preserved.images, restored.images); QVERIFY(preserved.images != converted.images);
        QCOMPARE(preserved.revision.sourceBytes, converted.revision.sourceBytes); QCOMPARE(preserved.revision.iccProfile, converted.revision.iccProfile);
        for (int y = 0; y < 2; ++y) for (int x = 0; x < 2; ++x)
            QCOMPARE(qAlpha(image(preserved).pixel(x, y)), qAlpha(image(converted).pixel(x, y)));
    }
    void sourceBytesAreAuthoritativeAndOwned() {
        QByteArray bytes = psd(); const QByteArray expected(bytes.constData(), bytes.size());
        CPsdSnapshot snapshot; snapshot.bytes = QByteArray::fromRawData(bytes.constData(), bytes.size()); snapshot.canonicalPath = "/hint/original.psd";
        snapshot.sha256 = "invented"; snapshot.layers.append(CPsdLayer()); snapshot.layers[0].name = "Invented";
        CExportControl control; bool called = false;
        QVERIFY(control.SetHandler([&](const auto&, QString*) { called = true; bytes.fill('x'); snapshot.canonicalPath = "/changed.psd"; return true; }));
        CArtworkSourceImport::SourceCandidate result; QString error;
        QVERIFY2(CArtworkSourceImport::Prepare(snapshot, "source", "revision", CModelArtwork::ColorPolicy::PreserveValues, {}, result, &error, &control), qPrintable(error));
        QVERIFY(called); QCOMPARE(result.revision.sourceBytes, expected); QCOMPARE(result.revision.path, QString("/hint/original.psd"));
        QCOMPARE(result.revision.fileName, QString("original.psd")); QCOMPARE(result.revision.layers[0].name, QString("Paint"));
        QCOMPARE(result.revision.layers[0].id, QString("record:0")); QCOMPARE(result.revision.layers[0].sourceIndex, 0);
        QCOMPARE(image(result).pixel(0, 0), qRgba(128, 64, 32, 0));
    }
    void displayOrderRemapsParentsAndReusesPixelPngs() {
        Layer end{"end", -1, 3}, first{"first", 1}, second{"second", 2}, group{"group", 3, 1}; second.visible = false;
        const auto result = prepare(psd({end, first, second, group})); const auto& layers = result.revision.layers;
        QCOMPARE(layers.size(), 3); QCOMPARE(layers[0].kind, CModelArtwork::LayerKind::Group); QCOMPARE(layers[0].sourceIndex, 3);
        QCOMPARE(layers[1].parentIndex, 0); QCOMPARE(layers[2].parentIndex, 0); QCOMPARE(layers[1].sourceIndex, 2);
        QCOMPARE(layers[1].bounds, QRect(-3, -5, 2, 2)); QVERIFY(!layers[1].visible);
        QCOMPARE(layers[1].imageDigest, layers[2].imageDigest); QCOMPARE(result.images.size(), 1);
    }
    void unsupportedLayersStayInLibraryWithoutFabricatedPixels() {
        Layer unsupported; unsupported.pixels = false; unsupported.extra = tag("SoLd", "opaque");
        const auto result = prepare(psd({unsupported})); QCOMPARE(result.revision.layers.size(), 1);
        QVERIFY(!result.revision.layers[0].pixelEquivalent); QVERIFY(result.revision.layers[0].imageDigest.isEmpty()); QVERIFY(result.images.isEmpty());
        QVERIFY(result.revision.layers[0].diagnostics.join('\n').contains("missing-rgb"));
        QVERIFY(result.revision.layers[0].diagnostics.join('\n').contains("unapplied-tag"));
    }
    void explicitColorConversionPreservesAlphaAndOriginalProfile() {
        const auto icc = QColorSpace(QColorSpace::SRgbLinear).iccProfile(); QVERIFY(!icc.isEmpty());
        const auto bytes = psd({Layer()}, icc);
        const auto preserved = prepare(bytes); const auto converted = prepare(bytes, CModelArtwork::ColorPolicy::ConvertToSRgb);
        QCOMPARE(preserved.revision.iccProfile, icc); QCOMPARE(converted.revision.iccProfile, icc);
        QCOMPARE(preserved.revision.sourceBytes, converted.revision.sourceBytes);
        const auto first = image(preserved), next = image(converted); QCOMPARE(first.pixel(0, 0), qRgba(128, 64, 32, 0));
        QVERIFY(qAbs(qRed(next.pixel(0, 0)) - 188) <= 1); QCOMPARE(qAlpha(next.pixel(0, 0)), 0);
        for (int y = 0; y < 2; ++y) for (int x = 0; x < 2; ++x) QCOMPARE(qAlpha(next.pixel(x, y)), qAlpha(first.pixel(x, y)));
        QVERIFY(preserved.revision.layers[0].imageDigest != converted.revision.layers[0].imageDigest);
        const auto untagged = prepare(psd(), CModelArtwork::ColorPolicy::ConvertToSRgb);
        QCOMPARE(image(untagged), image(prepare(psd()))); QVERIFY(untagged.diagnostics.join('\n').contains("missing-icc"));
    }
    void invalidIccConversionAndCancellationAreAtomic() {
        CPsdSnapshot snapshot; snapshot.bytes = psd({Layer()}, "invalid-profile");
        const auto original = prepare(psd()); auto result = original; QString error;
        QVERIFY(!CArtworkSourceImport::Prepare(snapshot, "source", "new", CModelArtwork::ColorPolicy::ConvertToSRgb, {}, result, &error));
        QVERIFY(error.contains("ICC")); QVERIFY(result.revision == original.revision); QCOMPARE(result.images, original.images);
        QVERIFY(CArtworkSourceImport::Prepare(snapshot, "source", "new", CModelArtwork::ColorPolicy::PreserveValues, {}, result, &error));
        QVERIFY(result.diagnostics.join('\n').contains("invalid-icc"));
        CExportControl control; control.SetHandler([&](const auto&, QString*) { control.Cancel(); return true; }); result = original;
        QVERIFY(!CArtworkSourceImport::Prepare(snapshot, "source", "new", CModelArtwork::ColorPolicy::PreserveValues, {}, result, &error, &control));
        QVERIFY(control.WasCanceled()); QVERIFY(result.revision == original.revision);
    }
    void duplicateSourceIdsPreserveSupportedPixelsButRequireManualMatching() {
        Layer first{"Left", 7}, second{"Right", 7};
        const auto imported = prepare(psd({first, second}));
        for (const auto& layer : imported.revision.layers) {
            QVERIFY(layer.pixelEquivalent); QVERIFY(!layer.imageDigest.isEmpty()); QVERIFY(layer.diagnostics.join('\n').contains("duplicate-layer-id"));
        }
        auto changed = imported.revision; changed.id = "new-revision";
        CPsdLayerMatcher::Result result; QString error; QVERIFY(CPsdLayerMatcher::Compare(imported.revision, changed, result, &error));
        for (const auto& match : result.matches) {
            QCOMPARE(match.status, CPsdLayerMatcher::Status::NeedsConfirmation); QVERIFY(match.matchedLayerId.isEmpty()); QCOMPARE(match.candidates.size(), 2);
        }
        CModelArtwork::State state; state.images = imported.images;
        state.revisions.insert(imported.revision.id, imported.revision);
        state.families.insert("family", {"family", "Source", CModelArtwork::SourceKind::Psd, imported.revision.id});
        CModelArtwork::Material material; material.id = "selected-first"; material.name = "Manual association";
        material.familyId = "family"; material.revisionId = imported.revision.id; material.layerId = imported.revision.layers[0].id;
        state.materials.insert(material.id, material); material.id = "selected-second"; state.materials.insert(material.id, material);
        QVERIFY2(CModelArtwork::Validate(state, nullptr, &error), qPrintable(error));
        state.revisions[imported.revision.id].layers[0].pixelEquivalent = false;
        QVERIFY(!CModelArtwork::Validate(state, nullptr, &error));
    }
    void documentRegistrationTransformIsExplicitAndValidated() {
        CPsdSnapshot snapshot; snapshot.bytes = psd(); CArtworkSourceImport::SourceCandidate result; QString error;
        QTransform registration; registration.translate(17, -13); registration.scale(2, 3);
        QVERIFY(CArtworkSourceImport::Prepare(snapshot, "source", "revision", CModelArtwork::ColorPolicy::PreserveValues, registration, result, &error));
        QCOMPARE(result.revision.documentToFamily, registration); QCOMPARE(result.revision.layers[0].bounds, QRect(-3, -5, 2, 2));
        const auto baseline = result.revision; QTransform singular; singular.scale(0, 1);
        QVERIFY(!CArtworkSourceImport::Prepare(snapshot, "source", "new", CModelArtwork::ColorPolicy::PreserveValues, singular, result, &error));
        QVERIFY(result.revision == baseline);
    }
    void opacityMetadataBecomesMaterialSamplingWithoutAlteringSourcePixels() {
        Layer input; input.opacity = 128; const auto imported = prepare(psd({input}));
        const auto& layer = imported.revision.layers[0]; QVERIFY(layer.pixelEquivalent); QCOMPARE(layer.opacity, quint8(128));
        QVERIFY(layer.diagnostics.join('\n').contains("opacity:"));
        CModelArtwork::State state; state.images = imported.images; state.revisions.insert(imported.revision.id, imported.revision);
        state.families.insert("family", {"family", "Source", CModelArtwork::SourceKind::Psd, imported.revision.id});
        CModelArtwork::Material material; material.id = "material"; material.name = "Layer"; material.familyId = "family";
        material.revisionId = imported.revision.id; material.layerId = layer.id; state.materials.insert(material.id, material);
        QString error; QVERIFY2(CModelArtwork::Validate(state, nullptr, &error), qPrintable(error));
        const auto source = image(imported); QCOMPARE(qAlpha(source.pixel(1, 1)), 255); QImage sampled;
        QVERIFY(CArtworkSampler::Render(source, {}, source.size(), material.boundary, CModelArtwork::Filter::ExactTexels,
            material.applyLayerOpacity ? layer.opacity : 255, sampled, &error));
        QCOMPARE(qAlpha(sampled.pixel(1, 1)), 128); QCOMPARE(qAlpha(sampled.pixel(1, 0)), 32); QCOMPARE(qAlpha(source.pixel(1, 1)), 255);
        material.applyLayerOpacity = false;
        QVERIFY(CArtworkSampler::Render(source, {}, source.size(), material.boundary, CModelArtwork::Filter::ExactTexels,
            material.applyLayerOpacity ? layer.opacity : 255, sampled, &error)); QCOMPARE(sampled, source);
        state.revisions[imported.revision.id].layers[0].pixelEquivalent = false;
        QVERIFY(!CModelArtwork::Validate(state, nullptr, &error));
    }
};
QTEST_GUILESS_MAIN(ArtworkSourceImportTests)
#include "artwork_source_import_tests.moc"
