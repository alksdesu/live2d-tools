/**
 * Copyright(c) Live2D Inc. All rights reserved.
 *
 * Use of this source code is governed by the Live2D Open Software license
 * that can be found at https://www.live2d.com/eula/live2d-open-software-license-agreement_en.html.
 */

#include "LAppModel.hpp"
#include <QCryptographicHash>
#include <limits>
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
#include "std/cmodeljson.h"
#include "std/cmodelresourcepath.h"
#include <QFile>
#include <QTextStream>
#include <QCoreApplication>
#include <QDir>
#include <QScopeGuard>
#include <QScopedValueRollback>
#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <exception>

using namespace Live2D::Cubism::Framework;
using namespace Live2D::Cubism::Framework::DefaultParameterId;
using namespace LAppDefine;

namespace {
    csmString ResourcePath(const csmString& directory, const csmChar* reference)
    {
        const QString path = CModelResourcePath::Resolve(QString::fromUtf8(directory.GetRawString()),
                                                        QString::fromUtf8(reference ? reference : ""));
        return csmString(path.toUtf8().constData());
    }

    csmByte* CreateBuffer(const csmChar* path, csmSizeInt* size)
    {
        if (!path || path[0] == '\0') { if (size) *size = 0; return nullptr; }
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
    const csmString path = ResourcePath(csmString(dir), fileName);
    if (path.GetLength() == 0) return false;

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
    const QString modelDirectory = QFileInfo(QString::fromUtf8(path.GetRawString())).absolutePath() + QLatin1Char('/');
    _modelHomeDir = csmString(modelDirectory.toUtf8().constData());
    m_modelSettingsPath = QDir::cleanPath(QFileInfo(QString::fromUtf8(path.GetRawString())).absoluteFilePath());
    m_modelSettingsBytes = QByteArray(reinterpret_cast<const char*>(buffer), int(size));
    m_modelSettingsDigest = QCryptographicHash::hash(m_modelSettingsBytes, QCryptographicHash::Sha256);
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
        const csmString path = ResourcePath(_modelHomeDir, _modelSetting->GetModelFileName());

        if (_debugMode)
        {
            LAppPal::PrintLogLn("[APP]create model: %s", setting->GetModelFileName());
        }

        buffer = CreateBuffer(path.GetRawString(), &size);
        if (buffer != NULL)
        {
            const auto release = qScopeGuard([&] { DeleteBuffer(buffer, path.GetRawString()); });
            const bool loaded = LoadModel(buffer, size, _mocConsistency);
            if (!loaded) return false;
            m_sourceMocBytes = QByteArray(reinterpret_cast<const char*>(buffer), size);
            m_sourceMocDigest = QCryptographicHash::hash(m_sourceMocBytes, QCryptographicHash::Sha256);
            m_previewMocDigest = m_sourceMocDigest;
            m_previewMocBytes = m_sourceMocBytes;
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
            const csmString path = ResourcePath(_modelHomeDir, _modelSetting->GetExpressionFileName(i));

            buffer = CreateBuffer(path.GetRawString(), &size);
            if (buffer == NULL)
            {
                continue;
            }
            ACubismMotion* motion = LoadExpression(buffer, size, name.GetRawString());

            if (motion && !ConfigureExpressionReferences(static_cast<CubismExpressionMotion*>(motion), QString::fromUtf8(path.GetRawString()))) {
                ACubismMotion::Delete(motion);
                motion = nullptr;
            }
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
        const csmString path = ResourcePath(_modelHomeDir, _modelSetting->GetPhysicsFileName());

        QString physicsError;
        if (!ReloadPhysicsFromFile(QString::fromUtf8(path.GetRawString()), &physicsError)) {
            LAppPal::PrintLogLn("[APP]Failed to load physics: %s", physicsError.toUtf8().constData());
            return false;
        }
    }

    //Pose
    if (strcmp(_modelSetting->GetPoseFileName(), "") != 0)
    {
        const csmString path = ResourcePath(_modelHomeDir, _modelSetting->GetPoseFileName());

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
        const csmString path = ResourcePath(_modelHomeDir, _modelSetting->GetUserDataFile());
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
        const csmString path = ResourcePath(_modelHomeDir, _modelSetting->GetMotionFileName(group, i));

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
            if (!ConfigureMotionReferences(tmpMotion, QString::fromUtf8(path.GetRawString()))) {
                ACubismMotion::Delete(tmpMotion);
                DeleteBuffer(buffer, path.GetRawString());
                continue;
            }

            if (_motions[name] != NULL)
            {
                ACubismMotion::Delete(_motions[name]);
            }
            _motions[name] = tmpMotion;
        }

        DeleteBuffer(buffer, path.GetRawString());
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

CubismMotionQueueEntryHandle LAppModel::StartMotion(const csmChar* group, csmInt32 no, csmInt32 priority, ACubismMotion::FinishedMotionCallback onFinishedMotionHandler)
{
    if (HasExportPreview() || _updating) return InvalidMotionQueueEntryHandleValue;
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
        const csmString path = ResourcePath(_modelHomeDir, fileName.GetRawString());

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
            if (!ConfigureMotionReferences(motion, QString::fromUtf8(path.GetRawString()))) {
                ACubismMotion::Delete(motion);
                DeleteBuffer(buffer, path.GetRawString());
                return InvalidMotionQueueEntryHandleValue;
            }
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
        const csmString path = ResourcePath(_modelHomeDir, voice.GetRawString());
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
    if (HasExportPreview() || (_initialized && _updating)) return;
    if (!std::isfinite(value)) return;
    if (value < 0.25f) value = 0.25f;
    if (value > 2.0f) value = 2.0f;
    _motionSpeed = value;
}

void LAppModel::SetMotionLoop(bool value)
{
    if (HasExportPreview() || (_initialized && _updating)) return;
    _motionLoop = value;
    for (csmMap<csmString, ACubismMotion*>::const_iterator iter = _motions.Begin(); iter != _motions.End(); ++iter) {
        if (iter->Second) static_cast<CubismMotion*>(iter->Second)->IsLoop(value);
    }
}

CubismMotionQueueEntryHandle LAppModel::StartRandomMotion(const csmChar* group, csmInt32 priority, ACubismMotion::FinishedMotionCallback onFinishedMotionHandler)
{
    if (HasExportPreview() || _updating) return InvalidMotionQueueEntryHandleValue;
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
    if (HasExportPreview() || (_initialized && _updating)) return;
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
    if (HasExportPreview() || (_initialized && _updating)) return;
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
    if (HasExportPreview() || (_initialized && _updating)) return false;
    const QString resolved = CModelResourcePath::Resolve(QString::fromUtf8(_modelHomeDir.GetRawString()), path);
    if (resolved.isEmpty()) return false;
    if (expressionId.isEmpty() || path.isEmpty()) return false;
    QFile file(resolved);
    if (!file.open(QIODevice::ReadOnly)) return false;
    const QByteArray data = file.readAll();
    if (file.error() != QFileDevice::NoError || data.isEmpty() ||
        data.size() > std::numeric_limits<csmSizeInt>::max()) return false;
    const QByteArray nameBytes = expressionId.toUtf8();
    ACubismMotion* replacement = LoadExpression(
        reinterpret_cast<const csmByte*>(data.constData()),
        static_cast<csmSizeInt>(data.size()), nameBytes.constData());
    if (!replacement) return false;
    const csmString key(nameBytes.constData());
    auto* previous = static_cast<CubismExpressionMotion*>(_expressions[key]);
    if ((previous && m_parameterReferences.expressions.contains(previous->ReferenceSourcePath()) &&
         previous->ReferenceSourcePath() != ReferenceSourcePath(resolved)) ||
        !ConfigureExpressionReferences(static_cast<CubismExpressionMotion*>(replacement), resolved)) {
        ACubismMotion::Delete(replacement);
        return false;
    }
    if (_expressionManager) _expressionManager->StopAllMotions();
    if (_expressions[key] != NULL) ACubismMotion::Delete(_expressions[key]);
    _expressions[key] = replacement;
    return true;
}

void LAppModel::SetRandomExpression()
{
    if (HasExportPreview() || (_initialized && _updating)) return;
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

bool LAppModel::ApplyPreviewMoc(const QByteArray& bytes, const QByteArray& sourceDigest, QString* error)
{
    if (HasExportPreview()) {
        if (error) *error = QStringLiteral("导出期间不能替换模型预览");
        return false;
    }
    if (HasEditorPreview()) return ApplyEditorPreview(bytes, sourceDigest, m_editorPose, error);
    if (error) error->clear();
    if (!_initialized || _updating || m_physicsDragActive || !_model || sourceDigest.isEmpty() || sourceDigest != m_sourceMocDigest ||
        bytes.isEmpty() || bytes.size() > std::numeric_limits<int>::max()) {
        if (error) *error = QStringLiteral("预览数据与当前模型的加载快照不匹配");
        return false;
    }
    try {
        const QByteArray digest = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
        if (digest == m_previewMocDigest) return true;
        const QByteArray ownedBytes(bytes.constData(), bytes.size());
        return ApplyPreviewTransaction(ownedBytes, sourceDigest, {}, error);
    } catch (const std::exception&) {
        if (error) *error = QStringLiteral("无法保存预览 MOC 字节快照");
        return false;
    }
}

bool LAppModel::ApplyEditorPreview(const QByteArray& bytes, const QByteArray& sourceDigest,
                                  const QMap<QString, float>& pose, QString* error)
{
    if (error) error->clear();
    if (HasExportPreview()) {
        if (error) *error = QStringLiteral("导出期间不能改变编辑姿态");
        return false;
    }
    if (!_initialized || _updating || !_model || sourceDigest.isEmpty() || sourceDigest != m_sourceMocDigest ||
        bytes.isEmpty() || bytes.size() > std::numeric_limits<int>::max()) {
        if (error) *error = QStringLiteral("编辑姿态与当前模型的加载快照不匹配");
        return false;
    }
    try {
        const QByteArray ownedBytes(bytes.constData(), bytes.size());
        const QByteArray digest = QCryptographicHash::hash(ownedBytes, QCryptographicHash::Sha256);
        QMap<QString, float> frozenPose;
        for (auto it = pose.cbegin(); it != pose.cend(); ++it)
            frozenPose.insert(QString(it.key().constData(), it.key().size()), it.value());
        if (HasEditorPreview() && digest == m_previewMocDigest && frozenPose == m_editorPose) return true;
        std::unique_ptr<CubismModel::RuntimeValueSnapshot> entry;
        QMap<QString, float> baseParameters;
        if (!HasEditorPreview()) {
            entry = std::make_unique<CubismModel::RuntimeValueSnapshot>();
            if (!_model->CaptureRuntimeValues(*entry, error)) return false;
            baseParameters = entry->Parameters();
        }
        const auto prepareModel = [&](CubismModel* candidate, QString* preparationError) {
            if (!candidate->ApplyParameterPose(frozenPose, preparationError)) return false;
            ApplyPartVisibility(*candidate);
            return true;
        };
        if (!ApplyPreviewTransaction(ownedBytes, sourceDigest, prepareModel, error)) return false;
        if (entry) {
            m_editorEntryState = std::move(entry);
            m_editorBaseParameters.swap(baseParameters);
        }
        m_editorPose.swap(frozenPose);
        return true;
    } catch (const std::exception&) {
        if (error) *error = QStringLiteral("无法准备编辑姿态，原预览保持不变");
        return false;
    }
}

bool LAppModel::EndEditorPreview(QString* error)
{
    if (error) error->clear();
    if (HasExportPreview()) {
        if (error) *error = QStringLiteral("导出期间不能改变编辑姿态");
        return false;
    }
    if (!HasEditorPreview()) return true;
    try {
        const auto restoreValues = [this](CubismModel* candidate, QString* preparationError) {
            return candidate->RestoreRuntimeValues(*m_editorEntryState, preparationError);
        };
        if (!ApplyPreviewTransaction(m_previewMocBytes, m_sourceMocDigest, restoreValues, error)) return false;
        m_editorEntryState.reset();
        m_editorPose.clear();
        m_editorBaseParameters.clear();
        return true;
    } catch (const std::exception&) {
        if (error) *error = QStringLiteral("无法恢复编辑前状态，编辑姿态保持不变");
        return false;
    }
}

bool LAppModel::ApplyPreviewTransaction(const QByteArray& ownedBytes, const QByteArray& sourceDigest,
                                       const CubismModelReplacement::ModelPreparation& prepareModel, QString* error,
                                       CubismModelUpdatePolicy policy, const ParameterStructureSeeds& seeds,
                                       const ParameterPhysicsReferences* physicsReferences, const PhysicsDraft* physicsDraft)
{
    if (!_initialized || _updating || !_model || sourceDigest.isEmpty() || sourceDigest != m_sourceMocDigest ||
        ownedBytes.isEmpty() || ownedBytes.size() > std::numeric_limits<int>::max()) {
        if (error) *error = QStringLiteral("预览数据与当前模型的加载快照不匹配");
        return false;
    }
    const QByteArray digest = QCryptographicHash::hash(ownedBytes, QCryptographicHash::Sha256);
    auto* previous = GetRenderer<Rendering::CubismRenderer_OpenGLES2>();
    auto* delegate = LAppDelegate::GetInstance();
    if (!previous || delegate->IsOffScreen() || delegate->m_isDraggingDeformer ||
        !previous->setGlOwningContext(delegate->getGlOwningContext())) {
        if (error) *error = QStringLiteral("请在模型预览就绪且拖拽、导出结束后更新关键形");
        return false;
    }
    const auto prepare = [](Rendering::CubismRenderer* renderer, CubismModel* model,
                            const Rendering::CubismRenderer& source, QString* preparationError) {
        auto* candidate = dynamic_cast<Rendering::CubismRenderer_OpenGLES2*>(renderer);
        const auto* previous = dynamic_cast<const Rendering::CubismRenderer_OpenGLES2*>(&source);
        return candidate && previous && candidate->PrepareModelReplacement(model, *previous, preparationError);
    };
    const auto prepareCompiledModel = [&](CubismModel* candidate, QString* detail) {
        candidate->ClearDrawableIndicesOverrides();
        return !prepareModel || prepareModel(candidate, detail);
    };
    QByteArray draftBytes = physicsDraft ? physicsDraft->bytes : m_physicsDraftBytes;
    const bool draftActive = physicsDraft || m_hasPhysicsDraft;
    PhysicsPreparation preparePhysics;
    if (draftActive || !m_physicsSourceBytes.isEmpty() || _physics) {
        if (m_physicsGeneration == std::numeric_limits<quint64>::max()) {
            if (error) *error = QStringLiteral("物理实例版本计数已耗尽");
            return false;
        }
        preparePhysics = [&](const CubismModel& previousModel, const CubismModel& replacement, PhysicsOwner& result, QString* detail) {
            return PreparePhysicsCandidate(previousModel, replacement, physicsReferences ? *physicsReferences : m_parameterReferences.physics,
                draftActive, draftActive ? draftBytes : m_physicsSourceBytes, physicsDraft && physicsDraft->stabilize,
                !physicsDraft, result, detail);
        };
    }
    const bool hadPhysics = _physics != nullptr;
    if (!ReplaceModel(reinterpret_cast<const csmByte*>(ownedBytes.constData()), csmSizeInt(ownedBytes.size()),
                      prepare, policy | CubismModelUpdatePolicy::AllowDrawableTopologyEdits, seeds, error,
                      prepareCompiledModel, physicsReferences, preparePhysics))
        return false;
    if (preparePhysics && (hadPhysics || _physics)) ++m_physicsGeneration;
    if (physicsDraft) { m_physicsDraftBytes.swap(draftBytes); m_hasPhysicsDraft = true; }
    m_previewMocBytes = ownedBytes;
    m_previewMocDigest = digest;
    delegate->m_savedVertices.clear();
    delegate->m_meshOverlayPositions.clear();
    return true;
}

bool LAppModel::ReloadRenderer()
{
    if (HasExportPreview() || (_initialized && _updating)) return false;
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
        const QString path = CModelResourcePath::Resolve(QString::fromUtf8(_modelHomeDir.GetRawString()), QString::fromUtf8(textureName));
        if (path.isEmpty()) return false;
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
    if (!mocFileName || mocFileName[0] == '\0') return false;

    csmByte* buffer;
    csmSizeInt size;

    const csmString path = ResourcePath(_modelHomeDir, mocFileName);

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
    if (HasExportPreview() || (_initialized && _updating)) return;
    if(_physics && std::isfinite(x) && std::isfinite(y)){
        auto opt = _physics->GetOptions();
        opt.Gravity.X = x;
        opt.Gravity.Y = y;
        _physics->SetOptions(opt);
    }
}


void LAppModel::SetPhysicsDragParams(const QStringList& paramIds, const QStringList& types)
{
    if (HasExportPreview() || (_initialized && _updating)) return;
    EndPhysicsDrag();
    QStringList ids, acceptedTypes;
    QSet<QString> seen;
    for (qsizetype row = 0; row < paramIds.size() && row < types.size(); ++row) {
        if (paramIds[row].isEmpty() || seen.contains(paramIds[row]) ||
            (types[row] != QLatin1String("X") && types[row] != QLatin1String("Y") && types[row] != QLatin1String("Angle"))) continue;
        seen.insert(paramIds[row]);
        ids.append(paramIds[row]);
        acceptedTypes.append(types[row]);
    }
    _physicsDragParamIds.swap(ids);
    _physicsDragTypes.swap(acceptedTypes);
}

void LAppModel::ApplyPhysicsDrag(float dx, float dy)
{
    if (HasExportPreview() || HasEditorPreview() || _snapshotMode || _updating || !CanPhysicsDrag() ||
        !_model || !std::isfinite(dx) || !std::isfinite(dy)) return;
    m_physicsDragX = qBound(-1.0f, dx, 1.0f);
    m_physicsDragY = qBound(-1.0f, dy, 1.0f);
    m_physicsDragActive = true;
}

void LAppModel::EndPhysicsDrag() noexcept
{
    m_physicsDragActive = false;
    m_physicsDragX = m_physicsDragY = 0.0f;
}

void LAppModel::SetPhysicsWind(float x, float y)
{
    if (HasExportPreview() || (_initialized && _updating)) return;
    if(_physics && std::isfinite(x) && std::isfinite(y)){
        auto opt = _physics->GetOptions();
        opt.Wind.X = x;
        opt.Wind.Y = y;
        _physics->SetOptions(opt);
    }
}

bool LAppModel::StartLipSyncWav(const QString& path)
{
    if (HasExportPreview() || (_initialized && _updating)) return false;
    const QString resolved = CModelResourcePath::Resolve(QString::fromUtf8(_modelHomeDir.GetRawString()), path);
    if (resolved.isEmpty()) return false;
    if (path.isEmpty()) return false;
    const QByteArray utf8Path = resolved.toUtf8();
    return _wavFileHandler.Start(csmString(utf8Path.constData()));
}

void LAppModel::StopLipSyncWav()
{
    if (HasExportPreview() || (_initialized && _updating)) return;
    _wavFileHandler.Stop();
}

void LAppModel::SetPartVisible(const QString& partId, bool visible)
{
    if (HasExportPreview() || (_initialized && _updating)) return;
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
    if (HasExportPreview() || (_initialized && _updating)) return;
    m_deformerDrag.SetParameters(paramIds, minVals, maxVals, defaultVals);
}

void LAppModel::ApplyDeformerDrag(float viewX, float viewY)
{
    if (HasExportPreview() || (_initialized && _updating)) return;
    if (!_model) return;
    m_deformerDrag.Apply(viewX, viewY);
}

void LAppModel::ProbeParamsForArtMeshes(const QStringList& artMeshNames)
{
    if (HasExportPreview() || (_initialized && _updating)) return;
    if (!_model || _updating || HasEditorPreview() || artMeshNames.isEmpty()) return;
    const QScopedValueRollback<csmBool> updating(_updating, true);
    try {
        auto* model = _model;
        auto* core = model->GetModel();
        if (!core) return;
        const int parameterCount = model->GetParameterCount();
        const int drawableCount = model->GetDrawableCount();
        if (parameterCount < 0 || drawableCount < 0) return;
        auto* parameterValues = Live2D::Cubism::Core::csmGetParameterValues(core);
        if (parameterCount && !parameterValues) return;
        QVector<float> original;
        QVector<CDeformerDrag::Parameter> parameters;
        original.reserve(parameterCount);
        parameters.reserve(parameterCount);
        for (int index = 0; index < parameterCount; ++index) {
            const auto id = model->GetParameterId(index);
            if (!id) return;
            original.append(parameterValues[index]);
            parameters.append({QString::fromUtf8(id->GetString().GetRawString()),
                model->GetParameterMinimumValue(index), model->GetParameterMaximumValue(index),
                model->GetParameterDefaultValue(index)});
        }
        const QSet<QString> targetNames(artMeshNames.cbegin(), artMeshNames.cend());
        QVector<int> targets;
        for (int index = 0; index < drawableCount; ++index) {
            const auto id = model->GetDrawableId(index);
            if (id && targetNames.contains(QString::fromUtf8(id->GetString().GetRawString()))) targets.append(index);
        }
        const auto evaluate = [&](const QVector<float>& values, CDeformerDrag::Vertices& vertices, QString* error) {
            if (model != _model || values.size() != parameterCount) return false;
            if (parameterCount) std::copy(values.cbegin(), values.cend(), parameterValues);
            model->Update();
            const auto* counts = Live2D::Cubism::Core::csmGetDrawableVertexCounts(core);
            const auto* positions = Live2D::Cubism::Core::csmGetDrawableVertexPositions(core);
            if (drawableCount && (!counts || !positions)) return false;
            CDeformerDrag::Vertices candidate;
            candidate.reserve(drawableCount);
            for (int drawable = 0; drawable < drawableCount; ++drawable) {
                const int count = counts[drawable];
                if (count < 0 || count > std::numeric_limits<int>::max() / 2 || (count && !positions[drawable])) {
                    if (error) *error = QStringLiteral("参数探测网格缺少有效顶点");
                    return false;
                }
                QVector<float> points;
                points.reserve(count * 2);
                for (int vertex = 0; vertex < count; ++vertex) {
                    points.append(positions[drawable][vertex].X);
                    points.append(positions[drawable][vertex].Y);
                }
                candidate.append(std::move(points));
            }
            vertices.swap(candidate);
            return true;
        };
        const auto restore = [&](QString*) {
            if (model != _model) return false;
            if (parameterCount) std::copy(original.cbegin(), original.cend(), parameterValues);
            const auto exactValues = qScopeGuard([&] {
                if (parameterCount) std::copy(original.cbegin(), original.cend(), parameterValues);
            });
            model->Update();
            return true;
        };
        QString error;
        if (!m_deformerDrag.ProbeParameters(parameters, original, drawableCount, targets, evaluate, restore, &error)) {
            LAppPal::PrintLogLn("Parameter probing failed: %s", qPrintable(error));
        }
    } catch (const std::exception&) {
        LAppPal::PrintLogLn("Unable to prepare deformer drag bindings");
    }
}
