#include "opengglform.h"
#include "edit/cmeshmap.h"


OpengGlForm::OpengGlForm(QWidget *par):QOpenGLWidget(par)
{
    constexpr int frame = 60;
    constexpr int fps = 1000 / frame;
    m_timer.start(fps);//
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

void OpengGlForm::do_OffSeceen(const QString& fileName,bool IsALl,bool IsFenzu)
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


    if (!fileName.isEmpty()) {
        makeCurrent();
        LAppDelegate::GetInstance()->StartOffset_FunpsdList(fileName,IsALl,IsFenzu);
        doneCurrent();
        resizeGL(this->width(),this->height());

// #ifdef QT_NO_DEBUG
//          qDebug() << "Release mode";
//            load.Run(curOpenModelName);

// #else
//        qDebug() << "Debug mode";
// #endif
        QMessageBox::about(this,"psd","保存成功");
        QFileInfo fileInfo(fileName);
        QString directoryPath = fileInfo.absoluteDir().path();
        QDesktopServices::openUrl(QUrl::fromLocalFile(directoryPath));
    }

    m_ceditstate.RemoveState(CEditState::PrintAllPsd);
    m_ceditstate.RemoveState(CEditState::PrintFenzuPsd);

}

void OpengGlForm::do_OffSeceenSilent(const QString &fileName, bool IsALl, bool IsFenzu)
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

    if (!fileName.isEmpty()) {
        makeCurrent();
        LAppDelegate::GetInstance()->StartOffset_FunpsdList(fileName,IsALl,IsFenzu);
        doneCurrent();
        resizeGL(this->width(),this->height());
    }

    m_ceditstate.RemoveState(CEditState::PrintAllPsd);
    m_ceditstate.RemoveState(CEditState::PrintFenzuPsd);
}

void OpengGlForm::do_TextureExtract(const QString& fileName, bool isAll, bool isFenzu)
{
    if (!fileName.isEmpty()) {
        LAppDelegate::GetInstance()->StartTextureExtraction(fileName, isAll, isFenzu);
        QMessageBox::about(this, "psd", "保存成功（原始纹理提取）");
        QFileInfo fileInfo(fileName);
        QString directoryPath = fileInfo.absoluteDir().path();
        QDesktopServices::openUrl(QUrl::fromLocalFile(directoryPath));
    }
}

void OpengGlForm::do_TextureExtractSilent(const QString& fileName, bool isAll, bool isFenzu)
{
    if (!fileName.isEmpty()) {
        LAppDelegate::GetInstance()->StartTextureExtraction(fileName, isAll, isFenzu);
    }
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
    tree->SetParaValue(name,value);
    LAppView* view= LAppDelegate::GetInstance()->GetView();
    view->InitWrapAndRotation();
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

    if(!m_ceditstate.IsContins(CEditState::ViewMove)){

        QString str = view->GetSelectMesh(QVector2D(pos),false);
        if(str!=""){
            //qDebug() << str;
            emit SendSelectMesh(str);//发送槽
        }
    }




}

void OpengGlForm::mouseReleaseEvent(QMouseEvent *event)
{

    LAppView* view = LAppDelegate::GetInstance()->GetView();
    view->GetMouseSelectRect()->SetStartAndEndPos(QVector2D(0,0),QVector2D(0,0));
    m_ceditstate.RemoveState(CEditState::MousePress);
}

void OpengGlForm::mouseMoveEvent(QMouseEvent *event)
{

    if(m_ceditstate.IsContins(CEditState::MousePress)){
        //qDebug() << "鼠标按下";
        if(m_ceditstate.IsContins(CEditState::ViewMove)){

            LAppView* view = LAppDelegate::GetInstance()->GetView();
            QVector4D curPos = view->GetWorldPoint(event->pos().x(),event->pos().y());
            QVector4D last =  QVector4D(m_config.GetLastPos()); ;
            QVector4D delta =  (curPos - last); // 计算鼠标移动的距离
            QMatrix4x4 temp = view->GetViewMatrix();
            QMatrix4x4 move;
            move.setToIdentity();//偏移矩阵
            move.translate(delta.x(), delta.y(), 0);
            temp = temp *   move;
            view->SetViewMatrix(temp);

        }else{
            LAppView* view = LAppDelegate::GetInstance()->GetView();
            QVector4D curPos = view->GetWorldPoint(event->pos().x(),event->pos().y());
            view->GetMouseSelectRect()->SetStartAndEndPos(m_config.GetLastPos(),QVector2D(curPos));

        }
    }
    else {

        if(!m_ceditstate.IsContins(CEditState::ViewMove)){
            LAppView* view = LAppDelegate::GetInstance()->GetView();
            QVector4D curPos = view->GetWorldPoint(event->pos().x(),event->pos().y());
            view->GetSelectMesh(QVector2D(curPos),true);
        }

        // 鼠标跟随：将坐标传给模型的 drag manager
        LAppLive2DManager* mgr = LAppLive2DManager::GetInstance();
        if(mgr && mgr->GetModelNum() > 0){
            LAppModel* model = mgr->GetModel(0);
            if(model->GetEnableMouseFollow()){
                float viewX = (float)event->pos().x() / (float)width() * 2.0f - 1.0f;
                float viewY = -((float)event->pos().y() / (float)height() * 2.0f - 1.0f);
                model->SetDragging(viewX, viewY);
            }
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


