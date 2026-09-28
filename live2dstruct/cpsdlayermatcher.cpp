#include "cpsdlayermatcher.h"
#include "std/cexportcontrol.h"
#include <QCryptographicHash>
#include <QHash>
#include <QSet>
#include <QtEndian>
#include <algorithm>
#include <limits>

namespace live2dstruct {
namespace {
using Artwork = CModelArtwork;
using Matcher = CPsdLayerMatcher;
bool fail(QString* error, const QString& message) { if (error) *error = message; return false; }
QString owned(const QString& value) { return QString(value.constData(), value.size()); }
bool validText(const QString& value, int maximum, bool required = false)
{ return value.size() <= maximum && !value.contains(QChar::Null) && (!required || !value.trimmed().isEmpty()); }
struct Layer {
    QString id, name;
    int parent = -1;
    Artwork::LayerKind kind = Artwork::LayerKind::Raster;
    std::optional<quint32> external;
    QSize size;
    QByteArray pixels, ancestry;
    bool supported = false;
};
struct Directory {
    QString family, sourceDocument;
    QVector<Layer> layers;
    QHash<QString, int> nativeIds;
    QHash<quint32, QVector<int>> external;
    QHash<QString, QVector<int>> names;
    QHash<QByteArray, QVector<int>> pixels;
    QHash<QByteArray, QVector<int>> ancestry;
    QMap<QPair<int, int>, QVector<int>> sizes;
};
bool freeze(const Artwork::Revision& revision, Directory& result, QString* error, CExportControl& control)
{
    if (!validText(revision.id, 1024, true) || !validText(revision.familyId, 1024, true) ||
        !validText(revision.sourceDocumentId, 1024) || !revision.sourceDocumentId.isValidUtf16() ||
        (!revision.sourceDocumentId.isEmpty() && revision.sourceDocumentId.trimmed().isEmpty()) || revision.layers.size() > Artwork::MaximumRecords)
        return fail(error, QStringLiteral("图层匹配的来源或目录数量无效"));
    Directory directory; directory.family = owned(revision.familyId); directory.sourceDocument = owned(revision.sourceDocumentId); directory.layers.reserve(revision.layers.size());
    qint64 metadata = qint64(directory.sourceDocument.size()) * 2; QSet<QString> ids;
    for (const auto& layer : revision.layers) {
        if (control.IsCancellationRequested()) { control.Checkpoint(); return fail(error, control.Error()); }
        const qint64 width = qint64(layer.bounds.right()) - layer.bounds.x() + 1;
        const qint64 height = qint64(layer.bounds.bottom()) - layer.bounds.y() + 1;
        if (!validText(layer.id, 1024, true) || !validText(layer.name, 32768) || ids.contains(layer.id) ||
            layer.parentIndex < -1 || layer.parentIndex >= revision.layers.size() ||
            (layer.kind != Artwork::LayerKind::Raster && layer.kind != Artwork::LayerKind::Group) ||
            (!layer.imageDigest.isEmpty() && layer.imageDigest.size() != 32) || width < 0 || height < 0 ||
            width > std::numeric_limits<int>::max() || height > std::numeric_limits<int>::max())
            return fail(error, QStringLiteral("图层匹配的标识、类型、摘要或父级无效"));
        if (((layer.kind == Artwork::LayerKind::Group || width == 0 || height == 0) && !layer.imageDigest.isEmpty()) ||
            (layer.kind == Artwork::LayerKind::Raster && width > 0 && height > 0 && layer.pixelEquivalent && layer.imageDigest.isEmpty()))
            return fail(error, QStringLiteral("图层匹配的像素引用与支持状态不一致"));
        const qint64 bytes = qint64(layer.id.size() + layer.name.size()) * 2;
        if (bytes > Artwork::MaximumMetadataBytes - metadata) return fail(error, QStringLiteral("图层匹配元数据超出预算"));
        metadata += bytes; ids.insert(layer.id);
        Layer value; value.id = owned(layer.id); value.name = owned(layer.name); value.parent = layer.parentIndex;
        value.kind = layer.kind; value.external = layer.externalId; value.size = QSize(int(width), int(height));
        value.pixels = QByteArray(layer.imageDigest.constData(), layer.imageDigest.size()); value.supported = layer.pixelEquivalent;
        if (!directory.sourceDocument.isEmpty()) directory.nativeIds.insert(value.id, directory.layers.size());
        directory.layers.append(std::move(value));
    }
    QVector<QByteArray> paths(directory.layers.size());
    QVector<int> depths(directory.layers.size(), -1);
    for (int index = 0; index < directory.layers.size(); ++index) {
        if (control.IsCancellationRequested()) { control.Checkpoint(); return fail(error, control.Error()); }
        auto& layer = directory.layers[index]; QVector<int> chain; int parent = index;
        while (parent >= 0 && paths[parent].isEmpty()) {
            if (chain.contains(parent) || chain.size() > Artwork::MaximumDepth)
                return fail(error, QStringLiteral("图层匹配的层级成环或超出深度限制"));
            chain.append(parent); const int next = directory.layers[parent].parent;
            if (next >= 0 && directory.layers[next].kind != Artwork::LayerKind::Group)
                return fail(error, QStringLiteral("图层匹配的父级不是分组"));
            parent = next;
        }
        QByteArray path = parent < 0 ? QByteArray(32, 0) : paths[parent];
        int depth = parent < 0 ? -1 : depths[parent];
        while (!chain.isEmpty()) {
            const int item = chain.takeLast();
            if (++depth > Artwork::MaximumDepth) return fail(error, QStringLiteral("图层匹配的父链超出深度限制"));
            depths[item] = depth; directory.layers[item].ancestry = path;
            const auto name = directory.layers[item].name.toUtf8(); char length[4]; qToBigEndian(quint32(name.size()), length);
            QCryptographicHash hash(QCryptographicHash::Sha256); hash.addData(path); hash.addData(QByteArrayView(length, 4)); hash.addData(name);
            path = hash.result(); paths[item] = path;
        }
        if (layer.external) directory.external[*layer.external].append(index);
        if (!layer.name.isEmpty()) directory.names[layer.name].append(index);
        if (!layer.pixels.isEmpty()) directory.pixels[layer.pixels].append(index);
        if (layer.kind == Artwork::LayerKind::Raster) {
            if (layer.parent >= 0) directory.ancestry[layer.ancestry].append(index);
            directory.sizes[{layer.size.width(), layer.size.height()}].append(index);
        }
    }
    result = std::move(directory); return true;
}
bool compatible(const Layer& first, const Layer& second)
{ return first.kind == Artwork::LayerKind::Raster && second.kind == Artwork::LayerKind::Raster && first.supported && second.supported; }
Matcher::Candidate candidate(const Layer& first, const Layer& second)
{
    Matcher::Candidate result; result.layerId = second.id; result.compatible = compatible(first, second);
    if (first.external && second.external && first.external == second.external) result.reasons.append(QStringLiteral("external-id: 来源图层 ID 相同"));
    if (!first.name.isEmpty() && first.name == second.name) result.reasons.append(QStringLiteral("name: 名称相同"));
    if (first.ancestry == second.ancestry) result.reasons.append(QStringLiteral("hierarchy: 父级名称路径相同"));
    if (first.size == second.size) result.reasons.append(QStringLiteral("size: 图层尺寸相同"));
    if (!first.pixels.isEmpty() && first.pixels == second.pixels) result.reasons.append(QStringLiteral("pixels: 冻结像素内容相同"));
    if (second.kind != Artwork::LayerKind::Raster) result.reasons.append(QStringLiteral("type-changed: 候选已变为分组"));
    if (!first.supported || !second.supported) result.reasons.append(QStringLiteral("unsupported: 图层含未支持的外观或来源限制"));
    return result;
}
}

bool CPsdLayerMatcher::Compare(const Artwork::Revision& previous, const Artwork::Revision& requested,
                               Result& result, QString* error, CExportControl* supplied)
{
    if (error) error->clear(); CExportControl local; auto& control = supplied ? *supplied : local;
    try {
        if (control.IsCancellationRequested()) { control.Checkpoint(); return fail(error, control.Error()); }
        Directory before, after;
        // 在任何进度回调之前冻结匹配所需字段，记录序号不参与身份判断。
        if (!freeze(previous, before, error, control) || !freeze(requested, after, error, control)) return false;
        if (before.family != after.family) return fail(error, QStringLiteral("不能在不同素材来源之间自动匹配图层"));
        if (!control.Checkpoint()) return fail(error, control.Error());
        Result output; QSet<int> confirmed; qint64 candidates = 0;
        const bool native = !before.sourceDocument.isEmpty() || !after.sourceDocument.isEmpty();
        const bool sameDocument = native && before.sourceDocument == after.sourceDocument;
        for (int index = 0; index < before.layers.size(); ++index) {
            const auto& old = before.layers[index]; if (old.kind != Artwork::LayerKind::Raster) continue;
            if (!control.Report(CExportControl::Phase::Preparing, index, before.layers.size(), old.name)) return fail(error, control.Error());
            Match match; match.oldLayerId = old.id;
            const auto external = old.external ? after.external.value(*old.external) : QVector<int>();
            const auto nativeMatch = after.nativeIds.constFind(old.id);
            if (sameDocument && nativeMatch != after.nativeIds.cend()) {
                if (candidates >= Artwork::MaximumRecords) return fail(error, QStringLiteral("图层匹配候选总量超出预算"));
                const int next = *nativeMatch; auto value = candidate(old, after.layers[next]);
                value.reasons.prepend(QStringLiteral("native-layer-id: 原生图层 GUID 相同"));
                match.candidates.append(value); ++candidates;
                match.reasons.append(QStringLiteral("native-document-layer-id: 同一来源文档中的原生图层 GUID 一致"));
                if (value.compatible) { match.status = Status::Matched; match.matchedLayerId = value.layerId; confirmed.insert(next); }
                else match.status = Status::Incompatible;
                output.matches.append(std::move(match)); continue;
            }
            if (!native && old.external && before.external.value(*old.external).size() == 1 && external.size() == 1) {
                if (candidates >= Artwork::MaximumRecords) return fail(error, QStringLiteral("图层匹配候选总量超出预算"));
                const int next = external[0]; auto value = candidate(old, after.layers[next]);
                match.candidates.append(value); ++candidates;
                match.reasons.append(QStringLiteral("unique-external-id: 同一来源中，两版图层 ID 均唯一"));
                if (value.compatible) { match.status = Status::Matched; match.matchedLayerId = value.layerId; confirmed.insert(next); }
                else match.status = Status::Incompatible;
                output.matches.append(std::move(match)); continue;
            }
            if (native) match.reasons.append(sameDocument ? QStringLiteral("native-layer-not-found: 新版没有对应原生图层 GUID，需用户确认") :
                QStringLiteral("native-document-mismatch: 原生文档身份不同，不能按 PSD 图层编号自动关联"));
            else if (!old.external) match.reasons.append(QStringLiteral("missing-external-id: 旧图层没有稳定 ID，需用户确认"));
            else if (before.external.value(*old.external).size() > 1 || external.size() > 1)
                match.reasons.append(QStringLiteral("duplicate-external-id: 来源图层 ID 重复，不能自动关联"));
            else match.reasons.append(QStringLiteral("external-id-not-found: 新版没有对应稳定 ID"));
            QSet<int> suggested;
            const auto add = [&](const QVector<int>& values) {
                for (int value : values) {
                    if (!control.Checkpoint()) return false;
                    if (!suggested.contains(value)) {
                        if (candidates >= Artwork::MaximumRecords) return false;
                        suggested.insert(value); ++candidates;
                    }
                }
                return true;
            };
            if (!add(external) || !add(after.names.value(old.name)) || !add(after.pixels.value(old.pixels)) ||
                (old.parent >= 0 && !add(after.ancestry.value(old.ancestry))) ||
                !add(after.sizes.value({old.size.width(), old.size.height()})))
                return fail(error, control.Checkpoint() ? QStringLiteral("图层匹配候选总量超出预算，请缩小需要匹配的来源目录") : control.Error());
            QVector<int> sorted(suggested.cbegin(), suggested.cend());
            const auto score = [&](int item) {
                const auto& next = after.layers[item];
                return (old.external && next.external == old.external ? 32 : 0) + (!old.pixels.isEmpty() && old.pixels == next.pixels ? 16 : 0) +
                    (!old.name.isEmpty() && old.name == next.name ? 8 : 0) + (old.ancestry == next.ancestry ? 4 : 0) + (old.size == next.size ? 2 : 0);
            };
            std::sort(sorted.begin(), sorted.end(), [&](int first, int second) { const int a = score(first), b = score(second); return a == b ? first < second : a > b; });
            bool possible = false;
            for (int item : sorted) { auto value = candidate(old, after.layers[item]); possible |= value.compatible; match.candidates.append(std::move(value)); }
            match.status = suggested.isEmpty() ? Status::Missing : possible ? Status::NeedsConfirmation : Status::Incompatible;
            if (!suggested.isEmpty()) match.reasons.append(QStringLiteral("suggestion-only: 名称、层级、尺寸与像素证据仅用于建议，不自动关联"));
            output.matches.append(std::move(match));
        }
        for (int index = 0; index < after.layers.size(); ++index)
            if (after.layers[index].kind == Artwork::LayerKind::Raster && !confirmed.contains(index)) output.unmatchedNewLayerIds.append(after.layers[index].id);
        if (!control.Checkpoint()) return fail(error, control.Error());
        result = std::move(output); return true;
    } catch (...) { return fail(error, QStringLiteral("无法准备图层匹配，现有映射保持不变")); }
}
}
