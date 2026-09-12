#include "c2dtree.h"
namespace live2dstruct {

C2DTree::C2DTree(const CSectionTable &sectionTable)
{
    InitDeformRoot(sectionTable);
}

C2DTree::~C2DTree()
{
    DeletPart(partRoot);
}

void C2DTree::DeletPart(QList<C2DTreeNode *> &noded)
{
    for(int  i = 0 ; i < noded.size() ;i++){
        if(dynamic_cast<CPartsNode*>(noded[i])){//判断是群组
            DeletPart(noded[i]->getChilds());
        }
        delete noded[i];
    }
    noded.clear();
}

void C2DTree::Test()
{


}

void C2DTree::OnUpdate()
{
    //qDebug() << "C2DTree::OnUpdate() 1";
    SetCurFrame(getDeformRoot());//变形器位置
    //qDebug() << "C2DTree::OnUpdate() 2";
    SetAllCurPoint(getDeformRoot());
    //qDebug() << "C2DTree::OnUpdate() 3";
}

std::vector<C2DTreeNode*> C2DTree::GetWarpKeys()
{
    OnUpdate();
    std::vector<C2DTreeNode*> vetKeyform;//树结构线性化
    for(int i = 0 ;i <deformRoot.size();i++){
        if(dynamic_cast<CDeformerNode*>(deformRoot[i])){
            vetKeyform.push_back(deformRoot[i]);
        }
        GetWarpKeys(deformRoot[i]->getChilds(),vetKeyform);
    }
    return vetKeyform;
}

void C2DTree::SetParaValue(const QString &name, float value)
{
    int size = parametersTree.size();
    for(int i = 0 ;i < size ;i++){
        CParameterNode* node = (CParameterNode*)parametersTree[i];
        if(node->getName() == name){
            node->SetCurValue(value);
        }
    }


}

void C2DTree::GetWarpKeys(QList<C2DTreeNode *> &list, std::vector<C2DTreeNode*> &data)
{
    for(int i = 0 ;i <list.size();i++){
        if(dynamic_cast<CDeformerNode*>(list[i])){

            data.push_back(list[i]);
        }
        GetWarpKeys(list[i]->getChilds(),data);
    }
}

void C2DTree::AddEdge(C2DTreeNode *father, C2DTreeNode *child)
{
    if (!father || !child || father == child) return;
    father->AddChild(child);
    child->AddFather(father);
}
//现在的目标区分那些参数和那些关键帧进行了绑定
void C2DTree::InitDeformRoot(const CSectionTable &sectionTable){
    //初始化参数
    //QList<C2DTreeNode*> parametersBindTree;
    //一个参数上面有二个关键帧，能分别控制

        //QMap<int,CParameterNode*> mapPara;
        QVector<QVector<CParameterNode*>> mpakeyPara;
        {

            for (int i = 0 ;i < sectionTable.parameters.size();i++){
                CParameters* paraSelect = sectionTable.parameters[i];
                CParameterNode* parameterNode = new CParameterNode(paraSelect->ids);
                parameterNode->InitParameterValue(sectionTable,*(paraSelect),0);
                parametersTree.push_back(parameterNode);
                //参数关键帧数量
                int counts = paraSelect->parameter_binding_sources_counts;
                if (counts < 0 || paraSelect->parameter_binding_sources_begin_indices < 0 ||
                    paraSelect->parameter_binding_sources_begin_indices > sectionTable.parameter_bindings.size() - counts)
                    counts = 0;
                for(int j = 1 ;j < counts;j++){
                    CParameterNode* next = new CParameterNode(paraSelect->ids);
                    next->InitParameterValue(sectionTable,*(paraSelect),j);
                    parametersTree.push_back(next);
                    parameterNode->AddParaNext(next);
                }

            }

            QVector<CParameterNode*> vetSave;
            for (int i = 0 ;i < sectionTable.keyform_bindings.size();i++){
                CKeyform_bindings* keyform_bindings = sectionTable.keyform_bindings[i];
                int offset = keyform_bindings->parameter_binding_index_sources_begin_indices;
                int counts = keyform_bindings->parameter_binding_index_sources_counts;
                vetSave.clear();
                if (offset >= 0 && counts >= 0 && offset <= sectionTable.parameter_binding_indices.size() - counts) {
                    for(int j = 0 ; j < counts ;j++){
                        int sources = sectionTable.parameter_binding_indices[offset+j]->binding_sources_indices;
                        if (sources >= 0 && sources < parametersTree.size()) vetSave.push_back((CParameterNode*)parametersTree[sources]);
                    }
                }
                mpakeyPara.push_back(vetSave);
            }

        }


        //初始化part结点
        QList<C2DTreeNode*> partutilList;
        {
            for (int i = 0 ;i < sectionTable.parts.size() ;i++){
                C2DTreeNode* treeNode = new CPartsNode(sectionTable.parts[i]->ids);
                partutilList.push_back(treeNode);

                int sources = sectionTable.parts[i]->keyform_binding_sources_indices;
                if (sources < 0 || sources >= mpakeyPara.size()) continue;
                for(int j = 0 ;j < mpakeyPara[sources].size();j++){
                    AddEdge(mpakeyPara[sources][j],treeNode);
                }
            }


            for (int i = 0 ;i < sectionTable.parts.size() ;i++){
                CParts* parts = sectionTable.parts[i];
                int parentIndeices = parts->parent_part_indices;
                if(parentIndeices == -1){//是根
                    partRoot.push_back(partutilList[i]);
                }
                else {
                    if (parentIndeices >= 0 && parentIndeices < partutilList.size())
                        AddEdge(partutilList[parentIndeices],partutilList[i]);
                }
            }
        }


        QList<C2DTreeNode*> deformutilList;//保存了所有的变形器
        QVector<CWarpNode*> vetWarp;
        QVector<CRotationNode*> vetRotation;
        {
            C2DTreeNode* treeNode = nullptr;
            for (int i = 0 ;i < sectionTable.deformers.size() ;i++){
                CDeformers* deformers = sectionTable.deformers[i];

                if(deformers->types == 1){
                    treeNode = new CRotationNode(deformers->ids);
                    if (deformers->specific_sources_indices >= 0 && deformers->specific_sources_indices < sectionTable.rotation_deformers.size()) {
                        CRotation_deformers* rdef= sectionTable.rotation_deformers[deformers->specific_sources_indices];
                        CRotationNode* ronode = (CRotationNode*)(treeNode);
                        ronode->InitRotation(*rdef,sectionTable);
                        vetRotation.push_back(ronode);
                    }
                }else{
                    treeNode = new CWarpNode(deformers->ids);
                    if (deformers->specific_sources_indices >= 0 && deformers->specific_sources_indices < sectionTable.warp_deformers.size()) {
                        CWarp_deformers* rdef = sectionTable.warp_deformers[deformers->specific_sources_indices];
                        CWarpNode* warpNode = (CWarpNode*) treeNode;
                        warpNode->InitWarp(*rdef,sectionTable);
                        vetWarp.push_back(warpNode);
                    }
                }
                //这里会被part添加
                if(deformers->parent_part_indices != -1){
                    if (deformers->parent_part_indices >= 0 && deformers->parent_part_indices < partutilList.size())
                        AddEdge(partutilList[deformers->parent_part_indices],treeNode);
                }
                deformutilList.push_back(treeNode);//添加所有的变形器

                int sources =deformers->keyform_binding_sources_indices;
                if (sources < 0 || sources >= mpakeyPara.size()) continue;
                for(int j = 0 ;j < mpakeyPara[sources].size();j++){
                    AddEdge(mpakeyPara[sources][j],treeNode);
                }



            }
            for (int i = 0 ;i < sectionTable.deformers.size();i++){
                CDeformers* deformers = sectionTable.deformers[i];
                int defparent = deformers->parent_deformer_indices;
                if(defparent == -1){
                    deformRoot.push_back(deformutilList[i]);
                }
                else {
                    if (defparent >= 0 && defparent < deformutilList.size())
                        AddEdge(deformutilList[defparent],deformutilList[i]);
                }
            }
        }


        //处理所有的参数
        {
            for (int i = 0 ;i < sectionTable.art_meshes.size() ;i++){
                CArt_meshes* art_meshes = sectionTable.art_meshes[i];
                CArtMeshNode* treeNode = new CArtMeshNode(art_meshes->ids);
                treeNode->InitArtMesh(*art_meshes,sectionTable);
                //初始化每一个art节点

                int artparent=art_meshes->parent_deformer_indices;
                if(artparent != -1){
                    if (artparent >= 0 && artparent < deformutilList.size()) AddEdge(deformutilList[artparent],treeNode);
                }else{
                    deformRoot.push_back(treeNode);//这个结点是变形器
                }
                if(art_meshes->parent_part_indices != -1){
                    if (art_meshes->parent_part_indices >= 0 && art_meshes->parent_part_indices < partutilList.size())
                        AddEdge(partutilList[art_meshes->parent_part_indices],treeNode);
                }
                else{
                    partRoot.push_back(treeNode);
                }

                int sources =art_meshes->keyform_binding_sources_indices;
                if (sources < 0 || sources >= mpakeyPara.size()) { artTree.push_back(treeNode); continue; }
                for(int j = 0 ;j < mpakeyPara[sources].size();j++){
                    AddEdge(mpakeyPara[sources][j],treeNode);
                }

                artTree.push_back(treeNode);
            }
        }



        //遍历扭曲变形


        //在遍历一边参数
        for (int m = 0 ;m < sectionTable.parameters.size();m++){
            CParameters* paraSelect = sectionTable.parameters[m];
            int parameter_types = paraSelect->parameter_types;
            if(parameter_types == 1){
                int begin_indices = paraSelect->blend_shape_parameter_binding_sources_begin_indices;
                int sources_counts = paraSelect->blend_shape_parameter_binding_sources_counts;
                int beginMin = begin_indices;
                int endMax = begin_indices + sources_counts;

                for(int i = 0 ;i < sectionTable.blend_shapes_warp_deformers.size() ;i++){
                    CBlend_shapes_warp_deformers* warpnode = sectionTable.blend_shapes_warp_deformers[i];
                    int indices =warpnode->blend_shape_keyform_binding_sources_begin_indices;
                    int counts = warpnode->blend_shape_keyform_binding_sources_counts;
                    if (indices < 0 || counts < 0 || indices > sectionTable.blend_shape_keyform_bindings.size() - counts ||
                        warpnode->target_indices < 0 || warpnode->target_indices >= vetWarp.size()) continue;
                    for(int j = 0 ;j < counts; j++){
                        CBlend_shape_keyform_bindings* warpkey = sectionTable.blend_shape_keyform_bindings[j+indices];
                        int paraIndex = warpkey->parameter_binding_sources_indices;
                        if(beginMin <= paraIndex && paraIndex < endMax){
                            AddEdge(parametersTree[m],vetWarp[warpnode->target_indices]);
                        }
                    }
                }


                for(int i = 0 ;i < sectionTable.blend_shapes_art_meshes.size() ;i++){
                    CBlend_shapes_art_meshes* artnode = sectionTable.blend_shapes_art_meshes[i];
                    int indices =artnode->blend_shape_keyform_binding_sources_begin_indices;
                    int counts = artnode->blend_shape_keyform_binding_sources_counts;
                    if (indices < 0 || counts < 0 || indices > sectionTable.blend_shape_keyform_bindings.size() - counts ||
                        artnode->target_indices < 0 || artnode->target_indices >= artTree.size()) continue;
                    for(int j = 0 ;j < counts; j++){
                        CBlend_shape_keyform_bindings* warpkey = sectionTable.blend_shape_keyform_bindings[j+indices];
                        int paraIndex = warpkey->parameter_binding_sources_indices;
                        if(beginMin <= paraIndex && paraIndex < endMax){
                            AddEdge(parametersTree[m],artTree[artnode->target_indices]);
                        }
                    }
                }

            }






        }



    }


void C2DTree::SetAllCurPoint(QList<C2DTreeNode *> &list)
{
    //根据参数计算当前帧位置
    for(int i = 0; i< list.size();i++){
        if(dynamic_cast<CPartsNode*>(list[i])){
            //直接跳过
        }
        else if(dynamic_cast<CParameterNode*>(list[i])){
            //直接跳过
        }
        else if(dynamic_cast<CRotationNode*>(list[i])){
            //qDebug() << "C2DTree::SetAllCurPointCRotationNode";
            CRotationNode* roRep = (CRotationNode*) list[i];
            CRotationNodeKeyform& keyForm = roRep->GetCurKeyForm();
            CRotationCoorType rotationCoorType = CRotationCoorType(roRep->GetBaseAngles(),keyForm);
            QList<C2DTreeNode*>& child = roRep->getChilds();
            SetChildPoint(child,&rotationCoorType);//对所有的孩子节点进行旋转变换
            SetAllCurPoint(list[i]->getChilds());//接着执行

        }
        else if(dynamic_cast<CWarpNode*>(list[i])){
            //qDebug() << "C2DTree::SetAllCurPointCWarpNode";
            CWarpNode* warpNode = (CWarpNode*) list[i];
            CWarpNodeKeyform& keyForm = warpNode->GetCurKeyForm();
            CWarpCoorType warpCoorType = CWarpCoorType(warpNode->GetColumns(),warpNode->GetRows(),keyForm.GetPosition(),warpNode->GetIs_quad_source());
            QList<C2DTreeNode*>& child = warpNode->getChilds();
            SetChildPoint(child,&warpCoorType);//会获取一个全局位置
            SetAllCurPoint(list[i]->getChilds());//接着执行
        }
        else if(dynamic_cast<CArtMeshNode*>(list[i])){
            //当前帧不计算

        }

    }



}

//根据参数信息设置当前帧数据
void C2DTree::SetCurFrame(QList<C2DTreeNode*> &list)
{
    //如何根据当前参数求当前帧
    for(int i = 0 ;i < list.size() ;i++){
        if(dynamic_cast<CRotationNode*>(list[i])){
            CRotationNode* roRep = (CRotationNode*) list[i];
            //int index = GetKeyIndex(roRep->GetKeyForms().size());
            //roRep->SetCurKeyForm(roRep->GetKeyForms()[index]);

            CLinearInterpolation::SetCKeyFormObject(roRep);
            //这个求关键帧算法有问题

        }
        else if(dynamic_cast<CWarpNode*>(list[i])){
            CWarpNode* warpNode = (CWarpNode*) list[i];
            //int index = GetKeyIndex(warpNode->GetKeyFormS().size());
            //warpNode->SetCurKeyForm(warpNode->GetKeyFormS()[index]);
            CLinearInterpolation::SetCKeyFormObject(warpNode);

        }
        else if(dynamic_cast<CArtMeshNode*>(list[i])){//当前帧不计算
            CArtMeshNode* artnode = (CArtMeshNode*) list[i];
            //int index = GetKeyIndex(artnode->GetKeyFormS().size());
            //artnode->SetCurMeshKey(artnode->GetKeyFormS()[index]);


            CLinearInterpolation::SetCKeyFormObject(artnode);
        }
        SetCurFrame(list[i]->getChilds());
    }


}

void C2DTree::SetChildPoint(QList<C2DTreeNode *> &list, CCoorTypeObject *type)
{
    for(int i = 0 ;i < list.size() ;i++){
        if(dynamic_cast<CRotationNode*>(list[i])){
            CRotationNode* roRep = (CRotationNode*) list[i];
            CRotationNodeKeyform& keyForm = roRep->GetCurKeyForm();
            std::vector<float>& src = keyForm.GetPosition();
            std::vector<float> vetdes;
            vetdes.resize(src.size(),0.0f);
            type->transformLocalToCanvas(src.data(),vetdes.data(),1,0,2);
            keyForm.SetPosition(vetdes);

        }
        else if(dynamic_cast<CWarpNode*>(list[i])){
            CWarpNode* warpNode = (CWarpNode*) list[i];
            CWarpNodeKeyform& keyForm = warpNode->GetCurKeyForm();
            std::vector<float>& src = keyForm.GetPosition();
            std::vector<float> vetdes;
            vetdes.resize(src.size(),0.0f);
            type->transformLocalToCanvas(src.data(),vetdes.data(),src.size() / 2,0,2);
            keyForm.SetPosition(vetdes);

        }
        else if(dynamic_cast<CArtMeshNode*>(list[i])){
            //当前帧不计算
            CArtMeshNode* artnode = (CArtMeshNode*) list[i];
            CArtMeshKeyForm& keyForm =  artnode->GetCurKeyForm();
            std::vector<float>& src = keyForm.GetPosition();
            std::vector<float> vetdes;
            vetdes.resize(src.size(),0.0f);
            type->transformLocalToCanvas(src.data(),vetdes.data(),src.size() / 2,0,2);
            keyForm.SetPosition(vetdes);
        }
    }

}

// //传入的是变形树结构
// void C2DTree::SetChildPointRop(QList<C2DTreeNode *> &list, CRotationCoorType &type)
// {




// }

// void C2DTree::SetChildWarp(QList<C2DTreeNode *> &list, CWarpCoorType &type)
// {
//     for(int i = 0 ;i < list.size() ;i++){
//         if(dynamic_cast<CRotationNode*>(list[i])){
//             CRotationNode* roRep = (CRotationNode*) list[i];
//             CRotationNodeKeyform& keyForm = roRep->GetCurKeyForm();
//             float src[2];
//             src[0] = keyForm.origin_x;
//             src[1] = keyForm.origin_y;
//             float des[2];
//             type.transformLocalToCanvas(src,des,1,0,2);
//             keyForm.origin_x =des[0];
//             keyForm.origin_y = des[1];

//         }
//         else if(dynamic_cast<CWarpNode*>(list[i])){
//             CWarpNode* warpNode = (CWarpNode*) list[i];
//             CWarpNodeKeyform& keyForm = warpNode->GetCurKeyForm();
//             std::vector<float>& src = keyForm.GetPosition();
//             std::vector<float> des;
//             des.resize(src.size(),0.0f);
//             type.transformLocalToCanvas(src.data(),des.data(),src.size() / 2,0,2);
//             keyForm.SetPosition(des);
//         }
//         else if(dynamic_cast<CArtMeshNode*>(list[i])){
//             //当前帧不计算
//             CArtMeshNode* artnode = (CArtMeshNode*) list[i];
//             CArtMeshKeyForm& keyForm =  artnode->GetCurMeshKey();
//             std::vector<float>& src = keyForm.getPosition();
//             std::vector<float> des;
//             des.resize(src.size(),0.0f);
//             type.transformLocalToCanvas(src.data(),des.data(),src.size() / 2,0,2);
//             keyForm.SetPosition(des);

//         }
//     }

// }

int C2DTree::GetKeyIndex(int size)
{
    int index = 0;
    if(size == 3 ){
        index = 1;
    }
    if(size == 9 ){
        index = 4;

    }
    if(size == 2 ){
        index = 0;

    }
    return index;
}





}
