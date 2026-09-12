#include "cmodeldocument.h"
#include "csectiontable.h"
#include "util/cfilereader.h"
#include "struct/cart_mesh_keyforms.h"
#include <QHash>
#include <QSet>
#include <QtGlobal>
#include <cmath>
#include <functional>

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
        if (source->uv_sources_begin_indices >= 0 &&
            source->uv_sources_begin_indices + source->vertex_counts * 2 <= table.uvs->uvs.size()) {
            for (int i = 0; i < source->vertex_counts * 2; ++i) {
                drawable.uvs.append(table.uvs->uvs[source->uv_sources_begin_indices + i]);
            }
        }
        if (source->position_index_sources_begin_indices >= 0 &&
            source->position_index_sources_begin_indices + source->position_index_sources_counts <= table.position_indices->indices.size()) {
            for (int i = 0; i < source->position_index_sources_counts; ++i) {
                drawable.indices.append(static_cast<quint16>(table.position_indices->indices[
                    source->position_index_sources_begin_indices + i]));
            }
        }
        if (source->drawable_mask_sources_begin_indices >= 0) {
            for (int i = 0; i < source->drawable_mask_sources_counts; ++i) {
                const int index = source->drawable_mask_sources_begin_indices + i;
                if (inRange(index, table.drawable_masks.size())) {
                    drawable.maskIndices.append(table.drawable_masks[index]->art_mesh_sources_indices);
                }
            }
        }
        if (source->keyform_sources_begin_indices >= 0) {
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
                if (begin >= 0 && begin + source->vertex_counts * 2 <= table.keyform_positions->xys.size()) {
                    for (int v = 0; v < source->vertex_counts * 2; ++v) {
                        keyform.positions.append(table.keyform_positions->xys[begin + v]);
                    }
                }
                drawable.keyforms.append(keyform);
            }
        }
        m_drawables.append(drawable);
    }

    m_loaded = true;
    return true;
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
    }

    QSet<QString> partIds;
    QVector<int> partStates(m_parts.size(), 0);
    std::function<void(int)> visitPart = [&](int index) {
        if (!inRange(index, m_parts.size())) return;
        if (partStates[index] == 1) {
            diagnostics.append({CModelDiagnostic::Error, QStringLiteral("part_cycle"),
                                 QStringLiteral("Part 父子关系存在循环"), index});
            return;
        }
        if (partStates[index] == 2) return;
        partStates[index] = 1;
        visitPart(m_parts[index].parentIndex);
        partStates[index] = 2;
    };
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
        visitPart(i);
    }

    QVector<int> states(m_deformers.size(), 0);
    std::function<void(int)> visit = [&](int index) {
        if (!inRange(index, m_deformers.size())) return;
        if (states[index] == 1) {
            diagnostics.append({CModelDiagnostic::Error, QStringLiteral("deformer_cycle"),
                                 QStringLiteral("Deformer 父子关系存在循环"), index});
            return;
        }
        if (states[index] == 2) return;
        states[index] = 1;
        const int parent = m_deformers[index].parentDeformerIndex;
        if (parent != -1 && !inRange(parent, m_deformers.size())) {
            diagnostics.append({CModelDiagnostic::Error, QStringLiteral("deformer_parent"),
                                 QStringLiteral("Deformer 父索引越界"), index});
        } else {
            visit(parent);
        }
        states[index] = 2;
    };
    for (int i = 0; i < m_deformers.size(); ++i) {
        if (m_deformers[i].parentPartIndex != -1 && !inRange(m_deformers[i].parentPartIndex, m_parts.size())) {
            diagnostics.append({CModelDiagnostic::Error, QStringLiteral("deformer_part"),
                                 QStringLiteral("Deformer 所属 Part 越界"), i});
        }
        visit(i);
    }

    QSet<QString> drawableIds;
    for (int i = 0; i < m_drawables.size(); ++i) {
        const CModelDrawable& drawable = m_drawables[i];
        if (drawable.id.isEmpty() || drawableIds.contains(drawable.id)) {
            diagnostics.append({CModelDiagnostic::Error, QStringLiteral("drawable_id"),
                                 QStringLiteral("ArtMesh ID 为空或重复"), i});
        }
        drawableIds.insert(drawable.id);
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
        for (int mask : drawable.maskIndices) {
            if (!inRange(mask, m_drawables.size())) {
                diagnostics.append({CModelDiagnostic::Error, QStringLiteral("mask_range"),
                                     QStringLiteral("Clipping mask 索引越界"), i});
            }
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
        }
    }
    return diagnostics;
}

}
