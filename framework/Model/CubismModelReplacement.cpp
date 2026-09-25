#include "CubismModelReplacement.hpp"
#include "CubismMoc.hpp"
#include "CubismModel.hpp"
#include "framework/Rendering/CubismRenderer.hpp"

#include <exception>
#include <utility>

namespace Live2D { namespace Cubism { namespace Framework {
namespace {
bool fail(QString* error, const QString& message)
{
    if (error) *error = message;
    return false;
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
    if (error) error->clear();
    if (_moc || _model || _renderer || _prepared)
        return fail(error, QStringLiteral("预览候选已使用"));
    if (!bytes || !size || !prepareRenderer || renderer.IsDrawing() || renderer.GetModel() != &source)
        return fail(error, QStringLiteral("预览候选的源模型或渲染器无效"));
    CubismModelReplacement candidate;
    try {
        candidate._moc = CubismMoc::Create(bytes, size, true);
        if (!candidate._moc) return fail(error, QStringLiteral("Cubism Core 未接受预览 MOC 数据"));
        candidate._model = candidate._moc->CreateModel();
        if (!candidate._model) return fail(error, QStringLiteral("无法创建预览模型"));
        if (!candidate._model->CopyRuntimeStateFrom(source, error)) return false;
        if (prepareModel && !prepareModel(candidate._model, error)) {
            if (error && error->isEmpty()) *error = QStringLiteral("预览模型姿态准备失败");
            return false;
        }
        candidate._model->Update();
        if (!candidate._model->HasFiniteDrawableState(error)) return false;
        candidate._model->InitMesh();
        if (!candidate._model->GetMeshMap()->CopyEditorStateFrom(*source.GetMeshMap()))
            return fail(error, QStringLiteral("预览网格无法保留当前选区"));
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
