#include "cmocdeformerorder.h"
#include "cmoclayoutrewriter.h"
#include "cparentgraph.h"
#include "csectiontable.h"
#include "csectionvalidation.h"
#include <QSet>
#include <QtEndian>
#include <algorithm>
#include <cstring>
#include <limits>
#include <queue>
#include <stdexcept>

namespace live2dstruct {
namespace {
constexpr int MaximumHeaderEntries = 1000000;
using Span = CSectionValidation::ColumnSpan;

bool fail(QString* error, const QString& message)
{
    if (error) *error = message;
    return false;
}

qint32 word(const QByteArray& bytes, qint64 offset)
{
    return qFromLittleEndian<qint32>(bytes.constData() + offset);
}

qint64 aligned(qint64 size) { return (size + 63) & ~qint64(63); }

bool range(qint64 offset, qint64 size, qsizetype available)
{
    return offset >= 0 && size >= 0 && offset <= available && size <= available - offset;
}

bool understoodReferences(const QByteArray& bytes, const CSectionTable& table,
                          const QVector<Span>& spans, QString* error)
{
    const int headerEnd = table.sectionOffsetTable->count_info->count_info_offset;
    if (headerEnd < CSectionValidation::KnownOffsetEnd(table.sectionOffsetTable->mocVersion) ||
        headerEnd % 4 || headerEnd > 0x40 + MaximumHeaderEntries * 4 || headerEnd > bytes.size())
        return fail(error, QStringLiteral("变形器排序的扩展文件头范围无效"));
    QSet<int> known;
    for (const auto& span : spans) known.insert(span.slot);
    int countEnd = bytes.size();
    for (int slot = 0x40; slot < headerEnd; slot += 4) {
        const int offset = word(bytes, slot);
        if (offset && (offset < headerEnd || offset > bytes.size()))
            return fail(error, QStringLiteral("变形器排序的区段偏移无效：0x%1").arg(slot, 0, 16));
        if (offset > headerEnd) countEnd = std::min(countEnd, offset);
        if (slot >= 0x48 && !known.contains(slot) && offset && offset != bytes.size())
            return fail(error, QStringLiteral("活动扩展区段 0x%1 的引用尚未识别，不能重排变形器").arg(slot, 0, 16));
    }
    const int knownCountBytes = CSectionValidation::CountInfoSize(table.sectionOffsetTable->mocVersion);
    for (int offset = headerEnd + knownCountBytes; offset < countEnd; ++offset)
        if (bytes[offset] != '\0')
            return fail(error, QStringLiteral("活动扩展数量 %1 尚未识别，不能重排变形器").arg((offset - headerEnd) / 4));
    return true;
}

bool scratchSpan(const QByteArray& bytes, const CSectionTable& table, const QVector<Span>& spans,
                 const Span& scratch, QString* error)
{
    const qint64 length = scratch.count * 8;
    if (!range(scratch.offset, length, bytes.size()))
        return fail(error, QStringLiteral("变形器运行时预留区不足 8 字节每行"));
    const auto overlaps = [&](qint64 offset, qint64 size) {
        return size && offset < scratch.offset + length && scratch.offset < offset + size;
    };
    if (overlaps(table.sectionOffsetTable->count_info->count_info_offset,
                 CSectionValidation::CountInfoSize(table.sectionOffsetTable->mocVersion)) ||
        overlaps(table.sectionOffsetTable->canvas_info->CanvasOffset, 20))
        return fail(error, QStringLiteral("变形器运行时预留区与元数据重叠"));
    for (const auto& span : spans) {
        if (span.slot == scratch.slot || !span.offset || !span.count) continue;
        qint64 width = span.width;
        if (QString::fromLatin1(span.name).contains(QLatin1String("runtime_space"))) width = 8;
        if (overlaps(span.offset, span.count * width))
            return fail(error, QStringLiteral("变形器运行时预留区与 %1 共享可写字节，不能保持排序语义")
                        .arg(QString::fromLatin1(span.name)));
    }
    return true;
}

bool avoidsMutableAliases(const QByteArray& bytes, const CSectionTable& table, const QVector<Span>& spans,
                          const QVector<Span>& moved, QString* error)
{
    const auto intersects = [](const Span& span, qint64 first, qint64 count, qint64 stride, qint64 width) {
        if (!span.count || !count) return false;
        const qint64 begin = span.offset, end = begin + span.count * span.width;
        qint64 cell = std::max(qint64(0), (begin - first) / stride);
        if (first + cell * stride + width <= begin) ++cell;
        return cell < count && first + cell * stride < end;
    };
    for (const auto& span : spans) {
        if (!span.offset || !span.count || !QString::fromLatin1(span.name).contains(QLatin1String("runtime_space"))) continue;
        if (!range(span.offset, span.count * 8, bytes.size()))
            return fail(error, QStringLiteral("运行时预留区 %1 的完整 8 字节行越界").arg(QString::fromLatin1(span.name)));
        for (const auto& column : moved)
            if (column.slot != span.slot && intersects(column, span.offset, 1, span.count * 8, span.count * 8))
                return fail(error, QStringLiteral("区段 %1 与 Core 可写预留区 %2 重叠，不能保持排序语义")
                            .arg(QString::fromLatin1(column.name), QString::fromLatin1(span.name)));
    }
    for (const auto* mesh : table.art_meshes) {
        const qint64 uvBegin = mesh->uv_sources_begin_indices, vertices = mesh->vertex_counts;
        const qint64 indexBegin = mesh->position_index_sources_begin_indices, indices = mesh->position_index_sources_counts;
        if (vertices < 0 || indices < 0 ||
            (vertices && !range(uvBegin, vertices * 2, table.uvs->uvs.size())) ||
            (indices && !range(indexBegin, indices, table.position_indices->indices.size())))
            return fail(error, QStringLiteral("ArtMesh %1 的活动 UV 或三角索引切片越界").arg(mesh->ids));
        const qint64 uvFirst = table.sectionOffsetTable->uvs->uvs + (uvBegin + 1) * 4;
        const qint64 indexFirst = table.sectionOffsetTable->position_indices->indices + indexBegin * 2;
        for (const auto& column : moved) {
            if (intersects(column, uvFirst, vertices, 8, 4))
                return fail(error, QStringLiteral("区段 %1 与 ArtMesh %2 的活动 UV 转换字节重叠，不能保持排序语义")
                            .arg(QString::fromLatin1(column.name), mesh->ids));
            if (indices && intersects(column, indexFirst, 1, indices * 2, indices * 2))
                return fail(error, QStringLiteral("区段 %1 与 ArtMesh %2 的活动三角索引转换字节重叠，不能保持排序语义")
                            .arg(QString::fromLatin1(column.name), mesh->ids));
        }
    }
    return true;
}

QVector<int> stableOrder(const CParentGraph& graph)
{
    std::priority_queue<int, std::vector<int>, std::greater<int>> ready;
    for (int root : graph.Roots()) ready.push(root);
    QVector<int> result;
    result.reserve(graph.Parents().size());
    while (!ready.empty()) {
        const int node = ready.top();
        ready.pop();
        result.append(node);
        for (int child = graph.FirstChild()[node]; child != -1; child = graph.NextSibling()[child]) ready.push(child);
    }
    return result;
}
}

bool CMocDeformerOrder::Normalize(const QByteArray& runtimeMoc, QByteArray& result, QString* error)
{
    if (error) error->clear();
    try {
        if (runtimeMoc.size() > std::numeric_limits<int>::max())
            return fail(error, QStringLiteral("变形器排序的 MOC 文件过大"));
        auto reader = CReader::FromBytes(runtimeMoc);
        CSectionTable table(reader);
        if (!table.IsValid()) return fail(error, QStringLiteral("变形器排序的 MOC 无效：%1").arg(reader.Error()));
        QVector<int> parents;
        parents.reserve(table.deformers.size());
        bool ordered = true;
        for (int index = 0; index < table.deformers.size(); ++index) {
            const int parent = table.deformers[index]->parent_deformer_indices;
            parents.append(parent);
            ordered = ordered && parent < index;
        }
        const CParentGraph graph(parents);
        if (!graph.IsValid()) return fail(error, QStringLiteral("变形器父链包含循环或越界引用"));
        for (const auto* mesh : table.art_meshes)
            if (mesh->parent_deformer_indices < -1 || mesh->parent_deformer_indices >= parents.size())
                return fail(error, QStringLiteral("ArtMesh 父变形器越界：%1").arg(mesh->ids));
        if (ordered) {
            result = runtimeMoc;
            return true;
        }
        for (const auto* deformer : table.deformers)
            if ((deformer->types != 0 && deformer->types != 1) || deformer->specific_sources_indices < 0 ||
                deformer->specific_sources_indices >= (deformer->types == 0 ? table.warp_deformers.size() : table.rotation_deformers.size()))
                return fail(error, QStringLiteral("变形器类型或专用源引用尚不能排序：%1").arg(deformer->ids));
        const auto spans = CSectionValidation::ArraySpans(*table.sectionOffsetTable, *table.count_info);
        if (spans.size() != CSectionValidation::KnownOffsetCount(table.sectionOffsetTable->mocVersion) - 2)
            return fail(error, QStringLiteral("变形器排序缺少完整区段定义"));
        if (!understoodReferences(runtimeMoc, table, spans, error)) return false;
        const auto order = stableOrder(graph);
        QVector<int> newIndex(order.size());
        for (int index = 0; index < order.size(); ++index) newIndex[order[index]] = index;
        QVector<Span> changed;
        bool hasParents = false, hasMeshParents = false, hasScratch = false;
        qint64 candidateSize = aligned(runtimeMoc.size());
        for (auto span : spans) {
            const QString name = QString::fromLatin1(span.name);
            if (!name.startsWith(QLatin1String("deformers.")) && name != QLatin1String("art_meshes.parent_deformer_indices")) continue;
            hasParents |= name == QLatin1String("deformers.parent_deformer_indices");
            hasMeshParents |= name == QLatin1String("art_meshes.parent_deformer_indices");
            if (name == QLatin1String("deformers.runtime_space_0")) {
                if (!scratchSpan(runtimeMoc, table, spans, span, error)) return false;
                span.width = 8;
                hasScratch = true;
            }
            if (!range(span.offset, span.count * span.width, runtimeMoc.size()))
                return fail(error, QStringLiteral("变形器排序区段越界：%1").arg(name));
            candidateSize = aligned(candidateSize + span.count * span.width);
            if (candidateSize > std::numeric_limits<int>::max())
                return fail(error, QStringLiteral("变形器排序后的 MOC 超过文件长度范围"));
            changed.append(span);
        }
        if (!hasParents || !hasMeshParents || !hasScratch)
            return fail(error, QStringLiteral("变形器排序缺少父级或运行时区段"));
        if (!avoidsMutableAliases(runtimeMoc, table, spans, changed, error)) return false;
        QByteArray candidate = runtimeMoc;
        candidate.reserve(candidateSize);
        QMap<int, qint64> relocated;
        for (const auto& span : changed) {
            const QString name = QString::fromLatin1(span.name);
            if (!span.count) continue;
            const bool meshParents = name == QLatin1String("art_meshes.parent_deformer_indices");
            const bool deformerParents = name == QLatin1String("deformers.parent_deformer_indices");
            candidate.resize(aligned(candidate.size()), '\0');
            const int begin = candidate.size();
            candidate.resize(begin + span.count * span.width);
            for (int index = 0; index < span.count; ++index) {
                const int old = meshParents ? index : order[index];
                const char* source = runtimeMoc.constData() + span.offset + old * span.width;
                char* target = candidate.data() + begin + index * span.width;
                if (deformerParents || meshParents) {
                    const int parent = qFromLittleEndian<qint32>(source);
                    qToLittleEndian(qint32(parent < 0 ? -1 : newIndex[parent]), target);
                } else std::memcpy(target, source, span.width);
            }
            qToLittleEndian(qint32(begin), candidate.data() + span.slot);
            relocated.insert(span.slot, span.count * span.width);
        }
        candidate.resize(aligned(candidate.size()), '\0');
        QByteArray normalized;
        if (!CMocLayoutRewriter::Rewrite(runtimeMoc, candidate, relocated, normalized, error)) return false;
        result = std::move(normalized);
        return true;
    } catch (const std::bad_alloc&) {
        return fail(error, QStringLiteral("变形器排序内存不足"));
    } catch (const std::length_error&) {
        return fail(error, QStringLiteral("变形器排序长度超出范围"));
    }
}
}
