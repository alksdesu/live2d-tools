#ifndef CSECTIONOFFSETTABLE_H
#define CSECTIONOFFSETTABLE_H
#include "live2dstruct/struct/cart_mesh_keyforms.h"
#include "live2dstruct/struct/cart_meshes.h"
#include "live2dstruct/struct/cglue_keyforms.h"
#include "live2dstruct/struct/cglue.h"
#include "live2dstruct/struct/cglue_info.h"
#include "live2dstruct/struct/cglue_keyforms.h"
#include "live2dstruct/struct/crotation_deformer_keyforms.h"
#include "live2dstruct/struct/cwarp_deformers.h"
#include "live2dstruct/struct/ccount_info.h"
#include "live2dstruct/struct/ckeys.h"
#include "live2dstruct/struct/cpart_keyforms.h"
#include "live2dstruct/struct/cwarp_deformer_keyforms.h"
#include "live2dstruct/struct/cpart_keyforms.h"
#include "live2dstruct/struct/ccanvas_info.h"
#include "live2dstruct/struct/cparts.h"
#include "live2dstruct/struct/cdeformers.h"
#include "live2dstruct/struct/crotation_deformers.h"
#include "live2dstruct/struct/cparameters.h"
#include "live2dstruct/struct/ckeyform_positions.h"
#include "live2dstruct/struct/cparameter_binding_indices.h"
#include "live2dstruct/struct/cparameter_bindings.h"
#include "live2dstruct/struct/ckeyform_bindings.h"
#include "live2dstruct/struct/cuvs.h"
#include "live2dstruct/struct/cposition_indices.h"
#include "live2dstruct/struct/cdrawable_masks.h"
#include "live2dstruct/struct/cdraw_order_groups.h"
#include "live2dstruct/struct/cdraw_order_group_objects.h"
#include "live2dstruct/util/cfilereader.h"

#include "live2dstruct/struct/cblend_shape_constraint_indices.h"
#include "live2dstruct/struct/cblend_shape_constraint_values.h"
#include "live2dstruct/struct/cblend_shape_constraints.h"
#include "live2dstruct/struct/cblend_shape_keyform_bindings.h"
#include "live2dstruct/struct/cblend_shapes_art_meshes.h"
#include "live2dstruct/struct/cblend_shapes_warp_deformers.h"
#include "live2dstruct/struct/cblend_shape_targets.h"
#include "live2dstruct/struct/cblend_shape_parameter_bindings.h"
#include "live2dstruct/struct/cblend_shapes_art_meshes.h"
#include "live2dstruct/struct/ckeyform_screen_colors.h"
#include "live2dstruct/struct/ckeyform_multiply_colors.h"


namespace live2dstruct {
struct COffscreensOffset
{
    int drawable_mask_runtime = 0;
    int owner_indices = 0;
    int drawable_flags = 0;
    int blend_modes = 0;
    int mask_sources_begin_indices = 0;
    int mask_sources_counts = 0;

    COffscreensOffset() = default;
    explicit COffscreensOffset(CReader& bytes)
        : drawable_mask_runtime(bytes.readInt()), owner_indices(bytes.readInt()),
          drawable_flags(bytes.readInt()), blend_modes(bytes.readInt()),
          mask_sources_begin_indices(bytes.readInt()), mask_sources_counts(bytes.readInt()) {}
};

struct COffscreenKeyformsOffset
{
    int opacities = 0;
    int multiply_color_sources_indices = 0;
    int screen_color_sources_indices = 0;

    COffscreenKeyformsOffset() = default;
    explicit COffscreenKeyformsOffset(CReader& bytes)
        : opacities(bytes.readInt()), multiply_color_sources_indices(bytes.readInt()),
          screen_color_sources_indices(bytes.readInt()) {}
};

class CSectionOffsetTable
{
public:
    CSectionOffsetTable(CReader& bytes);
    ~CSectionOffsetTable();
    int mocVersion = 0;

    CCount_info::Offset* count_info;
    CCanvas_info::Offset* canvas_info;
    CParts::Offset* parts;
    CDeformers::Offset* deformers;
    CWarp_deformers::Offset* warp_deformers;//变形
    CRotation_deformers::Offset* rotation_deformers;//转动
    CArt_meshes::Offset* art_meshes;
    CParameters::Offset* parameters;
    CPart_keyforms::Offset* part_keyforms;
    CWarp_deformer_keyforms::Offset* warp_deformer_keyforms;
    CRotation_deformer_keyforms::Offset* rotation_deformer_keyforms;
    CArt_mesh_keyforms::Offset* art_mesh_keyforms;
    CKeyform_positions::Offset* keyform_positions;
    CParameter_binding_indices::Offset* parameter_binding_indices;
    CKeyform_bindings::Offset* keyform_bindings;
    CParameter_bindings::Offset* parameter_bindings;
    CKeys::Offset* keys;
    CUvs::Offset* uvs;
    CPosition_indices::Offset* position_indices;
    CDrawable_masks::Offset* drawable_masks;
    CDraw_order_groups::Offset* draw_order_groups;
    CDraw_order_group_objects::Offset* draw_order_group_objects;
    CGlue::Offset* glue;
    CGlue_info::Offset* glue_info;
    CGlue_keyforms::Offset* glue_keyforms;



    CBlend_shape_constraint_indices::Offset* blend_shape_constraint_indices;
    CBlend_shape_constraint_values::Offset* blend_shape_constraint_values;
    CBlend_shape_constraints::Offset* blend_shape_constraints;
    CBlend_shape_keyform_bindings::Offset* blend_shape_keyform_bindings;
    CBlend_shape_parameter_bindings::Offset* blend_shape_parameter_bindings;
    CBlend_shapes_art_meshes::Offset* blend_shapes_art_meshes;
    CBlend_shapes_warp_deformers::Offset* blend_shapes_warp_deformers;
    CBlend_shape_targets::Offset* blend_shapes_parts;
    CBlend_shape_targets::Offset* blend_shapes_rotation_deformers;
    CBlend_shape_targets::Offset* blend_shapes_glues;
    COffscreensOffset* offscreens;
    COffscreenKeyformsOffset* offscreen_keyforms;
    CBlend_shape_targets::Offset* blend_shapes_offscreens;
    CKeyform_multiply_colors::Offset* keyform_multiply_colors;
    CKeyform_screen_colors::Offset* keyform_screen_colors;
};
}
#endif // CSECTIONOFFSETTABLE_H
