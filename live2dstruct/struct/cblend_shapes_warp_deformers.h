#ifndef CBLEND_SHAPES_WARP_DEFORMERS_H
#define CBLEND_SHAPES_WARP_DEFORMERS_H

#include "live2dstruct/util/cfilereader.h"
namespace live2dstruct {
class CBlend_shapes_warp_deformers
{
public:
    class Offset{
    public:
        int target_indices;
        int blend_shape_keyform_binding_sources_begin_indices;
        int blend_shape_keyform_binding_sources_counts;


        Offset(CReader& bytes) {
            target_indices = bytes.readInt();
            blend_shape_keyform_binding_sources_begin_indices=bytes.readInt();
            blend_shape_keyform_binding_sources_counts=bytes.readInt();

        }
    };



    CBlend_shapes_warp_deformers();

    CBlend_shapes_warp_deformers(CReader& bytes, const Offset& drawOrderGroups, int i) {
        bytes.seek(drawOrderGroups.target_indices + i*4);
        target_indices = bytes.readInt();

        bytes.seek(drawOrderGroups.blend_shape_keyform_binding_sources_begin_indices + i*4);
        blend_shape_keyform_binding_sources_begin_indices = bytes.readInt();

        bytes.seek(drawOrderGroups.blend_shape_keyform_binding_sources_counts + i*4);
        blend_shape_keyform_binding_sources_counts = bytes.readInt();


    }

    int target_indices;
    int blend_shape_keyform_binding_sources_begin_indices;
    int blend_shape_keyform_binding_sources_counts;


};
}
#endif // CBLEND_SHAPES_WARP_DEFORMERS_H
