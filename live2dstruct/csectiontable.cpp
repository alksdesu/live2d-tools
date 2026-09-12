#include "csectiontable.h"
namespace live2dstruct {


CSectionTable::CSectionTable(CReader&  bytes)  {//数据,以及数据起始位位置
    if (!bytes.IsValid() || !bytes.IsMoc3()) return;
    sectionOffsetTable = new CSectionOffsetTable(bytes);
    count_info = new CCount_info(bytes,*(sectionOffsetTable->count_info));
    canvas_info = new CCanvas_info(bytes,*(sectionOffsetTable->canvas_info));
    const auto validCount = [](int value) { return value >= 0 && value <= 1000000; };
    if (!validCount(count_info->parts) || !validCount(count_info->deformers) ||
        !validCount(count_info->warp_deformers) || !validCount(count_info->rotation_deformers) ||
        !validCount(count_info->art_meshes) || !validCount(count_info->parameters) ||
        !validCount(count_info->keyform_positions) || !validCount(count_info->uvs) ||
        !validCount(count_info->position_indices) ||
        !validCount(count_info->part_keyforms) || !validCount(count_info->warp_deformer_keyforms) ||
        !validCount(count_info->rotation_deformer_keyforms) || !validCount(count_info->art_mesh_keyforms) ||
        !validCount(count_info->parameter_binding_indices) || !validCount(count_info->keyform_bindings) ||
        !validCount(count_info->parameter_bindings) || !validCount(count_info->keys) ||
        !validCount(count_info->drawable_masks) || !validCount(count_info->draw_order_groups) ||
        !validCount(count_info->draw_order_group_objects) || !validCount(count_info->glue) ||
        !validCount(count_info->glue_info) || !validCount(count_info->glue_keyforms) ||
        !validCount(count_info->blend_shape_constraint_indices) ||
        !validCount(count_info->blend_shape_constraint_values) ||
        !validCount(count_info->blend_shape_constraints) ||
        !validCount(count_info->blend_shape_keyform_bindings) ||
        !validCount(count_info->blend_shape_parameter_bindings) ||
        !validCount(count_info->blend_shapes_art_meshes) ||
        !validCount(count_info->blend_shapes_warp_deformers) ||
        !validCount(count_info->keyform_multiply_colors) ||
        !validCount(count_info->keyform_screen_colors)) return;
    for (int i = 0 ;i < count_info->parts;i++){
        parts.push_back(new CParts(bytes,*(sectionOffsetTable->parts),i));
    }
    for (int i = 0 ;i < count_info->deformers;i++){
        deformers.push_back(new CDeformers(bytes,*(sectionOffsetTable->deformers),i));
    }
    for (int i = 0 ;i < count_info->warp_deformers;i++){
        warp_deformers.push_back(new CWarp_deformers(bytes,*(sectionOffsetTable->warp_deformers),i));

    }

    for (int i = 0 ;i < count_info->rotation_deformers;i++){
        rotation_deformers.push_back(new CRotation_deformers(bytes,*(sectionOffsetTable->rotation_deformers),i));

    }

    for (int i = 0 ;i < count_info->art_meshes;i++){
        art_meshes.push_back(new CArt_meshes(bytes,*(sectionOffsetTable->art_meshes),i));

    }

    for (int i = 0 ;i < count_info->parameters;i++){
        parameters.push_back(new CParameters(bytes,*(sectionOffsetTable->parameters),i));

    }

    for (int i = 0 ;i < count_info->part_keyforms;i++){
        part_keyforms.push_back(new CPart_keyforms(bytes,*sectionOffsetTable->part_keyforms,i));

    }

    for (int i = 0 ;i < count_info->warp_deformer_keyforms;i++){
        warp_deformer_keyforms.push_back(new CWarp_deformer_keyforms(bytes,*sectionOffsetTable->warp_deformer_keyforms,i));
    }

    for (int i = 0 ;i < count_info->rotation_deformer_keyforms;i++){
        rotation_deformer_keyforms.push_back(new CRotation_deformer_keyforms(bytes,*sectionOffsetTable->rotation_deformer_keyforms,i));
    }


    for (int i = 0 ;i < count_info->art_mesh_keyforms;i++){
        art_mesh_keyforms.push_back(new CArt_mesh_keyforms(bytes,*sectionOffsetTable->art_mesh_keyforms,i));

    }

    //就是这里导致文件解析速度太慢了
    keyform_positions = new CKeyform_positions(bytes,*sectionOffsetTable->keyform_positions,count_info->keyform_positions);




    for (int i = 0 ;i < count_info->parameter_binding_indices;i++){
        parameter_binding_indices.push_back(new CParameter_binding_indices(bytes,*sectionOffsetTable->parameter_binding_indices,i));

    }

    for (int i = 0 ;i < count_info->keyform_bindings;i++){

        keyform_bindings.push_back(new CKeyform_bindings(bytes,*sectionOffsetTable->keyform_bindings,i));

    }

    for (int i = 0 ;i < count_info->parameter_bindings;i++){
        parameter_bindings.push_back(new CParameter_bindings(bytes,*sectionOffsetTable->parameter_bindings,i));

    }


    keys = new CKeys(bytes,*sectionOffsetTable->keys,count_info->keys);
    uvs = new CUvs(bytes,*sectionOffsetTable->uvs,count_info->uvs);


    position_indices = new CPosition_indices(bytes,*sectionOffsetTable->position_indices,count_info->position_indices);

    for (int i = 0 ;i < count_info->drawable_masks;i++){
        drawable_masks.push_back(new CDrawable_masks(bytes,*sectionOffsetTable->drawable_masks,i));

    }

    for (int i = 0 ;i < count_info->draw_order_groups;i++){
        draw_order_groups.push_back(new CDraw_order_groups(bytes,*sectionOffsetTable->draw_order_groups,i));

    }

    for (int i = 0 ;i < count_info->draw_order_group_objects;i++){
        draw_order_group_objects.push_back(new CDraw_order_group_objects(bytes,*sectionOffsetTable->draw_order_group_objects,i));

    }

    for (int i = 0 ;i < count_info->glue;i++){
        glue.push_back(new CGlue(bytes,*sectionOffsetTable->glue,i));

    }

    for (int i = 0 ;i < count_info->glue_info;i++){
        glue_info.push_back(new CGlue_info(bytes,*sectionOffsetTable->glue_info,i));

    }

    for (int i = 0 ;i < count_info->glue_keyforms;i++){
        glue_keyforms.push_back(new CGlue_keyforms(bytes,*sectionOffsetTable->glue_keyforms,i));

    }



    for(int i = 0 ;i < count_info->blend_shape_constraint_indices;i++){
        //blend_shape_constraint_indices.push_back(new CBlend_shape_constraint_indices(bytes,*sectionOffsetTable->blend_shape_constraint_indices,i));

    }



    for(int i = 0 ;i < count_info->blend_shape_constraint_values;i++){
        blend_shape_constraint_values.push_back(new CBlend_shape_constraint_values(bytes,*sectionOffsetTable->blend_shape_constraint_values,i));

    }


    for(int i = 0 ;i < count_info->blend_shape_constraints;i++){
        blend_shape_constraints.push_back(new CBlend_shape_constraints(bytes,*sectionOffsetTable->blend_shape_constraints,i));

    }




    for(int i = 0 ;i < count_info->blend_shape_keyform_bindings;i++){
        blend_shape_keyform_bindings.push_back(new CBlend_shape_keyform_bindings(bytes,*sectionOffsetTable->blend_shape_keyform_bindings,i));

    }


    for(int i = 0 ;i < count_info->blend_shape_parameter_bindings;i++){
        blend_shape_parameter_bindings.push_back(new CBlend_shape_parameter_bindings(bytes,*sectionOffsetTable->blend_shape_parameter_bindings,i));

    }



    for(int i = 0 ;i < count_info->blend_shapes_art_meshes;i++){
        blend_shapes_art_meshes.push_back(new CBlend_shapes_art_meshes(bytes,*sectionOffsetTable->blend_shapes_art_meshes,i));

    }


    for(int i = 0 ;i < count_info->blend_shapes_warp_deformers;i++){
        blend_shapes_warp_deformers.push_back(new CBlend_shapes_warp_deformers(bytes,*sectionOffsetTable->blend_shapes_warp_deformers,i));

    }



    for(int i = 0 ;i < count_info->keyform_multiply_colors;i++){
        keyform_multiply_colors.push_back(new CKeyform_multiply_colors(bytes,*sectionOffsetTable->keyform_multiply_colors,i));

    }


    for(int i = 0 ;i < count_info->keyform_screen_colors;i++){
        keyform_screen_colors.push_back(new CKeyform_screen_colors(bytes,*sectionOffsetTable->keyform_screen_colors,i));

    }

    m_valid = bytes.IsValid();

}

CSectionTable::~CSectionTable()
{

    delete sectionOffsetTable;
    delete count_info;
    delete canvas_info;
    for(int i = 0 ;i < parts.size();i++){
        delete parts[i];
    }
    parts.clear();

    for(int i = 0 ;i < deformers.size();i++){
        delete deformers[i];
    }
    deformers.clear();

    for(int i = 0 ;i < warp_deformers.size();i++){
        delete warp_deformers[i];
    }
    warp_deformers.clear();

    for(int i = 0 ;i < rotation_deformers.size();i++){
        delete rotation_deformers[i];
    }
    rotation_deformers.clear();

    for(int i = 0 ;i < art_meshes.size();i++){
        delete art_meshes[i];
    }
    art_meshes.clear();

    for(int i = 0 ;i < parameters.size();i++){
        delete parameters[i];
    }
    parameters.clear();


    for(int i = 0 ;i < part_keyforms.size();i++){
        delete part_keyforms[i];
    }
    part_keyforms.clear();



    for(int i = 0 ;i < warp_deformer_keyforms.size();i++){
        delete warp_deformer_keyforms[i];
    }
    warp_deformer_keyforms.clear();



    for(int i = 0 ;i < rotation_deformer_keyforms.size();i++){
        delete rotation_deformer_keyforms[i];
    }
    rotation_deformer_keyforms.clear();



    for(int i = 0 ;i < art_mesh_keyforms.size();i++){
        delete art_mesh_keyforms[i];
    }
    art_mesh_keyforms.clear();


    delete keyform_positions;


    for(int i = 0 ;i < parameter_binding_indices.size();i++){
        delete parameter_binding_indices[i];
    }
    parameter_binding_indices.clear();

    for(int i = 0 ;i < keyform_bindings.size();i++){
        delete keyform_bindings[i];
    }
    keyform_bindings.clear();

    for(int i = 0 ;i < parameter_bindings.size();i++){
        delete parameter_bindings[i];
    }
    parameter_bindings.clear();


    delete keys;

    delete uvs;


    delete position_indices;

    for(int i = 0 ;i < drawable_masks.size();i++){
        delete drawable_masks[i];
    }
    drawable_masks.clear();


    for(int i = 0 ;i < draw_order_groups.size();i++){
        delete draw_order_groups[i];
    }
    draw_order_groups.clear();


    for(int i = 0 ;i < draw_order_group_objects.size();i++){
        delete draw_order_group_objects[i];
    }
    draw_order_group_objects.clear();


    for(int i = 0 ;i < glue.size();i++){
        delete glue[i];
    }
    glue.clear();


    for(int i = 0 ;i < glue_info.size();i++){
        delete glue_info[i];
    }
    glue_info.clear();

    for(int i = 0 ;i < glue_keyforms.size();i++){
        delete glue_keyforms[i];
    }
    glue_keyforms.clear();

    for (auto* item : blend_shape_constraint_indices) delete item;
    for (auto* item : blend_shape_constraint_values) delete item;
    for (auto* item : blend_shape_constraints) delete item;
    for (auto* item : blend_shape_keyform_bindings) delete item;
    for (auto* item : blend_shape_parameter_bindings) delete item;
    for (auto* item : blend_shapes_art_meshes) delete item;
    for (auto* item : blend_shapes_warp_deformers) delete item;
    for (auto* item : keyform_multiply_colors) delete item;
    for (auto* item : keyform_screen_colors) delete item;





}
}
