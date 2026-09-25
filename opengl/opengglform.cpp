#include "opengglform.h"
#include "edit/cmeshmap.h"
#include "ccanvascoordinates.h"
#include "framework/Id/CubismIdManager.hpp"
#include "framework/Rendering/OpenGL/CubismRenderer_OpenGLES2.hpp"
#include <QSurfaceFormat>
#include <QScopeGuard>
#include <QScopedValueRollback>
#include <QFocusEvent>
#include <QHideEvent>
#include <cmath>
#include <exception>
#include <limits>

namespace {
class RestoreContext
{
public:
    RestoreContext() : previous(QOpenGLContext::currentContext()), surface(previous ? previous->surface() : nullptr) {}
    ~RestoreContext()
    {
        if (QOpenGLContext::currentContext() == previous) return;
        if (auto* current = QOpenGLContext::currentContext()) current->doneCurrent();
        if (previous && surface) previous->makeCurrent(surface);
    }
private:
    QPointer<QOpenGLContext> previous;
    QSurface* surface;
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
    LAppDelegate::GetInstance()->Release();
    LAppDelegate::ReleaseInstance();
    if (context()) doneCurrent();
}

void OpengGlForm::SetInitOpenName(const QString &Name)
{
    m_config.SetInitFileName(Name);
    m_currentModelJson = Name;
}



void OpengGlForm::on_timeout()
{
    update();
}

bool OpengGlForm::do_OffSeceen(const QString& fileName,bool IsALl,bool IsFenzu,bool cubismCompat)
{
    bool ok = do_OffSeceenSilent(fileName,IsALl,IsFenzu,cubismCompat);

    if (!fileName.isEmpty()) {
        if (ok) {
            QMessageBox::about(this,"psd","保存成功");
            QFileInfo fileInfo(fileName);
            QString directoryPath = fileInfo.absoluteDir().path();
            QDesktopServices::openUrl(QUrl::fromLocalFile(directoryPath));
        } else {
            QMessageBox::warning(this,"psd",
                QString("导出失败，无法写入文件：\n%1\n请检查磁盘空间、路径权限或文件是否被占用").arg(fileName));
        }
    }
    return ok;
}

bool OpengGlForm::do_OffSeceenSilent(const QString &fileName, bool IsALl, bool IsFenzu, bool cubismCompat)
{
    if (fileName.isEmpty() || !IsModelReady() || !MakeRuntimeCurrent()) return false;
    const auto releaseContext = qScopeGuard([this] { doneCurrent(); });
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

    const auto clearPrintState = qScopeGuard([this] {
        m_ceditstate.RemoveState(CEditState::PrintAllPsd);
        m_ceditstate.RemoveState(CEditState::PrintFenzuPsd);
    });
    return LAppDelegate::GetInstance()->StartOffset_FunpsdList(fileName,IsALl,IsFenzu,cubismCompat);
}

bool OpengGlForm::do_TextureExtract(const QString& fileName, bool isAll, bool isFenzu, bool cubismCompat)
{
    bool ok = do_TextureExtractSilent(fileName, isAll, isFenzu, cubismCompat);
    if (!fileName.isEmpty()) {
        if (ok) {
            QMessageBox::about(this, "psd", "保存成功（原始纹理提取）");
            QFileInfo fileInfo(fileName);
            QString directoryPath = fileInfo.absoluteDir().path();
            QDesktopServices::openUrl(QUrl::fromLocalFile(directoryPath));
        } else {
            QMessageBox::warning(this, "psd",
                QString("导出失败，无法写入文件：\n%1\n请检查磁盘空间、路径权限或文件是否被占用").arg(fileName));
        }
    }
    return ok;
}

bool OpengGlForm::do_TextureExtractSilent(const QString& fileName, bool isAll, bool isFenzu, bool cubismCompat)
{
    if (!fileName.isEmpty() && m_runtimeReady && IsModelReady()) {
        return LAppDelegate::GetInstance()->StartTextureExtraction(fileName, isAll, isFenzu, cubismCompat);
    }
    return false;
}

void OpengGlForm::do_ShowMesh(const QString &artName)
{
    if (IsMeshDragging() && m_meshDrag->name != artName) {
        QString error;
        if (!CancelMeshDrag(&error)) { emit MeshEditRejected(error); return; }
    }
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
    if (!enabled && IsMeshDragging()) {
        QString error;
        if (!CancelMeshDrag(&error)) { emit MeshEditRejected(error); return; }
    }
    m_meshEditEnabled = enabled;
    if (!enabled) {
        m_meshEditName.clear();
    }
}

void OpengGlForm::SetActiveKeyform(const QString& name, int keyformIndex)
{
    if (name.isEmpty() || keyformIndex < 0) return;
    if (IsMeshDragging() && (m_meshDrag->name != name || m_meshDrag->keyform != keyformIndex)) {
        QString error;
        if (!CancelMeshDrag(&error)) { emit MeshEditRejected(error); return; }
    }
    m_meshEditName = name;
    m_activeKeyformIndex = keyformIndex;
}



void OpengGlForm::do_ShowWarp(const QString &artName)
{
    LAppView* view = LAppDelegate::GetInstance()->GetView();
    if(!view) return;
    view->ShowWarp(artName);
}



void OpengGlForm::do_setPoint(float x, float y)
{
    LAppLive2DManager* manager = LAppLive2DManager::GetInstance();
    if(!manager || manager->GetModelNum() <= 0 || !manager->GetModel(0)) return;
    CubismModelMatrix* matrix = manager->GetModel(0)->GetModelMatrix();
    if(!matrix) return;
    matrix->SetCenterPosition(x,y);
}

void OpengGlForm::do_change_para_value(const QString &name, float value,bool IsInit)
{
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
    LAppLive2DManager* mgr = LAppLive2DManager::GetInstance();
    if(mgr && mgr->GetModelNum() > 0 && mgr->GetModel(0)){
        if (mgr->GetModel(0)->HasEditorPreview()) return;
        mgr->GetModel(0)->SetDeformerDragParams(paramIds, minVals, maxVals, defaultVals);
    }
}

void OpengGlForm::do_ResetView()
{
    QString error;
    if (!CancelMeshDrag(&error)) { emit MeshEditRejected(error); return; }
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
    if (!std::isfinite(scale) || scale <= 0.0f || !std::isfinite(position.x()) || !std::isfinite(position.y())) return;
    QString error;
    if (!CancelMeshDrag(&error)) { emit MeshEditRejected(error); return; }
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
    auto* delegate = LAppDelegate::GetInstance();
    if (!delegate) return;
    delegate->m_hiddenDeformerDrawables = names;
}

void OpengGlForm::ClearEditorOverrides()
{
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
}

void OpengGlForm::PublishRenderOrder(const QStringList& names)
{
    if (names == m_lastRenderOrder) return;
    m_lastRenderOrder = names;
    emit RenderOrderChanged(names);
}

void OpengGlForm::SetDrawOrderGroups(
    const QHash<QString, int>& groupIndices,
    const QVector<live2dstruct::CDrawableOrderGroup>& groups)
{
    auto* delegate = LAppDelegate::GetInstance();
    delegate->m_drawableOrderGroupIndices = groupIndices;
    delegate->m_drawOrderGroups = groups;
}

void OpengGlForm::do_SetDrawableOpacity(const QString& name, float opacity)
{
    if (name.isEmpty() || !std::isfinite(opacity)) return;
    LAppDelegate::GetInstance()->m_drawableOpacityOverrides.insert(name, qBound(0.0f, opacity, 1.0f));
}

void OpengGlForm::do_SetDrawableDrawOrder(const QString& name, int drawOrder)
{
    if (name.isEmpty()) return;
    LAppDelegate::GetInstance()->m_drawableDrawOrderOverrides.insert(name, qBound(0, drawOrder, 1000));
}

void OpengGlForm::SetMeshEditHandlers(MeshActivate activate, MeshBegin begin, MeshMove move, MeshFinish finish)
{
    QString error;
    if (!CancelMeshDrag(&error)) { emit MeshEditRejected(error); return; }
    m_meshActivate = std::move(activate);
    m_meshBegin = std::move(begin);
    m_meshMove = std::move(move);
    m_meshFinish = std::move(finish);
}

bool OpengGlForm::ApplyMocPreview(const QByteArray& bytes, const QByteArray& sourceDigest,
                                 const QMap<QString, float>* pose, QString* error)
{
    if (error) error->clear();
    if (IsMeshDragging() && !m_meshEditCallbackActive)
        return MeshError(error, QStringLiteral("请先结束当前顶点拖拽"));
    const RestoreContext restore;
    if (!IsModelReady() || !MakeRuntimeCurrent())
        return MeshError(error, QStringLiteral("模型预览的 OpenGL 上下文尚未就绪"));
    auto* model = CurrentModel();
    if (!model) return MeshError(error, QStringLiteral("当前没有模型"));
    try {
        auto* view = LAppDelegate::GetInstance()->GetView();
        if (!view) return MeshError(error, QStringLiteral("变形器预览视图尚未就绪"));
        CTreeManage::Snapshot tree;
        LAppView::PreparedDeformerOverlays overlays;
        const auto values = pose ? *pose : model->GetParameterValues();
        if (!CTreeManage::PrepareSnapshot(bytes, values, tree, error) ||
            !view->PrepareDeformerOverlays(tree, overlays, error)) return false;
        if (!(pose ? model->ApplyEditorPreview(bytes, sourceDigest, *pose, error)
                   : model->ApplyPreviewMoc(bytes, sourceDigest, error))) return false;
        CTreeManage::SwapSnapshot(tree);
        view->SwapDeformerOverlays(overlays);
        update();
        return true;
    } catch (const std::exception&) {
        return MeshError(error, QStringLiteral("模型预览替换失败"));
    }
}

bool OpengGlForm::EndEditorPreview(QString* error)
{
    if (error) error->clear();
    if (IsMeshDragging()) return MeshError(error, QStringLiteral("请先结束当前顶点拖拽"));
    auto* model = CurrentModel();
    if (!model || !model->HasEditorPreview()) return true;
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
        return true;
    } catch (const std::exception&) {
        return MeshError(error, QStringLiteral("无法准备编辑前的变形器预览"));
    }
}

bool OpengGlForm::FinishMeshDrag(bool cancel, QString* error)
{
    if (error) error->clear();
    if (!IsMeshDragging()) return true;
    if (m_meshEditCallbackActive) return MeshError(error, QStringLiteral("正在提交顶点拖拽"));
    if (!m_meshFinish) return MeshError(error, QStringLiteral("尚未连接顶点拖拽提交器"));
    const QScopedValueRollback<bool> submitting(m_meshEditCallbackActive, true);
    try {
        if (!m_meshFinish(cancel, error)) {
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

bool OpengGlForm::BeginMeshDrag(const QString& name, const QPointF& pixel, QString* error)
{
    if (error) error->clear();
    if (!m_meshActivate || !m_meshBegin || !m_meshMove || !m_meshFinish)
        return MeshError(error, QStringLiteral("尚未连接关键形编辑控制器"));
    try {
        if (!m_meshActivate(name, m_activeKeyformIndex, error)) {
            if (error && error->isEmpty()) *error = QStringLiteral("关键形编辑姿态被拒绝");
            return false;
        }
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
        if (!m_meshBegin(drag.selected, vertices, error)) {
            if (error && error->isEmpty()) *error = QStringLiteral("无法开始顶点拖拽");
            return false;
        }
        m_meshDrag = std::move(drag);
        if (!MeshDragFrameMatches()) {
            CancelMeshDrag(error);
            return MeshError(error, QStringLiteral("开始拖拽时画布变换已改变"));
        }
        appModel = CurrentModel();
        map = appModel && appModel->GetModel() ? appModel->GetModel()->GetMeshMap() : nullptr;
        mesh = map ? map->GetDrawMesh(name) : nullptr;
        if (!mesh || !mesh->GetMesh() || mesh->GetMesh()->GetCpoint().size() != static_cast<size_t>(vertices.size() / 2)) {
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

void OpengGlForm::do_SetDrawableIndices(const QString& name, const QVector<quint16>& indices)
{
    if (name.isEmpty() || !live2dstruct::CMeshTopology::IsValid(indices, 32768)) return;
    auto* delegate = LAppDelegate::GetInstance();
    LAppLive2DManager* manager = LAppLive2DManager::GetInstance();
    if (manager && manager->GetModelNum() > 0 && manager->GetModel(0) && manager->GetModel(0)->GetModel()) {
        auto* model = manager->GetModel(0)->GetModel();
        const auto id = CubismFramework::GetIdManager()->GetId(name.toUtf8().constData());
        if (!model->SetDrawableIndicesOverride(model->GetDrawableIndex(id), indices)) return;
    }
    delegate->m_meshIndexOverrides.insert(name, indices);
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
    if (!MakeRuntimeCurrent()) return false;
    const auto releaseContext = qScopeGuard([this] { doneCurrent(); });
    const bool loaded = LAppDelegate::GetInstance()->ChangeScene(fileName);
    if (!loaded) return false;
    m_ceditstate.AddState(CEditState::ModelOpen);
    m_currentModelJson = fileName;
    return true;
}

bool OpengGlForm::BeginTemporaryModel(const QString& fileName)
{
    if (m_suspendedModel.has_value() || !MakeRuntimeCurrent()) return false;
    const auto releaseContext = qScopeGuard([this] { doneCurrent(); });
    const bool loaded = LAppDelegate::GetInstance()->BeginTemporaryScene(fileName);
    if (!loaded) return false;
    m_suspendedModel = SuspendedModel{m_currentModelJson, IsModelReady()};
    m_currentModelJson = fileName;
    m_ceditstate.AddState(CEditState::ModelOpen);
    return true;
}

bool OpengGlForm::EndTemporaryModel()
{
    if (!m_suspendedModel.has_value() || !MakeRuntimeCurrent()) return false;
    const auto releaseContext = qScopeGuard([this] { doneCurrent(); });
    const bool restored = LAppDelegate::GetInstance()->EndTemporaryScene();
    if (!restored) return false;
    m_currentModelJson = m_suspendedModel->path;
    if (m_suspendedModel->ready) m_ceditstate.AddState(CEditState::ModelOpen);
    else m_ceditstate.RemoveState(CEditState::ModelOpen);
    m_suspendedModel.reset();
    return true;
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
    if (!renderer || renderer->IsDrawing() || LAppDelegate::GetInstance()->IsOffScreen())
        return MeshError(error, QStringLiteral("请等待当前绘制或导出结束后更改蒙版精度"));
    renderer->UseHighPrecisionMask(enabled);
    update();
    return true;
}

void OpengGlForm::SyncPreviewView()
{
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
    if (IsMeshDragging()) {
        QString error;
        if (!CancelMeshDrag(&error)) emit MeshEditRejected(error);
    }
    if (m_runtimeReady) LAppDelegate::GetInstance()->resize(width,height);
}

void OpengGlForm::paintGL()
{
    if (!m_runtimeReady) return;
    auto* delegate = LAppDelegate::GetInstance();
    delegate->paint();
    auto* view = delegate->GetView();
    const QString error = view ? view->DeformerOverlayError() : QString();
    if (error == m_lastPreviewError) return;
    m_lastPreviewError = error;
    if (!error.isEmpty()) emit PreviewFailed(error);
}


void OpengGlForm::mousePressEvent(QMouseEvent *event)
{
    QString error;
    if (!CancelMeshDrag(&error)) { emit MeshEditRejected(error); return; }
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
    if (event->modifiers() & Qt::ControlModifier) {
        if (auto* model = CurrentModel(); model && model->HasEditorPreview()) {
            m_ceditstate.RemoveState(CEditState::MousePress);
            emit MeshEditRejected(QStringLiteral("请先退出关键形编辑再驱动变形器"));
            return;
        }
        auto* delegate = LAppDelegate::GetInstance();
        if (!delegate->m_dragTargetDrawables.isEmpty()) delegate->StartDeformerDrag();
        return;
    }
    const QString name = SelectCoreMesh(event->position(), false);
    emit SendSelectMesh(name);
    if (name.isEmpty()) return;
    if (m_meshEditEnabled && !BeginMeshDrag(name, event->position(), &error) && !error.isEmpty())
        emit MeshEditRejected(error);
}

void OpengGlForm::mouseReleaseEvent(QMouseEvent *event)
{
    if (IsMeshDragging()) {
        QString error;
        const bool cancel = event->button() != Qt::LeftButton || !MeshDragFrameMatches();
        if (!FinishMeshDrag(cancel, &error)) { emit MeshEditRejected(error); return; }
    }
    m_ceditstate.RemoveState(CEditState::MousePress);
    if (event->button() == Qt::RightButton) m_ceditstate.RemoveState(CEditState::ViewMove);
    if (!IsModelReady()) return;
    auto* delegate = LAppDelegate::GetInstance();
    if (auto* view = delegate->GetView()) view->GetMouseSelectRect()->SetStartAndEndPos({}, {});
    if (delegate->m_isDraggingDeformer) delegate->StopDeformerDrag();
}

void OpengGlForm::mouseMoveEvent(QMouseEvent *event)
{
    if (IsMeshDragging()) {
        QString error;
        if (!(event->buttons() & Qt::LeftButton) || !m_meshEditEnabled || !MeshDragFrameMatches()) {
            if (!CancelMeshDrag(&error)) emit MeshEditRejected(error);
            return;
        }
        QPointF current;
        if (!CCanvasCoordinates::ScreenToModel(m_meshDrag->mvp, m_meshDrag->size, event->position(), current)) {
            emit MeshEditRejected(QStringLiteral("鼠标位置无法转换到模型坐标"));
            return;
        }
        const QPointF delta = current - m_meshDrag->start;
        const QScopedValueRollback<bool> submitting(m_meshEditCallbackActive, true);
        try {
            if (!m_meshMove || !m_meshMove(delta, &error)) {
                emit MeshEditRejected(error.isEmpty() ? QStringLiteral("顶点移动被拒绝，保留最后接受的状态") : error);
                return;
            }
        } catch (const std::exception&) {
            emit MeshEditRejected(QStringLiteral("顶点移动失败，保留最后接受的状态"));
            return;
        }
        update();
        return;
    }
    if (!IsModelReady()) return;
    auto* view = LAppDelegate::GetInstance()->GetView();
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
            auto* model = CurrentModel();
            if (!model || model->HasEditorPreview()) return;
            if (!model->GetDeformerDragParamIds().isEmpty()) model->ApplyDeformerDrag(viewX, viewY);
            else model->SetDragging(viewX, viewY);
        } else {
            view->GetMouseSelectRect()->SetStartAndEndPos(QVector2D(m_pointerAnchor), QVector2D(current));
        }
    } else {
        if (!m_ceditstate.IsContins(CEditState::ViewMove)) SelectCoreMesh(event->position(), true);
        auto* model = CurrentModel();
        if (model && !model->HasEditorPreview()) {
            if (model->GetEnableMouseFollow()) model->SetDragging(viewX, viewY);
            model->ApplyPhysicsDrag(viewX, viewY);
        }
    }
}

void OpengGlForm::wheelEvent(QWheelEvent *event)
{
    QString error;
    if (!CancelMeshDrag(&error)) { emit MeshEditRejected(error); return; }
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
    if (event->key() == Qt::Key_Escape && !IsMeshDragging() && m_runtimeReady) {
        auto* model = CurrentModel();
        if (model && model->HasEditorPreview()) {
            emit KeyformEditingFinishedRequested();
            return;
        }
    }
    if (IsMeshDragging() && (event->key() == Qt::Key_Escape || event->key() == Qt::Key_Control ||
        event->key() == Qt::Key_Space || event->key() == Qt::Key_Up || event->key() == Qt::Key_Down ||
        event->key() == Qt::Key_W || event->key() == Qt::Key_A || event->key() == Qt::Key_S ||
        event->key() == Qt::Key_D || event->key() == Qt::Key_Z)) {
        QString error;
        if (!CancelMeshDrag(&error)) { emit MeshEditRejected(error); return; }
        if (event->key() == Qt::Key_Escape) return;
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
    QString error;
    if (!CancelMeshDrag(&error)) emit MeshEditRejected(error);
    QOpenGLWidget::focusOutEvent(event);
}

void OpengGlForm::hideEvent(QHideEvent* event)
{
    QString error;
    if (!CancelMeshDrag(&error)) emit MeshEditRejected(error);
    QOpenGLWidget::hideEvent(event);
}
