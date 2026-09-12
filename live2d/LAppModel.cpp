/**
 * Copyright(c) Live2D Inc. All rights reserved.
 *
 * Use of this source code is governed by the Live2D Open Software license
 * that can be found at https://www.live2d.com/eula/live2d-open-software-license-agreement_en.html.
 */

#include "LAppModel.hpp"
#include "opengl/cmeshbounds.h"

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
#include <QFile>
#include <QTextStream>
#include <QCoreApplication>
#include <cmath>

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

void LAppModel::LoadAssets(const csmChar* dir, const csmChar* fileName)
{
    m_mapParaToIndex.clear();
    _modelHomeDir = dir;

    if (_debugMode)
    {
        LAppPal::PrintLogLn("[APP]load model setting: %s", fileName);
    }

    csmSizeInt size;
    const csmString path = csmString(dir) + fileName;

    csmByte* buffer = CreateBuffer(path.GetRawString(), &size);
    if (buffer == NULL)
    {
        LAppPal::PrintLogLn("[APP]Failed to load model setting: %s", path.GetRawString());
        return;
    }
    ICubismModelSetting* setting = new CubismModelSettingJson(buffer, size);
    DeleteBuffer(buffer, path.GetRawString());

    SetupModel(setting);

    if (_model == NULL)
    {
        LAppPal::PrintLogLn("Failed to LoadAssets().");
        return;
    }

    m_mocName = setting->GetModelFileName();

    CreateRenderer();

    SetupTextures();
}

void LAppModel::SetupModel(ICubismModelSetting* setting)
{
    _updating = true;
    _initialized = false;

    _modelSetting = setting;

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
            LoadModel(buffer, size, _mocConsistency);
            DeleteBuffer(buffer, path.GetRawString());
        }
        else
        {
            LAppPal::PrintLogLn("[APP]Failed to load moc3: %s", path.GetRawString());
        }

        if (_model == NULL)
        {
            LAppPal::PrintLogLn("Failed to SetupModel().");
            return;
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
        buffer = CreateBuffer(path.GetRawString(), &size);
        if (buffer != NULL)
        {
            LoadUserData(buffer, size);
            DeleteBuffer(buffer, path.GetRawString());
        }
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
        return;
    }

    //Layout
    csmMap<csmString, csmFloat32> layout;
    _modelSetting->GetLayoutMap(layout);
    _modelMatrix->SetupFromLayout(layout);

    _model->SaveParameters();

    for (csmInt32 i = 0; i < _modelSetting->GetMotionGroupCount(); i++)
    {
        const csmChar* group = _modelSetting->GetMotionGroupName(i);
        PreloadMotionGroup(group);
    }

    _motionManager->StopAllMotions();

    _updating = false;
    _initialized = true;

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
    if (!_model) return;
    // 快照模式：只应用已保存的参数，跳过所有自动效果
    if (_snapshotMode) {
        _model->LoadParameters();
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
    if (!_motionManager->IsFinished())
    {
        motionUpdated = _motionManager->UpdateMotion(_model, deltaTimeSeconds);
    }

    _model->SaveParameters();

    _opacity = _model->GetModelOpacity();
    if (!motionUpdated)
    {
        if (_eyeBlink != NULL && _autoEyeBlink)
        {
            _eyeBlink->UpdateParameters(_model, deltaTimeSeconds);
        }
    }

    // 表情
    if (_expressionManager != NULL)
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

    if (_physics != NULL && _autoPhysics)
    {
        _physics->Evaluate(_model, deltaTimeSeconds);
    }

    // Pose
    if (_pose != NULL)
    {
        _pose->UpdateParameters(_model, deltaTimeSeconds);
    }

    // Part 可见性覆盖
    if(!_hiddenPartIds.isEmpty()){
        for(const QString& partId : _hiddenPartIds){
            CubismIdHandle id = CubismFramework::GetIdManager()->GetId(partId.toUtf8().constData());
            _model->SetPartOpacity(id, 0.0f);
        }
    }

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

void LAppModel::UpdateParaValue(const QString &parId, float value,bool IsInit)
{
    if(!_model || !std::isfinite(value) || !m_mapParaToIndex.contains(parId)) return;
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

    if (_debugMode)
    {
        LAppPal::PrintLogLn("[APP]start motion: [%s_%d]", group, no);
    }
    return  _motionManager->StartMotionPriority(motion, autoDelete, priority);
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
}

void LAppModel::DoDraw()
{
    if (_model == NULL)
    {
        return;
    }

    GetRenderer<Rendering::CubismRenderer_OpenGLES2>()->DrawModel();
}

void LAppModel::Draw(CubismMatrix44& matrix)
{
    if (_model == NULL)
    {
        return;
    }

    matrix.MultiplyByMatrix(_modelMatrix);
    GetRenderer<Rendering::CubismRenderer_OpenGLES2>()->SetMvpMatrix(&matrix);

    DoDraw();
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

void LAppModel::ReloadRenderer()
{
    DeleteRenderer();

    CreateRenderer();

    SetupTextures();
}

void LAppModel::SetupTextures()
{
    for (csmInt32 modelTextureNumber = 0; modelTextureNumber < _modelSetting->GetTextureCount(); modelTextureNumber++)
    {
        // テクスチャ名が空文字だった場合はロード・バインド処理をスキップ
        if (strcmp(_modelSetting->GetTextureFileName(modelTextureNumber), "") == 0)
        {
            continue;
        }

        //OpenGLのテクスチャユニットにテクスチャをロードする
        csmString texturePath = _modelSetting->GetTextureFileName(modelTextureNumber);
        texturePath = _modelHomeDir + texturePath;

        LAppTextureManager::TextureInfo* texture = LAppDelegate::GetInstance()->GetTextureManager()->CreateTextureFromPngFile(texturePath.GetRawString());
        const csmInt32 glTextueNumber = texture->id;
        LAppDelegate::GetInstance()->GetTextureManager()->AddModelTex(modelTextureNumber,QString(texturePath.GetRawString()));

        LAppDelegate::GetInstance()->GetTextureManager()->CreateTextureFromPngFile(texturePath.GetRawString());
        //OpenGL
        GetRenderer<Rendering::CubismRenderer_OpenGLES2>()->BindTexture(modelTextureNumber, glTextueNumber);
    }

#ifdef PREMULTIPLIED_ALPHA_ENABLE
    GetRenderer<Rendering::CubismRenderer_OpenGLES2>()->IsPremultipliedAlpha(true);
#else
    GetRenderer<Rendering::CubismRenderer_OpenGLES2>()->IsPremultipliedAlpha(false);
#endif

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
