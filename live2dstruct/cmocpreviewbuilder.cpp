#include "cmocpreviewbuilder.h"
#include "csectiontable.h"
#include "csectionvalidation.h"

#include <QCryptographicHash>
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <queue>
#include <stdexcept>

namespace live2dstruct {
namespace {
constexpr int PositionBeginSlot = 0x158;
constexpr int PositionPoolSlot = 0x15c;
constexpr int PositionCountIndex = 10;
constexpr int MaximumCount = 1000000;

bool Fail(QString* error, const QString& message)
{
    if (error) *error = message;
    return false;
}

bool Range(int begin, int count, int size)
{
    return count >= 0 && (count == 0 || (begin >= 0 && qint64(begin) + count <= size));
}

quint32 ReadWord(const QByteArray& bytes, int offset)
{
    return qFromLittleEndian<quint32>(bytes.constData() + offset);
}

void WriteWord(QByteArray& bytes, int offset, quint32 value)
{
    qToLittleEndian(value, bytes.data() + offset);
}

qint64 Align64(qint64 size)
{
    return (size + 63) & ~qint64(63);
}

struct Reference
{
    int begin;
    int count;
    int floats;
};

struct Coverage
{
    QVector<int> count;
    QVector<int> floats;
};

Coverage Measure(QVector<Reference> refs, int size)
{
    Coverage result{QVector<int>(size), QVector<int>(size)};
    QVector<int> difference(size + 1);
    for (const auto& ref : refs) {
        if (!ref.count) continue;
        ++difference[ref.begin];
        --difference[ref.begin + ref.count];
    }
    std::sort(refs.begin(), refs.end(), [](const auto& a, const auto& b) { return a.begin < b.begin; });
    std::priority_queue<std::pair<int, int>> active;
    qsizetype next = 0;
    int count = 0;
    for (int row = 0; row < size; ++row) {
        while (next < refs.size() && refs[next].begin <= row) {
            const auto& ref = refs[next++];
            if (ref.count) active.emplace(ref.floats, ref.begin + ref.count);
        }
        while (!active.empty() && active.top().second <= row) active.pop();
        count += difference[row];
        result.count[row] = count;
        result.floats[row] = active.empty() ? 0 : active.top().first;
    }
    return result;
}

bool Overlap(qint64 begin, qint64 end, qint64 otherBegin, qint64 otherEnd)
{
    return begin < otherEnd && otherBegin < end;
}

QByteArray Encode(const QVector<float>& positions)
{
    QByteArray encoded(positions.size() * 4, Qt::Uninitialized);
    for (qsizetype index = 0; index < positions.size(); ++index) {
        quint32 bits;
        std::memcpy(&bits, &positions[index], sizeof(bits));
        qToLittleEndian(bits, encoded.data() + index * 4);
    }
    return encoded;
}
}

struct CMocPreviewBuilder::Snapshot
{
    struct Mesh
    {
        QString id;
        int vertices;
        int firstRow;
        int rowCount;
    };
    struct Row
    {
        int begin;
        int owners;
        bool blendShape;
        bool safeInPlace = true;
    };
    QByteArray bytes;
    QByteArray digest;
    QVector<Mesh> meshes;
    QHash<QString, int> meshIndices;
    QVector<Row> rows;
    int poolOffset;
    int poolCount;
    int rowOffset;
    int countOffset;
};

bool CMocPreviewBuilder::Initialize(const QByteArray& baseline, QString* error)
{
    if (error) error->clear();
    try {
        CReader reader = CReader::FromBytes(baseline);
        if (!reader.IsValid() || !reader.IsMoc3() || baseline.size() < 8 || baseline[5] != 0)
            return Fail(error, QStringLiteral("预览补丁需要有效的小端 MOC3 文件头"));
        CSectionTable table(reader);
        if (!table.IsValid()) return Fail(error, reader.Error());
        const auto& offsets = *table.sectionOffsetTable;
        const auto spans = CSectionValidation::ArraySpans(offsets, *table.count_info);
        if (spans.size() != CSectionValidation::OffsetTableCount - 2)
            return Fail(error, QStringLiteral("预览补丁区段布局不完整"));

        auto state = std::make_shared<Snapshot>();
        state->bytes = QByteArray(baseline.constData(), baseline.size());
        state->digest = QCryptographicHash::hash(baseline, QCryptographicHash::Sha256);
        state->poolOffset = offsets.keyform_positions->xys;
        state->poolCount = table.count_info->keyform_positions;
        state->rowOffset = offsets.art_mesh_keyforms->keyform_position_sources_begin_indices;
        state->countOffset = offsets.count_info->count_info_offset + PositionCountIndex * 4;
        if (ReadWord(baseline, PositionBeginSlot) != quint32(state->rowOffset) ||
            ReadWord(baseline, PositionPoolSlot) != quint32(state->poolOffset) ||
            ReadWord(baseline, state->countOffset) != quint32(state->poolCount))
            return Fail(error, QStringLiteral("预览补丁位置区段与偏移表不一致"));
        for (float value : table.keyform_positions->xys)
            if (!std::isfinite(value)) return Fail(error, QStringLiteral("源关键形坐标包含非有限值"));

        QVector<Reference> artRefs, warpRefs, artProtected;
        for (const auto* mesh : table.art_meshes) {
            if (mesh->ids.isEmpty() || state->meshIndices.contains(mesh->ids))
                return Fail(error, QStringLiteral("ArtMesh ID 为空或重复：%1").arg(mesh->ids));
            if (mesh->vertex_counts < 0 || mesh->vertex_counts > MaximumCount / 2 ||
                !Range(mesh->keyform_sources_begin_indices, mesh->keyform_sources_counts, table.art_mesh_keyforms.size()))
                return Fail(error, QStringLiteral("ArtMesh 顶点或关键形范围无效：%1").arg(mesh->ids));
            state->meshIndices.insert(mesh->ids, state->meshes.size());
            state->meshes.append({mesh->ids, mesh->vertex_counts, mesh->keyform_sources_begin_indices, mesh->keyform_sources_counts});
            artRefs.append({mesh->keyform_sources_begin_indices, mesh->keyform_sources_counts, mesh->vertex_counts * 2});
        }
        for (const auto* warp : table.warp_deformers) {
            if (warp->vertex_counts < 0 || warp->vertex_counts > MaximumCount / 2 ||
                !Range(warp->keyform_sources_begin_indices, warp->keyform_sources_counts, table.warp_deformer_keyforms.size()))
                return Fail(error, QStringLiteral("Warp 顶点或关键形范围无效"));
            warpRefs.append({warp->keyform_sources_begin_indices, warp->keyform_sources_counts, warp->vertex_counts * 2});
        }

        const auto addBlendShapes = [&](const auto& shapes, const auto& targets, int keyformCount,
                                        QVector<Reference>& references) {
            QVector<Reference> bindingRefs;
            for (const auto* shape : shapes) {
                if (shape->target_indices < 0 || shape->target_indices >= targets.size() ||
                    !Range(shape->blend_shape_keyform_binding_sources_begin_indices,
                           shape->blend_shape_keyform_binding_sources_counts, table.blend_shape_keyform_bindings.size()))
                    return false;
                bindingRefs.append({shape->blend_shape_keyform_binding_sources_begin_indices,
                                    shape->blend_shape_keyform_binding_sources_counts,
                                    targets[shape->target_indices]->vertex_counts * 2});
            }
            const auto coverage = Measure(std::move(bindingRefs), table.blend_shape_keyform_bindings.size());
            for (int index = 0; index < coverage.count.size(); ++index) {
                if (!coverage.count[index]) continue;
                const auto* binding = table.blend_shape_keyform_bindings[index];
                if (!Range(binding->keyform_sources_blend_shape_indices, binding->keyform_sources_blend_shape_counts, keyformCount))
                    return false;
                references.append({binding->keyform_sources_blend_shape_indices,
                                   binding->keyform_sources_blend_shape_counts, coverage.floats[index]});
            }
            return true;
        };
        if (!addBlendShapes(table.blend_shapes_art_meshes, table.art_meshes, table.art_mesh_keyforms.size(), artProtected) ||
            !addBlendShapes(table.blend_shapes_warp_deformers, table.warp_deformers, table.warp_deformer_keyforms.size(), warpRefs))
            return Fail(error, QStringLiteral("BlendShape 目标或关键形引用越界"));

        const auto normal = Measure(artRefs, table.art_mesh_keyforms.size());
        const auto protectedRows = Measure(artProtected, table.art_mesh_keyforms.size());
        artRefs += artProtected;
        const auto art = Measure(std::move(artRefs), table.art_mesh_keyforms.size());
        const auto warp = Measure(std::move(warpRefs), table.warp_deformer_keyforms.size());
        struct Interval { int begin; int end; int artRow; };
        QVector<Interval> intervals;
        const auto addRows = [&](const auto& rows, const Coverage& coverage, bool editable) {
            for (int row = 0; row < rows.size(); ++row) {
                const int begin = rows[row]->keyform_position_sources_begin_indices;
                if (begin < 0 || begin > state->poolCount) return false;
                const int length = coverage.count[row] ? coverage.floats[row] : state->poolCount - begin;
                if (qint64(begin) + length > state->poolCount) return false;
                if (editable) state->rows.append({begin, normal.count[row], protectedRows.count[row] != 0});
                if (length) intervals.append({begin, begin + length, editable ? row : -1});
            }
            return true;
        };
        if (!addRows(table.art_mesh_keyforms, art, true) || !addRows(table.warp_deformer_keyforms, warp, false))
            return Fail(error, QStringLiteral("关键形位置切片越界"));
        std::sort(intervals.begin(), intervals.end(), [](const auto& a, const auto& b) {
            return a.begin != b.begin ? a.begin < b.begin : a.end < b.end;
        });
        int previousEnd = 0;
        for (int index = 0; index < intervals.size(); ++index) {
            const auto& interval = intervals[index];
            if (interval.artRow >= 0) {
                auto& row = state->rows[interval.artRow];
                row.safeInPlace = previousEnd <= interval.begin &&
                    (index + 1 == intervals.size() || intervals[index + 1].begin >= interval.end);
                const qint64 begin = qint64(state->poolOffset) + interval.begin * 4;
                const qint64 end = qint64(state->poolOffset) + interval.end * 4;
                for (const auto& span : spans) {
                    if (!span.count || (span.optional && !span.offset) ||
                        std::strcmp(span.name, "keyform_positions.xys") == 0) continue;
                    if (Overlap(begin, end, span.offset, span.offset + span.count * span.width)) row.safeInPlace = false;
                }
            }
            previousEnd = std::max(previousEnd, interval.end);
        }
        m_snapshot = std::move(state);
        return true;
    } catch (const std::bad_alloc&) {
        return Fail(error, QStringLiteral("预览布局内存不足"));
    } catch (const std::length_error&) {
        return Fail(error, QStringLiteral("预览布局长度超出范围"));
    }
}

QByteArray CMocPreviewBuilder::SourceDigest() const
{
    return m_snapshot ? m_snapshot->digest : QByteArray();
}

bool CMocPreviewBuilder::Build(const QHash<QString, QVector<QVector<float>>>& overrides,
                              QByteArray& result, QString* error) const
{
    if (error) error->clear();
    if (!m_snapshot) return Fail(error, QStringLiteral("预览补丁尚未初始化"));
    try {
        const auto& state = *m_snapshot;
        for (auto it = overrides.cbegin(); it != overrides.cend(); ++it)
            if (!state.meshIndices.contains(it.key())) return Fail(error, QStringLiteral("未知 ArtMesh：%1").arg(it.key()));
        struct Request { QByteArray bytes; int owners = 0; bool equal = true; bool changed = false; };
        QHash<int, Request> requests;
        QVector<int> order;
        for (const auto& mesh : state.meshes) {
            const auto found = overrides.constFind(mesh.id);
            if (found == overrides.cend()) continue;
            if (found->size() > mesh.rowCount) return Fail(error, QStringLiteral("关键形索引越界：%1").arg(mesh.id));
            for (int local = 0; local < found->size(); ++local) {
                const auto& positions = (*found)[local];
                if (positions.isEmpty()) continue;
                if (positions.size() != mesh.vertices * 2)
                    return Fail(error, QStringLiteral("关键形坐标数量不匹配：%1[%2]").arg(mesh.id).arg(local));
                for (float value : positions)
                    if (!std::isfinite(value)) return Fail(error, QStringLiteral("关键形坐标包含非有限值：%1[%2]").arg(mesh.id).arg(local));
                const int row = mesh.firstRow + local;
                const QByteArray bytes = Encode(positions);
                if (!requests.contains(row)) order.append(row);
                auto& request = requests[row];
                if (!request.owners) request.bytes = bytes;
                else request.equal = request.equal && request.bytes == bytes;
                ++request.owners;
                const int offset = state.poolOffset + state.rows[row].begin * 4;
                request.changed = request.changed || std::memcmp(state.bytes.constData() + offset, bytes.constData(), bytes.size()) != 0;
            }
        }
        QVector<int> changed;
        bool relocate = false;
        qint64 extraFloats = 0;
        for (int row : order) {
            const auto& request = requests[row];
            if (!request.changed) continue;
            const auto& source = state.rows[row];
            if (source.blendShape)
                return Fail(error, QStringLiteral("关键形行 %1 同时被 BlendShape 引用，无法独立修改").arg(row));
            if (request.owners != source.owners || !request.equal)
                return Fail(error, QStringLiteral("共享关键形行 %1 必须由全部 %2 个引用者提交相同坐标").arg(row).arg(source.owners));
            changed.append(row);
            relocate = relocate || !source.safeInPlace;
            extraFloats += request.bytes.size() / 4;
        }
        if (changed.isEmpty()) {
            result = state.bytes;
            return true;
        }
        QByteArray candidate = state.bytes;
        if (!relocate) {
            for (int row : changed) {
                const auto& bytes = requests[row].bytes;
                std::memcpy(candidate.data() + state.poolOffset + state.rows[row].begin * 4, bytes.constData(), bytes.size());
            }
        } else {
            const qint64 poolCount = state.poolCount + extraFloats;
            const qint64 poolOffset = Align64(candidate.size());
            const qint64 rowOffset = Align64(poolOffset + poolCount * 4);
            const qint64 size = Align64(rowOffset + state.rows.size() * 4);
            if (poolCount > MaximumCount || size > std::numeric_limits<int>::max())
                return Fail(error, QStringLiteral("隔离共享位置后的预览数据超出范围"));
            const auto originalSize = candidate.size();
            candidate.resize(size);
            std::memset(candidate.data() + originalSize, 0, size - originalSize);
            std::memcpy(candidate.data() + poolOffset, state.bytes.constData() + state.poolOffset, state.poolCount * 4);
            std::memcpy(candidate.data() + rowOffset, state.bytes.constData() + state.rowOffset, state.rows.size() * 4);
            int begin = state.poolCount;
            for (int row : changed) {
                const auto& bytes = requests[row].bytes;
                std::memcpy(candidate.data() + poolOffset + begin * 4, bytes.constData(), bytes.size());
                WriteWord(candidate, int(rowOffset) + row * 4, begin);
                begin += bytes.size() / 4;
            }
            WriteWord(candidate, PositionPoolSlot, quint32(poolOffset));
            WriteWord(candidate, PositionBeginSlot, quint32(rowOffset));
            WriteWord(candidate, state.countOffset, quint32(poolCount));
        }
        CReader reader = CReader::FromBytes(candidate);
        CSectionTable table(reader);
        if (!table.IsValid()) return Fail(error, QStringLiteral("预览补丁结构校验失败：%1").arg(reader.Error()));
        result = std::move(candidate);
        return true;
    } catch (const std::bad_alloc&) {
        return Fail(error, QStringLiteral("预览补丁内存不足"));
    } catch (const std::length_error&) {
        return Fail(error, QStringLiteral("预览补丁长度超出范围"));
    }
}
}
