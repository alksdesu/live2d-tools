#include "opengglform.h"
#include "edit/cmeshmap.h"
#include "framework/Id/CubismIdManager.hpp"
#include <QSurfaceFormat>
#include <QScopeGuard>
#include <cmath>


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
    m_meshEditEnabled = enabled;
    if (!enabled) {
        m_meshPointDragging = false;
        m_meshEditName.clear();
        m_meshEditBefore.clear();
    }
}

void OpengGlForm::SetActiveKeyform(const QString& name, int keyformIndex)
{
    if (name.isEmpty() || keyformIndex < 0) return;
    m_meshEditName = name;
    m_activeKeyformIndex = keyformIndex;
}

void OpengGlForm::AddWrapKey(std::vector<C2DTreeNode*> &node)
{
    m_mapWarpNode = node;
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
    LAppLive2DManager* Live2DManager = LAppLive2DManager::GetInstance();
    if(Live2DManager !=nullptr){
        int modelCount = Live2DManager->GetModelNum();
        if(modelCount > 0){
            LAppModel* model = Live2DManager->GetModel(0);
            if (model) {
                model->UpdateParaValue(name,value,IsInit);
            }
            if (model && IsInit && model->GetModel()) {
                CMeshMap* meshMap = model->GetModel()->GetMeshMap();
                if (meshMap) {
                    const auto* delegate = LAppDelegate::GetInstance();
                    for (auto it = delegate->m_meshOverlayPositions.cbegin();
                         it != delegate->m_meshOverlayPositions.cend(); ++it) {
                        const int keyformIndex = delegate->m_meshOverlayKeyformIndices.value(it.key(), -1);
                        const auto keyform = it.value().constFind(keyformIndex);
                        if (keyform != it.value().cend()) meshMap->SetMeshPositions(it.key(), keyform.value());
                    }
                    for (auto it = LAppDelegate::GetInstance()->m_meshIndexOverrides.cbegin();
                         it != LAppDelegate::GetInstance()->m_meshIndexOverrides.cend(); ++it) {
                        const auto id = CubismFramework::GetIdManager()->GetId(it.key().toUtf8().constData());
                        model->GetModel()->SetDrawableIndicesOverride(model->GetModel()->GetDrawableIndex(id), it.value());
                    }
                }
            }
        }
    }
    //修改当前参数对应的
    C2DTree* tree = CTreeManage::GetInstance();
    if (tree) tree->SetParaValue(name,value);
    LAppView* view= LAppDelegate::GetInstance()->GetView();
    if (view) view->InitWrapAndRotation();
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
        mgr->GetModel(0)->SetDeformerDragParams(paramIds, minVals, maxVals, defaultVals);
    }
}

void OpengGlForm::do_ResetView()
{
    LAppView* view = LAppDelegate::GetInstance()->GetView();
    if(!view) return;
    QMatrix4x4 identity;
    identity.setToIdentity();
    view->SetViewMatrix(identity);
    m_config.SetScalefactor(1.0f);
    m_config.SetLastPos(QVector2D());
}

void OpengGlForm::SetViewState(float scale, const QVector2D& position)
{
    LAppView* view = LAppDelegate::GetInstance()->GetView();
    if (!view) return;
    const float clampedScale = qBound(0.1f, scale, 20.0f);
    QMatrix4x4 matrix;
    matrix.setToIdentity();
    matrix.scale(clampedScale);
    matrix.translate(position.x(), position.y(), 0.0f);
    view->SetViewMatrix(matrix);
    m_config.SetScalefactor(clampedScale);
    m_config.SetLastPos(position);
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

void OpengGlForm::do_SetDeformerDrawableVisibility(const QStringList& artMeshNames, bool visible)
{
    auto* delegate = LAppDelegate::GetInstance();
    if (!delegate) return;
    for (const QString& name : artMeshNames) {
        if (name.isEmpty()) continue;
        if (visible) delegate->m_hiddenDeformerDrawables.remove(name);
        else delegate->m_hiddenDeformerDrawables.insert(name);
    }
}

void OpengGlForm::ClearEditorOverrides()
{
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

void OpengGlForm::do_SetDrawableKeyformPositions(const QString& name, int keyformIndex,
                                                  const QVector<float>& positions, bool activate)
{
    if (name.isEmpty() || keyformIndex < 0 || positions.isEmpty() || positions.size() % 2 != 0) return;
    auto* delegate = LAppDelegate::GetInstance();
    delegate->m_meshOverlayPositions[name].insert(keyformIndex, positions);
    if (!activate && (name != m_meshEditName || keyformIndex != m_activeKeyformIndex)) return;
    if (activate) SetActiveKeyform(name, keyformIndex);
    delegate->m_meshOverlayKeyformIndices.insert(name, keyformIndex);
    LAppLive2DManager* manager = LAppLive2DManager::GetInstance();
    if (!manager || manager->GetModelNum() <= 0 || !manager->GetModel(0) || !manager->GetModel(0)->GetModel()) return;
    CMeshMap* meshMap = manager->GetModel(0)->GetModel()->GetMeshMap();
    if (!meshMap) return;
    meshMap->SetMeshPositions(name, positions);
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
    makeCurrent();
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

void OpengGlForm::SyncPreviewView(const QMap<QString, float>& values)
{
    if (!isValid()) return;
    auto* manager = LAppLive2DManager::GetInstance();
    auto* model = manager && manager->GetModelNum() > 0 ? manager->GetModel(0) : nullptr;
    if (!model || !model->GetModel()) return;
    auto* coreModel = model->GetModel();
    auto* meshMap = coreModel->GetMeshMap();
    auto* delegate = LAppDelegate::GetInstance();
    if (meshMap) for (int index = 0; index < coreModel->GetDrawableCount(); ++index) {
        const QString id = QString::fromUtf8(coreModel->GetDrawableId(index)->GetString().GetRawString());
        QVector<float> positions = delegate->m_meshOverlayPositions.value(id)
            .value(delegate->m_meshOverlayKeyformIndices.value(id, -1));
        if (positions.isEmpty()) {
            const float* vertices = coreModel->GetDrawableVertices(index);
            const int count = coreModel->GetDrawableVertexCount(index);
            if (!vertices || count <= 0 || count > 1000000) continue;
            positions = QVector<float>(vertices, vertices + count * 2);
        }
        meshMap->SetMeshPositions(id, positions);
    }
    C2DTree* tree = CTreeManage::GetInstance();
    if (tree) for (auto it = values.cbegin(); it != values.cend(); ++it) tree->SetParaValue(it.key(), it.value());
    makeCurrent();
    if (delegate->GetView()) delegate->GetView()->InitWrapAndRotation();
    doneCurrent();
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
    if (m_runtimeReady) LAppDelegate::GetInstance()->resize(width,height);
}

void OpengGlForm::paintGL()
{
    if (m_runtimeReady) LAppDelegate::GetInstance()->paint();



}


void OpengGlForm::mousePressEvent(QMouseEvent *event)
{
    if (!IsModelReady()) return;
    LAppView* view = LAppDelegate::GetInstance()->GetView();
    if(!view) return;
    QVector4D pos = view->GetWorldPoint(event->pos().x(),event->pos().y());
    view->GetMouseSelectRect()->SetStartAndEndPos(QVector2D(0,0),QVector2D(0,0));
    m_ceditstate.AddState(CEditState::MousePress);
    m_config.SetLastPos(QVector2D(pos));

    // 右键拖拽视图
    if(event->button() == Qt::RightButton){
        m_ceditstate.AddState(CEditState::ViewMove);
        return;
    }

    if(!m_ceditstate.IsContins(CEditState::ViewMove)){
        bool ctrlHeld = event->modifiers() & Qt::ControlModifier;
        if(ctrlHeld && !LAppDelegate::GetInstance()->m_dragTargetDrawables.isEmpty()){
            // Ctrl+左键按下：冻结非目标 drawable 顶点
            LAppDelegate::GetInstance()->StartDeformerDrag();
        } else if(!ctrlHeld){
            QString str = view->GetSelectMesh(QVector2D(pos),false);
            if(!str.isEmpty()){
                emit SendSelectMesh(str);
                if (m_meshEditEnabled && event->button() == Qt::LeftButton &&
                    view->SelectMeshPoint(str, QVector2D(pos), 0.05f)) {
                    m_meshEditName = str;
                    m_meshEditBefore.clear();
                    if (LAppLive2DManager* manager = LAppLive2DManager::GetInstance()) {
                        if (manager->GetModelNum() > 0 && manager->GetModel(0) && manager->GetModel(0)->GetModel()) {
                            if (CMeshMap* map = manager->GetModel(0)->GetModel()->GetMeshMap())
                                m_meshEditBefore = map->MeshPositions(str);
                        }
                    }
                    if (!m_meshEditBefore.isEmpty()) {
                        m_meshPointDragging = true;
                        return;
                    }
                }
            } else {
                emit SendSelectMesh("");
                view->HideAllWarps();
            }
        }
    }




}

void OpengGlForm::mouseReleaseEvent(QMouseEvent *event)
{
    if (!IsModelReady()) return;
    LAppView* view = LAppDelegate::GetInstance()->GetView();
    if(!view) return;
    if (m_meshPointDragging) {
        if (LAppLive2DManager* manager = LAppLive2DManager::GetInstance()) {
            if (manager->GetModelNum() > 0 && manager->GetModel(0) && manager->GetModel(0)->GetModel()) {
                if (CMeshMap* map = manager->GetModel(0)->GetModel()->GetMeshMap()) {
                    const QVector<float> positions = map->MeshPositions(m_meshEditName);
                    if (!positions.isEmpty() && positions != m_meshEditBefore)
                        emit MeshKeyformEdited(m_meshEditName, m_activeKeyformIndex, positions);
                }
            }
        }
        m_meshPointDragging = false;
        m_meshEditBefore.clear();
    }
    view->GetMouseSelectRect()->SetStartAndEndPos(QVector2D(0,0),QVector2D(0,0));
    m_ceditstate.RemoveState(CEditState::MousePress);

    // 停止变形器拖拽
    if(LAppDelegate::GetInstance()->m_isDraggingDeformer){
        LAppDelegate::GetInstance()->StopDeformerDrag();
    }

    // 右键释放时退出视图移动
    if(event->button() == Qt::RightButton){
        m_ceditstate.RemoveState(CEditState::ViewMove);
    }
}

void OpengGlForm::mouseMoveEvent(QMouseEvent *event)
{
    if (!IsModelReady()) return;

    LAppView* view = LAppDelegate::GetInstance()->GetView();
    if(!view || width() <= 0 || height() <= 0) return;

    if (m_meshPointDragging && (event->buttons() & Qt::LeftButton)) {
        const QVector4D current = view->GetWorldPoint(event->pos().x(), event->pos().y());
        const QVector4D previous = QVector4D(m_config.GetLastPos());
        const QVector2D worldDelta(current.x() - previous.x(), current.y() - previous.y());
        const QVector2D delta = view->ModelDelta(worldDelta);
        if (LAppLive2DManager* manager = LAppLive2DManager::GetInstance()) {
            if (manager->GetModelNum() > 0 && manager->GetModel(0) && manager->GetModel(0)->GetModel()) {
                if (CMeshMap* map = manager->GetModel(0)->GetModel()->GetMeshMap())
                    map->MoveSelected(m_meshEditName, delta);
            }
        }
        m_config.SetLastPos(QVector2D(current));
        update();
        return;
    }

    // 计算归一化鼠标坐标 [-1, 1]
    float viewX = (float)event->pos().x() / (float)width() * 2.0f - 1.0f;
    float viewY = -((float)event->pos().y() / (float)height() * 2.0f - 1.0f);

    if(m_ceditstate.IsContins(CEditState::MousePress)){
        if(m_ceditstate.IsContins(CEditState::ViewMove)){
            QVector4D curPos = view->GetWorldPoint(event->pos().x(),event->pos().y());
            QVector4D last =  QVector4D(m_config.GetLastPos());
            QVector4D delta =  (curPos - last);
            QMatrix4x4 temp = view->GetViewMatrix();
            QMatrix4x4 move;
            move.setToIdentity();
            move.translate(delta.x(), delta.y(), 0);
            temp = temp * move;
            view->SetViewMatrix(temp);
        }else{
            // Ctrl+左键拖拽 → 驱动选中变形器绑定的参数
            bool ctrlHeld = event->modifiers() & Qt::ControlModifier;
            if(ctrlHeld){
                LAppLive2DManager* mgr = LAppLive2DManager::GetInstance();
                if(mgr && mgr->GetModelNum() > 0 && mgr->GetModel(0)){
                    LAppModel* model = mgr->GetModel(0);
                    if(!model->GetDeformerDragParamIds().isEmpty()){
                        model->ApplyDeformerDrag(viewX, viewY);
                    } else {
                        // 无绑定参数时 fallback 到标准面部跟随
                        model->SetDragging(viewX, viewY);
                    }
                }
            } else {
                QVector4D curPos = view->GetWorldPoint(event->pos().x(),event->pos().y());
                view->GetMouseSelectRect()->SetStartAndEndPos(m_config.GetLastPos(),QVector2D(curPos));
            }
        }
    }
    else {
        if(!m_ceditstate.IsContins(CEditState::ViewMove)){
            QVector4D curPos = view->GetWorldPoint(event->pos().x(),event->pos().y());
            view->GetSelectMesh(QVector2D(curPos),true);
        }

        // 鼠标跟随 + 物理拖拽
        LAppLive2DManager* mgr = LAppLive2DManager::GetInstance();
        if(mgr && mgr->GetModelNum() > 0 && mgr->GetModel(0)){
            LAppModel* model = mgr->GetModel(0);
            if(model->GetEnableMouseFollow()){
                model->SetDragging(viewX, viewY);
            }
            model->ApplyPhysicsDrag(viewX, viewY);
        }
    }


}

void OpengGlForm::wheelEvent(QWheelEvent *event)
{
    if (!IsModelReady()) return;
    //为什么状态没有用
    LAppView* view = LAppDelegate::GetInstance()->GetView();
    if(!view || width() <= 0 || height() <= 0) return;
    QVector4D curPos = view->GetWorldPoint(event->position().x(),event->position().y());
    float sor = event->angleDelta().y() ;
    float sflo = m_config.GeteScalefactor();
    if(sor > 0){
        sflo+=0.2f;
    }else{
        sflo-=0.2f;
    }
    if(sflo <= 0){
        sflo = 0.1;
    }
    m_config.SetScalefactor(sflo);
    QMatrix4x4 temp = view->GetViewMatrix();
    QMatrix4x4 matsor;
    temp.setToIdentity();
    temp.scale(sflo);
    temp =    temp * matsor   ;
    view->SetViewMatrix(temp);//设置为新的视图


    //产生一个新的向量
    QVector4D Newworldpoist = view->GetWorldPoint(event->position().x(), event->position().y());
    QVector4D delta = Newworldpoist -  curPos;
    QMatrix4x4 move;
    move.setToIdentity();//偏移矩阵
    move.translate(delta.x(), delta.y(), 0);
    temp = view->GetViewMatrix();//再次获取视图矩阵进行移动
    temp = temp *   move;
    view->SetViewMatrix(temp);




}





void OpengGlForm::keyPressEvent(QKeyEvent *event)
{
    if (!IsModelReady()) return;

    LAppView* view = LAppDelegate::GetInstance()->GetView();
    if(!view) return;
    constexpr float moveStep = 0.1f;
    constexpr float scaleStep = 0.2f;

    switch (event->key()) {
        case Qt::Key_Up: {
            float sflo = m_config.GeteScalefactor() + scaleStep;
            m_config.SetScalefactor(sflo);
            QMatrix4x4 temp;
            temp.setToIdentity();
            temp.scale(sflo);
            view->SetViewMatrix(temp);
            break;
        }
        case Qt::Key_Down: {
            float sflo = m_config.GeteScalefactor() - scaleStep;
            if(sflo <= 0.1f) sflo = 0.1f;
            m_config.SetScalefactor(sflo);
            QMatrix4x4 temp;
            temp.setToIdentity();
            temp.scale(sflo);
            view->SetViewMatrix(temp);
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
