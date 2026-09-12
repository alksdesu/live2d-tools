/**
 * Copyright(c) Live2D Inc. All rights reserved.
 *
 * Use of this source code is governed by the Live2D Open Software license
 * that can be found at https://www.live2d.com/eula/live2d-open-software-license-agreement_en.html.
 */

#include "LAppView.hpp"
#include <math.h>
#include <string>
#include "LAppPal.hpp"
#include "LAppDelegate.hpp"
#include "LAppLive2DManager.hpp"
#include "LAppTextureManager.hpp"
#include "LAppDefine.hpp"
#include "TouchManager.hpp"
#include "LAppSprite.hpp"
#include "LAppModel.hpp"
#include "QCoreApplication"

using namespace std;
using namespace LAppDefine;

LAppView::LAppView():
    _renderTarget(SelectTarget_None)
{

    _clearColor[0] = 1.0f;
    _clearColor[1] = 1.0f;
    _clearColor[2] = 1.0f;
    _clearColor[3] = 0.0f;

    _touchManager = new TouchManager();

    _viewMatrix = new CubismViewMatrix();
    m_projection = new CubismMatrix44();
    m_selectrect = new CDrawRect(QVector3D(0,1,0));
    m_mouseselectrect= new CDrawRect(QVector3D(0,0,0xFF));



}

LAppView::~LAppView()
{

    delete _viewMatrix;
    delete _touchManager;
    delete m_projection;
    delete m_selectrect;
    delete m_mouseselectrect;


}

void LAppView::Initialize()
{
    QMap<QString,CDrawObject*>::iterator itor;

    InitWrapAndRotation();

    for (itor = m_drawWarpMap.begin(); itor != m_drawWarpMap.end(); ++itor)
    {
        itor.value()->InitOpenGl();
    }
    m_projection->LoadIdentity();

}

void LAppView::Render()
{
    LAppLive2DManager* Live2DManager = LAppLive2DManager::GetInstance();
    if (!Live2DManager) return;
    Live2DManager->SetViewMatrix(_viewMatrix);//这里是视图模型
    int modelCount = Live2DManager->GetModelNum();
    if(modelCount > 0){
        CubismMatrix44 mvp = *m_projection;
        if (_viewMatrix != NULL)
        {
            mvp.MultiplyByMatrix(_viewMatrix);
        }
        Live2DManager->OnUpdate(mvp);
    }

    if(!LAppDelegate::GetInstance()->IsOffScreen()){
        m_mouseselectrect->Draw(GetProjectionView());
        //在这里把移动绘制处理好,视图，投影，和模型矩阵

        QMap<QString,CDrawObject*>::iterator itor;
        for (itor = m_drawWarpMap.begin(); itor != m_drawWarpMap.end(); ++itor)
        {
            itor.value()->Draw(GetMVP());
        }

    }


}

void LAppView::InitializeSprite()
{
    int width, height;
    width = LAppDelegate::GetInstance()->width();
    height = LAppDelegate::GetInstance()->height();
    if (width <= 0 || height <= 0) return;
    LAppLive2DManager* Live2DManager = LAppLive2DManager::GetInstance();
    if (!Live2DManager) return;
    int modelCount = Live2DManager->GetModelNum();
    if(modelCount > 0){
        LAppModel* model =Live2DManager->GetModel(0);
        if (model->GetModel() == NULL)
        {
            LAppPal::PrintLogLn("Failed to model->GetModel().");
            return ;
        }
        if (model->GetModel()->GetCanvasWidth() > 1.0f && width < height)
        {
            model->GetModelMatrix()->SetWidth(2.0f);
            m_projection->Scale(1.0f, static_cast<float>(width) / static_cast<float>(height));
        }
        else
        {
            m_projection->Scale(static_cast<float>(height) / static_cast<float>(width), 1.0f);
        }
    }


}






void LAppView::PreModelDraw(LAppModel& refModel)
{


}

void LAppView::PostModelDraw(LAppModel& refModel)
{

}

void LAppView::SwitchRenderingTarget(SelectTarget targetType)
{
    _renderTarget = targetType;
}

void LAppView::SetRenderTargetClearColor(float r, float g, float b)
{
    _clearColor[0] = r;
    _clearColor[1] = g;
    _clearColor[2] = b;
}

//将世界坐标系转为模型坐标系
QString LAppView::GetSelectMesh(const QVector2D &pos, bool IsOver)
{
    QString re="";
    LAppLive2DManager* Live2DManager = LAppLive2DManager::GetInstance();
    if (!Live2DManager) return re;
    int modelCount = Live2DManager->GetModelNum();
    if(modelCount > 0){
        LAppModel* modle =Live2DManager->GetModel(0);
        if (!modle || !modle->GetModel() || !modle->GetModelMatrix()) return re;
        CMeshMap* map = modle->GetModel()->GetMeshMap();
        if (!map) return re;
        QMatrix4x4 temp = modle->GetModelMatrix()->GetQMatrix44().inverted();//获取模型矩阵
        QVector4D moldePos = temp * QVector4D(pos,0,1.0f);//已经知道模型位置
        re = map->SetOver(QPointF(moldePos.x(),moldePos.y()),IsOver);
    }

    return re;
}

QVector4D LAppView::GetWorldPoint(int posX, int posY)
{
    float width, height;
    width = LAppDelegate::GetInstance()->width();
    height = LAppDelegate::GetInstance()->height();

    if (width <= 0.0f || height <= 0.0f) return {};
    float x=(2.0f*posX)/width-1.0f;
    float y=1.0f-(2.0f*posY) / height;
    float z=0.0f;
    QVector4D wolrdPostion(x,y,z,1);
    QMatrix4x4 tempPro = m_projection->GetQMatrix44();
    QMatrix4x4 view = _viewMatrix->GetQMatrix44();
    return view.inverted()*tempPro.inverted()*wolrdPostion;
}

QMatrix4x4 LAppView::GetProjectionView()
{
    QMatrix4x4 tempPro = m_projection->GetQMatrix44();
    QMatrix4x4 view = _viewMatrix->GetQMatrix44();
    return tempPro*view;
}

QMatrix4x4 LAppView::GetMVP(){
    QMatrix4x4 tempPro = m_projection->GetQMatrix44();
    QMatrix4x4 view = _viewMatrix->GetQMatrix44();
    LAppLive2DManager* manager = LAppLive2DManager::GetInstance();
    QMatrix4x4 modle;
    if(manager && manager->GetModelNum() > 0){
        LAppModel* model = manager->GetModel(0);
        if (model && model->GetModelMatrix()) modle = model->GetModelMatrix()->GetQMatrix44();
    }

    return tempPro*view * modle;

};

CDrawRect *LAppView::GetSelectrect()
{
    return m_selectrect;
}

CDrawRect *LAppView::GetMouseSelectRect()
{

    return m_mouseselectrect;
};

void LAppView::SetViewMatrix(const QMatrix4x4 &temp)
{

    float tr[16];
    tr[0] = temp(0,0);
    tr[4] = temp(0,1) ;
    tr[8] = temp(0,2) ;
    tr[12] = temp(0,3);

    tr[1] = temp(1,0);
    tr[5] = temp(1,1);
    tr[9] = temp(1,2);
    tr[13] = temp(1,3);

    tr[2] = temp(2,0) ;
    tr[6] = temp(2,1) ;
    tr[10] = temp(2,2);
    tr[14] = temp(2,3);

    tr[3] = temp(3,0);
    tr[7] = temp(3,1);
    tr[11] = temp(3,2);
    tr[15] = temp(3,3);

    _viewMatrix->SetMatrix(tr);

}

QMatrix4x4 LAppView::GetViewMatrix()
{
    return _viewMatrix->GetQMatrix44();
}

void LAppView::AddWrapKey(CWarpNode* node)
{
    if(m_drawWarpMap.contains(node->getName())){
        CDrawWarp* warp = (CDrawWarp*)m_drawWarpMap[node->getName()];
        warp->Init(node);

    }
    else{
        CDrawWarp* warp = new CDrawWarp();
        warp->Init(node);
        m_drawWarpMap.insert(node->getName(),warp);
    }



}
void LAppView::AddWrapKeyTest(CWarpNode* node)
{
    // if(m_drawWarpMap.contains(node->getName())){
    //     CDrawWarp* warp = (CDrawWarp*)m_drawWarpMap[node->getName()];
    //     warp->InitTest(0,node);
    // }
    // else{

    // }

    if(!m_drawWarpMap.contains("0")){
        for(int i = 0 ;i < 6;i++){
            CDrawWarp* warp = new CDrawWarp();
            warp->InitTest(i,node);
            warp->SetDraw(true);
            warp->SetColor(QVector3D(0,i*6,i-1));
            m_drawWarpMap.insert(QString::asprintf("%d",i),warp);
        }

    }


}

void LAppView::AddRotationKey(CRotationNode *node)
{
    if(m_drawWarpMap.contains(node->getName())){

        CDrawRotation* rotaion = (CDrawRotation*)m_drawWarpMap[node->getName()];
        rotaion->Init(node);

    }else{
        CDrawRotation* rotaion = new CDrawRotation();
        rotaion->Init(node);
        m_drawWarpMap.insert(node->getName(),rotaion);

    }

}

void LAppView::InitWrapAndRotation()
{

    C2DTree* tree = CTreeManage::GetInstance();
    if (!tree) return;
    std::vector<C2DTreeNode*> m_mapWarpNode = tree->GetWarpKeys();
    for(int i = 0 ;i < m_mapWarpNode.size() ;i++){
        if(dynamic_cast<CWarpNode*>(m_mapWarpNode[i])){
            CWarpNode* wrap = (CWarpNode*)m_mapWarpNode[i];
            AddWrapKey(wrap);
        }
        else if(dynamic_cast<CRotationNode*>(m_mapWarpNode[i])){
            CRotationNode* rotiation = (CRotationNode*)m_mapWarpNode[i];
            AddRotationKey(rotiation);
        }
    }

}

void LAppView::ShowWarp(const QString &warpName)
{
    QMap<QString,CDrawObject*>::iterator itor;
    for (itor = m_drawWarpMap.begin(); itor != m_drawWarpMap.end(); ++itor)
    {
        itor.value()->SetDraw(false);
    }
    if(m_drawWarpMap.contains(warpName)){
        m_drawWarpMap[warpName]->SetDraw(true);
    }
}

void LAppView::ShowWarps(const QStringList& names)
{
    // 先隐藏全部
    for(auto itor = m_drawWarpMap.begin(); itor != m_drawWarpMap.end(); ++itor){
        itor.value()->SetDraw(false);
    }
    // 显示指定的
    for(const QString& name : names){
        if(m_drawWarpMap.contains(name)){
            m_drawWarpMap[name]->SetDraw(true);
        }
    }
}

void LAppView::HideAllWarps()
{
    for(auto itor = m_drawWarpMap.begin(); itor != m_drawWarpMap.end(); ++itor){
        itor.value()->SetDraw(false);
    }
}

void LAppView::UpdateWarpPositions()
{
    // 重新从 C2DTree 获取最新的变形器位置
    C2DTree* tree = CTreeManage::GetInstance();
    if (!tree) return;
    std::vector<C2DTreeNode*> nodes = tree->GetWarpKeys();
    for(auto* node : nodes){
        if(auto* warp = dynamic_cast<CWarpNode*>(node)){
            if(m_drawWarpMap.contains(warp->getName())){
                CDrawWarp* dw = (CDrawWarp*)m_drawWarpMap[warp->getName()];
                dw->UpdatePositions(warp);
            }
        }
    }
}


float LAppView::GetSpriteAlpha(int assign) const
{

    float alpha = 0.25f + static_cast<float>(assign) * 0.5f; // サンプルとしてαに適当な差をつける
    if (alpha > 1.0f)
    {
        alpha = 1.0f;
    }
    if (alpha < 0.1f)
    {
        alpha = 0.1f;
    }

    return alpha;
}

void LAppView::ResizeSprite()
{
    InitializeSprite();
}
