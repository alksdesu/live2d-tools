#include "cparameterselectionquery.h"
#include "cblendshaperelations.h"
#include "cmodeldocument.h"
#include "cparentgraph.h"
#include "csectiontable.h"
#include <QSet>
#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <functional>
#include <numeric>

namespace live2dstruct {
namespace {
bool fail(QString* error, const QString& message) { if (error) *error = message; return false; }
bool index(int value, qsizetype size) { return value >= 0 && value < size; }
bool slice(int begin, int count, qsizetype size)
{
    return count == 0 ? begin >= -1 && begin <= size : begin >= 0 && count > 0 && begin <= size && count <= size - begin;
}
QString key(const QString& type, const QString& id) { return type + QChar('|') + id; }
int next(QVector<int>& values, int at)
{
    int root = at;
    while (values[root] != root) root = values[root];
    while (values[at] != at) { const int previous = at; at = values[at]; values[previous] = root; }
    return root;
}
void visitSlice(QVector<int>& pending, int begin, int count, const std::function<void(int)>& visit)
{
    if (!count) return;
    for (int at = next(pending, begin); at < begin + count; at = next(pending, at)) {
        pending[at] = next(pending, at + 1); visit(at);
    }
}
QVector<int> unvisited(int count) { QVector<int> result(count + 1); std::iota(result.begin(), result.end(), 0); return result; }
}

struct CParameterSelectionQuery::State
{
    struct Object { Target target; int part = -1, deformer = -1, binding = -1; };
    QVector<Object> objects;
    QHash<QString, int> byKey;
    QVector<Parameter> parameters;
    QVector<int> bindingOwners, references, constraintParameters;
    QSet<int> glueObjects;
    QVector<QPair<int, int>> bindings;
    QVector<bool> supportedBindings, parameterUsable;
    QVector<QVector<int>> descendants;
    int partCount = 0, deformerCount = 0;
    std::array<int, 4> formCounts{};
    CBlendShapeRelations blend;
    QHash<QString, int> blendSources;
    QByteArray base, digest;
    QHash<QString, int> parentOverrides;
    QHash<QString, CModelParameterMetadata> metadata;
    bool fromDocument = false;

    void classifyParameters()
    {
        QHash<QString, int> counts;
        for (const auto& parameter : parameters) ++counts[parameter.id];
        parameterUsable.clear(); parameterUsable.reserve(parameters.size());
        for (const auto& parameter : parameters)
            parameterUsable.append(!parameter.id.isEmpty() && counts.value(parameter.id) == 1 &&
                std::isfinite(parameter.minimum) && std::isfinite(parameter.maximum) && std::isfinite(parameter.defaultValue) &&
                parameter.minimum <= parameter.maximum);
    }

    bool hierarchy(QString* error)
    {
        QVector<int> parts, deformers;
        for (int at = 0; at < partCount; ++at) parts.append(objects[at].part);
        for (int at = 0; at < deformerCount; ++at) deformers.append(objects[partCount + at].deformer);
        if (!CParentGraph(parts).IsValid() || !CParentGraph(deformers).IsValid())
            return fail(error, QStringLiteral("关联参数的部件或变形器父链无效"));
        descendants.clear(); descendants.resize(objects.size());
        for (int at = 0; at < objects.size(); ++at) {
            const auto& object = objects[at];
            if (object.part < -1 || object.part >= partCount || object.deformer < -1 || object.deformer >= deformerCount)
                return fail(error, QStringLiteral("关联对象的父级引用越界"));
            if (object.part >= 0) descendants[object.part].append(at);
            if (object.deformer >= 0) descendants[partCount + object.deformer].append(at);
        }
        return true;
    }

    bool read(const CSectionTable& table, QString* error)
    {
        if (!table.IsValid()) return fail(error, QStringLiteral("关联参数需要有效的模型区段"));
        partCount = table.parts.size(); deformerCount = table.deformers.size();
        const auto add = [&](QString type, QString id, int part, int deformer, int binding) {
            const QString identity = key(type, id);
            if (id.isEmpty() || byKey.contains(identity) || binding < -1 || binding >= table.keyform_bindings.size()) return false;
            byKey.insert(identity, objects.size()); objects.append({{type, id}, part, deformer, binding}); return true;
        };
        const bool emptyOrdinaryBindings = table.keyform_bindings.isEmpty() && table.parameter_binding_indices.isEmpty() &&
            table.parameter_bindings.isEmpty() && (!table.keys || table.keys->values.empty());
        for (const auto* p : table.parts) {
            if (!p) return fail(error, QStringLiteral("关联部件标识或绑定无效"));
            const int binding = emptyOrdinaryBindings && p->keyform_binding_sources_indices == 0
                ? -1 : p->keyform_binding_sources_indices;
            if (!add("part", p->ids, p->parent_part_indices, -1, binding))
                return fail(error, QStringLiteral("关联部件标识或绑定无效"));
        }
        for (const auto* d : table.deformers) {
            if (!d || (d->types != 0 && d->types != 1) ||
                !add(d->types == 0 ? "warp" : "rotation", d->ids, d->parent_part_indices, d->parent_deformer_indices, d->keyform_binding_sources_indices))
                return fail(error, QStringLiteral("关联变形器标识、类型或绑定无效"));
        }
        for (const auto* m : table.art_meshes)
            if (!m || !add("artmesh", m->ids, m->parent_part_indices, m->parent_deformer_indices, m->keyform_binding_sources_indices))
                return fail(error, QStringLiteral("关联网格标识或绑定无效"));
        if (!hierarchy(error)) return false;
        for (const auto* glue : table.glue) if (glue) {
            for (int mesh : {glue->art_mesh_indices_a, glue->art_mesh_indices_b})
                if (index(mesh, table.art_meshes.size())) glueObjects.insert(partCount + deformerCount + mesh);
        }
        QVector<int> ownerCount(table.parameter_bindings.size() + 1), ownerXor(table.parameter_bindings.size() + 1);
        for (int at = 0; at < table.parameters.size(); ++at) {
            const auto* p = table.parameters[at];
            if (!p || !slice(p->parameter_binding_sources_begin_indices, p->parameter_binding_sources_counts, table.parameter_bindings.size()))
                return fail(error, QStringLiteral("关联参数定义或绑定区间无效"));
            parameters.append({p->ids, p->parameter_types, p->min_values, p->max_values, p->default_values});
            if (p->parameter_binding_sources_counts) {
                const int first = p->parameter_binding_sources_begin_indices, end = first + p->parameter_binding_sources_counts;
                ++ownerCount[first]; --ownerCount[end]; ownerXor[first] ^= at; ownerXor[end] ^= at;
            }
        }
        classifyParameters();
        int count = 0, owner = 0;
        for (int at = 0; at < table.parameter_bindings.size(); ++at) {
            count += ownerCount[at]; owner ^= ownerXor[at]; bindingOwners.append(count == 1 ? owner : -1);
        }
        static const std::vector<float> emptyKeys;
        const auto& keys = table.keys ? table.keys->values : emptyKeys;
        QVector<int> bad(keys.size() + 1), unordered(keys.size() + 1);
        for (int at = 0; at < int(keys.size()); ++at) {
            bad[at + 1] = bad[at] + !std::isfinite(keys[at]);
            unordered[at + 1] = unordered[at] + (at > 0 && keys[at] <= keys[at - 1]);
        }
        for (const auto* binding : table.parameter_bindings) {
            if (!binding || !slice(binding->keys_sources_begin_indices, binding->keys_sources_counts, keys.size()))
                return fail(error, QStringLiteral("关联参数关键值区间无效"));
            const int first = binding->keys_sources_begin_indices, length = binding->keys_sources_counts;
            supportedBindings.append(length > 0 && bad[first] == bad[first + length] && unordered[first + 1] == unordered[first + length]);
        }
        for (const auto* reference : table.parameter_binding_indices) {
            if (!reference || !index(reference->binding_sources_indices, bindingOwners.size()))
                return fail(error, QStringLiteral("关联参数绑定索引越界"));
            references.append(reference->binding_sources_indices);
        }
        for (const auto* binding : table.keyform_bindings) {
            if (!binding || !slice(binding->parameter_binding_index_sources_begin_indices, binding->parameter_binding_index_sources_counts, references.size()))
                return fail(error, QStringLiteral("关联关键形轴区间无效"));
            bindings.append({binding->parameter_binding_index_sources_begin_indices, binding->parameter_binding_index_sources_counts});
        }
        if (!blend.Build(table, error)) return false;
        for (const auto& target : blend.Targets()) if (target.selectable) {
            const QString type = target.type == CBlendShapeRelations::ArtMesh ? "artmesh" : target.type == CBlendShapeRelations::Warp ? "warp"
                : target.type == CBlendShapeRelations::Rotation ? "rotation" : "part";
            blendSources.insert(key(type, target.id), target.sourceIndex);
        }
        formCounts = {int(table.art_mesh_keyforms.size()), int(table.warp_deformer_keyforms.size()),
                      int(table.rotation_deformer_keyforms.size()), int(table.part_keyforms.size())};
        QVector<int> invalidValues(table.blend_shape_constraint_values.size() + 1);
        for (int at = 0; at < table.blend_shape_constraint_values.size(); ++at) {
            const auto* value = table.blend_shape_constraint_values[at];
            invalidValues[at + 1] = invalidValues[at] + (!value || !std::isfinite(value->keys) || !std::isfinite(value->weights));
        }
        for (const auto* reference : table.blend_shape_constraint_indices) {
            int parameter = -1;
            if (reference && index(reference->blend_shape_constraint_sources_indices, table.blend_shape_constraints.size())) {
                const auto* constraint = table.blend_shape_constraints[reference->blend_shape_constraint_sources_indices];
                if (constraint && index(constraint->parameter_indices, parameters.size()) && parameterUsable[constraint->parameter_indices]) {
                    const int first = constraint->blend_shape_constraint_value_sources_begin_indices, length = constraint->blend_shape_constraint_value_sources_counts;
                    if (slice(first, length, table.blend_shape_constraint_values.size()) && (!length || invalidValues[first] == invalidValues[first + length])) parameter = constraint->parameter_indices;
                }
            }
            constraintParameters.append(parameter);
        }
        return true;
    }
};

bool CParameterSelectionQuery::Build(const CSectionTable& section, QString* error)
{
    try {
        auto candidate = std::make_shared<State>();
        if (!candidate->read(section, error)) return false;
        m_state = std::move(candidate); if (error) error->clear(); return true;
    } catch (const std::exception&) { return fail(error, QStringLiteral("无法准备关联参数索引，原索引保持不变")); }
}

bool CParameterSelectionQuery::Build(const CModelDocument& document, QString* error)
{
    if (!document.IsLoaded()) return fail(error, QStringLiteral("尚未加载模型"));
    try {
        const bool sameBase = m_state && m_state->fromDocument && m_state->base == document.WorkingBaseBytes() && m_state->digest == document.SourceDigest();
        if (sameBase && m_state->parentOverrides == document.DeformerParentOverrides() && m_state->metadata == document.ParameterMetadataOverrides()) {
            if (error) error->clear(); return true;
        }
        std::shared_ptr<State> candidate;
        if (sameBase) candidate = std::make_shared<State>(*m_state);
        else {
            auto reader = CReader::FromBytes(document.WorkingBaseBytes()); CSectionTable table(reader);
            candidate = std::make_shared<State>(); if (!candidate->read(table, error)) return false;
        }
        if (candidate->parameters.size() != document.Parameters().size()) return fail(error, QStringLiteral("关联参数目录与文档不一致"));
        for (int at = 0; at < candidate->parameters.size(); ++at) {
            auto& parameter = candidate->parameters[at];
            if (parameter.id != document.Parameters()[at].id) return fail(error, QStringLiteral("关联参数标识与文档不一致"));
            const auto found = document.ParameterMetadataOverrides().constFind(parameter.id);
            const auto& source = document.Parameters()[at];
            parameter.minimum = found == document.ParameterMetadataOverrides().cend() ? source.minimum : found->minimum;
            parameter.maximum = found == document.ParameterMetadataOverrides().cend() ? source.maximum : found->maximum;
            parameter.defaultValue = found == document.ParameterMetadataOverrides().cend() ? source.defaultValue : found->defaultValue;
        }
        candidate->classifyParameters();
        if (candidate->deformerCount != document.Deformers().size()) return fail(error, QStringLiteral("关联变形器目录与文档不一致"));
        for (int at = 0; at < candidate->deformerCount; ++at) {
            auto& object = candidate->objects[candidate->partCount + at];
            if (object.target.id != document.Deformers()[at].id) return fail(error, QStringLiteral("关联变形器标识与文档不一致"));
            object.deformer = document.DeformerParentOverrides().value(object.target.id, document.Deformers()[at].parentDeformerIndex);
        }
        if (!candidate->hierarchy(error)) return false;
        candidate->base = document.WorkingBaseBytes(); candidate->digest = document.SourceDigest();
        candidate->parentOverrides = document.DeformerParentOverrides(); candidate->metadata = document.ParameterMetadataOverrides(); candidate->fromDocument = true;
        m_state = std::move(candidate); if (error) error->clear(); return true;
    } catch (const std::exception&) { return fail(error, QStringLiteral("无法准备文档关联参数，原索引保持不变")); }
}

bool CParameterSelectionQuery::Query(const QVector<Target>& selection, Result& result, QString* error) const
{
    if (!m_state) return fail(error, QStringLiteral("关联参数索引尚未准备"));
    try {
        const auto& state = *m_state;
        QVector<int> pending; QSet<int> expanded, included;
        for (const auto& target : selection) {
            const auto found = state.byKey.constFind(key(target.type, target.id));
            if (found == state.byKey.cend()) return fail(error, QStringLiteral("未找到关联对象：%1 · %2").arg(target.type, target.id));
            pending.append(*found);
        }
        while (!pending.isEmpty()) {
            const int at = pending.takeLast(); if (expanded.contains(at)) continue; expanded.insert(at); included.insert(at);
            if (state.objects[at].target.type != QLatin1String("artmesh")) for (int child : state.descendants[at]) pending.append(child);
        }
        pending = QVector<int>(included.cbegin(), included.cend());
        while (!pending.isEmpty()) {
            const auto& object = state.objects[pending.takeLast()];
            for (int parent : {object.part, object.deformer < 0 ? -1 : state.partCount + object.deformer})
                if (parent >= 0 && !included.contains(parent)) { included.insert(parent); pending.append(parent); }
        }
        QSet<int> ordinary, drivers, limiters;
        Result candidate;
        auto unused = unvisited(state.references.size());
        std::array<QSet<int>, 4> sources;
        for (int at : included) {
            const auto& object = state.objects[at];
            if (state.glueObjects.contains(at)) candidate.diagnostics.append(QStringLiteral("%1 受 Glue 影响；结果仅包含绑定及祖先关联参数").arg(object.target.id));
            if (object.binding >= 0) {
                const auto range = state.bindings[object.binding];
                visitSlice(unused, range.first, range.second, [&](int ref) {
                    const int binding = state.references[ref], parameter = state.bindingOwners[binding];
                    if (parameter >= 0 && state.parameterUsable[parameter] && state.parameters[parameter].type == 0 && state.supportedBindings[binding]) ordinary.insert(parameter);
                    else candidate.diagnostics.append(QStringLiteral("%1 的普通绑定缺少唯一有效的普通参数或关键值").arg(object.target.id));
                });
            }
            const auto source = state.blendSources.constFind(key(object.target.type, object.target.id));
            const int type = object.target.type == QLatin1String("artmesh") ? 0 : object.target.type == QLatin1String("warp") ? 1 : object.target.type == QLatin1String("rotation") ? 2 : 3;
            if (source != state.blendSources.cend()) sources[type].insert(*source);
        }
        std::array<QVector<int>, 4> rows;
        for (auto& entry : rows) entry = unvisited(state.blend.Bindings().size());
        auto constraints = unvisited(state.constraintParameters.size());
        for (const auto& shape : state.blend.Shapes()) {
            if (!shape.valid || !sources[shape.type].contains(shape.sourceIndex)) continue;
            visitSlice(rows[shape.type], shape.bindings.begin, shape.bindings.count, [&](int at) {
                const auto& binding = state.blend.Bindings()[at];
                if (!binding.valid || !slice(binding.keyforms.begin, binding.keyforms.count, state.formCounts[shape.type])) return;
                drivers.insert(binding.parameterIndex);
                if (!slice(binding.constraints.begin, binding.constraints.count, state.constraintParameters.size())) return;
                visitSlice(constraints, binding.constraints.begin, binding.constraints.count, [&](int ref) {
                    if (state.constraintParameters[ref] >= 0) limiters.insert(state.constraintParameters[ref]);
                });
            });
        }
        for (const auto& diagnostic : state.blend.Diagnostics()) candidate.diagnostics.append(diagnostic.message);
        candidate.diagnostics.removeDuplicates(); std::sort(candidate.diagnostics.begin(), candidate.diagnostics.end());
        for (int at = 0; at < state.parameters.size(); ++at) {
            const auto& id = state.parameters[at].id;
            if (ordinary.contains(at)) candidate.ordinary.append(id);
            if (drivers.contains(at)) candidate.blendDrivers.append(id);
            if (limiters.contains(at)) candidate.blendLimiters.append(id);
            if (ordinary.contains(at) || drivers.contains(at) || limiters.contains(at)) candidate.all.append(id);
        }
        result = std::move(candidate); if (error) error->clear(); return true;
    } catch (const std::exception&) { return fail(error, QStringLiteral("无法查询关联参数，原结果保持不变")); }
}

const QVector<CParameterSelectionQuery::Parameter>& CParameterSelectionQuery::Parameters() const
{
    static const QVector<Parameter> empty; return m_state ? m_state->parameters : empty;
}
}
