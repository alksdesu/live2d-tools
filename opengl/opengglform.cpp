#include "opengglform.h"
#include "edit/cmeshmap.h"
#include "ccanvascoordinates.h"
#include "framework/Id/CubismIdManager.hpp"
#include "framework/Rendering/OpenGL/CubismRenderer_OpenGLES2.hpp"
#include "conionskinrenderer.h"
#include "live2d/LAppPal.hpp"
#include "live2dstruct/cmocdeformerorder.h"
#include <QSurfaceFormat>
#include <QSurface>
#include <QScopeGuard>
#include <QFocusEvent>
#include <QHideEvent>
#include <QApplication>
#include <QPainter>
#include <QLineF>
#include <algorithm>
#include <cmath>
#include <exception>
#include <limits>

namespace {
class RestoreContext
{
public:
    RestoreContext() : previous(QOpenGLContext::currentContext()), surface(previous ? previous->surface() : nullptr),
        surfaceObject(dynamic_cast<QObject*>(surface)) {}
    ~RestoreContext()
    {
        if (QOpenGLContext::currentContext() == previous) return;
        if (auto* current = QOpenGLContext::currentContext()) current->doneCurrent();
        if (previous && previous->isValid() && surfaceObject) previous->makeCurrent(surface);
    }
private:
    QPointer<QOpenGLContext> previous;
    QSurface* surface;
    QPointer<QObject> surfaceObject;
};

bool MeshError(QString* error, const QString& message)
{
    if (error) *error = message;
    return false;
}

LAppModel* CurrentModel()
{
    auto* manager = LAppLive2DManager::GetInstance();
    return manager && manager->GetModelNum() > 0 ? manager->GetModel(0) : nullptr;
}

bool CorePositions(CubismModel& model, int index, QVector<float>& output)
{
    if (index < 0 || index >= model.GetDrawableCount()) return false;
    const int count = model.GetDrawableVertexCount(index);
    if (count <= 0 || count > 1000000) return false;
    const float* values = model.GetDrawableVertices(index);
    if (!values) return false;
    for (int value = 0; value < count * 2; ++value)
        if (!std::isfinite(values[value])) return false;
    output = QVector<float>(values, values + count * 2);
    return true;
}

bool ViewPoint(LAppView& view, const QSize& size, const QPointF& pixel, QPointF& output)
{
    if (!CCanvasCoordinates::ScreenToModel(view.GetProjectionView(), size, pixel, output)) return false;
    return std::abs(output.x()) <= std::numeric_limits<float>::max() &&
           std::abs(output.y()) <= std::numeric_limits<float>::max();
}

bool FiniteMatrix(const QMatrix4x4& matrix)
{
    for (int index = 0; index < 16; ++index)
        if (!std::isfinite(matrix.constData()[index])) return false;
    return true;
}
}


OpengGlForm::OpengGlForm(QWidget *par):QOpenGLWidget(par)
{
    // MSAA 抗锯齿
    QSurfaceFormat fmt = format();
    fmt.setSamples(4);
    setFormat(fmt);

    constexpr int frame = 60;
    constexpr int fps = 1000 / frame;
    m_timer.start(fps);
    connect(&m_timer,SIGNAL(timeout()),this,SLOT(on_timeout()));

    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
}

OpengGlForm::~OpengGlForm()
{
    if (context()) makeCurrent();
    QObject::disconnect(m_onionContextConnection);
    m_onionRenderer.reset();
    LAppDelegate::GetInstance()->Release();
    LAppDelegate::ReleaseInstance();
    if (context()) doneCurrent();
}

void OpengGlForm::SetInitOpenName(const QString &Name)
{
    if (m_exportSuspended && m_initializationCompleted) return;
    m_config.SetInitFileName(Name);
    m_currentModelJson = Name;
}



void OpengGlForm::on_timeout()
{
    if (m_exportSuspended) return;
    update();
}

bool OpengGlForm::do_OffSeceen(const QString& fileName,bool IsALl,bool IsFenzu,bool cubismCompat)
{
    QString error;
    bool ok = do_OffSeceenSilent(fileName,IsALl,IsFenzu,cubismCompat, &error);

    if (!fileName.isEmpty()) {
        if (ok) {
            QMessageBox::about(this,"psd","保存成功");
            QFileInfo fileInfo(fileName);
            QString directoryPath = fileInfo.absoluteDir().path();
            QDesktopServices::openUrl(QUrl::fromLocalFile(directoryPath));
        } else {
            QMessageBox::warning(this,"psd", error);
        }
    }
    return ok;
}

bool OpengGlForm::do_OffSeceenSilent(const QString &fileName, bool IsALl, bool IsFenzu, bool cubismCompat, QString* error,
                                   CExportControl* control)
{
    if (error) error->clear();
    if (fileName.isEmpty()) return MeshError(error, QStringLiteral("PSD 导出路径为空"));
    if (!IsModelReady()) return MeshError(error, QStringLiteral("当前模型尚未就绪"));
    const RestoreContext restore;
    if (!MakeRuntimeCurrent()) return MeshError(error, QStringLiteral("无法使用当前模型的 OpenGL 上下文"));
    if(IsALl){
        m_ceditstate.AddState(CEditState::PrintAllPsd);
    }
    else{
        m_ceditstate.RemoveState(CEditState::PrintAllPsd);
    }
    if(IsFenzu){
        m_ceditstate.AddState(CEditState::PrintFenzuPsd);
    }
    else{
        m_ceditstate.RemoveState(CEditState::PrintFenzuPsd);
    }

    const QPointer<OpengGlForm> surface = this;
    const auto clearPrintState = qScopeGuard([surface] {
        if (!surface) return;
        surface->m_ceditstate.RemoveState(CEditState::PrintAllPsd);
        surface->m_ceditstate.RemoveState(CEditState::PrintFenzuPsd);
    });
    return LAppDelegate::GetInstance()->StartOffset_FunpsdList(fileName,IsALl,IsFenzu,cubismCompat,error,control);
}

bool OpengGlForm::do_TextureExtract(const QString& fileName, bool isAll, bool isFenzu, bool cubismCompat)
{
    QString error;
    bool ok = do_TextureExtractSilent(fileName, isAll, isFenzu, cubismCompat, &error);
    if (!fileName.isEmpty()) {
        if (ok) {
            QMessageBox::about(this, "psd", "保存成功（原始纹理提取）");
            QFileInfo fileInfo(fileName);
            QString directoryPath = fileInfo.absoluteDir().path();
            QDesktopServices::openUrl(QUrl::fromLocalFile(directoryPath));
        } else {
            QMessageBox::warning(this, "psd", error);
        }
    }
    return ok;
}

bool OpengGlForm::do_TextureExtractSilent(const QString& fileName, bool isAll, bool isFenzu, bool cubismCompat, QString* error,
                                         CExportControl* control)
{
    if (error) error->clear();
    if (fileName.isEmpty()) return MeshError(error, QStringLiteral("PSD 导出路径为空"));
    if (!m_runtimeReady || !IsModelReady()) return MeshError(error, QStringLiteral("当前模型尚未就绪"));
    auto* delegate = LAppDelegate::GetInstance();
    if (!context() || delegate->getGlOwningContext() != context())
        return MeshError(error, QStringLiteral("当前模型不属于此预览窗口"));
    return delegate->StartTextureExtraction(fileName, isAll, isFenzu, cubismCompat,error,control);
}

void OpengGlForm::do_ShowMesh(const QString &artName)
{
    const QPointer<OpengGlForm> alive(this);
    if (m_exportSuspended || (m_meshToolDispatchActive && artName != m_meshToolTarget)) return;
    if (IsMeshDragging() && m_meshDrag->name != artName) {
        QString error;
        if (!CancelMeshDrag(&error)) { if (alive) emit MeshEditRejected(error); return; }
        if (!alive) return;
    }
    if (m_meshToolGesture && m_meshToolTarget != artName) CancelMeshToolGesture();
    if (!alive) return;
    if (!m_meshToolTarget.isEmpty() && m_meshToolTarget != artName) ResetMeshToolState();
    if (!alive) return;
    m_meshEditName = artName;
    LAppLive2DManager* Live2DManager = LAppLive2DManager::GetInstance();
    if(Live2DManager && Live2DManager->GetModelNum() > 0){
        LAppModel* model = Live2DManager->GetModel(0);
        if(!model || !model->GetModel()) return;
        CMeshMap* map = model->GetModel()->GetMeshMap();
        if(!map) return;
        map->SetShow(artName);

    }


}

void OpengGlForm::SetMeshEditEnabled(bool enabled)
{
    const QPointer<OpengGlForm> alive(this);
    if (m_exportSuspended) return;
    if (!enabled && IsMeshDragging()) {
        QString error;
        if (!CancelMeshDrag(&error)) { if (alive) emit MeshEditRejected(error); return; }
        if (!alive) return;
    }
    m_meshEditEnabled = enabled;
    if (!enabled) {
        CancelMeshToolGesture();
        if (!alive) return;
        ResetMeshToolState();
        if (!alive) return;
        m_meshEditName.clear();
    }
}

void OpengGlForm::SetPhysicsDragEnabled(bool enabled)
{
    if (m_exportSuspended) return;
    FinishDeformerDrag();
    m_physicsDragEnabled = enabled;
}

void OpengGlForm::SetActiveKeyform(const QString& name, int keyformIndex)
{
    const QPointer<OpengGlForm> alive(this);
    if (m_exportSuspended || (m_meshToolDispatchActive && (name != m_meshEditName || keyformIndex != m_activeKeyformIndex))) return;
    if (name.isEmpty() || keyformIndex < 0) return;
    if (IsMeshDragging() && (m_meshDrag->name != name || m_meshDrag->keyform != keyformIndex)) {
        QString error;
        if (!CancelMeshDrag(&error)) { if (alive) emit MeshEditRejected(error); return; }
        if (!alive) return;
    }
    if (m_meshToolGesture && (m_meshEditName != name || m_activeKeyformIndex != keyformIndex)) CancelMeshToolGesture();
    if (!alive) return;
    if (!m_meshToolTarget.isEmpty() && m_meshToolTarget != name) ResetMeshToolState();
    if (!alive) return;
    m_meshEditName = name;
    m_activeKeyformIndex = keyformIndex;
}



void OpengGlForm::do_ShowWarp(const QString &artName)
{
    if (m_exportSuspended) return;
    LAppView* view = LAppDelegate::GetInstance()->GetView();
    if(!view) return;
    view->ShowWarp(artName);
}



void OpengGlForm::do_setPoint(float x, float y)
{
    if (m_exportSuspended) return;
    LAppLive2DManager* manager = LAppLive2DManager::GetInstance();
    if(!manager || manager->GetModelNum() <= 0 || !manager->GetModel(0)) return;
    CubismModelMatrix* matrix = manager->GetModel(0)->GetModelMatrix();
    if(!matrix) return;
    matrix->SetCenterPosition(x,y);
}

void OpengGlForm::do_change_para_value(const QString &name, float value,bool IsInit)
{
    if (m_exportSuspended) return;
    const bool wasCurrent = context() && QOpenGLContext::currentContext() == context();
    if (!MakeRuntimeCurrent()) return;
    const auto releaseContext = qScopeGuard([&] { if (!wasCurrent) doneCurrent(); });
    LAppLive2DManager* Live2DManager = LAppLive2DManager::GetInstance();
    if(Live2DManager !=nullptr){
        int modelCount = Live2DManager->GetModelNum();
        if(modelCount > 0){
            LAppModel* model = Live2DManager->GetModel(0);
            if (model && model->HasEditorPreview()) return;
            if (model) {
                model->UpdateParaValue(name,value,IsInit);
            }
            if (model && IsInit && model->GetModel()) {
                CMeshMap* meshMap = model->GetModel()->GetMeshMap();
                if (meshMap) {
                    for (auto it = LAppDelegate::GetInstance()->m_meshIndexOverrides.cbegin();
                         it != LAppDelegate::GetInstance()->m_meshIndexOverrides.cend(); ++it) {
                        const auto id = CubismFramework::GetIdManager()->GetId(it.key().toUtf8().constData());
                        model->GetModel()->SetDrawableIndicesOverride(model->GetModel()->GetDrawableIndex(id), it.value());
                    }
                }
            }
        }
    }
    update();
}

void OpengGlForm::do_ShowDeformers(const QStringList& names)
{
    if (m_exportSuspended) return;
    LAppView* view = LAppDelegate::GetInstance()->GetView();
    if(!view) return;
    if(names.isEmpty()){
        view->HideAllWarps();
    } else {
        view->ShowWarps(names);
    }
}

void OpengGlForm::do_ShowDeformerMeshes(const QStringList& artMeshNames)
{
    if (m_exportSuspended) return;
    FinishDeformerDrag();
    LAppLive2DManager* mgr = LAppLive2DManager::GetInstance();
    if(!mgr || mgr->GetModelNum() <= 0) return;
    LAppModel* model = mgr->GetModel(0);
    if(!model || !model->GetModel()) return;
    CMeshMap* map = model->GetModel()->GetMeshMap();
    if(!map) return;

    // 设置拖拽目标 drawable 列表
    LAppDelegate::GetInstance()->m_dragTargetDrawables.clear();
    if(artMeshNames.isEmpty()){
        map->HideAll();
    } else {
        map->SetShowMultiple(artMeshNames);
        for(const QString& name : artMeshNames){
            LAppDelegate::GetInstance()->m_dragTargetDrawables.insert(name);
        }
    }
}

void OpengGlForm::do_SetDeformerDragParams(const QStringList& paramIds, const QList<float>& minVals,
                                            const QList<float>& maxVals, const QList<float>& defaultVals)
{
    if (m_exportSuspended) return;
    FinishDeformerDrag();
    LAppLive2DManager* mgr = LAppLive2DManager::GetInstance();
    if(mgr && mgr->GetModelNum() > 0 && mgr->GetModel(0)){
        if (mgr->GetModel(0)->HasEditorPreview()) return;
        mgr->GetModel(0)->SetDeformerDragParams(paramIds, minVals, maxVals, defaultVals);
    }
}

void OpengGlForm::do_ResetView()
{
    const QPointer<OpengGlForm> alive(this);
    if (m_exportSuspended) return;
    QString error;
    if (!CancelMeshDrag(&error)) { if (alive) emit MeshEditRejected(error); return; }
    if (!alive) return;
    FinishDeformerDrag();
    LAppView* view = LAppDelegate::GetInstance()->GetView();
    if(!view) return;
    QMatrix4x4 identity;
    identity.setToIdentity();
    view->SetViewMatrix(identity);
    m_config.SetScalefactor(1.0f);
}

QVector2D OpengGlForm::ViewPosition() const
{
    if (!m_runtimeReady) return {};
    auto* view = LAppDelegate::GetInstance()->GetView();
    const double scale = ViewScale();
    if (!view || !std::isfinite(scale) || scale <= 0) return {};
    const auto matrix = view->GetViewMatrix();
    const double x = double(matrix(0, 3)) / scale, y = double(matrix(1, 3)) / scale;
    if (!std::isfinite(x) || !std::isfinite(y) || std::abs(x) > std::numeric_limits<float>::max() ||
        std::abs(y) > std::numeric_limits<float>::max()) return {};
    return QVector2D(float(x), float(y));
}

void OpengGlForm::SetViewState(float scale, const QVector2D& position)
{
    const QPointer<OpengGlForm> alive(this);
    if (!std::isfinite(scale) || scale <= 0.0f || !std::isfinite(position.x()) || !std::isfinite(position.y())) return;
    QString error;
    if (!CancelMeshDrag(&error)) { if (alive) emit MeshEditRejected(error); return; }
    if (!alive) return;
    FinishDeformerDrag();
    const float clampedScale = qBound(0.1f, scale, 20.0f);
    LAppView* view = LAppDelegate::GetInstance()->GetView();
    if (!view) return;
    QMatrix4x4 matrix;
    matrix.setToIdentity();
    matrix.scale(clampedScale);
    matrix.translate(position.x(), position.y(), 0.0f);
    if (!FiniteMatrix(matrix)) return;
    m_config.SetScalefactor(clampedScale);
    view->SetViewMatrix(matrix);
}

void OpengGlForm::do_SetVisibility(const QString& name, const QString& type, bool visible)
{
    if (m_exportSuspended) return;
    if(type == "part"){
        // Part 通过 LAppModel 的隐藏列表控制（每帧 Update 中强制设 opacity=0）
        LAppLive2DManager* mgr = LAppLive2DManager::GetInstance();
        if(mgr && mgr->GetModelNum() > 0 && mgr->GetModel(0)){
            mgr->GetModel(0)->SetPartVisible(name, visible);
        }
    } else if (type == "artmesh") {
        if(visible){
            LAppDelegate::GetInstance()->m_hiddenDrawables.remove(name);
        } else {
            LAppDelegate::GetInstance()->m_hiddenDrawables.insert(name);
        }
    }
    qDebug() << "[Visibility]" << type << name << (visible ? "show" : "hide");
}

void OpengGlForm::SetHiddenDeformerDrawables(const QSet<QString>& names)
{
    if (m_exportSuspended) return;
    auto* delegate = LAppDelegate::GetInstance();
    if (!delegate) return;
    delegate->m_hiddenDeformerDrawables = names;
}

void OpengGlForm::ClearEditorOverrides()
{
    if (m_exportSuspended || m_meshToolDispatchActive) return;
    CancelMeshToolGesture();
    const bool wasCurrent = context() && QOpenGLContext::currentContext() == context();
    if (!MakeRuntimeCurrent()) return;
    const auto releaseContext = qScopeGuard([&] { if (!wasCurrent) doneCurrent(); });
    auto* delegate = LAppDelegate::GetInstance();
    if (!delegate) return;
    delegate->m_hiddenDrawables.clear();
    delegate->m_hiddenDeformerDrawables.clear();
    delegate->m_drawableOpacityOverrides.clear();
    delegate->m_drawableDrawOrderOverrides.clear();
    delegate->m_meshOverlayPositions.clear();
    delegate->m_meshOverlayKeyformIndices.clear();
    delegate->m_meshIndexOverrides.clear();
    LAppLive2DManager* manager = LAppLive2DManager::GetInstance();
    if (manager && manager->GetModelNum() > 0 && manager->GetModel(0) && manager->GetModel(0)->GetModel()) {
        auto* model = manager->GetModel(0)->GetModel();
        model->ClearDrawableIndicesOverrides();
        model->InitMesh();
    }
    NotifyMeshSelection();
}

void OpengGlForm::PublishRenderOrder(const QStringList& names)
{
    if (m_exportSuspended) return;
    if (names == m_lastRenderOrder) return;
    m_lastRenderOrder = names;
    emit RenderOrderChanged(names);
}

void OpengGlForm::SetDrawOrderGroups(
    const QHash<QString, int>& groupIndices,
    const QVector<live2dstruct::CDrawableOrderGroup>& groups)
{
    if (m_exportSuspended) return;
    auto* delegate = LAppDelegate::GetInstance();
    delegate->m_drawableOrderGroupIndices = groupIndices;
    delegate->m_drawOrderGroups = groups;
}

void OpengGlForm::do_SetDrawableOpacity(const QString& name, float opacity)
{
    if (m_exportSuspended) return;
    if (name.isEmpty() || !std::isfinite(opacity)) return;
    LAppDelegate::GetInstance()->m_drawableOpacityOverrides.insert(name, qBound(0.0f, opacity, 1.0f));
}

void OpengGlForm::do_SetDrawableDrawOrder(const QString& name, int drawOrder)
{
    if (m_exportSuspended) return;
    if (name.isEmpty()) return;
    LAppDelegate::GetInstance()->m_drawableDrawOrderOverrides.insert(name, qBound(0, drawOrder, 1000));
}

bool OpengGlForm::CaptureMeshToolSnapshot(const QPointF& pixel, MeshToolSnapshot& snapshot, QString* error) const
{
    return CaptureMeshToolState(m_meshToolTarget, pixel, true, snapshot, error);
}

bool OpengGlForm::CaptureMeshToolState(const QString& target, const QPointF& pixel, bool requireEditor,
                                       MeshToolSnapshot& snapshot, QString* error) const
{
    if (error) error->clear();
    if (m_exportSuspended || target.isEmpty() || CurrentPreviewDigest().isEmpty())
        return MeshError(error, QStringLiteral("网格工具没有可用的预览目标"));
    auto* appModel = CurrentModel();
    auto* view = LAppDelegate::GetInstance()->GetView();
    if (!appModel || !appModel->GetModel() || (requireEditor && !appModel->HasEditorPreview()) || appModel->HasExportPreview() || !view)
        return MeshError(error, QStringLiteral("请先固定当前 ArtMesh 关键形，再使用网格工具"));
    try {
        MeshToolSnapshot candidate;
        candidate.artMeshId = target;
        candidate.keyformIndex = m_activeKeyformIndex;
        candidate.previewDigest = appModel->GetPreviewMocDigest();
        candidate.mvp = view->GetMVP();
        candidate.viewportSize = size();
        candidate.pixelPosition = pixel;
        candidate.parameterValues = appModel->GetParameterValues();
        if (!CCanvasCoordinates::ScreenToModel(candidate.mvp, candidate.viewportSize, pixel, candidate.modelPosition))
            return MeshError(error, QStringLiteral("当前画布无法反解鼠标位置"));
        for (float value : candidate.parameterValues)
            if (!std::isfinite(value)) return MeshError(error, QStringLiteral("当前网格姿态包含无效参数"));
        auto* model = appModel->GetModel();
        int drawable = -1;
        for (int index = 0; index < model->GetDrawableCount(); ++index) {
            const auto id = model->GetDrawableId(index);
            if (id && QString::fromUtf8(id->GetString().GetRawString()) == candidate.artMeshId) { drawable = index; break; }
        }
        if (!CorePositions(*model, drawable, candidate.coreVertices))
            return MeshError(error, QStringLiteral("当前 ArtMesh 没有可用的 Core 顶点"));
        const int count = model->GetDrawableVertexIndexCount(drawable);
        const auto* indices = model->GetDrawableVertexIndices(drawable);
        if (count < 0 || count > 3000000 || count % 3 || (count && !indices))
            return MeshError(error, QStringLiteral("当前 ArtMesh 三角索引无效"));
        for (int index = 0; index < count; ++index) {
            if (indices[index] >= candidate.coreVertices.size() / 2)
                return MeshError(error, QStringLiteral("当前 ArtMesh 三角索引越界"));
            candidate.indices.append(indices[index]);
        }
        const auto* mesh = model->GetMeshMap() ? model->GetMeshMap()->GetDrawMesh(candidate.artMeshId) : nullptr;
        CDrawMesh::EditorState state;
        if (!mesh || !mesh->CaptureEditorState(state, error) || state.vertexCount != candidate.coreVertices.size() / 2)
            return MeshError(error, QStringLiteral("网格选区与 Core 顶点数量不一致"));
        candidate.selectedVertices = state.selectedVertices;
        snapshot = std::move(candidate);
        return true;
    } catch (const std::exception&) {
        return MeshError(error, QStringLiteral("无法捕获网格工具快照"));
    }
}

bool OpengGlForm::MeshToolSnapshotMatches(const MeshToolSnapshot& snapshot, QString* error) const
{
    MeshToolSnapshot current;
    if (!CaptureMeshToolSnapshot(snapshot.pixelPosition, current, error)) return false;
    if (snapshot.artMeshId != current.artMeshId || snapshot.keyformIndex != current.keyformIndex ||
        snapshot.previewDigest != current.previewDigest || snapshot.mvp != current.mvp ||
        snapshot.viewportSize != current.viewportSize || snapshot.parameterValues != current.parameterValues ||
        snapshot.coreVertices != current.coreVertices || snapshot.indices != current.indices ||
        snapshot.selectedVertices != current.selectedVertices)
        return MeshError(error, QStringLiteral("网格、姿态、视图或选区已改变，请重新操作"));
    return true;
}

bool OpengGlForm::CanSetMeshTool(MeshTool tool, const QString& target, QString* error) const
{
    if (error) error->clear();
    if (int(tool) < int(MeshTool::Select) || int(tool) > int(MeshTool::Delete) || target.isEmpty())
        return MeshError(error, QStringLiteral("网格工具或目标无效"));
    if (m_exportSuspended || m_meshToolDispatchActive || m_meshEditCallbackActive || m_deformerCallbackActive || IsKeyformDragging() || m_controlDragActive || m_physicsDragActive ||
        m_meshToolGesture || m_ceditstate.IsContins(CEditState::ViewMove))
        return MeshError(error, QStringLiteral("请先结束当前画布操作再切换网格工具"));
    if (tool != MeshTool::Select && !m_meshToolHandler)
        return MeshError(error, QStringLiteral("网格结构工具尚未连接"));
    MeshToolSnapshot snapshot;
    return CaptureMeshToolState(target, QPointF(width() / 2.0, height() / 2.0), false, snapshot, error);
}

bool OpengGlForm::SetMeshToolSelection(const MeshToolSnapshot& snapshot, const QVector<int>& selected, QString* error)
{
    if (!MeshToolSnapshotMatches(snapshot, error) || !SetToolSelection(selected, error)) return false;
    NotifyMeshSelection();
    return true;
}

bool OpengGlForm::SetMeshTool(MeshTool tool, const QString& target, QString* error)
{
    if (error) error->clear();
    if (target.isEmpty()) return ClearMeshTool(error);
    if (tool == m_meshTool && target == m_meshToolTarget) {
        MeshToolSnapshot snapshot;
        return CaptureMeshToolSnapshot(QPointF(width() / 2.0, height() / 2.0), snapshot, error);
    }
    if (!CanSetMeshTool(tool, target, error)) return false;
    MeshToolSnapshot snapshot;
    if (target != m_meshEditName || !CaptureMeshToolState(target, QPointF(width() / 2.0, height() / 2.0), true, snapshot, error))
        return MeshError(error, error && !error->isEmpty() ? *error : QStringLiteral("工具目标与当前关键形不一致"));
    CancelMeshToolGesture();
    m_meshToolTarget = target;
    m_meshTool = tool;
    update();
    const QPointer<OpengGlForm> alive(this);
    emit MeshToolChanged(tool, target);
    if (alive) NotifyMeshSelection();
    return true;
}

bool OpengGlForm::ClearMeshTool(QString* error)
{
    const QPointer<OpengGlForm> alive(this);
    if (error) error->clear();
    if (m_meshToolDispatchActive || m_meshEditCallbackActive || m_deformerCallbackActive) return MeshError(error, QStringLiteral("正在提交网格操作"));
    if (IsKeyformDragging() && !CancelMeshDrag(error)) return false;
    if (!alive) return false;
    CancelMeshToolGesture();
    if (!alive) return false;
    ResetMeshToolState();
    return true;
}

void OpengGlForm::ResetMeshToolState()
{
    m_meshToolGesture.reset();
    if (m_meshToolTarget.isEmpty()) return;
    m_meshToolTarget.clear();
    m_meshTool = MeshTool::Select;
    update();
    const QPointer<OpengGlForm> alive(this);
    emit MeshToolChanged(MeshTool::Select, {});
    if (alive) emit MeshSelectionChanged({}, {});
}

void OpengGlForm::SetMeshToolHandler(MeshToolHandler handler)
{
    if (m_meshToolDispatchActive) return;
    CancelMeshToolGesture();
    m_meshToolHandler = std::move(handler);
}

bool OpengGlForm::SetToolSelection(const QVector<int>& selected, QString* error)
{
    if (CurrentPreviewDigest().isEmpty()) return MeshError(error, QStringLiteral("当前网格选区不可用"));
    auto* model = CurrentModel();
    auto* mesh = model && model->GetModel() ? model->GetModel()->GetMeshMap()->GetDrawMesh(m_meshToolTarget) : nullptr;
    CDrawMesh::EditorState state;
    if (!mesh || !mesh->CaptureEditorState(state, error)) return false;
    state.selectedVertices = selected;
    std::sort(state.selectedVertices.begin(), state.selectedVertices.end());
    if (!mesh->RestoreEditorState(state, error)) return false;
    update();
    return true;
}

void OpengGlForm::NotifyMeshSelection()
{
    if (m_meshToolDispatchActive) { m_meshSelectionNotificationPending = true; return; }
    m_meshSelectionNotificationPending = false;
    if (m_meshToolTarget.isEmpty() || CurrentPreviewDigest().isEmpty()) return;
    auto* model = CurrentModel();
    const auto* mesh = model && model->GetModel() ? model->GetModel()->GetMeshMap()->GetDrawMesh(m_meshToolTarget) : nullptr;
    CDrawMesh::EditorState state;
    if (mesh && mesh->CaptureEditorState(state)) emit MeshSelectionChanged(m_meshToolTarget, state.selectedVertices);
}

void OpengGlForm::CancelMeshToolGesture()
{
    if (!m_meshToolGesture) return;
    const auto gesture = std::move(*m_meshToolGesture);
    m_meshToolGesture.reset();
    m_ceditstate.RemoveState(CEditState::MousePress);
    if (CurrentPreviewDigest() == gesture.snapshot.previewDigest) {
        auto* model = CurrentModel();
        auto* mesh = model && model->GetModel() ? model->GetModel()->GetMeshMap()->GetDrawMesh(gesture.snapshot.artMeshId) : nullptr;
        const auto old = gesture.selection.constFind(gesture.snapshot.artMeshId);
        CDrawMesh::EditorState current;
        if (mesh && old != gesture.selection.cend() && mesh->CaptureEditorState(current) && current.vertexCount == old->vertexCount) {
            current.selectedVertices = old->selectedVertices;
            mesh->RestoreEditorState(current);
        }
    }
    update();
    NotifyMeshSelection();
}

bool OpengGlForm::BeginMeshToolGesture(QMouseEvent* event, QString* error)
{
    if (m_meshToolDispatchActive) return MeshError(error, QStringLiteral("正在提交网格操作"));
    MeshToolGesture gesture;
    if (!CaptureMeshToolSnapshot(event->position(), gesture.snapshot, error) ||
        !CaptureMeshEditorState(gesture.selection, error)) return false;
    gesture.currentPixel = event->position();
    gesture.modifiers = event->modifiers();
    gesture.pickedVertex = CCanvasCoordinates::NearestVertex(gesture.snapshot.mvp, gesture.snapshot.viewportSize,
                                                            event->position(), gesture.snapshot.coreVertices);
    m_meshToolGesture = std::move(gesture);
    if (m_meshTool == MeshTool::Select && m_meshToolGesture->pickedVertex >= 0) {
        auto selected = m_meshToolGesture->snapshot.selectedVertices;
        const int point = m_meshToolGesture->pickedVertex;
        if (event->modifiers() & Qt::AltModifier) selected.removeAll(point);
        else if (event->modifiers() & Qt::ShiftModifier) {
            if (selected.contains(point)) selected.removeAll(point);
            else selected.append(point);
        } else if (!selected.contains(point)) selected = {point};
        if (!SetToolSelection(selected, error)) { CancelMeshToolGesture(); return false; }
        std::sort(selected.begin(), selected.end());
        m_meshToolGesture->snapshot.selectedVertices = std::move(selected);
    }
    update();
    NotifyMeshSelection();
    return true;
}

bool OpengGlForm::MoveMeshToolGesture(QMouseEvent* event, QString* error)
{
    if (!m_meshToolGesture) return true;
    if (!(event->buttons() & Qt::LeftButton) || !MeshToolSnapshotMatches(m_meshToolGesture->snapshot, error)) {
        CancelMeshToolGesture();
        return false;
    }
    auto& gesture = *m_meshToolGesture;
    gesture.currentPixel = event->position();
    if (m_meshTool != MeshTool::Select) { update(); return true; }
    const double distance = QLineF(gesture.snapshot.pixelPosition, event->position()).length();
    if (distance < QApplication::startDragDistance() && !gesture.rectangle) return true;
    gesture.rectangle = true;
    const QRectF rectangle = QRectF(gesture.snapshot.pixelPosition, event->position()).normalized();
    QSet<int> selected;
    if (gesture.modifiers & (Qt::ShiftModifier | Qt::AltModifier)) {
        const auto& original = gesture.selection[gesture.snapshot.artMeshId].selectedVertices;
        selected = QSet<int>(original.cbegin(), original.cend());
    }
    for (int index = 0; index < gesture.snapshot.coreVertices.size() / 2; ++index) {
        QPointF point;
        if (!CCanvasCoordinates::ModelToScreen(gesture.snapshot.mvp, gesture.snapshot.viewportSize,
            QPointF(gesture.snapshot.coreVertices[index * 2], gesture.snapshot.coreVertices[index * 2 + 1]), point)) {
            CancelMeshToolGesture();
            return MeshError(error, QStringLiteral("网格顶点无法投影到当前画布"));
        }
        if (rectangle.contains(point)) {
            if (gesture.modifiers & Qt::AltModifier) selected.remove(index);
            else selected.insert(index);
        }
    }
    QVector<int> values(selected.cbegin(), selected.cend());
    std::sort(values.begin(), values.end());
    if (!SetToolSelection(values, error)) { CancelMeshToolGesture(); return false; }
    gesture.snapshot.selectedVertices = std::move(values);
    update();
    NotifyMeshSelection();
    return true;
}

void OpengGlForm::FinishMeshToolGesture(QMouseEvent* event)
{
    if (!m_meshToolGesture) return;
    QString error;
    if (event->button() != Qt::LeftButton || !MeshToolSnapshotMatches(m_meshToolGesture->snapshot, &error)) {
        CancelMeshToolGesture();
        if (!error.isEmpty()) emit MeshEditRejected(error);
        return;
    }
    auto gesture = std::move(*m_meshToolGesture);
    m_meshToolGesture.reset();
    m_ceditstate.RemoveState(CEditState::MousePress);
    update();
    if (m_meshTool == MeshTool::Select) {
        if (!gesture.rectangle && !(gesture.modifiers & (Qt::ShiftModifier | Qt::AltModifier)))
            SetToolSelection(gesture.pickedVertex < 0 ? QVector<int>{} : QVector<int>{gesture.pickedVertex}, &error);
        if (!error.isEmpty()) emit MeshEditRejected(error);
        else NotifyMeshSelection();
        return;
    }
    if (QLineF(gesture.snapshot.pixelPosition, event->position()).length() >= QApplication::startDragDistance()) return;
    MeshToolSnapshot snapshot;
    if (!CaptureMeshToolSnapshot(event->position(), snapshot, &error)) { emit MeshEditRejected(error); return; }
    const int picked = CCanvasCoordinates::NearestVertex(snapshot.mvp, snapshot.viewportSize, event->position(), snapshot.coreVertices);
    if (m_meshTool == MeshTool::Delete) {
        if (picked < 0) return;
        snapshot.operationVertices = snapshot.selectedVertices.contains(picked) ? snapshot.selectedVertices : QVector<int>{picked};
    } else if (m_meshTool == MeshTool::Connect) {
        if (picked < 0) return;
        if (snapshot.selectedVertices.size() != 1) {
            if (!SetToolSelection({picked}, &error)) emit MeshEditRejected(error);
            else NotifyMeshSelection();
            return;
        }
        if (snapshot.selectedVertices.first() == picked) return;
        snapshot.operationVertices = {snapshot.selectedVertices.first(), picked};
    }
    DispatchMeshTool(m_meshTool, std::move(snapshot));
}

void OpengGlForm::DispatchMeshTool(MeshTool tool, MeshToolSnapshot snapshot)
{
    QString error;
    if (m_meshToolDispatchActive || !m_meshToolHandler || !MeshToolSnapshotMatches(snapshot, &error)) {
        emit MeshEditRejected(error.isEmpty() ? QStringLiteral("网格工具未就绪或正在提交") : error);
        return;
    }
    const QPointer<OpengGlForm> alive(this);
    m_meshToolDispatchActive = true;
    m_meshSelectionNotificationPending = false;
    bool accepted = false;
    try { const auto handler = m_meshToolHandler; accepted = handler(tool, snapshot, &error); }
    catch (const std::exception&) { error = QStringLiteral("网格工具提交失败，原状态保持不变"); }
    if (!alive) return;
    m_meshToolDispatchActive = false;
    update();
    if (accepted) NotifyMeshSelection();
    else {
        if (CurrentPreviewDigest() == snapshot.previewDigest) SetToolSelection(snapshot.selectedVertices, nullptr);
        if (m_meshSelectionNotificationPending) NotifyMeshSelection();
        if (alive) emit MeshEditRejected(error.isEmpty() ? QStringLiteral("网格操作被拒绝，原状态保持不变") : error);
    }
}

bool OpengGlForm::MoveMeshDrag(const QPointF& pixel, QString* error)
{
    if (m_meshEditCallbackActive || m_deformerCallbackActive)
        return MeshError(error, QStringLiteral("正在提交画布拖拽"));
    if (!m_meshDrag) return MeshError(error, QStringLiteral("当前没有顶点拖拽"));
    QPointF current;
    if (!CCanvasCoordinates::ScreenToModel(m_meshDrag->mvp, m_meshDrag->size, pixel, current))
        return MeshError(error, QStringLiteral("鼠标位置无法转换到模型坐标"));
    const QPointer<OpengGlForm> alive(this);
    m_meshEditCallbackActive = true;
    const auto release = qScopeGuard([alive] { if (alive) alive->m_meshEditCallbackActive = false; });
    try {
        const auto move = m_meshMove;
        if (!move || !move(current - m_meshDrag->start, error) || !alive)
            return MeshError(error, error && !error->isEmpty() ? *error : QStringLiteral("顶点移动被拒绝，保留最后接受的状态"));
        update();
        NotifyMeshSelection();
        return true;
    } catch (const std::exception&) {
        return MeshError(error, QStringLiteral("顶点移动失败，保留最后接受的状态"));
    }
}

void OpengGlForm::SetMeshEditHandlers(MeshActivate activate, MeshBegin begin, MeshMove move, MeshFinish finish)
{
    if (m_exportSuspended || m_meshEditCallbackActive || m_deformerCallbackActive) return;
    const QPointer<OpengGlForm> alive(this);
    QString error;
    if (!CancelMeshDrag(&error)) { if (alive) emit MeshEditRejected(error); return; }
    if (!alive) return;
    m_meshActivate = std::move(activate);
    m_meshBegin = std::move(begin);
    m_meshMove = std::move(move);
    m_meshFinish = std::move(finish);
}

bool OpengGlForm::ApplyMocPreview(const QByteArray& bytes, const QByteArray& sourceDigest,
                                 const QMap<QString, float>* pose, QString* error)
{
    if (error) error->clear();
    if (m_meshToolDispatchActive) return MeshError(error, QStringLiteral("正在提交网格操作"));
    if (m_exportSuspended) return MeshError(error, QStringLiteral("导出期间不能替换模型预览"));
    if ((IsMeshDragging() && !m_meshEditCallbackActive) ||
        (m_deformerGesture && !m_deformerCallbackActive) || m_meshToolGesture)
        return MeshError(error, QStringLiteral("请先结束当前顶点拖拽"));
    const RestoreContext restore;
    if (!IsModelReady() || !MakeRuntimeCurrent())
        return MeshError(error, QStringLiteral("模型预览的 OpenGL 上下文尚未就绪"));
    auto* model = CurrentModel();
    if (!model) return MeshError(error, QStringLiteral("当前没有模型"));
    try {
        QByteArray compiled;
        if (!live2dstruct::CMocDeformerOrder::Normalize(bytes, compiled, error)) return false;
        auto* view = LAppDelegate::GetInstance()->GetView();
        if (!view) return MeshError(error, QStringLiteral("变形器预览视图尚未就绪"));
        CTreeManage::Snapshot tree;
        LAppView::PreparedDeformerOverlays overlays;
        const auto values = pose ? *pose : model->GetParameterValues();
        if (!CTreeManage::PrepareSnapshot(compiled, values, tree, error) ||
            !view->PrepareDeformerOverlays(tree, overlays, error)) return false;
        if (!(pose ? model->ApplyEditorPreview(compiled, sourceDigest, *pose, error)
                   : model->ApplyPreviewMoc(compiled, sourceDigest, error))) return false;
        CTreeManage::SwapSnapshot(tree);
        view->SwapDeformerOverlays(overlays);
        update();
        return true;
    } catch (const std::exception&) {
        return MeshError(error, QStringLiteral("模型预览替换失败"));
    }
}

QByteArray OpengGlForm::CurrentPreviewDigest() const
{
    if (!m_runtimeReady || !IsModelReady() || !context() ||
        LAppDelegate::GetInstance()->getGlOwningContext() != context()) return {};
    const auto* model = CurrentModel();
    return model ? model->GetPreviewMocDigest() : QByteArray{};
}

bool OpengGlForm::CaptureMeshEditorState(CMeshEditorSnapshot& state, QString* error) const
{
    if (error) error->clear();
    if (CurrentPreviewDigest().isEmpty()) return MeshError(error, QStringLiteral("当前画布没有可用的模型选区"));
    const auto* model = CurrentModel();
    return model && model->CaptureMeshEditorState(state, error);
}

bool OpengGlForm::ApplyMeshGeometryPreview(const QByteArray& bytes, const QByteArray& sourceDigest,
                                          const CMeshGeometryMigration& migration, QString* error)
{
    if (error) error->clear();
    if (m_exportSuspended) return MeshError(error, QStringLiteral("导出期间不能修改网格几何"));
    if (m_meshEditCallbackActive || m_deformerCallbackActive || IsKeyformDragging() || m_controlDragActive || m_physicsDragActive || m_meshToolGesture)
        return MeshError(error, QStringLiteral("请先结束当前画布拖拽"));
    const RestoreContext restore;
    if (!IsModelReady() || !MakeRuntimeCurrent())
        return MeshError(error, QStringLiteral("模型预览的 OpenGL 上下文尚未就绪"));
    auto* model = CurrentModel();
    auto* view = LAppDelegate::GetInstance()->GetView();
    if (!model || !view) return MeshError(error, QStringLiteral("模型或变形器预览视图尚未就绪"));
    try {
        QByteArray compiled;
        if (!live2dstruct::CMocDeformerOrder::Normalize(bytes, compiled, error)) return false;
        CTreeManage::Snapshot tree;
        LAppView::PreparedDeformerOverlays overlays;
        const auto prepare = [&](const CubismModel& candidate, QString* detail) {
            CubismModel::RuntimeValueSnapshot values;
            return candidate.CaptureRuntimeValues(values, detail) &&
                CTreeManage::PrepareSnapshot(compiled, values.Parameters(), tree, detail) &&
                view->PrepareDeformerOverlays(tree, overlays, detail);
        };
        if (!model->ApplyMeshGeometryPreview(compiled, sourceDigest, migration, error, prepare)) return false;
        CTreeManage::SwapSnapshot(tree);
        view->SwapDeformerOverlays(overlays);
        update();
        if (!model->HasEditorPreview()) ResetMeshToolState();
        else NotifyMeshSelection();
        return true;
    } catch (const std::exception&) {
        return MeshError(error, QStringLiteral("无法准备网格几何的变形器预览"));
    }
}

bool OpengGlForm::ApplyDrawableMaterialPreview(const QByteArray& bytes, const QByteArray& sourceDigest,
                                              const QByteArray& expectedPreviewDigest, const QSet<QString>& allowedDrawableIds,
                                              QString* error)
{
    if (error) error->clear();
    if (m_exportSuspended || m_meshToolDispatchActive || m_meshEditCallbackActive || m_deformerCallbackActive ||
        IsKeyformDragging() || m_controlDragActive || m_physicsDragActive || m_meshToolGesture)
        return MeshError(error, QStringLiteral("请先结束当前画布操作或导出"));
    const RestoreContext restore;
    if (!IsModelReady() || !MakeRuntimeCurrent()) return MeshError(error, QStringLiteral("材质预览的 OpenGL 上下文尚未就绪"));
    auto* model = CurrentModel();
    auto* view = LAppDelegate::GetInstance()->GetView();
    if (!model || !view) return MeshError(error, QStringLiteral("模型或变形器预览视图尚未就绪"));
    try {
        QByteArray compiled;
        if (!live2dstruct::CMocDeformerOrder::Normalize(bytes, compiled, error)) return false;
        CTreeManage::Snapshot tree;
        LAppView::PreparedDeformerOverlays overlays;
        const auto prepare = [&](const CubismModel& candidate, QString* detail) {
            CubismModel::RuntimeValueSnapshot values;
            return candidate.CaptureRuntimeValues(values, detail) &&
                CTreeManage::PrepareSnapshot(compiled, values.Parameters(), tree, detail) &&
                view->PrepareDeformerOverlays(tree, overlays, detail);
        };
        if (!model->ApplyDrawableMaterialPreview(compiled, sourceDigest, expectedPreviewDigest, allowedDrawableIds, error, prepare)) return false;
        CTreeManage::SwapSnapshot(tree);
        view->SwapDeformerOverlays(overlays);
        update();
        return true;
    } catch (const std::exception&) {
        return MeshError(error, QStringLiteral("无法准备材质预览的图层与变形器视图"));
    }
}

bool OpengGlForm::ApplyAtlasPreview(const QByteArray& bytes, const QByteArray& sourceDigest,
                                              const QByteArray& expectedPreviewDigest, const QByteArray& expectedWorkspaceDigest,
                                     const LAppModel::AtlasState& workspace, const QSet<QString>& allowedDrawableIds,
                                              QString* error)
{
    if (error) error->clear();
    if (m_exportSuspended || m_meshToolDispatchActive || m_meshEditCallbackActive || m_deformerCallbackActive ||
        IsKeyformDragging() || m_controlDragActive || m_physicsDragActive || m_meshToolGesture)
        return MeshError(error, QStringLiteral("请先结束当前画布操作或导出"));
    const RestoreContext restore;
    if (!IsModelReady() || !MakeRuntimeCurrent()) return MeshError(error, QStringLiteral("图集预览的 OpenGL 上下文尚未就绪"));
    auto* model = CurrentModel();
    auto* view = LAppDelegate::GetInstance()->GetView();
    if (!model || !view) return MeshError(error, QStringLiteral("模型或变形器预览视图尚未就绪"));
    try {
        QByteArray compiled;
        if (!live2dstruct::CMocDeformerOrder::Normalize(bytes, compiled, error)) return false;
        CTreeManage::Snapshot tree;
        LAppView::PreparedDeformerOverlays overlays;
        const auto prepare = [&](const CubismModel& candidate, QString* detail) {
            CubismModel::RuntimeValueSnapshot values;
            return candidate.CaptureRuntimeValues(values, detail) &&
                CTreeManage::PrepareSnapshot(compiled, values.Parameters(), tree, detail) &&
                view->PrepareDeformerOverlays(tree, overlays, detail);
        };
        if (!model->ApplyAtlasPreview(compiled, sourceDigest, expectedPreviewDigest, expectedWorkspaceDigest, workspace, allowedDrawableIds, error, prepare)) return false;
        CTreeManage::SwapSnapshot(tree);
        view->SwapDeformerOverlays(overlays);
        update();
        return true;
    } catch (const std::exception&) {
        return MeshError(error, QStringLiteral("无法准备图集预览的图层与变形器视图"));
    }
}

bool OpengGlForm::ApplyParameterDefinitionPreview(const QByteArray& bytes, const QByteArray& sourceDigest,
                                                 const QMap<QString, float>& baseValues, bool keepEditorPreview,
                                                 QString* error)
{
    if (error) error->clear();
    if (m_meshToolDispatchActive) return MeshError(error, QStringLiteral("正在提交网格操作"));
    if (m_exportSuspended) return MeshError(error, QStringLiteral("导出期间不能修改参数定义"));
    if (m_meshEditCallbackActive || m_deformerCallbackActive || IsKeyformDragging() || m_controlDragActive || m_physicsDragActive || m_meshToolGesture) return MeshError(error, QStringLiteral("请先结束当前画布拖拽"));
    const RestoreContext restore;
    if (!IsModelReady() || !MakeRuntimeCurrent()) return MeshError(error, QStringLiteral("模型预览的 OpenGL 上下文尚未就绪"));
    auto* model = CurrentModel();
    auto* view = LAppDelegate::GetInstance()->GetView();
    if (!model || !view) return MeshError(error, QStringLiteral("模型或变形器预览视图尚未就绪"));
    try {
        QByteArray compiled;
        if (!live2dstruct::CMocDeformerOrder::Normalize(bytes, compiled, error)) return false;
        CTreeManage::Snapshot tree;
        LAppView::PreparedDeformerOverlays overlays;
        const auto prepare = [&](const CubismModel& candidate, QString* detail) {
            CubismModel::RuntimeValueSnapshot values;
            return candidate.CaptureRuntimeValues(values, detail) &&
                CTreeManage::PrepareSnapshot(compiled, values.Parameters(), tree, detail) &&
                view->PrepareDeformerOverlays(tree, overlays, detail);
        };
        if (!model->ApplyParameterDefinitionPreview(compiled, sourceDigest, baseValues, keepEditorPreview, error, prepare)) return false;
        CTreeManage::SwapSnapshot(tree);
        view->SwapDeformerOverlays(overlays);
        update();
        if (!keepEditorPreview) ResetMeshToolState();
        else NotifyMeshSelection();
        return true;
    } catch (const std::exception&) {
        return MeshError(error, QStringLiteral("无法准备参数定义的变形器预览"));
    }
}

bool OpengGlForm::ApplyParameterStructurePreview(const QByteArray& bytes, const QByteArray& sourceDigest,
                                                const ParameterStructureSeeds& addedSeeds,
                                                const QMap<QString, float>& baseValues, bool keepEditorPreview,
                                                QMap<QString, float>& preparedPose, QString* error,
                                                const ParameterReferenceState* references,
                                                const LAppModel::EditorPreviewContext* restoreEditor,
                                                const LAppModel::TimelinePreviewPreparation& prepareTimeline,
                                                const LAppModel::PhysicsDraft* physicsDraft,
                                                const CMeshGeometryMigration* geometry,
                                                const QMap<QString, float>* requestedEditorPose,
                                                const LAppModel::TextureState* textureState,
                                                const QSet<QString>* materialDrawableIds, const LAppModel::AtlasState* atlasState)
{
    if (error) error->clear();
    if (m_meshToolDispatchActive) return MeshError(error, QStringLiteral("正在提交网格操作"));
    if (m_exportSuspended) return MeshError(error, QStringLiteral("导出期间不能修改参数结构"));
    if (m_meshEditCallbackActive || m_deformerCallbackActive || IsKeyformDragging() || m_controlDragActive || m_physicsDragActive || m_meshToolGesture) return MeshError(error, QStringLiteral("请先结束当前画布拖拽"));
    const RestoreContext restore;
    if (!IsModelReady() || !MakeRuntimeCurrent()) return MeshError(error, QStringLiteral("模型预览的 OpenGL 上下文尚未就绪"));
    auto* model = CurrentModel();
    auto* view = LAppDelegate::GetInstance()->GetView();
    if (!model || !view) return MeshError(error, QStringLiteral("模型或变形器预览视图尚未就绪"));
    try {
        QByteArray compiled;
        if (!live2dstruct::CMocDeformerOrder::Normalize(bytes, compiled, error)) return false;
        CTreeManage::Snapshot tree;
        LAppView::PreparedDeformerOverlays overlays;
        QMap<QString, float> pose;
        const auto prepare = [&](const CubismModel& candidate, QString* detail) {
            CubismModel::RuntimeValueSnapshot values;
            if (!candidate.CaptureRuntimeValues(values, detail)) return false;
            pose = values.Parameters();
            return CTreeManage::PrepareSnapshot(compiled, pose, tree, detail) &&
                view->PrepareDeformerOverlays(tree, overlays, detail);
        };
        if (!model->ApplyParameterStructurePreview(compiled, sourceDigest, addedSeeds, baseValues,
                                                   keepEditorPreview, error, prepare, references, restoreEditor, prepareTimeline,
                                                   physicsDraft, geometry, requestedEditorPose, textureState, materialDrawableIds, atlasState)) return false;
        CTreeManage::SwapSnapshot(tree);
        view->SwapDeformerOverlays(overlays);
        preparedPose.swap(pose);
        update();
        if (!keepEditorPreview) ResetMeshToolState();
        else NotifyMeshSelection();
        return true;
    } catch (const std::exception&) {
        return MeshError(error, QStringLiteral("无法准备参数结构的变形器预览"));
    }
}

bool OpengGlForm::ApplyTextureState(const LAppModel::TextureState& state, QString* error)
{
    if (error) error->clear();
    if (m_exportSuspended || m_meshToolDispatchActive || m_meshEditCallbackActive || m_deformerCallbackActive ||
        IsKeyformDragging() || m_controlDragActive || m_physicsDragActive || m_meshToolGesture)
        return MeshError(error, QStringLiteral("请先结束当前画布操作或导出"));
    const RestoreContext restore;
    if (!IsModelReady() || !MakeRuntimeCurrent()) return MeshError(error, QStringLiteral("纹理预览尚未就绪"));
    auto* model = CurrentModel();
    if (!model || !model->ApplyTextureState(state, error)) return false;
    update();
    NotifyMeshSelection();
    return true;
}

bool OpengGlForm::EndEditorPreview(QString* error)
{
    const QPointer<OpengGlForm> alive(this);
    if (error) error->clear();
    if (m_exportSuspended) return MeshError(error, QStringLiteral("导出期间不能改变编辑姿态"));
    if (m_meshEditCallbackActive || m_deformerCallbackActive || IsKeyformDragging() || m_meshToolGesture || m_meshToolDispatchActive)
        return MeshError(error, QStringLiteral("请先结束当前网格操作"));
    auto* model = CurrentModel();
    if (!model || !model->HasEditorPreview()) { ResetMeshToolState(); return alive && ClearDeformerEditTarget(error); }
    const RestoreContext restore;
    if (!MakeRuntimeCurrent()) return MeshError(error, QStringLiteral("无法恢复模型预览的 OpenGL 上下文"));
    try {
        auto* view = LAppDelegate::GetInstance()->GetView();
        if (!view) return MeshError(error, QStringLiteral("无法恢复变形器预览视图"));
        CTreeManage::Snapshot tree;
        LAppView::PreparedDeformerOverlays overlays;
        if (!CTreeManage::PrepareSnapshot(model->GetPreviewMocBytes(), model->GetEditorBaseParameters(), tree, error) ||
            !view->PrepareDeformerOverlays(tree, overlays, error)) return false;
        if (!model->EndEditorPreview(error)) return false;
        CTreeManage::SwapSnapshot(tree);
        view->SwapDeformerOverlays(overlays);
        update();
        ResetMeshToolState();
        return alive && ClearDeformerEditTarget(error);
    } catch (const std::exception&) {
        return MeshError(error, QStringLiteral("无法准备编辑前的变形器预览"));
    }
}

bool OpengGlForm::FinishArtMeshDrag(bool cancel, QString* error)
{
    if (error) error->clear();
    if (m_meshEditCallbackActive) return MeshError(error, QStringLiteral("正在提交顶点拖拽"));
    if (m_meshToolDispatchActive) return MeshError(error, QStringLiteral("正在提交网格操作"));
    const QPointer<OpengGlForm> alive(this);
    CancelMeshToolGesture();
    if (!alive) return false;
    if (!IsMeshDragging()) return true;
    if (!m_meshFinish) return MeshError(error, QStringLiteral("尚未连接顶点拖拽提交器"));
    m_meshEditCallbackActive = true;
    const auto release = qScopeGuard([alive] { if (alive) alive->m_meshEditCallbackActive = false; });
    try {
        const auto finish = m_meshFinish;
        if (!finish(cancel, error) || !alive) {
            if (error && error->isEmpty()) *error = QStringLiteral("无法结束顶点拖拽，保留最后接受的状态");
            return false;
        }
    } catch (const std::exception&) {
        return MeshError(error, QStringLiteral("顶点拖拽提交失败，保留最后接受的状态"));
    }
    m_meshDrag.reset();
    m_ceditstate.RemoveState(CEditState::MousePress);
    update();
    return true;
}

bool OpengGlForm::FinishMeshDrag(bool cancel, QString* error)
{
    return FinishKeyformDrag(cancel, error);
}

bool OpengGlForm::CancelMeshDrag(QString* error)
{
    return FinishMeshDrag(true, error);
}

bool OpengGlForm::MeshDragFrameMatches() const
{
    if (!m_meshDrag || !m_runtimeReady || m_meshDrag->size != size()) return false;
    auto* view = LAppDelegate::GetInstance()->GetView();
    return view && view->GetMVP() == m_meshDrag->mvp;
}

QString OpengGlForm::SelectCoreMesh(const QPointF& pixel, bool hover)
{
    auto* appModel = CurrentModel();
    auto* delegate = LAppDelegate::GetInstance();
    auto* view = delegate->GetView();
    if (!appModel || !appModel->GetModel() || !view) return {};
    QPointF point;
    if (!CCanvasCoordinates::ScreenToModel(view->GetMVP(), size(), pixel, point)) return {};
    auto* model = appModel->GetModel();
    auto* map = model->GetMeshMap();
    if (!map) return {};
    QString preferred;
    if (!hover && m_meshEditEnabled && !m_meshEditName.isEmpty()) {
        const auto activeId = CubismFramework::GetIdManager()->GetId(m_meshEditName.toUtf8().constData());
        const int activeIndex = model->GetDrawableIndex(activeId);
        QVector<float> vertices;
        if (activeIndex >= 0 && !delegate->m_hiddenDrawables.contains(m_meshEditName) &&
            !delegate->m_hiddenDeformerDrawables.contains(m_meshEditName) &&
            model->GetDrawableDynamicFlagIsVisible(activeIndex) && CorePositions(*model, activeIndex, vertices) &&
            CCanvasCoordinates::NearestVertex(view->GetMVP(), size(), pixel, vertices) >= 0)
            preferred = m_meshEditName;
    }
    const auto order = model->GetSortedDrawableIndices(delegate->m_drawableDrawOrderOverrides,
        delegate->m_drawableOrderGroupIndices, delegate->m_drawOrderGroups);
    QString selected;
    for (auto it = order.crbegin(); it != order.crend(); ++it) {
        const int index = *it;
        if (index < 0 || index >= model->GetDrawableCount()) continue;
        const auto id = model->GetDrawableId(index);
        if (!id) continue;
        const QString name = QString::fromUtf8(id->GetString().GetRawString());
        auto* mesh = map->GetDrawMesh(name);
        QVector<float> positions;
        const bool visible = !delegate->m_hiddenDrawables.contains(name) &&
            !delegate->m_hiddenDeformerDrawables.contains(name) && model->GetDrawableDynamicFlagIsVisible(index);
        const bool hit = visible && selected.isEmpty() && mesh && CorePositions(*model, index, positions) &&
                         mesh->SetPositions(positions) &&
                         (preferred.isEmpty() ? mesh->ClickMesh(point) : name == preferred);
        if (hit) selected = name;
        if (hover && mesh) mesh->SetIsOver(hit);
    }
    return selected;
}

bool OpengGlForm::BeginMeshDrag(const QString& name, const QPointF& pixel, QString* error, bool activate)
{
    if (error) error->clear();
    if (m_meshEditCallbackActive || m_deformerCallbackActive || IsKeyformDragging())
        return MeshError(error, QStringLiteral("请先结束当前画布拖拽"));
    if (!m_meshActivate || !m_meshBegin || !m_meshMove || !m_meshFinish)
        return MeshError(error, QStringLiteral("尚未连接关键形编辑控制器"));
    const QPointer<OpengGlForm> alive(this);
    try {
        const auto activateMesh = m_meshActivate;
        if (activate && !activateMesh(name, m_activeKeyformIndex, error)) {
            if (error && error->isEmpty()) *error = QStringLiteral("关键形编辑姿态被拒绝");
            return false;
        }
        if (!alive) return false;
        auto* appModel = CurrentModel();
        auto* view = LAppDelegate::GetInstance()->GetView();
        if (!appModel || !appModel->GetModel() || !view) return MeshError(error, QStringLiteral("编辑模型已改变"));
        auto* model = appModel->GetModel();
        const auto id = CubismFramework::GetIdManager()->GetId(name.toUtf8().constData());
        QVector<float> vertices;
        if (!CorePositions(*model, model->GetDrawableIndex(id), vertices))
            return MeshError(error, QStringLiteral("当前 Core 顶点尚未就绪"));
        MeshDrag drag{name, m_activeKeyformIndex, {}, {}, view->GetMVP(), size()};
        if (!CCanvasCoordinates::ScreenToModel(drag.mvp, drag.size, pixel, drag.start))
            return MeshError(error, QStringLiteral("当前画布变换无法反解鼠标位置"));
        const int nearest = CCanvasCoordinates::NearestVertex(drag.mvp, drag.size, pixel, vertices);
        if (nearest < 0) return false;
        auto* map = model->GetMeshMap();
        auto* mesh = map ? map->GetDrawMesh(name) : nullptr;
        if (!mesh || !mesh->GetMesh() || !mesh->SetPositions(vertices))
            return MeshError(error, QStringLiteral("编辑网格与 Core 顶点不一致"));
        const auto& points = mesh->GetMesh()->GetCpoint();
        if (points.size() != static_cast<size_t>(vertices.size() / 2) || !points[nearest])
            return MeshError(error, QStringLiteral("网格选区与 Core 顶点数量不一致"));
        if (points[nearest]->GetIsSelect()) {
            for (int index = 0; index < int(points.size()); ++index)
                if (points[index] && points[index]->GetIsSelect()) drag.selected.append(index);
        } else drag.selected.append(nearest);
        m_meshEditCallbackActive = true;
        const auto release = qScopeGuard([alive] { if (alive) alive->m_meshEditCallbackActive = false; });
        const auto begin = m_meshBegin;
        if (!begin(drag.selected, vertices, error) || !alive) {
            if (error && error->isEmpty()) *error = QStringLiteral("无法开始顶点拖拽");
            return false;
        }
        m_meshDrag = std::move(drag);
        if (!MeshDragFrameMatches()) {
            m_meshEditCallbackActive = false;
            CancelMeshDrag(error);
            return MeshError(error, QStringLiteral("开始拖拽时画布变换已改变"));
        }
        appModel = CurrentModel();
        map = appModel && appModel->GetModel() ? appModel->GetModel()->GetMeshMap() : nullptr;
        mesh = map ? map->GetDrawMesh(name) : nullptr;
        if (!mesh || !mesh->GetMesh() || mesh->GetMesh()->GetCpoint().size() != static_cast<size_t>(vertices.size() / 2)) {
            m_meshEditCallbackActive = false;
            CancelMeshDrag(error);
            return MeshError(error, QStringLiteral("开始拖拽时网格已改变"));
        }
        for (auto* other : map->GetMapMesh()) if (other) other->CancelSelect();
        for (int index : m_meshDrag->selected) mesh->GetMesh()->GetCpoint()[index]->SetIsSelect(true);
        m_meshEditName = name;
        update();
        return true;
    } catch (const std::exception&) {
        return MeshError(error, QStringLiteral("无法开始关键形拖拽"));
    }
}

bool OpengGlForm::MakeRuntimeCurrent()
{
    if (!m_runtimeReady || !isValid()) return false;
    if (LAppDelegate::GetInstance()->getGlOwningContext() != context()) return false;
    if (QOpenGLContext::currentContext() != context()) makeCurrent();
    return QOpenGLContext::currentContext() == context();
}

bool OpengGlForm::do_ModelChange(const QString& fileName)
{
    if (m_exportSuspended) return false;
    return ChangeModel(fileName, nullptr);
}

bool OpengGlForm::LoadExportModel(const QString& fileName, QString* error)
{
    if (error) error->clear();
    if (!m_exportSuspended) return MeshError(error, QStringLiteral("导出模型只能在画布暂停时载入"));
    return ChangeModel(fileName, error);
}

bool OpengGlForm::ChangeModel(const QString& fileName, QString* error)
{
    const QPointer<OpengGlForm> alive(this);
    if (error) error->clear();
    if (fileName.isEmpty()) return MeshError(error, QStringLiteral("模型路径为空"));
    const RestoreContext restore;
    if (!MakeRuntimeCurrent()) return MeshError(error, QStringLiteral("无法使用模型预览的 OpenGL 上下文"));
    if (m_suspendedModel.has_value() || LAppLive2DManager::GetInstance()->HasTemporaryScene())
        return MeshError(error, QStringLiteral("请先恢复临时模型再载入下一个模型"));
    if (auto* model = CurrentModel(); model && model->HasExportPreview())
        return MeshError(error, QStringLiteral("请先恢复导出姿态再载入下一个模型"));
    if (m_meshToolDispatchActive) return MeshError(error, QStringLiteral("正在提交网格操作"));
    if (!FinishMeshDrag(true, error)) return false;
    if (!alive) return false;
    const QString nextPath(fileName.constData(), fileName.size());
    FinishDeformerDrag();
    const bool loaded = LAppDelegate::GetInstance()->ChangeScene(fileName);
    if (!loaded) return MeshError(error, QStringLiteral("无法载入模型，原模型保持不变：%1").arg(fileName));
    m_onionRenderer.reset();
    m_ceditstate.AddState(CEditState::ModelOpen);
    m_currentModelJson = nextPath;
    ResetMeshToolState();
    return true;
}

bool OpengGlForm::BeginTemporaryModel(const QString& fileName, QString* error)
{
    if (error) error->clear();
    if (m_meshToolDispatchActive || m_meshEditCallbackActive || m_deformerCallbackActive || IsKeyformDragging() || m_meshToolGesture)
        return MeshError(error, QStringLiteral("请先结束当前网格操作"));
    if (m_suspendedModel.has_value()) return MeshError(error, QStringLiteral("上一个临时模型尚未恢复"));
    const RestoreContext restore;
    if (!MakeRuntimeCurrent()) return MeshError(error, QStringLiteral("无法使用临时导出的 OpenGL 上下文"));
    auto* manager = LAppLive2DManager::GetInstance();
    if (manager->HasTemporaryScene()) return MeshError(error, QStringLiteral("已有临时模型占用预览"));
    FinishDeformerDrag();
    m_suspendedModel = SuspendedModel{m_currentModelJson, IsModelReady(), CurrentModel()};
    const bool loaded = LAppDelegate::GetInstance()->BeginTemporaryScene(fileName);
    if (!loaded) return MeshError(error, QStringLiteral("无法加载临时导出模型：%1").arg(fileName));
    m_currentModelJson = fileName;
    m_ceditstate.AddState(CEditState::ModelOpen);
    ResetMeshToolState();
    return true;
}

bool OpengGlForm::EndTemporaryModel(QString* error)
{
    if (error) error->clear();
    const RestoreContext restore;
    if (!MakeRuntimeCurrent()) return MeshError(error, QStringLiteral("无法恢复原模型的 OpenGL 上下文"));
    auto* manager = LAppLive2DManager::GetInstance();
    if (!m_suspendedModel.has_value())
        return !manager->HasTemporaryScene() || MeshError(error, QStringLiteral("临时模型缺少原预览身份"));
    if (manager->HasTemporaryScene() && !LAppDelegate::GetInstance()->EndTemporaryScene())
        return MeshError(error, QStringLiteral("原模型或纹理尚未恢复"));
    if (CurrentModel() != m_suspendedModel->identity)
        return MeshError(error, QStringLiteral("当前模型与导出前的原模型不一致"));
    m_currentModelJson = m_suspendedModel->path;
    if (m_suspendedModel->ready) m_ceditstate.AddState(CEditState::ModelOpen);
    else m_ceditstate.RemoveState(CEditState::ModelOpen);
    m_suspendedModel.reset();
    ResetMeshToolState();
    return true;
}

bool OpengGlForm::SetExportSuspended(bool suspended, QString* error)
{
    if (error) error->clear();
    if (suspended == m_exportSuspended) return true;
    if (suspended) {
        if (m_meshEditCallbackActive || m_deformerCallbackActive || IsKeyformDragging() || m_controlDragActive || m_physicsDragActive || m_meshToolGesture || m_meshToolDispatchActive)
            return MeshError(error, QStringLiteral("请先结束当前画布拖拽再导出"));
        m_ceditstate.RemoveState(CEditState::MousePress);
        m_ceditstate.RemoveState(CEditState::ViewMove);
        m_exportSuspended = true;
        return true;
    }
    const RestoreContext restore;
    if (!MakeRuntimeCurrent()) return MeshError(error, QStringLiteral("无法恢复预览的 OpenGL 上下文"));
    if (m_suspendedModel.has_value() || LAppLive2DManager::GetInstance()->HasTemporaryScene())
        return MeshError(error, QStringLiteral("临时模型尚未恢复，预览保持暂停"));
    if (auto* model = CurrentModel()) {
        auto* renderer = model->GetRenderer<Live2D::Cubism::Framework::Rendering::CubismRenderer_OpenGLES2>();
        if (model->HasExportPreview() || (renderer && renderer->HasPsdExport()))
            return MeshError(error, QStringLiteral("导出姿态或 GPU 会话尚未恢复，预览保持暂停"));
    }
    if (m_deferredResize && m_deferredResize->width() > 0 && m_deferredResize->height() > 0)
        LAppDelegate::GetInstance()->resize(m_deferredResize->width(), m_deferredResize->height());
    LAppPal::ResetTime();
    m_deferredResize.reset();
    m_exportSuspended = false;
    return true;
}

bool OpengGlForm::RecoverExportResources(QString* error)
{
    if (error) error->clear();
    const RestoreContext restore;
    if (!MakeRuntimeCurrent()) return MeshError(error, QStringLiteral("无法恢复导出资源的所属 OpenGL 上下文"));
    return LAppDelegate::GetInstance()->RecoverPsdExport(error);
}

bool OpengGlForm::HighPrecisionMasks() const
{
    if (!IsModelReady()) return false;
    auto* model = CurrentModel();
    auto* renderer = model ? model->GetRenderer<Live2D::Cubism::Framework::Rendering::CubismRenderer_OpenGLES2>() : nullptr;
    return renderer && renderer->IsUsingHighPrecisionMask();
}

bool OpengGlForm::SetHighPrecisionMasks(bool enabled, QString* error)
{
    if (error) error->clear();
    const RestoreContext restore;
    if (!IsModelReady() || !MakeRuntimeCurrent()) return MeshError(error, QStringLiteral("模型预览尚未就绪"));
    auto* model = CurrentModel();
    auto* renderer = model ? model->GetRenderer<Live2D::Cubism::Framework::Rendering::CubismRenderer_OpenGLES2>() : nullptr;
    if (!renderer || renderer->IsDrawing() || renderer->HasPsdExport() || LAppDelegate::GetInstance()->IsOffScreen())
        return MeshError(error, QStringLiteral("请等待当前绘制或导出结束后更改蒙版精度"));
    renderer->UseHighPrecisionMask(enabled);
    update();
    return true;
}

void OpengGlForm::SyncPreviewView()
{
    if (m_exportSuspended) return;
    const RestoreContext restore;
    if (!MakeRuntimeCurrent()) return;
    auto* manager = LAppLive2DManager::GetInstance();
    auto* model = manager && manager->GetModelNum() > 0 ? manager->GetModel(0) : nullptr;
    if (!model || !model->GetModel()) return;
    auto* coreModel = model->GetModel();
    auto* meshMap = coreModel->GetMeshMap();
    if (meshMap) for (int index = 0; index < coreModel->GetDrawableCount(); ++index) {
        const QString id = QString::fromUtf8(coreModel->GetDrawableId(index)->GetString().GetRawString());
        QVector<float> positions;
        if (CorePositions(*coreModel, index, positions)) meshMap->SetMeshPositions(id, positions);
    }
    update();
}



void OpengGlForm::initializeGL()
{
    QObject::disconnect(m_onionContextConnection);
    m_onionContextConnection = connect(context(), &QOpenGLContext::aboutToBeDestroyed, this, [this] {
        const RestoreContext restore;
        makeCurrent();
        m_onionRenderer.reset();
    }, Qt::DirectConnection);
    auto* delegate = LAppDelegate::GetInstance();
    const bool loaded = delegate->Initialize(this, m_config.GetInitFileName());
    m_runtimeReady = delegate->IsRuntimeReady() && delegate->getGlOwningContext() == context();
    if (loaded) m_ceditstate.AddState(CEditState::ModelOpen);
    else m_ceditstate.RemoveState(CEditState::ModelOpen);
    m_initializationCompleted = true;
    emit RuntimeInitialized(loaded);
}

void OpengGlForm::resizeGL(int width, int height)
{
    const QPointer<OpengGlForm> alive(this);
    if (m_exportSuspended) { m_deferredResize = QSize(width, height); return; }
    FinishDeformerDrag();
    if (IsKeyformDragging() || m_meshToolGesture) {
        QString error;
        if (!CancelMeshDrag(&error) && alive) emit MeshEditRejected(error);
        if (!alive) return;
    }
    if (m_runtimeReady) LAppDelegate::GetInstance()->resize(width,height);
}

void OpengGlForm::paintGL()
{
    if (m_exportSuspended || !m_runtimeReady) return;
    auto* delegate = LAppDelegate::GetInstance();
    delegate->paint();
    if (m_meshToolGesture && m_meshToolGesture->rectangle) {
        QPainter painter(this);
        const QColor highlight = palette().color(QPalette::Highlight);
        QColor fill = highlight;
        fill.setAlpha(40);
        painter.setPen(QPen(highlight, 1, Qt::DashLine));
        painter.setBrush(fill);
        painter.drawRect(QRectF(m_meshToolGesture->snapshot.pixelPosition, m_meshToolGesture->currentPixel).normalized());
    }
    RefreshDeformerOverlay();
    auto* view = delegate->GetView();
    const QString error = view ? view->DeformerOverlayError() : QString();
    if (error == m_lastPreviewError) return;
    m_lastPreviewError = error;
    if (!error.isEmpty()) emit PreviewFailed(error);
}


void OpengGlForm::mousePressEvent(QMouseEvent *event)
{
    const QPointer<OpengGlForm> alive(this);
    if (m_exportSuspended || m_meshToolDispatchActive || m_meshEditCallbackActive || m_deformerCallbackActive) { event->accept(); return; }
    QString error;
    if (!CancelMeshDrag(&error)) { if (alive) emit MeshEditRejected(error); return; }
    if (!alive) return;
    FinishDeformerDrag();
    if (!IsModelReady()) return;
    auto* view = LAppDelegate::GetInstance()->GetView();
    if (!view || !ViewPoint(*view, size(), event->position(), m_pointerAnchor)) return;
    view->GetMouseSelectRect()->SetStartAndEndPos({}, {});
    m_ceditstate.AddState(CEditState::MousePress);
    if (event->button() == Qt::RightButton) {
        m_ceditstate.AddState(CEditState::ViewMove);
        return;
    }
    if (m_ceditstate.IsContins(CEditState::ViewMove) || event->button() != Qt::LeftButton) return;
    if (auto* model = CurrentModel(); m_physicsDragEnabled && model && model->CanPhysicsDrag()) {
        if (model->HasEditorPreview()) {
            m_ceditstate.RemoveState(CEditState::MousePress);
            emit MeshEditRejected(QStringLiteral("请先退出关键形编辑再测试物理"));
            return;
        }
        m_physicsDragActive = true;
        m_physicsDragStart = event->position();
        model->ApplyPhysicsDrag(0, 0);
        return;
    }
    if (event->modifiers() & Qt::ControlModifier) {
        if (auto* model = CurrentModel(); model && model->HasEditorPreview()) {
            m_ceditstate.RemoveState(CEditState::MousePress);
            emit MeshEditRejected(QStringLiteral("请先退出关键形编辑再驱动变形器"));
            return;
        }
        auto* delegate = LAppDelegate::GetInstance();
        if (delegate->m_dragTargetDrawables.isEmpty() || delegate->StartDeformerDrag(&error)) {
            m_controlDragActive = true;
        } else {
            m_ceditstate.RemoveState(CEditState::MousePress);
            emit MeshEditRejected(error);
        }
        return;
    }
    event->ignore();
    const bool deformer = BeginDeformerGesture(event, &error);
    if (!alive) return;
    if (deformer || event->isAccepted()) {
        if (!deformer) {
            m_ceditstate.RemoveState(CEditState::MousePress);
            if (!error.isEmpty()) emit MeshEditRejected(error);
        }
        return;
    }
    event->accept();
    if (!m_meshToolTarget.isEmpty()) {
        event->accept();
        if (!BeginMeshToolGesture(event, &error)) {
            m_ceditstate.RemoveState(CEditState::MousePress);
            emit MeshEditRejected(error);
        }
        return;
    }
    const QString name = SelectCoreMesh(event->position(), false);
    emit SendSelectMesh(name);
    if (!alive || name.isEmpty()) return;
    if (m_meshEditEnabled && !BeginMeshDrag(name, event->position(), &error) && alive && !error.isEmpty())
        emit MeshEditRejected(error);
}

void OpengGlForm::mouseReleaseEvent(QMouseEvent *event)
{
    const QPointer<OpengGlForm> alive(this);
    if (m_exportSuspended || m_meshToolDispatchActive || m_meshEditCallbackActive || m_deformerCallbackActive) { event->accept(); return; }
    if (m_deformerGesture) {
        event->accept();
        QString error;
        const bool cancel = event->button() != Qt::LeftButton || !DeformerFrameMatches();
        if (!FinishDeformerGesture(cancel, &error) && alive) emit MeshEditRejected(error);
        return;
    }
    if (m_meshToolGesture) {
        event->accept();
        FinishMeshToolGesture(event);
        return;
    }
    if (IsMeshDragging()) {
        QString error;
        const bool cancel = event->button() != Qt::LeftButton || !MeshDragFrameMatches();
        if (!FinishMeshDrag(cancel, &error)) { if (alive) emit MeshEditRejected(error); return; }
        if (!alive) return;
    }
    m_ceditstate.RemoveState(CEditState::MousePress);
    if (event->button() == Qt::RightButton) m_ceditstate.RemoveState(CEditState::ViewMove);
    FinishDeformerDrag();
    if (!IsModelReady()) return;
    auto* delegate = LAppDelegate::GetInstance();
    if (auto* view = delegate->GetView()) view->GetMouseSelectRect()->SetStartAndEndPos({}, {});
}

void OpengGlForm::FinishDeformerDrag()
{
    if (m_physicsDragActive) m_ceditstate.RemoveState(CEditState::MousePress);
    m_physicsDragActive = false;
    if (m_controlDragActive) m_ceditstate.RemoveState(CEditState::MousePress);
    m_controlDragActive = false;
    if (!m_runtimeReady) return;
    auto* delegate = LAppDelegate::GetInstance();
    if (delegate->getGlOwningContext() != context()) return;
    if (auto* model = CurrentModel()) model->EndPhysicsDrag();
    if (delegate->m_isDraggingDeformer) m_ceditstate.RemoveState(CEditState::MousePress);
    delegate->StopDeformerDrag();
}

void OpengGlForm::mouseMoveEvent(QMouseEvent *event)
{
    const QPointer<OpengGlForm> alive(this);
    if (m_exportSuspended || m_meshToolDispatchActive || m_meshEditCallbackActive || m_deformerCallbackActive) { event->accept(); return; }
    if (m_physicsDragActive) {
        if (!(event->buttons() & Qt::LeftButton) || !m_physicsDragEnabled || !IsModelReady() || width() <= 0 || height() <= 0) {
            FinishDeformerDrag();
            return;
        }
        if (auto* model = CurrentModel()) {
            const QPointF delta = event->position() - m_physicsDragStart;
            model->ApplyPhysicsDrag(float(delta.x() * 2.0 / width()), float(-delta.y() * 2.0 / height()));
        }
        return;
    }
    if (m_deformerGesture) {
        event->accept();
        QString error;
        if (!MoveDeformerGesture(event, &error) && alive && !error.isEmpty()) emit MeshEditRejected(error);
        return;
    }
    if (IsMeshDragging()) {
        QString error;
        if (!(event->buttons() & Qt::LeftButton) || !m_meshEditEnabled || !MeshDragFrameMatches()) {
            if (!CancelMeshDrag(&error) && alive) emit MeshEditRejected(error);
            if (!alive) return;
            return;
        }
        if (!MoveMeshDrag(event->position(), &error) && alive) emit MeshEditRejected(error);
        return;
    }
    if (m_meshToolGesture) {
        event->accept();
        QString error;
        if (!MoveMeshToolGesture(event, &error) && !error.isEmpty()) emit MeshEditRejected(error);
        return;
    }
    if (!IsModelReady()) return;
    auto* delegate = LAppDelegate::GetInstance();
    if (m_controlDragActive &&
        (!(event->buttons() & Qt::LeftButton) || !(event->modifiers() & Qt::ControlModifier))) {
        FinishDeformerDrag();
        return;
    }
    auto* view = delegate->GetView();
    if (!view || width() <= 0 || height() <= 0) return;
    const float viewX = float(event->position().x() / width() * 2.0 - 1.0);
    const float viewY = float(1.0 - event->position().y() / height() * 2.0);
    if (!std::isfinite(viewX) || !std::isfinite(viewY)) return;
    if (m_ceditstate.IsContins(CEditState::MousePress)) {
        QPointF current;
        if (!ViewPoint(*view, size(), event->position(), current)) return;
        if (m_ceditstate.IsContins(CEditState::ViewMove)) {
            const QPointF delta = current - m_pointerAnchor;
            if (std::abs(delta.x()) > std::numeric_limits<float>::max() ||
                std::abs(delta.y()) > std::numeric_limits<float>::max()) return;
            QMatrix4x4 matrix = view->GetViewMatrix();
            matrix.translate(float(delta.x()), float(delta.y()));
            if (!FiniteMatrix(matrix)) return;
            view->SetViewMatrix(matrix);
        } else if (event->modifiers() & Qt::ControlModifier) {
            if (!m_controlDragActive || !(event->buttons() & Qt::LeftButton)) return;
            auto* model = CurrentModel();
            if (!model || model->HasEditorPreview()) return;
            if (delegate->m_isDraggingDeformer && !model->GetDeformerDragParamIds().isEmpty()) model->ApplyDeformerDrag(viewX, viewY);
            else model->SetDragging(viewX, viewY);
        } else {
            view->GetMouseSelectRect()->SetStartAndEndPos(QVector2D(m_pointerAnchor), QVector2D(current));
        }
    } else {
        if (m_meshToolTarget.isEmpty() && !m_ceditstate.IsContins(CEditState::ViewMove)) SelectCoreMesh(event->position(), true);
        auto* model = CurrentModel();
        if (model && !model->HasEditorPreview()) {
            if (model->GetEnableMouseFollow()) model->SetDragging(viewX, viewY);
        }
    }
}

void OpengGlForm::wheelEvent(QWheelEvent *event)
{
    const QPointer<OpengGlForm> alive(this);
    if (m_exportSuspended || m_meshToolDispatchActive || m_meshEditCallbackActive || m_deformerCallbackActive) { event->accept(); return; }
    QString error;
    if (!CancelMeshDrag(&error)) { if (alive) emit MeshEditRejected(error); return; }
    if (!alive) return;
    FinishDeformerDrag();
    if (!IsModelReady() || !event->angleDelta().y()) return;
    auto* view = LAppDelegate::GetInstance()->GetView();
    QPointF before;
    if (!view || !ViewPoint(*view, size(), event->position(), before)) return;
    const auto previousMatrix = view->GetViewMatrix();
    const float previousScale = ViewScale();
    const float nextScale = qBound(0.1f, previousScale + (event->angleDelta().y() > 0 ? 0.2f : -0.2f), 20.0f);
    SetViewState(nextScale, ViewPosition());
    QPointF after;
    if (!ViewPoint(*view, size(), event->position(), after) ||
        std::abs(after.x() - before.x()) > std::numeric_limits<float>::max() ||
        std::abs(after.y() - before.y()) > std::numeric_limits<float>::max()) {
        view->SetViewMatrix(previousMatrix);
        m_config.SetScalefactor(previousScale);
        return;
    }
    auto matrix = view->GetViewMatrix();
    matrix.translate(float(after.x() - before.x()), float(after.y() - before.y()));
    if (!FiniteMatrix(matrix)) {
        view->SetViewMatrix(previousMatrix);
        m_config.SetScalefactor(previousScale);
        return;
    }
    view->SetViewMatrix(matrix);
}

void OpengGlForm::keyPressEvent(QKeyEvent *event)
{
    const QPointer<OpengGlForm> alive(this);
    if (m_exportSuspended || m_meshToolDispatchActive || m_meshEditCallbackActive || m_deformerCallbackActive) { event->accept(); return; }
    if (m_meshToolGesture && (event->key() == Qt::Key_Escape || event->key() == Qt::Key_Control ||
        event->key() == Qt::Key_Space || event->key() == Qt::Key_Up || event->key() == Qt::Key_Down ||
        event->key() == Qt::Key_W || event->key() == Qt::Key_A || event->key() == Qt::Key_S ||
        event->key() == Qt::Key_D || event->key() == Qt::Key_Z)) {
        CancelMeshToolGesture();
        if (event->key() == Qt::Key_Escape) { event->accept(); return; }
    }
    if (!m_meshToolTarget.isEmpty() && !IsKeyformDragging()) {
        if (event->key() == Qt::Key_Escape) {
            QString error;
            MeshToolSnapshot snapshot;
            const bool anchor = m_meshTool == MeshTool::Connect &&
                CaptureMeshToolSnapshot(QPointF(width() / 2.0, height() / 2.0), snapshot, &error) && snapshot.selectedVertices.size() == 1;
            if (anchor ? !SetMeshToolSelection(snapshot, {}, &error) : !ClearMeshTool(&error)) emit MeshEditRejected(error);
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace ||
            ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) && m_meshTool == MeshTool::Connect)) {
            event->accept();
            if (event->isAutoRepeat()) return;
            QString error;
            MeshToolSnapshot snapshot;
            if (!CaptureMeshToolSnapshot(QPointF(width() / 2.0, height() / 2.0), snapshot, &error)) {
                emit MeshEditRejected(error);
                return;
            }
            snapshot.operationVertices = snapshot.selectedVertices;
            DispatchMeshTool(event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace ? MeshTool::Delete : MeshTool::Connect,
                             std::move(snapshot));
            return;
        }
        if (event->matches(QKeySequence::SelectAll)) {
            event->accept();
            QString error;
            MeshToolSnapshot snapshot;
            if (!CaptureMeshToolSnapshot(QPointF(width() / 2.0, height() / 2.0), snapshot, &error)) {
                emit MeshEditRejected(error);
                return;
            }
            QVector<int> selected;
            selected.reserve(snapshot.coreVertices.size() / 2);
            for (int index = 0; index < snapshot.coreVertices.size() / 2; ++index) selected.append(index);
            if (!SetMeshToolSelection(snapshot, selected, &error)) emit MeshEditRejected(error);
            return;
        }
    }
    if (m_physicsDragActive && event->key() == Qt::Key_Escape) {
        FinishDeformerDrag();
        return;
    }
    if (m_controlDragActive &&
        (event->key() == Qt::Key_Escape || event->key() == Qt::Key_Space || event->key() == Qt::Key_Up ||
         event->key() == Qt::Key_Down || event->key() == Qt::Key_W || event->key() == Qt::Key_A ||
         event->key() == Qt::Key_S || event->key() == Qt::Key_D)) {
        FinishDeformerDrag();
        if (event->key() == Qt::Key_Escape) return;
    }
    if (event->key() == Qt::Key_Escape && !IsKeyformDragging() && m_runtimeReady) {
        auto* model = CurrentModel();
        if (model && model->HasEditorPreview()) {
            emit KeyformEditingFinishedRequested();
            return;
        }
    }
    if (IsKeyformDragging() && (event->key() == Qt::Key_Escape || event->key() == Qt::Key_Control ||
        event->key() == Qt::Key_Space || event->key() == Qt::Key_Up || event->key() == Qt::Key_Down ||
        event->key() == Qt::Key_W || event->key() == Qt::Key_A || event->key() == Qt::Key_S ||
        event->key() == Qt::Key_D || event->key() == Qt::Key_Z)) {
        QString error;
        if (!CancelMeshDrag(&error)) { if (alive) emit MeshEditRejected(error); return; }
        if (!alive) return;
        if (event->key() == Qt::Key_Escape) return;
    }
    if (event->modifiers() & (Qt::ControlModifier | Qt::MetaModifier)) {
        event->ignore();
        return;
    }
    if (!IsModelReady()) return;

    LAppView* view = LAppDelegate::GetInstance()->GetView();
    if(!view) return;
    constexpr float moveStep = 0.1f;
    constexpr float scaleStep = 0.2f;

    switch (event->key()) {
        case Qt::Key_Up: {
            SetViewState(ViewScale() + scaleStep, ViewPosition());
            break;
        }
        case Qt::Key_Down: {
            SetViewState(qMax(0.1f, ViewScale() - scaleStep), ViewPosition());
            break;
        }
        case Qt::Key_W: {
            QMatrix4x4 temp = view->GetViewMatrix();
            QMatrix4x4 move;
            move.setToIdentity();
            move.translate(0, moveStep, 0);
            temp = temp * move;
            view->SetViewMatrix(temp);
            break;
        }
        case Qt::Key_Space:
            m_ceditstate.AddState(CEditState::ViewMove);
            break;
        case Qt::Key_D: {
            QMatrix4x4 temp = view->GetViewMatrix();
            QMatrix4x4 move;
            move.setToIdentity();
            move.translate(moveStep, 0, 0);
            temp = temp * move;
            view->SetViewMatrix(temp);
            break;
        }
        case Qt::Key_A: {
            QMatrix4x4 temp = view->GetViewMatrix();
            QMatrix4x4 move;
            move.setToIdentity();
            move.translate(-moveStep, 0, 0);
            temp = temp * move;
            view->SetViewMatrix(temp);
            break;
        }
        case Qt::Key_S: {
            QMatrix4x4 temp = view->GetViewMatrix();
            QMatrix4x4 move;
            move.setToIdentity();
            move.translate(0, -moveStep, 0);
            temp = temp * move;
            view->SetViewMatrix(temp);
            break;
        }
        case Qt::Key_Z: {
            LAppLive2DManager* mgr = LAppLive2DManager::GetInstance();
            if(mgr && mgr->GetModelNum() > 0 && mgr->GetModel(0) && mgr->GetModel(0)->GetModel()){
                CMeshMap* map = mgr->GetModel(0)->GetModel()->GetMeshMap();
                if(map) map->SetShow("");
            }
            break;
        }
        default:break;
    }


}

void OpengGlForm::keyReleaseEvent(QKeyEvent *event)
{
    if (m_exportSuspended || m_meshToolDispatchActive || m_meshEditCallbackActive || m_deformerCallbackActive) { event->accept(); return; }
    if (event->key() == Qt::Key_Control && !event->isAutoRepeat()) FinishDeformerDrag();
    if (!IsModelReady()) return;
    switch (event->key()) {
    case Qt::Key_Up:;break;
    case Qt::Key_Down:;break;
    case Qt::Key_W: ;break;
    case Qt::Key_Space:
        m_ceditstate.RemoveState(CEditState::ViewMove);
        break;
    case Qt::Key_D: ;break;
    case Qt::Key_A:break;
    case Qt::Key_Z: break;
    default:break;

    }
}

void OpengGlForm::focusOutEvent(QFocusEvent* event)
{
    const QPointer<OpengGlForm> alive(this);
    if (m_exportSuspended || m_meshToolDispatchActive || m_meshEditCallbackActive || m_deformerCallbackActive) { QOpenGLWidget::focusOutEvent(event); return; }
    QString error;
    if (!CancelMeshDrag(&error) && alive) emit MeshEditRejected(error);
    if (!alive) return;
    FinishDeformerDrag();
    m_ceditstate.RemoveState(CEditState::MousePress);
    m_ceditstate.RemoveState(CEditState::ViewMove);
    QOpenGLWidget::focusOutEvent(event);
}

void OpengGlForm::hideEvent(QHideEvent* event)
{
    const QPointer<OpengGlForm> alive(this);
    if (m_exportSuspended || m_meshToolDispatchActive || m_meshEditCallbackActive || m_deformerCallbackActive) { QOpenGLWidget::hideEvent(event); return; }
    QString error;
    if (!CancelMeshDrag(&error) && alive) emit MeshEditRejected(error);
    if (!alive) return;
    FinishDeformerDrag();
    m_ceditstate.RemoveState(CEditState::MousePress);
    m_ceditstate.RemoveState(CEditState::ViewMove);
    QOpenGLWidget::hideEvent(event);
}
