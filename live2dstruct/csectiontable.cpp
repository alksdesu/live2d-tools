#include "csectiontable.h"
#include "csectionvalidation.h"
#include <type_traits>
namespace live2dstruct {


CSectionTable::CSectionTable(CReader& bytes)
{
    if (!bytes.IsValid()) return;
    if (!bytes.IsMoc3()) {
        bytes.Fail(QStringLiteral("moc3 文件签名或版本无效"));
        return;
    }
    if (!bytes.RequireSpan(QStringLiteral("section_offsets"), CSectionValidation::OffsetTableStart,
                           CSectionValidation::OffsetTableCount, sizeof(qint32))) return;
    sectionOffsetTable = new CSectionOffsetTable(bytes);
    if (!bytes.IsValid() || !CSectionValidation::ValidateMetadata(bytes, *sectionOffsetTable)) return;
    count_info = new CCount_info(bytes, *sectionOffsetTable->count_info);
    canvas_info = new CCanvas_info(bytes, *sectionOffsetTable->canvas_info);
    if (!bytes.IsValid() || !CSectionValidation::ValidateArrays(bytes, *sectionOffsetTable, *count_info)) return;

    const auto readItems = [&bytes](auto& target, const auto& offsets, int count) {
        using Item = std::remove_pointer_t<typename std::decay_t<decltype(target)>::value_type>;
        for (int index = 0; index < count; ++index) {
            target.push_back(new Item(bytes, offsets, index));
            if (!bytes.IsValid()) return false;
        }
        return true;
    };
    if (!readItems(parts, *sectionOffsetTable->parts, count_info->parts)) return;
    if (!readItems(deformers, *sectionOffsetTable->deformers, count_info->deformers)) return;
    if (!readItems(warp_deformers, *sectionOffsetTable->warp_deformers, count_info->warp_deformers)) return;
    if (!readItems(rotation_deformers, *sectionOffsetTable->rotation_deformers, count_info->rotation_deformers)) return;
    if (!readItems(art_meshes, *sectionOffsetTable->art_meshes, count_info->art_meshes)) return;
    if (!readItems(parameters, *sectionOffsetTable->parameters, count_info->parameters)) return;
    if (!readItems(part_keyforms, *sectionOffsetTable->part_keyforms, count_info->part_keyforms)) return;
    if (!readItems(warp_deformer_keyforms, *sectionOffsetTable->warp_deformer_keyforms, count_info->warp_deformer_keyforms)) return;
    if (!readItems(rotation_deformer_keyforms, *sectionOffsetTable->rotation_deformer_keyforms, count_info->rotation_deformer_keyforms)) return;
    if (!readItems(art_mesh_keyforms, *sectionOffsetTable->art_mesh_keyforms, count_info->art_mesh_keyforms)) return;
    keyform_positions = new CKeyform_positions(bytes, *sectionOffsetTable->keyform_positions, count_info->keyform_positions);
    if (!bytes.IsValid()) return;
    if (!readItems(parameter_binding_indices, *sectionOffsetTable->parameter_binding_indices, count_info->parameter_binding_indices)) return;
    if (!readItems(keyform_bindings, *sectionOffsetTable->keyform_bindings, count_info->keyform_bindings)) return;
    if (!readItems(parameter_bindings, *sectionOffsetTable->parameter_bindings, count_info->parameter_bindings)) return;
    keys = new CKeys(bytes, *sectionOffsetTable->keys, count_info->keys);
    if (!bytes.IsValid()) return;
    uvs = new CUvs(bytes, *sectionOffsetTable->uvs, count_info->uvs);
    if (!bytes.IsValid()) return;
    position_indices = new CPosition_indices(bytes, *sectionOffsetTable->position_indices, count_info->position_indices);
    if (!bytes.IsValid()) return;
    if (!readItems(drawable_masks, *sectionOffsetTable->drawable_masks, count_info->drawable_masks)) return;
    if (!readItems(draw_order_groups, *sectionOffsetTable->draw_order_groups, count_info->draw_order_groups)) return;
    if (!readItems(draw_order_group_objects, *sectionOffsetTable->draw_order_group_objects, count_info->draw_order_group_objects)) return;
    if (!readItems(glue, *sectionOffsetTable->glue, count_info->glue)) return;
    if (!readItems(glue_info, *sectionOffsetTable->glue_info, count_info->glue_info)) return;
    if (!readItems(glue_keyforms, *sectionOffsetTable->glue_keyforms, count_info->glue_keyforms)) return;
    if (!readItems(blend_shape_constraint_values, *sectionOffsetTable->blend_shape_constraint_values, count_info->blend_shape_constraint_values)) return;
    if (!readItems(blend_shape_constraints, *sectionOffsetTable->blend_shape_constraints, count_info->blend_shape_constraints)) return;
    if (!readItems(blend_shape_keyform_bindings, *sectionOffsetTable->blend_shape_keyform_bindings, count_info->blend_shape_keyform_bindings)) return;
    if (!readItems(blend_shape_parameter_bindings, *sectionOffsetTable->blend_shape_parameter_bindings, count_info->blend_shape_parameter_bindings)) return;
    if (!readItems(blend_shapes_art_meshes, *sectionOffsetTable->blend_shapes_art_meshes, count_info->blend_shapes_art_meshes)) return;
    if (!readItems(blend_shapes_warp_deformers, *sectionOffsetTable->blend_shapes_warp_deformers, count_info->blend_shapes_warp_deformers)) return;
    if (!readItems(keyform_multiply_colors, *sectionOffsetTable->keyform_multiply_colors, count_info->keyform_multiply_colors)) return;
    if (!readItems(keyform_screen_colors, *sectionOffsetTable->keyform_screen_colors, count_info->keyform_screen_colors)) return;
    m_valid = true;
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
