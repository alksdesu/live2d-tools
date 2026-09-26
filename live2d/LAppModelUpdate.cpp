/**
 * Copyright(c) Live2D Inc. All rights reserved.
 * Use of this source code is governed by the Live2D Open Software license that can be found at https://www.live2d.com/eula/live2d-open-software-license-agreement_en.html.
 */

#include "LAppModel.hpp"
#include "LAppPal.hpp"
#include "live2dstruct/cmotionblendpreview.h"
#include <framework/Physics/CubismPhysics.hpp>
#include <cmath>
#include <exception>
#include <memory>

using namespace Live2D::Cubism::Framework;

bool LAppModel::BeginExportPreview(bool savedParameterSnapshot, QString* error)
{
    if (error) error->clear();
    if (HasExportPreview() || !_initialized || _updating || !_model || !std::isfinite(_opacity)) {
        if (error) *error = QStringLiteral("模型尚未就绪或已有导出姿态等待恢复");
        return false;
    }
    try {
        auto entry = std::make_unique<CubismModel::RuntimeValueSnapshot>();
        if (!_model->CaptureRuntimeValues(*entry, error)) return false;
        m_exportModel = _model;
        m_exportSavedParameters = savedParameterSnapshot;
        m_exportPreviousOpacity = _opacity;
        m_exportPreviousSnapshotMode = _snapshotMode;
        m_exportEntryState = std::move(entry);
        return true;
    } catch (const std::exception&) {
        if (error) *error = QStringLiteral("无法保存导出前的模型姿态");
        return false;
    }
}

bool LAppModel::EndExportPreview(QString* error)
{
    if (error) error->clear();
    if (!HasExportPreview()) return true;
    if (!_initialized || _updating || !_model || _model != m_exportModel) {
        if (error) *error = QStringLiteral("当前模型与导出姿态的原模型不一致");
        return false;
    }
    if (!_model->RestoreRuntimeValues(*m_exportEntryState, error)) return false;
    _opacity = m_exportPreviousOpacity;
    _snapshotMode = m_exportPreviousSnapshotMode;
    _model->Update();
    m_exportEntryState.reset();
    m_exportModel = nullptr;
    return true;
}

void LAppModel::Update()
{
    if (!_initialized || _updating || !_model) return;
    if (HasExportPreview()) {
        if (_model != m_exportModel) return;
        if (m_exportSavedParameters && !HasEditorPreview()) {
            _model->LoadParameters();
            ApplyTimelinePreview();
            ApplyPartVisibility();
        }
        _opacity = _model->GetModelOpacity();
        _model->Update();
        return;
    }
    if (HasEditorPreview()) {
        if (!_model->ApplyParameterPose(m_editorPose)) return;
        ApplyPartVisibility();
        _model->Update();
        return;
    }
    // 快照模式：只应用已保存的参数，跳过所有自动效果
    if (_snapshotMode) {
        _model->LoadParameters();
        ApplyTimelinePreview();
        ApplyPartVisibility();
        _opacity = _model->GetModelOpacity();
        _model->Update();
        return;
    }

    const csmFloat32 deltaTimeSeconds = LAppPal::GetDeltaTime();
    _userTimeSeconds += deltaTimeSeconds;

    _dragManager->Update(deltaTimeSeconds);
    _dragX = _dragManager->GetX();
    _dragY = _dragManager->GetY();

    csmBool motionUpdated = false;
    _model->LoadParameters();

    // 动作更新（不自动播放随机动作，由 UI 触发）
    if (!_timelinePreviewActive && !_motionManager->IsFinished())
    {
        motionUpdated = _motionManager->UpdateMotion(_model, deltaTimeSeconds * _motionSpeed);
    }

    _model->SaveParameters();

    if (_lipSyncIds.GetSize() > 0)
    {
        _wavFileHandler.Update(deltaTimeSeconds);
        const csmFloat32 value = _wavFileHandler.GetRms();
        for (csmUint32 i = 0; i < _lipSyncIds.GetSize(); ++i)
        {
            if (i < m_suppressedLipSync.size() && m_suppressedLipSync[i]) continue;
            _model->AddParameterValue(_lipSyncIds[i], value, 0.8f);
        }
    }

    if (!motionUpdated)
    {
        if (_eyeBlink != NULL && _autoEyeBlink)
        {
            _eyeBlink->UpdateParameters(_model, deltaTimeSeconds);
        }
    }

    // 表情
    if (_expressionManager != NULL && !_timelinePreviewActive)
    {
        _expressionManager->UpdateMotion(_model, deltaTimeSeconds);
    }

    // 鼠标拖拽 → 头部跟随
    if (_enableMouseFollow)
    {
        if (0 >= m_suppressedMouseFollow.size() || !m_suppressedMouseFollow[0]) _model->AddParameterValue(_idParamAngleX, _dragX * 30);
        if (1 >= m_suppressedMouseFollow.size() || !m_suppressedMouseFollow[1]) _model->AddParameterValue(_idParamAngleY, _dragY * 30);
        if (2 >= m_suppressedMouseFollow.size() || !m_suppressedMouseFollow[2]) _model->AddParameterValue(_idParamAngleZ, _dragX * _dragY * -30);
        if (3 >= m_suppressedMouseFollow.size() || !m_suppressedMouseFollow[3]) _model->AddParameterValue(_idParamBodyAngleX, _dragX * 10);
        if (4 >= m_suppressedMouseFollow.size() || !m_suppressedMouseFollow[4]) _model->AddParameterValue(_idParamEyeBallX, _dragX);
        if (5 >= m_suppressedMouseFollow.size() || !m_suppressedMouseFollow[5]) _model->AddParameterValue(_idParamEyeBallY, _dragY);
    }

    // 呼吸
    if (_breath != NULL && _autoBreath)
    {
       _breath->UpdateParameters(_model, deltaTimeSeconds);
    }

    ApplyTimelinePreview();
    if (_physics != NULL && _autoPhysics)
    {
        _physics->Evaluate(_model, deltaTimeSeconds);
    }

    // Pose
    if (_pose != NULL)
    {
        _pose->UpdateParameters(_model, deltaTimeSeconds);
    }

    ApplyTimelinePreview();
    ApplyPartVisibility();
    _opacity = _model->GetModelOpacity();

    // 变形器拖拽参数覆盖（在所有效果之后、Update 之前强制设置）
    if(m_deformerDrag.IsActive()){
        const auto& ids = m_deformerDrag.Ids();
        const auto& values = m_deformerDrag.Values();
        for(qsizetype i = 0; i < ids.size(); i++){
            const auto index = m_mapParaToIndex.constFind(ids[i]);
            if (index == m_mapParaToIndex.cend()) continue;
            _model->SetParameterValue(index.value(), values[i]);
        }
    }

    _model->Update();

}

int LAppModel::FindPartIndex(const QString& id) const
{
    if (!_model) return -1;
    for (int index = 0; index < _model->GetPartCount(); ++index)
        if (id == QString::fromUtf8(_model->GetPartId(index)->GetString().GetRawString())) return index;
    return -1;
}

QMap<QString, float> LAppModel::GetParameterValues() const
{
    QMap<QString, float> values;
    if (_model) for (auto it = m_mapParaToIndex.cbegin(); it != m_mapParaToIndex.cend(); ++it)
        values.insert(it.key(), _model->GetParameterValue(it.value()));
    return values;
}

bool LAppModel::SetTimelinePreview(const CMotionPreviewFrame& frame, const CMotionTimeline* fadeTimeline,
                                  double time, bool loop)
{
    if (HasEditorPreview() || HasExportPreview()) return false;
    if (!_model || !frame.IsFinite() || (fadeTimeline && !std::isfinite(time))) return false;
    if (frame.IsEmpty()) { ClearTimelinePreview(); return true; }
    _model->LoadParameters();
    if (!_timelinePreviewActive) {
        _timelineBaseOpacity = _model->GetModelOpacity();
        _timelineBaseParameters = GetParameterValues();
        for (int index = 0; index < _model->GetPartCount(); ++index)
            _timelineBaseParts.insert(QString::fromUtf8(_model->GetPartId(index)->GetString().GetRawString()),
                                      _model->GetPartOpacity(index));
        _timelinePreviewActive = true;
    }
    for (auto it = _timelinePreview.parts.cbegin(); it != _timelinePreview.parts.cend(); ++it) {
        if (frame.parts.contains(it.key())) continue;
        const int index = FindPartIndex(it.key());
        if (index >= 0) _model->SetPartOpacity(index, _timelineBaseParts.value(it.key(), 1.0f));
    }
    QSet<QString> eyeIds, lipIds;
    for (csmUint32 i = 0; i < _eyeBlinkIds.GetSize(); ++i) {
        const auto id = QString::fromUtf8(_eyeBlinkIds[i]->GetString().GetRawString());
        if (!m_parameterReferences.eyeBlink.contains(id)) eyeIds.insert(id);
    }
    for (csmUint32 i = 0; i < _lipSyncIds.GetSize(); ++i) {
        const auto id = QString::fromUtf8(_lipSyncIds[i]->GetString().GetRawString());
        if (!m_parameterReferences.lipSync.contains(id)) lipIds.insert(id);
    }
    m_timelineExplicitParameterIds = QSet<QString>(frame.parameters.keyBegin(), frame.parameters.keyEnd());
    _timelinePreview = fadeTimeline
        ? CMotionBlendPreview::Evaluate(*fadeTimeline, time, _timelineBaseParameters, eyeIds, lipIds, loop) : frame;
    _timelineParameters = _timelinePreview.ResolveParameters(eyeIds, lipIds);
    if (!_timelinePreview.model.contains(QStringLiteral("Opacity"))) _model->SetModelOpacity(_timelineBaseOpacity);
    ApplyTimelinePreview();
    ApplyPartVisibility();
    _opacity = _model->GetModelOpacity();
    _model->Update();
    return true;
}

void LAppModel::ApplyTimelinePreview()
{
    if (!_model || !_timelinePreviewActive) return;
    for (auto it = _timelineParameters.cbegin(); it != _timelineParameters.cend(); ++it)
        if (m_mapParaToIndex.contains(it.key())) _model->SetParameterValue(m_mapParaToIndex.value(it.key()), it.value());
    for (auto it = _timelinePreview.parts.cbegin(); it != _timelinePreview.parts.cend(); ++it) {
        const int index = FindPartIndex(it.key());
        if (index >= 0) _model->SetPartOpacity(index, qBound(0.0f, it.value(), 1.0f));
    }
    if (_timelinePreview.model.contains(QStringLiteral("Opacity")))
        _model->SetModelOpacity(qBound(0.0f, _timelinePreview.model.value(QStringLiteral("Opacity")), 1.0f));
}

void LAppModel::ApplyPartVisibility()
{
    if (!_model) return;
    ApplyPartVisibility(*_model);
}

void LAppModel::ApplyPartVisibility(CubismModel& model) const
{
    if (_hiddenPartIds.isEmpty()) return;
    for (int index = 0; index < model.GetPartCount(); ++index)
        if (_hiddenPartIds.contains(QString::fromUtf8(model.GetPartId(index)->GetString().GetRawString())))
            model.SetPartOpacity(index, 0.0f);
}

void LAppModel::ClearTimelinePreview()
{
    if (HasEditorPreview() || HasExportPreview()) return;
    if (!_model || !_timelinePreviewActive) return;
    _timelinePreviewActive = false;
    _model->LoadParameters();
    for (auto it = _timelineBaseParts.cbegin(); it != _timelineBaseParts.cend(); ++it) {
        const int index = FindPartIndex(it.key());
        if (index >= 0) _model->SetPartOpacity(index, it.value());
    }
    _model->SetModelOpacity(_timelineBaseOpacity);
    _opacity = _timelineBaseOpacity;
    _timelinePreview = {};
    m_timelineExplicitParameterIds.clear();
    _timelineParameters.clear();
    _timelineBaseParts.clear();
    _timelineBaseParameters.clear();
    ApplyPartVisibility();
    _model->Update();
}

void LAppModel::UpdateParaValue(const QString &parId, float value,bool IsInit)
{
    if (HasEditorPreview() || HasExportPreview()) return;
    if(!_model || !std::isfinite(value) || !m_mapParaToIndex.contains(parId)) return;
    if (!IsInit) ClearTimelinePreview();
    int Id = m_mapParaToIndex[parId];

    // 先恢复干净的快照，修改目标参数，再保存回去
    // 避免把呼吸/物理等帧内叠加效果固化进快照
    _model->LoadParameters();
    _model->SetParameterValue(Id, value);
    _model->SaveParameters();

    if(IsInit){
        _model->Update();
        _model->InitMesh();
    }
}
