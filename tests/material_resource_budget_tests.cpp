#include "live2dstruct/cmaterialresourcebudget.h"
#include "live2dstruct/cmaterialvalidationcache.h"
#include <QtTest>
#include <QBuffer>
#include <QCryptographicHash>
#include <QImage>
#include <QColor>
#include <future>
using namespace live2dstruct;
namespace {
using Budget = CMaterialResourceBudget; using Artwork = CModelArtwork;
QByteArray png(QColor color = Qt::red, QSize size = QSize(2, 2)) {
    QImage image(size, QImage::Format_RGBA8888); image.fill(color); QBuffer buffer; buffer.open(QIODevice::WriteOnly); image.save(&buffer, "PNG"); return buffer.data();
}
QByteArray hash(const QByteArray& bytes) { return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256); }
CTextureAssets::Catalog catalog(const QByteArray& bytes) {
    CTextureAssets::Source source; CTextureAssets::MakeSource("/tmp/material-budget-source.png", bytes, source); return {{0, source}};
}
CModelAtlasState snapshot(const CTextureAssets::Catalog& sources) {
    CModelAtlasState result; CTextureWorkspace::Initialize(sources, {}, result.workspace); return result;
}
Artwork::State artwork(const QByteArray& bytes, bool pixels = true, QSize size = QSize(2, 2)) {
    Artwork::State result; const auto digest = hash(bytes);
    if (pixels) result.images.insert(digest, {size, bytes});
    result.families.insert("family", {"family", "Source", Artwork::SourceKind::Png, "revision"});
    Artwork::Revision revision; revision.id = "revision"; revision.familyId = "family";
    revision.sourceBytes = bytes; revision.sourceDigest = digest; revision.canvasSize = size;
    Artwork::Layer layer; layer.id = "layer"; layer.bounds = QRect(QPoint(), size); layer.pixelEquivalent = pixels;
    if (pixels) layer.imageDigest = digest; else layer.diagnostics = {"unsupported: no independent pixel layer"};
    revision.layers = {layer}; result.revisions.insert(revision.id, revision); return result;
}
qint64 catalogMetadata(const CTextureAssets::Catalog& sources) {
    if (sources.isEmpty()) return 0;
    qint64 result = 64 + sources.size() * qint64(sizeof(int) + sizeof(CTextureAssets::Source) + 4 * sizeof(void*));
    for (const auto& source : sources) result += 64 + (source.path.size() + 1) * 2 + 64 + 32;
    return result;
}
qint64 workspaceMetadata(const CTextureWorkspace::State& value) {
    qint64 result = 64 + 32 + 64 + value.atlases.size() * qint64(sizeof(int) + sizeof(CTextureWorkspace::Atlas) + 4 * sizeof(void*));
    for (const auto& atlas : value.atlases) result += 64 + (atlas.id.size() + 1) * 2 + 64 + (atlas.name.size() + 1) * 2;
    return result;
}
}
class MaterialResourceBudgetTests : public QObject {
    Q_OBJECT
private slots:
    void exactByteAndPixelBoundariesIncludeOriginalSources() {
        const auto bytes = png(); const auto sources = catalog(bytes); const auto value = snapshot(sources); QString error; Budget::Usage usage;
        const qint64 metadata = catalogMetadata(sources) + workspaceMetadata(value.workspace);
        const Budget::Limits exact{bytes.size() + metadata, 16}; QVERIFY2(Budget::Validate(sources, {value}, exact, &usage, &error), qPrintable(error));
        QCOMPARE(usage.encodedBytes, qint64(bytes.size())); QCOMPARE(usage.decodedBytes, qint64(16)); QCOMPARE(usage.uniqueBlobs, 1); QCOMPARE(usage.uniqueImages, 1);
        QCOMPARE(usage.metadataBytes, metadata);
        usage.encodedBytes = 12345;
        QVERIFY(!Budget::Validate(sources, {value}, Budget::Limits{exact.maxStorageBytes - 1, 16}, &usage, &error)); QCOMPARE(usage.encodedBytes, qint64(12345));
        QVERIFY(!Budget::Validate(sources, {value}, Budget::Limits{exact.maxStorageBytes, 15}, &usage, &error)); QCOMPARE(usage.encodedBytes, qint64(12345));
        QVERIFY(!Budget::Validate(sources, {value}, Budget::Limits{exact.maxStorageBytes, 12}, &usage, &error)); QCOMPARE(usage.encodedBytes, qint64(12345));
    }
    void currentUndoAndRedoUseOneContentPool() {
        const auto first = png(Qt::red), second = png(Qt::blue); const auto sources = catalog(first);
        auto before = snapshot(sources), after = before; after.workspace.atlases[0].png = second;
        Budget::Usage usage; QString error;
        const qint64 storage = first.size() + second.size() + catalogMetadata(sources) + workspaceMetadata(before.workspace) + workspaceMetadata(after.workspace);
        QVERIFY2(Budget::Validate(sources, {before, after, before, after}, {storage, 32}, &usage, &error), qPrintable(error));
        QCOMPARE(usage.encodedBytes, qint64(first.size() + second.size())); QCOMPARE(usage.decodedBytes, qint64(32)); QCOMPARE(usage.uniqueImages, 2);
        QCOMPARE(usage.StorageBytes(), storage);
        QVERIFY(!Budget::Validate(sources, {before, after}, {storage, 31}, nullptr, &error));
    }
    void sourceBytesAndImageRolesHaveIndependentDeduplication() {
        const auto bytes = png(); CModelAtlasState raw, image; raw.artwork = artwork(bytes, false); image.artwork = artwork(bytes, true);
        Budget::Usage usage; QString error; QVERIFY2(Budget::Validate({}, {raw, image}, &usage, &error), qPrintable(error));
        QCOMPARE(usage.encodedBytes, qint64(bytes.size())); QCOMPARE(usage.decodedBytes, qint64(16)); QCOMPARE(usage.uniqueImages, 1);
        Artwork::Usage a, b; QVERIFY(Artwork::Validate(raw.artwork, &a)); QVERIFY(Artwork::Validate(image.artwork, &b));
        QCOMPARE(usage.metadataBytes, a.metadataBytes + b.metadataBytes);
        QVERIFY(Budget::Validate({}, {raw, image, raw, image}, {usage.StorageBytes(), 16}, nullptr, &error));
        QVERIFY(!Budget::Validate({}, {raw, image}, {usage.StorageBytes() - 1, 16}, nullptr, &error));
        QVERIFY(!Budget::Validate({}, {raw, image}, {usage.StorageBytes(), 15}, nullptr, &error));
    }
    void atlasAndArtworkSharePixelsButDistinctMetadataCostsBudget() {
        const auto bytes = png(); const auto sources = catalog(bytes); auto first = snapshot(sources); first.artwork = artwork(bytes);
        auto changed = first; changed.artwork.families["family"].name = "Renamed";
        Budget::Usage base, history; QString error;
        QVERIFY(Budget::Validate(sources, {first, first}, &base, &error)); QVERIFY(Budget::Validate(sources, {first, changed}, &history, &error));
        QCOMPARE(base.encodedBytes, history.encodedBytes); QCOMPARE(history.uniqueImages, 1); QCOMPARE(history.decodedBytes, qint64(16));
        QVERIFY(history.metadataBytes > base.metadataBytes);
        QVERIFY(!Budget::Validate(sources, {first, changed}, {base.StorageBytes(), 16}, nullptr, &error));
    }
    void uvAndAtlasRenameHistoryConsumeMetadataEvenWhenAllPixelsAreShared() {
        const auto bytes = png(); const auto sources = catalog(bytes); auto base = snapshot(sources);
        base.materials = {{"mesh", 0, {0, 0, 1, 0, 0, 1}}};
        auto uv = base; uv.materials[0].uvs[0] = .25f;
        auto renamed = base; renamed.workspace.atlases[0].name = "Renamed";
        const qint64 materials = 64 + sizeof(CModelDrawableMaterial) + 64 + 10 + 64 + 6 * sizeof(float);
        const qint64 baseCost = bytes.size() + catalogMetadata(sources) + workspaceMetadata(base.workspace) + materials;
        QString error; Budget::Usage usage;
        QVERIFY2(Budget::Validate(sources, {base, base}, {baseCost, 16}, &usage, &error), qPrintable(error)); QCOMPARE(usage.StorageBytes(), baseCost);
        QVERIFY(!Budget::Validate(sources, {base, uv}, {baseCost + materials - 1, 16}, nullptr, &error));
        QVERIFY(!Budget::Validate(sources, {base, renamed}, {baseCost + workspaceMetadata(renamed.workspace) - 1, 16}, nullptr, &error));
        const qint64 complete = baseCost + materials + workspaceMetadata(renamed.workspace);
        QVERIFY2(Budget::Validate(sources, {base, uv, renamed, base, uv, renamed}, {complete, 16}, &usage, &error), qPrintable(error));
        QCOMPARE(usage.StorageBytes(), complete); QCOMPARE(usage.uniqueImages, 1); QCOMPARE(usage.uniqueBlobs, 1); QCOMPARE(usage.decodedBytes, qint64(16));
    }
    void cacheOwnsVerifiedBytesAndNeverTrustsBorrowedPointerIdentity() {
        QByteArray bytes = png(); const QByteArray original(bytes.constData(), bytes.size()); auto borrowed = QByteArray::fromRawData(bytes.constData(), bytes.size());
        CMaterialValidationCache cache; QByteArray digest; QString error;
        QVERIFY(cache.ValidatePng(borrowed, {2, 2}, digest, &error)); const auto expected = digest; QCOMPARE(cache.PngDecodeAttempts(), 1);
        const auto hashed = cache.HashedBytes();
        QVERIFY(cache.ValidatePng(borrowed, {2, 2}, digest, &error)); QCOMPARE(cache.PngDecodeAttempts(), 1); QCOMPARE(cache.HashedBytes(), hashed);
        bytes[0] = 'x'; digest = "sentinel";
        QVERIFY(!cache.ValidatePng(borrowed, {2, 2}, digest, &error)); QCOMPARE(digest, QByteArray("sentinel"));
        QVERIFY(!cache.ValidatePng(borrowed, {2, 2}, digest, &error, expected)); QCOMPARE(digest, QByteArray("sentinel"));
        QVERIFY(cache.ValidatePng(original, {2, 2}, digest, &error, expected)); QCOMPARE(digest, expected); QCOMPARE(cache.PngDecodeAttempts(), 1);
        QVERIFY(!cache.ValidatePng(original, {1, 4}, digest, &error, expected)); QCOMPARE(digest, expected);
    }
    void blobOnlyValidationCannotSkipLaterPngDecode() {
        const auto bytes = png(); CMaterialValidationCache cache; QByteArray digest; QString error;
        QVERIFY(cache.ValidateBlob(bytes, digest, &error)); QCOMPARE(cache.PngDecodeAttempts(), 0);
        QVERIFY(cache.ValidatePng(bytes, {2, 2}, digest, &error)); QCOMPARE(cache.PngDecodeAttempts(), 1);
        const auto truncated = bytes.left(33); QVERIFY(cache.ValidateBlob(truncated, digest, &error)); const auto rawDigest = digest;
        QVERIFY(!cache.ValidatePng(truncated, {2, 2}, digest, &error, rawDigest)); QCOMPARE(digest, rawDigest);
        QVERIFY(!cache.ValidatePng(truncated, {2, 2}, digest, &error, rawDigest));
    }
    void sharedFullValidationCacheDecodesRepeatedHistoryImagesOnce() {
        const auto bytes = png(Qt::green, {256, 256}); auto sources = catalog(bytes); auto value = snapshot(sources);
        value.artwork = artwork(bytes, true, {256, 256}); CMaterialValidationCache cache; QString error;
        QVERIFY(CTextureAssets::ValidateCatalogWithCache(sources, cache, &error));
        for (int index = 0; index < 30; ++index) {
            value.workspace.atlases[0].name = QString::number(index); value.artwork.families["family"].name = QString::number(index);
            QVERIFY(CTextureWorkspace::ValidateWithCache(sources, value.workspace, cache, &error));
            QVERIFY(Artwork::ValidateWithCache(value.artwork, cache, nullptr, &error));
        }
        QCOMPARE(cache.PngDecodeAttempts(), 1); QCOMPARE(cache.HashedBytes(), qint64(bytes.size()));
    }
    void malformedInputsAndLimitsLeaveUsageUntouched() {
        const auto bytes = png(); auto sources = catalog(bytes); auto value = snapshot(sources); Budget::Usage usage; usage.metadataBytes = 777; QString error;
        auto invalid = value; invalid.workspace.atlases[0].size = {1, 4};
        QVERIFY(!Budget::Validate(sources, {invalid}, &usage, &error)); QCOMPARE(usage.metadataBytes, qint64(777));
        invalid = value; invalid.artwork = artwork(bytes); invalid.artwork.revisions["revision"].sourceDigest = QByteArray(32, 'x');
        QVERIFY(!Budget::Validate(sources, {invalid}, &usage, &error)); QCOMPARE(usage.metadataBytes, qint64(777));
        sources[0].digest = QByteArray(32, 'x'); QVERIFY(!Budget::Validate(sources, {value}, &usage, &error)); QCOMPARE(usage.metadataBytes, qint64(777));
        QVERIFY(!Budget::Validate({}, {}, {-1, 0}, &usage, &error)); QCOMPARE(usage.metadataBytes, qint64(777));
        QVERIFY(Budget::Validate({}, {}, {0, 0}, &usage, &error)); QCOMPARE(usage.StorageBytes(), qint64(0)); QCOMPARE(usage.decodedBytes, qint64(0));
    }
    void frozenStateReusesCompleteValidationWithoutBorrowingCallerFields() {
        const auto bytes = png(); Artwork::State frozen; QString error; QVERIFY(Artwork::Freeze(artwork(bytes), frozen, &error));
        QByteArray fingerprint; QVERIFY(Artwork::Fingerprint(frozen, fingerprint)); CMaterialValidationCache cache;
        QVERIFY(Artwork::ValidateWithCache(frozen, cache, nullptr, &error)); QCOMPARE(cache.PngDecodeAttempts(), 0); QCOMPARE(cache.HashedBytes(), qint64(0));
        QByteArray buffer(bytes.constData(), bytes.size()); auto borrowed = frozen;
        borrowed.images.first().png = QByteArray::fromRawData(buffer.constData(), buffer.size());
        borrowed.revisions["revision"].sourceBytes = borrowed.images.first().png;
        Artwork::State owned; QVERIFY(Artwork::Freeze(borrowed, owned, &error)); buffer.fill('x');
        QVERIFY(!Artwork::Validate(borrowed, nullptr, &error)); QVERIFY(Artwork::Validate(owned, nullptr, &error));
        QByteArray after; QVERIFY(Artwork::Fingerprint(owned, after)); QCOMPARE(after, fingerprint); QCOMPARE(owned.images.first().png, bytes);
    }
    void metadataChangesReuseOnlyValidatedContentAndChangedImagesAreDecoded() {
        Artwork::State frozen; QString error; QVERIFY(Artwork::Freeze(artwork(png()), frozen, &error));
        auto renamed = frozen; renamed.families["family"].name = "New name"; CMaterialValidationCache cache;
        QVERIFY(Artwork::ValidateWithCache(renamed, cache, nullptr, &error)); QCOMPARE(cache.PngDecodeAttempts(), 0); QCOMPARE(cache.HashedBytes(), qint64(0));
        QByteArray first, second; QVERIFY(Artwork::Fingerprint(frozen, first)); QVERIFY(Artwork::Fingerprint(renamed, second)); QVERIFY(first != second);
        const auto replacement = png(Qt::blue); const auto key = hash(replacement); auto changed = frozen; changed.images.clear(); changed.images.insert(key, {{2, 2}, replacement});
        auto revision = changed.revisions.take("revision"); revision.id = "replacement"; revision.sourceBytes = replacement; revision.sourceDigest = key; revision.layers[0].imageDigest = key;
        changed.revisions.insert(revision.id, revision); changed.families["family"].currentRevision = revision.id;
        CMaterialValidationCache newContent; QVERIFY(Artwork::ValidateWithCache(changed, newContent, nullptr, &error));
        QCOMPARE(newContent.PngDecodeAttempts(), 1); QCOMPARE(newContent.HashedBytes(), qint64(replacement.size()));
        changed.revisions["replacement"].layers[0].parentIndex = 42; QVERIFY(!Artwork::Validate(changed, nullptr, &error));
    }
    void sealedSnapshotsAllowIndependentConcurrentMetadataCandidates() {
        Artwork::State baseline; QVERIFY(Artwork::Freeze(artwork(png()), baseline)); QByteArray initial; QVERIFY(Artwork::Fingerprint(baseline, initial));
        std::vector<std::future<bool>> jobs;
        for (int index = 0; index < 4; ++index) jobs.push_back(std::async(std::launch::async, [baseline, index] {
            auto candidate = baseline; candidate.families["family"].name = QString::number(index); Artwork::State frozen;
            return Artwork::Freeze(candidate, frozen) && Artwork::Validate(frozen) && frozen.families["family"].name == QString::number(index);
        }));
        for (auto& job : jobs) QVERIFY(job.get()); QByteArray final; QVERIFY(Artwork::Fingerprint(baseline, final)); QCOMPARE(final, initial);
    }
};
QTEST_GUILESS_MAIN(MaterialResourceBudgetTests)
#include "material_resource_budget_tests.moc"
