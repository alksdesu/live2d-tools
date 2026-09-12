#include "cartmeshnode.h"
namespace live2dstruct {

CArtMeshNode::CArtMeshNode(const QString &name):C2DTreeNode(name)
{

}

CArtMeshNode::~CArtMeshNode()
{
    //qDebug("CArtMeshNode::~CArtMeshNode()");
}

void CArtMeshNode::InitArtMesh(CArt_meshes &art_meshes,const CSectionTable &sectionTable)
{
    meshKeys.clear();
    is_visible = art_meshes.is_visible;
    is_enabled = art_meshes.is_enabled;
    texture_nos = art_meshes.texture_nos;
    drawable_flags =art_meshes.drawable_flags;
    vertex_counts =art_meshes.vertex_counts;
    if (vertex_counts <= 0 || vertex_counts > 1000000 || !sectionTable.uvs || !sectionTable.position_indices ||
        !sectionTable.keyform_positions ||
        art_meshes.uv_sources_begin_indices < 0 ||
        art_meshes.uv_sources_begin_indices > static_cast<int>(sectionTable.uvs->uvs.size()) - vertex_counts * 2 ||
        art_meshes.position_index_sources_begin_indices < 0 || art_meshes.position_index_sources_counts < 3 ||
        art_meshes.position_index_sources_counts % 3 != 0 ||
        art_meshes.position_index_sources_begin_indices > static_cast<int>(sectionTable.position_indices->indices.size()) - art_meshes.position_index_sources_counts ||
        art_meshes.keyform_sources_begin_indices < 0 || art_meshes.keyform_sources_counts <= 0 ||
        art_meshes.keyform_sources_begin_indices > sectionTable.art_mesh_keyforms.size() - art_meshes.keyform_sources_counts) return;
    int vecoun = vertex_counts * 2;
    int count = vertex_counts * 2;
    uv.resize(count,0);
    int begin = art_meshes.uv_sources_begin_indices;
    for (int i = 0 ;i < count;i++){
        if(i%2==1){
            uv[i] = 1-sectionTable.uvs->GetData(begin + i);
        }else{
            uv[i] = sectionTable.uvs->GetData(begin + i);
        }

    }
    count = art_meshes.position_index_sources_counts ;
    begin = art_meshes.position_index_sources_begin_indices;
    position.resize(count,0);
    for (int i = 0 ; i < count;i+=3){

        position[i] = sectionTable.position_indices->GetData(begin + i+2);
        position[i+1] = sectionTable.position_indices->GetData(begin + i+1);
        position[i+2] = sectionTable.position_indices->GetData(begin + i);
    }
    //只需要最后输出的结果纹理坐标反向就好
    count = art_meshes.keyform_sources_counts ;
    begin = art_meshes.keyform_sources_begin_indices;
    std::vector<float> floats;

    //优化了加载速度的问题
    for (int i = 0 ;i < count ;i++){
        CArt_mesh_keyforms* artMeshKeyforms = sectionTable.art_mesh_keyforms[begin+i];
        floats.resize(vecoun,0.0f);
        float opacities = artMeshKeyforms->opacities;
        float drawOrder = artMeshKeyforms->draw_orders;
        int start = artMeshKeyforms->keyform_position_sources_begin_indices;
        for (int j = 0 ; j < vecoun;j++){
            floats[j] = sectionTable.keyform_positions->GetData(start+j);
        }
        meshKeys.push_back(CArtMeshKeyForm(opacities,drawOrder,floats));
    }


}


}
