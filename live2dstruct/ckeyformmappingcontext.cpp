#include "ckeyformmappingcontext.h"
#include "cmodeldocument.h"
#include "ckeyformbindinggrid.h"
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
        if (!document.KeyformParameterValues(artMeshId, QStringLiteral("artmesh"), keyformIndex, corner, error)) return false;
        if (fullPose.size() != document.Parameters().size())
            return fail(error, QStringLiteral("编辑姿态必须包含模型的全部参数"));
        QSet<QString> parameterIds;
        for (const auto& parameter : document.Parameters()) {
            const auto value = fullPose.constFind(parameter.id);
            const auto metadata = document.ParameterMetadata(parameter.id);
            if (parameter.id.isEmpty() || parameterIds.contains(parameter.id) || value == fullPose.cend() ||
                !std::isfinite(value.value()) || !std::isfinite(parameter.minimum) || !std::isfinite(parameter.maximum) ||
                value.value() < parameter.minimum || value.value() > parameter.maximum ||
                value.value() < metadata.minimum || value.value() > metadata.maximum)
                return fail(error, QStringLiteral("编辑姿态参数缺失、重复或超出范围：%1").arg(parameter.id));
            parameterIds.insert(parameter.id);
        }
        for (auto value = corner.cbegin(); value != corner.cend(); ++value)
            if (fullPose.value(value.key()) != value.value())
                return fail(error, QStringLiteral("编辑姿态未处于关键形角点：%1").arg(value.key()));

        auto candidate = std::make_shared<State>(document.SourceBytes());
        if (!candidate->section.IsValid()) return fail(error, candidate->reader.Error());
        const auto& table = candidate->section;
        CKeyformBindingGrid bindings;
        if (!bindings.Build(table, error)) return false;
        int meshIndex = -1;
        for (int index = 0; index < table.art_meshes.size(); ++index) {
            if (table.art_meshes[index]->ids != artMeshId) continue;
            if (meshIndex >= 0) return fail(error, QStringLiteral("ArtMesh ID 重复：%1").arg(artMeshId));
            meshIndex = index;
        }
        if (meshIndex < 0) return fail(error, QStringLiteral("快照中没有 ArtMesh：%1").arg(artMeshId));
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
            if ((deformer->types == 0 && document.DeformerWarpKeyforms(deformer->ids) != source->warpKeyforms) ||
                (deformer->types == 1 && document.DeformerRotationKeyforms(deformer->ids) != source->rotationKeyforms))
                return fail(error, QStringLiteral("父变形器关键形覆盖尚未同步至模型预览：%1").arg(deformer->ids));
            parent = deformer->parent_deformer_indices;
        }

        const QVector<float> local = document.DrawableKeyformPositions(artMeshId, keyformIndex);
        if (local.isEmpty() || local.size() != qint64(table.art_meshes[meshIndex]->vertex_counts) * 2)
            return fail(error, QStringLiteral("目标关键形顶点数量无效：%1").arg(artMeshId));
        candidate->tree = std::make_unique<C2DTree>(table);
        QSet<QString> assigned;
        for (auto* node : candidate->tree->getParametersTree()) {
            auto* parameter = dynamic_cast<CParameterNode*>(node);
            if (!parameter || assigned.contains(parameter->getName())) continue;
            parameter->SetCurValue(fullPose.value(parameter->getName()));
            assigned.insert(parameter->getName());
        }
        candidate->tree->GetWarpKeys();
        for (auto* node : candidate->tree->getArtTree()) {
            if (node->getName() == artMeshId) candidate->target = dynamic_cast<const CArtMeshNode*>(node);
        }
        if (!candidate->target || candidate->target->GetCanvasPositions().empty())
            return fail(error, QStringLiteral("目标关键形的父链无法求值：%1").arg(artMeshId));
        if (!map(*candidate->target, local, candidate->expected, false, error)) return false;
        QVector<float> restored;
        if (!map(*candidate->target, candidate->expected, restored, true, error) || !compare(restored, local, error)) return false;
        m_state = std::move(candidate);
        if (error) error->clear();
        return true;
    } catch (const std::exception&) {
        return fail(error, QStringLiteral("关键形映射上下文构建失败，原上下文保持不变"));
    }
}

bool CKeyformMappingContext::ToModel(const QVector<float>& local, QVector<float>& world, QString* error) const
{
    return m_state ? map(*m_state->target, local, world, false, error)
                   : fail(error, QStringLiteral("关键形映射上下文尚未构建"));
}

bool CKeyformMappingContext::FromModel(const QVector<float>& world, QVector<float>& local, QString* error) const
{
    return m_state ? map(*m_state->target, world, local, true, error)
                   : fail(error, QStringLiteral("关键形映射上下文尚未构建"));
}

bool CKeyformMappingContext::MatchesRuntime(const QVector<float>& actualModelVertices, QString* error) const
{
    return m_state ? compare(actualModelVertices, m_state->expected, error)
                   : fail(error, QStringLiteral("关键形映射上下文尚未构建"));
}
}
