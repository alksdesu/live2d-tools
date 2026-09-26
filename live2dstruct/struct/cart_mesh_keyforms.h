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

        int multiply_color_sources_indices = 0;
        int screen_color_sources_indices = 0;

        void ReadColorOffsets(CReader& bytes)
        {
            multiply_color_sources_indices = bytes.readInt();
            screen_color_sources_indices = bytes.readInt();
        }

        Offset(CReader& bytes) {
            opacities = bytes.readInt();
            draw_orders = bytes.readInt();
            keyform_position_sources_begin_indices = bytes.readInt();
        }
    };
    int multiply_color_sources_index = -1;
    int screen_color_sources_index = -1;


public:
    CArt_mesh_keyforms(CReader& bytes,const Offset& artMeshKeyforms, int i) {
        if (artMeshKeyforms.multiply_color_sources_indices) {
            bytes.seek(artMeshKeyforms.multiply_color_sources_indices + i * 4);
            multiply_color_sources_index = bytes.readInt();
        }
        if (artMeshKeyforms.screen_color_sources_indices) {
            bytes.seek(artMeshKeyforms.screen_color_sources_indices + i * 4);
            screen_color_sources_index = bytes.readInt();
        }

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
