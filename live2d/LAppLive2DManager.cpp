/**
 * Copyright(c) Live2D Inc. All rights reserved.
 *
 * Use of this source code is governed by the Live2D Open Software license
 * that can be found at https://www.live2d.com/eula/live2d-open-software-license-agreement_en.html.
 */

#include "LAppLive2DManager.hpp"
#include <stdio.h>
#include <stdlib.h>
#ifdef Q_OS_WIN
#include <io.h>
#endif

#include <framework/Rendering/CubismRenderer.hpp>
#include "LAppPal.hpp"
#include "LAppDefine.hpp"
#include "LAppDelegate.hpp"
#include "LAppModel.hpp"
#include "LAppTextureManager.hpp"
#include "LAppView.hpp"
#include <QCoreApplication>
#include <algorithm>
#include <memory>
using namespace Csm;
using namespace LAppDefine;
using namespace std;

namespace {
    LAppLive2DManager* s_instance = NULL;

    void FinishedMotion(ACubismMotion* self)
    {
        LAppPal::PrintLogLn("Motion Finished: %x", self);
    }

    int CompareCsmString(const void* a, const void* b)
    {
        return strcmp(reinterpret_cast<const Csm::csmString*>(a)->GetRawString(),
            reinterpret_cast<const Csm::csmString*>(b)->GetRawString());
    }
}

LAppLive2DManager* LAppLive2DManager::GetInstance()
{
    if (s_instance == NULL)
    {
        s_instance = new LAppLive2DManager();
    }

    return s_instance;
}

void LAppLive2DManager::ReleaseInstance()
{
    if (s_instance != NULL)
    {
        delete s_instance;
    }

    s_instance = NULL;
}

LAppLive2DManager::LAppLive2DManager()
    : _viewMatrix(NULL)
    , _sceneIndex(0)
{
    _viewMatrix = new CubismMatrix44();
    SetUpModel();
}

LAppLive2DManager::~LAppLive2DManager()
{
    ReleaseAllModel();
    delete _viewMatrix;
}

void LAppLive2DManager::ReleaseAllModel()
{
    _models.clear();
    m_suspendedModels.clear();
    m_suspendedSceneIndex = 0;
    m_temporaryScene = false;
}

void LAppLive2DManager::SetUpModel()
{
    _modelDir.Clear();
    QString ResourcesPath = LAppDelegate::GetInstance()->getResourcesPath();
    QDir dir(ResourcesPath);
    QDir::Filters filters = QDir::Dirs | QDir::NoDotAndDotDot;
    QFileInfoList subDirs = dir.entryInfoList(filters);
    foreach (const QFileInfo &subDir, subDirs) {
        QString filename = subDir.fileName();
        _modelDir.PushBack(csmString(filename.toStdString().c_str()));
    }
    qsort(_modelDir.GetPtr(), _modelDir.GetSize(), sizeof(csmString), CompareCsmString);
}

csmVector<csmString> LAppLive2DManager::GetModelDir() const
{
    return _modelDir;
}

csmInt32 LAppLive2DManager::GetModelDirSize() const
{
    return _modelDir.GetSize();
}

LAppModel* LAppLive2DManager::GetModel(csmUint32 no) const
{
    if (no < _models.size())
    {
        return _models[no].get();
    }

    return NULL;
}

void LAppLive2DManager::OnDrag(csmFloat32 x, csmFloat32 y) const
{
    for (csmUint32 i = 0; i < _models.size(); i++)
    {
        LAppModel* model = GetModel(i);
        if (model && model->IsInitialized()) model->SetDragging(x, y);
    }
}

void LAppLive2DManager::OnTap(csmFloat32 x, csmFloat32 y)
{
    if (DebugLogEnable)
    {
        LAppPal::PrintLogLn("[APP]tap point: {x:%.2f y:%.2f}", x, y);
    }

    for (csmUint32 i = 0; i < _models.size(); i++)
    {
        if (!_models[i] || !_models[i]->IsInitialized() || _models[i]->HasTimelinePreview()) continue;
        if (_models[i]->HitTest(HitAreaNameHead, x, y))
        {
            if (DebugLogEnable)
            {
                LAppPal::PrintLogLn("[APP]hit area: [%s]", HitAreaNameHead);
            }
            _models[i]->SetRandomExpression();
        }
        else if (_models[i]->HitTest(HitAreaNameBody, x, y))
        {
            if (DebugLogEnable)
            {
                LAppPal::PrintLogLn("[APP]hit area: [%s]", HitAreaNameBody);
            }
            _models[i]->StartRandomMotion(MotionGroupTapBody, PriorityNormal, FinishedMotion);
        }
    }
}

bool LAppLive2DManager::OnUpdate(CubismMatrix44& projection) const
{

    const csmUint32 modelCount = LAppDelegate::GetInstance()->IsOffScreen()
        ? std::min(csmUint32(1), GetModelNum()) : GetModelNum();
    LAppView* view = LAppDelegate::GetInstance()->GetView();
    for (csmUint32 i = 0; i < modelCount; ++i)
    {
        LAppModel* model = GetModel(i);
        if (!model || !model->IsInitialized()) return false;
        if (view) view->PreModelDraw(*model);//设置离屏渲染缓冲帧
        model->Update();
        CubismMatrix44 modelProjection = projection;
        const bool drawn = model->Draw(modelProjection);
        if (view) view->PostModelDraw(*model);//删除离屏渲染缓冲区
        if (!drawn) return false;
    }
    return modelCount > 0;
}

void LAppLive2DManager::NextScene()
{
    if (GetModelDirSize() <= 0) return;
    csmInt32 no = (_sceneIndex + 1) % GetModelDirSize();
    ChangeScene(no);
}

bool LAppLive2DManager::ChangeScene(Csm::csmInt32 index)
{
    if (m_temporaryScene || index < 0 || index >= GetModelDirSize()) return false;
    const QString modelName = QString::fromUtf8(_modelDir[index].GetRawString());
    const QDir resources(LAppDelegate::GetInstance()->getResourcesPath());
    const QByteArray modelPath = (resources.absoluteFilePath(modelName) + QLatin1Char('/')).toUtf8();
    const QByteArray modelJsonName = (modelName + QStringLiteral(".model3.json")).toUtf8();
    auto primary = std::make_unique<LAppModel>();
    if (!primary->LoadAssets(modelPath.constData(), modelJsonName.constData())) return false;
    std::unique_ptr<LAppModel> secondary;
#if defined(USE_RENDER_TARGET) || defined(USE_MODEL_RENDER_TARGET)
    secondary = std::make_unique<LAppModel>();
    if (!secondary->LoadAssets(modelPath.constData(), modelJsonName.constData()) || !secondary->GetModelMatrix()) return false;
    secondary->GetModelMatrix()->TranslateX(0.2f);
#endif
    std::vector<std::unique_ptr<LAppModel>> candidates;
    candidates.reserve(secondary ? 2 : 1);
    candidates.push_back(std::move(primary));
    if (secondary) candidates.push_back(std::move(secondary));
    _models.swap(candidates);
    candidates.clear();
    _sceneIndex = index;
    CommitModelTextures();
    if (DebugLogEnable) LAppPal::PrintLogLn("[APP]model index: %d", _sceneIndex);
#if defined(USE_RENDER_TARGET)
    const LAppView::SelectTarget useRenderTarget = LAppView::SelectTarget_ViewFrameBuffer;
#elif defined(USE_MODEL_RENDER_TARGET)
    const LAppView::SelectTarget useRenderTarget = LAppView::SelectTarget_ModelFrameBuffer;
#else
    const LAppView::SelectTarget useRenderTarget = LAppView::SelectTarget_None;
#endif
    LAppView* view = LAppDelegate::GetInstance()->GetView();
    if (view) {
        view->SwitchRenderingTarget(useRenderTarget);
        view->SetRenderTargetClearColor(1.0f, 1.0f, 1.0f);
    }
    return true;
}

bool LAppLive2DManager::ChangeSceneFileName(const QString& modelJsonName)
{
    if (m_temporaryScene) return false;
    const QFileInfo fileInfo(modelJsonName);
    if (!fileInfo.exists() || !fileInfo.isFile()) return false;
    const QByteArray modelPath = (fileInfo.absolutePath() + QLatin1Char('/')).toUtf8();
    const QByteArray fileName = fileInfo.fileName().toUtf8();
    auto candidate = std::make_unique<LAppModel>();
    if (!candidate->LoadAssets(modelPath.constData(), fileName.constData())) return false;
    std::vector<std::unique_ptr<LAppModel>> candidates;
    candidates.push_back(std::move(candidate));
    _models.swap(candidates);
    candidates.clear();
    CommitModelTextures();
    LAppView* view = LAppDelegate::GetInstance()->GetView();
    if (view) {
        view->SwitchRenderingTarget(LAppView::SelectTarget_None);
        view->SetRenderTargetClearColor(1.0f, 1.0f, 1.0f);
    }
    return true;
}

bool LAppLive2DManager::BeginTemporaryScene(const QString& modelJsonName)
{
    if (m_temporaryScene) return false;
    const QFileInfo fileInfo(modelJsonName);
    if (!fileInfo.exists() || !fileInfo.isFile()) return false;
    const QByteArray modelPath = (fileInfo.absolutePath() + QLatin1Char('/')).toUtf8();
    const QByteArray fileName = fileInfo.fileName().toUtf8();
    auto candidate = std::make_unique<LAppModel>();
    if (!candidate->LoadAssets(modelPath.constData(), fileName.constData())) return false;
    std::vector<std::unique_ptr<LAppModel>> candidates;
    candidates.push_back(std::move(candidate));
    m_suspendedModels.swap(_models);
    _models.swap(candidates);
    m_suspendedSceneIndex = _sceneIndex;
    m_temporaryScene = true;
    CommitModelTextures();
    return true;
}

bool LAppLive2DManager::EndTemporaryScene()
{
    if (!m_temporaryScene) return false;
    _models.clear();
    _models.swap(m_suspendedModels);
    _sceneIndex = m_suspendedSceneIndex;
    m_suspendedSceneIndex = 0;
    m_temporaryScene = false;
    CommitModelTextures();
    return true;
}

void LAppLive2DManager::CommitModelTextures()
{
    if (auto* view = LAppDelegate::GetInstance()->GetView()) view->InitializeSprite();
    auto* textures = LAppDelegate::GetInstance()->GetTextureManager();
    if (!textures) return;
    textures->SetModelTextures(!_models.empty() ? _models[0]->GetTexturePaths() : QMap<int, QString>());
    QSet<QString> retained;
    for (const auto& model : _models) {
        for (const QString& path : model->GetTexturePaths()) retained.insert(path);
        model->CommitTextureCache();
    }
    for (const auto& model : m_suspendedModels)
        for (const QString& path : model->GetTexturePaths()) retained.insert(path);
    textures->ReleaseUnusedTextures(retained);
}

csmUint32 LAppLive2DManager::GetModelNum() const
{
    return static_cast<csmUint32>(_models.size());
}

void LAppLive2DManager::SetViewMatrix(CubismMatrix44* m)
{
    if (!_viewMatrix || !m) return;
    for (int i = 0; i < 16; i++) {
        _viewMatrix->GetArray()[i] = m->GetArray()[i];
    }
}
