/**
 * Copyright(c) Live2D Inc. All rights reserved.
 *
 * Use of this source code is governed by the Live2D Open Software license
 * that can be found at https://www.live2d.com/eula/live2d-open-software-license-agreement_en.html.
 */

#include "LAppDelegate.hpp"
#include <iostream>
#include <QScopeGuard>
#include <QScopedValueRollback>

#include "LAppView.hpp"
#include "LAppPal.hpp"
#include "LAppDefine.hpp"
#include "LAppLive2DManager.hpp"
#include "LAppTextureManager.hpp"
#include "opengl/ctextureextractor.h"
#include "live2dstruct/util/cfilereader.h"
#include "live2dstruct/csectiontable.h"
#include "live2dstruct/ccanvasgeometry.h"
#include "ui/para/ccidjson.h"


using namespace Csm;
using namespace std;
using namespace LAppDefine;

namespace {
    LAppDelegate* s_instance = NULL;
}

LAppDelegate* LAppDelegate::GetInstance()
{
    if (s_instance == NULL)
    {
        s_instance = new LAppDelegate();
    }

    return s_instance;
}

void LAppDelegate::ReleaseInstance()
{
    if (s_instance != NULL)
    {
        delete s_instance;
    }

    s_instance = NULL;
}

bool LAppDelegate::Initialize(const QWidget* windows,const QString& strMoldeName)
{
    if (!windows || _view || _textureManager || !initializeOpenGLFunctions()) return false;

    _view = new LAppView();
    _textureManager = new LAppTextureManager();
    ResourcesPath = QCoreApplication::applicationDirPath()+"/Resources/";
    m_IsPrintALl =false;
    m_IsPrintFenzu=false;
    m_hiddenDrawables.clear();
    m_hiddenDeformerDrawables.clear();
    m_drawableOpacityOverrides.clear();
    m_drawableDrawOrderOverrides.clear();
    m_meshOverlayPositions.clear();
    m_meshOverlayKeyformIndices.clear();
    m_meshIndexOverrides.clear();
    m_dragTargetDrawables.clear();
    m_savedVertices.clear();
    m_isDraggingDeformer = false;

    _IsOffScreen =false;


    _windows = windows;
    _windowWidth = _windows->width();
    _windowHeight = _windows->height();
    _OffWidth = _windows->width();
    _OffHeight = _windows->height();




    _textureManager->setGlOwningContext(owningContext());
    if (!read.setGlOwningContext(owningContext()) || !coffscreen.setGlOwningContext(owningContext())) return false;
    if (DebugLogEnable)
    {
        LAppPal::PrintLogLn("START");
    }

    //透過設定 设置渲染的透明度
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    _view->Initialize();
    return InitializeCubism(strMoldeName);
}



void LAppDelegate::Release()
{
    ClearModelRuntimeState(true);
    LAppLive2DManager::ReleaseInstance();
    delete _view;
    _view = nullptr;
    delete _textureManager;
    _textureManager = nullptr;
    _windows = nullptr;
    if (CubismFramework::IsInitialized()) CubismFramework::Dispose();
    CubismFramework::CleanUp();
}



LAppDelegate::LAppDelegate():
    _cubismOption(),
    _view(nullptr),
    _windows(nullptr),
    _textureManager(nullptr),
    _windowWidth(0),
    _windowHeight(0),
    _IsOffScreen(false),
    _OffWidth(0),
    _OffHeight(0),
    cpsdwriteList(nullptr)
{

}

LAppDelegate::~LAppDelegate()
{

}

bool LAppDelegate::InitializeCubism(const QString &strMoldeName)
{

    //setup cubism
    _cubismOption.LogFunction = LAppPal::PrintMessage;
    _cubismOption.LoggingLevel = LAppDefine::CubismLoggingLevel;
    if (!Csm::CubismFramework::StartUp(&_cubismAllocator, &_cubismOption)) return false;
    CubismFramework::Initialize();
    if (!CubismFramework::IsInitialized()) return false;
    auto* manager = LAppLive2DManager::GetInstance();
    const bool loaded = strMoldeName.isEmpty() ? manager->ChangeScene(0) : manager->ChangeSceneFileName(strMoldeName);
    LAppPal::UpdateTime();
    return loaded;
}

bool LAppDelegate::IsRuntimeReady() const
{
    return _view && _textureManager && CubismFramework::IsInitialized();
}


LAppTextureManager *LAppDelegate::GetTextureManager()
{ return _textureManager; }

const QWidget *LAppDelegate::GetWindow()
{
    return _windows;
};

void LAppDelegate::setWindow(QWidget *windows)
{
    _windows = windows;
}

QOpenGLContext *LAppDelegate::getGlOwningContext()
{
    return owningContext();
}

int LAppDelegate::width()
{
    if(_IsOffScreen){
        return _OffWidth;
    }else{
        return _windows->width();
    }

}

int LAppDelegate::height()
{
    if(_IsOffScreen){
        //return _OffHeight;
        return _OffHeight;
    }else{
        return _windows->height();
    }

}

void LAppDelegate::paint()
{

    LAppPal::UpdateTime();
    glClearColor(m_clearColor[0], m_clearColor[1], m_clearColor[2], m_clearColor[3]);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glClearDepth(1.0);
    _view->Render();

}

void LAppDelegate::resize(int width, int height)
{

    if(_IsOffScreen){
        glViewport(0, 0,_OffWidth,_OffHeight);//离屏幕渲染高, height);
    }else{
        if((_windowWidth!=width || _windowHeight!=height) && width>0 && height>0)
        {
            InitOffset(width,height);
        }else{
            glViewport(0, 0, width, height);
        }
    }
}



void LAppDelegate::SetOffsetWidthANDHeight(int width, int height)
{
    _OffWidth = width;
    _OffHeight = height;

}

bool LAppDelegate::IsOffScreen()
{
    return _IsOffScreen;
}

void LAppDelegate::SetIsOffset(bool f)
{
    _IsOffScreen =f;
}


void LAppDelegate::InitOffset(int width, int height)
{
    _view->Initialize();
    _view->ResizeSprite();
    _windowWidth = width;
    _windowHeight = height;
    glViewport(0, 0, width, height);

}


GLuint LAppDelegate::getTexFrame()
{
    return coffscreen.GetColorBuffer();
}





CPsdWriteList *LAppDelegate::GetCPsdWriteList()
{
    return cpsdwriteList;
}


bool LAppDelegate::StartOffset_FunpsdList(const QString &name,bool IsALl,bool IsFenzu,bool cubismCompat)
{
    if (!IsRuntimeReady() || name.isEmpty() || _IsOffScreen || cpsdwriteList ||
        QOpenGLContext::currentContext() != owningContext()) return false;
    const QScopedValueRollback<bool> printAll(m_IsPrintALl, IsALl);
    const QScopedValueRollback<bool> printGroups(m_IsPrintFenzu, IsFenzu);
    LAppLive2DManager* Live2DManager = LAppLive2DManager::GetInstance();
    if (!Live2DManager || Live2DManager->GetModelNum() == 0) return false;
    LAppModel* modelr = Live2DManager->GetModel(0);
    if (!modelr || !modelr->GetModel() || !modelr->GetModelMatrix()) return false;
    Live2D::Cubism::Core::csmVector2 sizeInPixels{}, originInPixels{};
    csmFloat32 pixelsPerUnit = 0.0f;
    Live2D::Cubism::Core::csmReadCanvasInfo(modelr->GetModel()->GetModel(), &sizeInPixels, &originInPixels, &pixelsPerUnit);
    const QSize canvasSize = live2dstruct::CCanvasGeometry::PixelSize(sizeInPixels.X, sizeInPixels.Y);
    std::array<float, 16> canvasProjection;
    if (!live2dstruct::CCanvasGeometry::ModelToNdc(sizeInPixels.X, sizeInPixels.Y,
            originInPixels.X, originInPixels.Y, pixelsPerUnit, canvasSize, canvasProjection)) return false;
    const int width = canvasSize.width(), height = canvasSize.height();
    CPsdWriteList writer;
    const QScopedValueRollback<CPsdWriteList*> psdWriter(cpsdwriteList, &writer);
    QString strhome = modelr->GetModelHomeDir().GetRawString();
    QString strcid =modelr->GetModelSetting()->GetDisplayInfoFileName();
    QString strmoc=modelr->GetModelSetting()->GetModelFileName();
    cpsdwriteList->SetCidName(strhome+"/"+strcid);
    cpsdwriteList->SetMocName(strhome+"/"+strmoc);
    if (!cpsdwriteList->CreatePsd(name,width,height,cubismCompat)) return false;
    const auto closePsd = qScopeGuard([&] { writer.ClosePsd(); });
    if (!coffscreen.CreateOffscreenSurface(width,height)) return false;
    const auto destroySurface = qScopeGuard([&] { coffscreen.DestroyOffscreenSurface(); });
    const auto projection = _view->GetProjectionMatrix();
    const auto viewMatrix = _view->GetViewMatrix();
    const auto modelMatrix = *modelr->GetModelMatrix();
    const QScopedValueRollback<bool> offscreen(_IsOffScreen);
    const QScopedValueRollback<int> offWidth(_OffWidth), offHeight(_OffHeight);
    GLint viewport[4];
    glGetIntegerv(GL_VIEWPORT, viewport);
    const auto restoreView = qScopeGuard([&] {
        *modelr->GetModelMatrix() = modelMatrix;
        _view->SetProjectionMatrix(projection);
        _view->SetViewMatrix(viewMatrix);
        glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    });
    if (!coffscreen.BeginDraw()) return false;
    bool drawing = true;
    const auto endDraw = qScopeGuard([&] { if (drawing) coffscreen.EndDraw(); });
    SetIsOffset(true);//开始渲染
    SetOffsetWidthANDHeight(width,height);
    CubismMatrix44 exportProjection;
    exportProjection.SetMatrix(canvasProjection.data());
    _view->SetProjectionMatrix(exportProjection);
    _view->SetViewMatrix(QMatrix4x4());
    modelr->GetModelMatrix()->LoadIdentity();
    glViewport(0, 0, width, height);
    LAppPal::UpdateTime();
    if (!_view->Render() || !coffscreen.EndDraw()) return false;
    drawing = false;
    return cpsdwriteList->WritePsd();
}

bool LAppDelegate::StartTextureExtraction(const QString& psdPath, bool includeHidden, bool groupByParts, bool cubismCompat)
{
    if (!IsRuntimeReady() || psdPath.isEmpty()) return false;
    LAppLive2DManager* live2DManager = LAppLive2DManager::GetInstance();
    LAppModel* model = live2DManager->GetModel(0);
    if (!model) {
        qWarning("StartTextureExtraction: 没有加载模型");
        return false;
    }

    QString homeDir = model->GetModelHomeDir().GetRawString();
    QString cidFileName = model->GetModelSetting()->GetDisplayInfoFileName();
    QString mocFileName = model->GetModelSetting()->GetModelFileName();
    QString cidPath = homeDir + "/" + cidFileName;
    QString mocPath = homeDir + "/" + mocFileName;

    // 获取纹理路径
    QMap<int, QString>& texMap = _textureManager->GetModelTex();
    QVector<QString> atlasPaths;
    for (auto it = texMap.begin(); it != texMap.end(); ++it) {
        // 确保按索引顺序添加
        while (atlasPaths.size() <= it.key()) {
            atlasPaths.append(QString());
        }
        atlasPaths[it.key()] = it.value();
    }

    // 加载纹理图集
    CTextureExtractor extractor;
    if (!extractor.LoadAtlases(atlasPaths)) {
        qWarning("StartTextureExtraction: 纹理加载失败");
        return false;
    }

    // 解析 moc3
    live2dstruct::CReader reader(mocPath);
    live2dstruct::CSectionTable sectionTable(reader);
    if (!sectionTable.IsValid()) {
        qWarning("StartTextureExtraction: moc3 解析失败: %s", reader.Error().toUtf8().constData());
        return false;
    }

    // 获取画布尺寸
    QSize canvasSize = CTextureExtractor::GetCanvasPixelSize(sectionTable.canvas_info);
    int canvasW = canvasSize.width();
    int canvasH = canvasSize.height();
    if (canvasW <= 0 || canvasH <= 0) {
        qWarning("StartTextureExtraction: 画布尺寸无效");
        return false;
    }

    CPsdWriteList writer;
    if (!writer.CreatePsd(psdPath, canvasW, canvasH, cubismCompat)) return false;

    // 提取所有图层
    QVector<ExtractedLayer> layers = extractor.ExtractAll(&sectionTable, includeHidden, &m_meshIndexOverrides);

    // 解析 CDI 显示名称
    CCidJson cid;
    cid.Parse(cidPath);

    CubismModel* runtimeModel = model->GetModel();
    if (!runtimeModel) return false;
    QHash<QString, const ExtractedLayer*> layersById;
    for (const ExtractedLayer& layer : layers) layersById.insert(layer.name, &layer);
    QVector<int> sortedIndices;
    for (int index : runtimeModel->GetSortedDrawableIndices(
             m_drawableDrawOrderOverrides, m_drawableOrderGroupIndices, m_drawOrderGroups)) {
        const auto id = runtimeModel->GetDrawableId(index);
        if (id && layersById.contains(QString::fromUtf8(id->GetString().GetRawString()))) sortedIndices.append(index);
    }
    const auto writeLayer = [&](int index) {
        const auto id = runtimeModel->GetDrawableId(index);
        if (!id) return;
        const auto* layer = layersById.value(QString::fromUtf8(id->GetString().GetRawString()), nullptr);
        if (!layer) return;
        QString displayName = cid.getPartName(layer->name);
        if (displayName.isEmpty()) displayName = layer->name;
        const float opacity = m_drawableOpacityOverrides.value(layer->name, layer->opacity);
        writer.AddLayer(displayName, layer->canvasRect, layer->image,
                        static_cast<uint8_t>(qBound(0.0f, opacity * 255.0f, 255.0f)),
                        CTextureExtractor::BlendModeToPsd(layer->drawableFlags),
                        !layer->maskIndices.isEmpty(), false);
    };

    if (groupByParts) {
        writer.SetMocName(mocPath);
        writer.SetCidName(cidPath);
        const auto order = writer.GetSortDrawableIndexList(sortedIndices, runtimeModel);
        const auto& names = writer.GetArtToName();
        for (int i = 0; i < order.size(); ++i) {
            if (order[i].second == CPsdWriteList::GroupStart) writer.AddGroupStart(names[i]);
            else if (order[i].second == CPsdWriteList::GroupEnd) writer.AddGroupEnd();
            else writeLayer(order[i].first);
        }
    } else {
        for (int index : sortedIndices) writeLayer(index);
    }

    bool ok = writer.WritePsd();
    writer.ClosePsd();
    return ok;
}

bool LAppDelegate::ChangeScene(const QString &modelJsonName)
{
    if (!IsRuntimeReady() || !LAppLive2DManager::GetInstance()->ChangeSceneFileName(modelJsonName)) return false;
    ClearModelRuntimeState(true);
    return true;
}

bool LAppDelegate::BeginTemporaryScene(const QString& modelJsonName)
{
    if (!IsRuntimeReady() || !LAppLive2DManager::GetInstance()->BeginTemporaryScene(modelJsonName)) return false;
    ClearModelRuntimeState(true);
    return true;
}

bool LAppDelegate::EndTemporaryScene()
{
    if (!IsRuntimeReady() || !LAppLive2DManager::GetInstance()->EndTemporaryScene()) return false;
    ClearModelRuntimeState(true);
    return true;
}

void LAppDelegate::ClearModelRuntimeState(bool clearOrderGroups)
{
    m_hiddenDrawables.clear();
    m_hiddenDeformerDrawables.clear();
    m_drawableOpacityOverrides.clear();
    m_drawableDrawOrderOverrides.clear();
    m_meshOverlayPositions.clear();
    m_meshOverlayKeyformIndices.clear();
    m_meshIndexOverrides.clear();
    m_dragTargetDrawables.clear();
    m_savedVertices.clear();
    m_isDraggingDeformer = false;
    if (clearOrderGroups) {
        m_drawableOrderGroupIndices.clear();
        m_drawOrderGroups.clear();
    }
}

//通过文件直接打开，如果已经打开场景就

void LAppDelegate::StartDeformerDrag()
{
    m_isDraggingDeformer = true;
    m_savedVertices.clear();
    // 保存所有非目标 drawable 的当前顶点
    LAppLive2DManager* mgr = LAppLive2DManager::GetInstance();
    if(!mgr || mgr->GetModelNum() <= 0 || !mgr->GetModel(0) || !mgr->GetModel(0)->GetModel()) return;
    auto* model = mgr->GetModel(0)->GetModel();
    int drawCount = model->GetDrawableCount();
    for(int i = 0; i < drawCount; i++){
        QString name(model->GetDrawableId(i)->GetString().GetRawString());
        if(m_dragTargetDrawables.contains(name)) continue;
        int vCount = model->GetDrawableVertexCount(i);
        if(vCount <= 0) continue;
        auto* verts = model->GetDrawableVertexPositions(i);
        std::vector<float> buf(vCount * 2);
        for(int v = 0; v < vCount; v++){
            buf[v * 2]     = verts[v].X;
            buf[v * 2 + 1] = verts[v].Y;
        }
        m_savedVertices[i] = std::move(buf);
    }
}

void LAppDelegate::StopDeformerDrag()
{
    m_isDraggingDeformer = false;
    m_savedVertices.clear();
    // 恢复参数
    LAppLive2DManager* mgr = LAppLive2DManager::GetInstance();
    if(mgr && mgr->GetModelNum() > 0 && mgr->GetModel(0)){
        mgr->GetModel(0)->StopDeformerDrag();
    }
}
