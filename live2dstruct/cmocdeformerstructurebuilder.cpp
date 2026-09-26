#include "cmocdeformerstructurebuilder.h"
#include "cparentgraph.h"
#include "csectiontable.h"
#include "csectionvalidation.h"
#include <QSet>
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <numeric>
#include <stdexcept>

namespace live2dstruct {
namespace {
using Builder = CMocDeformerStructureBuilder;
using Owner = Builder::Owner;
constexpr int MaximumCount = 1000000;

bool fail(QString* error, const QString& message) { if (error) *error = message; return false; }
QByteArray integer(int value)
{
    QByteArray bytes(4, Qt::Uninitialized); qToLittleEndian(qint32(value), bytes.data()); return bytes;
}
QByteArray floating(float value)
{
    quint32 bits; std::memcpy(&bits, &value, 4);
    QByteArray bytes(4, Qt::Uninitialized); qToLittleEndian(bits, bytes.data()); return bytes;
}
bool range(qint64 first, qint64 count, qint64 size)
{
    return first >= 0 && count >= 0 && first <= size && count <= size - first;
}
bool validId(const QString& id)
{
    const auto encoded = id.toUtf8();
    return !id.isEmpty() && encoded.size() < 64 && !encoded.contains('\0') && QString::fromUtf8(encoded) == id;
}

class Columns
{
public:
    Columns(const QByteArray& source, const CSectionTable& table) : bytes(source)
    {
        for (auto column : CSectionValidation::ArraySpans(*table.sectionOffsetTable, *table.count_info)) {
            column.width = CSectionValidation::StorageWidth(column);
            spans.insert(QByteArray(column.name), column);
            counts.insert(column.countIndex, int(column.count));
        }
    }
    const CSectionValidation::ColumnSpan& span(const QByteArray& name) const
    {
        const auto found = spans.constFind(name);
        if (found == spans.cend()) throw std::length_error("Unknown deformer column");
        return *found;
    }
    bool present(const QByteArray& name) const
    {
        const auto found = spans.constFind(name);
        return found != spans.cend() && (found->offset || state.columns.contains(found->slot));
    }
    int count(const QByteArray& name) const { return counts.value(span(name).countIndex); }
    QByteArray data(const QByteArray& name) const
    {
        const auto& column = span(name);
        const auto changed = state.columns.constFind(column.slot);
        if (changed != state.columns.cend()) return *changed;
        if (!column.offset) return {};
        if (!range(column.offset, column.count * column.width, bytes.size()))
            throw std::length_error("Incomplete deformer column storage");
        return bytes.mid(column.offset, column.count * column.width);
    }
    void set(const QByteArray& name, int row, int value)
    {
        const auto& column = span(name);
        auto payload = data(name);
        if (column.width != 4 || !range(row, 1, payload.size() / 4)) throw std::length_error("Invalid deformer cell");
        qToLittleEndian(qint32(value), payload.data() + row * 4);
        state.columns.insert(column.slot, std::move(payload));
    }
    int append(const QByteArray& group, const QMap<QByteArray, QByteArray>& fields, int copies = 1)
    {
        int countIndex = -1;
        for (auto it = spans.cbegin(); it != spans.cend(); ++it) {
            if (!it.key().startsWith(group + '.')) continue;
            const auto& column = *it;
            countIndex = column.countIndex;
            const int old = counts.value(countIndex);
            if (copies < 1 || copies > MaximumCount - old) throw std::length_error("Deformer row count limit");
            const auto field = fields.constFind(it.key().mid(group.size() + 1));
            if (column.optional && !column.offset && field == fields.cend()) continue;
            auto payload = data(it.key());
            if (!column.offset && column.optional && payload.isEmpty()) payload.fill('\0', old * column.width);
            if (payload.size() != qint64(old) * column.width) throw std::length_error("Deformer row group size mismatch");
            QByteArray added;
            if (field != fields.cend()) added = *field;
            else if (it.key().contains("color_sources_")) added = integer(-1);
            else added.fill('\0', column.width);
            if (added.size() != column.width) throw std::length_error("Deformer cell width mismatch");
            payload.append(added.repeated(copies));
            state.columns.insert(column.slot, std::move(payload));
        }
        if (countIndex < 0) throw std::length_error("Unknown deformer row group");
        const int result = counts.value(countIndex);
        state.counts[countIndex] = counts[countIndex] = result + copies;
        return result;
    }
    int appendPositions(const QVector<float>& positions)
    {
        const auto& column = span("keyform_positions.xys");
        const int first = counts.value(column.countIndex);
        if (positions.size() > MaximumCount - first) throw std::length_error("Deformer positions count limit");
        auto payload = data("keyform_positions.xys");
        if (payload.size() != qint64(first) * 4) throw std::length_error("Deformer positions length mismatch");
        for (float value : positions) payload.append(floating(value));
        state.columns.insert(column.slot, std::move(payload));
        state.counts[column.countIndex] = counts[column.countIndex] = first + positions.size();
        return first;
    }
    void removeGeneric(int index)
    {
        const int countIndex = span("deformers.ids").countIndex;
        const int old = counts.value(countIndex);
        if (!range(index, 1, old)) throw std::length_error("Invalid deformer removal");
        for (auto it = spans.cbegin(); it != spans.cend(); ++it) {
            if (!it.key().startsWith("deformers.")) continue;
            auto payload = data(it.key());
            if (payload.size() != qint64(old) * it->width) throw std::length_error("Incomplete deformer removal column");
            payload.remove(qint64(index) * it->width, it->width);
            state.columns.insert(it->slot, std::move(payload));
        }
        state.counts[countIndex] = counts[countIndex] = old - 1;
    }
    CMocParameterStructureBuilder::ColumnState state;
private:
    QByteArray bytes;
    QMap<QByteArray, CSectionValidation::ColumnSpan> spans;
    QMap<int, int> counts;
};

struct Catalog
{
    QHash<QString, int> deformers, meshes;
    QVector<int> parents;
};

bool catalog(const CSectionTable& table, Catalog& result, QString* error)
{
    for (int index = 0; index < table.deformers.size(); ++index) {
        const auto* node = table.deformers[index];
        if (!validId(node->ids) || result.deformers.contains(node->ids) || (node->types != 0 && node->types != 1) ||
            node->parent_part_indices < -1 || node->parent_part_indices >= table.parts.size())
            return fail(error, QStringLiteral("变形器标识、类型或所属部件无效：%1").arg(node->ids));
        const auto count = node->types == 0 ? table.warp_deformers.size() : table.rotation_deformers.size();
        if (node->specific_sources_indices < 0 || node->specific_sources_indices >= count)
            return fail(error, QStringLiteral("变形器所属源无效：%1").arg(node->ids));
        result.deformers.insert(node->ids, index);
        result.parents.append(node->parent_deformer_indices);
    }
    if (!CParentGraph(result.parents).IsValid()) return fail(error, QStringLiteral("变形器父链缺失或形成循环"));
    for (int index = 0; index < table.art_meshes.size(); ++index) {
        const auto* mesh = table.art_meshes[index];
        if (!validId(mesh->ids) || result.meshes.contains(mesh->ids) || mesh->parent_deformer_indices < -1 ||
            mesh->parent_deformer_indices >= table.deformers.size())
            return fail(error, QStringLiteral("网格标识或父变形器无效：%1").arg(mesh->ids));
        result.meshes.insert(mesh->ids, index);
    }
    return true;
}

Owner deformerOwner(const CDeformers& node)
{
    return {node.types == 0 ? Owner::Warp : Owner::Rotation, node.ids};
}

bool prepareInsert(const CSectionTable& table, const Catalog& ids, const Builder::InsertRequest& request,
                   Builder::Impact& impact, QVector<float>& positions, QString* error)
{
    if (!validId(request.id) || ids.deformers.contains(request.id))
        return fail(error, QStringLiteral("新变形器 ID 必须唯一、非空且 UTF-8 长度小于 64 字节"));
    if (request.partIndex < -1 || request.partIndex >= table.parts.size() ||
        (!request.parentId.isEmpty() && !ids.deformers.contains(request.parentId)))
        return fail(error, QStringLiteral("新变形器所属部件或父级不存在"));
    const int parent = ids.deformers.value(request.parentId, -1);
    if (parent >= 0 && table.deformers[parent]->parent_part_indices != request.partIndex)
        return fail(error, QStringLiteral("新变形器必须与父变形器属于同一部件"));
    if (request.shape.valueless_by_exception()) return fail(error, QStringLiteral("新变形器形状无效"));
    if (const auto* warp = std::get_if<Builder::Warp>(&request.shape)) {
        if (warp->quad && table.sectionOffsetTable->mocVersion == 1)
            return fail(error, QStringLiteral("当前 MOC 版本仅支持三角插值，请关闭四边形插值"));
        if (warp->rows < 1 || warp->columns < 1 || warp->rows >= MaximumCount || warp->columns >= MaximumCount)
            return fail(error, QStringLiteral("Warp 网格行列数无效"));
        const qint64 count = (qint64(warp->rows) + 1) * (qint64(warp->columns) + 1);
        if (count > MaximumCount / 2 || count * 2 > MaximumCount - table.count_info->keyform_positions)
            return fail(error, QStringLiteral("Warp 控制点数量超出范围"));
        if (warp->positions.isEmpty()) {
            positions.reserve(count * 2);
            for (int row = 0; row <= warp->rows; ++row) for (int column = 0; column <= warp->columns; ++column) {
                positions.append(float(column) / warp->columns);
                positions.append(float(row) / warp->rows);
            }
        } else {
            if (warp->positions.size() != count * 2 || !std::all_of(warp->positions.cbegin(), warp->positions.cend(),
                [](float value) { return std::isfinite(value); }))
                return fail(error, QStringLiteral("Warp 控制点数量或坐标无效"));
            positions = warp->positions;
        }
    } else {
        const auto& rotation = std::get<Builder::Rotation>(request.shape);
        for (float value : {rotation.originX, rotation.originY, rotation.angle, rotation.baseAngle, rotation.scale})
            if (!std::isfinite(value)) return fail(error, QStringLiteral("Rotation 原点、角度或缩放无效"));
    }
    QSet<QPair<int, QString>> seen;
    for (const auto& owner : request.children) {
        if (seen.contains({int(owner.type), owner.id})) return fail(error, QStringLiteral("选区包含重复对象：%1").arg(owner.id));
        seen.insert({int(owner.type), owner.id});
        int ownerParent = -2, part = -2;
        if (owner.type == Owner::ArtMesh && ids.meshes.contains(owner.id)) {
            const auto* mesh = table.art_meshes[ids.meshes.value(owner.id)];
            ownerParent = mesh->parent_deformer_indices; part = mesh->parent_part_indices;
        } else if ((owner.type == Owner::Warp || owner.type == Owner::Rotation) && ids.deformers.contains(owner.id)) {
            const auto* node = table.deformers[ids.deformers.value(owner.id)];
            if (deformerOwner(*node).type != owner.type) return fail(error, QStringLiteral("选区变形器类型不一致：%1").arg(owner.id));
            ownerParent = node->parent_deformer_indices; part = node->parent_part_indices;
        }
        if (ownerParent != parent || part != request.partIndex)
            return fail(error, QStringLiteral("所选对象必须存在并具有相同的部件和直接父级：%1").arg(owner.id));
    }
    impact.id = request.id; impact.parentId = request.parentId; impact.partIndex = request.partIndex;
    impact.insertedIndex = table.deformers.size(); impact.reparented = request.children;
    impact.oldDeformerToNew.resize(table.deformers.size());
    std::iota(impact.oldDeformerToNew.begin(), impact.oldDeformerToNew.end(), 0);
    return true;
}

int emptyBinding(const CSectionTable& table, Columns& columns)
{
    for (int index = 0; index < table.keyform_bindings.size(); ++index)
        if (table.keyform_bindings[index]->parameter_binding_index_sources_counts == 0) return index;
    return columns.append("keyform_bindings", {{"parameter_binding_index_sources_begin_indices", integer(0)},
                                              {"parameter_binding_index_sources_counts", integer(0)}});
}

class ColorColumns
{
public:
    ColorColumns(const Columns& columns, const QByteArray& group) : count(columns.count(group + ".r"))
    {
        for (const auto* channel : {".r", ".g", ".b"}) {
            const auto data = columns.data(group + channel);
            if (data.size() != qint64(count) * 4) throw std::length_error("Incomplete deformer color pool");
            channels.append(data);
        }
    }
    bool matches(int index, float expected) const
    {
        if (index < 0 || index >= count) return false;
        for (const auto& channel : channels) {
            const auto bits = qFromLittleEndian<quint32>(channel.constData() + index * 4);
            float value; std::memcpy(&value, &bits, 4);
            if (value != expected) return false;
        }
        return true;
    }
    int count;
private:
    QVector<QByteArray> channels;
};

int appendColor(Columns& columns, const QByteArray& group, float value, int count = 1)
{
    return columns.append(group, {{"r", floating(value)}, {"g", floating(value)}, {"b", floating(value)}}, count);
}

void neutralColors(Columns& columns, const QByteArray& specific, const QByteArray& keyforms,
                   QMap<QByteArray, QByteArray>& definition, QMap<QByteArray, QByteArray>& form)
{
    const bool legacy = columns.present(specific + ".keyform_color_sources_begin_indices");
    const bool multiply = columns.present(keyforms + ".multiply_color_sources_indices");
    const bool screen = columns.present(keyforms + ".screen_color_sources_indices");
    if (!legacy && !multiply && !screen) return;
    const QByteArray mulGroup("keyform_multiply_colors"), screenGroup("keyform_screen_colors");
    const ColorColumns mulColumns(columns, mulGroup), screenColumns(columns, screenGroup);
    int common = -1;
    if (legacy) {
        const int paired = std::min(mulColumns.count, screenColumns.count);
        for (int index = 0; index < paired; ++index)
            if (mulColumns.matches(index, 1) && screenColumns.matches(index, 0)) { common = index; break; }
        if (common < 0) {
            common = std::max(mulColumns.count, screenColumns.count);
            appendColor(columns, mulGroup, 1, common - mulColumns.count + 1);
            appendColor(columns, screenGroup, 0, common - screenColumns.count + 1);
        }
        definition.insert("keyform_color_sources_begin_indices", integer(common));
    }
    const auto indexFor = [&](const QByteArray& group, const ColorColumns& source, float value) {
        if (common >= 0) return common;
        for (int index = 0; index < source.count; ++index)
            if (source.matches(index, value)) return index;
        return appendColor(columns, group, value);
    };
    if (multiply) form.insert("multiply_color_sources_indices", integer(indexFor(mulGroup, mulColumns, 1)));
    if (screen) form.insert("screen_color_sources_indices", integer(indexFor(screenGroup, screenColumns, 0)));
}

bool insert(const QByteArray& base, const CSectionTable& table, const Catalog& ids,
             const Builder::InsertRequest& request, Builder::Result& result, QString* error)
{
    Builder::Result candidate;
    QVector<float> positions;
    if (!prepareInsert(table, ids, request, candidate.impact, positions, error)) return false;
    Columns columns(base, table);
    const int binding = emptyBinding(table, columns);
    int specific, type;
    if (const auto* warp = std::get_if<Builder::Warp>(&request.shape)) {
        type = 0;
        const int position = columns.appendPositions(positions);
        QMap<QByteArray, QByteArray> form{{"opacities", floating(1)}, {"keyform_position_sources_begin_indices", integer(position)}};
        QMap<QByteArray, QByteArray> fields{{"keyform_binding_sources_indices", integer(binding)},
            {"keyform_sources_counts", integer(1)},
            {"vertex_counts", integer(positions.size() / 2)}, {"rows", integer(warp->rows)}, {"columns", integer(warp->columns)}};
        neutralColors(columns, "warp_deformers", "warp_deformer_keyforms", fields, form);
        const int keyform = columns.append("warp_deformer_keyforms", form);
        fields.insert("keyform_sources_begin_indices", integer(keyform));
        if (warp->quad || table.sectionOffsetTable->warp_deformers->is_quad_source) fields.insert("is_quad_source", integer(warp->quad));
        specific = columns.append("warp_deformers", fields);
    } else {
        type = 1;
        const auto& rotation = std::get<Builder::Rotation>(request.shape);
        QMap<QByteArray, QByteArray> form{{"opacities", floating(1)},
            {"angles", floating(rotation.angle)}, {"origin_x", floating(rotation.originX)}, {"origin_y", floating(rotation.originY)},
            {"scales", floating(rotation.scale)}, {"is_reflect_x", integer(rotation.reflectX)}, {"is_reflect_y", integer(rotation.reflectY)}};
        QMap<QByteArray, QByteArray> fields{{"keyform_binding_sources_indices", integer(binding)},
            {"keyform_sources_counts", integer(1)}, {"base_angles", floating(rotation.baseAngle)}};
        neutralColors(columns, "rotation_deformers", "rotation_deformer_keyforms", fields, form);
        const int keyform = columns.append("rotation_deformer_keyforms", form);
        fields.insert("keyform_sources_begin_indices", integer(keyform));
        specific = columns.append("rotation_deformers", fields);
    }
    auto encoded = request.id.toUtf8(); encoded.resize(64, '\0');
    const int added = columns.append("deformers", {{"ids", encoded}, {"keyform_binding_sources_indices", integer(binding)},
        {"is_visible", integer(1)}, {"is_enabled", integer(1)}, {"parent_part_indices", integer(request.partIndex)},
        {"parent_deformer_indices", integer(ids.deformers.value(request.parentId, -1))}, {"types", integer(type)},
        {"specific_sources_indices", integer(specific)}});
    for (const auto& owner : request.children) {
        if (owner.type == Owner::ArtMesh) columns.set("art_meshes.parent_deformer_indices", ids.meshes.value(owner.id), added);
        else columns.set("deformers.parent_deformer_indices", ids.deformers.value(owner.id), added);
    }
    if (!CMocParameterStructureBuilder::BuildColumnState(base, columns.state, candidate.bytes, error)) return false;
    candidate.state = std::move(columns.state);
    result = std::move(candidate);
    return true;
}

bool remove(const QByteArray& base, const CSectionTable& table, const Catalog& ids,
             const Builder::RemoveRequest& request, Builder::Result& result, QString* error)
{
    if (!ids.deformers.contains(request.id)) return fail(error, QStringLiteral("要删除的变形器不存在：%1").arg(request.id));
    const int removed = ids.deformers.value(request.id);
    const auto* node = table.deformers[removed];
    const int parent = node->parent_deformer_indices;
    Builder::Result candidate;
    auto& impact = candidate.impact;
    impact.id = request.id; impact.partIndex = node->parent_part_indices;
    if (parent >= 0) impact.parentId = table.deformers[parent]->ids;
    impact.oldDeformerToNew.reserve(table.deformers.size());
    for (int index = 0; index < table.deformers.size(); ++index)
        impact.oldDeformerToNew.append(index == removed ? -1 : index - int(index > removed));
    const auto nextParent = [&](int old) {
        if (old == removed) old = parent;
        return old < 0 ? -1 : impact.oldDeformerToNew[old];
    };
    Columns columns(base, table);
    for (int index = 0; index < table.deformers.size(); ++index) {
        if (index == removed) continue;
        const auto* child = table.deformers[index];
        if (child->parent_deformer_indices == removed) impact.reparented.append(deformerOwner(*child));
        const int next = nextParent(child->parent_deformer_indices);
        if (next != child->parent_deformer_indices) columns.set("deformers.parent_deformer_indices", index, next);
    }
    for (int index = 0; index < table.art_meshes.size(); ++index) {
        const auto* mesh = table.art_meshes[index];
        if (mesh->parent_deformer_indices == removed) impact.reparented.append({Owner::ArtMesh, mesh->ids});
        const int next = nextParent(mesh->parent_deformer_indices);
        if (next != mesh->parent_deformer_indices) columns.set("art_meshes.parent_deformer_indices", index, next);
    }
    columns.removeGeneric(removed);
    if (!CMocParameterStructureBuilder::BuildColumnState(base, columns.state, candidate.bytes, error)) return false;
    candidate.state = std::move(columns.state);
    result = std::move(candidate);
    return true;
}

template<class Operation> bool execute(const QByteArray& base, QString* error, Operation&& operation)
{
    if (error) error->clear();
    try {
        auto reader = CReader::FromBytes(base); CSectionTable table(reader);
        if (!table.IsValid() || base.size() < 8 || quint8(base[5]) != 0)
            return fail(error, QStringLiteral("变形器结构需要有效的小端 MOC：%1").arg(reader.Error()));
        Catalog ids;
        return catalog(table, ids, error) && operation(table, ids);
    } catch (const std::exception&) {
        return fail(error, QStringLiteral("变形器结构准备失败，原模型保持不变"));
    }
}
}

bool CMocDeformerStructureBuilder::AnalyzeInsert(const QByteArray& base, const InsertRequest& request, Impact& result, QString* error)
{
    Result candidate;
    if (!Insert(base, request, candidate, error)) return false;
    result = std::move(candidate.impact);
    return true;
}

bool CMocDeformerStructureBuilder::Insert(const QByteArray& base, const InsertRequest& request, Result& result, QString* error)
{
    return execute(base, error, [&](const CSectionTable& table, const Catalog& ids) { return insert(base, table, ids, request, result, error); });
}

bool CMocDeformerStructureBuilder::AnalyzeRemove(const QByteArray& base, const RemoveRequest& request, Impact& result, QString* error)
{
    Result candidate;
    if (!Remove(base, request, candidate, error)) return false;
    result = std::move(candidate.impact);
    return true;
}

bool CMocDeformerStructureBuilder::Remove(const QByteArray& base, const RemoveRequest& request, Result& result, QString* error)
{
    return execute(base, error, [&](const CSectionTable& table, const Catalog& ids) { return remove(base, table, ids, request, result, error); });
}
}
