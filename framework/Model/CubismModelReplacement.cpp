#include "CubismModelReplacement.hpp"
#include "CubismMoc.hpp"
#include "CubismModel.hpp"
#include "framework/Rendering/CubismRenderer.hpp"

#include <QSet>
#include <algorithm>
#include <exception>
#include <cmath>
#include <limits>
#include <utility>

namespace Live2D { namespace Cubism { namespace Framework {
namespace {
bool fail(QString* error, const QString& message)
{
    if (error) *error = message;
    return false;
}

bool verifyGeometry(const CubismModel& model, const CMeshGeometryExpectation& expectation, QString* error)
{
    if (expectation.points.isEmpty() || expectation.points.size() > 1000000 ||
        !std::isfinite(expectation.tolerance) || expectation.tolerance <= 0)
        return fail(error, QStringLiteral("新顶点落点校验的数量或容差无效"));
    QHash<QString, int> drawables;
    for (int index = 0; index < model.GetDrawableCount(); ++index)
        drawables.insert(QString::fromUtf8(model.GetDrawableId(index)->GetString().GetRawString()), index);
    QHash<QString, QSet<int>> seen;
    for (const auto& point : expectation.points) {
        const auto found = drawables.constFind(point.artMeshId);
        if (found == drawables.cend() || point.vertexIndex < 0 ||
            point.vertexIndex >= model.GetDrawableVertexCount(*found) || seen[point.artMeshId].contains(point.vertexIndex) ||
            !std::isfinite(point.modelPosition.x()) || !std::isfinite(point.modelPosition.y()) ||
            std::abs(point.modelPosition.x()) > std::numeric_limits<float>::max() ||
            std::abs(point.modelPosition.y()) > std::numeric_limits<float>::max())
            return fail(error, QStringLiteral("新顶点落点校验的对象、索引或坐标无效"));
        seen[point.artMeshId].insert(point.vertexIndex);
        const auto* positions = model.GetDrawableVertexPositions(*found);
        if (!positions) return fail(error, QStringLiteral("Core 缺少新顶点坐标"));
        const auto& actual = positions[point.vertexIndex];
        const double distance = std::hypot(double(actual.X) - point.modelPosition.x(),
                                           double(actual.Y) - point.modelPosition.y());
        if (!std::isfinite(distance) || distance > expectation.tolerance)
            return fail(error, QStringLiteral("Core 新顶点未落在预期位置：%1[%2]，偏差 %3")
                        .arg(point.artMeshId).arg(point.vertexIndex).arg(distance, 0, 'g', 9));
    }
    return true;
}
}

CubismModelReplacement::~CubismModelReplacement()
{
    Rendering::CubismRenderer::Delete(_renderer);
    if (_moc && _model) _moc->DeleteModel(_model);
    CubismMoc::Delete(_moc);
}

bool CubismModelReplacement::Prepare(const csmByte* bytes, csmSizeInt size,
                                    const CubismMoc& sourceMoc, const CubismModel& source,
                                    const Rendering::CubismRenderer& renderer,
                                    const RendererPreparation& prepareRenderer, QString* error,
                                    const ModelPreparation& prepareModel)
{
    return Prepare(bytes, size, sourceMoc, source, renderer, prepareRenderer,
                   CubismModelUpdatePolicy::Strict, error, prepareModel);
}

bool CubismModelReplacement::Prepare(const csmByte* bytes, csmSizeInt size,
                                    const CubismMoc& sourceMoc, const CubismModel& source,
                                    const Rendering::CubismRenderer& renderer,
                                    const RendererPreparation& prepareRenderer, CubismModelUpdatePolicy policy,
                                    QString* error, const ModelPreparation& prepareModel)
{
    return Prepare(bytes, size, sourceMoc, source, renderer, prepareRenderer, policy, {}, error, prepareModel);
}

bool CubismModelReplacement::Prepare(const csmByte* bytes, csmSizeInt size,
                                    const CubismMoc& sourceMoc, const CubismModel& source,
                                    const Rendering::CubismRenderer& renderer,
                                    const RendererPreparation& prepareRenderer, CubismModelUpdatePolicy policy,
                                    const ParameterStructureSeeds& seeds, QString* error, const ModelPreparation& prepareModel,
                                    const CMeshGeometryMigration* geometry)
{
    if (error) error->clear();
    if (_moc || _model || _renderer || _prepared)
        return fail(error, QStringLiteral("预览候选已使用"));
    if (!bytes || !size || !prepareRenderer || renderer.IsDrawing() || renderer.GetModel() != &source)
        return fail(error, QStringLiteral("预览候选的源模型或渲染器无效"));
    if (AllowsModelUpdate(policy, CubismModelUpdatePolicy::AllowDrawableGeometryEdits) != (geometry != nullptr))
        return fail(error, QStringLiteral("几何预览需要明确的顶点身份迁移"));
    CubismModelReplacement candidate;
    try {
        candidate._moc = CubismMoc::Create(bytes, size, true);
        if (!candidate._moc) return fail(error, QStringLiteral("Cubism Core 未接受预览 MOC 数据"));
        candidate._model = candidate._moc->CreateModel();
        if (!candidate._model) return fail(error, QStringLiteral("无法创建预览模型"));
        if (candidate._model->GetDrawableCount() != source.GetDrawableCount() && !geometry)
            return fail(error, QStringLiteral("Drawable 数量变化必须提供显式几何迁移"));
        if (!candidate._model->CopyRuntimeStateFrom(source, policy, seeds, error)) return false;
        if (prepareModel && !prepareModel(candidate._model, error)) {
            if (error && error->isEmpty()) *error = QStringLiteral("预览模型姿态准备失败");
            return false;
        }
        candidate._model->Update();
        if (!candidate._model->HasFiniteDrawableState(error)) return false;
        if (geometry && geometry->expectation && !verifyGeometry(*candidate._model, *geometry->expectation, error)) return false;
        candidate._model->InitMesh();
        if (geometry) {
            const int sharedDrawables = std::min(source.GetDrawableCount(), candidate._model->GetDrawableCount());
            for (int index = 0; index < sharedDrawables; ++index) {
                const int count = candidate._model->GetDrawableVertexCount(index);
                bool changed = count != source.GetDrawableVertexCount(index);
                if (!changed) {
                    const auto* before = source.GetDrawableVertexUvs(index);
                    const auto* after = candidate._model->GetDrawableVertexUvs(index);
                    for (int vertex = 0; vertex < count && !changed; ++vertex)
                        changed = before[vertex].X != after[vertex].X || before[vertex].Y != after[vertex].Y;
                }
                const QString id = QString::fromUtf8(source.GetDrawableId(index)->GetString().GetRawString());
                if (changed && !geometry->remaps.contains(id))
                    return fail(error, QStringLiteral("几何改变缺少顶点身份映射：%1").arg(id));
            }
            if (!candidate._model->GetMeshMap()->CopyEditorStateFrom(*source.GetMeshMap(), *geometry, error)) return false;
        } else if (!candidate._model->GetMeshMap()->CopyEditorStateFrom(*source.GetMeshMap())) {
            return fail(error, QStringLiteral("预览网格无法保留当前选区"));
        }
        candidate._renderer = Rendering::CubismRenderer::Create();
        if (!candidate._renderer) return fail(error, QStringLiteral("无法创建预览渲染器"));
        if (!prepareRenderer(candidate._renderer, candidate._model, renderer, error)) {
            if (error && error->isEmpty()) *error = QStringLiteral("预览渲染资源准备失败");
            return false;
        }
        if (candidate._renderer->GetModel() != candidate._model)
            return fail(error, QStringLiteral("预览渲染器未绑定候选模型"));
        std::swap(_moc, candidate._moc);
        std::swap(_model, candidate._model);
        std::swap(_renderer, candidate._renderer);
        _sourceMoc = &sourceMoc;
        _sourceModel = &source;
        _sourceRenderer = &renderer;
        _prepared = true;
        return true;
    } catch (const std::exception&) {
        return fail(error, QStringLiteral("预览模型准备失败，原模型保持不变"));
    }
}

bool CubismModelReplacement::Commit(CubismMoc*& moc, CubismModel*& model,
                                   Rendering::CubismRenderer*& renderer) noexcept
{
    if (!_prepared || moc != _sourceMoc || model != _sourceModel || renderer != _sourceRenderer) return false;
    std::swap(moc, _moc);
    std::swap(model, _model);
    std::swap(renderer, _renderer);
    _prepared = false;
    return true;
}
}}}
