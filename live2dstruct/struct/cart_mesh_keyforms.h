#ifndef CART_MESH_KEYFORMS_H
#define CART_MESH_KEYFORMS_H
#include "live2dstruct/util/cfilereader.h"

namespace live2dstruct {
class CArt_mesh_keyforms
{
public:
    class Offset{
    public:
        int opacities;
        int draw_orders;
        int keyform_position_sources_begin_indices;

        Offset(CReader& bytes) {
            opacities = bytes.readInt();
            draw_orders = bytes.readInt();
            keyform_position_sources_begin_indices = bytes.readInt();
        }
    };

public:
    CArt_mesh_keyforms(CReader& bytes,const Offset& artMeshKeyforms, int i) {
        bytes.seek(artMeshKeyforms.opacities + i*4);
        opacities = bytes.readFloat();

        bytes.seek(artMeshKeyforms.draw_orders + i*4);
        draw_orders = bytes.readFloat();

        bytes.seek(artMeshKeyforms.keyform_position_sources_begin_indices + i*4);
        keyform_position_sources_begin_indices = bytes.readInt();

    }

    float opacities;//不透明度
    float draw_orders;//渲染层级
    int keyform_position_sources_begin_indices;//未知

};
}
#endif // CART_MESH_KEYFORMS_H
