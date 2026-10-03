#include "cblendshapeposeevaluator.h"
#include "cblendshaperelations.h"
#include "csectiontable.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace live2dstruct {
namespace {
bool fail(QString* error, const QString& message)
{
    if (error) *error = message;
    return false;
}

bool finiteSlice(const std::vector<float>& values, int begin, int count)
{
    return begin >= 0 && count >= 0 && begin <= values.size() && count <= values.size() - begin &&
        std::all_of(values.cbegin() + begin, values.cbegin() + begin + count,
                    [](float value) { return std::isfinite(value); });
}

bool finiteSlice(const QVector<float>& values, int begin, int count)
{
    return begin >= 0 && count >= 0 && begin <= values.size() && count <= values.size() - begin &&
        std::all_of(values.cbegin() + begin, values.cbegin() + begin + count,
                    [](float value) { return std::isfinite(value); });
}

int parameterForBinding(const CSectionTable& table, int bindingIndex)
{
    for (int index = 0; index < table.parameters.size(); ++index) {
        const auto* parameter = table.parameters[index];
        if (!parameter || parameter->parameter_types != 1) continue;
        if (bindingIndex >= parameter->blend_shape_parameter_binding_sources_begin_indices &&
            bindingIndex < parameter->blend_shape_parameter_binding_sources_begin_indices +
                               parameter->blend_shape_parameter_binding_sources_counts)
            return index;
    }
    return -1;
}

float curveValue(const QVector<const CBlend_shape_constraint_values*>& values, float current)
{
    if (values.isEmpty()) return 1.0f;
    if (current <= values.front()->keys) return values.front()->weights;
    if (current >= values.back()->keys) return values.back()->weights;
    for (int index = 1; index < values.size(); ++index) {
        const float left = values[index - 1]->keys;
        const float right = values[index]->keys;
        if (current > right) continue;
        if (right <= left) return values[index]->weights;
        const float t = (current - left) / (right - left);
        return values[index - 1]->weights + t * (values[index]->weights - values[index - 1]->weights);
    }
    return values.back()->weights;
}

float weightFor(float current, float base, float key)
{
    if (!std::isfinite(current) || !std::isfinite(base) || !std::isfinite(key) || key == base) return 0.0f;
    const float distance = current - base;
    const float span = key - base;
    if (distance == 0.0f || std::signbit(distance) != std::signbit(span)) return 0.0f;
    return std::clamp(std::abs(distance / span), 0.0f, 1.0f);
}

bool positionRow(const std::vector<float>& values, int begin, int count, QVector<float>& result)
{
    if (!finiteSlice(values, begin, count)) return false;
    result = QVector<float>(values.cbegin() + begin, values.cbegin() + begin + count);
    return true;
}
}

bool CBlendShapePoseEvaluator::Build(const CSectionTable& table, const QMap<QString, float>& fullPose,
                                     Overrides& result, QString* error) const
{
    result = {};
    if (error) error->clear();
    if (!table.IsValid()) return fail(error, QStringLiteral("BlendShape 姿态需要有效的模型区段"));
    for (const auto* parameter : table.parameters) {
        if (!parameter || parameter->ids.isEmpty() || !fullPose.contains(parameter->ids) ||
            !std::isfinite(fullPose.value(parameter->ids)))
            return fail(error, QStringLiteral("BlendShape 姿态缺少参数：%1").arg(parameter ? parameter->ids : QString()));
    }

    CBlendShapeRelations relations;
    if (!relations.Build(table, error)) return false;
    const auto evaluateConstraints = [&](const CBlend_shape_keyform_bindings* binding, int parameterIndex,
                                         float weight, float& limited) {
        limited = weight;
        if (!binding || binding->blend_shape_constraint_index_sources_begin_indices < 0 ||
            binding->blend_shape_constraint_index_sources_counts < 0 ||
            binding->blend_shape_constraint_index_sources_begin_indices > table.blend_shape_constraint_indices.size() -
                binding->blend_shape_constraint_index_sources_counts)
            return false;
        for (int index = 0; index < binding->blend_shape_constraint_index_sources_counts; ++index) {
            const auto* reference = table.blend_shape_constraint_indices[
                binding->blend_shape_constraint_index_sources_begin_indices + index];
            if (!reference || reference->blend_shape_constraint_sources_indices < 0 ||
                reference->blend_shape_constraint_sources_indices >= table.blend_shape_constraints.size()) return false;
            const auto* constraint = table.blend_shape_constraints[reference->blend_shape_constraint_sources_indices];
            if (!constraint || constraint->parameter_indices < 0 || constraint->parameter_indices >= table.parameters.size() ||
                constraint->blend_shape_constraint_value_sources_begin_indices < 0 ||
                constraint->blend_shape_constraint_value_sources_counts < 0 ||
                constraint->blend_shape_constraint_value_sources_begin_indices > table.blend_shape_constraint_values.size() -
                    constraint->blend_shape_constraint_value_sources_counts) return false;
            const auto* constraintParameter = table.parameters[constraint->parameter_indices];
            QVector<const CBlend_shape_constraint_values*> values;
            for (int value = 0; value < constraint->blend_shape_constraint_value_sources_counts; ++value) {
                const auto* item = table.blend_shape_constraint_values[
                    constraint->blend_shape_constraint_value_sources_begin_indices + value];
                if (!item || !std::isfinite(item->keys) || !std::isfinite(item->weights)) return false;
                values.append(item);
            }
            std::sort(values.begin(), values.end(), [](const auto* left, const auto* right) { return left->keys < right->keys; });
            const float limit = curveValue(values, fullPose.value(constraintParameter->ids));
            if (!std::isfinite(limit)) return false;
            limited = std::min(limited, std::clamp(limit, 0.0f, 1.0f) * weight);
        }
        Q_UNUSED(parameterIndex);
        return std::isfinite(limited);
    };

    const auto evaluateShapes = [&](const auto& shapes, int targetType, auto&& apply) {
        for (const auto* shape : shapes) {
            if (!shape || shape->target_indices < 0 || shape->blend_shape_keyform_binding_sources_begin_indices < 0 ||
                shape->blend_shape_keyform_binding_sources_counts < 0 ||
                shape->blend_shape_keyform_binding_sources_begin_indices > table.blend_shape_keyform_bindings.size() -
                    shape->blend_shape_keyform_binding_sources_counts) return false;
            for (int offset = 0; offset < shape->blend_shape_keyform_binding_sources_counts; ++offset) {
                const int bindingIndex = shape->blend_shape_keyform_binding_sources_begin_indices + offset;
                const auto* binding = table.blend_shape_keyform_bindings[bindingIndex];
                const int parameterIndex = binding ? parameterForBinding(table, binding->parameter_binding_sources_indices) : -1;
                if (!binding || parameterIndex < 0 || binding->parameter_binding_sources_indices < 0 ||
                    binding->parameter_binding_sources_indices >= table.blend_shape_parameter_bindings.size()) return false;
                const auto* parameterBinding = table.blend_shape_parameter_bindings[binding->parameter_binding_sources_indices];
                if (!parameterBinding || parameterBinding->keys_sources_begin_indices < 0 ||
                    parameterBinding->keys_sources_counts <= 0 ||
                    parameterBinding->keys_sources_begin_indices > table.keys->values.size() - parameterBinding->keys_sources_counts ||
                    parameterBinding->base_key_indices < 0 || parameterBinding->base_key_indices >= parameterBinding->keys_sources_counts ||
                    binding->keyform_sources_blend_shape_counts != parameterBinding->keys_sources_counts ||
                    binding->keyform_sources_blend_shape_indices < 0) return false;
                const auto* parameter = table.parameters[parameterIndex];
                const float current = fullPose.value(parameter->ids);
                const float base = table.keys->GetData(parameterBinding->keys_sources_begin_indices + parameterBinding->base_key_indices);
                for (int local = 0; local < binding->keyform_sources_blend_shape_counts; ++local) {
                    if (local == parameterBinding->base_key_indices) continue;
                    const float key = table.keys->GetData(parameterBinding->keys_sources_begin_indices + local);
                    float weight = weightFor(current, base, key), limited = 0.0f;
                    if (weight == 0.0f) continue;
                    if (!evaluateConstraints(binding, parameterIndex, weight, limited)) return false;
                    if (limited == 0.0f) continue;
                    apply(targetType, shape->target_indices, binding->keyform_sources_blend_shape_indices + local,
                          parameterIndex, limited);
                }
            }
        }
        return true;
    };

    const auto applyPosition = [&](int type, int target, int row, int parameter, float weight) {
        QVector<float> base, morph;
        int count = 0;
        if (type == CBlendShapeRelations::ArtMesh) {
            if (target >= table.art_meshes.size()) return;
            const auto* mesh = table.art_meshes[target];
            count = mesh->vertex_counts * 2;
            if (mesh->keyform_sources_begin_indices < 0 || mesh->keyform_sources_begin_indices >= table.art_mesh_keyforms.size() ||
                row < 0 || row >= table.art_mesh_keyforms.size()) return;
            const auto* baseForm = table.art_mesh_keyforms[mesh->keyform_sources_begin_indices];
            const auto* morphForm = table.art_mesh_keyforms[row];
            if (!baseForm || !morphForm || !positionRow(table.keyform_positions->xys,
                                                        baseForm->keyform_position_sources_begin_indices, count, base) ||
                !positionRow(table.keyform_positions->xys,
                             morphForm->keyform_position_sources_begin_indices, count, morph)) return;
            auto& delta = result.artMeshPositions[mesh->ids];
            if (delta.isEmpty()) delta.fill(0.0f, count);
            for (int index = 0; index < count; ++index) delta[index] += (morph[index] - base[index]) * weight;
        } else if (type == CBlendShapeRelations::Warp) {
            if (target >= table.warp_deformers.size()) return;
            const auto* warp = table.warp_deformers[target];
            count = warp->vertex_counts * 2;
            if (warp->keyform_sources_begin_indices < 0 || warp->keyform_sources_begin_indices >= table.warp_deformer_keyforms.size() ||
                row < 0 || row >= table.warp_deformer_keyforms.size()) return;
            const auto* baseForm = table.warp_deformer_keyforms[warp->keyform_sources_begin_indices];
            const auto* morphForm = table.warp_deformer_keyforms[row];
            if (!baseForm || !morphForm || !positionRow(table.keyform_positions->xys,
                                                        baseForm->keyform_position_sources_begin_indices, count, base) ||
                !positionRow(table.keyform_positions->xys,
                             morphForm->keyform_position_sources_begin_indices, count, morph)) return;
            QString id;
            for (const auto* deformer : table.deformers)
                if (deformer && deformer->types == 0 && deformer->specific_sources_indices == target) { id = deformer->ids; break; }
            if (id.isEmpty()) return;
            auto& delta = result.warpPositions[id];
            if (delta.isEmpty()) delta.fill(0.0f, count);
            for (int index = 0; index < count; ++index) delta[index] += (morph[index] - base[index]) * weight;
        }
        Q_UNUSED(parameter);
    };
    if (!evaluateShapes(table.blend_shapes_art_meshes, CBlendShapeRelations::ArtMesh, applyPosition) ||
        !evaluateShapes(table.blend_shapes_warp_deformers, CBlendShapeRelations::Warp, applyPosition))
        return fail(error, QStringLiteral("BlendShape 绑定或限制器数据无效"));
    if (error) error->clear();
    return true;
}
}
