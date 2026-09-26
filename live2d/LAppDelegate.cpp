/**
 * Copyright(c) Live2D Inc. All rights reserved.
 *
 * Use of this source code is governed by the Live2D Open Software license
 * that can be found at https://www.live2d.com/eula/live2d-open-software-license-agreement_en.html.
 */

#include "std/cmodelresourcepath.h"
#include "LAppDelegate.hpp"
#include <iostream>
#include <QScopeGuard>
#include <QScopedValueRollback>
#include <cmath>
#include <exception>
#include <limits>

#include "LAppView.hpp"
#include "LAppPal.hpp"
#include "LAppDefine.hpp"
#include "LAppLive2DManager.hpp"
#include "LAppTextureManager.hpp"
#include "opengl/ctextureextractor.h"
#include "opengl/cpsdfiletask.h"
#include "opengl/cpsdrendersession.h"
#include "framework/Rendering/OpenGL/CubismRenderer_OpenGLES2.hpp"
#include "ui/cexporttaskrunner.h"
#include "std/cexportcontrol.h"
#include <QOpenGLWidget>
#include <QPointer>
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
    m_psdExportSession.reset();
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


bool LAppDelegate::StartOffset_FunpsdList(const QString& name, bool includeHidden, bool groups, bool cubismCompat,
                                        QString* error, CExportControl* control)
{
    CExportControl localControl;
    if (!control) control = &localControl;
    const auto fail = [&](const QString& reason) {
        control->Fail(reason);
        if (error) *error = control->Error().isEmpty() ? reason : control->Error();
        return false;
    };
    if (error) error->clear();
    if (!IsRuntimeReady() || name.isEmpty() || m_exportInProgress || m_psdExportSession || _IsOffScreen || cpsdwriteList ||
        QOpenGLContext::currentContext() != owningContext()) return fail(QStringLiteral("PSD 导出状态或所属上下文无效"));
    const QPointer<QOpenGLWidget> surface = qobject_cast<QOpenGLWidget*>(const_cast<QWidget*>(_windows));
    const QPointer<QOpenGLContext> context = owningContext();
    auto* manager = LAppLive2DManager::GetInstance();
    auto* model = manager && manager->GetModelNum() ? manager->GetModel(0) : nullptr;
    auto* renderer = model ? model->GetRenderer<Rendering::CubismRenderer_OpenGLES2>() : nullptr;
    if (!surface || !model || !model->GetModel() || !renderer || !model->HasExportPreview())
        return fail(QStringLiteral("PSD 导出需要已冻结的模型预览"));
    m_exportInProgress = true;
    const auto release = qScopeGuard([this, surface] { if (surface && s_instance == this) m_exportInProgress = false; });
    const auto detach = qScopeGuard([this, surface] {
        if (surface && s_instance == this && m_psdExportSession) m_psdExportSession->DetachControl();
    });
    if (!control->Report(CExportControl::Phase::Preparing)) { if (error) *error = control->Error(); return false; }
    model->Update();
    auto* core = model->GetModel();
    Live2D::Cubism::Core::csmVector2 pixels{}, origin{};
    csmFloat32 ppu = 0;
    Live2D::Cubism::Core::csmReadCanvasInfo(core->GetModel(), &pixels, &origin, &ppu);
    const QSize canvas = live2dstruct::CCanvasGeometry::PixelSize(pixels.X, pixels.Y);
    std::array<float, 16> projection;
    if (!live2dstruct::CCanvasGeometry::ModelToNdc(pixels.X, pixels.Y, origin.X, origin.Y, ppu, canvas, projection))
        return fail(QStringLiteral("PSD 画布尺寸、原点或缩放无效"));
    CubismMatrix44 matrix;
    matrix.SetMatrix(projection.data());
    const auto sorted = core->GetSortedDrawableIndices(m_drawableDrawOrderOverrides, m_drawableOrderGroupIndices, m_drawOrderGroups);
    QVector<CPsdRenderSession::Entry> entries;
    const auto appendLayer = [&](int index) {
        if (!includeHidden && !core->GetDrawableDynamicFlagIsVisible(index)) return;
        const QString id = QString::fromUtf8(core->GetDrawableId(index)->GetString().GetRawString());
        if (m_hiddenDrawables.contains(id) || m_hiddenDeformerDrawables.contains(id)) return;
        entries.append({CPsdExportItem::Type::Layer, index, id, m_drawableOpacityOverrides.value(id, -1.0f)});
    };
    if (groups) {
        const QString home = QString::fromUtf8(model->GetModelHomeDir().GetRawString());
        const QString mocPath = CModelResourcePath::Resolve(home, QString::fromUtf8(model->GetModelSetting()->GetModelFileName()));
        live2dstruct::CModelDocument document;
        QString sourceError;
        if (!document.LoadBytes(model->GetSourceMocBytes(), mocPath, &sourceError))
            return fail(QStringLiteral("PSD 分组源模型读取失败：%1").arg(sourceError));
        if (document.Drawables().size() != core->GetDrawableCount())
            return fail(QStringLiteral("PSD 分组源网格数量与运行时不一致"));
        QHash<QString, int> sourceIndices;
        for (int index = 0; index < document.Drawables().size(); ++index) {
            const QString& id = document.Drawables()[index].id;
            if (id.isEmpty() || sourceIndices.contains(id)) return fail(QStringLiteral("PSD 分组源网格身份无效或重复"));
            sourceIndices.insert(id, index);
        }
        QVector<int> sources;
        QHash<int, int> runtimeIndices;
        for (int index : sorted) {
            if (!control->Checkpoint()) { if (error) *error = control->Error(); return false; }
            const QString id = QString::fromUtf8(core->GetDrawableId(index)->GetString().GetRawString());
            const int sourceIndex = sourceIndices.value(id, -1);
            if (sourceIndex < 0 || runtimeIndices.contains(sourceIndex)) return fail(QStringLiteral("PSD 分组无法映射运行时网格：%1").arg(id));
            sources.append(sourceIndex);
            runtimeIndices.insert(sourceIndex, index);
        }
        CCidJson names;
        names.Parse(CModelResourcePath::Resolve(home, QString::fromUtf8(model->GetModelSetting()->GetDisplayInfoFileName())));
        const auto order = live2dstruct::BuildGroupedLayerOrder(document.Parts(), document.Drawables(), sources);
        if (!sorted.isEmpty() && order.isEmpty()) return fail(QStringLiteral("PSD 分组计划为空"));
        for (const auto& entry : order) {
            if (!control->Checkpoint()) { if (error) *error = control->Error(); return false; }
            if (entry.type == live2dstruct::CLayerOrderItem::Layer) appendLayer(runtimeIndices.value(entry.drawableIndex));
            else {
                const QString display = names.getPartName(entry.id);
                entries.append({entry.type == live2dstruct::CLayerOrderItem::GroupStart ? CPsdExportItem::Type::GroupStart
                                                                                     : CPsdExportItem::Type::GroupEnd,
                                 -1, display.isEmpty() ? entry.id : display, -1.0f});
            }
        }
    } else {
        for (int index : sorted) appendLayer(index);
    }
    auto candidate = std::make_unique<CPsdRenderSession>(surface, context, core, renderer, canvas, matrix, std::move(entries),
        [surface, context] {
            if (!surface || surface->context() != context) return false;
            surface->makeCurrent();
            return QOpenGLContext::currentContext() == context;
        }, [surface, context, model, core, renderer](QString* detail) {
            if (!surface || surface->context() != context) { if (detail) *detail = QStringLiteral("导出窗口的上下文已改变"); return false; }
            auto* active = LAppLive2DManager::GetInstance();
            if (!active || active->GetModel(0) != model || model->GetModel() != core ||
                model->GetRenderer<Rendering::CubismRenderer_OpenGLES2>() != renderer || !model->HasExportPreview()) {
                if (detail) *detail = QStringLiteral("导出模型或渲染器已改变");
                return false;
            }
            return true;
        }, control);
    if (!candidate->Begin(includeHidden, error)) return false;
    m_psdExportSession = std::move(candidate);
    auto* session = m_psdExportSession.get();
    const bool written = CPsdFileTask::Execute(name, canvas, cubismCompat,
        [&](CPsdExportItem& item, QString* detail) {
            if (!surface || s_instance != this || m_psdExportSession.get() != session) {
                if (detail) *detail = QStringLiteral("PSD 导出窗口或会话已关闭");
                return false;
            }
            return session->Next(item, detail);
        }, control, error);
    if (!surface || s_instance != this || m_psdExportSession.get() != session)
        return fail(QStringLiteral("PSD 导出窗口或会话已关闭"));
    QString cleanupError;
    const bool restored = session->Finish(written, &cleanupError);
    if (!restored) return fail(cleanupError);
    m_psdExportSession.reset();
    return written;
}

bool LAppDelegate::RecoverPsdExport(QString* error)
{
    if (error) error->clear();
    if (m_exportInProgress) {
        if (error) *error = QStringLiteral("PSD 导出任务仍在执行，暂不能恢复资源");
        return false;
    }
    if (!m_psdExportSession) return true;
    if (!m_psdExportSession->Finish(false, error)) return false;
    m_psdExportSession.reset();
    return true;
}

bool LAppDelegate::StartTextureExtraction(const QString& path, bool includeHidden, bool groups, bool cubismCompat,
                                         QString* error, CExportControl* control)
{
    CExportControl localControl;
    if (!control) control = &localControl;
    if (error) error->clear();
    const auto fail = [&](const QString& reason) {
        control->Fail(reason);
        if (error) *error = control->Error().isEmpty() ? reason : control->Error();
        return false;
    };
    if (!IsRuntimeReady() || path.isEmpty() || m_exportInProgress || m_psdExportSession)
        return fail(QStringLiteral("纹理导出尚未就绪或上次 GPU 资源尚未恢复"));
    auto* manager = LAppLive2DManager::GetInstance();
    auto* model = manager && manager->GetModelNum() ? manager->GetModel(0) : nullptr;
    if (!model || !model->GetModel() || !model->HasExportPreview()) return fail(QStringLiteral("纹理导出需要已冻结的模型预览"));
    const QPointer<QWidget> surface = const_cast<QWidget*>(_windows);
    m_exportInProgress = true;
    const auto release = qScopeGuard([this, surface] { if (surface && s_instance == this) m_exportInProgress = false; });
    const QString home = QString::fromUtf8(model->GetModelHomeDir().GetRawString());
    const QString cidPath = CModelResourcePath::Resolve(home, QString::fromUtf8(model->GetModelSetting()->GetDisplayInfoFileName()));
    const QString mocPath = CModelResourcePath::Resolve(home, QString::fromUtf8(model->GetModelSetting()->GetModelFileName()));
    const QByteArray source = model->GetPreviewMocBytes();
    if (source.isEmpty()) return fail(QStringLiteral("当前模型缺少有效 MOC 快照，无法提取纹理"));
    const auto topology = m_meshIndexOverrides;
    const auto opacity = m_drawableOpacityOverrides;
    const QSet<QString> hidden = m_hiddenDrawables | m_hiddenDeformerDrawables;
    QVector<QString> atlases;
    const auto texturePaths = _textureManager->GetModelTex();
    for (auto texture = texturePaths.cbegin(); texture != texturePaths.cend(); ++texture) {
        while (atlases.size() <= texture.key()) atlases.append(QString());
        atlases[texture.key()] = texture.value();
    }
    QStringList order;
    QHash<QString, QVector<float>> modelPositions;
    model->Update();
    auto* runtime = model->GetModel();
    for (int index = 0; index < runtime->GetDrawableCount(); ++index) {
        const auto id = QString::fromUtf8(runtime->GetDrawableId(index)->GetString().GetRawString());
        const int count = runtime->GetDrawableVertexCount(index);
        const auto* positions = runtime->GetDrawableVertices(index);
        if (id.isEmpty() || modelPositions.contains(id) || count < 0 || count > 1000000 || (count && !positions))
            return fail(QStringLiteral("当前网格坐标快照无效：%1").arg(id));
        QVector<float> copy;
        copy.reserve(count * 2);
        for (int coordinate = 0; coordinate < count * 2; ++coordinate) {
            if (!std::isfinite(positions[coordinate])) return fail(QStringLiteral("网格坐标不是有限数值：%1").arg(id));
            copy.append(positions[coordinate]);
        }
        modelPositions.insert(id, std::move(copy));
    }
    for (int index : runtime->GetSortedDrawableIndices(m_drawableDrawOrderOverrides, m_drawableOrderGroupIndices, m_drawOrderGroups))
        order.append(QString::fromUtf8(runtime->GetDrawableId(index)->GetString().GetRawString()));
    return CExportTaskRunner::Run([=](const CExportTaskRunner::GuiCall&, QString* detail) {
        CTextureExtractor extractor;
        if (!extractor.LoadAtlases(atlases, control)) { if (detail) *detail = control->Error(); return false; }
        auto reader = live2dstruct::CReader::FromBytes(source);
        live2dstruct::CSectionTable table(reader);
        if (!table.IsValid()) { if (detail) *detail = QStringLiteral("moc3 解析失败：%1").arg(reader.Error()); return false; }
        const QSize canvas = CTextureExtractor::GetCanvasPixelSize(table.canvas_info);
        CPsdWriteList writer;
        writer.SetControl(control);
        if (!writer.CreatePsd(path, canvas.width(), canvas.height(), cubismCompat)) { if (detail) *detail = control->Error(); return false; }
        const auto layers = extractor.ExtractAll(&table, includeHidden, &topology, control, &modelPositions);
        if (!control->Checkpoint()) { if (detail) *detail = control->Error(); return false; }
        QHash<QString, const ExtractedLayer*> byId;
        for (const auto& layer : layers) byId.insert(layer.name, &layer);
        CCidJson names;
        names.Parse(cidPath);
        const auto writeLayer = [&](const QString& id) {
            if (hidden.contains(id)) return true;
            const auto* layer = byId.value(id, nullptr);
            if (!layer) return true;
            QString name = names.getPartName(id);
            if (name.isEmpty()) name = id;
            const float alpha = opacity.value(id, layer->opacity);
            return writer.AddLayer(name, layer->canvasRect, layer->image,
                                   static_cast<quint8>(qBound(0.0f, alpha * 255.0f, 255.0f)),
                                   CTextureExtractor::BlendModeToPsd(layer->drawableFlags), false, false);
        };
        if (groups) {
            live2dstruct::CModelDocument document;
            if (!document.LoadBytes(source, mocPath, detail)) return false;
            QHash<QString, int> sourceIndices;
            for (int index = 0; index < document.Drawables().size(); ++index) sourceIndices.insert(document.Drawables()[index].id, index);
            QVector<int> sources;
            for (const QString& id : order) {
                if (!sourceIndices.contains(id)) { if (detail) *detail = QStringLiteral("纹理分组无法映射运行时网格：%1").arg(id); return false; }
                if (byId.contains(id) && !hidden.contains(id)) sources.append(sourceIndices.value(id));
            }
            const auto grouped = live2dstruct::BuildGroupedLayerOrder(document.Parts(), document.Drawables(), sources);
            for (const auto& entry : grouped) {
                if (!control->Checkpoint()) { if (detail) *detail = control->Error(); return false; }
                if (entry.type == live2dstruct::CLayerOrderItem::Layer) { if (!writeLayer(entry.id)) return false; }
                else if (entry.type == live2dstruct::CLayerOrderItem::GroupStart) {
                    const QString display = names.getPartName(entry.id);
                    if (!writer.AddGroupStart(display.isEmpty() ? entry.id : display)) return false;
                } else if (!writer.AddGroupEnd()) return false;
            }
        } else {
            for (const QString& id : order) {
                if (!control->Checkpoint()) { if (detail) *detail = control->Error(); return false; }
                if (!writeLayer(id)) return false;
            }
        }
        return writer.WritePsd();
    }, control, error);
}

bool LAppDelegate::ChangeScene(const QString &modelJsonName)
{
    if (m_exportInProgress || m_psdExportSession) return false;
    if (!IsRuntimeReady() || !LAppLive2DManager::GetInstance()->ChangeSceneFileName(modelJsonName)) return false;
    ClearModelRuntimeState(true);
    return true;
}

bool LAppDelegate::BeginTemporaryScene(const QString& modelJsonName)
{
    if (m_exportInProgress || m_psdExportSession) return false;
    if (!IsRuntimeReady() || !LAppLive2DManager::GetInstance()->BeginTemporaryScene(modelJsonName)) return false;
    ClearModelRuntimeState(true);
    return true;
}

bool LAppDelegate::EndTemporaryScene()
{
    if (m_exportInProgress || m_psdExportSession) return false;
    if (!IsRuntimeReady() || !LAppLive2DManager::GetInstance()->EndTemporaryScene()) return false;
    ClearModelRuntimeState(true);
    return true;
}

void LAppDelegate::ClearModelRuntimeState(bool clearOrderGroups)
{
    if (m_exportInProgress || m_psdExportSession) return;
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

bool LAppDelegate::StartDeformerDrag(QString* error)
{
    if (error) error->clear();
    StopDeformerDrag();
    const auto fail = [&](const QString& message) {
        if (error) *error = message;
        return false;
    };
    auto* manager = LAppLive2DManager::GetInstance();
    if (!manager || manager->GetModelNum() <= 0 || !manager->GetModel(0) ||
        !manager->GetModel(0)->GetModel() || manager->GetModel(0)->HasEditorPreview() || m_dragTargetDrawables.isEmpty())
        return fail(QStringLiteral("当前模型或变形器拖动目标不可用"));
    try {
        auto* model = manager->GetModel(0)->GetModel();
        QMap<int, std::vector<float>> saved;
        bool hasTarget = false;
        const int drawCount = model->GetDrawableCount();
        if (drawCount < 0) return fail(QStringLiteral("模型网格数量无效"));
        for (int index = 0; index < drawCount; ++index) {
            const auto id = model->GetDrawableId(index);
            if (!id) return fail(QStringLiteral("模型网格缺少标识"));
            if (m_dragTargetDrawables.contains(QString::fromUtf8(id->GetString().GetRawString()))) {
                hasTarget = true;
                continue;
            }
            const int count = model->GetDrawableVertexCount(index);
            if (count < 0 || count > std::numeric_limits<int>::max() / 2)
                return fail(QStringLiteral("模型网格顶点数量无效"));
            if (!count) continue;
            const auto* vertices = model->GetDrawableVertexPositions(index);
            if (!vertices) return fail(QStringLiteral("模型网格缺少顶点"));
            std::vector<float> points(count * 2);
            for (int vertex = 0; vertex < count; ++vertex) {
                if (!std::isfinite(vertices[vertex].X) || !std::isfinite(vertices[vertex].Y))
                    return fail(QStringLiteral("模型网格坐标无效"));
                points[vertex * 2] = vertices[vertex].X;
                points[vertex * 2 + 1] = vertices[vertex].Y;
            }
            saved.insert(index, std::move(points));
        }
        if (!hasTarget) return fail(QStringLiteral("模型中没有指定的拖动目标"));
        m_savedVertices.swap(saved);
        m_isDraggingDeformer = true;
        return true;
    } catch (const std::exception&) {
        return fail(QStringLiteral("无法准备变形器拖动顶点"));
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
