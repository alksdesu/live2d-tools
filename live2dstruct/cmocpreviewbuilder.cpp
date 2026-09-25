#include "cmocpreviewbuilder.h"
#include "csectiontable.h"
#include "csectionvalidation.h"

#include <QCryptographicHash>
#include <QtEndian>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <queue>
#include <stdexcept>

namespace live2dstruct {
namespace {
constexpr int MaximumCount = 1000000;
constexpr int PositionCountIndex = 10;
enum class Kind { ArtMesh, Warp, Rotation };
enum class ColumnId {
    Parent, WarpOpacity, WarpBegin, RotationOpacity, Angle, OriginX, OriginY, Scale,
    ReflectX, ReflectY, ArtBegin, Positions, Count
};
constexpr size_t ColumnCount = static_cast<size_t>(ColumnId::Count);
size_t Index(Kind kind) { return static_cast<size_t>(kind); }
size_t Index(ColumnId column) { return static_cast<size_t>(column); }

bool Fail(QString* error, const QString& message)
{
    if (error) *error = message;
    return false;
}

bool Range(int begin, int count, int size)
{
    return count >= 0 && (count == 0 || (begin >= 0 && qint64(begin) + count <= size));
}

quint32 Word(const QByteArray& bytes, int offset)
{
    return qFromLittleEndian<quint32>(bytes.constData() + offset);
}

void Put(QByteArray& bytes, int offset, quint32 value)
{
    qToLittleEndian(value, bytes.data() + offset);
}

QByteArray EncodeWord(quint32 value)
{
    QByteArray bytes(4, Qt::Uninitialized);
    Put(bytes, 0, value);
    return bytes;
}

QByteArray EncodeFloat(float value)
{
    quint32 bits;
    std::memcpy(&bits, &value, sizeof(bits));
    return EncodeWord(bits);
}

QByteArray EncodePositions(const QVector<float>& values)
{
    QByteArray bytes(values.size() * 4, Qt::Uninitialized);
    for (qsizetype index = 0; index < values.size(); ++index) {
        quint32 bits;
        std::memcpy(&bits, &values[index], sizeof(bits));
        Put(bytes, int(index * 4), bits);
    }
    return bytes;
}

qint64 Align64(qint64 value) { return (value + 63) & ~qint64(63); }
bool Opacity(float value) { return std::isfinite(value) && value >= 0 && value <= 1; }

struct Reference { int begin; int count; int floats; };
struct Coverage { QVector<int> count; QVector<int> floats; };

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

struct Column
{
    const char* name;
    int slot;
    int offset;
    int count;
    QVector<QPair<qint64, qint64>> aliases;

    bool Safe(qint64 first, qint64 bytes) const
    {
        const qint64 begin = qint64(offset) + first * 4;
        const auto found = std::lower_bound(aliases.cbegin(), aliases.cend(), begin,
            [](const auto& range, qint64 value) { return range.second <= value; });
        return found == aliases.cend() || found->first >= begin + bytes;
    }
};

struct Source
{
    int firstRow;
    int rowCount;
    int vertices;
    int gridRows = 0;
    int gridColumns = 0;
    int owners = 0;
};

struct Row
{
    int begin = 0;
    int owners = 0;
    bool blendShape = false;
    bool orphanSource = false;
    bool safePosition = true;
};

struct Address { Kind kind; int row; };
struct Request
{
    QVector<QByteArray> fields;
    int owners = 0;
    bool equal = true;
    bool changed = false;
};
using Patches = std::array<QMap<int, QByteArray>, ColumnCount>;

const Request& GetRequest(const std::array<QHash<int, Request>, 3>& requests, const Address& address)
{
    return requests[Index(address.kind)].constFind(address.row).value();
}

ColumnId PositionBegin(Kind kind) { return kind == Kind::ArtMesh ? ColumnId::ArtBegin : ColumnId::WarpBegin; }
ColumnId RotationColumn(int field) { return static_cast<ColumnId>(Index(ColumnId::RotationOpacity) + field); }
}

struct CMocPreviewBuilder::Snapshot
{
    struct Mesh { QString id; Source source; };
    struct Deformer { QString id; int type; int specific; int parent; int part; };
    QByteArray bytes;
    QByteArray digest;
    QVector<Mesh> meshes;
    QVector<Deformer> deformers;
    QHash<QString, int> meshIndices, deformerIndices;
    QVector<Source> warps, rotations;
    std::array<QVector<Row>, 3> rows;
    std::array<Column, ColumnCount> columns;
    int positionCountOffset = 0;
    int partCount = 0;

    const Column& Get(ColumnId column) const { return columns[Index(column)]; }
    QByteArray Cell(ColumnId column, int row) const { return bytes.mid(Get(column).offset + row * 4, 4); }
    QByteArray Positions(Kind kind, int row, int vertices) const
    {
        return bytes.mid(Get(ColumnId::Positions).offset + rows[Index(kind)][row].begin * 4, vertices * 8);
    }
    QByteArray FloatCell(ColumnId column, int row, float value) const
    {
        const auto source = Cell(column, row);
        const quint32 bits = Word(source, 0);
        float original;
        std::memcpy(&original, &bits, sizeof(original));
        return original == value ? source : EncodeFloat(value);
    }
    QByteArray BoolCell(ColumnId column, int row, bool value) const
    {
        const auto source = Cell(column, row);
        return (Word(source, 0) != 0) == value ? source : EncodeWord(value ? 1 : 0);
    }
    bool Read(CSectionTable& table, QString* error);
    bool ParentPatches(const QHash<QString, int>& overrides, Patches& patches, QString* error) const;
    bool Write(const Patches& patches, const QVector<Address>& positions,
               const std::array<QHash<int, Request>, 3>& requests,
               QByteArray& output, QString* error) const;
};

bool CMocPreviewBuilder::Snapshot::Read(CSectionTable& table, QString* error)
{
    const auto& offsets = *table.sectionOffsetTable;
    const auto& counts = *table.count_info;
    const auto spans = CSectionValidation::ArraySpans(offsets, counts);
    if (spans.size() != CSectionValidation::OffsetTableCount - 2)
        return Fail(error, QStringLiteral("预览补丁区段布局不完整"));
    columns = {{
        {"deformers.parent_deformer_indices", 0x80, offsets.deformers->parent_deformer_indices, counts.deformers, {}},
        {"warp_deformer_keyforms.opacities", 0x12c, offsets.warp_deformer_keyforms->opacities, counts.warp_deformer_keyforms, {}},
        {"warp_deformer_keyforms.keyform_position_sources_begin_indices", 0x130, offsets.warp_deformer_keyforms->keyform_position_sources_begin_indices, counts.warp_deformer_keyforms, {}},
        {"rotation_deformer_keyforms.opacities", 0x134, offsets.rotation_deformer_keyforms->opacities, counts.rotation_deformer_keyforms, {}},
        {"rotation_deformer_keyforms.angles", 0x138, offsets.rotation_deformer_keyforms->angles, counts.rotation_deformer_keyforms, {}},
        {"rotation_deformer_keyforms.origin_x", 0x13c, offsets.rotation_deformer_keyforms->origin_x, counts.rotation_deformer_keyforms, {}},
        {"rotation_deformer_keyforms.origin_y", 0x140, offsets.rotation_deformer_keyforms->origin_y, counts.rotation_deformer_keyforms, {}},
        {"rotation_deformer_keyforms.scales", 0x144, offsets.rotation_deformer_keyforms->scales, counts.rotation_deformer_keyforms, {}},
        {"rotation_deformer_keyforms.is_reflect_x", 0x148, offsets.rotation_deformer_keyforms->is_reflect_x, counts.rotation_deformer_keyforms, {}},
        {"rotation_deformer_keyforms.is_reflect_y", 0x14c, offsets.rotation_deformer_keyforms->is_reflect_y, counts.rotation_deformer_keyforms, {}},
        {"art_mesh_keyforms.keyform_position_sources_begin_indices", 0x158, offsets.art_mesh_keyforms->keyform_position_sources_begin_indices, counts.art_mesh_keyforms, {}},
        {"keyform_positions.xys", 0x15c, offsets.keyform_positions->xys, counts.keyform_positions, {}}
    }};
    positionCountOffset = offsets.count_info->count_info_offset + PositionCountIndex * 4;
    partCount = counts.parts;
    if (Word(bytes, positionCountOffset) != quint32(counts.keyform_positions))
        return Fail(error, QStringLiteral("预览位置数量与区段表不一致"));
    for (auto& column : columns) {
        if (Word(bytes, column.slot) != quint32(column.offset))
            return Fail(error, QStringLiteral("预览区段 %1 与偏移表不一致").arg(QString::fromLatin1(column.name)));
        const qint64 end = qint64(column.offset) + qint64(column.count) * 4;
        for (const auto& span : spans) {
            if (!span.count || (span.optional && !span.offset) || std::strcmp(span.name, column.name) == 0) continue;
            const qint64 first = std::max(qint64(column.offset), span.offset);
            const qint64 last = std::min(end, span.offset + span.count * span.width);
            if (first < last) column.aliases.append({first, last});
        }
        std::sort(column.aliases.begin(), column.aliases.end());
        QVector<QPair<qint64, qint64>> merged;
        for (const auto& range : column.aliases) {
            if (!merged.isEmpty() && range.first <= merged.last().second)
                merged.last().second = std::max(merged.last().second, range.second);
            else merged.append(range);
        }
        column.aliases.swap(merged);
    }
    for (float value : table.keyform_positions->xys)
        if (!std::isfinite(value)) return Fail(error, QStringLiteral("源关键形坐标包含非有限值"));

    std::array<QVector<Reference>, 3> normal, all, orphan, blend;
    for (const auto* mesh : table.art_meshes) {
        if (mesh->ids.isEmpty() || meshIndices.contains(mesh->ids))
            return Fail(error, QStringLiteral("ArtMesh ID 为空或重复：%1").arg(mesh->ids));
        if (mesh->vertex_counts < 0 || mesh->vertex_counts > MaximumCount / 2 ||
            !Range(mesh->keyform_sources_begin_indices, mesh->keyform_sources_counts, counts.art_mesh_keyforms))
            return Fail(error, QStringLiteral("ArtMesh 顶点或关键形范围无效：%1").arg(mesh->ids));
        meshIndices.insert(mesh->ids, meshes.size());
        meshes.append({mesh->ids, {mesh->keyform_sources_begin_indices, mesh->keyform_sources_counts, mesh->vertex_counts}});
        normal[Index(Kind::ArtMesh)].append({mesh->keyform_sources_begin_indices, mesh->keyform_sources_counts, mesh->vertex_counts * 2});
    }
    for (const auto* warp : table.warp_deformers) {
        if (warp->vertex_counts < 0 || warp->vertex_counts > MaximumCount / 2 ||
            !Range(warp->keyform_sources_begin_indices, warp->keyform_sources_counts, counts.warp_deformer_keyforms))
            return Fail(error, QStringLiteral("Warp 顶点或关键形范围无效"));
        warps.append({warp->keyform_sources_begin_indices, warp->keyform_sources_counts, warp->vertex_counts, warp->rows, warp->columns});
    }
    for (const auto* rotation : table.rotation_deformers) {
        if (!Range(rotation->keyform_sources_begin_indices, rotation->keyform_sources_counts, counts.rotation_deformer_keyforms))
            return Fail(error, QStringLiteral("Rotation 关键形范围无效"));
        rotations.append({rotation->keyform_sources_begin_indices, rotation->keyform_sources_counts, 0});
    }
    for (const auto* deformer : table.deformers) {
        if (deformer->ids.isEmpty() || deformerIndices.contains(deformer->ids))
            return Fail(error, QStringLiteral("Deformer ID 为空或重复：%1").arg(deformer->ids));
        deformerIndices.insert(deformer->ids, deformers.size());
        deformers.append({deformer->ids, deformer->types, deformer->specific_sources_indices,
                           deformer->parent_deformer_indices, deformer->parent_part_indices});
        if (deformer->types != 0 && deformer->types != 1) continue;
        auto& sources = deformer->types == 0 ? warps : rotations;
        if (deformer->specific_sources_indices < 0 || deformer->specific_sources_indices >= sources.size())
            return Fail(error, QStringLiteral("变形器专用源索引越界：%1").arg(deformer->ids));
        auto& source = sources[deformer->specific_sources_indices];
        ++source.owners;
        const Kind kind = deformer->types == 0 ? Kind::Warp : Kind::Rotation;
        normal[Index(kind)].append({source.firstRow, source.rowCount, source.vertices * 2});
    }
    all[Index(Kind::ArtMesh)] = normal[Index(Kind::ArtMesh)];
    for (Kind kind : {Kind::Warp, Kind::Rotation}) {
        const auto& sources = kind == Kind::Warp ? warps : rotations;
        for (const auto& source : sources) {
            const Reference reference{source.firstRow, source.rowCount, source.vertices * 2};
            all[Index(kind)].append(reference);
            if (!source.owners) orphan[Index(kind)].append(reference);
        }
    }
    const auto addBlendShapes = [&](const auto& shapes, const auto& targets, Kind kind, int rowCount) {
        QVector<Reference> bindings;
        for (const auto* shape : shapes) {
            if (shape->target_indices < 0 || shape->target_indices >= targets.size() ||
                !Range(shape->blend_shape_keyform_binding_sources_begin_indices,
                       shape->blend_shape_keyform_binding_sources_counts, table.blend_shape_keyform_bindings.size())) return false;
            bindings.append({shape->blend_shape_keyform_binding_sources_begin_indices,
                             shape->blend_shape_keyform_binding_sources_counts, targets[shape->target_indices]->vertex_counts * 2});
        }
        const auto coverage = Measure(std::move(bindings), table.blend_shape_keyform_bindings.size());
        for (int index = 0; index < coverage.count.size(); ++index) {
            if (!coverage.count[index]) continue;
            const auto* binding = table.blend_shape_keyform_bindings[index];
            if (!Range(binding->keyform_sources_blend_shape_indices, binding->keyform_sources_blend_shape_counts, rowCount)) return false;
            const Reference reference{binding->keyform_sources_blend_shape_indices,
                                      binding->keyform_sources_blend_shape_counts, coverage.floats[index]};
            blend[Index(kind)].append(reference);
            all[Index(kind)].append(reference);
        }
        return true;
    };
    if (!addBlendShapes(table.blend_shapes_art_meshes, table.art_meshes, Kind::ArtMesh, counts.art_mesh_keyforms) ||
        !addBlendShapes(table.blend_shapes_warp_deformers, table.warp_deformers, Kind::Warp, counts.warp_deformer_keyforms))
        return Fail(error, QStringLiteral("BlendShape 目标或关键形引用越界"));

    struct Interval { int begin; int end; Kind kind; int row; };
    QVector<Interval> intervals;
    const std::array<int, 3> rowCounts{counts.art_mesh_keyforms, counts.warp_deformer_keyforms, counts.rotation_deformer_keyforms};
    for (Kind kind : {Kind::ArtMesh, Kind::Warp, Kind::Rotation}) {
        const int count = rowCounts[Index(kind)];
        const auto owners = Measure(std::move(normal[Index(kind)]), count);
        const auto protectedRows = Measure(std::move(blend[Index(kind)]), count);
        const auto orphanRows = Measure(std::move(orphan[Index(kind)]), count);
        const auto coverage = Measure(std::move(all[Index(kind)]), count);
        auto& target = rows[Index(kind)];
        target.reserve(count);
        for (int row = 0; row < count; ++row) {
            Row data;
            data.owners = owners.count[row];
            data.blendShape = protectedRows.count[row] != 0;
            data.orphanSource = orphanRows.count[row] != 0;
            if (kind != Kind::Rotation) {
                const quint32 begin = Word(bytes, Get(PositionBegin(kind)).offset + row * 4);
                if (begin > quint32(counts.keyform_positions)) return Fail(error, QStringLiteral("关键形位置切片起点越界"));
                data.begin = int(begin);
                const int length = coverage.count[row] ? coverage.floats[row] : counts.keyform_positions - data.begin;
                if (qint64(data.begin) + length > counts.keyform_positions)
                    return Fail(error, QStringLiteral("关键形位置切片越界"));
                if (length) intervals.append({data.begin, data.begin + length, kind, row});
                data.safePosition = Get(ColumnId::Positions).Safe(data.begin, qint64(length) * 4);
            }
            target.append(data);
        }
    }
    std::sort(intervals.begin(), intervals.end(), [](const auto& a, const auto& b) {
        return a.begin != b.begin ? a.begin < b.begin : a.end < b.end;
    });
    int previousEnd = 0;
    for (int index = 0; index < intervals.size(); ++index) {
        const auto& interval = intervals[index];
        auto& row = rows[Index(interval.kind)][interval.row];
        row.safePosition = row.safePosition && previousEnd <= interval.begin &&
            (index + 1 == intervals.size() || intervals[index + 1].begin >= interval.end);
        previousEnd = std::max(previousEnd, interval.end);
    }
    return true;
}

bool CMocPreviewBuilder::Snapshot::ParentPatches(const QHash<QString, int>& overrides,
                                                Patches& patches, QString* error) const
{
    if (overrides.isEmpty()) return true;
    QVector<int> parents;
    parents.reserve(deformers.size());
    for (const auto& deformer : deformers) parents.append(deformer.parent);
    for (auto it = overrides.cbegin(); it != overrides.cend(); ++it) {
        const auto found = deformerIndices.constFind(it.key());
        if (found == deformerIndices.cend()) return Fail(error, QStringLiteral("未知父级覆盖目标：%1").arg(it.key()));
        const int index = *found, parent = it.value();
        const auto& deformer = deformers[index];
        if ((deformer.type != 0 && deformer.type != 1) || parent < -1 || parent >= deformers.size() || parent == index)
            return Fail(error, QStringLiteral("变形器父级覆盖无效：%1").arg(it.key()));
        if (parent != deformer.parent && parent >= 0 && deformer.part != deformers[parent].part)
            return Fail(error, QStringLiteral("新父变形器必须属于同一 Part：%1").arg(it.key()));
        parents[index] = parent;
        if (parent != deformer.parent) patches[Index(ColumnId::Parent)].insert(index, EncodeWord(quint32(parent)));
    }
    QVector<quint8> states(deformers.size(), 0);
    for (int start = 0; start < parents.size(); ++start) {
        if (states[start]) continue;
        QVector<int> path;
        int current = start;
        while (current != -1) {
            if (current < 0 || current >= parents.size()) return Fail(error, QStringLiteral("变形器父级索引越界"));
            if (states[current] == 2) break;
            if (states[current] == 1) return Fail(error, QStringLiteral("变形器父级覆盖形成循环"));
            if (deformers[current].part < -1 || deformers[current].part >= partCount)
                return Fail(error, QStringLiteral("变形器所属 Part 索引越界"));
            states[current] = 1;
            path.append(current);
            current = parents[current];
        }
        for (int index : path) states[index] = 2;
    }
    return true;
}

bool CMocPreviewBuilder::Snapshot::Write(const Patches& input, const QVector<Address>& positions,
                                        const std::array<QHash<int, Request>, 3>& requests,
                                        QByteArray& output, QString* error) const
{
    Patches patches = input;
    bool relocatePool = false;
    qint64 extraFloats = 0;
    for (const auto& address : positions) {
        relocatePool = relocatePool || !rows[Index(address.kind)][address.row].safePosition;
        extraFloats += GetRequest(requests, address).fields[0].size() / 4;
    }
    const auto& pool = Get(ColumnId::Positions);
    qint64 finalSize = bytes.size();
    qint64 poolOffset = pool.offset;
    int poolCount = pool.count;
    std::array<qint64, ColumnCount> destinations{};
    std::array<bool, ColumnCount> relocated{};
    if (relocatePool) {
        if (extraFloats > MaximumCount - pool.count) return Fail(error, QStringLiteral("隔离共享位置后的预览数据超出范围"));
        poolOffset = Align64(finalSize);
        poolCount += int(extraFloats);
        finalSize = poolOffset + qint64(poolCount) * 4;
        int begin = pool.count;
        for (const auto& address : positions) {
            const ColumnId column = PositionBegin(address.kind);
            patches[Index(column)].insert(address.row, EncodeWord(quint32(begin)));
            relocated[Index(column)] = true;
            begin += GetRequest(requests, address).fields[0].size() / 4;
        }
    }
    for (size_t index = 0; index < ColumnCount; ++index) {
        const auto& column = columns[index];
        destinations[index] = column.offset;
        if (patches[index].isEmpty()) continue;
        for (auto it = patches[index].cbegin(); it != patches[index].cend(); ++it) {
            if (it.key() < 0 || it.key() >= column.count || it->size() != 4)
                return Fail(error, QStringLiteral("预览字段补丁越界"));
            relocated[index] = relocated[index] || !column.Safe(it.key(), 4);
        }
        if (relocated[index]) {
            destinations[index] = Align64(finalSize);
            finalSize = destinations[index] + qint64(column.count) * 4;
        }
    }
    if (finalSize != bytes.size()) finalSize = Align64(finalSize);
    if (finalSize > std::numeric_limits<int>::max()) return Fail(error, QStringLiteral("预览补丁文件长度超出范围"));
    QByteArray candidate = bytes;
    if (finalSize != bytes.size()) {
        candidate.resize(finalSize);
        std::memset(candidate.data() + bytes.size(), 0, finalSize - bytes.size());
    }
    if (relocatePool) {
        std::memcpy(candidate.data() + poolOffset, bytes.constData() + pool.offset, pool.count * 4);
        Put(candidate, pool.slot, quint32(poolOffset));
        Put(candidate, positionCountOffset, quint32(poolCount));
    }
    for (size_t index = 0; index < ColumnCount; ++index) {
        const auto& column = columns[index];
        if (relocated[index]) {
            std::memcpy(candidate.data() + destinations[index], bytes.constData() + column.offset, column.count * 4);
            Put(candidate, column.slot, quint32(destinations[index]));
        }
        for (auto it = patches[index].cbegin(); it != patches[index].cend(); ++it)
            std::memcpy(candidate.data() + destinations[index] + it.key() * 4, it->constData(), 4);
    }
    qint64 appended = pool.count;
    for (const auto& address : positions) {
        const auto& values = GetRequest(requests, address).fields[0];
        const qint64 begin = relocatePool ? appended : rows[Index(address.kind)][address.row].begin;
        std::memcpy(candidate.data() + poolOffset + begin * 4, values.constData(), values.size());
        appended += values.size() / 4;
    }
    CReader reader = CReader::FromBytes(candidate);
    CSectionTable table(reader);
    if (!table.IsValid()) return Fail(error, QStringLiteral("预览补丁结构校验失败：%1").arg(reader.Error()));
    output = std::move(candidate);
    return true;
}

bool CMocPreviewBuilder::Initialize(const QByteArray& baseline, QString* error)
{
    if (error) error->clear();
    try {
        CReader reader = CReader::FromBytes(baseline);
        if (!reader.IsValid() || !reader.IsMoc3() || baseline.size() < 8 || baseline[5] != 0)
            return Fail(error, QStringLiteral("预览补丁需要有效的小端 MOC3 文件头"));
        CSectionTable table(reader);
        if (!table.IsValid()) return Fail(error, reader.Error());
        auto candidate = std::make_shared<Snapshot>();
        candidate->bytes = QByteArray(baseline.constData(), baseline.size());
        candidate->digest = QCryptographicHash::hash(candidate->bytes, QCryptographicHash::Sha256);
        if (!candidate->Read(table, error)) return false;
        m_snapshot = std::move(candidate);
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
    return Build(overrides, {}, {}, {}, result, error);
}

bool CMocPreviewBuilder::Build(const QHash<QString, QVector<QVector<float>>>& artMeshPositions,
                              const QHash<QString, QVector<CModelWarpKeyform>>& warpKeyforms,
                              const QHash<QString, QVector<CModelRotationKeyform>>& rotationKeyforms,
                              const QHash<QString, int>& deformerParents,
                              QByteArray& result, QString* error) const
{
    if (error) error->clear();
    if (!m_snapshot) return Fail(error, QStringLiteral("预览补丁尚未初始化"));
    try {
        const auto& state = *m_snapshot;
        for (auto it = artMeshPositions.cbegin(); it != artMeshPositions.cend(); ++it)
            if (!state.meshIndices.contains(it.key())) return Fail(error, QStringLiteral("未知 ArtMesh：%1").arg(it.key()));
        const auto knownDeformers = [&](const auto& overrides, int type) {
            for (auto it = overrides.cbegin(); it != overrides.cend(); ++it) {
                const auto found = state.deformerIndices.constFind(it.key());
                if (found == state.deformerIndices.cend() || state.deformers[*found].type != type)
                    return Fail(error, QStringLiteral("未知变形器或类型不匹配：%1").arg(it.key()));
            }
            return true;
        };
        if (!knownDeformers(warpKeyforms, 0) || !knownDeformers(rotationKeyforms, 1)) return false;
        Patches patches;
        if (!state.ParentPatches(deformerParents, patches, error)) return false;
        std::array<QHash<int, Request>, 3> requests;
        QVector<Address> order;
        const auto collect = [&](Kind kind, int row, QVector<QByteArray> fields, const QVector<QByteArray>& original) {
            auto& table = requests[Index(kind)];
            if (!table.contains(row)) order.append({kind, row});
            auto& request = table[row];
            if (!request.owners) request.fields = fields;
            else request.equal = request.equal && request.fields == fields;
            ++request.owners;
            request.changed = request.changed || fields != original;
        };
        const auto finitePositions = [](const QVector<float>& values) {
            return std::all_of(values.cbegin(), values.cend(), [](float value) { return std::isfinite(value); });
        };
        for (const auto& mesh : state.meshes) {
            const auto found = artMeshPositions.constFind(mesh.id);
            if (found == artMeshPositions.cend()) continue;
            if (found->size() > mesh.source.rowCount) return Fail(error, QStringLiteral("关键形索引越界：%1").arg(mesh.id));
            for (int local = 0; local < found->size(); ++local) {
                const auto& values = (*found)[local];
                if (values.isEmpty()) continue;
                if (values.size() != mesh.source.vertices * 2 || !finitePositions(values))
                    return Fail(error, QStringLiteral("关键形坐标数量或数值无效：%1[%2]").arg(mesh.id).arg(local));
                const int row = mesh.source.firstRow + local;
                collect(Kind::ArtMesh, row, {EncodePositions(values)}, {state.Positions(Kind::ArtMesh, row, mesh.source.vertices)});
            }
        }
        for (const auto& deformer : state.deformers) {
            if (deformer.type == 0 && warpKeyforms.contains(deformer.id)) {
                const auto& forms = warpKeyforms[deformer.id];
                const auto& source = state.warps[deformer.specific];
                const qint64 vertices = (qint64(source.gridRows) + 1) * (qint64(source.gridColumns) + 1);
                if (source.gridRows < 0 || source.gridColumns < 0 || source.gridRows > 1000 || source.gridColumns > 1000 ||
                    vertices != source.vertices || forms.isEmpty() || forms.size() != source.rowCount)
                    return Fail(error, QStringLiteral("Warp 网格或关键形数量无效：%1").arg(deformer.id));
                for (int local = 0; local < forms.size(); ++local) {
                    const auto& form = forms[local];
                    const int row = source.firstRow + local;
                    if (!Opacity(form.opacity) || form.positions.size() != source.vertices * 2 || !finitePositions(form.positions))
                        return Fail(error, QStringLiteral("Warp 关键形数值无效：%1[%2]").arg(deformer.id).arg(local));
                    collect(Kind::Warp, row, {EncodePositions(form.positions), state.FloatCell(ColumnId::WarpOpacity, row, form.opacity)},
                            {state.Positions(Kind::Warp, row, source.vertices), state.Cell(ColumnId::WarpOpacity, row)});
                }
            } else if (deformer.type == 1 && rotationKeyforms.contains(deformer.id)) {
                const auto& forms = rotationKeyforms[deformer.id];
                const auto& source = state.rotations[deformer.specific];
                if (forms.isEmpty() || forms.size() != source.rowCount)
                    return Fail(error, QStringLiteral("Rotation 关键形数量无效：%1").arg(deformer.id));
                for (int local = 0; local < forms.size(); ++local) {
                    const auto& form = forms[local];
                    const int row = source.firstRow + local;
                    if (!Opacity(form.opacity) || !std::isfinite(form.angle) || !std::isfinite(form.originX) ||
                        !std::isfinite(form.originY) || !std::isfinite(form.scale) || form.scale < 0)
                        return Fail(error, QStringLiteral("Rotation 关键形数值无效：%1[%2]").arg(deformer.id).arg(local));
                    QVector<QByteArray> original;
                    for (int field = 0; field < 7; ++field) original.append(state.Cell(RotationColumn(field), row));
                    collect(Kind::Rotation, row, {state.FloatCell(ColumnId::RotationOpacity, row, form.opacity),
                        state.FloatCell(ColumnId::Angle, row, form.angle), state.FloatCell(ColumnId::OriginX, row, form.originX),
                        state.FloatCell(ColumnId::OriginY, row, form.originY), state.FloatCell(ColumnId::Scale, row, form.scale),
                        state.BoolCell(ColumnId::ReflectX, row, form.reflectX), state.BoolCell(ColumnId::ReflectY, row, form.reflectY)}, original);
                }
            }
        }
        QVector<Address> changedPositions;
        for (const auto& address : order) {
            const auto& request = GetRequest(requests, address);
            if (!request.changed) continue;
            const auto& row = state.rows[Index(address.kind)][address.row];
            if (row.blendShape) return Fail(error, QStringLiteral("关键形行 %1 同时被 BlendShape 引用，无法独立修改").arg(address.row));
            if (row.orphanSource) return Fail(error, QStringLiteral("关键形行 %1 仍被未映射的变形器专用源引用").arg(address.row));
            if (!request.equal || request.owners != row.owners)
                return Fail(error, QStringLiteral("共享关键形行 %1 必须由全部 %2 个引用者提交相同数据").arg(address.row).arg(row.owners));
            if (address.kind != Kind::Rotation) {
                const auto& values = request.fields[0];
                const auto original = state.bytes.mid(state.Get(ColumnId::Positions).offset + row.begin * 4, values.size());
                if (values != original) changedPositions.append(address);
                if (address.kind == Kind::Warp && request.fields[1] != state.Cell(ColumnId::WarpOpacity, address.row))
                    patches[Index(ColumnId::WarpOpacity)].insert(address.row, request.fields[1]);
            } else {
                for (int field = 0; field < 7; ++field) {
                    const auto column = RotationColumn(field);
                    if (request.fields[field] != state.Cell(column, address.row))
                        patches[Index(column)].insert(address.row, request.fields[field]);
                }
            }
        }
        const bool anyCells = std::any_of(patches.cbegin(), patches.cend(), [](const auto& patch) { return !patch.isEmpty(); });
        if (!anyCells && changedPositions.isEmpty()) {
            result = state.bytes;
            return true;
        }
        return state.Write(patches, changedPositions, requests, result, error);
    } catch (const std::bad_alloc&) {
        return Fail(error, QStringLiteral("预览补丁内存不足"));
    } catch (const std::length_error&) {
        return Fail(error, QStringLiteral("预览补丁长度超出范围"));
    }
}
}
