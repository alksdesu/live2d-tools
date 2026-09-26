#include "cmocpreviewbuilder.h"
#include "cmoclayoutrewriter.h"
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
constexpr int MaskCountIndex = 17;
enum class Kind { ArtMesh, Warp, Rotation };
enum class ColumnId {
    Parent, WarpOpacity, WarpBegin, RotationOpacity, Angle, OriginX, OriginY, Scale,
    ReflectX, ReflectY, ArtBegin, Positions, DrawableFlags, MaskBegin, MaskCount, Masks,
    ArtOpacity, ArtDrawOrder, ArtSourceBegin, Specific,
    WarpBinding, WarpSourceBegin, WarpSourceCount, WarpVertices, WarpRows, WarpColumns, WarpQuad, WarpColor,
    RotationBinding, RotationSourceBegin, RotationSourceCount, RotationBaseAngle, RotationColor,
    BlendWarpTarget, BlendWarpBegin, BlendWarpCount,
    ParameterMax, ParameterMin, ParameterDefault, ParameterRepeat, ParameterDecimal,
    ArtMultiplyIndex, ArtScreenIndex, WarpMultiplyIndex, WarpScreenIndex, RotationMultiplyIndex, RotationScreenIndex,
    BlendRotationTarget, BlendRotationBegin, BlendRotationCount, Count
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
    int width = 4;
    bool optional = false;
    bool enabled = true;

    bool Safe(qint64 first, qint64 bytes) const
    {
        const qint64 begin = qint64(offset) + first * width;
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
struct ObjectEdit
{
    Kind kind;
    int index;
    int specific;
    QVector<int> changedRows;
};
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
    struct Mesh
    {
        QString id;
        Source source;
        int maskBegin = 0;
        int maskCount = 0;
        bool safeMasks = true;
    };
    struct Deformer { QString id; int type; int specific; int parent; int part; };
    QByteArray bytes;
    QByteArray digest;
    QVector<Mesh> meshes;
    QVector<Deformer> deformers;
    QHash<QString, int> meshIndices, deformerIndices, parameterIndices;
    QVector<Source> warps, rotations;
    std::array<QVector<Row>, 3> rows;
    std::array<Column, ColumnCount> columns;
    int positionCountOffset = 0;
    int maskCountOffset = 0;
    int countInfoOffset = 0;
    int partCount = 0;
    QHash<QString, int> originalParents;

    const Column& Get(ColumnId column) const { return columns[Index(column)]; }
    QByteArray Cell(ColumnId column, int row) const
    {
        const auto& data = Get(column);
        return bytes.mid(data.offset + row * data.width, data.width);
    }
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
    bool Detach(const QVector<ObjectEdit>& objects, const std::array<QHash<int, Request>, 3>& requests,
                QByteArray& output, bool& changed, QString* error) const;
    bool ParentPatches(const QHash<QString, int>& overrides, Patches& patches, QString* error) const;
    bool ParameterPatches(const QHash<QString, CModelParameterMetadata>& overrides, Patches& patches, QString* error) const;
    bool MaskPatches(const Overrides& overrides, Patches& patches, QMap<int, QByteArray>& masks, QString* error) const;
    bool Write(const Patches& patches, const QVector<Address>& positions,
               const std::array<QHash<int, Request>, 3>& requests,
               const QMap<int, QByteArray>& masks,
               QByteArray& output, QString* error) const;
};

bool CMocPreviewBuilder::Snapshot::Read(CSectionTable& table, QString* error)
{
    const auto& offsets = *table.sectionOffsetTable;
    const auto& counts = *table.count_info;
    auto spans = CSectionValidation::ArraySpans(offsets, counts);
    if (spans.size() != CSectionValidation::KnownOffsetCount(offsets.mocVersion) - 2)
        return Fail(error, QStringLiteral("预览补丁区段布局不完整"));
    for (auto& span : spans) {
        span.width = CSectionValidation::StorageWidth(span);
        if (span.offset && (span.offset > bytes.size() || span.count * span.width > bytes.size() - span.offset))
            return Fail(error, QStringLiteral("预览区段 %1 的完整存储空间越界").arg(QString::fromLatin1(span.name)));
    }
    const int headerEnd = offsets.count_info->count_info_offset;
    if (headerEnd % 4 || (headerEnd - CSectionValidation::OffsetTableStart) / 4 > MaximumCount)
        return Fail(error, QStringLiteral("预览补丁扩展偏移表未对齐或数量超出范围"));
    QVector<qint64> physicalOffsets;
    for (int slot = CSectionValidation::OffsetTableStart; slot < headerEnd; slot += 4) {
        const qint64 offset = Word(bytes, slot);
        if (!offset) continue;
        if (offset < headerEnd || offset > bytes.size())
            return Fail(error, QStringLiteral("预览补丁扩展区段 0x%1 偏移无效").arg(slot, 0, 16));
        physicalOffsets.append(offset);
    }
    std::sort(physicalOffsets.begin(), physicalOffsets.end());
    physicalOffsets.erase(std::unique(physicalOffsets.begin(), physicalOffsets.end()), physicalOffsets.end());
    for (int slot = CSectionValidation::OffsetTableEnd; slot < headerEnd; slot += 4) {
        const qint64 offset = Word(bytes, slot);
        if (!offset) continue;
        const auto next = std::upper_bound(physicalOffsets.cbegin(), physicalOffsets.cend(), offset);
        const qint64 end = next == physicalOffsets.cend() ? bytes.size() : *next;
        spans.append({"unparsed_header_column", offset, end - offset, 1});
    }
    columns = {{
        {"deformers.parent_deformer_indices", -1, offsets.deformers->parent_deformer_indices, counts.deformers, {}},
        {"warp_deformer_keyforms.opacities", -1, offsets.warp_deformer_keyforms->opacities, counts.warp_deformer_keyforms, {}},
        {"warp_deformer_keyforms.keyform_position_sources_begin_indices", -1, offsets.warp_deformer_keyforms->keyform_position_sources_begin_indices, counts.warp_deformer_keyforms, {}},
        {"rotation_deformer_keyforms.opacities", -1, offsets.rotation_deformer_keyforms->opacities, counts.rotation_deformer_keyforms, {}},
        {"rotation_deformer_keyforms.angles", -1, offsets.rotation_deformer_keyforms->angles, counts.rotation_deformer_keyforms, {}},
        {"rotation_deformer_keyforms.origin_x", -1, offsets.rotation_deformer_keyforms->origin_x, counts.rotation_deformer_keyforms, {}},
        {"rotation_deformer_keyforms.origin_y", -1, offsets.rotation_deformer_keyforms->origin_y, counts.rotation_deformer_keyforms, {}},
        {"rotation_deformer_keyforms.scales", -1, offsets.rotation_deformer_keyforms->scales, counts.rotation_deformer_keyforms, {}},
        {"rotation_deformer_keyforms.is_reflect_x", -1, offsets.rotation_deformer_keyforms->is_reflect_x, counts.rotation_deformer_keyforms, {}},
        {"rotation_deformer_keyforms.is_reflect_y", -1, offsets.rotation_deformer_keyforms->is_reflect_y, counts.rotation_deformer_keyforms, {}},
        {"art_mesh_keyforms.keyform_position_sources_begin_indices", -1, offsets.art_mesh_keyforms->keyform_position_sources_begin_indices, counts.art_mesh_keyforms, {}},
        {"keyform_positions.xys", -1, offsets.keyform_positions->xys, counts.keyform_positions, {}},
        {"art_meshes.drawable_flags", -1, offsets.art_meshes->drawable_flags, counts.art_meshes, {}, 1},
        {"art_meshes.drawable_mask_sources_begin_indices", -1, offsets.art_meshes->drawable_mask_sources_begin_indices, counts.art_meshes, {}},
        {"art_meshes.drawable_mask_sources_counts", -1, offsets.art_meshes->drawable_mask_sources_counts, counts.art_meshes, {}},
        {"drawable_masks.art_mesh_sources_indices", -1, offsets.drawable_masks->art_mesh_sources_indices, counts.drawable_masks, {}},
        {"art_mesh_keyforms.opacities", -1, offsets.art_mesh_keyforms->opacities, counts.art_mesh_keyforms, {}},
        {"art_mesh_keyforms.draw_orders", -1, offsets.art_mesh_keyforms->draw_orders, counts.art_mesh_keyforms, {}},
        {"art_meshes.keyform_sources_begin_indices", -1, offsets.art_meshes->keyform_sources_begin_indices, counts.art_meshes, {}},
        {"deformers.specific_sources_indices", -1, offsets.deformers->specific_sources_indices, counts.deformers, {}},
        {"warp_deformers.keyform_binding_sources_indices", -1, offsets.warp_deformers->keyform_binding_sources_indices, counts.warp_deformers, {}},
        {"warp_deformers.keyform_sources_begin_indices", -1, offsets.warp_deformers->keyform_sources_begin_indices, counts.warp_deformers, {}},
        {"warp_deformers.keyform_sources_counts", -1, offsets.warp_deformers->keyform_sources_counts, counts.warp_deformers, {}},
        {"warp_deformers.vertex_counts", -1, offsets.warp_deformers->vertex_counts, counts.warp_deformers, {}},
        {"warp_deformers.rows", -1, offsets.warp_deformers->rows, counts.warp_deformers, {}},
        {"warp_deformers.columns", -1, offsets.warp_deformers->columns, counts.warp_deformers, {}},
        {"warp_deformers.is_quad_source", -1, offsets.warp_deformers->is_quad_source, counts.warp_deformers, {}, 4, true},
        {"warp_deformers.keyform_color_sources_begin_indices", -1, offsets.warp_deformers->keyform_color_sources_begin_indices, counts.warp_deformers, {}, 4, true},
        {"rotation_deformers.keyform_binding_sources_indices", -1, offsets.rotation_deformers->keyform_binding_sources_indices, counts.rotation_deformers, {}},
        {"rotation_deformers.keyform_sources_begin_indices", -1, offsets.rotation_deformers->keyform_sources_begin_indices, counts.rotation_deformers, {}},
        {"rotation_deformers.keyform_sources_counts", -1, offsets.rotation_deformers->keyform_sources_counts, counts.rotation_deformers, {}},
        {"rotation_deformers.base_angles", -1, offsets.rotation_deformers->base_angles, counts.rotation_deformers, {}},
        {"rotation_deformers.keyform_color_sources_begin_indices", -1, offsets.rotation_deformers->keyform_color_sources_begin_indices, counts.rotation_deformers, {}, 4, true},
        {"blend_shapes_warp_deformers.target_indices", -1, offsets.blend_shapes_warp_deformers->target_indices, counts.blend_shapes_warp_deformers, {}},
        {"blend_shapes_warp_deformers.blend_shape_keyform_binding_sources_begin_indices", -1, offsets.blend_shapes_warp_deformers->blend_shape_keyform_binding_sources_begin_indices, counts.blend_shapes_warp_deformers, {}},
        {"blend_shapes_warp_deformers.blend_shape_keyform_binding_sources_counts", -1, offsets.blend_shapes_warp_deformers->blend_shape_keyform_binding_sources_counts, counts.blend_shapes_warp_deformers, {}},
        {"parameters.max_values", -1, offsets.parameters->max_values, counts.parameters, {}},
        {"parameters.min_values", -1, offsets.parameters->min_values, counts.parameters, {}},
        {"parameters.default_values", -1, offsets.parameters->default_values, counts.parameters, {}},
        {"parameters.is_repeat", -1, offsets.parameters->is_repeat, counts.parameters, {}},
        {"parameters.decimal_places", -1, offsets.parameters->decimal_places, counts.parameters, {}},
        {"art_mesh_keyforms.multiply_color_sources_indices", -1, offsets.art_mesh_keyforms->multiply_color_sources_indices, counts.art_mesh_keyforms, {}, 4, true, offsets.mocVersion >= 5},
        {"art_mesh_keyforms.screen_color_sources_indices", -1, offsets.art_mesh_keyforms->screen_color_sources_indices, counts.art_mesh_keyforms, {}, 4, true, offsets.mocVersion >= 5},
        {"warp_deformer_keyforms.multiply_color_sources_indices", -1, offsets.warp_deformer_keyforms->multiply_color_sources_indices, counts.warp_deformer_keyforms, {}, 4, true, offsets.mocVersion >= 5},
        {"warp_deformer_keyforms.screen_color_sources_indices", -1, offsets.warp_deformer_keyforms->screen_color_sources_indices, counts.warp_deformer_keyforms, {}, 4, true, offsets.mocVersion >= 5},
        {"rotation_deformer_keyforms.multiply_color_sources_indices", -1, offsets.rotation_deformer_keyforms->multiply_color_sources_indices, counts.rotation_deformer_keyforms, {}, 4, true, offsets.mocVersion >= 5},
        {"rotation_deformer_keyforms.screen_color_sources_indices", -1, offsets.rotation_deformer_keyforms->screen_color_sources_indices, counts.rotation_deformer_keyforms, {}, 4, true, offsets.mocVersion >= 5},
        {"blend_shapes_rotation_deformers.target_indices", -1, offsets.blend_shapes_rotation_deformers->target_indices, counts.blend_shapes_rotation_deformers, {}, 4, false, offsets.mocVersion >= 5},
        {"blend_shapes_rotation_deformers.blend_shape_keyform_binding_sources_begin_indices", -1, offsets.blend_shapes_rotation_deformers->blend_shape_keyform_binding_sources_begin_indices, counts.blend_shapes_rotation_deformers, {}, 4, false, offsets.mocVersion >= 5},
        {"blend_shapes_rotation_deformers.blend_shape_keyform_binding_sources_counts", -1, offsets.blend_shapes_rotation_deformers->blend_shape_keyform_binding_sources_counts, counts.blend_shapes_rotation_deformers, {}, 4, false, offsets.mocVersion >= 5}
    }};
    countInfoOffset = offsets.count_info->count_info_offset;
    positionCountOffset = offsets.count_info->count_info_offset + PositionCountIndex * 4;
    maskCountOffset = offsets.count_info->count_info_offset + MaskCountIndex * 4;
    partCount = counts.parts;
    if (Word(bytes, positionCountOffset) != quint32(counts.keyform_positions))
        return Fail(error, QStringLiteral("预览位置数量与区段表不一致"));
    if (Word(bytes, maskCountOffset) != quint32(counts.drawable_masks))
        return Fail(error, QStringLiteral("预览蒙版数量与区段表不一致"));
    for (auto& column : columns) {
        if (!column.enabled) continue;
        const auto schema = std::find_if(spans.cbegin(), spans.cend(), [&](const auto& span) {
            return std::strcmp(span.name, column.name) == 0;
        });
        if (schema == spans.cend() || schema->slot < 0 || schema->offset != column.offset ||
            schema->count != column.count || schema->width != column.width)
            return Fail(error, QStringLiteral("预览区段与共享定义不一致：%1").arg(QString::fromLatin1(column.name)));
        column.slot = schema->slot;
        if (Word(bytes, column.slot) != quint32(column.offset))
            return Fail(error, QStringLiteral("预览区段 %1 与偏移表不一致").arg(QString::fromLatin1(column.name)));
        const qint64 end = qint64(column.offset) + qint64(column.count) * column.width;
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
    for (int index = 0; index < table.parameters.size(); ++index) {
        const QString& id = table.parameters[index]->ids;
        if (parameterIndices.contains(id)) parameterIndices[id] = -1;
        else parameterIndices.insert(id, index);
    }

    std::array<QVector<Reference>, 3> normal, all, orphan, blend;
    QVector<int> maskCoverage(counts.drawable_masks + 1, 0);
    for (const auto* mesh : table.art_meshes) {
        if (mesh->ids.isEmpty() || meshIndices.contains(mesh->ids))
            return Fail(error, QStringLiteral("ArtMesh ID 为空或重复：%1").arg(mesh->ids));
        if (mesh->vertex_counts < 0 || mesh->vertex_counts > MaximumCount / 2 ||
            !Range(mesh->keyform_sources_begin_indices, mesh->keyform_sources_counts, counts.art_mesh_keyforms))
            return Fail(error, QStringLiteral("ArtMesh 顶点或关键形范围无效：%1").arg(mesh->ids));
        if (!Range(mesh->drawable_mask_sources_begin_indices, mesh->drawable_mask_sources_counts, counts.drawable_masks))
            return Fail(error, QStringLiteral("ArtMesh 蒙版切片越界：%1").arg(mesh->ids));
        meshIndices.insert(mesh->ids, meshes.size());
        meshes.append({mesh->ids, {mesh->keyform_sources_begin_indices, mesh->keyform_sources_counts, mesh->vertex_counts},
                       mesh->drawable_mask_sources_begin_indices, mesh->drawable_mask_sources_counts});
        if (mesh->drawable_mask_sources_counts) {
            ++maskCoverage[mesh->drawable_mask_sources_begin_indices];
            --maskCoverage[mesh->drawable_mask_sources_begin_indices + mesh->drawable_mask_sources_counts];
        }
        normal[Index(Kind::ArtMesh)].append({mesh->keyform_sources_begin_indices, mesh->keyform_sources_counts, mesh->vertex_counts * 2});
    }
    QVector<int> sharedMaskPrefix(counts.drawable_masks + 1, 0);
    int maskOwners = 0;
    for (int index = 0; index < counts.drawable_masks; ++index) {
        maskOwners += maskCoverage[index];
        sharedMaskPrefix[index + 1] = sharedMaskPrefix[index] + (maskOwners > 1 ? 1 : 0);
    }
    for (auto& mesh : meshes) {
        if (!mesh.maskCount) continue;
        mesh.safeMasks = sharedMaskPrefix[mesh.maskBegin] == sharedMaskPrefix[mesh.maskBegin + mesh.maskCount] &&
            Get(ColumnId::Masks).Safe(mesh.maskBegin, qint64(mesh.maskCount) * 4);
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
    for (int index = 0; index < table.blend_shapes_rotation_deformers.size(); ++index) {
        const auto* shape = table.blend_shapes_rotation_deformers[index];
        if (shape->target_indices < 0 || shape->target_indices >= rotations.size() ||
            !Range(shape->blend_shape_keyform_binding_sources_begin_indices,
                   shape->blend_shape_keyform_binding_sources_counts, table.blend_shape_keyform_bindings.size()))
            return Fail(error, QStringLiteral("Rotation BlendShape 目标或绑定越界"));
        for (int at = 0; at < shape->blend_shape_keyform_binding_sources_counts; ++at) {
            const auto* binding = table.blend_shape_keyform_bindings[shape->blend_shape_keyform_binding_sources_begin_indices + at];
            if (!Range(binding->keyform_sources_blend_shape_indices, binding->keyform_sources_blend_shape_counts,
                       counts.rotation_deformer_keyforms))
                return Fail(error, QStringLiteral("Rotation BlendShape 关键形范围越界"));
            const Reference reference{binding->keyform_sources_blend_shape_indices, binding->keyform_sources_blend_shape_counts, 0};
            blend[Index(Kind::Rotation)].append(reference);
            all[Index(Kind::Rotation)].append(reference);
        }
    }

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

bool CMocPreviewBuilder::Snapshot::Detach(const QVector<ObjectEdit>& objects,
                                          const std::array<QHash<int, Request>, 3>& requests,
                                          QByteArray& output, bool& changed, QString* error) const
{
    changed = false;
    QVector<const ObjectEdit*> selected;
    for (const auto& object : objects) {
        bool isolate = false;
        for (int rowIndex : object.changedRows) {
            const auto& row = rows[Index(object.kind)][rowIndex];
            const auto& request = requests[Index(object.kind)].constFind(rowIndex).value();
            isolate = isolate || row.blendShape || row.orphanSource || !request.equal || request.owners != row.owners;
        }
        if (!isolate) continue;
        if (object.kind != Kind::ArtMesh &&
            (object.kind == Kind::Warp ? warps : rotations)[object.specific].owners > 1)
            return Fail(error, QStringLiteral("共享专用变形器源无法保证无损拆分：%1").arg(deformers[object.index].id));
        selected.append(&object);
    }
    if (selected.isEmpty()) return true;

    const std::array<QVector<ColumnId>, 3> groups{{
        {ColumnId::ArtOpacity, ColumnId::ArtDrawOrder, ColumnId::ArtBegin, ColumnId::ArtMultiplyIndex, ColumnId::ArtScreenIndex},
        {ColumnId::WarpOpacity, ColumnId::WarpBegin, ColumnId::WarpMultiplyIndex, ColumnId::WarpScreenIndex},
        {ColumnId::RotationOpacity, ColumnId::Angle, ColumnId::OriginX, ColumnId::OriginY,
         ColumnId::Scale, ColumnId::ReflectX, ColumnId::ReflectY, ColumnId::RotationMultiplyIndex, ColumnId::RotationScreenIndex}
    }};
    constexpr std::array<int, 3> countIndices{9, 7, 8};
    std::array<QVector<int>, 3> appended;
    Patches patches;
    const auto appendRows = [&](int group, int first, int count, int& destination) {
        const auto& column = Get(groups[group].first());
        const qint64 total = qint64(column.count) + appended[group].size() + count;
        if (!Range(first, count, column.count) || total > MaximumCount)
            return Fail(error, QStringLiteral("独立关键形数组超出范围：%1").arg(QString::fromLatin1(column.name)));
        destination = column.count + appended[group].size();
        for (int row = 0; row < count; ++row) appended[group].append(first + row);
        return true;
    };
    for (const auto* object : selected) {
        const Source& source = object->kind == Kind::ArtMesh ? meshes[object->index].source :
            (object->kind == Kind::Warp ? warps : rotations)[object->specific];
        int keyformBegin = 0;
        if (!appendRows(int(Index(object->kind)), source.firstRow, source.rowCount, keyformBegin)) return false;
        if (object->kind == Kind::ArtMesh) {
            patches[Index(ColumnId::ArtSourceBegin)].insert(object->index, EncodeWord(quint32(keyformBegin)));
            continue;
        }
        const ColumnId sourceBegin = object->kind == Kind::Warp ? ColumnId::WarpSourceBegin : ColumnId::RotationSourceBegin;
        patches[Index(sourceBegin)].insert(object->specific, EncodeWord(quint32(keyformBegin)));
    }

    std::array<int, ColumnCount> columnGroups;
    columnGroups.fill(-1);
    for (int group = 0; group < int(groups.size()); ++group)
        for (ColumnId column : groups[group]) columnGroups[Index(column)] = group;
    std::array<qint64, ColumnCount> destinations{};
    std::array<int, ColumnCount> sizes{};
    std::array<bool, ColumnCount> relocated{};
    qint64 finalSize = bytes.size();
    for (size_t index = 0; index < ColumnCount; ++index) {
        const auto& column = columns[index];
        const int group = columnGroups[index];
        const int extra = group < 0 ? 0 : appended[group].size();
        sizes[index] = column.count + extra;
        if (!column.enabled || (!extra && patches[index].isEmpty()) || (column.optional && !column.offset)) continue;
        for (auto it = patches[index].cbegin(); it != patches[index].cend(); ++it) {
            if (it.key() < 0 || it.key() >= sizes[index] || it->size() != column.width)
                return Fail(error, QStringLiteral("独立关键形字段补丁越界"));
        }
        relocated[index] = true;
        destinations[index] = Align64(finalSize);
        finalSize = destinations[index] + qint64(sizes[index]) * column.width;
    }
    finalSize = Align64(finalSize);
    if (finalSize > std::numeric_limits<int>::max()) return Fail(error, QStringLiteral("独立关键形文件长度超出范围"));
    QByteArray candidate = bytes;
    candidate.resize(finalSize);
    std::memset(candidate.data() + bytes.size(), 0, finalSize - bytes.size());
    for (int group = 0; group < int(groups.size()); ++group) {
        if (!appended[group].isEmpty())
            Put(candidate, countInfoOffset + countIndices[group] * 4,
                quint32(Get(groups[group].first()).count + appended[group].size()));
    }
    for (size_t index = 0; index < ColumnCount; ++index) {
        if (!relocated[index]) continue;
        const auto& column = columns[index];
        const qint64 destination = destinations[index];
        if (column.count)
            std::memcpy(candidate.data() + destination, bytes.constData() + column.offset, column.count * column.width);
        const int group = columnGroups[index];
        if (group >= 0) {
            for (int row = 0; row < appended[group].size(); ++row)
                std::memcpy(candidate.data() + destination + qint64(column.count + row) * column.width,
                            bytes.constData() + column.offset + qint64(appended[group][row]) * column.width, column.width);
        }
        for (auto it = patches[index].cbegin(); it != patches[index].cend(); ++it)
            std::memcpy(candidate.data() + destination + qint64(it.key()) * column.width, it->constData(), column.width);
        Put(candidate, column.slot, quint32(destination));
    }
    QMap<int, qint64> relocatedByteSizes;
    for (size_t index = 0; index < ColumnCount; ++index)
        if (relocated[index]) relocatedByteSizes.insert(columns[index].slot, qint64(sizes[index]) * columns[index].width);
    QByteArray ordered;
    if (!CMocLayoutRewriter::Rewrite(bytes, candidate, relocatedByteSizes, ordered, error)) return false;
    output = std::move(ordered);
    changed = true;
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
        if (parent != deformer.parent && parent != originalParents.value(deformer.id, deformer.parent) &&
            parent >= 0 && deformer.part != deformers[parent].part)
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

bool CMocPreviewBuilder::Snapshot::MaskPatches(const Overrides& overrides, Patches& patches,
                                              QMap<int, QByteArray>& masks, QString* error) const
{
    for (auto it = overrides.drawableMasks.cbegin(); it != overrides.drawableMasks.cend(); ++it)
        if (!meshIndices.contains(it.key())) return Fail(error, QStringLiteral("未知蒙版目标：%1").arg(it.key()));
    for (auto it = overrides.drawableMaskInverted.cbegin(); it != overrides.drawableMaskInverted.cend(); ++it)
        if (!meshIndices.contains(it.key())) return Fail(error, QStringLiteral("未知反转蒙版目标：%1").arg(it.key()));
    for (int index = 0; index < meshes.size(); ++index) {
        const auto& mesh = meshes[index];
        const auto inverted = overrides.drawableMaskInverted.constFind(mesh.id);
        if (inverted != overrides.drawableMaskInverted.cend()) {
            const auto original = Cell(ColumnId::DrawableFlags, index);
            const quint8 flags = static_cast<quint8>(original[0]);
            const quint8 value = (flags & quint8(~0x08)) | (inverted.value() ? 0x08 : 0);
            if (value != flags) patches[Index(ColumnId::DrawableFlags)].insert(index, QByteArray(1, char(value)));
        }
        const auto requested = overrides.drawableMasks.constFind(mesh.id);
        if (requested == overrides.drawableMasks.cend()) continue;
        if (requested->size() > MaximumCount) return Fail(error, QStringLiteral("蒙版列表数量超出范围：%1").arg(mesh.id));
        QByteArray encoded(requested->size() * 4, Qt::Uninitialized);
        for (int item = 0; item < requested->size(); ++item) Put(encoded, item * 4, quint32((*requested)[item]));
        const auto original = mesh.maskCount ?
            bytes.mid(Get(ColumnId::Masks).offset + mesh.maskBegin * 4, mesh.maskCount * 4) : QByteArray();
        if (encoded == original) continue;
        QSet<int> unique;
        for (int target : *requested) {
            if (target < 0 || target >= meshes.size() || target == index || unique.contains(target))
                return Fail(error, QStringLiteral("蒙版索引越界、自引用或重复：%1").arg(mesh.id));
            unique.insert(target);
        }
        if (requested->size() != mesh.maskCount)
            patches[Index(ColumnId::MaskCount)].insert(index, EncodeWord(quint32(requested->size())));
        if (!encoded.isEmpty()) masks.insert(index, std::move(encoded));
    }
    return true;
}

bool CMocPreviewBuilder::Snapshot::ParameterPatches(const QHash<QString, CModelParameterMetadata>& overrides,
                                                  Patches& patches, QString* error) const
{
    for (auto entry = overrides.cbegin(); entry != overrides.cend(); ++entry) {
        const auto found = parameterIndices.constFind(entry.key());
        if (entry.key().isEmpty() || found == parameterIndices.cend() || found.value() < 0)
            return Fail(error, QStringLiteral("参数 ID 不存在或重复：%1").arg(entry.key()));
        const auto& metadata = entry.value();
        if (!std::isfinite(metadata.minimum) || !std::isfinite(metadata.maximum) ||
            !std::isfinite(metadata.defaultValue) || metadata.minimum > metadata.maximum ||
            metadata.defaultValue < metadata.minimum || metadata.defaultValue > metadata.maximum ||
            metadata.decimalPlaces < 0 || metadata.decimalPlaces > 9)
            return Fail(error, QStringLiteral("参数范围、默认值或精度无效：%1").arg(entry.key()));
        const int row = found.value();
        const std::array<std::pair<ColumnId, QByteArray>, 5> fields{{
            {ColumnId::ParameterMax, FloatCell(ColumnId::ParameterMax, row, metadata.maximum)},
            {ColumnId::ParameterMin, FloatCell(ColumnId::ParameterMin, row, metadata.minimum)},
            {ColumnId::ParameterDefault, FloatCell(ColumnId::ParameterDefault, row, metadata.defaultValue)},
            {ColumnId::ParameterRepeat, BoolCell(ColumnId::ParameterRepeat, row, metadata.repeat)},
            {ColumnId::ParameterDecimal, EncodeWord(quint32(metadata.decimalPlaces))}
        }};
        for (const auto& field : fields)
            if (field.second != Cell(field.first, row)) patches[Index(field.first)].insert(row, field.second);
    }
    return true;
}

bool CMocPreviewBuilder::Snapshot::Write(const Patches& input, const QVector<Address>& positions,
                                        const std::array<QHash<int, Request>, 3>& requests,
                                        const QMap<int, QByteArray>& masks,
                                        QByteArray& output, QString* error) const
{
    struct PoolChange
    {
        ColumnId beginColumn;
        int row;
        int originalBegin;
        const QByteArray* values;
    };
    struct PoolWrite
    {
        ColumnId column;
        int countOffset;
        QVector<PoolChange> changes;
        bool relocate = false;
        qint64 extra = 0;
        qint64 offset = 0;
        int count = 0;
    };
    Patches patches = input;
    std::array<PoolWrite, 2> pools{{{ColumnId::Positions, positionCountOffset, {}},
                                   {ColumnId::Masks, maskCountOffset, {}}}};
    for (const auto& address : positions) {
        const auto& row = rows[Index(address.kind)][address.row];
        const auto& values = GetRequest(requests, address).fields[0];
        pools[0].relocate = pools[0].relocate || !row.safePosition;
        pools[0].extra += values.size() / 4;
        pools[0].changes.append({PositionBegin(address.kind), address.row, row.begin, &values});
    }
    for (auto it = masks.cbegin(); it != masks.cend(); ++it) {
        const auto& mesh = meshes[it.key()];
        pools[1].relocate = pools[1].relocate || !mesh.safeMasks || it->size() / 4 != mesh.maskCount;
        pools[1].extra += it->size() / 4;
        pools[1].changes.append({ColumnId::MaskBegin, it.key(), mesh.maskBegin, &it.value()});
    }
    qint64 finalSize = bytes.size();
    std::array<qint64, ColumnCount> destinations{};
    std::array<bool, ColumnCount> relocated{};
    for (auto& plan : pools) {
        const auto& column = Get(plan.column);
        plan.offset = column.offset;
        plan.count = column.count;
        if (!plan.relocate) continue;
        if (plan.extra > MaximumCount - column.count)
            return Fail(error, QStringLiteral("隔离共享数组后的预览数据超出范围：%1").arg(QString::fromLatin1(column.name)));
        plan.offset = Align64(finalSize);
        plan.count += int(plan.extra);
        finalSize = plan.offset + qint64(plan.count) * column.width;
        int begin = column.count;
        for (const auto& change : plan.changes) {
            patches[Index(change.beginColumn)].insert(change.row, EncodeWord(quint32(begin)));
            if (plan.column == ColumnId::Positions) relocated[Index(change.beginColumn)] = true;
            begin += change.values->size() / column.width;
        }
    }
    for (size_t index = 0; index < ColumnCount; ++index) {
        const auto& column = columns[index];
        destinations[index] = column.offset;
        if (patches[index].isEmpty()) continue;
        for (auto it = patches[index].cbegin(); it != patches[index].cend(); ++it) {
            if (it.key() < 0 || it.key() >= column.count || it->size() != column.width)
                return Fail(error, QStringLiteral("预览字段补丁越界"));
            relocated[index] = relocated[index] || !column.Safe(it.key(), column.width);
        }
        if (relocated[index]) {
            destinations[index] = Align64(finalSize);
            finalSize = destinations[index] + qint64(column.count) * column.width;
        }
    }
    if (finalSize != bytes.size()) finalSize = Align64(finalSize);
    if (finalSize > std::numeric_limits<int>::max()) return Fail(error, QStringLiteral("预览补丁文件长度超出范围"));
    QByteArray candidate = bytes;
    if (finalSize != bytes.size()) {
        candidate.resize(finalSize);
        std::memset(candidate.data() + bytes.size(), 0, finalSize - bytes.size());
    }
    for (const auto& plan : pools) {
        if (!plan.relocate) continue;
        const auto& column = Get(plan.column);
        if (column.count)
            std::memcpy(candidate.data() + plan.offset, bytes.constData() + column.offset, column.count * column.width);
        Put(candidate, column.slot, quint32(plan.offset));
        Put(candidate, plan.countOffset, quint32(plan.count));
    }
    for (size_t index = 0; index < ColumnCount; ++index) {
        const auto& column = columns[index];
        if (relocated[index]) {
            std::memcpy(candidate.data() + destinations[index], bytes.constData() + column.offset, column.count * column.width);
            Put(candidate, column.slot, quint32(destinations[index]));
        }
        for (auto it = patches[index].cbegin(); it != patches[index].cend(); ++it)
            std::memcpy(candidate.data() + destinations[index] + it.key() * column.width, it->constData(), column.width);
    }
    for (const auto& plan : pools) {
        const auto& column = Get(plan.column);
        qint64 appended = column.count;
        for (const auto& change : plan.changes) {
            const auto& values = *change.values;
            const qint64 begin = plan.relocate ? appended : change.originalBegin;
            std::memcpy(candidate.data() + plan.offset + begin * column.width, values.constData(), values.size());
            appended += values.size() / column.width;
        }
    }
    QMap<int, qint64> relocatedByteSizes;
    for (const auto& plan : pools)
        if (plan.relocate) relocatedByteSizes.insert(Get(plan.column).slot, qint64(plan.count) * Get(plan.column).width);
    for (size_t index = 0; index < ColumnCount; ++index)
        if (relocated[index]) relocatedByteSizes.insert(columns[index].slot, qint64(columns[index].count) * columns[index].width);
    if (!relocatedByteSizes.isEmpty()) {
        QByteArray ordered;
        if (!CMocLayoutRewriter::Rewrite(bytes, candidate, relocatedByteSizes, ordered, error)) return false;
        candidate.swap(ordered);
    }
    CReader reader = CReader::FromBytes(candidate);
    CSectionTable table(reader);
    if (!table.IsValid()) return Fail(error, QStringLiteral("预览补丁结构校验失败：%1").arg(reader.Error()));
    output = std::move(candidate);
    return true;
}

bool CMocPreviewBuilder::Initialize(const QByteArray& baseline, QString* error)
{
    return Initialize(baseline, {}, error);
}

bool CMocPreviewBuilder::Initialize(const QByteArray& baseline, const QHash<QString, int>& originalParents, QString* error)
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
        for (auto it = originalParents.cbegin(); it != originalParents.cend(); ++it) {
            const auto found = candidate->deformerIndices.constFind(it.key());
            if (found == candidate->deformerIndices.cend() || it.value() < -1 || it.value() >= candidate->deformers.size() ||
                it.value() == found.value())
                return Fail(error, QStringLiteral("原始父级来源无效：%1").arg(it.key()));
        }
        candidate->originalParents = originalParents;
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
    Overrides overrides;
    overrides.artMeshPositions = artMeshPositions;
    overrides.warpKeyforms = warpKeyforms;
    overrides.rotationKeyforms = rotationKeyforms;
    overrides.deformerParents = deformerParents;
    return BuildOverrides(overrides, result, error);
}

bool CMocPreviewBuilder::BuildOverrides(const Overrides& overrides, QByteArray& result, QString* error) const
{
    if (error) error->clear();
    if (!m_snapshot) return Fail(error, QStringLiteral("预览补丁尚未初始化"));
    try {
        std::unique_ptr<Snapshot> detached;
        const Snapshot* working = m_snapshot.get();
        for (int attempt = 0; attempt < 2; ++attempt) {
            const auto& state = *working;
            const auto& artMeshPositions = overrides.artMeshPositions;
            const auto& warpKeyforms = overrides.warpKeyforms;
            const auto& rotationKeyforms = overrides.rotationKeyforms;
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
            QMap<int, QByteArray> masks;
            if (!state.ParentPatches(overrides.deformerParents, patches, error) ||
                !state.MaskPatches(overrides, patches, masks, error) ||
                !state.ParameterPatches(overrides.parameterMetadata, patches, error)) return false;
            std::array<QHash<int, Request>, 3> requests;
            QVector<Address> order;
            QVector<ObjectEdit> objects;
            const auto collect = [&](ObjectEdit& object, int row, QVector<QByteArray> fields, const QVector<QByteArray>& original) {
                const Kind kind = object.kind;
                auto& table = requests[Index(kind)];
                if (!table.contains(row)) order.append({kind, row});
                auto& request = table[row];
                if (!request.owners) request.fields = fields;
                else request.equal = request.equal && request.fields == fields;
                ++request.owners;
                request.changed = request.changed || fields != original;
                if (fields != original) object.changedRows.append(row);
            };
            const auto finitePositions = [](const QVector<float>& values) {
                return std::all_of(values.cbegin(), values.cend(), [](float value) { return std::isfinite(value); });
            };
            for (const auto& mesh : state.meshes) {
                const auto found = artMeshPositions.constFind(mesh.id);
                if (found == artMeshPositions.cend()) continue;
                if (found->size() > mesh.source.rowCount) return Fail(error, QStringLiteral("关键形索引越界：%1").arg(mesh.id));
                ObjectEdit object{Kind::ArtMesh, state.meshIndices.value(mesh.id), -1, {}};
                for (int local = 0; local < found->size(); ++local) {
                    const auto& values = (*found)[local];
                    if (values.isEmpty()) continue;
                    if (values.size() != mesh.source.vertices * 2 || !finitePositions(values))
                        return Fail(error, QStringLiteral("关键形坐标数量或数值无效：%1[%2]").arg(mesh.id).arg(local));
                    const int row = mesh.source.firstRow + local;
                    collect(object, row, {EncodePositions(values)}, {state.Positions(Kind::ArtMesh, row, mesh.source.vertices)});
                }
                objects.append(std::move(object));
            }
            for (const auto& deformer : state.deformers) {
                if (deformer.type == 0 && warpKeyforms.contains(deformer.id)) {
                    const auto& forms = warpKeyforms[deformer.id];
                    const auto& source = state.warps[deformer.specific];
                    ObjectEdit object{Kind::Warp, state.deformerIndices.value(deformer.id), deformer.specific, {}};
                    const qint64 vertices = (qint64(source.gridRows) + 1) * (qint64(source.gridColumns) + 1);
                    if (source.gridRows < 0 || source.gridColumns < 0 || source.gridRows > 1000 || source.gridColumns > 1000 ||
                        vertices != source.vertices || forms.isEmpty() || forms.size() != source.rowCount)
                        return Fail(error, QStringLiteral("Warp 网格或关键形数量无效：%1").arg(deformer.id));
                    for (int local = 0; local < forms.size(); ++local) {
                        const auto& form = forms[local];
                        const int row = source.firstRow + local;
                        if (!Opacity(form.opacity) || form.positions.size() != source.vertices * 2 || !finitePositions(form.positions))
                            return Fail(error, QStringLiteral("Warp 关键形数值无效：%1[%2]").arg(deformer.id).arg(local));
                        collect(object, row, {EncodePositions(form.positions), state.FloatCell(ColumnId::WarpOpacity, row, form.opacity)},
                                {state.Positions(Kind::Warp, row, source.vertices), state.Cell(ColumnId::WarpOpacity, row)});
                    }
                    objects.append(std::move(object));
                } else if (deformer.type == 1 && rotationKeyforms.contains(deformer.id)) {
                    const auto& forms = rotationKeyforms[deformer.id];
                    const auto& source = state.rotations[deformer.specific];
                    ObjectEdit object{Kind::Rotation, state.deformerIndices.value(deformer.id), deformer.specific, {}};
                    if (forms.isEmpty() || forms.size() != source.rowCount)
                        return Fail(error, QStringLiteral("Rotation 关键形数量无效：%1").arg(deformer.id));
                    for (int local = 0; local < forms.size(); ++local) {
                        const auto& form = forms[local];
                        const int row = source.firstRow + local;
                        if (!form.IsValid())
                            return Fail(error, QStringLiteral("Rotation 关键形数值无效：%1[%2]").arg(deformer.id).arg(local));
                        QVector<QByteArray> original;
                        for (int field = 0; field < 7; ++field) original.append(state.Cell(RotationColumn(field), row));
                        collect(object, row, {state.FloatCell(ColumnId::RotationOpacity, row, form.opacity),
                            state.FloatCell(ColumnId::Angle, row, form.angle), state.FloatCell(ColumnId::OriginX, row, form.originX),
                            state.FloatCell(ColumnId::OriginY, row, form.originY), state.FloatCell(ColumnId::Scale, row, form.scale),
                            state.BoolCell(ColumnId::ReflectX, row, form.reflectX), state.BoolCell(ColumnId::ReflectY, row, form.reflectY)}, original);
                    }
                    objects.append(std::move(object));
                }
            }
            if (attempt == 0) {
                QByteArray structure;
                bool changed = false;
                if (!state.Detach(objects, requests, structure, changed, error)) return false;
                if (changed) {
                    CReader reader = CReader::FromBytes(structure);
                    CSectionTable table(reader);
                    if (!table.IsValid()) return Fail(error, QStringLiteral("独立关键形结构校验失败：%1").arg(reader.Error()));
                    detached = std::make_unique<Snapshot>();
                    detached->bytes = std::move(structure);
                    detached->digest = state.digest;
                    if (!detached->Read(table, error)) return false;
                    detached->originalParents = state.originalParents;
                    working = detached.get();
                    continue;
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
            if (!anyCells && changedPositions.isEmpty() && masks.isEmpty()) {
                result = state.bytes;
                return true;
            }
            return state.Write(patches, changedPositions, requests, masks, result, error);
        }
        return Fail(error, QStringLiteral("无法完成独立关键形补丁"));
    } catch (const std::bad_alloc&) {
        return Fail(error, QStringLiteral("预览补丁内存不足"));
    } catch (const std::length_error&) {
        return Fail(error, QStringLiteral("预览补丁长度超出范围"));
    }
}
}
