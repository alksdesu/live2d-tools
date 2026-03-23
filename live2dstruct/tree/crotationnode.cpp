#include "crotationnode.h"
namespace live2dstruct {

CRotationNode::CRotationNode(const QString &name) :CDeformerNode(name)
{


}

CRotationNode::~CRotationNode()
{
    //qDebug("CRotationNode::~CRotationNode()");
}

void CRotationNode::InitRotation(const CRotation_deformers &rdef, const CSectionTable &sectionTable)
{
    int begin = rdef.keyform_sources_begin_indices;
    int count = rdef.keyform_sources_counts;
    base_angles = rdef.base_angles;
    list.clear();
    for (int i = 0;i < count;i++){
        CRotation_deformer_keyforms* deformerKeyforms = sectionTable.rotation_deformer_keyforms[i+begin];
        CRotationNodeKeyform record;
        record.Setangles(deformerKeyforms->angles) ;
        record.Setis_reflect_x(deformerKeyforms->is_reflect_x);
        record.Setis_reflect_y(deformerKeyforms->is_reflect_y);
        record.Setopacities(deformerKeyforms->opacities);
        record.Setorigin_x(deformerKeyforms->origin_x);
        record.Setorigin_y(deformerKeyforms->origin_y);
        record.Setscales(deformerKeyforms->scales);
        list.push_back(record);
    }
}

}
