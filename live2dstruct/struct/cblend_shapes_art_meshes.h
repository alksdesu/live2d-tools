#ifndef CBLEND_SHAPES_ART_MESHES_H
#define CBLEND_SHAPES_ART_MESHES_H

#include "live2dstruct/util/cfilereader.h"
namespace live2dstruct {
class CBlend_shapes_art_meshes
{
public:

    class Offset{
    public:
        int target_indices;
        int blend_shape_keyform_binding_sources_begin_indices;
        int blend_shape_keyform_binding_sources_counts;

        Offset(CReader& bytes) {
            target_indices =bytes.readInt();
            blend_shape_keyform_binding_sources_begin_indices=bytes.readInt();
            blend_shape_keyform_binding_sources_counts=bytes.readInt();

        }
    };

    CBlend_shapes_art_meshes();
    int  target_indices;
    int blend_shape_keyform_binding_sources_begin_indices;
    int blend_shape_keyform_binding_sources_counts;

    CBlend_shapes_art_meshes(CReader& bytes, const Offset& drawOrderGroups, int i) {
        bytes.seek(drawOrderGroups.target_indices + i*4);
        target_indices = bytes.readInt();

        bytes.seek(drawOrderGroups.blend_shape_keyform_binding_sources_begin_indices + i*4);
        blend_shape_keyform_binding_sources_begin_indices = bytes.readInt();

        bytes.seek(drawOrderGroups.blend_shape_keyform_binding_sources_counts + i*4);
        blend_shape_keyform_binding_sources_counts = bytes.readInt();


    }




};
}
#endif // CBLEND_SHAPES_ART_MESHES_H
