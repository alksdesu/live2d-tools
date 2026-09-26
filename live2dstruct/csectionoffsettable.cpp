#include "csectionoffsettable.h"
namespace live2dstruct {



CSectionOffsetTable::CSectionOffsetTable(CReader &bytes)
{
    mocVersion = bytes.m_byte.size() > 4 ? quint8(bytes.m_byte[4]) : 0;
    bytes.seek(0x40);//从
    count_info = new CCount_info::Offset(bytes);
    canvas_info =new CCanvas_info::Offset(bytes);
    parts = new CParts::Offset(bytes);
    deformers = new CDeformers::Offset(bytes);
    warp_deformers = new CWarp_deformers::Offset(bytes);
    rotation_deformers = new CRotation_deformers::Offset(bytes);
    art_meshes = new CArt_meshes::Offset(bytes);
    parameters = new CParameters::Offset(bytes);
    part_keyforms = new CPart_keyforms::Offset(bytes);
    warp_deformer_keyforms = new CWarp_deformer_keyforms::Offset(bytes);
    rotation_deformer_keyforms = new CRotation_deformer_keyforms::Offset(bytes);
    art_mesh_keyforms = new CArt_mesh_keyforms::Offset(bytes);
    keyform_positions = new CKeyform_positions::Offset(bytes);
    parameter_binding_indices = new CParameter_binding_indices::Offset(bytes);
    keyform_bindings = new CKeyform_bindings::Offset(bytes);
    parameter_bindings = new CParameter_bindings::Offset(bytes);
    keys = new CKeys::Offset(bytes);
    uvs = new CUvs::Offset(bytes);
    position_indices = new CPosition_indices::Offset(bytes);
    drawable_masks = new CDrawable_masks::Offset(bytes);
    draw_order_groups = new CDraw_order_groups::Offset(bytes);
    draw_order_group_objects = new CDraw_order_group_objects::Offset(bytes);
    glue = new CGlue::Offset(bytes);
    glue_info = new CGlue_info::Offset(bytes);
    glue_keyforms = new CGlue_keyforms::Offset(bytes);
    warp_deformers->Set_is_quad_source(bytes);//为什么这个这么特殊?



    //这里可能版本特殊导致的，需要对不同的版本进行测试
    parameters->InitKeyForm(bytes);
    warp_deformers->Set_keyform_color_sources_begin_indices(bytes);
    rotation_deformers->Set_keyform_color_sources_begin_indices(bytes);
    art_meshes->Set_keyform_color_sources_begin_indices(bytes);
    keyform_multiply_colors = new  CKeyform_multiply_colors::Offset(bytes);
    keyform_screen_colors = new  CKeyform_screen_colors::Offset(bytes);
    parameters->InitParaExe(bytes);    //为什么需要多获取参数占8个字节
    blend_shape_parameter_bindings = new  CBlend_shape_parameter_bindings::Offset(bytes);
    blend_shape_keyform_bindings = new  CBlend_shape_keyform_bindings::Offset(bytes);
    blend_shapes_warp_deformers = new  CBlend_shapes_warp_deformers::Offset(bytes);
    blend_shapes_art_meshes = new  CBlend_shapes_art_meshes::Offset(bytes);
    blend_shape_constraint_indices = new CBlend_shape_constraint_indices::Offset(bytes);
    blend_shape_constraints = new  CBlend_shape_constraints::Offset(bytes);
    blend_shape_constraint_values = new  CBlend_shape_constraint_values::Offset(bytes);
    if (mocVersion >= 5) {
        warp_deformer_keyforms->ReadColorOffsets(bytes);
        rotation_deformer_keyforms->ReadColorOffsets(bytes);
        art_mesh_keyforms->ReadColorOffsets(bytes);
        blend_shapes_parts = new CBlend_shape_targets::Offset(bytes);
        blend_shapes_rotation_deformers = new CBlend_shape_targets::Offset(bytes);
    } else {
        blend_shapes_parts = new CBlend_shape_targets::Offset();
        blend_shapes_rotation_deformers = new CBlend_shape_targets::Offset();
    }


}

CSectionOffsetTable::~CSectionOffsetTable()
{
    delete count_info;
    delete canvas_info;
    delete parts;
    delete deformers;
    delete warp_deformers;//变形
    delete rotation_deformers;//转动
    delete art_meshes;
    delete parameters;
    delete part_keyforms;
    delete warp_deformer_keyforms;
    delete rotation_deformer_keyforms;
    delete art_mesh_keyforms;
    delete keyform_positions;
    delete parameter_binding_indices;
    delete keyform_bindings;
    delete parameter_bindings;
    delete keys;
    delete uvs;
    delete position_indices;
    delete drawable_masks;
    delete draw_order_groups;
    delete draw_order_group_objects;
    delete glue;
    delete glue_info;
    delete glue_keyforms;
    delete blend_shape_constraint_indices;
    delete blend_shape_constraint_values;
    delete blend_shape_constraints;
    delete blend_shape_keyform_bindings;
    delete blend_shape_parameter_bindings;
    delete blend_shapes_art_meshes;
    delete blend_shapes_warp_deformers;
    delete blend_shapes_parts;
    delete blend_shapes_rotation_deformers;
    delete keyform_multiply_colors;
    delete keyform_screen_colors;

}

}
