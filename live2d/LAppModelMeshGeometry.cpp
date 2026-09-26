#include "LAppModel.hpp"
#include <exception>
#include <limits>

using namespace Live2D::Cubism::Framework;

bool LAppModel::CaptureMeshEditorState(CMeshEditorSnapshot& state, QString* error) const
{
    if (error) error->clear();
    if (!_initialized || _updating || !_model) {
        if (error) *error = QStringLiteral("模型尚未就绪，无法保存网格选区");
        return false;
    }
    return _model->GetMeshMap()->CaptureEditorState(state, error);
}

bool LAppModel::ApplyMeshGeometryPreview(const QByteArray& bytes, const QByteArray& sourceDigest,
                                        const CMeshGeometryMigration& migration, QString* error,
                                        const ParameterDefinitionPreparation& preparePreview)
{
    if (error) error->clear();
    if (!_initialized || _updating || m_physicsDragActive || !_model || HasExportPreview() ||
        sourceDigest.isEmpty() || sourceDigest != m_sourceMocDigest ||
        migration.sourcePreviewDigest.size() != 32 || migration.sourcePreviewDigest != m_previewMocDigest ||
        bytes.isEmpty() || bytes.size() > std::numeric_limits<int>::max()) {
        if (error) *error = QStringLiteral("网格几何与当前预览版本不一致，或画布仍在交互中");
        return false;
    }
    try {
        const QByteArray ownedBytes(bytes.constData(), bytes.size());
        const auto prepare = [&](CubismModel* candidate, QString* detail) {
            if (HasEditorPreview()) {
                if (!candidate->ApplyParameterPose(m_editorPose, detail)) return false;
            }
            return !preparePreview || preparePreview(*candidate, detail);
        };
        return ApplyPreviewTransaction(ownedBytes, sourceDigest, prepare, error,
            CubismModelUpdatePolicy::AllowDrawableGeometryEdits, {}, nullptr, nullptr, &migration);
    } catch (const std::exception&) {
        if (error) *error = QStringLiteral("无法准备网格几何预览，原模型保持不变");
        return false;
    }
}
