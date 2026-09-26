#include "csectionvalidation.h"
#include "csectionoffsettable.h"
#include "live2dstruct/struct/ccount_info.h"
#include "live2dstruct/util/cfilereader.h"
#include <array>
#include <iterator>
#include <cstring>

namespace live2dstruct {
namespace {
constexpr qint64 CanvasInfoBytes = 5 * 4;
constexpr qint64 MaximumCount = 1000000;

struct Count
{
    const char* name;
    qint64 value;
    const void* offsets;
};

std::array<Count, 34> arrayCounts(const CSectionOffsetTable& offsets, const CCount_info& info)
{
    const std::array<Count, 34> counts{{
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
        {"blend_shapes_parts", info.blend_shapes_parts, offsets.blend_shapes_parts},
        {"blend_shapes_rotation_deformers", info.blend_shapes_rotation_deformers, offsets.blend_shapes_rotation_deformers},
    }};
    return counts;
}

bool overlaps(qint64 first, qint64 firstSize, qint64 second, qint64 secondSize)
{
    return first < second + secondSize && second < first + firstSize;
}
}

qint64 CSectionValidation::StorageWidth(const ColumnSpan& span)
{
    return span.name && std::strstr(span.name, "runtime_space") ? 8 : span.width;
}

bool CSectionValidation::ValidateMetadata(CReader& reader, const CSectionOffsetTable& offsets)
{
    if (!reader.IsValid()) return false;
    const qint64 countInfoBytes = CountInfoSize(offsets.mocVersion);
    const int offsetTableEnd = KnownOffsetEnd(offsets.mocVersion);
    if (!reader.RequireSpan(QStringLiteral("offset_table"), OffsetTableStart, KnownOffsetCount(offsets.mocVersion), 4)) return false;
    if (!offsets.count_info) return reader.Fail(QStringLiteral("moc3 缺少区段偏移：count_info"));
    if (!offsets.canvas_info) return reader.Fail(QStringLiteral("moc3 缺少区段偏移：canvas_info"));
    const qint64 countOffset = offsets.count_info->count_info_offset;
    const qint64 canvasOffset = offsets.canvas_info->CanvasOffset;
    if (!reader.RequireSpan(QStringLiteral("count_info"), countOffset, 1, countInfoBytes) ||
        !reader.RequireSpan(QStringLiteral("canvas_info"), canvasOffset, 1, CanvasInfoBytes)) return false;
    if (countOffset < offsetTableEnd)
        return reader.Fail(QStringLiteral("moc3 count_info 与文件头或偏移表重叠"));
    if (canvasOffset < offsetTableEnd)
        return reader.Fail(QStringLiteral("moc3 canvas_info 与文件头或偏移表重叠"));
    if (overlaps(countOffset, countInfoBytes, canvasOffset, CanvasInfoBytes))
        return reader.Fail(QStringLiteral("moc3 count_info 与 canvas_info 重叠"));
    return true;
}

QVector<CSectionValidation::ColumnSpan> CSectionValidation::ArraySpans(
    const CSectionOffsetTable& offsets, const CCount_info& info)
{
    if (!offsets.count_info || !offsets.canvas_info) return {};
    for (const Count& count : arrayCounts(offsets, info))
        if (!count.offsets) return {};
    const ColumnSpan columns[]{
        {"parts.runtime_space_0", offsets.parts->runtime_space_0, info.parts, 4, true, 0x48},
        {"parts.ids", offsets.parts->ids, info.parts, 64, false, 0x4c},
        {"parts.keyform_binding_sources_indices", offsets.parts->keyform_binding_sources_indices, info.parts, 4, false, 0x50},
        {"parts.keyform_sources_begin_indices", offsets.parts->keyform_sources_begin_indices, info.parts, 4, false, 0x54},
        {"parts.keyform_sources_counts", offsets.parts->keyform_sources_counts, info.parts, 4, false, 0x58},
        {"parts.is_visible", offsets.parts->is_visible, info.parts, 4, false, 0x5c},
        {"parts.is_enabled", offsets.parts->is_enabled, info.parts, 4, false, 0x60},
        {"parts.parent_part_indices", offsets.parts->parent_part_indices, info.parts, 4, false, 0x64},
        {"deformers.runtime_space_0", offsets.deformers->runtime_space_0, info.deformers, 4, false, 0x68},
        {"deformers.ids", offsets.deformers->ids, info.deformers, 64, false, 0x6c},
        {"deformers.keyform_binding_sources_indices", offsets.deformers->keyform_binding_sources_indices, info.deformers, 4, false, 0x70},
        {"deformers.is_visible", offsets.deformers->is_visible, info.deformers, 4, false, 0x74},
        {"deformers.is_enabled", offsets.deformers->is_enabled, info.deformers, 4, false, 0x78},
        {"deformers.parent_part_indices", offsets.deformers->parent_part_indices, info.deformers, 4, false, 0x7c},
        {"deformers.parent_deformer_indices", offsets.deformers->parent_deformer_indices, info.deformers, 4, false, 0x80},
        {"deformers.types", offsets.deformers->types, info.deformers, 4, false, 0x84},
        {"deformers.specific_sources_indices", offsets.deformers->specific_sources_indices, info.deformers, 4, false, 0x88},
        {"warp_deformers.keyform_binding_sources_indices", offsets.warp_deformers->keyform_binding_sources_indices, info.warp_deformers, 4, false, 0x8c},
        {"warp_deformers.keyform_sources_begin_indices", offsets.warp_deformers->keyform_sources_begin_indices, info.warp_deformers, 4, false, 0x90},
        {"warp_deformers.keyform_sources_counts", offsets.warp_deformers->keyform_sources_counts, info.warp_deformers, 4, false, 0x94},
        {"warp_deformers.vertex_counts", offsets.warp_deformers->vertex_counts, info.warp_deformers, 4, false, 0x98},
        {"warp_deformers.rows", offsets.warp_deformers->rows, info.warp_deformers, 4, false, 0x9c},
        {"warp_deformers.columns", offsets.warp_deformers->columns, info.warp_deformers, 4, false, 0xa0},
        {"warp_deformers.is_quad_source", offsets.warp_deformers->is_quad_source, info.warp_deformers, 4, true, 0x1d4},
        {"warp_deformers.keyform_color_sources_begin_indices", offsets.warp_deformers->keyform_color_sources_begin_indices, info.warp_deformers, 4, true, 0x1e4},
        {"rotation_deformers.keyform_binding_sources_indices", offsets.rotation_deformers->keyform_binding_sources_indices, info.rotation_deformers, 4, false, 0xa4},
        {"rotation_deformers.keyform_sources_begin_indices", offsets.rotation_deformers->keyform_sources_begin_indices, info.rotation_deformers, 4, false, 0xa8},
        {"rotation_deformers.keyform_sources_counts", offsets.rotation_deformers->keyform_sources_counts, info.rotation_deformers, 4, false, 0xac},
        {"rotation_deformers.base_angles", offsets.rotation_deformers->base_angles, info.rotation_deformers, 4, false, 0xb0},
        {"rotation_deformers.keyform_color_sources_begin_indices", offsets.rotation_deformers->keyform_color_sources_begin_indices, info.rotation_deformers, 4, true, 0x1e8},
        {"art_meshes.runtime_space_0", offsets.art_meshes->runtime_space_0, info.art_meshes, 4, false, 0xb4},
        {"art_meshes.runtime_space_1", offsets.art_meshes->runtime_space_1, info.art_meshes, 4, false, 0xb8},
        {"art_meshes.runtime_space_2", offsets.art_meshes->runtime_space_2, info.art_meshes, 4, false, 0xbc},
        {"art_meshes.runtime_space_3", offsets.art_meshes->runtime_space_3, info.art_meshes, 4, false, 0xc0},
        {"art_meshes.ids", offsets.art_meshes->ids, info.art_meshes, 64, false, 0xc4},
        {"art_meshes.keyform_binding_sources_indices", offsets.art_meshes->keyform_binding_sources_indices, info.art_meshes, 4, false, 0xc8},
        {"art_meshes.keyform_sources_begin_indices", offsets.art_meshes->keyform_sources_begin_indices, info.art_meshes, 4, false, 0xcc},
        {"art_meshes.keyform_sources_counts", offsets.art_meshes->keyform_sources_counts, info.art_meshes, 4, false, 0xd0},
        {"art_meshes.is_visible", offsets.art_meshes->is_visible, info.art_meshes, 4, false, 0xd4},
        {"art_meshes.is_enabled", offsets.art_meshes->is_enabled, info.art_meshes, 4, false, 0xd8},
        {"art_meshes.parent_part_indices", offsets.art_meshes->parent_part_indices, info.art_meshes, 4, false, 0xdc},
        {"art_meshes.parent_deformer_indices", offsets.art_meshes->parent_deformer_indices, info.art_meshes, 4, false, 0xe0},
        {"art_meshes.texture_nos", offsets.art_meshes->texture_nos, info.art_meshes, 4, false, 0xe4},
        {"art_meshes.drawable_flags", offsets.art_meshes->drawable_flags, info.art_meshes, 1, false, 0xe8},
        {"art_meshes.vertex_counts", offsets.art_meshes->vertex_counts, info.art_meshes, 4, false, 0xec},
        {"art_meshes.uv_sources_begin_indices", offsets.art_meshes->uv_sources_begin_indices, info.art_meshes, 4, false, 0xf0},
        {"art_meshes.position_index_sources_begin_indices", offsets.art_meshes->position_index_sources_begin_indices, info.art_meshes, 4, false, 0xf4},
        {"art_meshes.position_index_sources_counts", offsets.art_meshes->position_index_sources_counts, info.art_meshes, 4, false, 0xf8},
        {"art_meshes.drawable_mask_sources_begin_indices", offsets.art_meshes->drawable_mask_sources_begin_indices, info.art_meshes, 4, false, 0xfc},
        {"art_meshes.drawable_mask_sources_counts", offsets.art_meshes->drawable_mask_sources_counts, info.art_meshes, 4, false, 0x100},
        {"art_meshes.keyform_color_sources_begin_indices", offsets.art_meshes->keyform_color_sources_begin_indices, info.art_meshes, 4, true, 0x1ec},
        {"parameters.runtime_space_0", offsets.parameters->runtime_space_0, info.parameters, 4, false, 0x104},
        {"parameters.ids", offsets.parameters->ids, info.parameters, 64, false, 0x108},
        {"parameters.max_values", offsets.parameters->max_values, info.parameters, 4, false, 0x10c},
        {"parameters.min_values", offsets.parameters->min_values, info.parameters, 4, false, 0x110},
        {"parameters.default_values", offsets.parameters->default_values, info.parameters, 4, false, 0x114},
        {"parameters.is_repeat", offsets.parameters->is_repeat, info.parameters, 4, false, 0x118},
        {"parameters.decimal_places", offsets.parameters->decimal_places, info.parameters, 4, false, 0x11c},
        {"parameters.parameter_binding_sources_begin_indices", offsets.parameters->parameter_binding_sources_begin_indices, info.parameters, 4, false, 0x120},
        {"parameters.parameter_binding_sources_counts", offsets.parameters->parameter_binding_sources_counts, info.parameters, 4, false, 0x124},
        {"parameters.extension_runtime_space_0", offsets.parameters->extension_runtime_space_0, info.parameters, 4, true, 0x1d8},
        {"parameters.keys_sources_begin_indices", offsets.parameters->keys_sources_begin_indices, info.parameters, 4, true, 0x1dc},
        {"parameters.keys_sources_counts", offsets.parameters->keys_sources_counts, info.parameters, 4, true, 0x1e0},
        {"parameters.parameter_types", offsets.parameters->parameter_types, info.parameters, 4, true, 0x208},
        {"parameters.blend_shape_parameter_binding_sources_begin_indices", offsets.parameters->blend_shape_parameter_binding_sources_begin_indices, info.parameters, 4, true, 0x20c},
        {"parameters.blend_shape_parameter_binding_sources_counts", offsets.parameters->blend_shape_parameter_binding_sources_counts, info.parameters, 4, true, 0x210},
        {"part_keyforms.draw_orders", offsets.part_keyforms->draw_orders, info.part_keyforms, 4, false, 0x128},
        {"warp_deformer_keyforms.opacities", offsets.warp_deformer_keyforms->opacities, info.warp_deformer_keyforms, 4, false, 0x12c},
        {"warp_deformer_keyforms.keyform_position_sources_begin_indices", offsets.warp_deformer_keyforms->keyform_position_sources_begin_indices, info.warp_deformer_keyforms, 4, false, 0x130},
        {"rotation_deformer_keyforms.opacities", offsets.rotation_deformer_keyforms->opacities, info.rotation_deformer_keyforms, 4, false, 0x134},
        {"rotation_deformer_keyforms.angles", offsets.rotation_deformer_keyforms->angles, info.rotation_deformer_keyforms, 4, false, 0x138},
        {"rotation_deformer_keyforms.origin_x", offsets.rotation_deformer_keyforms->origin_x, info.rotation_deformer_keyforms, 4, false, 0x13c},
        {"rotation_deformer_keyforms.origin_y", offsets.rotation_deformer_keyforms->origin_y, info.rotation_deformer_keyforms, 4, false, 0x140},
        {"rotation_deformer_keyforms.scales", offsets.rotation_deformer_keyforms->scales, info.rotation_deformer_keyforms, 4, false, 0x144},
        {"rotation_deformer_keyforms.is_reflect_x", offsets.rotation_deformer_keyforms->is_reflect_x, info.rotation_deformer_keyforms, 4, false, 0x148},
        {"rotation_deformer_keyforms.is_reflect_y", offsets.rotation_deformer_keyforms->is_reflect_y, info.rotation_deformer_keyforms, 4, false, 0x14c},
        {"art_mesh_keyforms.opacities", offsets.art_mesh_keyforms->opacities, info.art_mesh_keyforms, 4, false, 0x150},
        {"art_mesh_keyforms.draw_orders", offsets.art_mesh_keyforms->draw_orders, info.art_mesh_keyforms, 4, false, 0x154},
        {"art_mesh_keyforms.keyform_position_sources_begin_indices", offsets.art_mesh_keyforms->keyform_position_sources_begin_indices, info.art_mesh_keyforms, 4, false, 0x158},
        {"keyform_positions.xys", offsets.keyform_positions->xys, info.keyform_positions, 4, false, 0x15c},
        {"parameter_binding_indices.binding_sources_indices", offsets.parameter_binding_indices->binding_sources_indices, info.parameter_binding_indices, 4, false, 0x160},
        {"keyform_bindings.parameter_binding_index_sources_begin_indices", offsets.keyform_bindings->parameter_binding_index_sources_begin_indices, info.keyform_bindings, 4, false, 0x164},
        {"keyform_bindings.parameter_binding_index_sources_counts", offsets.keyform_bindings->parameter_binding_index_sources_counts, info.keyform_bindings, 4, false, 0x168},
        {"parameter_bindings.keys_sources_begin_indices", offsets.parameter_bindings->keys_sources_begin_indices, info.parameter_bindings, 4, false, 0x16c},
        {"parameter_bindings.keys_sources_counts", offsets.parameter_bindings->keys_sources_counts, info.parameter_bindings, 4, false, 0x170},
        {"keys.values", offsets.keys->values, info.keys, 4, false, 0x174},
        {"uvs.uvs", offsets.uvs->uvs, info.uvs, 4, false, 0x178},
        {"position_indices.indices", offsets.position_indices->indices, info.position_indices, 2, false, 0x17c},
        {"drawable_masks.art_mesh_sources_indices", offsets.drawable_masks->art_mesh_sources_indices, info.drawable_masks, 4, false, 0x180},
        {"draw_order_groups.object_sources_begin_indices", offsets.draw_order_groups->object_sources_begin_indices, info.draw_order_groups, 4, false, 0x184},
        {"draw_order_groups.object_sources_counts", offsets.draw_order_groups->object_sources_counts, info.draw_order_groups, 4, false, 0x188},
        {"draw_order_groups.object_sources_total_counts", offsets.draw_order_groups->object_sources_total_counts, info.draw_order_groups, 4, false, 0x18c},
        {"draw_order_groups.maximum_draw_orders", offsets.draw_order_groups->maximum_draw_orders, info.draw_order_groups, 4, false, 0x190},
        {"draw_order_groups.minimum_draw_orders", offsets.draw_order_groups->minimum_draw_orders, info.draw_order_groups, 4, false, 0x194},
        {"draw_order_group_objects.types", offsets.draw_order_group_objects->types, info.draw_order_group_objects, 4, false, 0x198},
        {"draw_order_group_objects.indices", offsets.draw_order_group_objects->indices, info.draw_order_group_objects, 4, false, 0x19c},
        {"draw_order_group_objects.self_indices", offsets.draw_order_group_objects->self_indices, info.draw_order_group_objects, 4, false, 0x1a0},
        {"glue.runtime_space", offsets.glue->runtime_space, info.glue, 4, false, 0x1a4},
        {"glue.ids", offsets.glue->ids, info.glue, 64, false, 0x1a8},
        {"glue.keyform_binding_sources_indices", offsets.glue->keyform_binding_sources_indices, info.glue, 4, false, 0x1ac},
        {"glue.keyform_sources_begin_indices", offsets.glue->keyform_sources_begin_indices, info.glue, 4, false, 0x1b0},
        {"glue.keyform_sources_counts", offsets.glue->keyform_sources_counts, info.glue, 4, false, 0x1b4},
        {"glue.art_mesh_indices_a", offsets.glue->art_mesh_indices_a, info.glue, 4, false, 0x1b8},
        {"glue.art_mesh_indices_b", offsets.glue->art_mesh_indices_b, info.glue, 4, false, 0x1bc},
        {"glue.glue_info_sources_begin_indices", offsets.glue->glue_info_sources_begin_indices, info.glue, 4, false, 0x1c0},
        {"glue.glue_info_sources_counts", offsets.glue->glue_info_sources_counts, info.glue, 4, false, 0x1c4},
        {"glue_info.weights", offsets.glue_info->weights, info.glue_info, 4, false, 0x1c8},
        {"glue_info.position_indices", offsets.glue_info->position_indices, info.glue_info, 2, false, 0x1cc},
        {"glue_keyforms.intensities", offsets.glue_keyforms->intensities, info.glue_keyforms, 4, false, 0x1d0},
        {"keyform_multiply_colors.r", offsets.keyform_multiply_colors->r, info.keyform_multiply_colors, 4, false, 0x1f0},
        {"keyform_multiply_colors.g", offsets.keyform_multiply_colors->g, info.keyform_multiply_colors, 4, false, 0x1f4},
        {"keyform_multiply_colors.b", offsets.keyform_multiply_colors->b, info.keyform_multiply_colors, 4, false, 0x1f8},
        {"keyform_screen_colors.r", offsets.keyform_screen_colors->r, info.keyform_screen_colors, 4, false, 0x1fc},
        {"keyform_screen_colors.g", offsets.keyform_screen_colors->g, info.keyform_screen_colors, 4, false, 0x200},
        {"keyform_screen_colors.b", offsets.keyform_screen_colors->b, info.keyform_screen_colors, 4, false, 0x204},
        {"blend_shape_parameter_bindings.keys_sources_begin_indices", offsets.blend_shape_parameter_bindings->keys_sources_begin_indices, info.blend_shape_parameter_bindings, 4, false, 0x214},
        {"blend_shape_parameter_bindings.keys_sources_counts", offsets.blend_shape_parameter_bindings->keys_sources_counts, info.blend_shape_parameter_bindings, 4, false, 0x218},
        {"blend_shape_parameter_bindings.base_key_indices", offsets.blend_shape_parameter_bindings->base_key_indices, info.blend_shape_parameter_bindings, 4, false, 0x21c},
        {"blend_shape_keyform_bindings.parameter_binding_sources_indices", offsets.blend_shape_keyform_bindings->parameter_binding_sources_indices, info.blend_shape_keyform_bindings, 4, false, 0x220},
        {"blend_shape_keyform_bindings.keyform_sources_blend_shape_indices", offsets.blend_shape_keyform_bindings->keyform_sources_blend_shape_indices, info.blend_shape_keyform_bindings, 4, false, 0x224},
        {"blend_shape_keyform_bindings.keyform_sources_blend_shape_counts", offsets.blend_shape_keyform_bindings->keyform_sources_blend_shape_counts, info.blend_shape_keyform_bindings, 4, false, 0x228},
        {"blend_shape_keyform_bindings.blend_shape_constraint_index_sources_begin_indices", offsets.blend_shape_keyform_bindings->blend_shape_constraint_index_sources_begin_indices, info.blend_shape_keyform_bindings, 4, false, 0x22c},
        {"blend_shape_keyform_bindings.blend_shape_constraint_index_sources_counts", offsets.blend_shape_keyform_bindings->blend_shape_constraint_index_sources_counts, info.blend_shape_keyform_bindings, 4, false, 0x230},
        {"blend_shapes_warp_deformers.target_indices", offsets.blend_shapes_warp_deformers->target_indices, info.blend_shapes_warp_deformers, 4, false, 0x234},
        {"blend_shapes_warp_deformers.blend_shape_keyform_binding_sources_begin_indices", offsets.blend_shapes_warp_deformers->blend_shape_keyform_binding_sources_begin_indices, info.blend_shapes_warp_deformers, 4, false, 0x238},
        {"blend_shapes_warp_deformers.blend_shape_keyform_binding_sources_counts", offsets.blend_shapes_warp_deformers->blend_shape_keyform_binding_sources_counts, info.blend_shapes_warp_deformers, 4, false, 0x23c},
        {"blend_shapes_art_meshes.target_indices", offsets.blend_shapes_art_meshes->target_indices, info.blend_shapes_art_meshes, 4, false, 0x240},
        {"blend_shapes_art_meshes.blend_shape_keyform_binding_sources_begin_indices", offsets.blend_shapes_art_meshes->blend_shape_keyform_binding_sources_begin_indices, info.blend_shapes_art_meshes, 4, false, 0x244},
        {"blend_shapes_art_meshes.blend_shape_keyform_binding_sources_counts", offsets.blend_shapes_art_meshes->blend_shape_keyform_binding_sources_counts, info.blend_shapes_art_meshes, 4, false, 0x248},
        {"blend_shape_constraint_indices.blend_shape_constraint_sources_indices", offsets.blend_shape_constraint_indices->blend_shape_constraint_sources_indices, info.blend_shape_constraint_indices, 4, false, 0x24c},
        {"blend_shape_constraints.parameter_indices", offsets.blend_shape_constraints->parameter_indices, info.blend_shape_constraints, 4, false, 0x250},
        {"blend_shape_constraints.blend_shape_constraint_value_sources_begin_indices", offsets.blend_shape_constraints->blend_shape_constraint_value_sources_begin_indices, info.blend_shape_constraints, 4, false, 0x254},
        {"blend_shape_constraints.blend_shape_constraint_value_sources_counts", offsets.blend_shape_constraints->blend_shape_constraint_value_sources_counts, info.blend_shape_constraints, 4, false, 0x258},
        {"blend_shape_constraint_values.keys", offsets.blend_shape_constraint_values->keys, info.blend_shape_constraint_values, 4, false, 0x25c},
        {"blend_shape_constraint_values.weights", offsets.blend_shape_constraint_values->weights, info.blend_shape_constraint_values, 4, false, 0x260},
    };
    static_assert(sizeof(columns) / sizeof(columns[0]) == OffsetTableCount - 2);
    QVector<ColumnSpan> result(std::begin(columns), std::end(columns));
    if (offsets.mocVersion >= 5) {
        result.append({"warp_deformer_keyforms.multiply_color_sources_indices", offsets.warp_deformer_keyforms->multiply_color_sources_indices, info.warp_deformer_keyforms, 4, true, 0x264});
        result.append({"warp_deformer_keyforms.screen_color_sources_indices", offsets.warp_deformer_keyforms->screen_color_sources_indices, info.warp_deformer_keyforms, 4, true, 0x268});
        result.append({"rotation_deformer_keyforms.multiply_color_sources_indices", offsets.rotation_deformer_keyforms->multiply_color_sources_indices, info.rotation_deformer_keyforms, 4, true, 0x26c});
        result.append({"rotation_deformer_keyforms.screen_color_sources_indices", offsets.rotation_deformer_keyforms->screen_color_sources_indices, info.rotation_deformer_keyforms, 4, true, 0x270});
        result.append({"art_mesh_keyforms.multiply_color_sources_indices", offsets.art_mesh_keyforms->multiply_color_sources_indices, info.art_mesh_keyforms, 4, true, 0x274});
        result.append({"art_mesh_keyforms.screen_color_sources_indices", offsets.art_mesh_keyforms->screen_color_sources_indices, info.art_mesh_keyforms, 4, true, 0x278});
        result.append({"blend_shapes_parts.target_indices", offsets.blend_shapes_parts->target_indices, info.blend_shapes_parts, 4, false, 0x27c});
        result.append({"blend_shapes_parts.blend_shape_keyform_binding_sources_begin_indices", offsets.blend_shapes_parts->blend_shape_keyform_binding_sources_begin_indices, info.blend_shapes_parts, 4, false, 0x280});
        result.append({"blend_shapes_parts.blend_shape_keyform_binding_sources_counts", offsets.blend_shapes_parts->blend_shape_keyform_binding_sources_counts, info.blend_shapes_parts, 4, false, 0x284});
        result.append({"blend_shapes_rotation_deformers.target_indices", offsets.blend_shapes_rotation_deformers->target_indices, info.blend_shapes_rotation_deformers, 4, false, 0x288});
        result.append({"blend_shapes_rotation_deformers.blend_shape_keyform_binding_sources_begin_indices", offsets.blend_shapes_rotation_deformers->blend_shape_keyform_binding_sources_begin_indices, info.blend_shapes_rotation_deformers, 4, false, 0x28c});
        result.append({"blend_shapes_rotation_deformers.blend_shape_keyform_binding_sources_counts", offsets.blend_shapes_rotation_deformers->blend_shape_keyform_binding_sources_counts, info.blend_shapes_rotation_deformers, 4, false, 0x290});
    }
    const auto counts = arrayCounts(offsets, info);
    for (auto& column : result) {
        const auto length = std::strchr(column.name, '.') - column.name;
        for (size_t index = 0; index < counts.size(); ++index)
            if (std::strlen(counts[index].name) == size_t(length) && std::strncmp(column.name, counts[index].name, size_t(length)) == 0) {
                column.countIndex = int(index);
                break;
            }
    }
    return result;
}

bool CSectionValidation::ValidateArrays(CReader& reader, const CSectionOffsetTable& offsets, const CCount_info& info)
{
    if (!ValidateMetadata(reader, offsets)) return false;
    const auto counts = arrayCounts(offsets, info);
    for (const Count& count : counts) {
        const QString label = QString::fromLatin1(count.name);
        if (count.value < 0 || count.value > MaximumCount)
            return reader.Fail(QStringLiteral("moc3 count_info.%1 数量越界：%2，允许 0..%3")
                                   .arg(label).arg(count.value).arg(MaximumCount));
        if (!count.offsets)
            return reader.Fail(QStringLiteral("moc3 缺少区段偏移：%1").arg(label));
    }
    const auto columns = ArraySpans(offsets, info);
    const qint64 countOffset = offsets.count_info->count_info_offset;
    const qint64 canvasOffset = offsets.canvas_info->CanvasOffset;
    const qint64 countInfoBytes = CountInfoSize(offsets.mocVersion);
    for (const ColumnSpan& column : columns) {
        if (column.optional && column.offset == 0) continue;
        const QString label = QString::fromLatin1(column.name);
        if (!reader.RequireSpan(label, column.offset, column.count, column.width)) return false;
        if (column.count == 0) continue;
        if (column.offset < KnownOffsetEnd(offsets.mocVersion))
            return reader.Fail(QStringLiteral("moc3 %1 与文件头或偏移表重叠").arg(label));
        const qint64 bytes = column.count * column.width;
        if (overlaps(column.offset, bytes, countOffset, countInfoBytes))
            return reader.Fail(QStringLiteral("moc3 %1 与 count_info 重叠").arg(label));
        if (overlaps(column.offset, bytes, canvasOffset, CanvasInfoBytes))
            return reader.Fail(QStringLiteral("moc3 %1 与 canvas_info 重叠").arg(label));
    }
    return true;
}
}
