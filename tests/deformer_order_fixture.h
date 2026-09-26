#ifndef DEFORMER_ORDER_FIXTURE_H
#define DEFORMER_ORDER_FIXTURE_H

#include "live2dstruct/csectiontable.h"
#include "live2dstruct/csectionvalidation.h"
#include <QtEndian>
#include <QFile>
#include <QHash>
#include <algorithm>
#include <cstring>
#include <numeric>

namespace DeformerOrderFixture {
inline QByteArray read(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}
inline qint32 word(const QByteArray& bytes, qint64 offset) { return qFromLittleEndian<qint32>(bytes.constData() + offset); }
inline void put(QByteArray& bytes, qint64 offset, qint32 value) { qToLittleEndian(value, bytes.data() + offset); }
inline qint64 width(const live2dstruct::CSectionValidation::ColumnSpan& span)
{
    return std::strcmp(span.name, "deformers.runtime_space_0") == 0 ? 8 : span.width;
}
inline QByteArray payload(const QByteArray& bytes, const live2dstruct::CSectionValidation::ColumnSpan& span)
{
    return span.offset ? bytes.mid(span.offset, span.count * width(span)) : QByteArray();
}
inline QByteArray permute(const QByteArray& source, const QVector<int>& order)
{
    auto reader = live2dstruct::CReader::FromBytes(source);
    live2dstruct::CSectionTable table(reader);
    if (!table.IsValid() || order.size() != table.deformers.size()) return {};
    QVector<int> inverse(order.size(), -1);
    for (int index = 0; index < order.size(); ++index) {
        if (order[index] < 0 || order[index] >= order.size() || inverse[order[index]] != -1) return {};
        inverse[order[index]] = index;
    }
    QByteArray result = source;
    for (const auto& span : live2dstruct::CSectionValidation::ArraySpans(*table.sectionOffsetTable, *table.count_info)) {
        const QString name = QString::fromLatin1(span.name);
        if (!name.startsWith("deformers.")) continue;
        const qint64 cell = width(span);
        if (span.offset + span.count * cell > source.size()) return {};
        for (int index = 0; index < order.size(); ++index)
            std::memcpy(result.data() + span.offset + index * cell, source.constData() + span.offset + order[index] * cell, cell);
    }
    const int parentOffset = table.sectionOffsetTable->deformers->parent_deformer_indices;
    for (int index = 0; index < order.size(); ++index) {
        const int parent = table.deformers[order[index]]->parent_deformer_indices;
        put(result, parentOffset + index * 4, parent < 0 ? -1 : inverse[parent]);
    }
    const int meshOffset = table.sectionOffsetTable->art_meshes->parent_deformer_indices;
    for (int index = 0; index < table.art_meshes.size(); ++index) {
        const int parent = table.art_meshes[index]->parent_deformer_indices;
        put(result, meshOffset + index * 4, parent < 0 ? -1 : inverse[parent]);
    }
    return result;
}
inline QVector<int> reverseOrder(const live2dstruct::CSectionTable& table)
{
    QVector<int> result(table.deformers.size());
    std::iota(result.rbegin(), result.rend(), 0);
    return result;
}
inline QString semanticDifference(const QByteArray& before, const QByteArray& after)
{
    auto ar = live2dstruct::CReader::FromBytes(before), br = live2dstruct::CReader::FromBytes(after);
    live2dstruct::CSectionTable a(ar), b(br);
    if (!a.IsValid() || !b.IsValid() || a.deformers.size() != b.deformers.size()) return "invalid table/count";
    QHash<QString, int> oldIndices;
    for (int i = 0; i < a.deformers.size(); ++i) oldIndices.insert(a.deformers[i]->ids, i);
    const auto as = live2dstruct::CSectionValidation::ArraySpans(*a.sectionOffsetTable, *a.count_info);
    const auto bs = live2dstruct::CSectionValidation::ArraySpans(*b.sectionOffsetTable, *b.count_info);
    if (as.size() != bs.size()) return "schema size";
    for (int column = 0; column < as.size(); ++column) {
        const auto& x = as[column]; const auto& y = bs[column]; const QString name = QString::fromLatin1(x.name);
        if (name.startsWith("deformers.")) {
            for (int next = 0; next < b.deformers.size(); ++next) {
                const int old = oldIndices.value(b.deformers[next]->ids, -1);
                if (old < 0) return "unknown ID";
                if (name == "deformers.parent_deformer_indices") {
                    const int ap = a.deformers[old]->parent_deformer_indices, bp = b.deformers[next]->parent_deformer_indices;
                    if (bp >= next || (ap < 0) != (bp < 0) || (ap >= 0 && a.deformers[ap]->ids != b.deformers[bp]->ids)) return name;
                } else if (before.mid(x.offset + old * width(x), width(x)) != after.mid(y.offset + next * width(y), width(y))) return name;
            }
        } else if (name == "art_meshes.parent_deformer_indices") {
            if (a.art_meshes.size() != b.art_meshes.size()) return "mesh count";
            for (int mesh = 0; mesh < a.art_meshes.size(); ++mesh) {
                const int ap = a.art_meshes[mesh]->parent_deformer_indices, bp = b.art_meshes[mesh]->parent_deformer_indices;
                if ((ap < 0) != (bp < 0) || (ap >= 0 && a.deformers[ap]->ids != b.deformers[bp]->ids)) return name;
            }
        } else if (payload(before, x) != payload(after, y)) return name;
    }
    if (before.left(0x40) != after.left(0x40)) return "header";
    if (before.mid(a.sectionOffsetTable->canvas_info->CanvasOffset, 20) != after.mid(b.sectionOffsetTable->canvas_info->CanvasOffset, 20)) return "canvas";
    return {};
}
}

#endif
