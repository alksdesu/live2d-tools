/**
 * Copyright(c) Live2D Inc. All rights reserved.
 * Use of this source code is governed by the Live2D Open Software license: https://www.live2d.com/eula/live2d-open-software-license-agreement_en.html.
 */
#include "LAppModel.hpp"
#include <QHash>
#include <cmath>
#include <exception>

using namespace Live2D::Cubism::Framework;

namespace {
bool fail(QString* error, const QString& message) { if (error) *error = message; return false; }
bool finiteMap(const QMap<QString, float>& values)
{
    for (auto value = values.cbegin(); value != values.cend(); ++value)
        if (value.key().isEmpty() || !std::isfinite(value.value())) return false;
    return true;
}
bool sameDefinition(const CubismModel::RuntimeValueSnapshot& first, const CubismModel::RuntimeValueSnapshot& second)
{
    return first.parameterIds == second.parameterIds && first.parameterTypes == second.parameterTypes &&
           first.minimums == second.minimums && first.maximums == second.maximums && first.partIds == second.partIds;
}
void swapFrame(CMotionPreviewFrame& first, CMotionPreviewFrame& second) noexcept
{
    first.parameters.swap(second.parameters); first.model.swap(second.model); first.parts.swap(second.parts);
}
}

struct LAppModel::UserParameterContext::Data
{
    std::weak_ptr<const char> lifetime;
    const LAppModel* owner = nullptr;
    CubismModel* model = nullptr;
    const Live2D::Cubism::Core::csmModel* core = nullptr;
    quint64 generation = 0;
    QByteArray sourceDigest, previewDigest;
    CubismModel::RuntimeValueSnapshot current;
    EditorPreviewContext editor;
    CMotionPreviewFrame timeline;
    QSet<QString> explicitParameters;
    QMap<QString, float> timelineParameters, baseParts, baseParameters;
    float opacity = 1, baseOpacity = 1;
    bool timelineActive = false, snapshotMode = false, userSnapshotActive = false;
};

bool LAppModel::UserParameterContext::IsValid() const
{
    return m_data && !m_data->lifetime.expired() && m_data->owner &&
        m_data->model == m_data->owner->_model && m_data->core == m_data->owner->_model->GetModel() &&
        m_data->generation == m_data->owner->m_userParameterModelGeneration &&
        m_data->sourceDigest == m_data->owner->m_sourceMocDigest && m_data->previewDigest == m_data->owner->m_previewMocDigest;
}

bool LAppModel::UserParameterReady(QString* error) const
{
    if (!_initialized || _updating || !_model || HasExportPreview() || m_deformerDrag.IsActive() || m_physicsDragActive || m_sourceMocDigest.size() != 32 || m_previewMocDigest.size() != 32)
        return fail(error, QStringLiteral("当前模型状态不允许编辑用户参数"));
    return true;
}

bool LAppModel::CaptureUserParameterContext(UserParameterContext& result, QString* error) const
{
    if (error) error->clear();
    if (!UserParameterReady(error)) return false;
    try {
        auto candidate = std::make_shared<UserParameterContext::Data>();
        candidate->owner = this; candidate->lifetime = m_userParameterLifetime;
        candidate->model = _model; candidate->core = _model->GetModel(); candidate->generation = m_userParameterModelGeneration;
        candidate->sourceDigest = m_sourceMocDigest; candidate->previewDigest = m_previewMocDigest;
        if (!_model->CaptureRuntimeValues(candidate->current, error)) return false;
        candidate->editor.active = HasEditorPreview(); candidate->editor.sourceDigest = m_sourceMocDigest;
        candidate->editor.current = candidate->current;
        if (candidate->editor.active) {
            candidate->editor.entry = *m_editorEntryState;
            CubismModel::RuntimeValueSnapshot checked;
            if (!sameDefinition(candidate->editor.entry, candidate->current) ||
                !_model->RebaseRuntimeValues(candidate->editor.entry, checked, error))
                return fail(error, QStringLiteral("关键形编辑前的用户参数上下文与当前模型不匹配"));
        }
        candidate->editor.pose = m_editorPose; candidate->editor.base = m_editorBaseParameters;
        candidate->timeline = _timelinePreview; candidate->explicitParameters = m_timelineExplicitParameterIds;
        candidate->timelineParameters = _timelineParameters; candidate->baseParts = _timelineBaseParts;
        candidate->baseParameters = _timelineBaseParameters; candidate->timelineActive = _timelinePreviewActive;
        candidate->opacity = _opacity; candidate->baseOpacity = _timelineBaseOpacity; candidate->snapshotMode = _snapshotMode; candidate->userSnapshotActive = m_userParameterSnapshotActive;
        if (!candidate->timeline.IsFinite() || !std::isfinite(candidate->opacity) || !std::isfinite(candidate->baseOpacity) ||
            !finiteMap(candidate->editor.pose) || !finiteMap(candidate->editor.base) || !finiteMap(candidate->timelineParameters) ||
            !finiteMap(candidate->baseParts) || !finiteMap(candidate->baseParameters))
            return fail(error, QStringLiteral("参数预览上下文包含非有限值或空标识"));
        UserParameterContext context; context.m_data = std::move(candidate); result = std::move(context);
        return true;
    } catch (const std::exception&) { return fail(error, QStringLiteral("无法保存用户参数上下文，原结果保持不变")); }
}

bool LAppModel::CaptureUserParameterBaseValues(QMap<QString, float>& result, QString* error) const
{
    UserParameterContext context;
    if (!CaptureUserParameterContext(context, error)) return false;
    try {
        const auto& snapshot = context.m_data->editor.active ? context.m_data->editor.entry : context.m_data->current;
        QMap<QString, float> candidate;
        for (int index = 0; index < snapshot.parameterIds.size(); ++index)
            if (snapshot.HasSavedParameter(index)) candidate.insert(snapshot.parameterIds[index], snapshot.savedParameters[index]);
        result.swap(candidate); return true;
    } catch (const std::exception&) { return fail(error, QStringLiteral("无法读取已保存的用户参数，原结果保持不变")); }
}

bool LAppModel::ApplyUserParameterValues(const QMap<QString, float>& values, QString* error)
{
    if (error) error->clear();
    if (!UserParameterReady(error)) return false;
    if (values.isEmpty()) return true;
    try {
        UserParameterContext context;
        if (!CaptureUserParameterContext(context, error)) return false;
        const auto& state = *context.m_data;
        auto candidate = state.editor.active ? state.editor.entry : state.current;
        QHash<QString, int> parameters;
        for (int index = 0; index < candidate.parameterIds.size(); ++index) parameters.insert(candidate.parameterIds[index], index);
        for (auto value = values.cbegin(); value != values.cend(); ++value) {
            const auto index = parameters.constFind(value.key());
            if (index == parameters.cend() || !std::isfinite(value.value()) || value.value() < candidate.minimums[*index] || value.value() > candidate.maximums[*index])
                return fail(error, QStringLiteral("参数 %1 不存在、非有限或超出范围").arg(value.key()));
        }
        if (state.timelineActive) {
            for (int index = 0; index < candidate.parameterIds.size(); ++index)
                if (candidate.HasSavedParameter(index)) candidate.parameterValues[index] = candidate.savedParameters[index];
            QHash<QString, int> parts;
            for (int index = 0; index < candidate.partIds.size(); ++index) parts.insert(candidate.partIds[index], index);
            for (auto part = state.baseParts.cbegin(); part != state.baseParts.cend(); ++part) {
                const auto index = parts.constFind(part.key());
                if (index == parts.cend()) return fail(error, QStringLiteral("时间轴基础部件已不存在：%1").arg(part.key()));
                candidate.partOpacities[*index] = part.value();
            }
            candidate.modelOpacity = state.baseOpacity;
        }
        for (auto value = values.cbegin(); value != values.cend(); ++value) {
            const int index = parameters.value(value.key());
            candidate.parameterValues[index] = value.value();
            if (!candidate.SetSavedParameter(index, value.value())) return fail(error, QStringLiteral("无法准备参数保存值：%1").arg(value.key()));
        }
        if (!_model->RestoreRuntimeValues(candidate, error)) return false;
        if (state.editor.active || state.timelineActive) _opacity = candidate.modelOpacity;
        if (state.editor.active) { m_editorEntryState.reset(); m_editorPose.clear(); m_editorBaseParameters.clear(); }
        if (state.timelineActive) {
            _timelinePreviewActive = false;
            _timelinePreview.parameters.clear(); _timelinePreview.model.clear(); _timelinePreview.parts.clear();
            m_timelineExplicitParameterIds.clear(); _timelineParameters.clear(); _timelineBaseParts.clear(); _timelineBaseParameters.clear();
        }
        return true;
    } catch (const std::exception&) { return fail(error, QStringLiteral("批量参数修改失败，原用户上下文保持不变")); }
}

bool LAppModel::RestoreUserParameterContext(const UserParameterContext& context, QString* error)
{
    if (error) error->clear();
    if (!UserParameterReady(error)) return false;
    if (!context.IsValid() || context.m_data->owner != this)
        return fail(error, QStringLiteral("用户参数上下文属于已改变或不同的模型"));
    try {
        const auto& state = *context.m_data;
        std::unique_ptr<CubismModel::RuntimeValueSnapshot> entry;
        if (state.editor.active) entry = std::make_unique<CubismModel::RuntimeValueSnapshot>(state.editor.entry);
        auto pose = state.editor.pose, base = state.editor.base;
        auto timeline = state.timeline;
        auto explicitParameters = state.explicitParameters;
        auto timelineParameters = state.timelineParameters, baseParts = state.baseParts, baseParameters = state.baseParameters;
        if (!_model->RestoreRuntimeValues(state.current, error)) return false;
        m_editorEntryState.swap(entry); m_editorPose.swap(pose); m_editorBaseParameters.swap(base);
        swapFrame(_timelinePreview, timeline); m_timelineExplicitParameterIds.swap(explicitParameters);
        _timelineParameters.swap(timelineParameters); _timelineBaseParts.swap(baseParts); _timelineBaseParameters.swap(baseParameters);
        _timelinePreviewActive = state.timelineActive; _timelineBaseOpacity = state.baseOpacity; _opacity = state.opacity;
        _snapshotMode = state.snapshotMode; m_userParameterSnapshotActive = state.userSnapshotActive;
        return true;
    } catch (const std::exception&) { return fail(error, QStringLiteral("无法恢复用户参数上下文，当前状态保持不变")); }
}

bool LAppModel::BeginUserParameterPreview(QString* error)
{
    if (error) error->clear();
    if (!UserParameterReady(error)) return false;
    m_userParameterSnapshotActive = true; _snapshotMode = true;
    return true;
}

void LAppModel::EndUserParameterPreview(bool previousSnapshotMode) noexcept
{
    m_userParameterSnapshotActive = false; _snapshotMode = previousSnapshotMode;
}
