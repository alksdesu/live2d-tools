#ifndef CROTATION_DEFORMERS_H
#define CROTATION_DEFORMERS_H
#include "live2dstruct/util/cfilereader.h"
namespace live2dstruct {
class CRotation_deformers
{
public:
    class Offset{
    public:
        int keyform_binding_sources_indices;
        int keyform_sources_begin_indices;
        int keyform_sources_counts;
        int base_angles;

        Offset(CReader& bytes) {
            keyform_binding_sources_indices = bytes.readInt();
            keyform_sources_begin_indices= bytes.readInt();
            keyform_sources_counts= bytes.readInt();
            base_angles= bytes.readInt();
        }
        int keyform_color_sources_begin_indices;
        void Set_keyform_color_sources_begin_indices(CReader& bytes){
            keyform_color_sources_begin_indices = bytes.readInt();
        }
    };
    int keyform_binding_sources_indices;//关键帧数组起始位置
    int keyform_sources_begin_indices;//关键帧开始位置信息
    int keyform_sources_counts;//旋转关键帧数量信息
    float base_angles;//基础旋转角度信息
    CRotation_deformers(CReader& bytes, const Offset& rotationDeformers, int i) {
        bytes.seek(rotationDeformers.keyform_binding_sources_indices + i*4);
        keyform_binding_sources_indices = bytes.readInt();

        bytes.seek(rotationDeformers.keyform_sources_begin_indices + i*4);
        keyform_sources_begin_indices = bytes.readInt();

        bytes.seek(rotationDeformers.keyform_sources_counts + i*4);
        keyform_sources_counts = bytes.readInt();

        bytes.seek(rotationDeformers.base_angles + i*4);
        base_angles = bytes.readFloat();

    }


};
}

#endif // CROTATION_DEFORMERS_H
