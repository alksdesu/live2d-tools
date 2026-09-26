#include "cmocruntimebuilder.h"
#include "cmocparameterstructurebuilder.h"
#include "cmeshtopology.h"
#include "cparentgraph.h"
#include "csectiontable.h"
#include "csectionvalidation.h"
#include "cartmeshgeometrylayout.h"
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <exception>
#include <limits>

namespace live2dstruct {
namespace {
constexpr int MaximumCount = 1000000;

bool fail(QString* error, const QString& message)
{
    if (error) *error = message;
    return false;
}

bool range(int first, int count, int size)
{
    return first >= 0 && count >= 0 && first <= size && count <= size - first;
}

class Columns
{
public:
    Columns(const QByteArray& bytes, const CSectionTable& table) : source(bytes), spans(
        CSectionValidation::ArraySpans(*table.sectionOffsetTable, *table.count_info))
    {
        for (int index = 0; index < spans.size(); ++index) {
            names.insert(QByteArray(spans[index].name), index);
            counts.insert(spans[index].countIndex, int(spans[index].count));
        }
    }

    const CSectionValidation::ColumnSpan& span(const char* name) const { return spans[names.value(name, -1)]; }
    QByteArray data(const char* name) const { return data(span(name)); }
    QByteArray data(const CSectionValidation::ColumnSpan& column) const
    {
        const auto changed = state.columns.constFind(column.slot);
        return changed == state.columns.cend() ? (column.offset ? source.mid(column.offset, column.count * column.width) : QByteArray()) : *changed;
    }
    QByteArray& writable(const CSectionValidation::ColumnSpan& column)
    {
        auto changed = state.columns.find(column.slot);
        if (changed == state.columns.end()) changed = state.columns.insert(column.slot, data(column));
        return *changed;
    }
    int integer(const char* name, int row) const
    {
        const auto& column = span(name);
        const auto changed = state.columns.constFind(column.slot);
        const auto* bytes = changed == state.columns.cend() ? source.constData() + column.offset : changed->constData();
        return qFromLittleEndian<qint32>(bytes + row * 4);
    }
    float real(const char* name, int row) const
    {
        const quint32 bits = quint32(integer(name, row));
        float value;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }
    void setInteger(const char* name, int row, int value)
    {
        const auto& column = span(name);
        auto& bytes = writable(column);
        qToLittleEndian(qint32(value), bytes.data() + row * 4);
    }
    void setReal(const char* name, int row, float value)
    {
        qint32 bits;
        std::memcpy(&bits, &value, sizeof(bits));
        setInteger(name, row, bits);
    }
    bool appendRows(int countIndex, int first, int size, int& destination, QString* error)
    {
        const int previous = counts.value(countIndex, -1);
        if (!range(first, size, previous) || size > MaximumCount - previous)
            return fail(error, QStringLiteral("运行时关键形区间或数量无效"));
        destination = previous;
        for (const auto& column : spans) {
            if (column.countIndex != countIndex || !column.offset) continue;
            auto& bytes = writable(column);
            const auto rows = bytes.mid(qint64(first) * column.width, qint64(size) * column.width);
            if (rows.size() != qint64(size) * column.width || bytes.size() != qint64(previous) * column.width)
                return fail(error, QStringLiteral("运行时列长度不一致：%1").arg(QString::fromLatin1(column.name)));
            bytes.append(rows);
        }
        counts[countIndex] = previous + size;
        state.counts.insert(countIndex, previous + size);
        return true;
    }
    bool appendIndices(const QVector<quint16>& indices, int& destination, QString* error)
    {
        const auto& column = span("position_indices.indices");
        auto& bytes = writable(column);
        const int previous = counts.value(column.countIndex);
        if (indices.size() > MaximumCount - previous || bytes.size() != qint64(previous) * 2)
            return fail(error, QStringLiteral("运行时三角索引数量超出范围"));
        destination = previous;
        for (quint16 index : indices) {
            char encoded[2];
            qToLittleEndian(index, encoded);
            bytes.append(encoded, 2);
        }
        counts[column.countIndex] = previous + indices.size();
        state.counts.insert(column.countIndex, previous + indices.size());
        return true;
    }

    CMocParameterStructureBuilder::ColumnState state;
    QMap<int, int> counts;
private:
    const QByteArray& source;
    QVector<CSectionValidation::ColumnSpan> spans;
    QHash<QByteArray, int> names;
};

bool visibility(const CSectionTable& table, const QMap<QString, bool>& values,
                Columns& columns, QVector<bool>& hidden, QString* error)
{
    hidden.fill(false, table.art_meshes.size());
    if (values.isEmpty()) return true;
    QSet<QString> keys;
    const auto visible = [&](const QString& prefix, const QString& id, bool original, bool& result) {
        const auto key = prefix + id;
        if (id.isEmpty() || keys.contains(key)) return fail(error, QStringLiteral("模型对象 ID 无效或重复：%1").arg(key));
        keys.insert(key);
        result = values.value(key, original);
        return true;
    };
    QVector<int> partParents, deformerParents;
    QVector<bool> partHidden(table.parts.size()), deformerHidden(table.deformers.size());
    for (int index = 0; index < table.parts.size(); ++index) {
        const auto* part = table.parts[index];
        bool shown;
        if (!visible(QStringLiteral("part|"), part->ids, part->is_visible, shown)) return false;
        if (shown != part->is_visible) columns.setInteger("parts.is_visible", index, shown);
        partHidden[index] = !shown;
        partParents.append(part->parent_part_indices);
    }
    const CParentGraph parts(partParents);
    if (!parts.IsValid()) return fail(error, QStringLiteral("Part 父级关系无效"));
    for (int index : parts.TopologicalOrder())
        if (partParents[index] >= 0) partHidden[index] = partHidden[index] || partHidden[partParents[index]];
    for (int index = 0; index < table.deformers.size(); ++index) {
        const auto* deformer = table.deformers[index];
        if (deformer->types != 0 && deformer->types != 1)
            return fail(error, QStringLiteral("变形器类型不受支持：%1").arg(deformer->ids));
        if (deformer->parent_part_indices < -1 || deformer->parent_part_indices >= table.parts.size())
            return fail(error, QStringLiteral("变形器 Part 索引无效：%1").arg(deformer->ids));
        bool shown;
        if (!visible(deformer->types ? QStringLiteral("rotation|") : QStringLiteral("warp|"),
                     deformer->ids, deformer->is_visible, shown)) return false;
        if (shown != deformer->is_visible) columns.setInteger("deformers.is_visible", index, shown);
        deformerHidden[index] = !shown;
        deformerParents.append(deformer->parent_deformer_indices);
    }
    const CParentGraph deformers(deformerParents);
    if (!deformers.IsValid()) return fail(error, QStringLiteral("变形器父级关系无效"));
    for (int index : deformers.TopologicalOrder())
        if (deformerParents[index] >= 0) deformerHidden[index] = deformerHidden[index] || deformerHidden[deformerParents[index]];
    for (int index = 0; index < table.art_meshes.size(); ++index) {
        const auto* mesh = table.art_meshes[index];
        if (mesh->parent_part_indices < -1 || mesh->parent_part_indices >= table.parts.size() ||
            mesh->parent_deformer_indices < -1 || mesh->parent_deformer_indices >= table.deformers.size())
            return fail(error, QStringLiteral("ArtMesh 父级索引无效：%1").arg(mesh->ids));
        bool shown;
        if (!visible(QStringLiteral("artmesh|"), mesh->ids, mesh->is_visible, shown)) return false;
        if (shown != mesh->is_visible) columns.setInteger("art_meshes.is_visible", index, shown);
        hidden[index] = !shown || (mesh->parent_part_indices >= 0 && partHidden[mesh->parent_part_indices]) ||
            (mesh->parent_deformer_indices >= 0 && deformerHidden[mesh->parent_deformer_indices]);
    }
    for (auto value = values.cbegin(); value != values.cend(); ++value)
        if (!keys.contains(value.key())) return fail(error, QStringLiteral("可见性对象不存在：%1").arg(value.key()));
    return true;
}

bool blendRows(const CSectionTable& table, Columns& columns, QVector<QVector<int>>& shapes, QString* error)
{
    shapes.resize(table.art_meshes.size());
    for (int shape = 0; shape < table.blend_shapes_art_meshes.size(); ++shape) {
        const auto* value = table.blend_shapes_art_meshes[shape];
        if (!range(value->target_indices, 1, table.art_meshes.size()) ||
            !range(value->blend_shape_keyform_binding_sources_begin_indices, value->blend_shape_keyform_binding_sources_counts,
                   table.blend_shape_keyform_bindings.size())) return fail(error, QStringLiteral("ArtMesh BlendShape 目标或绑定区间无效"));
        shapes[value->target_indices].append(shape);
        for (int binding = value->blend_shape_keyform_binding_sources_begin_indices;
             binding < value->blend_shape_keyform_binding_sources_begin_indices + value->blend_shape_keyform_binding_sources_counts; ++binding)
            if (!range(columns.integer("blend_shape_keyform_bindings.keyform_sources_blend_shape_indices", binding),
                       columns.integer("blend_shape_keyform_bindings.keyform_sources_blend_shape_counts", binding), table.art_mesh_keyforms.size()))
                return fail(error, QStringLiteral("ArtMesh BlendShape 关键形区间无效"));
    }
    return true;
}

bool hideBlendShapes(const CSectionTable& table, const QVector<int>& shapes, Columns& columns, QString* error)
{
    for (int shape : shapes) {
        const auto* source = table.blend_shapes_art_meshes[shape];
        const int first = source->blend_shape_keyform_binding_sources_begin_indices;
        const int count = source->blend_shape_keyform_binding_sources_counts;
        bool changed = false;
        for (int binding = first; binding < first + count; ++binding) {
            const int begin = columns.integer("blend_shape_keyform_bindings.keyform_sources_blend_shape_indices", binding);
            const int size = columns.integer("blend_shape_keyform_bindings.keyform_sources_blend_shape_counts", binding);
            for (int row = begin; row < begin + size; ++row) changed = changed || columns.real("art_mesh_keyforms.opacities", row) != 0;
        }
        if (!changed) continue;
        int nextBinding;
        if (!columns.appendRows(columns.span("blend_shape_keyform_bindings.parameter_binding_sources_indices").countIndex,
                                first, count, nextBinding, error)) return false;
        columns.setInteger("blend_shapes_art_meshes.blend_shape_keyform_binding_sources_begin_indices", shape, nextBinding);
        for (int binding = nextBinding; binding < nextBinding + count; ++binding) {
            const int begin = columns.integer("blend_shape_keyform_bindings.keyform_sources_blend_shape_indices", binding);
            const int size = columns.integer("blend_shape_keyform_bindings.keyform_sources_blend_shape_counts", binding);
            int nextRow;
            if (!columns.appendRows(columns.span("art_mesh_keyforms.opacities").countIndex, begin, size, nextRow, error)) return false;
            columns.setInteger("blend_shape_keyform_bindings.keyform_sources_blend_shape_indices", binding, nextRow);
            for (int row = nextRow; row < nextRow + size; ++row) columns.setReal("art_mesh_keyforms.opacities", row, 0);
        }
    }
    return true;
}

bool updateOrderRanges(const CSectionTable& table, const QHash<int, int>& edits,
                       const QVector<QVector<int>>& shapes, Columns& columns, QString* error)
{
    if (edits.isEmpty()) return true;
    QHash<int, int> groups;
    QVector<int> parents(table.draw_order_groups.size(), -1);
    QSet<int> groupedParts;
    for (int group = 0; group < table.draw_order_groups.size(); ++group) {
        const auto* value = table.draw_order_groups[group];
        if (!range(value->object_sources_begin_indices, value->object_sources_counts, table.draw_order_group_objects.size()) ||
            value->minimum_draw_orders > value->maximum_draw_orders)
            return fail(error, QStringLiteral("绘制顺序组定义无效"));
        for (int row = value->object_sources_begin_indices; row < value->object_sources_begin_indices + value->object_sources_counts; ++row) {
            const auto* object = table.draw_order_group_objects[row];
            if (object->types == 0) {
                if (!range(object->indices, 1, table.art_meshes.size()) || groups.contains(object->indices))
                    return fail(error, QStringLiteral("绘制顺序组的 ArtMesh 引用无效或重复"));
                groups.insert(object->indices, group);
            } else {
                if (object->types != 1 || !range(object->indices, 1, table.parts.size()) ||
                    !range(object->self_indices, 1, table.draw_order_groups.size()) ||
                    groupedParts.contains(object->indices) || parents[object->self_indices] != -1)
                    return fail(error, QStringLiteral("绘制顺序组的 Part 引用无效"));
                groupedParts.insert(object->indices);
                parents[object->self_indices] = group;
            }
        }
    }
    if (!CParentGraph(parents).IsValid()) return fail(error, QStringLiteral("绘制顺序组包含循环"));
    for (auto edit = edits.cbegin(); edit != edits.cend(); ++edit) {
        if (!groups.contains(edit.key())) return fail(error, QStringLiteral("ArtMesh 缺少绘制顺序组"));
        double minimum = edit.value(), maximum = edit.value();
        for (int shape : shapes[edit.key()]) {
            const auto* source = table.blend_shapes_art_meshes[shape];
            for (int binding = source->blend_shape_keyform_binding_sources_begin_indices;
                 binding < source->blend_shape_keyform_binding_sources_begin_indices + source->blend_shape_keyform_binding_sources_counts; ++binding) {
                const int begin = columns.integer("blend_shape_keyform_bindings.keyform_sources_blend_shape_indices", binding);
                const int size = columns.integer("blend_shape_keyform_bindings.keyform_sources_blend_shape_counts", binding);
                double low = 0, high = 0;
                for (int row = begin; row < begin + size; ++row) {
                    const float value = columns.real("art_mesh_keyforms.draw_orders", row);
                    if (!std::isfinite(value)) return fail(error, QStringLiteral("BlendShape 绘制顺序含非有限值"));
                    low = std::min(low, double(value)); high = std::max(high, double(value));
                }
                minimum += low; maximum += high;
            }
        }
        const int group = groups[edit.key()];
        minimum = std::min(minimum, double(columns.integer("draw_order_groups.minimum_draw_orders", group)));
        maximum = std::max(maximum, double(columns.integer("draw_order_groups.maximum_draw_orders", group)));
        minimum = std::floor(minimum); maximum = std::ceil(maximum);
        if (minimum < std::numeric_limits<int>::min() || maximum > std::numeric_limits<int>::max() || maximum - minimum > MaximumCount)
            return fail(error, QStringLiteral("绘制顺序组范围过大，无法安全生成运行时模型"));
        columns.setInteger("draw_order_groups.minimum_draw_orders", group, int(minimum));
        columns.setInteger("draw_order_groups.maximum_draw_orders", group, int(maximum));
    }
    return true;
}
}

bool CMocRuntimeBuilder::Build(const QByteArray& source, const Overrides& overrides, QByteArray& result, QString* error) try
{
    if (error) error->clear();
    auto reader = CReader::FromBytes(source);
    CSectionTable table(reader);
    if (!table.IsValid()) return fail(error, reader.Error());
    if (overrides.visibility.isEmpty() && overrides.drawableOpacity.isEmpty() && overrides.drawableDrawOrder.isEmpty() &&
        overrides.drawableIndices.isEmpty()) { result = source; return true; }
    Columns columns(source, table);
    QVector<bool> hidden;
    if (!visibility(table, overrides.visibility, columns, hidden, error)) return false;
    QVector<QVector<int>> shapes;
    if (!blendRows(table, columns, shapes, error)) return false;
    QSet<QString> ids;
    QSet<int> changedIndices;
    QHash<int, int> orderEdits;
    for (int index = 0; index < table.art_meshes.size(); ++index) {
        const auto* mesh = table.art_meshes[index];
        if (mesh->ids.isEmpty() || ids.contains(mesh->ids)) return fail(error, QStringLiteral("ArtMesh ID 无效或重复"));
        ids.insert(mesh->ids);
        const auto opacity = overrides.drawableOpacity.constFind(mesh->ids);
        const auto order = overrides.drawableDrawOrder.constFind(mesh->ids);
        if (opacity != overrides.drawableOpacity.cend() && (!std::isfinite(*opacity) || *opacity < 0 || *opacity > 1))
            return fail(error, QStringLiteral("ArtMesh 不透明度无效：%1").arg(mesh->ids));
        if (order != overrides.drawableDrawOrder.cend() && (*order < 0 || *order > 1000))
            return fail(error, QStringLiteral("ArtMesh 绘制顺序无效：%1").arg(mesh->ids));
        const int first = mesh->keyform_sources_begin_indices, count = mesh->keyform_sources_counts;
        if (!range(first, count, table.art_mesh_keyforms.size()) ||
            (count == 0 && (opacity != overrides.drawableOpacity.cend() || order != overrides.drawableDrawOrder.cend())))
            return fail(error, QStringLiteral("ArtMesh 关键形区间无效：%1").arg(mesh->ids));
        const bool setOpacity = hidden[index] || opacity != overrides.drawableOpacity.cend();
        const float nextOpacity = hidden[index] ? 0 : setOpacity ? *opacity : 0;
        bool changed = false;
        for (int row = first; row < first + count; ++row) {
            changed = changed || (setOpacity && columns.real("art_mesh_keyforms.opacities", row) != nextOpacity);
            if (order != overrides.drawableDrawOrder.cend() && columns.real("art_mesh_keyforms.draw_orders", row) != *order) {
                changed = true;
                orderEdits.insert(index, *order);
            }
        }
        if (changed) {
            int destination;
            if (!columns.appendRows(columns.span("art_mesh_keyforms.opacities").countIndex, first, count, destination, error)) return false;
            columns.setInteger("art_meshes.keyform_sources_begin_indices", index, destination);
            for (int row = destination; row < destination + count; ++row) {
                if (setOpacity) columns.setReal("art_mesh_keyforms.opacities", row, nextOpacity);
                if (order != overrides.drawableDrawOrder.cend()) columns.setReal("art_mesh_keyforms.draw_orders", row, *order);
            }
        }
        if (hidden[index] && !hideBlendShapes(table, shapes[index], columns, error)) return false;
        const auto indices = overrides.drawableIndices.constFind(mesh->ids);
        if (indices != overrides.drawableIndices.cend()) {
            if (!CMeshTopology::IsValid(*indices, mesh->vertex_counts) ||
                !range(mesh->position_index_sources_begin_indices, mesh->position_index_sources_counts, table.position_indices->indices.size()))
                return fail(error, QStringLiteral("ArtMesh 三角索引无效：%1").arg(mesh->ids));
            bool same = indices->size() == mesh->position_index_sources_counts;
            for (int row = 0; same && row < indices->size(); ++row)
                same = (*indices)[row] == quint16(table.position_indices->indices[mesh->position_index_sources_begin_indices + row]);
            if (!same) {
                changedIndices.insert(index);
                int destination;
                if (!columns.appendIndices(*indices, destination, error)) return false;
                columns.setInteger("art_meshes.position_index_sources_begin_indices", index, destination);
                columns.setInteger("art_meshes.position_index_sources_counts", index, indices->size());
            }
        }
    }
    const auto validObjects = [&](const auto& values) {
        for (auto value = values.cbegin(); value != values.cend(); ++value)
            if (!ids.contains(value.key())) return fail(error, QStringLiteral("ArtMesh 不存在：%1").arg(value.key()));
        return true;
    };
    if (!validObjects(overrides.drawableOpacity) || !validObjects(overrides.drawableDrawOrder) ||
        !validObjects(overrides.drawableIndices) || !updateOrderRanges(table, orderEdits, shapes, columns, error)) return false;
    QStringList conflicts;
    if (!CArtMeshGeometryLayout::CheckIsolation(table, changedIndices, CArtMeshGeometryLayout::Pool::Indices, conflicts, error))
        return false;
    if (!conflicts.isEmpty()) return fail(error, conflicts.join(QLatin1Char('\n')));
    return CMocParameterStructureBuilder::BuildColumnState(source, columns.state, result, error);
}
catch (const std::exception&)
{
    return fail(error, QStringLiteral("无法准备运行时模型，原模型保持不变"));
}
}
