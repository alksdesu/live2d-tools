#include "c2dtree.h"
#include "live2dstruct/keyform/clinearinterpolation.h"
#include <QSet>
namespace live2dstruct {

C2DTree::C2DTree(const CSectionTable &sectionTable)
{
    InitDeformRoot(sectionTable);
}

C2DTree::~C2DTree() = default;

void C2DTree::Test() {}

void C2DTree::OnUpdate()
{
    struct Entry {
        C2DTreeNode* node;
        std::shared_ptr<const CCoordinateFrame> parent;
    };
    std::vector<Entry> pending;
    for (auto it = deformRoot.crbegin(); it != deformRoot.crend(); ++it) pending.push_back({*it, {}});
    QSet<C2DTreeNode*> visited;
    m_displayDeformers.clear();
    while (!pending.empty()) {
        Entry entry = std::move(pending.back());
        pending.pop_back();
        C2DTreeNode* node = entry.node;
        if (!node || visited.contains(node)) continue;
        visited.insert(node);
        CLinearInterpolation::SetCKeyFormObject(node);
        node->SetParentFrame(entry.parent);
        std::shared_ptr<const CCoordinateFrame> childFrame = entry.parent;
        if (auto* rotation = dynamic_cast<CRotationNode*>(node)) {
            auto& key = rotation->GetCurKeyForm();
            node->UpdateCanvasPositions(key.GetPosition());
            childFrame = std::make_shared<CCoordinateFrame>(
                std::make_unique<CRotationCoorType>(rotation->GetBaseAngles(), key), entry.parent);
            m_displayDeformers.push_back(node);
        } else if (auto* warp = dynamic_cast<CWarpNode*>(node)) {
            auto& key = warp->GetCurKeyForm();
            node->UpdateCanvasPositions(key.GetPosition());
            childFrame = std::make_shared<CCoordinateFrame>(
                std::make_unique<CWarpCoorType>(warp->GetColumns(), warp->GetRows(),
                                              key.GetPosition(), warp->GetIs_quad_source()), entry.parent);
            m_displayDeformers.push_back(node);
        } else if (auto* mesh = dynamic_cast<CArtMeshNode*>(node)) {
            node->UpdateCanvasPositions(mesh->GetCurKeyForm().GetPosition());
        }
        const auto& children = node->getChilds();
        for (auto it = children.crbegin(); it != children.crend(); ++it) pending.push_back({*it, childFrame});
    }
}

std::vector<C2DTreeNode*> C2DTree::GetWarpKeys()
{
    OnUpdate();
    return m_displayDeformers;
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

void C2DTree::AddEdge(C2DTreeNode *father, C2DTreeNode *child)
{
    if (!father || !child || father == child) return;
    father->AddChild(child);
    child->AddFather(father);
}
//现在的目标区分那些参数和那些关键帧进行了绑定
void C2DTree::InitDeformRoot(const CSectionTable &sectionTable){
    const auto parentChainValid = [](int index, const QVector<int>& parents) {
        if (index < 0 || index >= parents.size()) return false;
        QVector<bool> visited(parents.size(), false);
        int current = index;
        while (current >= 0) {
            if (current >= parents.size() || visited[current]) return false;
            visited[current] = true;
            current = parents[current];
        }
        return true;
    };
    QVector<int> partParents;
    partParents.reserve(sectionTable.parts.size());
    for (const CParts* part : sectionTable.parts) partParents.append(part->parent_part_indices);
    QVector<int> deformerParents;
    deformerParents.reserve(sectionTable.deformers.size());
    for (const CDeformers* deformer : sectionTable.deformers) deformerParents.append(deformer->parent_deformer_indices);
    //初始化参数
    //QList<C2DTreeNode*> parametersBindTree;
    //一个参数上面有二个关键帧，能分别控制

        //QMap<int,CParameterNode*> mapPara;
        QVector<QVector<CParameterNode*>> mpakeyPara;
        {

            for (int i = 0 ;i < sectionTable.parameters.size();i++){
                CParameters* paraSelect = sectionTable.parameters[i];
                CParameterNode* parameterNode = Own(new CParameterNode(paraSelect->ids));
                parameterNode->InitParameterValue(sectionTable,*(paraSelect),0);
                parametersTree.push_back(parameterNode);
                //参数关键帧数量
                int counts = paraSelect->parameter_binding_sources_counts;
                if (counts < 0 || paraSelect->parameter_binding_sources_begin_indices < 0 ||
                    paraSelect->parameter_binding_sources_begin_indices > sectionTable.parameter_bindings.size() - counts)
                    counts = 0;
                for(int j = 1 ;j < counts;j++){
                    CParameterNode* next = Own(new CParameterNode(paraSelect->ids));
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
                C2DTreeNode* treeNode = Own(new CPartsNode(sectionTable.parts[i]->ids));
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
                    if (parentIndeices >= 0 && parentIndeices < partutilList.size() && parentChainValid(i, partParents))
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
                    treeNode = Own(new CRotationNode(deformers->ids));
                    if (deformers->specific_sources_indices >= 0 && deformers->specific_sources_indices < sectionTable.rotation_deformers.size()) {
                        CRotation_deformers* rdef= sectionTable.rotation_deformers[deformers->specific_sources_indices];
                        CRotationNode* ronode = (CRotationNode*)(treeNode);
                        ronode->InitRotation(*rdef,sectionTable);
                        vetRotation.push_back(ronode);
                    }
                }else{
                    treeNode = Own(new CWarpNode(deformers->ids));
                    if (deformers->specific_sources_indices >= 0 && deformers->specific_sources_indices < sectionTable.warp_deformers.size()) {
                        CWarp_deformers* rdef = sectionTable.warp_deformers[deformers->specific_sources_indices];
                        CWarpNode* warpNode = (CWarpNode*) treeNode;
                        warpNode->InitWarp(*rdef,sectionTable);
                        vetWarp.push_back(warpNode);
                    }
                }
                //这里会被part添加
                if(deformers->parent_part_indices != -1){
                    if (deformers->parent_part_indices >= 0 && deformers->parent_part_indices < partutilList.size() &&
                        parentChainValid(deformers->parent_part_indices, partParents))
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
                    if (defparent >= 0 && defparent < deformutilList.size() && parentChainValid(i, deformerParents))
                        AddEdge(deformutilList[defparent],deformutilList[i]);
                }
            }
        }


        //处理所有的参数
        {
            for (int i = 0 ;i < sectionTable.art_meshes.size() ;i++){
                CArt_meshes* art_meshes = sectionTable.art_meshes[i];
                CArtMeshNode* treeNode = Own(new CArtMeshNode(art_meshes->ids));
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


}
