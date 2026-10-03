#include "ckeyformmappingcontext.h"
#include "cmodeldocument.h"
#include "ckeyformbindinggrid.h"
#include "cblendshapeposeevaluator.h"
#include "csectiontable.h"
#include "tree/c2dtree.h"

#include <QSet>
#include <algorithm>
#include <cmath>
#include <exception>
#include <utility>

namespace live2dstruct {
namespace {
bool fail(QString* error, const QString& message)
{
    if (error) *error = message;
    return false;
}

bool close(float actual, float expected)
{
    if (!std::isfinite(actual) || !std::isfinite(expected)) return false;
    const double difference = std::abs(double(actual) - expected);
    const double scale = std::max(std::abs(double(actual)), std::abs(double(expected)));
    return difference <= CKeyformMappingContext::AbsoluteTolerance + CKeyformMappingContext::RelativeTolerance * scale;
}

bool compare(const QVector<float>& actual, const QVector<float>& expected, QString* error)
{
    if (actual.size() != expected.size()) return fail(error, QStringLiteral("映射顶点数量不一致"));
    for (qsizetype index = 0; index < actual.size(); ++index) {
        if (!close(actual[index], expected[index]))
            return fail(error, QStringLiteral("顶点 %1 的 %2 坐标与父链映射不符：%3 / %4")
                        .arg(index / 2).arg(index % 2 ? QStringLiteral("Y") : QStringLiteral("X"))
                        .arg(actual[index], 0, 'g', 9).arg(expected[index], 0, 'g', 9));
    }
    if (error) error->clear();
    return true;
}

bool sameSlice(const std::vector<float>& pool, qint64 begin, const QVector<float>& values)
{
    return begin >= 0 && begin <= qint64(pool.size()) && values.size() <= qint64(pool.size()) - begin &&
           std::equal(values.cbegin(), values.cend(), pool.cbegin() + begin);
}

bool sameDeformer(const CSectionTable& table, const CDeformers& definition,
                  const CModelDeformer& source, const CModelDocument& document)
{
    const int specific = definition.specific_sources_indices;
    if (definition.parent_part_indices != source.parentPartIndex) return false;
    if (definition.types == 0) {
        if (source.type != CModelDeformer::Warp || specific < 0 || specific >= table.warp_deformers.size()) return false;
        const auto* warp = table.warp_deformers[specific];
        const auto forms = document.DeformerWarpKeyforms(source.id);
        const qint64 begin = warp->keyform_sources_begin_indices;
        if (warp->rows != source.rows || warp->columns != source.columns || warp->is_quad_source != source.quadTransform ||
            qint64(warp->vertex_counts) != (qint64(source.rows) + 1) * (qint64(source.columns) + 1) ||
            forms.size() != warp->keyform_sources_counts || begin < 0 ||
            begin > table.warp_deformer_keyforms.size() || forms.size() > table.warp_deformer_keyforms.size() - begin)
            return false;
        for (int index = 0; index < forms.size(); ++index) {
            const auto* form = table.warp_deformer_keyforms[begin + index];
            if (forms[index].opacity != form->opacities || forms[index].positions.size() != qint64(warp->vertex_counts) * 2 ||
                !sameSlice(table.keyform_positions->xys, form->keyform_position_sources_begin_indices, forms[index].positions))
                return false;
        }
        return true;
    }
    if (source.type != CModelDeformer::Rotation || specific < 0 || specific >= table.rotation_deformers.size()) return false;
    const auto* rotation = table.rotation_deformers[specific];
    const auto forms = document.DeformerRotationKeyforms(source.id);
    const qint64 begin = rotation->keyform_sources_begin_indices;
    if (source.baseAngle != rotation->base_angles || forms.size() != rotation->keyform_sources_counts || begin < 0 ||
        begin > table.rotation_deformer_keyforms.size() || forms.size() > table.rotation_deformer_keyforms.size() - begin)
        return false;
    for (int index = 0; index < forms.size(); ++index) {
        const auto* form = table.rotation_deformer_keyforms[begin + index];
        const CModelRotationKeyform actual{form->opacities, form->angles, form->origin_x, form->origin_y,
                                          form->scales, form->is_reflect_x, form->is_reflect_y};
        if (!(forms[index] == actual)) return false;
    }
    return true;
}

bool map(const C2DTreeNode& node, const QVector<float>& input, QVector<float>& output, bool inverse, QString* error)
{
    if (input.size() > 2000000 || input.size() % 2 != 0)
        return fail(error, QStringLiteral("映射坐标数量无效"));
    try {
        const std::vector<float> values(input.cbegin(), input.cend());
        std::vector<float> mapped;
        if (!(inverse ? node.MapFromCanvas(values, mapped) : node.MapToCanvas(values, mapped)))
            return fail(error, inverse ? QStringLiteral("父链无法唯一反解该坐标，可能存在退化或折叠")
                                       : QStringLiteral("父链正向映射失败或坐标超出范围"));
        QVector<float> candidate(mapped.cbegin(), mapped.cend());
        output.swap(candidate);
        if (error) error->clear();
        return true;
    } catch (const std::exception&) {
        return fail(error, QStringLiteral("映射坐标所需内存不足"));
    }
}
}

struct CKeyformMappingContext::State
{
    explicit State(const QByteArray& bytes) : reader(CReader::FromBytes(bytes)), section(reader) {}
    CReader reader;
    CSectionTable section;
    std::unique_ptr<C2DTree> tree;
    const CArtMeshNode* target = nullptr;
    QVector<float> expected;
};

bool CKeyformMappingContext::Build(const CModelDocument& document, const QString& artMeshId, int keyformIndex,
                                  const QMap<QString, float>& fullPose, QString* error)
{
    if (error) error->clear();
    if (!document.IsLoaded() || document.SourceBytes().isEmpty())
        return fail(error, QStringLiteral("关键形映射需要已加载的模型快照"));
    try {
        QMap<QString, float> corner;
        auto sourceReader = CReader::FromBytes(document.SourceBytes());
        CSectionTable sourceTable(sourceReader);
        {
            CKeyformBindingGrid sourceBindings;
            if (!sourceBindings.Build(sourceTable, error, true) ||
                !sourceBindings.Pose(CKeyformBindingGrid::ArtMesh, artMeshId, keyformIndex, corner, error)) return false;
        }
        QMap<QString, float> evaluationPose = fullPose;
        for (int index = 0; index < sourceTable.parameters.size(); ++index) {
            const auto* parameter = sourceTable.parameters[index];
            if (!parameter || parameter->parameter_types != 1 || parameter->blend_shape_parameter_binding_sources_counts <= 0 ||
                parameter->blend_shape_parameter_binding_sources_begin_indices < 0 ||
                parameter->blend_shape_parameter_binding_sources_begin_indices >= sourceTable.blend_shape_parameter_bindings.size()) continue;
            const auto* binding = sourceTable.blend_shape_parameter_bindings[
                parameter->blend_shape_parameter_binding_sources_begin_indices];
            if (!binding || binding->base_key_indices < 0 || binding->base_key_indices >= binding->keys_sources_counts ||
                binding->keys_sources_begin_indices < 0 ||
                binding->keys_sources_begin_indices + binding->base_key_indices >= sourceTable.keys->values.size()) continue;
            evaluationPose.insert(parameter->ids,
                                  sourceTable.keys->GetData(binding->keys_sources_begin_indices + binding->base_key_indices));
        }
        for (const auto& parameter : document.Parameters()) {
            if (parameter.type == 1) {
                corner.remove(parameter.id);
            }
        }
        if (evaluationPose.size() != document.Parameters().size())
            return fail(error, QStringLiteral("编辑姿态必须包含模型的全部参数"));
        QSet<QString> parameterIds;
        for (const auto& parameter : document.Parameters()) {
            const auto value = evaluationPose.constFind(parameter.id);
            const auto metadata = document.ParameterMetadata(parameter.id);
            if (parameter.id.isEmpty() || parameterIds.contains(parameter.id) || value == fullPose.cend() ||
                !std::isfinite(value.value()) || !std::isfinite(metadata.minimum) || !std::isfinite(metadata.maximum) ||
                value.value() < metadata.minimum || value.value() > metadata.maximum)
                return fail(error, QStringLiteral("编辑姿态参数缺失、重复或超出范围：%1").arg(parameter.id));
            parameterIds.insert(parameter.id);
        }
        for (auto value = corner.cbegin(); value != corner.cend(); ++value)
            if (evaluationPose.value(value.key()) != value.value())
                return fail(error, QStringLiteral("编辑姿态未处于关键形角点：%1").arg(value.key()));

        QByteArray previewBytes;
        if (!document.BuildPreviewMoc(previewBytes, error)) return false;
        auto candidate = std::make_shared<State>(previewBytes);
        if (!candidate->section.IsValid()) return fail(error, candidate->reader.Error());
        const auto& table = candidate->section;
        CKeyformBindingGrid bindings;
        if (!bindings.Build(table, error, true)) return false;
        CBlendShapePoseEvaluator::Overrides poseOverrides;
        CBlendShapePoseEvaluator evaluator;
        if (!evaluator.Build(table, evaluationPose, poseOverrides, error)) return false;
        QMap<QString, float> patchedCorner;
        if (!bindings.Pose(CKeyformBindingGrid::ArtMesh, artMeshId, keyformIndex, patchedCorner, error)) return false;
        for (const auto& parameter : document.Parameters()) if (parameter.type == 1) patchedCorner.remove(parameter.id);
        if (patchedCorner != corner)
            return fail(error, QStringLiteral("预览补丁改变了目标关键形的参数绑定"));
        int meshIndex = -1;
        for (int index = 0; index < table.art_meshes.size(); ++index) {
            if (table.art_meshes[index]->ids != artMeshId) continue;
            if (meshIndex >= 0) return fail(error, QStringLiteral("ArtMesh ID 重复：%1").arg(artMeshId));
            meshIndex = index;
        }
        if (meshIndex < 0) return fail(error, QStringLiteral("快照中没有 ArtMesh：%1").arg(artMeshId));
        if (meshIndex >= document.Drawables().size() || document.Drawables()[meshIndex].id != artMeshId ||
            document.Drawables()[meshIndex].parentDeformerIndex != table.art_meshes[meshIndex]->parent_deformer_indices ||
            document.Drawables()[meshIndex].vertexCount != table.art_meshes[meshIndex]->vertex_counts ||
            document.Drawables()[meshIndex].keyforms.size() != table.art_meshes[meshIndex]->keyform_sources_counts)
            return fail(error, QStringLiteral("目标网格结构与预览补丁不一致：%1").arg(artMeshId));
        for (const auto* glue : table.glue) {
            if (glue->art_mesh_indices_a < 0 || glue->art_mesh_indices_a >= table.art_meshes.size() ||
                glue->art_mesh_indices_b < 0 || glue->art_mesh_indices_b >= table.art_meshes.size())
                return fail(error, QStringLiteral("Glue 引用了无效的 ArtMesh"));
            if (glue->art_mesh_indices_a == meshIndex || glue->art_mesh_indices_b == meshIndex)
                return fail(error, QStringLiteral("ArtMesh 受 Glue 影响，尚不支持父链反解：%1").arg(artMeshId));
        }

        QHash<QString, int> deformerIdCounts;
        for (const auto* deformer : table.deformers) ++deformerIdCounts[deformer->ids];
        QSet<int> ancestors;
        int parent = table.art_meshes[meshIndex]->parent_deformer_indices;
        while (parent != -1) {
            if (parent < 0 || parent >= table.deformers.size() || ancestors.contains(parent))
                return fail(error, QStringLiteral("ArtMesh 父链缺失或形成循环：%1").arg(artMeshId));
            ancestors.insert(parent);
            const auto* deformer = table.deformers[parent];
            if (parent >= document.Deformers().size() || document.Deformers()[parent].id != deformer->ids)
                return fail(error, QStringLiteral("父变形器索引与文档不一致：%1").arg(deformer->ids));
            if (deformer->ids.isEmpty() || deformerIdCounts.value(deformer->ids) != 1)
                return fail(error, QStringLiteral("父变形器 ID 缺失或重复：%1").arg(deformer->ids));
            if (deformer->types != 0 && deformer->types != 1)
                return fail(error, QStringLiteral("父链含不支持的变形器类型：%1").arg(deformer->ids));
            const auto type = deformer->types == 0 ? CKeyformBindingGrid::Warp : CKeyformBindingGrid::Rotation;
            const QString diagnostic = bindings.Diagnostic(type, deformer->ids);
            if (!diagnostic.isEmpty()) return fail(error, QStringLiteral("父链 %1：%2").arg(deformer->ids, diagnostic));
            if (document.DeformerParentIndex(deformer->ids) != deformer->parent_deformer_indices)
                return fail(error, QStringLiteral("父级覆盖尚未同步至模型预览：%1").arg(deformer->ids));
            const auto source = std::find_if(document.Deformers().cbegin(), document.Deformers().cend(),
                [deformer](const auto& value) { return value.id == deformer->ids; });
            if (source == document.Deformers().cend())
                return fail(error, QStringLiteral("文档缺少父变形器：%1").arg(deformer->ids));
            if (!sameDeformer(table, *deformer, *source, document))
                return fail(error, QStringLiteral("父变形器数据与预览补丁不一致：%1").arg(deformer->ids));
            parent = deformer->parent_deformer_indices;
        }

        const QVector<float> local = document.DrawableKeyformPositions(artMeshId, keyformIndex);
        if (local.isEmpty() || local.size() != qint64(table.art_meshes[meshIndex]->vertex_counts) * 2)
            return fail(error, QStringLiteral("目标关键形顶点数量无效：%1").arg(artMeshId));
        const qint64 row = qint64(table.art_meshes[meshIndex]->keyform_sources_begin_indices) + keyformIndex;
        if (row < 0 || row >= table.art_mesh_keyforms.size() ||
            !sameSlice(table.keyform_positions->xys, table.art_mesh_keyforms[row]->keyform_position_sources_begin_indices, local))
            return fail(error, QStringLiteral("目标关键形坐标与预览补丁不一致：%1").arg(artMeshId));
        candidate->tree = std::make_unique<C2DTree>(table);
        candidate->tree->SetPoseOverrides(poseOverrides);
        QSet<QString> assigned;
        for (auto* node : candidate->tree->getParametersTree()) {
            auto* parameter = dynamic_cast<CParameterNode*>(node);
            if (!parameter || assigned.contains(parameter->getName())) continue;
            parameter->SetCurValue(evaluationPose.value(parameter->getName()));
            assigned.insert(parameter->getName());
        }
        candidate->tree->GetWarpKeys();
        for (auto* node : candidate->tree->getArtTree()) {
            if (node->getName() != artMeshId) continue;
            auto* mesh = dynamic_cast<CArtMeshNode*>(node);
            if (!mesh || mesh->GetCanvasPositions().empty())
                return fail(error, QStringLiteral("目标关键形的父链无法求值：%1").arg(artMeshId));
            if (!sameSlice(mesh->GetCurKeyForm().GetPosition(), 0, local) ||
                mesh->GetCurKeyForm().GetPosition().size() != size_t(local.size()))
                return fail(error, QStringLiteral("目标关键形的求值坐标与角点不一致：%1").arg(artMeshId));
            candidate->target = mesh;
        }
        if (!candidate->target || candidate->target->GetCanvasPositions().empty())
            return fail(error, QStringLiteral("目标关键形的父链无法求值：%1").arg(artMeshId));
        if (!map(*candidate->target, local, candidate->expected, false, error)) return false;
        QVector<float> restored, reprojected;
        if (!map(*candidate->target, candidate->expected, restored, true, error) ||
            !map(*candidate->target, restored, reprojected, false, error) ||
            !compare(reprojected, candidate->expected, error)) return false;
        for (qsizetype index = 1; index < candidate->expected.size(); index += 2)
            candidate->expected[index] = -candidate->expected[index];
        m_state = std::move(candidate);
        if (error) error->clear();
        return true;
    } catch (const std::exception&) {
        return fail(error, QStringLiteral("关键形映射上下文构建失败，原上下文保持不变"));
    }
}

bool CKeyformMappingContext::ToModel(const QVector<float>& local, QVector<float>& world, QString* error) const
{
    if (!m_state) return fail(error, QStringLiteral("关键形映射上下文尚未构建"));
    QVector<float> candidate;
    if (!map(*m_state->target, local, candidate, false, error)) return false;
    for (qsizetype index = 1; index < candidate.size(); index += 2) candidate[index] = -candidate[index];
    world.swap(candidate);
    return true;
}

bool CKeyformMappingContext::FromModel(const QVector<float>& world, QVector<float>& local, QString* error) const
{
    if (!m_state) return fail(error, QStringLiteral("关键形映射上下文尚未构建"));
    auto canvas = world;
    for (qsizetype index = 1; index < canvas.size(); index += 2) canvas[index] = -canvas[index];
    return map(*m_state->target, canvas, local, true, error);
}

bool CKeyformMappingContext::MatchesRuntime(const QVector<float>& actualModelVertices, QString* error) const
{
    return m_state ? compare(actualModelVertices, m_state->expected, error)
                   : fail(error, QStringLiteral("关键形映射上下文尚未构建"));
}
}
