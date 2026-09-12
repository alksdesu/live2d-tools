#include "cwarpnode.h"
namespace live2dstruct {

CWarpNode::CWarpNode(const QString &name) : CDeformerNode(name)
{


}

CWarpNode::~CWarpNode()
{
    //qDebug("CWarpNode::~CWarpNode()");
}

void CWarpNode::InitWarp(const CWarp_deformers &warpDeformers, const CSectionTable &sectionTable)
{
    is_quad_source = warpDeformers.is_quad_source;
    columns = warpDeformers.columns;
    rows =warpDeformers.rows;
    vertex_counts = warpDeformers.vertex_counts;
    if (vertex_counts <= 0 || vertex_counts > 1000000 || !sectionTable.keyform_positions ||
        warpDeformers.keyform_sources_begin_indices < 0 || warpDeformers.keyform_sources_counts <= 0 ||
        warpDeformers.keyform_sources_begin_indices > sectionTable.warp_deformer_keyforms.size() - warpDeformers.keyform_sources_counts) return;
    int count = warpDeformers.keyform_sources_counts;
    int length = vertex_counts * 2;
    vetWarp.clear();
    for (int j = 0 ;j < count;j++){
        //有偏移位置
        CWarp_deformer_keyforms* warpDeformerKeyforms = sectionTable.warp_deformer_keyforms[warpDeformers.keyform_sources_begin_indices+j];
        int offset = warpDeformerKeyforms->keyform_position_sources_begin_indices ;

        float opacities = warpDeformerKeyforms->opacities;
        std::vector<float> floats;
        floats.resize(length,0.0f);

        for (int i = 0 ;i < length;i++){
            floats[i] = sectionTable.keyform_positions->GetData(i + offset);
        }
        vetWarp.push_back(CWarpNodeKeyform(opacities,floats));

    }

}

}
