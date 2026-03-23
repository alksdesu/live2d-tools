#ifndef CCOUNT_INFO_H
#define CCOUNT_INFO_H
#include "live2dstruct/util/cfilereader.h"
namespace live2dstruct {
class CCount_info
{
public:
    class Offset{
    public:
        int count_info_offset;

        Offset(CReader& bytes) {
            count_info_offset = bytes.readInt();
        }
    };

    CCount_info(CReader& bytes,const  Offset& countInfo) {
        bytes.seek(countInfo.count_info_offset);
        parts=bytes.readInt();
        deformers=bytes.readInt();
        warp_deformers=bytes.readInt();
        rotation_deformers=bytes.readInt();
        art_meshes=bytes.readInt();
        parameters=bytes.readInt();
        part_keyforms=bytes.readInt();
        warp_deformer_keyforms=bytes.readInt();
        rotation_deformer_keyforms=bytes.readInt();
        art_mesh_keyforms=bytes.readInt();
        keyform_positions=bytes.readInt();
        parameter_binding_indices=bytes.readInt();
        keyform_bindings=bytes.readInt();
        parameter_bindings=bytes.readInt();
        keys=bytes.readInt();
        uvs=bytes.readInt();
        position_indices=bytes.readInt();
        drawable_masks=bytes.readInt();
        draw_order_groups=bytes.readInt();
        draw_order_group_objects=bytes.readInt();
        glue=bytes.readInt();
        glue_info=bytes.readInt();
        glue_keyforms=bytes.readInt();


        keyform_multiply_colors=bytes.readInt();
        keyform_screen_colors=bytes.readInt();
        blend_shape_parameter_bindings=bytes.readInt();
        blend_shape_keyform_bindings=bytes.readInt();
        blend_shapes_warp_deformers=bytes.readInt();
        blend_shapes_art_meshes=bytes.readInt();
        blend_shape_constraint_indices=bytes.readInt();
        blend_shape_constraints=bytes.readInt();
        blend_shape_constraint_values=bytes.readInt();
    }

    int parts;
    int deformers;
    int warp_deformers;
    int rotation_deformers;
    int art_meshes;
    int parameters;
    int part_keyforms;
    int warp_deformer_keyforms;
    int rotation_deformer_keyforms;
    int art_mesh_keyforms;
    int keyform_positions;
    int parameter_binding_indices;
    int keyform_bindings;
    int parameter_bindings;
    int keys;
    int uvs;
    int position_indices;
    int drawable_masks;
    int draw_order_groups;
    int draw_order_group_objects;
    int glue;
    int glue_info;
    int glue_keyforms;



    int blend_shape_constraint_indices;
    int blend_shape_constraint_values;
    int blend_shape_constraints;
    int blend_shape_keyform_bindings;
    int blend_shape_parameter_bindings;
    int blend_shapes_art_meshes;
    int blend_shapes_warp_deformers;
    int keyform_multiply_colors;
    int keyform_screen_colors;
};
}

#endif // CCOUNT_INFO_H
