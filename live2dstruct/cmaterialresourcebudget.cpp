#include "cmaterialresourcebudget.h"
#include "cmaterialvalidationcache.h"
#include <QSet>

namespace live2dstruct {
namespace {
using Budget = CMaterialResourceBudget;
bool fail(QString* error, const QString& message) { if (error) *error = message; return false; }
struct Counter
{
    CMaterialValidationCache cache;
    Budget::Usage usage;
    Budget::Limits limits;
    QSet<QByteArray> encoded, decoded;

    bool blob(const QByteArray& bytes, std::optional<QSize> size, const QByteArray& expected, QString* error) {
        if (bytes.isEmpty()) return !size || fail(error, QStringLiteral("素材图像数据为空"));
        QByteArray digest;
        if (size ? !cache.ValidatePng(bytes, *size, digest, error, expected) : !cache.ValidateBlob(bytes, digest, error, expected)) return false;
        if (!encoded.contains(digest)) {
            if (bytes.size() > limits.maxStorageBytes - usage.encodedBytes - usage.metadataBytes)
                return fail(error, QStringLiteral("素材来源、图集和撤销历史的存储预算不足"));
            usage.encodedBytes += bytes.size(); encoded.insert(digest); ++usage.uniqueBlobs;
        }
        if (size && !decoded.contains(digest)) {
            const qint64 pixels = qint64(size->width()) * size->height();
            if (pixels > limits.maxDecodedBytes / 4 || pixels * 4 > limits.maxDecodedBytes - usage.decodedBytes)
                return fail(error, QStringLiteral("素材来源、图集和撤销历史的解码像素预算不足"));
            usage.decodedBytes += pixels * 4; decoded.insert(digest); ++usage.uniqueImages;
        }
        return true;
    }
    bool metadata(qint64 bytes, QString* error) {
        if (bytes < 0 || bytes > limits.maxStorageBytes - usage.encodedBytes - usage.metadataBytes)
            return fail(error, QStringLiteral("素材关联和撤销历史的元数据预算不足"));
        usage.metadataBytes += bytes; return true;
    }
    bool allocation(qint64 bytes, QString* error) { return bytes == 0 || metadata(bytes + 64, error); }
    bool text(const QString& value, QString* error) { return value.isEmpty() || allocation((value.size() + 1) * qint64(sizeof(QChar)), error); }
    bool sourceMetadata(const CTextureAssets::Catalog& sources, QString* error) {
        if (!allocation(sources.size() * qint64(sizeof(int) + sizeof(CTextureAssets::Source) + 4 * sizeof(void*)), error)) return false;
        for (const auto& source : sources) if (!text(source.path, error) || !allocation(source.digest.size(), error)) return false;
        return true;
    }
    bool workspaceMetadata(const CTextureWorkspace::State& state, QString* error) {
        if (!allocation(state.sourceFingerprint.size(), error) ||
            !allocation(state.atlases.size() * qint64(sizeof(int) + sizeof(CTextureWorkspace::Atlas) + 4 * sizeof(void*)), error)) return false;
        for (const auto& atlas : state.atlases) if (!text(atlas.id, error) || !text(atlas.name, error)) return false;
        return true;
    }
    bool materialMetadata(const CModelMaterialState& state, QString* error) {
        if (!allocation(state.size() * qint64(sizeof(CModelDrawableMaterial)), error)) return false;
        for (const auto& material : state)
            if (!text(material.id, error) || !allocation(material.uvs.size() * qint64(sizeof(float)), error)) return false;
        return true;
    }
};
}

bool CMaterialResourceBudget::Validate(const CTextureAssets::Catalog& sources, const QVector<CModelAtlasState>& snapshots,
                                       const Limits& limits, Usage* usage, QString* error) try
{
    if (error) error->clear();
    if (limits.maxStorageBytes < 0 || limits.maxStorageBytes > CTextureAssets::MaximumBytes ||
        limits.maxDecodedBytes < 0 || limits.maxDecodedBytes > CTextureWorkspace::MaximumDecodedBytes ||
        snapshots.size() > CModelArtwork::MaximumRecords)
        return fail(error, QStringLiteral("素材资源预算配置或快照数量无效"));
    Counter counter; counter.limits = limits;
    if (!CTextureAssets::ValidateCatalogWithCache(sources, counter.cache, error) || !counter.sourceMetadata(sources, error)) return false;
    for (const auto& source : sources) if (!counter.blob(source.png, source.size, source.digest, error)) return false;
    QVector<CTextureWorkspace::State> workspaces;
    QVector<CModelArtwork::State> authorStates;
    QVector<CModelMaterialState> materialStates;
    for (const auto& snapshot : snapshots) {
        if (!snapshot.materials.isEmpty() && !materialStates.contains(snapshot.materials)) {
            if (!CModelMaterialEdit::Validate(snapshot.materials, error) || !counter.materialMetadata(snapshot.materials, error)) return false;
            materialStates.append(snapshot.materials);
        }
        const auto& workspace = snapshot.workspace;
        if ((!workspace.sourceFingerprint.isEmpty() || !workspace.atlases.isEmpty()) && !workspaces.contains(workspace)) {
            if (!CTextureWorkspace::ValidateWithCache(sources, workspace, counter.cache, error) || !counter.workspaceMetadata(workspace, error)) return false;
            for (const auto& atlas : workspace.atlases)
                if (!atlas.png.isEmpty() && !counter.blob(atlas.png, atlas.size, {}, error)) return false;
            workspaces.append(workspace);
        }
        const auto& artwork = snapshot.artwork;
        if (artwork.IsEmpty() || authorStates.contains(artwork)) continue;
        CModelArtwork::Usage authorUsage;
        if (!CModelArtwork::ValidateWithCache(artwork, counter.cache, &authorUsage, error) || !counter.metadata(authorUsage.metadataBytes, error)) return false;
        for (auto image = artwork.images.cbegin(); image != artwork.images.cend(); ++image)
            if (!counter.blob(image->png, image->size, image.key(), error)) return false;
        for (const auto& revision : artwork.revisions)
            if (!counter.blob(revision.sourceBytes, {}, revision.sourceDigest, error) || !counter.blob(revision.iccProfile, {}, {}, error)) return false;
        authorStates.append(artwork);
    }
    if (usage) *usage = counter.usage;
    return true;
}
catch (...) { return fail(error, QStringLiteral("无法核算素材资源预算，原状态保持不变")); }

bool CMaterialResourceBudget::Validate(const CTextureAssets::Catalog& sources, const QVector<CModelAtlasState>& snapshots,
                                       Usage* usage, QString* error)
{
    return Validate(sources, snapshots, Limits{}, usage, error);
}
}
