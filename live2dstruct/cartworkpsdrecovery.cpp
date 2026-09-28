#include "cartworkpsdrecovery.h"
#include "cmo3/ccmo3project.h"
#include "std/cexportcontrol.h"
#include <QCryptographicHash>
#include <QDir>
#include <QMap>
#include <QSet>
#include <limits>

namespace live2dstruct {
namespace {
using Artwork = CModelArtwork;
bool fail(QString* error, const QString& message)
{
    if (error) *error = message;
    return false;
}

bool safeBounds(const Artwork::State& state, QString* error)
{
    qint64 records = state.revisions.size();
    if (records > Artwork::MaximumRecords) return fail(error, QStringLiteral("来源目录超出记录预算"));
    for (const auto& revision : state.revisions) {
        if (revision.layers.size() > Artwork::MaximumRecords - records)
            return fail(error, QStringLiteral("来源目录超出记录预算"));
        records += revision.layers.size();
        for (const auto& layer : revision.layers) {
            int left, top, right, bottom;
            layer.bounds.getCoords(&left, &top, &right, &bottom);
            const qint64 width = qint64(right) - left + 1, height = qint64(bottom) - top + 1;
            if (width < 0 || height < 0 || width > std::numeric_limits<int>::max() || height > std::numeric_limits<int>::max())
                return fail(error, QStringLiteral("来源目录边界超出可表示范围"));
        }
    }
    return true;
}

qint64 directoryBytes(const CPsdDirectory& directory)
{
    qint64 result = directory.bytes.size() + directory.sha256.size() + directory.iccProfile.size();
    result += qint64(directory.layers.size()) * (sizeof(CPsdDirectoryLayer) + sizeof(int));
    const auto diagnosticBytes = [&](const QVector<CPsdDiagnostic>& entries) {
        qint64 bytes = qint64(entries.size()) * sizeof(CPsdDiagnostic);
        for (const auto& item : entries) bytes += qint64(item.code.size() + item.message.size()) * 2;
        return bytes;
    };
    result += diagnosticBytes(directory.diagnostics);
    for (const auto& layer : directory.layers) {
        result += qint64(layer.name.size()) * 2 + layer.blendMode.size() + diagnosticBytes(layer.diagnostics);
        result += qint64(layer.additionalInfo.size()) * sizeof(CPsdAdditionalInfo);
        for (const auto& item : layer.additionalInfo) result += item.key.size() + item.data.size();
    }
    return result;
}

qint64 sourceCatalogBytes(const CPsdSourceCatalog& catalog)
{
    qint64 result = sizeof(CPsdSourceCatalog) + catalog.sourceDigest.size();
    const auto text = [&](const QString& value) { result += qint64(value.size()) * 2; };
    const auto notes = [&](const QVector<CPsdSourceDiagnostic>& values) {
        result += qint64(values.size()) * sizeof(CPsdSourceDiagnostic);
        for (const auto& value : values) { text(value.code); text(value.message); text(value.layerId); }
    };
    text(catalog.sourcePath); text(catalog.sourceVersion); notes(catalog.diagnostics);
    for (const auto& path : catalog.protectedPaths) text(path);
    QSet<QByteArray> blobs;
    const auto blob = [&](const QByteArray& value) {
        if (value.isEmpty()) return;
        const auto key = QCryptographicHash::hash(value, QCryptographicHash::Sha256);
        if (!blobs.contains(key)) { blobs.insert(key); result += value.size(); }
    };
    for (const auto& entry : catalog.documents) {
        result += sizeof(CPsdSourceCatalog::Document); notes(entry.diagnostics);
        for (const auto& value : {entry.origin.sourcePath, entry.origin.familyId, entry.origin.familyName,
                                 entry.origin.revisionId, entry.origin.fileName, entry.origin.sourceVersion}) text(value);
        result += entry.origin.sourceDigest.size();
        const auto* document = entry.Reconstructed();
        if (!document) continue;
        text(document->id); text(document->name); blob(document->iccProfile); notes(document->diagnostics);
        result += qint64(document->layers.size()) * sizeof(CPsdSourceLayer);
        for (const auto& layer : document->layers) { text(layer.id); text(layer.name); result += layer.blendMode.size(); blob(layer.png); }
    }
    return result;
}
}

bool CArtworkPsdRecovery::Prepare(const Artwork::State& artwork, CPsdSourceCatalog& result,
                                   QString* error, CExportControl* requested)
{
    if (error) error->clear();
    CExportControl local;
    auto& control = requested ? *requested : local;
    try {
        if (control.IsCancellationRequested()) { control.Checkpoint(); return fail(error, control.Error()); }
        Artwork::State frozen;
        if (!safeBounds(artwork, error) || !Artwork::Freeze(artwork, frozen, error)) return false;
        if (!control.Report(CExportControl::Phase::Preparing)) return fail(error, control.Error());
        QMap<QString, QSet<QString>> linked, detached;
        for (const auto& binding : frozen.bindings) {
            if (!control.Checkpoint()) return fail(error, control.Error());
            const auto placement = frozen.placements.constFind(binding.placementId);
            const auto material = frozen.materials.constFind(placement->materialId);
            auto& meshes = binding.status == Artwork::BindingStatus::Linked ? linked[material->revisionId] : detached[material->revisionId];
            meshes.insert(binding.meshId);
        }
        CPsdSourceCatalog candidate;
        struct Cached {
            QByteArray bytes;
            CPsdOriginal original;
            QString error;
        };
        QMap<QByteArray, Cached> cache;
        struct CachedCmo {
            QByteArray bytes;
            CPsdSourceCatalog artwork;
            QMap<QString, int> documents;
            QString version, error;
        };
        QMap<QByteArray, CachedCmo> cmoCache;
        qint64 retainedBytes = 0;
        int sourceIndex = 0;
        for (const auto& revision : frozen.revisions) {
            const auto family = frozen.families.constFind(revision.familyId);
            if (family->kind != Artwork::SourceKind::Psd && family->kind != Artwork::SourceKind::Cmo3) continue;
            if (!control.Report(CExportControl::Phase::Preparing, sourceIndex++, frozen.revisions.size(), revision.fileName))
                return fail(error, control.Error());
            CPsdSourceCatalog::Origin origin;
            origin.sourcePath = revision.path;
            origin.sourceDigest = revision.sourceDigest;
            origin.familyId = family->id;
            origin.familyName = family->name;
            origin.revisionId = revision.id;
            origin.fileName = revision.fileName;
            origin.currentRevision = family->currentRevision == revision.id;
            origin.linkedMeshes = linked.value(revision.id).size();
            origin.detachedMeshes = detached.value(revision.id).size();
            CPsdSourceCatalog::Document entry;
            if (family->kind == Artwork::SourceKind::Cmo3) {
                auto previous = cmoCache.constFind(revision.sourceDigest);
                if (previous != cmoCache.cend() && previous->bytes != revision.sourceBytes)
                    return fail(error, QStringLiteral("原 CMO3 来源摘要发生内容冲突"));
                if (previous == cmoCache.cend()) {
                    CachedCmo value; value.bytes = revision.sourceBytes;
                    cmo3::CCmo3Project project;
                    if (cmo3::CCmo3Project::OpenBytes(revision.sourceBytes, revision.path, project, &value.error, &control)) {
                        value.version = project.SourceVersion();
                        if (const auto* sources = project.Artwork()) {
                            value.artwork = *sources;
                            for (int index = 0; index < sources->documents.size(); ++index)
                                if (const auto* document = sources->documents[index].Reconstructed()) value.documents.insert(document->id, index);
                        } else value.error = project.ArtworkError();
                    }
                    if (!control.Checkpoint()) return fail(error, control.Error());
                    const qint64 amount = value.bytes.size() + sourceCatalogBytes(value.artwork);
                    if (amount > Artwork::MaximumBytes - retainedBytes)
                        return fail(error, QStringLiteral("CMO3 恢复目录及源图像总量超出预算"));
                    retainedBytes += amount;
                    cmoCache.insert(revision.sourceDigest, std::move(value));
                    previous = cmoCache.constFind(revision.sourceDigest);
                }
                origin.sourceVersion = previous->version;
                const auto selected = previous->documents.constFind(revision.sourceDocumentId);
                if (selected == previous->documents.cend()) {
                    CPsdSourceDocument missing; missing.id = revision.sourceDocumentId; missing.name = family->name;
                    entry = CPsdSourceCatalog::Document(std::move(missing));
                    entry.diagnostics.append({CPsdSourceDiagnostic::Severity::Error, QStringLiteral("unavailable-cmo-document"),
                        previous->error.isEmpty() ? QStringLiteral("CMO3 原工程中没有这个来源文档 GUID") : previous->error, {}});
                } else {
                    entry = previous->artwork.documents[*selected];
                    entry.diagnostics.append(previous->artwork.diagnostics);
                }
                entry.origin = std::move(origin);
            } else {
                CPsdOriginal original;
                QString detail;
                const auto previous = cache.constFind(revision.sourceDigest);
                bool prepared = false;
                if (previous != cache.cend()) {
                    if (previous->bytes != revision.sourceBytes)
                        return fail(error, QStringLiteral("原 PSD 来源摘要发生内容冲突"));
                    original = previous->original;
                    detail = previous->error;
                    prepared = original.IsValid();
                } else {
                    CExportControl child;
                    child.SetHandler([&](const CExportControl::Progress& progress, QString* reason) {
                        const bool accepted = control.Report(progress.phase, progress.current, progress.total, progress.detail);
                        if (!accepted && reason) *reason = control.Error();
                        return accepted;
                    });
                    prepared = CPsdOriginal::Prepare(revision.sourceBytes, revision.sourceDigest, original, &detail, &child);
                    if (!control.Checkpoint()) return fail(error, control.Error());
                    if (prepared) {
                        const qint64 amount = directoryBytes(*original.Directory());
                        if (amount > Artwork::MaximumBytes - retainedBytes)
                            return fail(error, QStringLiteral("原 PSD 恢复目录及文件总量超出预算"));
                        retainedBytes += amount;
                    }
                    cache.insert(revision.sourceDigest, {revision.sourceBytes, original, detail});
                }
                entry = CPsdSourceCatalog::Document(std::move(original), std::move(origin));
                if (!prepared) entry.diagnostics.append({CPsdSourceDiagnostic::Severity::Error,
                    QStringLiteral("invalid-original-psd"), detail.isEmpty() ? QStringLiteral("原 PSD 未通过完整性验证") : detail, {}});
            }
            qint64 metadata = sizeof(CPsdSourceCatalog::Document) + 32;
            for (const auto& value : {entry.origin.sourcePath, entry.origin.familyId, entry.origin.familyName,
                                      entry.origin.revisionId, entry.origin.fileName, entry.origin.sourceVersion}) metadata += qint64(value.size()) * 2;
            for (const auto& value : entry.diagnostics)
                metadata += sizeof(CPsdSourceDiagnostic) + qint64(value.code.size() + value.message.size() + value.layerId.size()) * 2;
            if (metadata > Artwork::MaximumBytes - retainedBytes)
                return fail(error, QStringLiteral("来源恢复目录及文件总量超出预算"));
            retainedBytes += metadata;
            candidate.documents.append(std::move(entry));
            if (QDir::isAbsolutePath(revision.path)) candidate.protectedPaths.append(revision.path);
        }
        candidate.protectedPaths.removeDuplicates();
        if (!control.Checkpoint()) return fail(error, control.Error());
        result = std::move(candidate);
        return true;
    } catch (...) {
        return fail(error, QStringLiteral("无法准备已保存来源，原恢复目录保持不变"));
    }
}
}
