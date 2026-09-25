#include "cblendshaperelations.h"
#include "csectiontable.h"

#include <QHash>
#include <QSet>
#include <algorithm>
#include <array>
#include <cmath>
#include <exception>

namespace live2dstruct {
namespace {
bool contains(int index, qsizetype size) { return index >= 0 && index < size; }
bool validSlice(CBlendShapeRelations::Slice slice, qsizetype size)
{
    if (!slice.count) return slice.begin >= -1 && slice.begin <= size;
    return slice.begin >= 0 && slice.count > 0 && qint64(slice.begin) + slice.count <= size;
}
QString targetKey(CBlendShapeRelations::TargetType type, const QString& id)
{
    return QString::number(int(type)) + QLatin1Char('|') + id;
}
template<typename T> const QVector<T>& empty()
{
    static const QVector<T> value;
    return value;
}
}

struct CBlendShapeRelations::State
{
    QVector<Parameter> parameters;
    QVector<int> blendParameters;
    QVector<ParameterBinding> parameterBindings;
    QVector<Binding> bindings;
    QVector<Target> targets;
    QVector<Shape> shapes;
    QVector<float> keys;
    QVector<ConstraintValue> values;
    QVector<Limiter> constraints;
    QVector<int> constraintIndices;
    QVector<Diagnostic> diagnostics;
    QHash<QString, int> parameterIds, targetIds;
    std::array<QHash<int, QVector<int>>, 2> parameterRows;
    std::array<QVector<quint8>, 2> usableRows;
    int artCount = 0, warpCount = 0;

    void report(const QString& code, const QString& table, int row, const QString& message)
    {
        diagnostics.append({code, message, table, row});
    }

    void read(const CSectionTable& table)
    {
        artCount = table.art_meshes.size();
        warpCount = table.warp_deformers.size();
        if (table.keys) keys = QVector<float>(table.keys->values.cbegin(), table.keys->values.cend());
        QVector<int> badKeys(keys.size() + 1);
        for (int index = 0; index < keys.size(); ++index)
            badKeys[index + 1] = badKeys[index] + !std::isfinite(keys[index]);
        parameterBindings.reserve(table.blend_shape_parameter_bindings.size());
        for (int index = 0; index < table.blend_shape_parameter_bindings.size(); ++index) {
            const auto* source = table.blend_shape_parameter_bindings[index];
            ParameterBinding binding;
            binding.index = index;
            if (source) {
                binding.keys = {source->keys_sources_begin_indices, source->keys_sources_counts};
                binding.baseKeyIndex = source->base_key_indices;
                binding.valid = validSlice(binding.keys, keys.size()) && (!binding.keys.count ||
                    badKeys[binding.keys.begin] == badKeys[binding.keys.begin + binding.keys.count]);
            }
            if (!binding.valid) report("bs_keys", "blend_shape_parameter_bindings", index, QStringLiteral("融合参数关键值区间无效或包含非有限值"));
            parameterBindings.append(binding);
        }

        QVector<int> ownerDifference(parameterBindings.size() + 1);
        QVector<quint32> ownerXor(parameterBindings.size() + 1);
        parameters.reserve(table.parameters.size());
        for (int index = 0; index < table.parameters.size(); ++index) {
            const auto* source = table.parameters[index];
            Parameter parameter;
            parameter.index = index;
            if (source) {
                parameter.id = source->ids;
                parameter.type = source->parameter_types;
                parameter.minimum = source->min_values;
                parameter.maximum = source->max_values;
                parameter.defaultValue = source->default_values;
                parameter.selectable = !parameter.id.isEmpty() && std::isfinite(parameter.minimum) &&
                    std::isfinite(parameter.maximum) && std::isfinite(parameter.defaultValue) &&
                    parameter.minimum <= parameter.defaultValue && parameter.defaultValue <= parameter.maximum;
                const Slice owned{source->blend_shape_parameter_binding_sources_begin_indices,
                                  source->blend_shape_parameter_binding_sources_counts};
                if (!validSlice(owned, parameterBindings.size()))
                    report("bs_parameter_slice", "parameters", index, QStringLiteral("融合参数绑定所属区间无效"));
                else if (owned.count) {
                    ++ownerDifference[owned.begin];
                    --ownerDifference[owned.begin + owned.count];
                    ownerXor[owned.begin] ^= quint32(index + 1);
                    ownerXor[owned.begin + owned.count] ^= quint32(index + 1);
                }
            }
            if (!parameter.selectable) report("bs_parameter_metadata", "parameters", index, QStringLiteral("参数标识或数值范围无效"));
            if (parameter.type != 0 && parameter.type != 1) {
                parameter.selectable = false;
                report("bs_parameter_type", "parameters", index, QStringLiteral("参数类型无效"));
            }
            if (parameter.type == 1) blendParameters.append(index);
            if (!parameter.id.isEmpty()) {
                const auto found = parameterIds.constFind(parameter.id);
                if (found != parameterIds.cend()) {
                    if (found.value() >= 0) parameters[found.value()].selectable = false;
                    parameter.selectable = false;
                    parameterIds.insert(parameter.id, -1);
                    report("bs_parameter_id", "parameters", index, QStringLiteral("参数标识重复：%1").arg(parameter.id));
                } else parameterIds.insert(parameter.id, index);
            }
            parameters.append(parameter);
        }
        QVector<int> owners(parameterBindings.size(), -1);
        int ownerCount = 0;
        quint32 owner = 0;
        for (int index = 0; index < parameterBindings.size(); ++index) {
            ownerCount += ownerDifference[index];
            owner ^= ownerXor[index];
            if (ownerCount == 1) owners[index] = int(owner) - 1;
            else if (ownerCount > 1) {
                owners[index] = -2;
                report("bs_binding_owner", "blend_shape_parameter_bindings", index, QStringLiteral("融合参数绑定同时属于多个参数"));
            }
        }

        values.reserve(table.blend_shape_constraint_values.size());
        QVector<int> badValues(table.blend_shape_constraint_values.size() + 1);
        for (int index = 0; index < table.blend_shape_constraint_values.size(); ++index) {
            const auto* source = table.blend_shape_constraint_values[index];
            values.append(source ? ConstraintValue{source->keys, source->weights} : ConstraintValue{});
            badValues[index + 1] = badValues[index] + (!source || !std::isfinite(source->keys) || !std::isfinite(source->weights));
        }
        for (int index = 0; index < table.blend_shape_constraints.size(); ++index) {
            const auto* source = table.blend_shape_constraints[index];
            Limiter limiter;
            limiter.constraintIndex = index;
            if (source) {
                limiter.parameterIndex = source->parameter_indices;
                limiter.values = {source->blend_shape_constraint_value_sources_begin_indices,
                                  source->blend_shape_constraint_value_sources_counts};
                limiter.valid = contains(limiter.parameterIndex, parameters.size()) && parameters[limiter.parameterIndex].selectable &&
                    validSlice(limiter.values, values.size()) && (!limiter.values.count ||
                    badValues[limiter.values.begin] == badValues[limiter.values.begin + limiter.values.count]);
            }
            if (!limiter.valid) report("bs_constraint", "blend_shape_constraints", index, QStringLiteral("权重限制参数或曲线区间无效"));
            constraints.append(limiter);
        }
        for (int index = 0; index < table.blend_shape_constraint_indices.size(); ++index) {
            const auto* source = table.blend_shape_constraint_indices[index];
            const int reference = source ? source->blend_shape_constraint_sources_indices : -1;
            constraintIndices.append(reference);
            if (!contains(reference, constraints.size()))
                report("bs_constraint_reference", "blend_shape_constraint_indices", index, QStringLiteral("权重限制索引越界"));
        }

        const std::array<int, 2> keyformCounts{int(table.art_mesh_keyforms.size()), int(table.warp_deformer_keyforms.size())};
        std::array<QVector<int>, 2> badRows{
            QVector<int>(table.blend_shape_keyform_bindings.size() + 1), QVector<int>(table.blend_shape_keyform_bindings.size() + 1)};
        for (int index = 0; index < table.blend_shape_keyform_bindings.size(); ++index) {
            const auto* source = table.blend_shape_keyform_bindings[index];
            Binding binding;
            binding.index = index;
            if (source) {
                binding.parameterBindingIndex = source->parameter_binding_sources_indices;
                binding.keyforms = {source->keyform_sources_blend_shape_indices, source->keyform_sources_blend_shape_counts};
                binding.constraints = {source->blend_shape_constraint_index_sources_begin_indices,
                                       source->blend_shape_constraint_index_sources_counts};
                if (contains(binding.parameterBindingIndex, owners.size())) {
                    binding.parameterIndex = owners[binding.parameterBindingIndex];
                    binding.valid = contains(binding.parameterIndex, parameters.size()) &&
                        parameters[binding.parameterIndex].selectable && parameters[binding.parameterIndex].type == 1 &&
                        parameterBindings[binding.parameterBindingIndex].valid;
                }
            }
            if (!binding.valid) report("bs_keyform_owner", "blend_shape_keyform_bindings", index, QStringLiteral("融合关键形绑定缺少唯一有效的融合参数"));
            if (!source || !validSlice(binding.constraints, constraintIndices.size()))
                report("bs_constraint_slice", "blend_shape_keyform_bindings", index, QStringLiteral("权重限制引用区间无效"));
            for (int type = 0; type < 2; ++type) {
                const bool span = source && validSlice(binding.keyforms, keyformCounts[type]);
                badRows[type][index + 1] = badRows[type][index] + !span;
                usableRows[type].append(binding.valid && span);
                if (binding.valid && span) parameterRows[type][binding.parameterIndex].append(index);
            }
            bindings.append(binding);
        }

        const auto addTarget = [&](Target target) {
            target.selectable = !target.id.isEmpty();
            const auto key = targetKey(target.type, target.id);
            const auto found = targetIds.constFind(key);
            if (found != targetIds.cend()) {
                if (found.value() >= 0) targets[found.value()].selectable = false;
                target.selectable = false;
                targetIds.insert(key, -1);
                report("bs_target_id", target.type == ArtMesh ? "art_meshes" : "deformers", target.objectIndex,
                       QStringLiteral("目标标识重复：%1").arg(target.id));
            } else targetIds.insert(key, targets.size());
            if (target.id.isEmpty()) report("bs_target_id", target.type == ArtMesh ? "art_meshes" : "deformers",
                                           target.objectIndex, QStringLiteral("目标标识为空"));
            if (target.type == Warp && !contains(target.sourceIndex, warpCount)) {
                target.selectable = false;
                report("bs_warp_source", "deformers", target.objectIndex, QStringLiteral("网格变形器专用源索引无效"));
            }
            targets.append(target);
        };
        for (int index = 0; index < table.art_meshes.size(); ++index) {
            const auto* source = table.art_meshes[index];
            addTarget({ArtMesh, index, index, source ? source->ids : QString(), false});
        }
        for (int index = 0; index < table.deformers.size(); ++index) {
            const auto* source = table.deformers[index];
            if (source && source->types == 0) addTarget({Warp, index, source->specific_sources_indices, source->ids, false});
        }
        const auto addShapes = [&](const auto& sources, TargetType type, int size) {
            const QString tableName = type == ArtMesh ? "blend_shapes_art_meshes" : "blend_shapes_warp_deformers";
            for (int index = 0; index < sources.size(); ++index) {
                const auto* source = sources[index];
                Shape shape;
                shape.type = type;
                shape.index = index;
                if (source) {
                    shape.sourceIndex = source->target_indices;
                    shape.bindings = {source->blend_shape_keyform_binding_sources_begin_indices,
                                      source->blend_shape_keyform_binding_sources_counts};
                    shape.valid = contains(shape.sourceIndex, size) && validSlice(shape.bindings, bindings.size());
                }
                if (!shape.valid) report("bs_shape", tableName, index, QStringLiteral("融合目标或关键形绑定区间无效"));
                else if (shape.bindings.count && badRows[type][shape.bindings.begin] !=
                         badRows[type][shape.bindings.begin + shape.bindings.count])
                    report("bs_keyform_slice", tableName, index, QStringLiteral("融合关键形行区间与目标类型不匹配"));
                shapes.append(shape);
            }
        };
        addShapes(table.blend_shapes_art_meshes, ArtMesh, artCount);
        addShapes(table.blend_shapes_warp_deformers, Warp, warpCount);
    }
};

bool CBlendShapeRelations::Build(const CSectionTable& table, QString* error)
{
    if (error) error->clear();
    if (!table.IsValid()) {
        if (error) *error = QStringLiteral("融合关系需要有效的模型区段");
        return false;
    }
    try {
        auto candidate = std::make_shared<State>();
        candidate->read(table);
        m_state = std::move(candidate);
        return true;
    } catch (const std::exception&) {
        if (error) *error = QStringLiteral("融合关系索引所需内存不足");
        return false;
    }
}

const QVector<CBlendShapeRelations::Parameter>& CBlendShapeRelations::Parameters() const { return m_state ? m_state->parameters : empty<Parameter>(); }
const QVector<int>& CBlendShapeRelations::BlendShapeParameterIndices() const { return m_state ? m_state->blendParameters : empty<int>(); }
const QVector<CBlendShapeRelations::ParameterBinding>& CBlendShapeRelations::ParameterBindings() const { return m_state ? m_state->parameterBindings : empty<ParameterBinding>(); }
const QVector<CBlendShapeRelations::Binding>& CBlendShapeRelations::Bindings() const { return m_state ? m_state->bindings : empty<Binding>(); }
const QVector<CBlendShapeRelations::Target>& CBlendShapeRelations::Targets() const { return m_state ? m_state->targets : empty<Target>(); }
const QVector<CBlendShapeRelations::Shape>& CBlendShapeRelations::Shapes() const { return m_state ? m_state->shapes : empty<Shape>(); }
const QVector<float>& CBlendShapeRelations::Keys() const { return m_state ? m_state->keys : empty<float>(); }
const QVector<CBlendShapeRelations::ConstraintValue>& CBlendShapeRelations::ConstraintValues() const { return m_state ? m_state->values : empty<ConstraintValue>(); }
const QVector<CBlendShapeRelations::Diagnostic>& CBlendShapeRelations::Diagnostics() const { return m_state ? m_state->diagnostics : empty<Diagnostic>(); }

int CBlendShapeRelations::ParameterIndex(const QString& id) const
{
    if (!m_state) return -1;
    const int index = m_state->parameterIds.value(id, -1);
    return contains(index, m_state->parameters.size()) && m_state->parameters[index].selectable ? index : -1;
}

QVector<CBlendShapeRelations::Target> CBlendShapeRelations::TargetsForParameter(const QString& id) const
{
    return TargetsForParameter(ParameterIndex(id));
}

QVector<CBlendShapeRelations::Target> CBlendShapeRelations::TargetsForParameter(int parameterIndex) const
{
    QVector<Target> result;
    if (!m_state || !contains(parameterIndex, m_state->parameters.size()) || !m_state->parameters[parameterIndex].selectable) return result;
    std::array<QSet<int>, 2> selected;
    for (const auto& shape : m_state->shapes) {
        if (!shape.valid || !shape.bindings.count) continue;
        const auto rows = m_state->parameterRows[shape.type].constFind(parameterIndex);
        if (rows == m_state->parameterRows[shape.type].cend()) continue;
        const auto found = std::lower_bound(rows->cbegin(), rows->cend(), shape.bindings.begin);
        if (found != rows->cend() && *found < qint64(shape.bindings.begin) + shape.bindings.count)
            selected[shape.type].insert(shape.sourceIndex);
    }
    for (const auto& target : m_state->targets)
        if (target.selectable && selected[target.type].contains(target.sourceIndex)) result.append(target);
    return result;
}

QVector<int> CBlendShapeRelations::BindingsForTarget(TargetType type, const QString& id) const
{
    QVector<int> result;
    if (!m_state || (type != ArtMesh && type != Warp)) return result;
    const int index = m_state->targetIds.value(targetKey(type, id), -1);
    if (!contains(index, m_state->targets.size()) || !m_state->targets[index].selectable) return result;
    const int sourceIndex = m_state->targets[index].sourceIndex;
    QVector<Slice> slices;
    for (const auto& shape : m_state->shapes)
        if (shape.valid && shape.type == type && shape.sourceIndex == sourceIndex && shape.bindings.count) slices.append(shape.bindings);
    std::sort(slices.begin(), slices.end(), [](Slice first, Slice second) { return first.begin < second.begin; });
    int visited = 0;
    for (const auto& slice : slices) {
        const int end = slice.begin + slice.count;
        for (int row = std::max(visited, slice.begin); row < end; ++row)
            if (m_state->usableRows[type][row]) result.append(row);
        visited = std::max(visited, end);
    }
    return result;
}

QVector<int> CBlendShapeRelations::ParametersForTarget(TargetType type, const QString& id) const
{
    QVector<int> result;
    QSet<int> selected;
    for (int binding : BindingsForTarget(type, id)) selected.insert(m_state->bindings[binding].parameterIndex);
    for (int index : selected) result.append(index);
    std::sort(result.begin(), result.end());
    return result;
}

QVector<CBlendShapeRelations::Limiter> CBlendShapeRelations::LimitersForBinding(int bindingIndex) const
{
    QVector<Limiter> result;
    if (!m_state || !contains(bindingIndex, m_state->bindings.size())) return result;
    const auto slice = m_state->bindings[bindingIndex].constraints;
    if (!validSlice(slice, m_state->constraintIndices.size())) return result;
    QSet<int> selected;
    for (int index = 0; index < slice.count; ++index) {
        const int constraint = m_state->constraintIndices[slice.begin + index];
        if (!contains(constraint, m_state->constraints.size()) || !m_state->constraints[constraint].valid || selected.contains(constraint)) continue;
        selected.insert(constraint);
        result.append(m_state->constraints[constraint]);
    }
    return result;
}
}
