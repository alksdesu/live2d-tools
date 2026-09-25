/**
 * Copyright(c) Live2D Inc. All rights reserved.
 *
 * Use of this source code is governed by the Live2D Open Software license
 * that can be found at https://www.live2d.com/eula/live2d-open-software-license-agreement_en.html.
 */

#include "LAppModel.hpp"
#include "opengl/cmeshbounds.h"
#include "live2dstruct/cmotionblendpreview.h"

#include <framework/CubismModelSettingJson.hpp>
#include <framework/Motion/CubismMotion.hpp>
#include <framework/Physics/CubismPhysics.hpp>
#include <framework/CubismDefaultParameterId.hpp>
#include <framework/Rendering/OpenGL/CubismRenderer_OpenGLES2.hpp>
#include <framework/Utils/CubismString.hpp>
#include <framework/Id/CubismIdManager.hpp>
#include <framework/Motion/CubismMotionQueueEntry.hpp>
#include "LAppDefine.hpp"
#include "LAppPal.hpp"
#include "LAppTextureManager.hpp"
#include "LAppDelegate.hpp"
#include "LAppView.hpp"
#include "live2dstruct/ctreemanage.h"
#include "live2dstruct/cmodeluserdata.h"
#include "std/cmodeljson.h"
#include <QFile>
#include <QTextStream>
#include <QCoreApplication>
#include <QDir>
#include <QScopeGuard>
#include <cmath>
#include <limits>
#include <memory>

using namespace Live2D::Cubism::Framework;
using namespace Live2D::Cubism::Framework::DefaultParameterId;
using namespace LAppDefine;

namespace {
    csmByte* CreateBuffer(const csmChar* path, csmSizeInt* size)
    {
        if (DebugLogEnable)
        {
            LAppPal::PrintLogLn("[APP]create buffer: %s ", path);
        }
        return LAppPal::LoadFileAsBytes(path, size);
    }

    void DeleteBuffer(csmByte* buffer, const csmChar* path = "")
    {
        if (DebugLogEnable)
        {
            LAppPal::PrintLogLn("[APP]delete buffer: %s", path);
        }
        LAppPal::ReleaseBytes(buffer);
    }
}

LAppModel::LAppModel()
    : CubismUserModel()
    , _modelSetting(NULL)
    , _userTimeSeconds(0.0f)
{
    if (MocConsistencyValidationEnable)
    {
        _mocConsistency = true;
    }

    if (DebugLogEnable)
    {
        _debugMode = true;
    }

    _idParamAngleX = CubismFramework::GetIdManager()->GetId(ParamAngleX);
    _idParamAngleY = CubismFramework::GetIdManager()->GetId(ParamAngleY);
    _idParamAngleZ = CubismFramework::GetIdManager()->GetId(ParamAngleZ);
    _idParamBodyAngleX = CubismFramework::GetIdManager()->GetId(ParamBodyAngleX);
    _idParamEyeBallX = CubismFramework::GetIdManager()->GetId(ParamEyeBallX);
    _idParamEyeBallY = CubismFramework::GetIdManager()->GetId(ParamEyeBallY);
}

LAppModel::~LAppModel()
{
    DeleteRenderer();
    ReleasePendingTextures();
    _renderBuffer.DestroyOffscreenSurface();

    ReleaseMotions();
    ReleaseExpressions();

    if (_modelSetting != NULL)
    {
        for (csmInt32 i = 0; i < _modelSetting->GetMotionGroupCount(); i++)
        {
            const csmChar* group = _modelSetting->GetMotionGroupName(i);
            ReleaseMotionGroup(group);
        }
        delete(_modelSetting);
    }
}

bool LAppModel::LoadAssets(const csmChar* dir, const csmChar* fileName)
{
    if (!dir || !fileName || fileName[0] == '\0' || _modelSetting || _moc || _model ||
        _modelMatrix || _initialized || _updating) return false;

    if (_debugMode)
    {
        LAppPal::PrintLogLn("[APP]load model setting: %s", fileName);
    }

    csmSizeInt size = 0;
    const csmString path = csmString(dir) + fileName;

    csmByte* buffer = CreateBuffer(path.GetRawString(), &size);
    if (buffer == NULL)
    {
        LAppPal::PrintLogLn("[APP]Failed to load model setting: %s", path.GetRawString());
        return false;
    }
    const auto releaseBuffer = qScopeGuard([&] { DeleteBuffer(buffer, path.GetRawString()); });
    if (size == 0 || size > std::numeric_limits<int>::max()) return false;
    QString settingsError;
    if (!CModelJson::ValidateRuntimeSettings(
            QByteArray(reinterpret_cast<const char*>(buffer), static_cast<int>(size)), &settingsError)) {
        LAppPal::PrintLogLn("[APP]Invalid model settings: %s", settingsError.toUtf8().constData());
        return false;
    }
    auto setting = std::make_unique<CubismModelSettingJson>(buffer, size);
    if (!setting->GetJsonPointer()) return false;
    _modelHomeDir = dir;
    if (!SetupModel(setting.release())) return false;
    m_mocName = QString::fromUtf8(_modelSetting->GetModelFileName());
    if (!CreateRenderer()) return false;
    if (!SetupTextures()) {
        DeleteRenderer();
        return false;
    }
    _initialized = true;
    return true;
}

bool LAppModel::SetupModel(ICubismModelSetting* setting)
{
    _updating = true;
    const auto resetUpdating = qScopeGuard([this] { _updating = false; });
    _initialized = false;

    _modelSetting = setting;
    if (!_modelSetting || !_modelSetting->GetModelFileName() ||
        _modelSetting->GetModelFileName()[0] == '\0') return false;

    csmByte* buffer;
    csmSizeInt size;


    //要重写这部分模型加载
    //Cubism Model
    if (strcmp(_modelSetting->GetModelFileName(), "") != 0)
    //if (true)
    {
        csmString path = _modelSetting->GetModelFileName();
        path = _modelHomeDir + path;

        if (_debugMode)
        {
            LAppPal::PrintLogLn("[APP]create model: %s", setting->GetModelFileName());
        }

        buffer = CreateBuffer(path.GetRawString(), &size);
        if (buffer != NULL)
        {
            const bool loaded = LoadModel(buffer, size, _mocConsistency);
            DeleteBuffer(buffer, path.GetRawString());
            if (!loaded) return false;
        }
        else
        {
            LAppPal::PrintLogLn("[APP]Failed to load moc3: %s", path.GetRawString());
        }

        if (_model == NULL)
        {
            LAppPal::PrintLogLn("Failed to SetupModel().");
            return false;
        }

        m_mapParaToIndex.clear();
        int  Count = _model->GetParameterCount();
        for(int i = 0 ;i < Count;i++){
            QString name(_model->GetParameterId(i)->GetString().GetRawString()) ;
            m_mapParaToIndex.insert(name,i);
        }
    }

    //Expression
    if (_modelSetting->GetExpressionCount() > 0)
    {
        const csmInt32 count = _modelSetting->GetExpressionCount();
        for (csmInt32 i = 0; i < count; i++)
        {
            csmString name = _modelSetting->GetExpressionName(i);
            csmString path = _modelSetting->GetExpressionFileName(i);
            path = _modelHomeDir + path;

            buffer = CreateBuffer(path.GetRawString(), &size);
            if (buffer == NULL)
            {
                continue;
            }
            ACubismMotion* motion = LoadExpression(buffer, size, name.GetRawString());

            if (motion)
            {
                if (_expressions[name] != NULL)
                {
                    ACubismMotion::Delete(_expressions[name]);
                    _expressions[name] = NULL;
                }
                _expressions[name] = motion;
            }

            DeleteBuffer(buffer, path.GetRawString());
        }
    }

    //Physics
    if (strcmp(_modelSetting->GetPhysicsFileName(), "") != 0)
    {
        csmString path = _modelSetting->GetPhysicsFileName();
        path = _modelHomeDir + path;

        buffer = CreateBuffer(path.GetRawString(), &size);
        if (buffer != NULL)
        {
            LoadPhysics(buffer, size);
            DeleteBuffer(buffer, path.GetRawString());
        }
    }

    //Pose
    if (strcmp(_modelSetting->GetPoseFileName(), "") != 0)
    {
        csmString path = _modelSetting->GetPoseFileName();
        path = _modelHomeDir + path;

        buffer = CreateBuffer(path.GetRawString(), &size);
        if (buffer != NULL)
        {
            LoadPose(buffer, size);
            DeleteBuffer(buffer, path.GetRawString());
        }
    }

    //EyeBlink
    if (_modelSetting->GetEyeBlinkParameterCount() > 0)
    {
        _eyeBlink = CubismEyeBlink::Create(_modelSetting);
    }

    //Breath
    {
        _breath = CubismBreath::Create();

        csmVector<CubismBreath::BreathParameterData> breathParameters;

        breathParameters.PushBack(CubismBreath::BreathParameterData(_idParamAngleX, 0.0f, 15.0f, 6.5345f, 0.5f));
        breathParameters.PushBack(CubismBreath::BreathParameterData(_idParamAngleY, 0.0f, 8.0f, 3.5345f, 0.5f));
        breathParameters.PushBack(CubismBreath::BreathParameterData(_idParamAngleZ, 0.0f, 10.0f, 5.5345f, 0.5f));
        breathParameters.PushBack(CubismBreath::BreathParameterData(_idParamBodyAngleX, 0.0f, 4.0f, 15.5345f, 0.5f));
        breathParameters.PushBack(CubismBreath::BreathParameterData(CubismFramework::GetIdManager()->GetId(ParamBreath), 0.5f, 0.5f, 3.2345f, 0.5f));

        _breath->SetParameters(breathParameters);
    }

    //UserData
    if (strcmp(_modelSetting->GetUserDataFile(), "") != 0)
    {
        csmString path = _modelSetting->GetUserDataFile();
        path = _modelHomeDir + path;
        ReloadUserDataFromFile(QString::fromUtf8(path.GetRawString()));
    }

    // EyeBlinkIds
    {
        csmInt32 eyeBlinkIdCount = _modelSetting->GetEyeBlinkParameterCount();
        for (csmInt32 i = 0; i < eyeBlinkIdCount; ++i)
        {
            _eyeBlinkIds.PushBack(_modelSetting->GetEyeBlinkParameterId(i));
        }
    }

    // LipSyncIds
    {
        csmInt32 lipSyncIdCount = _modelSetting->GetLipSyncParameterCount();
        for (csmInt32 i = 0; i < lipSyncIdCount; ++i)
        {
            _lipSyncIds.PushBack(_modelSetting->GetLipSyncParameterId(i));
        }
    }

    if (_modelSetting == NULL || _modelMatrix == NULL)
    {
        LAppPal::PrintLogLn("Failed to SetupModel().");
        return false;
    }

    //Layout
    csmMap<csmString, csmFloat32> layout;
    _modelSetting->GetLayoutMap(layout);
    if (!_modelMatrix->TrySetupFromLayout(layout)) return false;

    _model->SaveParameters();

    for (csmInt32 i = 0; i < _modelSetting->GetMotionGroupCount(); i++)
    {
        const csmChar* group = _modelSetting->GetMotionGroupName(i);
        PreloadMotionGroup(group);
    }

    _motionManager->StopAllMotions();

    return true;
}

void LAppModel::PreloadMotionGroup(const csmChar* group)
{
    const csmInt32 count = _modelSetting->GetMotionCount(group);

    for (csmInt32 i = 0; i < count; i++)
    {
        //ex) idle_0
        csmString name = Utils::CubismString::GetFormatedString("%s_%d", group, i);
        csmString path = _modelSetting->GetMotionFileName(group, i);
        path = _modelHomeDir + path;

        if (_debugMode)
        {
            LAppPal::PrintLogLn("[APP]load motion: %s => [%s_%d] ", path.GetRawString(), group, i);
        }

        csmByte* buffer;
        csmSizeInt size;
        buffer = CreateBuffer(path.GetRawString(), &size);
        if (buffer == NULL)
        {
            continue;
        }
        CubismMotion* tmpMotion = static_cast<CubismMotion*>(LoadMotion(buffer, size, name.GetRawString()));

        if (tmpMotion)
        {
            csmFloat32 fadeTime = _modelSetting->GetMotionFadeInTimeValue(group, i);
            if (fadeTime >= 0.0f)
            {
                tmpMotion->SetFadeInTime(fadeTime);
            }

            fadeTime = _modelSetting->GetMotionFadeOutTimeValue(group, i);
            if (fadeTime >= 0.0f)
            {
                tmpMotion->SetFadeOutTime(fadeTime);
            }
            tmpMotion->SetEffectIds(_eyeBlinkIds, _lipSyncIds);

            if (_motions[name] != NULL)
            {
                ACubismMotion::Delete(_motions[name]);
            }
            _motions[name] = tmpMotion;
        }

        DeleteBuffer(buffer, path.GetRawString());
    }
}

void LAppModel::ReleaseMotionGroup(const csmChar* group) const
{
    const csmInt32 count = _modelSetting->GetMotionCount(group);
    for (csmInt32 i = 0; i < count; i++)
    {
        csmString voice = _modelSetting->GetMotionSoundFileName(group, i);
        if (strcmp(voice.GetRawString(), "") != 0)
        {
            csmString path = voice;
            path = _modelHomeDir + path;
        }
    }
}

/**
* @brief すべてのモーションデータの解放
*
* すべてのモーションデータを解放する。
*/
void LAppModel::ReleaseMotions()
{
    for (csmMap<csmString, ACubismMotion*>::const_iterator iter = _motions.Begin(); iter != _motions.End(); ++iter)
    {
        ACubismMotion::Delete(iter->Second);
    }

    _motions.Clear();
}

/**
* @brief すべての表情データの解放
*
* すべての表情データを解放する。
*/
void LAppModel::ReleaseExpressions()
{
    for (csmMap<csmString, ACubismMotion*>::const_iterator iter = _expressions.Begin(); iter != _expressions.End(); ++iter)
    {
        ACubismMotion::Delete(iter->Second);
    }

    _expressions.Clear();
}

void LAppModel::Update()
{
    if (!_initialized || _updating || !_model) return;
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
        _model->AddParameterValue(_idParamAngleX, _dragX * 30);
        _model->AddParameterValue(_idParamAngleY, _dragY * 30);
        _model->AddParameterValue(_idParamAngleZ, _dragX * _dragY * -30);
        _model->AddParameterValue(_idParamBodyAngleX, _dragX * 10);
        _model->AddParameterValue(_idParamEyeBallX, _dragX);
        _model->AddParameterValue(_idParamEyeBallY, _dragY);
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
    if(!_deformerDragParamIds.isEmpty() && _deformerDragActive){
        for(int i = 0; i < _deformerDragParamIds.size(); i++){
            if(!m_mapParaToIndex.contains(_deformerDragParamIds[i])) continue;
            int idx = m_mapParaToIndex[_deformerDragParamIds[i]];
            _model->SetParameterValue(idx, _deformerDragValues[i]);
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
    for (csmUint32 i = 0; i < _eyeBlinkIds.GetSize(); ++i)
        eyeIds.insert(QString::fromUtf8(_eyeBlinkIds[i]->GetString().GetRawString()));
    for (csmUint32 i = 0; i < _lipSyncIds.GetSize(); ++i)
        lipIds.insert(QString::fromUtf8(_lipSyncIds[i]->GetString().GetRawString()));
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
    for (const auto& id : _hiddenPartIds) {
        const int index = FindPartIndex(id);
        if (index >= 0) _model->SetPartOpacity(index, 0.0f);
    }
}

void LAppModel::ClearTimelinePreview()
{
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
    _timelineParameters.clear();
    _timelineBaseParts.clear();
    _timelineBaseParameters.clear();
    ApplyPartVisibility();
    _model->Update();
}

void LAppModel::UpdateParaValue(const QString &parId, float value,bool IsInit)
{
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

CubismMotionQueueEntryHandle LAppModel::StartMotion(const csmChar* group, csmInt32 no, csmInt32 priority, ACubismMotion::FinishedMotionCallback onFinishedMotionHandler)
{
    if (!_modelSetting || !_motionManager || !group || no < 0 || no >= _modelSetting->GetMotionCount(group))
        return InvalidMotionQueueEntryHandleValue;
    if (priority == PriorityForce)
    {
        _motionManager->SetReservePriority(priority);
    }
    else if (!_motionManager->ReserveMotion(priority))
    {
        if (_debugMode)
        {
            LAppPal::PrintLogLn("[APP]can't start motion.");
        }
        return InvalidMotionQueueEntryHandleValue;
    }

    const csmString fileName = _modelSetting->GetMotionFileName(group, no);

    //ex) idle_0
    csmString name = Utils::CubismString::GetFormatedString("%s_%d", group, no);
    CubismMotion* motion = static_cast<CubismMotion*>(_motions[name.GetRawString()]);
    csmBool autoDelete = false;

    if (motion == NULL)
    {
        csmString path = fileName;
        path = _modelHomeDir + path;

        csmByte* buffer;
        csmSizeInt size;
        buffer = CreateBuffer(path.GetRawString(), &size);
        if (buffer == NULL)
        {
            return InvalidMotionQueueEntryHandleValue;
        }
        motion = static_cast<CubismMotion*>(LoadMotion(buffer, size, NULL, onFinishedMotionHandler));

        if  (motion)
        {
            csmFloat32 fadeTime = _modelSetting->GetMotionFadeInTimeValue(group, no);
            if (fadeTime >= 0.0f)
            {
                motion->SetFadeInTime(fadeTime);
            }

            fadeTime = _modelSetting->GetMotionFadeOutTimeValue(group, no);
            if (fadeTime >= 0.0f)
            {
                motion->SetFadeOutTime(fadeTime);
            }
            motion->SetEffectIds(_eyeBlinkIds, _lipSyncIds);
            autoDelete = true; // 終了時にメモリから削除
        }

        DeleteBuffer(buffer, path.GetRawString());
    }
    else
    {
        motion->SetFinishedMotionHandler(onFinishedMotionHandler);
    }

    //voice
    csmString voice = _modelSetting->GetMotionSoundFileName(group, no);
    if (strcmp(voice.GetRawString(), "") != 0)
    {
        csmString path = voice;
        path = _modelHomeDir + path;
        _wavFileHandler.Start(path);
    }
    else
    {
        _wavFileHandler.Stop();
    }

    if (_debugMode)
    {
        LAppPal::PrintLogLn("[APP]start motion: [%s_%d]", group, no);
    }
    if (motion) {
        CubismMotion* cubismMotion = static_cast<CubismMotion*>(motion);
        cubismMotion->IsLoop(_motionLoop);
    }
    return  _motionManager->StartMotionPriority(motion, autoDelete, priority);
}

void LAppModel::SetMotionSpeed(float value)
{
    if (!std::isfinite(value)) return;
    if (value < 0.25f) value = 0.25f;
    if (value > 2.0f) value = 2.0f;
    _motionSpeed = value;
}

void LAppModel::SetMotionLoop(bool value)
{
    _motionLoop = value;
    for (csmMap<csmString, ACubismMotion*>::const_iterator iter = _motions.Begin(); iter != _motions.End(); ++iter) {
        if (iter->Second) static_cast<CubismMotion*>(iter->Second)->IsLoop(value);
    }
}

CubismMotionQueueEntryHandle LAppModel::StartRandomMotion(const csmChar* group, csmInt32 priority, ACubismMotion::FinishedMotionCallback onFinishedMotionHandler)
{
    if (!_modelSetting || !group) return InvalidMotionQueueEntryHandleValue;
    if (_modelSetting->GetMotionCount(group) == 0)
    {
        return InvalidMotionQueueEntryHandleValue;
    }

    csmInt32 no = rand() % _modelSetting->GetMotionCount(group);

    return StartMotion(group, no, priority, onFinishedMotionHandler);
}

void LAppModel::StopAllMotions()
{
    if (_motionManager) _motionManager->StopAllMotions();
    _wavFileHandler.Stop();
}

bool LAppModel::DoDraw()
{
    if (!_initialized || _updating || !_model)
    {
        return false;
    }

    auto* renderer = GetRenderer<Rendering::CubismRenderer_OpenGLES2>();
    if (!renderer) return false;
    const auto color = renderer->GetModelColor();
    renderer->SetModelColor(color.R, color.G, color.B, _model->GetModelOpacity());
    return renderer->DrawModel();
}

bool LAppModel::Draw(CubismMatrix44& matrix)
{
    if (!_initialized || _updating || !_model || !_modelMatrix)
    {
        return false;
    }

    auto* renderer = GetRenderer<Rendering::CubismRenderer_OpenGLES2>();
    if (!renderer) return false;
    matrix.MultiplyByMatrix(_modelMatrix);
    renderer->SetMvpMatrix(&matrix);

    return DoDraw();
}

csmBool LAppModel::HitTest(const csmChar* hitAreaName, csmFloat32 x, csmFloat32 y)
{
    if (!_model || !_modelSetting || !hitAreaName) return false;
    // 透明時は当たり判定なし。
    if (_opacity < 1)
    {
        return false;
    }
    const csmInt32 count = _modelSetting->GetHitAreasCount();
    for (csmInt32 i = 0; i < count; i++)
    {
        if (strcmp(_modelSetting->GetHitAreaName(i), hitAreaName) == 0)
        {
            const CubismIdHandle drawID = _modelSetting->GetHitAreaId(i);
            return IsHit(drawID, x, y);
        }
    }
    return false; // 存在しない場合はfalse
}

void LAppModel::SetExpression(const csmChar* expressionID)
{
    if (!_expressionManager || !expressionID) return;
    ACubismMotion* motion = _expressions[expressionID];
    if (_debugMode)
    {
        LAppPal::PrintLogLn("[APP]expression: [%s]", expressionID);
    }

    if (motion != NULL)
    {
        _expressionManager->StartMotionPriority(motion, false, PriorityForce);
    }
    else
    {
        if (_debugMode) LAppPal::PrintLogLn("[APP]expression[%s] is null ", expressionID);
    }
}

bool LAppModel::ReloadExpressionFromFile(const QString& expressionId, const QString& path)
{
    if (expressionId.isEmpty() || path.isEmpty()) return false;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return false;
    const QByteArray data = file.readAll();
    if (file.error() != QFileDevice::NoError || data.isEmpty() ||
        data.size() > std::numeric_limits<csmSizeInt>::max()) return false;
    const QByteArray nameBytes = expressionId.toUtf8();
    ACubismMotion* replacement = LoadExpression(
        reinterpret_cast<const csmByte*>(data.constData()),
        static_cast<csmSizeInt>(data.size()), nameBytes.constData());
    if (!replacement) return false;
    const csmString key(nameBytes.constData());
    if (_expressionManager) _expressionManager->StopAllMotions();
    if (_expressions[key] != NULL) ACubismMotion::Delete(_expressions[key]);
    _expressions[key] = replacement;
    return true;
}

void LAppModel::SetRandomExpression()
{
    if (_expressions.GetSize() == 0)
    {
        return;
    }

    csmInt32 no = rand() % _expressions.GetSize();
    csmMap<csmString, ACubismMotion*>::const_iterator map_ite;
    csmInt32 i = 0;
    for (map_ite = _expressions.Begin(); map_ite != _expressions.End(); map_ite++)
    {
        if (i == no)
        {
            csmString name = (*map_ite).First;
            SetExpression(name.GetRawString());
            return;
        }
        i++;
    }
}

bool LAppModel::ReloadRenderer()
{
    if (!_model || !_modelSetting || !_modelMatrix || _updating) return false;
    if (!CreateRenderer()) return false;
    _initialized = false;
    if (!SetupTextures()) {
        DeleteRenderer();
        return false;
    }
    _initialized = true;
    if (m_textureCacheCommitted) CommitTextureCache();
    return true;
}

bool LAppModel::SetupTextures()
{
    if (!_model || !_modelSetting) return false;
    auto* renderer = GetRenderer<Rendering::CubismRenderer_OpenGLES2>();
    auto* textures = LAppDelegate::GetInstance()->GetTextureManager();
    if (!renderer) return false;
    QMap<int, QString> paths;
    QMap<int, GLuint> bindings;
    QSet<GLuint> created;
    auto rollback = qScopeGuard([&] {
        for (GLuint textureId : created) textures->ReleaseTexture(textureId);
    });
    for (csmInt32 modelTextureNumber = 0; modelTextureNumber < _modelSetting->GetTextureCount(); modelTextureNumber++)
    {
        const csmChar* textureName = _modelSetting->GetTextureFileName(modelTextureNumber);
        if (!textureName) return false;
        if (textureName[0] == '\0') continue;
        if (!textures) return false;
        const QString path = QDir::cleanPath(QDir(QString::fromUtf8(_modelHomeDir.GetRawString()))
                                                .absoluteFilePath(QString::fromUtf8(textureName)));
        const std::string fileName = path.toUtf8().toStdString();
        auto* texture = textures->GetTextureInfoByName(fileName);
        if (!texture) {
            texture = textures->CreateTextureFromPngFile(fileName);
            if (texture && texture->id != 0) created.insert(texture->id);
        }
        if (!texture || texture->id == 0 || texture->width <= 0 || texture->height <= 0) return false;
        paths.insert(modelTextureNumber, path);
        bindings.insert(modelTextureNumber, texture->id);
    }
    for (auto it = bindings.cbegin(); it != bindings.cend(); ++it) renderer->BindTexture(it.key(), it.value());
#ifdef PREMULTIPLIED_ALPHA_ENABLE
    renderer->IsPremultipliedAlpha(true);
#else
    renderer->IsPremultipliedAlpha(false);
#endif
    m_texturePaths = std::move(paths);
    m_pendingTextureIds.unite(created);
    rollback.dismiss();
    return true;
}

void LAppModel::ReleasePendingTextures()
{
    if (m_pendingTextureIds.isEmpty()) return;
    auto* textures = LAppDelegate::GetInstance()->GetTextureManager();
    if (textures) for (csmUint32 textureId : m_pendingTextureIds) textures->ReleaseTexture(textureId);
    m_pendingTextureIds.clear();
}

void LAppModel::MotionEventFired(const csmString& eventValue)
{
    CubismLogInfo("%s is fired on LAppModel!!", eventValue.GetRawString());
}

Csm::Rendering::CubismOffscreenSurface_OpenGLES2& LAppModel::GetRenderBuffer()
{
    return _renderBuffer;
}

csmBool LAppModel::HasMocConsistencyFromFile(const csmChar* mocFileName)
{
    CSM_ASSERT(strcmp(mocFileName, ""));

    csmByte* buffer;
    csmSizeInt size;

    csmString path = mocFileName;
    path = _modelHomeDir + path;

    buffer = CreateBuffer(path.GetRawString(), &size);
    if (buffer == NULL)
    {
        CubismLogInfo("Failed to load MOC3 for consistency check.");
        return false;
    }

    csmBool consistency = CubismMoc::HasMocConsistencyFromUnrevivedMoc(buffer, size);
    if (!consistency)
    {
        CubismLogInfo("Inconsistent MOC3.");
    }
    else
    {
        CubismLogInfo("Consistent MOC3.");
    }

    DeleteBuffer(buffer);

    return consistency;
}

QRectF LAppModel::GetCurModelRectF()
{
    if (!_model) return {};
    QRectF result;
    for (int i = 0; i < _model->GetDrawableCount(); ++i) {
        result = result.united(MeshBounds::Model(_model->GetDrawableVertices(i),
                                               _model->GetDrawableVertexCount(i)));
    }
    return result;
}

void LAppModel::SetPhysicsGravity(float x, float y)
{
    if(_physics && std::isfinite(x) && std::isfinite(y)){
        auto opt = _physics->GetOptions();
        opt.Gravity.X = x;
        opt.Gravity.Y = y;
        _physics->SetOptions(opt);
    }
}

bool LAppModel::ReloadPhysicsFromFile(const QString& path)
{
    if (path.isEmpty()) return false;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return false;
    const QByteArray data = file.readAll();
    if (file.error() != QFileDevice::NoError || data.isEmpty() ||
        data.size() > std::numeric_limits<csmSizeInt>::max()) return false;
    CubismPhysics* replacement = CubismPhysics::Create(
        reinterpret_cast<const csmByte*>(data.constData()), static_cast<csmSizeInt>(data.size()));
    if (!replacement) return false;
    CubismPhysics::Delete(_physics);
    _physics = replacement;
    return true;
}

bool LAppModel::ReloadUserDataFromFile(const QString& path)
{
    if (!_model) return false;
    live2dstruct::CModelUserData document;
    if (!document.Load(path)) return false;
    QSet<QString> drawableIds;
    for (int index = 0; index < _model->GetDrawableCount(); ++index)
        drawableIds.insert(QString::fromUtf8(_model->GetDrawableId(index)->GetString().GetRawString()));
    for (const auto& entry : document.Entries())
        if (!drawableIds.contains(entry.id)) return false;
    const QByteArray bytes = document.ToJson();
    if (bytes.size() > std::numeric_limits<csmSizeInt>::max()) return false;
    CubismModelUserData* replacement = CubismModelUserData::Create(
        reinterpret_cast<const csmByte*>(bytes.constData()), static_cast<csmSizeInt>(bytes.size()));
    if (!replacement) return false;
    if (replacement->GetArtMeshUserDatas().GetSize() != document.Entries().size()) {
        CubismModelUserData::Delete(replacement);
        return false;
    }
    CubismModelUserData::Delete(_modelUserData);
    _modelUserData = replacement;
    return true;
}

void LAppModel::SetPhysicsDragParams(const QStringList& paramIds, const QStringList& types)
{
    _physicsDragParamIds = paramIds;
    _physicsDragTypes = types;
}

void LAppModel::ApplyPhysicsDrag(float dx, float dy)
{
    if(!_model || _physicsDragParamIds.isEmpty() || !std::isfinite(dx) || !std::isfinite(dy)) return;
    for(int i = 0; i < _physicsDragParamIds.size(); i++){
        const CubismId* paramId = CubismFramework::GetIdManager()->GetId(_physicsDragParamIds[i].toUtf8().constData());
        float value = 0;
        QString type = (i < _physicsDragTypes.size()) ? _physicsDragTypes[i] : "X";
        if(type == "X") value = dx * 30.0f;
        else if(type == "Y") value = dy * 30.0f;
        else if(type == "Angle") value = (dx + dy) * 15.0f;
        _model->AddParameterValue(paramId, value);
    }
}

void LAppModel::SetPhysicsWind(float x, float y)
{
    if(_physics && std::isfinite(x) && std::isfinite(y)){
        auto opt = _physics->GetOptions();
        opt.Wind.X = x;
        opt.Wind.Y = y;
        _physics->SetOptions(opt);
    }
}

bool LAppModel::StartLipSyncWav(const QString& path)
{
    if (path.isEmpty()) return false;
    const QByteArray utf8Path = path.toUtf8();
    return _wavFileHandler.Start(csmString(utf8Path.constData()));
}

void LAppModel::StopLipSyncWav()
{
    _wavFileHandler.Stop();
}

void LAppModel::SetPartVisible(const QString& partId, bool visible)
{
    if (!_model) return;
    if(visible){
        _hiddenPartIds.remove(partId);
        CubismIdHandle id = CubismFramework::GetIdManager()->GetId(partId.toUtf8().constData());
        _model->SetPartOpacity(id, 1.0f);
    } else {
        _hiddenPartIds.insert(partId);
    }
}

void LAppModel::SetDeformerDragParams(const QStringList& paramIds, const QList<float>& minVals,
                                       const QList<float>& maxVals, const QList<float>& defaultVals)
{
    _deformerDragParamIds.clear();
    _deformerDragMinVals.clear();
    _deformerDragMaxVals.clear();
    _deformerDragDefaultVals.clear();
    const int count = qMin(paramIds.size(), qMin(minVals.size(), qMin(maxVals.size(), defaultVals.size())));
    for (int i = 0; i < count; ++i) {
        if (paramIds[i].isEmpty() || !std::isfinite(minVals[i]) || !std::isfinite(maxVals[i]) ||
            !std::isfinite(defaultVals[i]) || minVals[i] > maxVals[i]) continue;
        _deformerDragParamIds.append(paramIds[i]);
        _deformerDragMinVals.append(minVals[i]);
        _deformerDragMaxVals.append(maxVals[i]);
        _deformerDragDefaultVals.append(qBound(minVals[i], defaultVals[i], maxVals[i]));
    }
}

void LAppModel::ApplyDeformerDrag(float viewX, float viewY)
{
    if(!_model || _deformerDragParamIds.isEmpty() ||
       _deformerDragParamIds.size() != _deformerDragMinVals.size() ||
       _deformerDragParamIds.size() != _deformerDragMaxVals.size() ||
       !std::isfinite(viewX) || !std::isfinite(viewY)) return;

    _deformerDragActive = true;
    _deformerDragValues.resize(_deformerDragParamIds.size());
    for(int i = 0; i < _deformerDragParamIds.size(); i++){
        float minV = _deformerDragMinVals[i];
        float maxV = _deformerDragMaxVals[i];
        float range = maxV - minV;
        float input = (i % 2 == 0) ? viewX : viewY;
        float value = minV + (input + 1.0f) * 0.5f * range;
        _deformerDragValues[i] = qBound(minV, value, maxV);
    }

    // 同步更新 C2DTree 的参数，让变形器线条跟着动
    C2DTree* tree = CTreeManage::GetInstance();
    if (!tree) return;
    for(int i = 0; i < _deformerDragParamIds.size(); i++){
        tree->SetParaValue(_deformerDragParamIds[i], _deformerDragValues[i]);
    }
    LAppDelegate::GetInstance()->GetView()->UpdateWarpPositions();
}

void LAppModel::ProbeParamsForArtMeshes(const QStringList& artMeshNames)
{
    _deformerDragParamIds.clear();
    _deformerDragMinVals.clear();
    _deformerDragMaxVals.clear();
    _deformerDragDefaultVals.clear();

    if(!_model || artMeshNames.isEmpty()) return;

    // 找到目标 ArtMesh 的 drawable 索引
    QVector<int> targetIndices;
    QVector<int> allIndices;
    int drawCount = _model->GetDrawableCount();
    QSet<QString> targetSet(artMeshNames.begin(), artMeshNames.end());
    for(int i = 0; i < drawCount; i++){
        QString name(_model->GetDrawableId(i)->GetString().GetRawString());
        allIndices.append(i);
        if(targetSet.contains(name)){
            targetIndices.append(i);
        }
    }
    if(targetIndices.isEmpty()) return;

    // 保存当前参数快照
    int paramCount = _model->GetParameterCount();
    QVector<float> savedValues(paramCount);
    for(int i = 0; i < paramCount; i++){
        savedValues[i] = _model->GetParameterValue(i);
    }

    // 记录所有 drawable 的基准顶点（只取第一个顶点做快速比较）
    _model->Update();
    QVector<QPair<float,float>> baseFirstVert(drawCount);
    for(int di = 0; di < drawCount; di++){
        if(_model->GetDrawableVertexCount(di) > 0){
            auto* v = _model->GetDrawableVertexPositions(di);
            baseFirstVert[di] = {v[0].X, v[0].Y};
        }
    }

    // 逐个参数微扰测试
    struct ParamScore { int idx; bool affectsTarget; int totalAffected; };
    QVector<ParamScore> candidates;

    for(int p = 0; p < paramCount; p++){
        float minV = _model->GetParameterMinimumValue(p);
        float maxV = _model->GetParameterMaximumValue(p);
        float curV = savedValues[p];
        float testV = (curV < (minV + maxV) * 0.5f) ? maxV : minV;

        for(int i = 0; i < paramCount; i++)
            _model->SetParameterValue(i, savedValues[i]);
        _model->SetParameterValue(p, testV);
        _model->Update();

        // 检查目标是否变化
        bool affectsTarget = false;
        for(int di : targetIndices){
            if(_model->GetDrawableVertexCount(di) > 0){
                auto* v = _model->GetDrawableVertexPositions(di);
                float dx = v[0].X - baseFirstVert[di].first;
                float dy = v[0].Y - baseFirstVert[di].second;
                if(dx * dx + dy * dy > 1e-10f){
                    affectsTarget = true;
                    break;
                }
            }
        }
        if(!affectsTarget) continue;

        // 计算该参数影响了多少个 drawable
        int totalAffected = 0;
        for(int di = 0; di < drawCount; di++){
            if(_model->GetDrawableVertexCount(di) > 0){
                auto* v = _model->GetDrawableVertexPositions(di);
                float dx = v[0].X - baseFirstVert[di].first;
                float dy = v[0].Y - baseFirstVert[di].second;
                if(dx * dx + dy * dy > 1e-10f) totalAffected++;
            }
        }
        candidates.append({p, true, totalAffected});
    }

    // 恢复参数
    for(int i = 0; i < paramCount; i++)
        _model->SetParameterValue(i, savedValues[i]);
    _model->Update();

    // 过滤：只保留局部参数（影响不超过 50% 的 drawable）
    int threshold = drawCount / 2;
    QVector<ParamScore> localParams;
    for(const auto& c : candidates){
        if(c.totalAffected <= threshold)
            localParams.append(c);
    }
    // 如果没有局部参数，退而取影响面最小的几个
    if(localParams.isEmpty() && !candidates.isEmpty()){
        std::sort(candidates.begin(), candidates.end(),
                  [](const ParamScore& a, const ParamScore& b){ return a.totalAffected < b.totalAffected; });
        for(int i = 0; i < qMin(candidates.size(), 4); i++)
            localParams.append(candidates[i]);
    }

    // 按影响面从小到大排序，最多取4个
    std::sort(localParams.begin(), localParams.end(),
              [](const ParamScore& a, const ParamScore& b){ return a.totalAffected < b.totalAffected; });
    int limit = qMin(localParams.size(), 4);
    for(int i = 0; i < limit; i++){
        int p = localParams[i].idx;
        QString id(_model->GetParameterId(p)->GetString().GetRawString());
        _deformerDragParamIds << id;
        _deformerDragMinVals << _model->GetParameterMinimumValue(p);
        _deformerDragMaxVals << _model->GetParameterMaximumValue(p);
        _deformerDragDefaultVals << _model->GetParameterDefaultValue(p);
    }
}
