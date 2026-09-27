#include "LAppModel.hpp"
#include "LAppDelegate.hpp"
#include "framework/Rendering/OpenGL/CubismRenderer_OpenGLES2.hpp"
#include "live2dstruct/cmocdeformerorder.h"
#include "live2dstruct/cmocmaterialbuilder.h"
#include <QScopedValueRollback>
#include <cstring>
#include <exception>
#include <limits>

using namespace Live2D::Cubism::Framework;

bool LAppModel::ValidateMaterialCandidate(const CubismModel& candidate, const QSet<QString>& allowedDrawableIds,
                                          bool geometryChanges, QString* error,
                                          const QMap<int, csmUint32>* candidateBindings)
{
    const auto fail = [&](const QString& reason) { if (error) *error = reason; return false; };
    const auto* renderer = GetRenderer<Rendering::CubismRenderer_OpenGLES2>();
    if (!_model || !renderer || candidate.GetDrawableCount() != _model->GetDrawableCount())
        return fail(QStringLiteral("材质预览的源模型或纹理绑定无效"));
    auto* delegate = LAppDelegate::GetInstance();
    if (!delegate->getGlOwningContext() || QOpenGLContext::currentContext() != delegate->getGlOwningContext())
        return fail(QStringLiteral("材质验证需要当前模型所属 OpenGL 上下文"));
    QSet<QString> remaining = allowedDrawableIds;
    const auto& bindings = renderer->GetBindedTextures();
    QSet<int> available;
    for (auto it = bindings.Begin(); it != bindings.End(); ++it) {
        if (it->First < 0 || (it->Second && !delegate->glIsTexture(it->Second)))
            return fail(QStringLiteral("原渲染器纹理绑定无效"));
        if (it->Second && m_textureBindings.value(it->First) == it->Second && m_textureWorkspace.atlases.contains(it->First))
            available.insert(it->First);
    }
    if (available.size() != m_textureBindings.size() || available.size() != m_textureWorkspace.atlases.size())
        return fail(QStringLiteral("当前图集目录与渲染器绑定不一致"));
    if (candidateBindings) {
        available.clear();
        for (auto it = candidateBindings->cbegin(); it != candidateBindings->cend(); ++it) {
            if (it.key() < 0 || !it.value() || !delegate->glIsTexture(it.value()))
                return fail(QStringLiteral("候选图集纹理绑定无效"));
            available.insert(it.key());
        }
    }
    for (int index = 0; index < candidate.GetDrawableCount(); ++index) {
        const QString id = QString::fromUtf8(candidate.GetDrawableId(index)->GetString().GetRawString());
        remaining.remove(id);
        const int slot = candidate.GetDrawableTextureIndex(index);
        const bool slotChanged = slot != _model->GetDrawableTextureIndex(index);
        if ((candidate.GetDrawableVertexCount(index) || slotChanged) &&
            !available.contains(slot))
            return fail(QStringLiteral("网格 %1 的纹理槽 %2 没有可用的图集").arg(id).arg(slot));
        if (slotChanged && !allowedDrawableIds.contains(id))
            return fail(QStringLiteral("材质预览包含未授权的纹理槽修改：%1").arg(id));
        if (geometryChanges || allowedDrawableIds.contains(id)) continue;
        const int count = candidate.GetDrawableVertexCount(index);
        if (count != _model->GetDrawableVertexCount(index))
            return fail(QStringLiteral("材质预览改变了网格顶点数量：%1").arg(id));
        const auto* before = _model->GetDrawableVertexUvs(index);
        const auto* after = candidate.GetDrawableVertexUvs(index);
        if (count && (!before || !after)) return fail(QStringLiteral("材质预览缺少 UV：%1").arg(id));
        for (int vertex = 0; vertex < count; ++vertex)
            if (std::memcmp(&before[vertex].X, &after[vertex].X, sizeof(float)) ||
                std::memcmp(&before[vertex].Y, &after[vertex].Y, sizeof(float)))
                return fail(QStringLiteral("材质预览包含未授权的 UV 修改：%1").arg(id));
    }
    return remaining.isEmpty() || fail(QStringLiteral("材质预览引用了不存在的网格"));
}

bool LAppModel::ApplyDrawableMaterialPreview(const QByteArray& bytes, const QByteArray& sourceDigest,
                                             const QByteArray& expectedPreviewDigest, const QSet<QString>& allowedDrawableIds,
                                             QString* error, const ParameterDefinitionPreparation& preparePreview)
{
    if (error) error->clear();
    if (!_initialized || _updating || !_model || HasExportPreview() || m_physicsDragActive ||
        m_userParameterSnapshotActive || m_deformerDrag.IsActive() || sourceDigest.isEmpty() ||
        sourceDigest != m_sourceMocDigest || expectedPreviewDigest.size() != 32 ||
        expectedPreviewDigest != m_previewMocDigest || bytes.isEmpty() || bytes.size() > std::numeric_limits<int>::max()) {
        if (error) *error = QStringLiteral("材质预览与当前模型版本不一致，或画布仍在交互中");
        return false;
    }
    auto* sourceRenderer = GetRenderer<Rendering::CubismRenderer_OpenGLES2>();
    if (!sourceRenderer || sourceRenderer->IsDrawing() || sourceRenderer->HasPsdExport()) {
        if (error) *error = QStringLiteral("请先结束当前绘制或 PSD 导出，再更新材质预览");
        return false;
    }
    try {
        QByteArray compiled;
        if (!live2dstruct::CMocDeformerOrder::Normalize(bytes, compiled, error) ||
            !live2dstruct::CMocMaterialBuilder::ValidateTransition(m_previewMocBytes, compiled, allowedDrawableIds, error)) return false;
        if (compiled == m_previewMocBytes) {
            auto* renderer = GetRenderer<Rendering::CubismRenderer_OpenGLES2>();
            auto* delegate = LAppDelegate::GetInstance();
            if (!renderer || delegate->IsOffScreen() || delegate->m_isDraggingDeformer ||
                !renderer->setGlOwningContext(delegate->getGlOwningContext()) ||
                !ValidateMaterialCandidate(*_model, allowedDrawableIds, false, error)) {
                if (error && error->isEmpty()) *error = QStringLiteral("材质预览尚未就绪");
                return false;
            }
            CubismModel::RuntimeValueSnapshot values;
            if (!_model->CaptureRuntimeValues(values, error) || !_model->HasFiniteDrawableState(error)) return false;
            const QScopedValueRollback<csmBool> updating(_updating, true);
            return !preparePreview || preparePreview(*_model, error);
        }
        const auto prepare = [&](CubismModel* candidate, QString* detail) {
            return !preparePreview || preparePreview(*candidate, detail);
        };
        return ApplyPreviewTransaction(compiled, sourceDigest, prepare, error,
            CubismModelUpdatePolicy::AllowDrawableMaterialEdits, {}, nullptr, nullptr, nullptr, nullptr, &allowedDrawableIds);
    } catch (const std::exception&) {
        if (error) *error = QStringLiteral("无法准备材质预览，原模型保持不变");
        return false;
    }
}

bool LAppModel::ApplyAtlasPreview(const QByteArray& bytes, const QByteArray& sourceDigest,
                                 const QByteArray& expectedPreviewDigest, const QByteArray& expectedWorkspaceDigest,
                                 const AtlasState& workspace, const QSet<QString>& allowedDrawableIds,
                                 QString* error, const ParameterDefinitionPreparation& preparePreview)
{
    if (error) error->clear();
    if (!_initialized || _updating || !_model || HasExportPreview() || m_physicsDragActive ||
        m_userParameterSnapshotActive || m_deformerDrag.IsActive() || sourceDigest.isEmpty() ||
        sourceDigest != m_sourceMocDigest || expectedPreviewDigest.size() != 32 ||
        expectedPreviewDigest != m_previewMocDigest || expectedWorkspaceDigest.size() != 32 ||
        bytes.isEmpty() || bytes.size() > std::numeric_limits<int>::max()) {
        if (error) *error = QStringLiteral("图集预览与当前模型版本不一致，或画布仍在交互中");
        return false;
    }
    auto* renderer = GetRenderer<Rendering::CubismRenderer_OpenGLES2>();
    auto* delegate = LAppDelegate::GetInstance();
    if (!renderer || renderer->IsDrawing() || renderer->HasPsdExport() || delegate->IsOffScreen() ||
        delegate->m_isDraggingDeformer || !renderer->setGlOwningContext(delegate->getGlOwningContext())) {
        if (error) *error = QStringLiteral("请先结束当前绘制或导出，再更新图集预览");
        return false;
    }
    try {
        QByteArray fingerprint, compiled;
        if (!live2dstruct::CTextureWorkspace::Fingerprint(m_textureSources, m_textureWorkspace, fingerprint, error)) return false;
        if (fingerprint != expectedWorkspaceDigest) {
            if (error) *error = QStringLiteral("图集内容已改变，请重新打开图集编辑");
            return false;
        }
        if (!live2dstruct::CTextureWorkspace::ValidateTransition(m_textureSources, m_textureWorkspace, workspace,
                live2dstruct::CTextureWorkspace::Transition::ReplaceGenerated, error) ||
            !live2dstruct::CMocDeformerOrder::Normalize(bytes, compiled, error) ||
            !live2dstruct::CMocMaterialBuilder::ValidateTransition(m_previewMocBytes, compiled, allowedDrawableIds, error)) return false;
        CubismModel::RuntimeValueSnapshot values;
        if (!_model->CaptureRuntimeValues(values, error) || !_model->HasFiniteDrawableState(error)) return false;
        if (compiled == m_previewMocBytes && workspace == m_textureWorkspace) {
            if (!ValidateMaterialCandidate(*_model, allowedDrawableIds, false, error)) return false;
            const QScopedValueRollback<csmBool> updating(_updating, true);
            return !preparePreview || preparePreview(*_model, error);
        }
        const auto prepare = [&](CubismModel* candidate, QString* detail) {
            return !preparePreview || preparePreview(*candidate, detail);
        };
        return ApplyPreviewTransaction(compiled, sourceDigest, prepare, error,
            CubismModelUpdatePolicy::AllowDrawableMaterialEdits, {}, nullptr, nullptr, nullptr, nullptr,
            &allowedDrawableIds, &workspace);
    } catch (const std::exception&) {
        if (error) *error = QStringLiteral("无法准备图集预览，原模型保持不变");
        return false;
    }
}
