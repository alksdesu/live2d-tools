#include "opengglform.h"
#include "edit/cmeshmap.h"
#include <QSurfaceFormat>
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
    LAppDelegate::GetInstance()->Release();
    LAppDelegate::ReleaseInstance();
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

    bool ok = false;
    if (!fileName.isEmpty()) {
        makeCurrent();
        ok = LAppDelegate::GetInstance()->StartOffset_FunpsdList(fileName,IsALl,IsFenzu,cubismCompat);
        doneCurrent();
        resizeGL(this->width(),this->height());
    }

    m_ceditstate.RemoveState(CEditState::PrintAllPsd);
    m_ceditstate.RemoveState(CEditState::PrintFenzuPsd);
    return ok;
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
    if (!fileName.isEmpty()) {
        return LAppDelegate::GetInstance()->StartTextureExtraction(fileName, isAll, isFenzu, cubismCompat);
    }
    return false;
}

void OpengGlForm::do_ShowMesh(const QString &artName)
{
    LAppLive2DManager* Live2DManager = LAppLive2DManager::GetInstance();
    int size = Live2DManager->GetModelNum();
    if(size > 0){
        LAppModel* model = Live2DManager->GetModel(0);
        CMeshMap* map = model->GetModel()->GetMeshMap();
        map->SetShow(artName);

    }


}

void OpengGlForm::AddWrapKey(std::vector<C2DTreeNode*> &node)
{
    m_mapWarpNode = node;
}

void OpengGlForm::do_ShowWarp(const QString &artName)
{
    LAppView* view = LAppDelegate::GetInstance()->GetView();
    view->ShowWarp(artName);
}



void OpengGlForm::do_setPoint(float x, float y)
{
    CubismModelMatrix* matrix = LAppLive2DManager::GetInstance()->GetModel(0)->GetModelMatrix();
    matrix->SetCenterPosition(x,y);
}

void OpengGlForm::do_change_para_value(const QString &name, float value,bool IsInit)
{
    LAppLive2DManager* Live2DManager = LAppLive2DManager::GetInstance();
    if(Live2DManager !=nullptr){
        int modelCount = Live2DManager->GetModelNum();
        if(modelCount > 0){
            Live2DManager->GetModel(0)->UpdateParaValue(name,value,IsInit);
        }
    }
    //修改当前参数对应的
    C2DTree* tree = CTreeManage::GetInstance();
    if (tree) tree->SetParaValue(name,value);
    LAppView* view= LAppDelegate::GetInstance()->GetView();
    view->InitWrapAndRotation();
}

void OpengGlForm::do_ShowDeformers(const QStringList& names)
{
    LAppView* view = LAppDelegate::GetInstance()->GetView();
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
    if(mgr && mgr->GetModelNum() > 0){
        mgr->GetModel(0)->SetDeformerDragParams(paramIds, minVals, maxVals, defaultVals);
    }
}

void OpengGlForm::do_ResetView()
{
    LAppView* view = LAppDelegate::GetInstance()->GetView();
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
        if(mgr && mgr->GetModelNum() > 0){
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
                                                  const QVector<float>& positions)
{
    Q_UNUSED(keyformIndex);
    if (name.isEmpty()) return;
    LAppLive2DManager* manager = LAppLive2DManager::GetInstance();
    if (!manager || manager->GetModelNum() <= 0 || !manager->GetModel(0) || !manager->GetModel(0)->GetModel()) return;
    manager->GetModel(0)->GetModel()->GetMeshMap()->SetMeshPositions(name, positions);
}

void OpengGlForm::do_ModelChange(const QString& fileName)
{
    if(m_ceditstate.IsContins(CEditState::ModelOpen)){
        makeCurrent();
        LAppDelegate::GetInstance()->ChangeScene(fileName);
        doneCurrent();
    }
    m_currentModelJson = fileName;
}



void OpengGlForm::initializeGL()
{

    LAppDelegate::GetInstance()->Initialize(this,m_config.GetInitFileName());//初始化完成后



    m_ceditstate.AddState(CEditState::ModelOpen);

}

void OpengGlForm::resizeGL(int width, int height)
{
    LAppDelegate::GetInstance()->resize(width,height);
}

void OpengGlForm::paintGL()
{
    LAppDelegate::GetInstance()->paint();



}


void OpengGlForm::mousePressEvent(QMouseEvent *event)
{
    LAppView* view = LAppDelegate::GetInstance()->GetView();
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
            } else {
                emit SendSelectMesh("");
                view->HideAllWarps();
            }
        }
    }




}

void OpengGlForm::mouseReleaseEvent(QMouseEvent *event)
{
    LAppView* view = LAppDelegate::GetInstance()->GetView();
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

    // 计算归一化鼠标坐标 [-1, 1]
    float viewX = (float)event->pos().x() / (float)width() * 2.0f - 1.0f;
    float viewY = -((float)event->pos().y() / (float)height() * 2.0f - 1.0f);

    if(m_ceditstate.IsContins(CEditState::MousePress)){
        if(m_ceditstate.IsContins(CEditState::ViewMove)){
            LAppView* view = LAppDelegate::GetInstance()->GetView();
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
                if(mgr && mgr->GetModelNum() > 0){
                    LAppModel* model = mgr->GetModel(0);
                    if(!model->GetDeformerDragParamIds().isEmpty()){
                        model->ApplyDeformerDrag(viewX, viewY);
                    } else {
                        // 无绑定参数时 fallback 到标准面部跟随
                        model->SetDragging(viewX, viewY);
                    }
                }
            } else {
                LAppView* view = LAppDelegate::GetInstance()->GetView();
                QVector4D curPos = view->GetWorldPoint(event->pos().x(),event->pos().y());
                view->GetMouseSelectRect()->SetStartAndEndPos(m_config.GetLastPos(),QVector2D(curPos));
            }
        }
    }
    else {
        if(!m_ceditstate.IsContins(CEditState::ViewMove)){
            LAppView* view = LAppDelegate::GetInstance()->GetView();
            QVector4D curPos = view->GetWorldPoint(event->pos().x(),event->pos().y());
            view->GetSelectMesh(QVector2D(curPos),true);
        }

        // 鼠标跟随 + 物理拖拽
        LAppLive2DManager* mgr = LAppLive2DManager::GetInstance();
        if(mgr && mgr->GetModelNum() > 0){
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
    //为什么状态没有用
    LAppView* view = LAppDelegate::GetInstance()->GetView();
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

    LAppView* view = LAppDelegate::GetInstance()->GetView();
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
            if(mgr && mgr->GetModelNum() > 0){
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
