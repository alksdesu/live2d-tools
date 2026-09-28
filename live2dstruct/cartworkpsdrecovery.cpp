#include "cartworkpsdrecovery.h"
#include "std/cexportcontrol.h"
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
        qint64 retainedBytes = 0;
        int sourceIndex = 0;
        for (const auto& revision : frozen.revisions) {
            const auto family = frozen.families.constFind(revision.familyId);
            if (family->kind != Artwork::SourceKind::Psd) continue;
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
            CPsdSourceCatalog::Document entry(std::move(original), std::move(origin));
            if (!prepared) entry.diagnostics.append({CPsdSourceDiagnostic::Severity::Error,
                QStringLiteral("invalid-original-psd"), detail.isEmpty() ? QStringLiteral("原 PSD 未通过完整性验证") : detail, {}});
            qint64 metadata = sizeof(CPsdSourceCatalog::Document) + 32;
            for (const auto& value : {entry.origin.sourcePath, entry.origin.familyId, entry.origin.familyName,
                                      entry.origin.revisionId, entry.origin.fileName}) metadata += qint64(value.size()) * 2;
            for (const auto& value : entry.diagnostics)
                metadata += sizeof(CPsdSourceDiagnostic) + qint64(value.code.size() + value.message.size() + value.layerId.size()) * 2;
            if (metadata > Artwork::MaximumBytes - retainedBytes)
                return fail(error, QStringLiteral("原 PSD 恢复目录及文件总量超出预算"));
            retainedBytes += metadata;
            candidate.documents.append(std::move(entry));
            if (QDir::isAbsolutePath(revision.path)) candidate.protectedPaths.append(revision.path);
        }
        candidate.protectedPaths.removeDuplicates();
        if (!control.Checkpoint()) return fail(error, control.Error());
        result = std::move(candidate);
        return true;
    } catch (...) {
        return fail(error, QStringLiteral("无法准备已保存 PSD 来源，原恢复目录保持不变"));
    }
}
}
