#include "cartmeshgeometrylayout.h"
#include "csectiontable.h"
#include <algorithm>
#include <utility>

namespace live2dstruct {
bool CArtMeshGeometryLayout::CheckIsolation(const CSectionTable& table, const QSet<int>& changed, Pool pool,
                                           QStringList& conflicts, QString* error)
{
    const auto fail = [&](const QString& message) { if (error) *error = message; return false; };
    if (!table.IsValid() || (pool != Pool::Uvs && pool != Pool::Indices))
        return fail(QStringLiteral("几何存储检查的模型或类型无效"));
    for (int mesh : changed) if (mesh < 0 || mesh >= table.art_meshes.size())
        return fail(QStringLiteral("几何存储检查的 ArtMesh 索引越界"));
    QStringList result;
    if (changed.isEmpty()) { conflicts = std::move(result); return true; }
    const bool uv = pool == Pool::Uvs;
    struct Interval { qint64 first, end; int mesh; };
    QVector<Interval> intervals;
    intervals.reserve(table.art_meshes.size());
    for (int index = 0; index < table.art_meshes.size(); ++index) {
        const auto* mesh = table.art_meshes[index];
        const qint64 first = uv ? mesh->uv_sources_begin_indices : mesh->position_index_sources_begin_indices;
        const qint64 count = uv ? qint64(mesh->vertex_counts) * 2 : mesh->position_index_sources_counts;
        const qint64 size = uv ? table.uvs->uvs.size() : table.position_indices->indices.size();
        if (!count) continue;
        if (first < 0 || count < 0 || first > size || count > size - first)
            return fail(QStringLiteral("源 ArtMesh 的 UV 或三角索引切片越界：%1").arg(mesh->ids));
        intervals.append({first, first + count, index});
    }
    std::sort(intervals.begin(), intervals.end(), [](const Interval& a, const Interval& b) {
        return a.first != b.first ? a.first < b.first : a.end != b.end ? a.end < b.end : a.mesh < b.mesh;
    });
    qint64 end = -1;
    int previous = -1;
    for (int index = 0; index < intervals.size(); ++index) {
        const auto& current = intervals[index];
        const int other = end > current.first ? previous :
            index + 1 < intervals.size() && intervals[index + 1].first < current.end ? intervals[index + 1].mesh : -1;
        if (changed.contains(current.mesh) && other >= 0)
            result.append(QStringLiteral("ArtMesh %1 的%2存储与 %3 重叠；独立编辑会改变 Core 的原地转换，需要先规范化几何")
                .arg(table.art_meshes[current.mesh]->ids, uv ? QStringLiteral(" UV ") : QStringLiteral("三角索引"),
                     table.art_meshes[other]->ids));
        if (current.end > end) { end = current.end; previous = current.mesh; }
    }
    conflicts = std::move(result);
    return true;
}
}
