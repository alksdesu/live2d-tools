#include "csectionvalidation.h"
#include "csectionoffsettable.h"
#include "live2dstruct/struct/ccount_info.h"
#include "live2dstruct/util/cfilereader.h"

namespace live2dstruct {
namespace {
constexpr qint64 CountInfoBytes = 32 * 4;
constexpr qint64 CanvasInfoBytes = 5 * 4;
constexpr qint64 MaximumCount = 1000000;

struct Count
{
    const char* name;
    qint64 value;
    const void* offsets;
};

struct Column
{
    const char* name;
    qint64 offset;
    qint64 count;
    qint64 width = 4;
    bool optional = false;
};

bool overlaps(qint64 first, qint64 firstSize, qint64 second, qint64 secondSize)
{
    return first < second + secondSize && second < first + firstSize;
}
}

bool CSectionValidation::ValidateMetadata(CReader& reader, const CSectionOffsetTable& offsets)
{
    if (!reader.IsValid()) return false;
    if (!reader.RequireSpan(QStringLiteral("offset_table"), OffsetTableStart, OffsetTableCount, 4)) return false;
    if (!offsets.count_info) return reader.Fail(QStringLiteral("moc3 缺少区段偏移：count_info"));
    if (!offsets.canvas_info) return reader.Fail(QStringLiteral("moc3 缺少区段偏移：canvas_info"));
    const qint64 countOffset = offsets.count_info->count_info_offset;
    const qint64 canvasOffset = offsets.canvas_info->CanvasOffset;
    if (!reader.RequireSpan(QStringLiteral("count_info"), countOffset, 1, CountInfoBytes) ||
        !reader.RequireSpan(QStringLiteral("canvas_info"), canvasOffset, 1, CanvasInfoBytes)) return false;
    if (countOffset < OffsetTableEnd)
        return reader.Fail(QStringLiteral("moc3 count_info 与文件头或偏移表重叠"));
    if (canvasOffset < OffsetTableEnd)
        return reader.Fail(QStringLiteral("moc3 canvas_info 与文件头或偏移表重叠"));
    if (overlaps(countOffset, CountInfoBytes, canvasOffset, CanvasInfoBytes))
        return reader.Fail(QStringLiteral("moc3 count_info 与 canvas_info 重叠"));
    return true;
}

bool CSectionValidation::ValidateArrays(CReader& reader, const CSectionOffsetTable& offsets, const CCount_info& info)
{
    if (!ValidateMetadata(reader, offsets)) return false;
    const Count counts[]{
        {"parts", info.parts, offsets.parts},
        {"deformers", info.deformers, offsets.deformers},
        {"warp_deformers", info.warp_deformers, offsets.warp_deformers},
        {"rotation_deformers", info.rotation_deformers, offsets.rotation_deformers},
        {"art_meshes", info.art_meshes, offsets.art_meshes},
        {"parameters", info.parameters, offsets.parameters},
        {"part_keyforms", info.part_keyforms, offsets.part_keyforms},
        {"warp_deformer_keyforms", info.warp_deformer_keyforms, offsets.warp_deformer_keyforms},
        {"rotation_deformer_keyforms", info.rotation_deformer_keyforms, offsets.rotation_deformer_keyforms},
        {"art_mesh_keyforms", info.art_mesh_keyforms, offsets.art_mesh_keyforms},
        {"keyform_positions", info.keyform_positions, offsets.keyform_positions},
        {"parameter_binding_indices", info.parameter_binding_indices, offsets.parameter_binding_indices},
        {"keyform_bindings", info.keyform_bindings, offsets.keyform_bindings},
        {"parameter_bindings", info.parameter_bindings, offsets.parameter_bindings},
        {"keys", info.keys, offsets.keys},
        {"uvs", info.uvs, offsets.uvs},
        {"position_indices", info.position_indices, offsets.position_indices},
        {"drawable_masks", info.drawable_masks, offsets.drawable_masks},
        {"draw_order_groups", info.draw_order_groups, offsets.draw_order_groups},
        {"draw_order_group_objects", info.draw_order_group_objects, offsets.draw_order_group_objects},
        {"glue", info.glue, offsets.glue},
        {"glue_info", info.glue_info, offsets.glue_info},
        {"glue_keyforms", info.glue_keyforms, offsets.glue_keyforms},
        {"keyform_multiply_colors", info.keyform_multiply_colors, offsets.keyform_multiply_colors},
        {"keyform_screen_colors", info.keyform_screen_colors, offsets.keyform_screen_colors},
        {"blend_shape_parameter_bindings", info.blend_shape_parameter_bindings, offsets.blend_shape_parameter_bindings},
        {"blend_shape_keyform_bindings", info.blend_shape_keyform_bindings, offsets.blend_shape_keyform_bindings},
        {"blend_shapes_warp_deformers", info.blend_shapes_warp_deformers, offsets.blend_shapes_warp_deformers},
        {"blend_shapes_art_meshes", info.blend_shapes_art_meshes, offsets.blend_shapes_art_meshes},
        {"blend_shape_constraint_indices", info.blend_shape_constraint_indices, offsets.blend_shape_constraint_indices},
        {"blend_shape_constraints", info.blend_shape_constraints, offsets.blend_shape_constraints},
        {"blend_shape_constraint_values", info.blend_shape_constraint_values, offsets.blend_shape_constraint_values},
    };
    static_assert(sizeof(counts) / sizeof(counts[0]) * 4 == CountInfoBytes);
    for (const Count& count : counts) {
        const QString label = QString::fromLatin1(count.name);
        if (count.value < 0 || count.value > MaximumCount)
            return reader.Fail(QStringLiteral("moc3 count_info.%1 数量越界：%2，允许 0..%3")
                                   .arg(label).arg(count.value).arg(MaximumCount));
        if (!count.offsets)
            return reader.Fail(QStringLiteral("moc3 缺少区段偏移：%1").arg(label));
    }
    const Column columns[]{
        {"parts.runtime_space_0", offsets.parts->runtime_space_0, info.parts, 4, true},
        {"parts.ids", offsets.parts->ids, info.parts, 64},
        {"parts.keyform_binding_sources_indices", offsets.parts->keyform_binding_sources_indices, info.parts},
        {"parts.keyform_sources_begin_indices", offsets.parts->keyform_sources_begin_indices, info.parts},
        {"parts.keyform_sources_counts", offsets.parts->keyform_sources_counts, info.parts},
        {"parts.is_visible", offsets.parts->is_visible, info.parts},
        {"parts.is_enabled", offsets.parts->is_enabled, info.parts},
        {"parts.parent_part_indices", offsets.parts->parent_part_indices, info.parts},
        {"deformers.runtime_space_0", offsets.deformers->runtime_space_0, info.deformers},
        {"deformers.ids", offsets.deformers->ids, info.deformers, 64},
        {"deformers.keyform_binding_sources_indices", offsets.deformers->keyform_binding_sources_indices, info.deformers},
        {"deformers.is_visible", offsets.deformers->is_visible, info.deformers},
        {"deformers.is_enabled", offsets.deformers->is_enabled, info.deformers},
        {"deformers.parent_part_indices", offsets.deformers->parent_part_indices, info.deformers},
        {"deformers.parent_deformer_indices", offsets.deformers->parent_deformer_indices, info.deformers},
        {"deformers.types", offsets.deformers->types, info.deformers},
        {"deformers.specific_sources_indices", offsets.deformers->specific_sources_indices, info.deformers},
        {"warp_deformers.keyform_binding_sources_indices", offsets.warp_deformers->keyform_binding_sources_indices, info.warp_deformers},
        {"warp_deformers.keyform_sources_begin_indices", offsets.warp_deformers->keyform_sources_begin_indices, info.warp_deformers},
        {"warp_deformers.keyform_sources_counts", offsets.warp_deformers->keyform_sources_counts, info.warp_deformers},
        {"warp_deformers.vertex_counts", offsets.warp_deformers->vertex_counts, info.warp_deformers},
        {"warp_deformers.rows", offsets.warp_deformers->rows, info.warp_deformers},
        {"warp_deformers.columns", offsets.warp_deformers->columns, info.warp_deformers},
        {"warp_deformers.is_quad_source", offsets.warp_deformers->is_quad_source, info.warp_deformers, 4, true},
        {"warp_deformers.keyform_color_sources_begin_indices", offsets.warp_deformers->keyform_color_sources_begin_indices, info.warp_deformers, 4, true},
        {"rotation_deformers.keyform_binding_sources_indices", offsets.rotation_deformers->keyform_binding_sources_indices, info.rotation_deformers},
        {"rotation_deformers.keyform_sources_begin_indices", offsets.rotation_deformers->keyform_sources_begin_indices, info.rotation_deformers},
        {"rotation_deformers.keyform_sources_counts", offsets.rotation_deformers->keyform_sources_counts, info.rotation_deformers},
        {"rotation_deformers.base_angles", offsets.rotation_deformers->base_angles, info.rotation_deformers},
        {"rotation_deformers.keyform_color_sources_begin_indices", offsets.rotation_deformers->keyform_color_sources_begin_indices, info.rotation_deformers, 4, true},
        {"art_meshes.runtime_space_0", offsets.art_meshes->runtime_space_0, info.art_meshes},
        {"art_meshes.runtime_space_1", offsets.art_meshes->runtime_space_1, info.art_meshes},
        {"art_meshes.runtime_space_2", offsets.art_meshes->runtime_space_2, info.art_meshes},
        {"art_meshes.runtime_space_3", offsets.art_meshes->runtime_space_3, info.art_meshes},
        {"art_meshes.ids", offsets.art_meshes->ids, info.art_meshes, 64},
        {"art_meshes.keyform_binding_sources_indices", offsets.art_meshes->keyform_binding_sources_indices, info.art_meshes},
        {"art_meshes.keyform_sources_begin_indices", offsets.art_meshes->keyform_sources_begin_indices, info.art_meshes},
        {"art_meshes.keyform_sources_counts", offsets.art_meshes->keyform_sources_counts, info.art_meshes},
        {"art_meshes.is_visible", offsets.art_meshes->is_visible, info.art_meshes},
        {"art_meshes.is_enabled", offsets.art_meshes->is_enabled, info.art_meshes},
        {"art_meshes.parent_part_indices", offsets.art_meshes->parent_part_indices, info.art_meshes},
        {"art_meshes.parent_deformer_indices", offsets.art_meshes->parent_deformer_indices, info.art_meshes},
        {"art_meshes.texture_nos", offsets.art_meshes->texture_nos, info.art_meshes},
        {"art_meshes.drawable_flags", offsets.art_meshes->drawable_flags, info.art_meshes, 1},
        {"art_meshes.vertex_counts", offsets.art_meshes->vertex_counts, info.art_meshes},
        {"art_meshes.uv_sources_begin_indices", offsets.art_meshes->uv_sources_begin_indices, info.art_meshes},
        {"art_meshes.position_index_sources_begin_indices", offsets.art_meshes->position_index_sources_begin_indices, info.art_meshes},
        {"art_meshes.position_index_sources_counts", offsets.art_meshes->position_index_sources_counts, info.art_meshes},
        {"art_meshes.drawable_mask_sources_begin_indices", offsets.art_meshes->drawable_mask_sources_begin_indices, info.art_meshes},
        {"art_meshes.drawable_mask_sources_counts", offsets.art_meshes->drawable_mask_sources_counts, info.art_meshes},
        {"art_meshes.keyform_color_sources_begin_indices", offsets.art_meshes->keyform_color_sources_begin_indices, info.art_meshes, 4, true},
        {"parameters.runtime_space_0", offsets.parameters->runtime_space_0, info.parameters},
        {"parameters.ids", offsets.parameters->ids, info.parameters, 64},
        {"parameters.max_values", offsets.parameters->max_values, info.parameters},
        {"parameters.min_values", offsets.parameters->min_values, info.parameters},
        {"parameters.default_values", offsets.parameters->default_values, info.parameters},
        {"parameters.is_repeat", offsets.parameters->is_repeat, info.parameters},
        {"parameters.decimal_places", offsets.parameters->decimal_places, info.parameters},
        {"parameters.parameter_binding_sources_begin_indices", offsets.parameters->parameter_binding_sources_begin_indices, info.parameters},
        {"parameters.parameter_binding_sources_counts", offsets.parameters->parameter_binding_sources_counts, info.parameters},
        {"parameters.extension_runtime_space_0", offsets.parameters->extension_runtime_space_0, info.parameters, 4, true},
        {"parameters.keys_sources_begin_indices", offsets.parameters->keys_sources_begin_indices, info.parameters, 4, true},
        {"parameters.keys_sources_counts", offsets.parameters->keys_sources_counts, info.parameters, 4, true},
        {"parameters.parameter_types", offsets.parameters->parameter_types, info.parameters, 4, true},
        {"parameters.blend_shape_parameter_binding_sources_begin_indices", offsets.parameters->blend_shape_parameter_binding_sources_begin_indices, info.parameters, 4, true},
        {"parameters.blend_shape_parameter_binding_sources_counts", offsets.parameters->blend_shape_parameter_binding_sources_counts, info.parameters, 4, true},
        {"part_keyforms.draw_orders", offsets.part_keyforms->draw_orders, info.part_keyforms},
        {"warp_deformer_keyforms.opacities", offsets.warp_deformer_keyforms->opacities, info.warp_deformer_keyforms},
        {"warp_deformer_keyforms.keyform_position_sources_begin_indices", offsets.warp_deformer_keyforms->keyform_position_sources_begin_indices, info.warp_deformer_keyforms},
        {"rotation_deformer_keyforms.opacities", offsets.rotation_deformer_keyforms->opacities, info.rotation_deformer_keyforms},
        {"rotation_deformer_keyforms.angles", offsets.rotation_deformer_keyforms->angles, info.rotation_deformer_keyforms},
        {"rotation_deformer_keyforms.origin_x", offsets.rotation_deformer_keyforms->origin_x, info.rotation_deformer_keyforms},
        {"rotation_deformer_keyforms.origin_y", offsets.rotation_deformer_keyforms->origin_y, info.rotation_deformer_keyforms},
        {"rotation_deformer_keyforms.scales", offsets.rotation_deformer_keyforms->scales, info.rotation_deformer_keyforms},
        {"rotation_deformer_keyforms.is_reflect_x", offsets.rotation_deformer_keyforms->is_reflect_x, info.rotation_deformer_keyforms},
        {"rotation_deformer_keyforms.is_reflect_y", offsets.rotation_deformer_keyforms->is_reflect_y, info.rotation_deformer_keyforms},
        {"art_mesh_keyforms.opacities", offsets.art_mesh_keyforms->opacities, info.art_mesh_keyforms},
        {"art_mesh_keyforms.draw_orders", offsets.art_mesh_keyforms->draw_orders, info.art_mesh_keyforms},
        {"art_mesh_keyforms.keyform_position_sources_begin_indices", offsets.art_mesh_keyforms->keyform_position_sources_begin_indices, info.art_mesh_keyforms},
        {"keyform_positions.xys", offsets.keyform_positions->xys, info.keyform_positions},
        {"parameter_binding_indices.binding_sources_indices", offsets.parameter_binding_indices->binding_sources_indices, info.parameter_binding_indices},
        {"keyform_bindings.parameter_binding_index_sources_begin_indices", offsets.keyform_bindings->parameter_binding_index_sources_begin_indices, info.keyform_bindings},
        {"keyform_bindings.parameter_binding_index_sources_counts", offsets.keyform_bindings->parameter_binding_index_sources_counts, info.keyform_bindings},
        {"parameter_bindings.keys_sources_begin_indices", offsets.parameter_bindings->keys_sources_begin_indices, info.parameter_bindings},
        {"parameter_bindings.keys_sources_counts", offsets.parameter_bindings->keys_sources_counts, info.parameter_bindings},
        {"keys.values", offsets.keys->values, info.keys},
        {"uvs.uvs", offsets.uvs->uvs, info.uvs},
        {"position_indices.indices", offsets.position_indices->indices, info.position_indices, 2},
        {"drawable_masks.art_mesh_sources_indices", offsets.drawable_masks->art_mesh_sources_indices, info.drawable_masks},
        {"draw_order_groups.object_sources_begin_indices", offsets.draw_order_groups->object_sources_begin_indices, info.draw_order_groups},
        {"draw_order_groups.object_sources_counts", offsets.draw_order_groups->object_sources_counts, info.draw_order_groups},
        {"draw_order_groups.object_sources_total_counts", offsets.draw_order_groups->object_sources_total_counts, info.draw_order_groups},
        {"draw_order_groups.maximum_draw_orders", offsets.draw_order_groups->maximum_draw_orders, info.draw_order_groups},
        {"draw_order_groups.minimum_draw_orders", offsets.draw_order_groups->minimum_draw_orders, info.draw_order_groups},
        {"draw_order_group_objects.types", offsets.draw_order_group_objects->types, info.draw_order_group_objects},
        {"draw_order_group_objects.indices", offsets.draw_order_group_objects->indices, info.draw_order_group_objects},
        {"draw_order_group_objects.self_indices", offsets.draw_order_group_objects->self_indices, info.draw_order_group_objects},
        {"glue.runtime_space", offsets.glue->runtime_space, info.glue},
        {"glue.ids", offsets.glue->ids, info.glue, 64},
        {"glue.keyform_binding_sources_indices", offsets.glue->keyform_binding_sources_indices, info.glue},
        {"glue.keyform_sources_begin_indices", offsets.glue->keyform_sources_begin_indices, info.glue},
        {"glue.keyform_sources_counts", offsets.glue->keyform_sources_counts, info.glue},
        {"glue.art_mesh_indices_a", offsets.glue->art_mesh_indices_a, info.glue},
        {"glue.art_mesh_indices_b", offsets.glue->art_mesh_indices_b, info.glue},
        {"glue.glue_info_sources_begin_indices", offsets.glue->glue_info_sources_begin_indices, info.glue},
        {"glue.glue_info_sources_counts", offsets.glue->glue_info_sources_counts, info.glue},
        {"glue_info.weights", offsets.glue_info->weights, info.glue_info},
        {"glue_info.position_indices", offsets.glue_info->position_indices, info.glue_info, 2},
        {"glue_keyforms.intensities", offsets.glue_keyforms->intensities, info.glue_keyforms},
        {"keyform_multiply_colors.r", offsets.keyform_multiply_colors->r, info.keyform_multiply_colors},
        {"keyform_multiply_colors.g", offsets.keyform_multiply_colors->g, info.keyform_multiply_colors},
        {"keyform_multiply_colors.b", offsets.keyform_multiply_colors->b, info.keyform_multiply_colors},
        {"keyform_screen_colors.r", offsets.keyform_screen_colors->r, info.keyform_screen_colors},
        {"keyform_screen_colors.g", offsets.keyform_screen_colors->g, info.keyform_screen_colors},
        {"keyform_screen_colors.b", offsets.keyform_screen_colors->b, info.keyform_screen_colors},
        {"blend_shape_parameter_bindings.keys_sources_begin_indices", offsets.blend_shape_parameter_bindings->keys_sources_begin_indices, info.blend_shape_parameter_bindings},
        {"blend_shape_parameter_bindings.keys_sources_counts", offsets.blend_shape_parameter_bindings->keys_sources_counts, info.blend_shape_parameter_bindings},
        {"blend_shape_parameter_bindings.base_key_indices", offsets.blend_shape_parameter_bindings->base_key_indices, info.blend_shape_parameter_bindings},
        {"blend_shape_keyform_bindings.parameter_binding_sources_indices", offsets.blend_shape_keyform_bindings->parameter_binding_sources_indices, info.blend_shape_keyform_bindings},
        {"blend_shape_keyform_bindings.keyform_sources_blend_shape_indices", offsets.blend_shape_keyform_bindings->keyform_sources_blend_shape_indices, info.blend_shape_keyform_bindings},
        {"blend_shape_keyform_bindings.keyform_sources_blend_shape_counts", offsets.blend_shape_keyform_bindings->keyform_sources_blend_shape_counts, info.blend_shape_keyform_bindings},
        {"blend_shape_keyform_bindings.blend_shape_constraint_index_sources_begin_indices", offsets.blend_shape_keyform_bindings->blend_shape_constraint_index_sources_begin_indices, info.blend_shape_keyform_bindings},
        {"blend_shape_keyform_bindings.blend_shape_constraint_index_sources_counts", offsets.blend_shape_keyform_bindings->blend_shape_constraint_index_sources_counts, info.blend_shape_keyform_bindings},
        {"blend_shapes_warp_deformers.target_indices", offsets.blend_shapes_warp_deformers->target_indices, info.blend_shapes_warp_deformers},
        {"blend_shapes_warp_deformers.blend_shape_keyform_binding_sources_begin_indices", offsets.blend_shapes_warp_deformers->blend_shape_keyform_binding_sources_begin_indices, info.blend_shapes_warp_deformers},
        {"blend_shapes_warp_deformers.blend_shape_keyform_binding_sources_counts", offsets.blend_shapes_warp_deformers->blend_shape_keyform_binding_sources_counts, info.blend_shapes_warp_deformers},
        {"blend_shapes_art_meshes.target_indices", offsets.blend_shapes_art_meshes->target_indices, info.blend_shapes_art_meshes},
        {"blend_shapes_art_meshes.blend_shape_keyform_binding_sources_begin_indices", offsets.blend_shapes_art_meshes->blend_shape_keyform_binding_sources_begin_indices, info.blend_shapes_art_meshes},
        {"blend_shapes_art_meshes.blend_shape_keyform_binding_sources_counts", offsets.blend_shapes_art_meshes->blend_shape_keyform_binding_sources_counts, info.blend_shapes_art_meshes},
        {"blend_shape_constraint_indices.blend_shape_constraint_sources_indices", offsets.blend_shape_constraint_indices->blend_shape_constraint_sources_indices, info.blend_shape_constraint_indices},
        {"blend_shape_constraints.parameter_indices", offsets.blend_shape_constraints->parameter_indices, info.blend_shape_constraints},
        {"blend_shape_constraints.blend_shape_constraint_value_sources_begin_indices", offsets.blend_shape_constraints->blend_shape_constraint_value_sources_begin_indices, info.blend_shape_constraints},
        {"blend_shape_constraints.blend_shape_constraint_value_sources_counts", offsets.blend_shape_constraints->blend_shape_constraint_value_sources_counts, info.blend_shape_constraints},
        {"blend_shape_constraint_values.keys", offsets.blend_shape_constraint_values->keys, info.blend_shape_constraint_values},
        {"blend_shape_constraint_values.weights", offsets.blend_shape_constraint_values->weights, info.blend_shape_constraint_values},
    };
    static_assert(sizeof(columns) / sizeof(columns[0]) == OffsetTableCount - 2);
    const qint64 countOffset = offsets.count_info->count_info_offset;
    const qint64 canvasOffset = offsets.canvas_info->CanvasOffset;
    for (const Column& column : columns) {
        if (column.optional && column.offset == 0) continue;
        const QString label = QString::fromLatin1(column.name);
        if (!reader.RequireSpan(label, column.offset, column.count, column.width)) return false;
        if (column.count == 0) continue;
        if (column.offset < OffsetTableEnd)
            return reader.Fail(QStringLiteral("moc3 %1 与文件头或偏移表重叠").arg(label));
        const qint64 bytes = column.count * column.width;
        if (overlaps(column.offset, bytes, countOffset, CountInfoBytes))
            return reader.Fail(QStringLiteral("moc3 %1 与 count_info 重叠").arg(label));
        if (overlaps(column.offset, bytes, canvasOffset, CanvasInfoBytes))
            return reader.Fail(QStringLiteral("moc3 %1 与 canvas_info 重叠").arg(label));
    }
    return true;
}
}
