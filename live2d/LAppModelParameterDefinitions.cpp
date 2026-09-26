/**
 * Copyright(c) Live2D Inc. All rights reserved.
 * Use of this source code is governed by the Live2D Open Software license: https://www.live2d.com/eula/live2d-open-software-license-agreement_en.html.
 */
#include "LAppModel.hpp"
#include <cmath>
#include <exception>
#include <limits>

using namespace Live2D::Cubism::Framework;

bool LAppModel::ApplyParameterDefinitionPreview(const QByteArray& bytes, const QByteArray& sourceDigest,
                                                const QMap<QString, float>& baseValues, bool keepEditorPreview,
                                                QString* error, const ParameterDefinitionPreparation& preparePreview)
{
    if (error) error->clear();
    if (HasExportPreview() || !_initialized || _updating || !_model || sourceDigest.isEmpty() ||
        sourceDigest != m_sourceMocDigest || bytes.isEmpty() || bytes.size() > std::numeric_limits<int>::max()) {
        if (error) *error = QStringLiteral("当前模型状态不允许修改参数定义");
        return false;
    }
    try {
        const QByteArray ownedBytes(bytes.constData(), bytes.size());
        CubismModel::RuntimeValueSnapshot sourceState;
        if (!_model->CaptureRuntimeValues(sourceState, error)) return false;
        const bool editing = HasEditorPreview();
        auto nextEditorPose = m_editorPose;
        auto nextEditorBase = m_editorBaseParameters;
        auto nextTimelineBase = _timelineBaseParameters;
        std::unique_ptr<CubismModel::RuntimeValueSnapshot> nextEntry;
        const auto prepare = [&](CubismModel* candidate, QString* detail) {
            const auto reject = [&](const QString& reason) { if (detail) *detail = reason; return false; };
            CubismModel::RuntimeValueSnapshot values;
            if (!candidate->CaptureRuntimeValues(values, detail)) return false;
            QHash<QString, int> indices;
            for (int index = 0; index < values.parameterIds.size(); ++index) indices.insert(values.parameterIds[index], index);
            const auto bound = [&](int index, float value) {
                if (value < values.minimums[index]) return values.minimums[index];
                if (value > values.maximums[index]) return values.maximums[index];
                return value;
            };
            const auto migrate = [&](QMap<QString, float>& map) {
                for (auto it = map.begin(); it != map.end(); ++it) {
                    const auto index = indices.constFind(it.key());
                    if (index == indices.cend() || !std::isfinite(it.value())) return false;
                    if (sourceState.minimums[*index] != values.minimums[*index] ||
                        sourceState.maximums[*index] != values.maximums[*index]) it.value() = bound(*index, it.value());
                }
                return true;
            };
            if (!migrate(nextEditorPose) || !migrate(nextEditorBase) || !migrate(nextTimelineBase))
                return reject(QStringLiteral("参数预览基线含未知参数或非有限值"));
            for (auto it = baseValues.cbegin(); it != baseValues.cend(); ++it) {
                const auto index = indices.constFind(it.key());
                if (index == indices.cend() || !std::isfinite(it.value()) || it.value() < values.minimums[*index] ||
                    it.value() > values.maximums[*index] || !values.HasSavedParameter(*index))
                    return reject(QStringLiteral("参数基线 %1 缺失、未初始化或超出新范围").arg(it.key()));
                values.savedParameters[*index] = it.value();
                if (!editing && !_timelinePreviewActive) values.parameterValues[*index] = it.value();
                if (_timelinePreviewActive) nextTimelineBase[it.key()] = it.value();
                if (editing && !_timelinePreviewActive) nextEditorBase[it.key()] = it.value();
            }
            if (editing) {
                nextEntry = std::make_unique<CubismModel::RuntimeValueSnapshot>();
                if (!candidate->RebaseRuntimeValues(*m_editorEntryState, *nextEntry, detail)) return false;
                for (auto it = baseValues.cbegin(); it != baseValues.cend(); ++it) {
                    const int index = indices.value(it.key());
                    if (!nextEntry->HasSavedParameter(index))
                        return reject(QStringLiteral("编辑前参数基线尚未初始化：%1").arg(it.key()));
                    if (!_timelinePreviewActive) nextEntry->parameterValues[index] = it.value();
                    nextEntry->savedParameters[index] = it.value();
                }
                if (!keepEditorPreview) values = *nextEntry;
            }
            if (!candidate->RestoreRuntimeValues(values, detail)) return false;
            if (editing && keepEditorPreview && !candidate->ApplyParameterPose(nextEditorPose, detail)) return false;
            return !preparePreview || preparePreview(*candidate, detail);
        };
        const auto policy = CubismModelUpdatePolicy::AllowClippingEdits | CubismModelUpdatePolicy::AllowParameterDefinitionEdits;
        if (!ApplyPreviewTransaction(ownedBytes, sourceDigest, prepare, error, policy)) return false;
        _timelineBaseParameters.swap(nextTimelineBase);
        if (editing && keepEditorPreview) {
            m_editorEntryState.swap(nextEntry);
            m_editorPose.swap(nextEditorPose);
            m_editorBaseParameters.swap(nextEditorBase);
        } else if (editing) {
            m_editorEntryState.reset();
            m_editorPose.clear();
            m_editorBaseParameters.clear();
        }
        return true;
    } catch (const std::exception&) {
        if (error) *error = QStringLiteral("参数定义修改失败，原模型与预览基线保持不变");
        return false;
    }
}
