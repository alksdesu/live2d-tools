/**
 * Copyright(c) Live2D Inc. All rights reserved.
 *
 * Use of this source code is governed by the Live2D Open Software license
 * that can be found at https://www.live2d.com/eula/live2d-open-software-license-agreement_en.html.
 */

#include "LAppDelegate.hpp"
#include <iostream>

#include "LAppView.hpp"
#include "LAppPal.hpp"
#include "LAppDefine.hpp"
#include "LAppLive2DManager.hpp"
#include "LAppTextureManager.hpp"
#include "opengl/ctextureextractor.h"
#include "live2dstruct/util/cfilereader.h"
#include "live2dstruct/csectiontable.h"
#include "live2dstruct/tree/c2dtree.h"
#include "live2dstruct/tree/cartmeshnode.h"
#include "live2dstruct/tree/cpartsnode.h"
#include "ui/para/ccidjson.h"
#include <functional>


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
    initializeOpenGLFunctions();

    _view = new LAppView();
    _textureManager = new LAppTextureManager();
    ResourcesPath = QCoreApplication::applicationDirPath()+"/Resources/";
    m_IsPrintALl =false;
    m_IsPrintFenzu=false;
    m_hiddenDrawables.clear();

    _IsOffScreen =false;


    _windows = windows;
    _windowWidth = _windows->width();
    _windowHeight = _windows->height();
    _OffWidth = _windows->width();
    _OffHeight = _windows->height();




    _textureManager->setGlOwningContext(owningContext());
    read.setGlOwningContext(owningContext());
    coffscreen.setGlOwningContext(owningContext());
    if (DebugLogEnable)
    {
        LAppPal::PrintLogLn("START");
    }

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);

    //透過設定 设置渲染的透明度
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    _view->Initialize();
    InitializeCubism(strMoldeName);

    return GL_TRUE;
}



void LAppDelegate::Release()
{
    delete _textureManager;
    delete _view;
    LAppLive2DManager::ReleaseInstance();
    CubismFramework::Dispose();
    CubismFramework::CleanUp();
}



LAppDelegate::LAppDelegate():
    _cubismOption(),
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

void LAppDelegate::InitializeCubism(const QString &strMoldeName)
{

    //setup cubism
    _cubismOption.LogFunction = LAppPal::PrintMessage;
    _cubismOption.LoggingLevel = LAppDefine::CubismLoggingLevel;
    Csm::CubismFramework::StartUp(&_cubismAllocator, &_cubismOption);
    CubismFramework::Initialize();
    LAppLive2DManager::GetInstance();//加载默认的场景
    if(strMoldeName==""){
        LAppLive2DManager::GetInstance()->ChangeScene(0);
    }else{
        LAppLive2DManager::GetInstance()->ChangeSceneFileName(strMoldeName);
    }
    CubismMatrix44 projection;
    LAppPal::UpdateTime();
    _view->InitializeSprite();
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
    m_IsPrintALl =IsALl;
    m_IsPrintFenzu=IsFenzu;
    cpsdwriteList = new CPsdWriteList();//要设置默认的帧缓冲，这样读取的时候才会读取默认位置
    LAppLive2DManager* Live2DManager = LAppLive2DManager::GetInstance();
    LAppModel* modelr = Live2DManager->GetModel(0);
    int width =  modelr->GetModel()->GetCanvasWidthPixel();
    int height =  modelr->GetModel()->GetCanvasHeightPixel();
    QString strhome = modelr->GetModelHomeDir().GetRawString();
    QString strcid =modelr->GetModelSetting()->GetDisplayInfoFileName();
    QString strmoc=modelr->GetModelSetting()->GetModelFileName();
    cpsdwriteList->SetCidName(strhome+"/"+strcid);
    cpsdwriteList->SetMocName(strhome+"/"+strmoc);
    cpsdwriteList->CreatePsd(name,width,height,cubismCompat);
    coffscreen.CreateOffscreenSurface(width,height);
    coffscreen.BeginDraw();
    SetIsOffset(true);//开始渲染
    SetOffsetWidthANDHeight(width,height);
    InitOffset(width,height);
    paint();
    bool ok = cpsdwriteList->WritePsd();
    cpsdwriteList->ClosePsd();
    coffscreen.EndDraw();
    coffscreen.DestroyOffscreenSurface();

    delete cpsdwriteList;
    cpsdwriteList = nullptr;
    m_IsPrintALl =false;
    m_IsPrintFenzu=false;
    return ok;
}

bool LAppDelegate::StartTextureExtraction(const QString& psdPath, bool includeHidden, bool groupByParts, bool cubismCompat)
{
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

    // 提取所有图层
    QVector<ExtractedLayer> layers = extractor.ExtractAll(&sectionTable, includeHidden);

    // 解析 CDI 显示名称
    CCidJson cid;
    cid.Parse(cidPath);

    // 创建 PSD
    CPsdWriteList writer;
    writer.CreatePsd(psdPath, canvasW, canvasH, cubismCompat);

    if (groupByParts) {
        // 使用 C2DTree 构建 Part 层级
        writer.SetMocName(mocPath);
        writer.SetCidName(cidPath);

        // 构建 mesh name → 提取结果的映射
        QMap<QString, const ExtractedLayer*> layerMap;
        for (const ExtractedLayer& l : layers) {
            layerMap.insert(l.name, &l);
        }

        // 获取排序后的 drawable 列表（复用现有分组逻辑）
        // 需要构建一个 sortedDrawableIndexList（按渲染顺序）
        QVector<int> sortedIndices;
        for (int i = 0; i < layers.size(); ++i) {
            sortedIndices.append(layers[i].meshIndex);
        }

        // 利用已有的 C2DTree 分组排序
        live2dstruct::C2DTree tree(sectionTable);
        QMap<QString, int> weightMap;
        QMap<QString, int> nameToMeshIdx;
        for (int i = 0; i < layers.size(); ++i) {
            weightMap.insert(layers[i].name, i);
            nameToMeshIdx.insert(layers[i].name, i);
        }

        // 递归遍历 partRoot 写入 PSD
        // 使用 lambda 递归
        std::function<void(QList<live2dstruct::C2DTreeNode*>&)> writeTree;
        writeTree = [&](QList<live2dstruct::C2DTreeNode*>& nodes) {
            for (auto* node : nodes) {
                if (dynamic_cast<live2dstruct::CArtMeshNode*>(node)) {
                    QString meshName = node->getName();
                    if (layerMap.contains(meshName)) {
                        const ExtractedLayer* el = layerMap[meshName];
                        QString displayName = cid.getPartName(meshName);
                        if (displayName.isEmpty()) displayName = meshName;

                        uint32_t blendKey = CTextureExtractor::BlendModeToPsd(el->drawableFlags);
                        uint8_t opacityByte = static_cast<uint8_t>(qBound(0.0f, el->opacity * 255.0f, 255.0f));

                        writer.AddLayer(displayName, el->canvasRect, el->image,
                                       opacityByte, blendKey, !el->maskIndices.isEmpty(), false);
                    }
                } else if (dynamic_cast<live2dstruct::CPartsNode*>(node)) {
                    QString partName = node->getName();
                    QString displayName = cid.getPartName(partName);
                    if (displayName.isEmpty()) displayName = partName;

                    writer.AddGroupStart(displayName);
                    writeTree(node->getChilds());
                    writer.AddGroupEnd();
                }
            }
        };

        writeTree(tree.getPartRoot());
    } else {
        // 扁平模式：按索引顺序输出所有图层
        for (const ExtractedLayer& el : layers) {
            QString displayName = cid.getPartName(el.name);
            if (displayName.isEmpty()) displayName = el.name;

            uint32_t blendKey = CTextureExtractor::BlendModeToPsd(el.drawableFlags);
            uint8_t opacityByte = static_cast<uint8_t>(qBound(0.0f, el.opacity * 255.0f, 255.0f));

            writer.AddLayer(displayName, el.canvasRect, el.image,
                           opacityByte, blendKey, !el.maskIndices.isEmpty(), false);
        }
    }

    bool ok = writer.WritePsd();
    writer.ClosePsd();
    return ok;
}

void LAppDelegate::ChangeScene(const QString &modelJsonName)
{

    LAppLive2DManager* live2DManager = LAppLive2DManager::GetInstance();
    live2DManager->ChangeSceneFileName(modelJsonName);
}

//通过文件直接打开，如果已经打开场景就

void LAppDelegate::StartDeformerDrag()
{
    m_isDraggingDeformer = true;
    m_savedVertices.clear();
    // 保存所有非目标 drawable 的当前顶点
    LAppLive2DManager* mgr = LAppLive2DManager::GetInstance();
    if(!mgr || mgr->GetModelNum() <= 0) return;
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
    if(mgr && mgr->GetModelNum() > 0){
        mgr->GetModel(0)->StopDeformerDrag();
    }
}
