#include "cmodeldocument.h"
#include "csectiontable.h"
#include "util/cfilereader.h"
#include "struct/cart_mesh_keyforms.h"
#include <QHash>
#include <QSet>
#include <QtGlobal>
#include <cmath>
#include <limits>

namespace live2dstruct {

namespace {
void setError(QString* error, const QString& value)
{
    if (error) *error = value;
}

bool inRange(int value, int size)
{
    return value >= 0 && value < size;
}

bool validSlice(int begin, int count, int size)
{
    return begin >= 0 && count >= 0 && begin <= size && count <= size - begin;
}
}

void CModelDocument::Clear()
{
    m_loaded = false;
    m_sourcePath.clear();
    m_canvasPixels = {};
    m_parameters.clear();
    m_parts.clear();
    m_deformers.clear();
    m_drawables.clear();
    m_drawOrderGroups.clear();
    m_drawableOpacityOverrides.clear();
    m_drawableDrawOrderOverrides.clear();
    m_parameterValues.clear();
    m_drawableKeyformPositionOverrides.clear();
    m_drawableIndexOverrides.clear();
    m_drawableMaskIndexOverrides.clear();
    m_deformerParentOverrides.clear();
    m_drawableMaskInvertedOverrides.clear();
    m_lockedKeys.clear();
    ClearEditHistory();
}

bool CModelDocument::Load(const QString& mocPath, QString* error)
{
    Clear();
    CReader reader(mocPath);
    CSectionTable table(reader);
    if (!table.IsValid()) {
        setError(error, reader.Error().isEmpty() ? QStringLiteral("moc3 解析失败") : reader.Error());
        return false;
    }

    m_sourcePath = mocPath;
    m_canvasPixels = QSize(qRound(table.canvas_info->width), qRound(table.canvas_info->height));

    m_parameters.reserve(table.parameters.size());
    for (const CParameters* source : table.parameters) {
        CModelParameter parameter;
        parameter.id = source->ids;
        parameter.minimum = source->min_values;
        parameter.maximum = source->max_values;
        parameter.defaultValue = source->default_values;
        parameter.decimalPlaces = source->decimal_places;
        parameter.repeat = source->is_repeat;
        parameter.type = source->parameter_types;
        if (table.keys && validSlice(source->keys_sources_begin_indices, source->keys_sources_counts,
                                     static_cast<int>(table.keys->values.size()))) {
            parameter.keyValues.reserve(source->keys_sources_counts);
            for (int keyIndex = 0; keyIndex < source->keys_sources_counts; ++keyIndex) {
                parameter.keyValues.append(table.keys->GetData(source->keys_sources_begin_indices + keyIndex));
            }
        }
        m_parameters.append(parameter);
    }

    m_parts.reserve(table.parts.size());
    for (const CParts* source : table.parts) {
        m_parts.append({source->ids, source->parent_part_indices, source->is_visible, source->is_enabled});
    }

    m_deformers.reserve(table.deformers.size());
    for (const CDeformers* source : table.deformers) {
        CModelDeformer deformer;
        deformer.id = source->ids;
        deformer.type = source->types == 0 ? CModelDeformer::Warp : CModelDeformer::Rotation;
        deformer.parentPartIndex = source->parent_part_indices;
        deformer.parentDeformerIndex = source->parent_deformer_indices;
        deformer.visible = source->is_visible;
        deformer.enabled = source->is_enabled;
        if (deformer.type == CModelDeformer::Warp && inRange(source->specific_sources_indices, table.warp_deformers.size())) {
            const CWarp_deformers* warp = table.warp_deformers[source->specific_sources_indices];
            deformer.rows = warp->rows;
            deformer.columns = warp->columns;
        }
        m_deformers.append(deformer);
    }

    m_drawables.reserve(table.art_meshes.size());
    for (const CArt_meshes* source : table.art_meshes) {
        CModelDrawable drawable;
        drawable.id = source->ids;
        drawable.parentPartIndex = source->parent_part_indices;
        drawable.parentDeformerIndex = source->parent_deformer_indices;
        drawable.textureIndex = source->texture_nos;
        drawable.flags = source->drawable_flags;
        drawable.blendMode = source->drawable_flags & 0x3;
        drawable.culling = (source->drawable_flags & 0x4) == 0;
        drawable.vertexCount = source->vertex_counts;
        drawable.visible = source->is_visible;
        drawable.enabled = source->is_enabled;
        const bool vertexCountValid = source->vertex_counts >= 0 && source->vertex_counts <= 1000000 &&
            source->vertex_counts <= std::numeric_limits<int>::max() / 2;
        if (vertexCountValid && validSlice(source->uv_sources_begin_indices, source->vertex_counts * 2,
                                           static_cast<int>(table.uvs->uvs.size()))) {
            for (int i = 0; i < source->vertex_counts * 2; ++i) {
                drawable.uvs.append(table.uvs->uvs[source->uv_sources_begin_indices + i]);
            }
        }
        if (validSlice(source->position_index_sources_begin_indices, source->position_index_sources_counts,
                       static_cast<int>(table.position_indices->indices.size()))) {
            for (int i = 0; i < source->position_index_sources_counts; ++i) {
                const short rawIndex = table.position_indices->indices[
                    source->position_index_sources_begin_indices + i];
                if (rawIndex < 0 || rawIndex >= source->vertex_counts) continue;
                drawable.indices.append(static_cast<quint16>(rawIndex));
            }
        }
        if (validSlice(source->drawable_mask_sources_begin_indices, source->drawable_mask_sources_counts,
                       static_cast<int>(table.drawable_masks.size()))) {
            for (int i = 0; i < source->drawable_mask_sources_counts; ++i) {
                const int index = source->drawable_mask_sources_begin_indices + i;
                if (inRange(index, table.drawable_masks.size())) {
                    drawable.maskIndices.append(table.drawable_masks[index]->art_mesh_sources_indices);
                }
            }
        }
        if (validSlice(source->keyform_sources_begin_indices, source->keyform_sources_counts,
                       static_cast<int>(table.art_mesh_keyforms.size()))) {
            for (int i = 0; i < source->keyform_sources_counts; ++i) {
                const int index = source->keyform_sources_begin_indices + i;
                if (!inRange(index, table.art_mesh_keyforms.size())) continue;
                const CArt_mesh_keyforms* sourceKeyform = table.art_mesh_keyforms[index];
                CModelKeyform keyform;
                keyform.opacity = sourceKeyform->opacities;
                keyform.drawOrder = sourceKeyform->draw_orders;
                if (i == 0) {
                    drawable.opacity = keyform.opacity;
                    drawable.drawOrder = keyform.drawOrder;
                }
                const int begin = sourceKeyform->keyform_position_sources_begin_indices;
                if (vertexCountValid && validSlice(begin, source->vertex_counts * 2,
                                                   static_cast<int>(table.keyform_positions->xys.size()))) {
                    for (int v = 0; v < source->vertex_counts * 2; ++v) {
                        keyform.positions.append(table.keyform_positions->xys[begin + v]);
                    }
                }
                drawable.keyforms.append(keyform);
            }
        }
        m_drawables.append(drawable);
    }

    m_drawOrderGroups.reserve(table.draw_order_groups.size());
    for (const CDraw_order_groups* source : table.draw_order_groups) {
        CModelDrawOrderGroup group;
        group.minimumDrawOrder = source->minimum_draw_orders;
        group.maximumDrawOrder = source->maximum_draw_orders;
        group.objectCount = source->object_sources_counts;
        group.totalObjectCount = source->object_sources_total_counts;
        group.objectsValid = validSlice(source->object_sources_begin_indices, group.objectCount,
                                        table.draw_order_group_objects.size());
        if (group.objectsValid) {
            group.objects.reserve(group.objectCount);
            for (int i = 0; i < group.objectCount; ++i) {
                const CDraw_order_group_objects* object = table.draw_order_group_objects[
                    source->object_sources_begin_indices + i];
                group.objects.append({object->types, object->indices, object->self_indices});
            }
        }
        m_drawOrderGroups.append(group);
    }

    m_loaded = true;
    return true;
}

bool CModelDocument::SetPartVisible(const QString& id, bool visible)
{
    for (CModelPart& part : m_parts) {
        if (part.id != id) continue;
        part.visible = visible;
        return true;
    }
    return false;
}

bool CModelDocument::SetParameterValue(const QString& id, float value)
{
    if (!std::isfinite(value)) return false;
    for (const CModelParameter& parameter : m_parameters) {
        if (parameter.id != id) continue;
        if (value < parameter.minimum || value > parameter.maximum) return false;
        m_parameterValues.insert(id, value);
        return true;
    }
    return false;
}

float CModelDocument::ParameterValue(const QString& id) const
{
    for (const CModelParameter& parameter : m_parameters) {
        if (parameter.id == id) return m_parameterValues.value(id, parameter.defaultValue);
    }
    return 0.0f;
}

QVector<float> CModelDocument::DrawableKeyformPositions(const QString& id, int keyformIndex) const
{
    if (keyformIndex < 0) return {};
    const auto overrideIt = m_drawableKeyformPositionOverrides.constFind(id);
    if (overrideIt != m_drawableKeyformPositionOverrides.cend() && keyformIndex < overrideIt->size()) {
        return overrideIt->at(keyformIndex);
    }
    for (const CModelDrawable& drawable : m_drawables) {
        if (drawable.id != id || keyformIndex >= drawable.keyforms.size()) continue;
        return drawable.keyforms.at(keyformIndex).positions;
    }
    return {};
}

bool CModelDocument::SetDrawableKeyformPositions(const QString& id, int keyformIndex,
                                                 const QVector<float>& positions)
{
    if (keyformIndex < 0) return false;
    for (const CModelDrawable& drawable : m_drawables) {
        if (drawable.id != id || keyformIndex >= drawable.keyforms.size()) continue;
        if (positions.size() != drawable.vertexCount * 2) return false;
        for (float value : positions) if (!std::isfinite(value)) return false;
        auto& overrides = m_drawableKeyformPositionOverrides[id];
        if (overrides.size() < drawable.keyforms.size()) overrides.resize(drawable.keyforms.size());
        overrides[keyformIndex] = positions;
        return true;
    }
    return false;
}

QVector<quint16> CModelDocument::DrawableIndices(const QString& id) const
{
    if (m_drawableIndexOverrides.contains(id)) return m_drawableIndexOverrides.value(id);
    for (const CModelDrawable& drawable : m_drawables) {
        if (drawable.id == id) return drawable.indices;
    }
    return {};
}

bool CModelDocument::SetDrawableIndices(const QString& id, const QVector<quint16>& indices)
{
    for (const CModelDrawable& drawable : m_drawables) {
        if (drawable.id != id) continue;
        if (indices.isEmpty() || indices.size() % 3 != 0 || indices.size() > 3000000) return false;
        for (quint16 index : indices) if (index >= drawable.vertexCount) return false;
        m_drawableIndexOverrides.insert(id, indices);
        return true;
    }
    return false;
}

QVector<int> CModelDocument::DrawableMaskIndices(const QString& id) const
{
    if (m_drawableMaskIndexOverrides.contains(id)) return m_drawableMaskIndexOverrides.value(id);
    for (const CModelDrawable& drawable : m_drawables) {
        if (drawable.id == id) return drawable.maskIndices;
    }
    return {};
}

int CModelDocument::DeformerParentIndex(const QString& id) const
{
    if (m_deformerParentOverrides.contains(id)) return m_deformerParentOverrides.value(id);
    for (const CModelDeformer& deformer : m_deformers) {
        if (deformer.id == id) return deformer.parentDeformerIndex;
    }
    return -1;
}

bool CModelDocument::SetDeformerParentIndex(const QString& id, int parentIndex)
{
    int deformerIndex = -1;
    for (int i = 0; i < m_deformers.size(); ++i) {
        if (m_deformers[i].id == id) { deformerIndex = i; break; }
    }
    if (deformerIndex < 0) return false;
    if (parentIndex < -1 || parentIndex >= m_deformers.size() || parentIndex == deformerIndex) return false;
    if (parentIndex >= 0 && m_deformers[parentIndex].parentPartIndex != m_deformers[deformerIndex].parentPartIndex) return false;
    QVector<int> states(m_deformers.size(), 0);
    int current = parentIndex;
    while (current >= 0) {
        if (current >= m_deformers.size() || states[current] == 1) return false;
        if (current == deformerIndex) return false;
        states[current] = 1;
        current = DeformerParentIndex(m_deformers[current].id);
    }
    m_deformerParentOverrides.insert(id, parentIndex);
    return true;
}

bool CModelDocument::SetDeformerParentIndices(const QHash<QString, int>& parents)
{
    const QHash<QString, int> previous = m_deformerParentOverrides;
    for (auto it = parents.cbegin(); it != parents.cend(); ++it) {
        if (!SetDeformerParentIndex(it.key(), it.value())) {
            m_deformerParentOverrides = previous;
            return false;
        }
    }
    return true;
}

bool CModelDocument::SetDrawableMaskIndices(const QString& id, const QVector<int>& maskIndices)
{
    for (int drawableIndex = 0; drawableIndex < m_drawables.size(); ++drawableIndex) {
        if (m_drawables[drawableIndex].id != id) continue;
        QSet<int> unique;
        for (int maskIndex : maskIndices) {
            if (!inRange(maskIndex, m_drawables.size()) || maskIndex == drawableIndex ||
                unique.contains(maskIndex)) return false;
            unique.insert(maskIndex);
        }
        m_drawableMaskIndexOverrides.insert(id, maskIndices);
        return true;
    }
    return false;
}

bool CModelDocument::DrawableMaskInverted(const QString& id) const
{
    return m_drawableMaskInvertedOverrides.value(id, false);
}

bool CModelDocument::SetDrawableMaskInverted(const QString& id, bool inverted)
{
    for (const CModelDrawable& drawable : m_drawables) {
        if (drawable.id != id) continue;
        if (DrawableMaskIndices(id).isEmpty() && inverted) return false;
        m_drawableMaskInvertedOverrides.insert(id, inverted);
        return true;
    }
    return false;
}

bool CModelDocument::SetDeformerVisible(const QString& id, bool visible)
{
    for (CModelDeformer& deformer : m_deformers) {
        if (deformer.id != id) continue;
        deformer.visible = visible;
        return true;
    }
    return false;
}

bool CModelDocument::SetDrawableVisible(const QString& id, bool visible)
{
    for (CModelDrawable& drawable : m_drawables) {
        if (drawable.id != id) continue;
        drawable.visible = visible;
        return true;
    }
    return false;
}

bool CModelDocument::SetDrawableOpacity(const QString& id, float opacity)
{
    if (!std::isfinite(opacity)) return false;
    const float clamped = qBound(0.0f, opacity, 1.0f);
    for (const CModelDrawable& drawable : m_drawables) {
        if (drawable.id != id) continue;
        m_drawableOpacityOverrides.insert(id, clamped);
        return true;
    }
    return false;
}

float CModelDocument::DrawableOpacity(const CModelDrawable& drawable) const
{
    return m_drawableOpacityOverrides.value(drawable.id, drawable.opacity);
}

bool CModelDocument::SetDrawableDrawOrder(const QString& id, int drawOrder)
{
    for (const CModelDrawable& drawable : m_drawables) {
        if (drawable.id != id) continue;
        m_drawableDrawOrderOverrides.insert(id, qBound(0, drawOrder, 1000));
        return true;
    }
    return false;
}

float CModelDocument::DrawableDrawOrder(const CModelDrawable& drawable) const
{
    return m_drawableDrawOrderOverrides.value(drawable.id, static_cast<int>(qBound(0.0f, drawable.drawOrder, 1000.0f)));
}

void CModelDocument::SetLocked(const QString& type, const QString& id, bool locked)
{
    if (type.isEmpty() || id.isEmpty()) return;
    const QString key = type + QLatin1Char('|') + id;
    if (locked) m_lockedKeys.insert(key);
    else m_lockedKeys.remove(key);
}

void CModelDocument::ClearLocked()
{
    m_lockedKeys.clear();
}

bool CModelDocument::IsLocked(const QString& type, const QString& id) const
{
    return m_lockedKeys.contains(type + QLatin1Char('|') + id);
}

bool CModelDocument::ApplyEdit(const CModelEdit& edit, bool forward)
{
    const QVariant& value = forward ? edit.after : edit.before;
    if (edit.field == CModelEdit::Visibility) {
        const bool visible = value.toBool();
        if (edit.type == QLatin1String("part")) return SetPartVisible(edit.name, visible);
        if (edit.type == QLatin1String("warp") || edit.type == QLatin1String("rotation")) return SetDeformerVisible(edit.name, visible);
        if (edit.type == QLatin1String("artmesh")) return SetDrawableVisible(edit.name, visible);
        return false;
    }
    if (edit.field == CModelEdit::Opacity) {
        if (edit.type != QLatin1String("artmesh") || !value.canConvert<float>()) return false;
        const float opacity = value.toFloat();
        return std::isfinite(opacity) && opacity >= 0.0f && opacity <= 1.0f && SetDrawableOpacity(edit.name, opacity);
    }
    if (edit.field == CModelEdit::DrawOrder) {
        if (edit.type != QLatin1String("artmesh") || !value.canConvert<int>()) return false;
        const double number = value.toDouble();
        if (!std::isfinite(number) || std::floor(number) != number || number < 0.0 || number > 1000.0) return false;
        return SetDrawableDrawOrder(edit.name, static_cast<int>(number));
    }
    if (edit.field == CModelEdit::ParameterValue) {
        if (edit.type != QLatin1String("parameter") || !value.canConvert<float>()) return false;
        return SetParameterValue(edit.name, value.toFloat());
    }
    if (edit.field == CModelEdit::KeyformPositions) {
        if (edit.type != QLatin1String("artmesh") || !value.canConvert<QVector<float>>()) return false;
        return SetDrawableKeyformPositions(edit.name, edit.index, value.value<QVector<float>>());
    }
    if (edit.field == CModelEdit::MeshIndices) {
        if (edit.type != QLatin1String("artmesh") || !value.canConvert<QVector<quint16>>()) return false;
        return SetDrawableIndices(edit.name, value.value<QVector<quint16>>());
    }
    if (edit.field == CModelEdit::MaskIndices) {
        if (edit.type != QLatin1String("artmesh") || !value.canConvert<QVector<int>>()) return false;
        return SetDrawableMaskIndices(edit.name, value.value<QVector<int>>());
    }
    if (edit.field == CModelEdit::DeformerParent) {
        if (edit.type != QLatin1String("warp") && edit.type != QLatin1String("rotation")) return false;
        if (!value.canConvert<int>()) return false;
        return SetDeformerParentIndex(edit.name, value.toInt());
    }
    if (edit.field == CModelEdit::MaskInverted) {
        if (edit.type != QLatin1String("artmesh") || !value.canConvert<bool>()) return false;
        return SetDrawableMaskInverted(edit.name, value.toBool());
    }
    return false;
}

bool CModelDocument::ExecuteEdit(const CModelEdit& edit)
{
    if (edit.name.isEmpty() || edit.before == edit.after) return false;
    if (!ApplyEdit(edit, true)) return false;
    if (m_editIndex < m_editHistory.size()) m_editHistory.resize(m_editIndex);
    if (edit.coalesce && !m_editHistory.isEmpty() && m_editIndex == m_editHistory.size()) {
        CModelEdit& last = m_editHistory.last();
        if (last.name == edit.name && last.type == edit.type && last.field == edit.field &&
            last.index == edit.index && last.coalesce && last.after == edit.before) {
            last.after = edit.after;
            return true;
        }
    }
    m_editHistory.append(edit);
    m_editIndex = m_editHistory.size();
    return true;
}

bool CModelDocument::PeekUndoEdit(CModelEdit* edit) const
{
    if (m_editIndex <= 0) return false;
    if (edit) *edit = m_editHistory[m_editIndex - 1];
    return true;
}

bool CModelDocument::PeekRedoEdit(CModelEdit* edit) const
{
    if (m_editIndex >= m_editHistory.size()) return false;
    if (edit) *edit = m_editHistory[m_editIndex];
    return true;
}

bool CModelDocument::UndoEdit(CModelEdit* edit)
{
    if (m_editIndex <= 0) return false;
    const CModelEdit command = m_editHistory[m_editIndex - 1];
    if (!ApplyEdit(command, false)) return false;
    --m_editIndex;
    if (edit) *edit = command;
    return true;
}

bool CModelDocument::RedoEdit(CModelEdit* edit)
{
    if (m_editIndex >= m_editHistory.size()) return false;
    const CModelEdit command = m_editHistory[m_editIndex];
    if (!ApplyEdit(command, true)) return false;
    ++m_editIndex;
    if (edit) *edit = command;
    return true;
}

void CModelDocument::ClearEditHistory()
{
    m_editHistory.clear();
    m_editIndex = 0;
}

QSet<QString> CModelDocument::HiddenIds() const
{
    QSet<QString> hidden;
    for (const CModelPart& part : m_parts) if (!part.visible) hidden.insert(part.id);
    for (const CModelDeformer& deformer : m_deformers) if (!deformer.visible) hidden.insert(deformer.id);
    for (const CModelDrawable& drawable : m_drawables) if (!drawable.visible) hidden.insert(drawable.id);
    return hidden;
}

QVector<CModelDiagnostic> CModelDocument::Validate() const
{
    QVector<CModelDiagnostic> diagnostics;
    if (!m_loaded) {
        diagnostics.append({CModelDiagnostic::Error, QStringLiteral("not_loaded"),
                             QStringLiteral("模型尚未加载"), -1});
        return diagnostics;
    }
    if (m_canvasPixels.isEmpty()) {
        diagnostics.append({CModelDiagnostic::Error, QStringLiteral("canvas_size"),
                             QStringLiteral("画布尺寸无效"), -1});
    }

    QSet<QString> parameterIds;
    for (int i = 0; i < m_parameters.size(); ++i) {
        const CModelParameter& parameter = m_parameters[i];
        if (parameter.id.isEmpty() || parameterIds.contains(parameter.id)) {
            diagnostics.append({CModelDiagnostic::Error, QStringLiteral("parameter_id"),
                                 QStringLiteral("参数 ID 为空或重复"), i});
        }
        parameterIds.insert(parameter.id);
        if (parameter.minimum > parameter.maximum ||
            parameter.defaultValue < parameter.minimum || parameter.defaultValue > parameter.maximum) {
            diagnostics.append({CModelDiagnostic::Error, QStringLiteral("parameter_range"),
                                 QStringLiteral("参数范围或默认值无效"), i});
        }
        if (!std::isfinite(parameter.minimum) || !std::isfinite(parameter.maximum) ||
            !std::isfinite(parameter.defaultValue)) {
            diagnostics.append({CModelDiagnostic::Error, QStringLiteral("parameter_nan"),
                                 QStringLiteral("参数包含非有限数值"), i});
        }
        float previousKey = -std::numeric_limits<float>::infinity();
        for (float key : parameter.keyValues) {
            if (!std::isfinite(key) || key < parameter.minimum || key > parameter.maximum || key < previousKey) {
                diagnostics.append({CModelDiagnostic::Warning, QStringLiteral("parameter_keys"),
                                     QStringLiteral("参数关键值无效或未递增"), i});
                break;
            }
            previousKey = key;
        }
    }

    QSet<QString> partIds;
    QVector<int> partStates(m_parts.size(), 0);
    for (int i = 0; i < m_parts.size(); ++i) {
        const CModelPart& part = m_parts[i];
        if (part.id.isEmpty() || partIds.contains(part.id)) {
            diagnostics.append({CModelDiagnostic::Error, QStringLiteral("part_id"),
                                 QStringLiteral("Part ID 为空或重复"), i});
        }
        partIds.insert(part.id);
        if (part.parentIndex != -1 && !inRange(part.parentIndex, m_parts.size())) {
            diagnostics.append({CModelDiagnostic::Error, QStringLiteral("part_parent"),
                                 QStringLiteral("Part 父索引越界"), i});
        }
        if (partStates[i] != 0) continue;
        QVector<int> path;
        int current = i;
        while (inRange(current, m_parts.size()) && partStates[current] == 0) {
            partStates[current] = 1;
            path.append(current);
            current = m_parts[current].parentIndex;
        }
        if (inRange(current, m_parts.size()) && partStates[current] == 1) {
            diagnostics.append({CModelDiagnostic::Error, QStringLiteral("part_cycle"),
                                 QStringLiteral("Part 父子关系存在循环"), current});
        }
        for (int member : path) partStates[member] = 2;
    }

    QVector<int> states(m_deformers.size(), 0);
    for (int i = 0; i < m_deformers.size(); ++i) {
        if (m_deformers[i].parentPartIndex != -1 && !inRange(m_deformers[i].parentPartIndex, m_parts.size())) {
            diagnostics.append({CModelDiagnostic::Error, QStringLiteral("deformer_part"),
                                 QStringLiteral("Deformer 所属 Part 越界"), i});
        }
        if (states[i] != 0) continue;
        QVector<int> path;
        int current = i;
        while (inRange(current, m_deformers.size()) && states[current] == 0) {
            states[current] = 1;
            path.append(current);
            const int parent = m_deformers[current].parentDeformerIndex;
            if (parent != -1 && !inRange(parent, m_deformers.size())) {
                diagnostics.append({CModelDiagnostic::Error, QStringLiteral("deformer_parent"),
                                     QStringLiteral("Deformer 父索引越界"), current});
                current = -1;
                break;
            }
            current = parent;
        }
        if (inRange(current, m_deformers.size()) && states[current] == 1) {
            diagnostics.append({CModelDiagnostic::Error, QStringLiteral("deformer_cycle"),
                                 QStringLiteral("Deformer 父子关系存在循环"), current});
        }
        for (int member : path) states[member] = 2;
    }

    QSet<QString> drawableIds;
    for (int i = 0; i < m_drawables.size(); ++i) {
        const CModelDrawable& drawable = m_drawables[i];
        if (drawable.id.isEmpty() || drawableIds.contains(drawable.id)) {
            diagnostics.append({CModelDiagnostic::Error, QStringLiteral("drawable_id"),
                                 QStringLiteral("ArtMesh ID 为空或重复"), i});
        }
        drawableIds.insert(drawable.id);
        if (drawable.vertexCount < 0 || drawable.vertexCount > 1000000) {
            diagnostics.append({CModelDiagnostic::Error, QStringLiteral("vertex_count"),
                                 QStringLiteral("ArtMesh 顶点数量无效"), i});
        }
        if (drawable.parentPartIndex != -1 && !inRange(drawable.parentPartIndex, m_parts.size())) {
            diagnostics.append({CModelDiagnostic::Error, QStringLiteral("drawable_part"),
                                 QStringLiteral("ArtMesh 所属 Part 越界"), i});
        }
        if (drawable.parentDeformerIndex != -1 && !inRange(drawable.parentDeformerIndex, m_deformers.size())) {
            diagnostics.append({CModelDiagnostic::Error, QStringLiteral("drawable_deformer"),
                                 QStringLiteral("ArtMesh 所属 Deformer 越界"), i});
        }
        for (quint16 index : drawable.indices) {
            if (index >= drawable.vertexCount) {
                diagnostics.append({CModelDiagnostic::Error, QStringLiteral("index_range"),
                                     QStringLiteral("ArtMesh 三角形索引越界"), i});
                break;
            }
        }
        if (!drawable.indices.isEmpty() && drawable.indices.size() % 3 != 0) {
            diagnostics.append({CModelDiagnostic::Error, QStringLiteral("index_triangle_count"),
                                 QStringLiteral("ArtMesh 索引数量不是三角形的倍数"), i});
        }
        QSet<int> maskIndices;
        for (int mask : drawable.maskIndices) {
            if (!inRange(mask, m_drawables.size())) {
                diagnostics.append({CModelDiagnostic::Error, QStringLiteral("mask_range"),
                                     QStringLiteral("Clipping mask 索引越界"), i});
            } else if (mask == i) {
                diagnostics.append({CModelDiagnostic::Error, QStringLiteral("mask_self"),
                                     QStringLiteral("ArtMesh 不能引用自身作为 Clipping mask"), i});
            } else if (maskIndices.contains(mask)) {
                diagnostics.append({CModelDiagnostic::Error, QStringLiteral("mask_duplicate"),
                                     QStringLiteral("Clipping mask 引用重复"), i});
            }
            maskIndices.insert(mask);
        }
        if (!drawable.uvs.isEmpty() && drawable.uvs.size() != drawable.vertexCount * 2) {
            diagnostics.append({CModelDiagnostic::Error, QStringLiteral("uv_count"),
                                 QStringLiteral("UV 数量与顶点数量不一致"), i});
        }
        if (drawable.vertexCount > 0 && drawable.uvs.isEmpty()) {
            diagnostics.append({CModelDiagnostic::Error, QStringLiteral("uv_missing"),
                                 QStringLiteral("ArtMesh 缺少 UV 数据"), i});
        }
        if (drawable.vertexCount > 0 && drawable.indices.isEmpty()) {
            diagnostics.append({CModelDiagnostic::Error, QStringLiteral("index_missing"),
                                 QStringLiteral("ArtMesh 缺少三角形索引"), i});
        }
        for (float uv : drawable.uvs) {
            if (!std::isfinite(uv)) {
                diagnostics.append({CModelDiagnostic::Error, QStringLiteral("uv_nan"),
                                     QStringLiteral("ArtMesh UV 包含非有限数值"), i});
                break;
            }
        }
        if (!std::isfinite(drawable.opacity) || drawable.opacity < -0.001f || drawable.opacity > 1.001f) {
            diagnostics.append({CModelDiagnostic::Error, QStringLiteral("opacity_range"),
                                 QStringLiteral("ArtMesh 默认透明度超出 0 到 1 范围"), i});
        }
        for (const CModelKeyform& keyform : drawable.keyforms) {
            if (!keyform.positions.isEmpty() && keyform.positions.size() != drawable.vertexCount * 2) {
                diagnostics.append({CModelDiagnostic::Error, QStringLiteral("keyform_position_count"),
                                     QStringLiteral("关键形顶点数量与 ArtMesh 不一致"), i});
                break;
            }
            if (!std::isfinite(keyform.opacity) || !std::isfinite(keyform.drawOrder)) {
                diagnostics.append({CModelDiagnostic::Error, QStringLiteral("keyform_nan"),
                                     QStringLiteral("关键形包含非有限数值"), i});
                break;
            }
            bool finitePositions = true;
            for (float value : keyform.positions) {
                if (!std::isfinite(value)) { finitePositions = false; break; }
            }
            if (!finitePositions) {
                diagnostics.append({CModelDiagnostic::Error, QStringLiteral("keyform_position_nan"),
                                     QStringLiteral("关键形顶点包含非有限数值"), i});
                break;
            }
        }
    }

    for (auto it = m_parameterValues.cbegin(); it != m_parameterValues.cend(); ++it) {
        bool found = false;
        for (const CModelParameter& parameter : m_parameters) {
            if (parameter.id != it.key()) continue;
            found = true;
            if (!std::isfinite(it.value()) || it.value() < parameter.minimum || it.value() > parameter.maximum) {
                diagnostics.append({CModelDiagnostic::Error, QStringLiteral("parameter_override_range"),
                                     QStringLiteral("参数覆盖值无效：%1").arg(it.key()), -1});
            }
            break;
        }
        if (!found) {
            diagnostics.append({CModelDiagnostic::Error, QStringLiteral("parameter_override_id"),
                                 QStringLiteral("参数覆盖引用了不存在的参数：%1").arg(it.key()), -1});
        }
    }
    for (auto it = m_drawableOpacityOverrides.cbegin(); it != m_drawableOpacityOverrides.cend(); ++it) {
        bool found = false;
        for (const CModelDrawable& drawable : m_drawables) if (drawable.id == it.key()) { found = true; break; }
        if (!found) {
            diagnostics.append({CModelDiagnostic::Error, QStringLiteral("opacity_override_id"),
                                 QStringLiteral("透明度覆盖引用了不存在的 ArtMesh：%1").arg(it.key()), -1});
        } else if (!std::isfinite(it.value()) || it.value() < 0.0f || it.value() > 1.0f) {
            diagnostics.append({CModelDiagnostic::Error, QStringLiteral("opacity_override_range"),
                                 QStringLiteral("透明度覆盖值无效：%1").arg(it.key()), -1});
        }
    }
    for (auto it = m_drawableDrawOrderOverrides.cbegin(); it != m_drawableDrawOrderOverrides.cend(); ++it) {
        bool found = false;
        for (const CModelDrawable& drawable : m_drawables) if (drawable.id == it.key()) { found = true; break; }
        if (!found) {
            diagnostics.append({CModelDiagnostic::Error, QStringLiteral("draw_order_override_id"),
                                 QStringLiteral("绘制顺序覆盖引用了不存在的 ArtMesh：%1").arg(it.key()), -1});
        } else if (it.value() < 0 || it.value() > 1000) {
            diagnostics.append({CModelDiagnostic::Error, QStringLiteral("draw_order_override_range"),
                                 QStringLiteral("绘制顺序覆盖值无效：%1").arg(it.key()), -1});
        }
    }
    for (auto meshIt = m_drawableKeyformPositionOverrides.cbegin();
         meshIt != m_drawableKeyformPositionOverrides.cend(); ++meshIt) {
        const CModelDrawable* drawable = nullptr;
        for (const CModelDrawable& candidate : m_drawables) {
            if (candidate.id == meshIt.key()) { drawable = &candidate; break; }
        }
        if (!drawable) {
            diagnostics.append({CModelDiagnostic::Error, QStringLiteral("keyform_override_id"),
                                 QStringLiteral("关键形覆盖引用了不存在的 ArtMesh：%1").arg(meshIt.key()), -1});
            continue;
        }
        if (meshIt.value().size() > drawable->keyforms.size()) {
            diagnostics.append({CModelDiagnostic::Error, QStringLiteral("keyform_override_count"),
                                 QStringLiteral("关键形覆盖数量超出 ArtMesh：%1").arg(meshIt.key()), -1});
            continue;
        }
        for (const QVector<float>& positions : meshIt.value()) {
            if (positions.isEmpty()) continue;
            if (positions.size() != drawable->vertexCount * 2) {
                diagnostics.append({CModelDiagnostic::Error, QStringLiteral("keyform_override_position_count"),
                                     QStringLiteral("关键形覆盖顶点数量无效：%1").arg(meshIt.key()), -1});
                break;
            }
            bool finite = true;
            for (float value : positions) if (!std::isfinite(value)) { finite = false; break; }
            if (!finite) {
                diagnostics.append({CModelDiagnostic::Error, QStringLiteral("keyform_override_nan"),
                                     QStringLiteral("关键形覆盖包含非有限数值：%1").arg(meshIt.key()), -1});
                break;
            }
        }
    }
    for (auto indexIt = m_drawableIndexOverrides.cbegin(); indexIt != m_drawableIndexOverrides.cend(); ++indexIt) {
        const CModelDrawable* drawable = nullptr;
        for (const CModelDrawable& candidate : m_drawables) {
            if (candidate.id == indexIt.key()) { drawable = &candidate; break; }
        }
        if (!drawable) {
            diagnostics.append({CModelDiagnostic::Error, QStringLiteral("index_override_id"),
                                 QStringLiteral("索引覆盖引用了不存在的 ArtMesh：%1").arg(indexIt.key()), -1});
            continue;
        }
        if (indexIt.value().isEmpty() || indexIt.value().size() % 3 != 0 || indexIt.value().size() > 3000000) {
            diagnostics.append({CModelDiagnostic::Error, QStringLiteral("index_override_count"),
                                 QStringLiteral("索引覆盖数量无效：%1").arg(indexIt.key()), -1});
            continue;
        }
        for (quint16 index : indexIt.value()) {
            if (index >= drawable->vertexCount) {
                diagnostics.append({CModelDiagnostic::Error, QStringLiteral("index_override_range"),
                                     QStringLiteral("索引覆盖越界：%1").arg(indexIt.key()), -1});
                break;
            }
        }
    }
    for (auto maskIt = m_drawableMaskIndexOverrides.cbegin(); maskIt != m_drawableMaskIndexOverrides.cend(); ++maskIt) {
        int drawableIndex = -1;
        for (int i = 0; i < m_drawables.size(); ++i) {
            if (m_drawables[i].id == maskIt.key()) { drawableIndex = i; break; }
        }
        if (drawableIndex < 0) {
            diagnostics.append({CModelDiagnostic::Error, QStringLiteral("mask_override_id"),
                                 QStringLiteral("蒙版覆盖引用了不存在的 ArtMesh：%1").arg(maskIt.key()), -1});
            continue;
        }
        QSet<int> unique;
        for (int maskIndex : maskIt.value()) {
            if (!inRange(maskIndex, m_drawables.size()) || maskIndex == drawableIndex) {
                diagnostics.append({CModelDiagnostic::Error, QStringLiteral("mask_override_range"),
                                     QStringLiteral("蒙版覆盖索引无效：%1").arg(maskIt.key()), -1});
                break;
            }
            if (unique.contains(maskIndex)) {
                diagnostics.append({CModelDiagnostic::Error, QStringLiteral("mask_override_duplicate"),
                                     QStringLiteral("蒙版覆盖存在重复引用：%1").arg(maskIt.key()), -1});
                break;
            }
            unique.insert(maskIndex);
        }
    }
    for (auto parentIt = m_deformerParentOverrides.cbegin(); parentIt != m_deformerParentOverrides.cend(); ++parentIt) {
        int deformerIndex = -1;
        for (int i = 0; i < m_deformers.size(); ++i) {
            if (m_deformers[i].id == parentIt.key()) { deformerIndex = i; break; }
        }
        const int parentIndex = parentIt.value();
        if (deformerIndex < 0 || parentIndex < -1 || parentIndex >= m_deformers.size() || parentIndex == deformerIndex ||
            (parentIndex >= 0 && m_deformers[parentIndex].parentPartIndex != m_deformers[deformerIndex].parentPartIndex)) {
            diagnostics.append({CModelDiagnostic::Error, QStringLiteral("deformer_parent_override"),
                                 QStringLiteral("变形器父级覆盖无效：%1").arg(parentIt.key()), -1});
            continue;
        }
        QSet<int> visited;
        int current = parentIndex;
        while (current >= 0 && current < m_deformers.size()) {
            if (current == deformerIndex || visited.contains(current)) {
                diagnostics.append({CModelDiagnostic::Error, QStringLiteral("deformer_parent_cycle"),
                                     QStringLiteral("变形器父级覆盖形成循环：%1").arg(parentIt.key()), -1});
                break;
            }
            visited.insert(current);
            current = DeformerParentIndex(m_deformers[current].id);
        }
    }
    for (auto invertIt = m_drawableMaskInvertedOverrides.cbegin(); invertIt != m_drawableMaskInvertedOverrides.cend(); ++invertIt) {
        bool found = false;
        for (const CModelDrawable& drawable : m_drawables) {
            if (drawable.id != invertIt.key()) continue;
            found = true;
            if (invertIt.value() && DrawableMaskIndices(drawable.id).isEmpty()) {
                diagnostics.append({CModelDiagnostic::Error, QStringLiteral("mask_inverted_empty"),
                                     QStringLiteral("没有 Clipping mask 时不能启用反向蒙版：%1").arg(invertIt.key()), -1});
            }
            break;
        }
        if (!found) {
            diagnostics.append({CModelDiagnostic::Error, QStringLiteral("mask_inverted_id"),
                                 QStringLiteral("反向蒙版覆盖引用了不存在的 ArtMesh：%1").arg(invertIt.key()), -1});
        }
    }
    for (const QString& key : m_lockedKeys) {
        const int separator = key.indexOf(QLatin1Char('|'));
        if (separator <= 0 || separator >= key.size() - 1) {
            diagnostics.append({CModelDiagnostic::Error, QStringLiteral("locked_key"),
                                 QStringLiteral("锁定键格式无效"), -1});
        }
    }

    for (int i = 0; i < m_drawOrderGroups.size(); ++i) {
        const CModelDrawOrderGroup& group = m_drawOrderGroups[i];
        if (group.minimumDrawOrder < 0 || group.maximumDrawOrder > 1000 ||
            group.minimumDrawOrder > group.maximumDrawOrder) {
            diagnostics.append({CModelDiagnostic::Error, QStringLiteral("draw_order_group_range"),
                                 QStringLiteral("Draw Order Group 范围无效"), i});
        }
        if (group.objectCount < 0 || group.totalObjectCount < group.objectCount ||
            group.objectCount != group.objects.size() || !group.objectsValid) {
            diagnostics.append({CModelDiagnostic::Error, QStringLiteral("draw_order_group_objects"),
                                 QStringLiteral("Draw Order Group 成员切片无效"), i});
            continue;
        }
        QSet<QString> members;
        for (const CModelDrawOrderGroupObject& object : group.objects) {
            if (object.type == 0) {
                if (!inRange(object.index, m_drawables.size())) {
                    diagnostics.append({CModelDiagnostic::Error, QStringLiteral("draw_order_group_drawable"),
                                         QStringLiteral("Draw Order Group ArtMesh 索引越界"), i});
                    continue;
                }
                if (object.selfIndex != -1) {
                    diagnostics.append({CModelDiagnostic::Error, QStringLiteral("draw_order_group_self"),
                                         QStringLiteral("ArtMesh 的组索引应为空"), i});
                }
                const QString key = QStringLiteral("drawable:%1").arg(object.index);
                if (members.contains(key)) {
                    diagnostics.append({CModelDiagnostic::Error, QStringLiteral("draw_order_group_duplicate"),
                                         QStringLiteral("Draw Order Group 成员重复"), i});
                }
                members.insert(key);
            } else if (object.type == 1) {
                if (!inRange(object.index, m_parts.size()) ||
                    !inRange(object.selfIndex, m_drawOrderGroups.size())) {
                    diagnostics.append({CModelDiagnostic::Error, QStringLiteral("draw_order_group_part"),
                                         QStringLiteral("Draw Order Group Part 或组索引越界"), i});
                    continue;
                }
                const QString key = QStringLiteral("part:%1").arg(object.index);
                if (members.contains(key)) {
                    diagnostics.append({CModelDiagnostic::Error, QStringLiteral("draw_order_group_duplicate"),
                                         QStringLiteral("Draw Order Group 成员重复"), i});
                }
                members.insert(key);
            } else {
                diagnostics.append({CModelDiagnostic::Error, QStringLiteral("draw_order_group_type"),
                                     QStringLiteral("Draw Order Group 成员类型无效"), i});
            }
        }
    }
    return diagnostics;
}

}
