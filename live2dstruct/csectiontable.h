#ifndef CSECTIONTABLE_H
#define CSECTIONTABLE_H

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


#include "live2dstruct/struct/cblend_shape_constraint_indices.h"
#include "live2dstruct/struct/cblend_shape_constraint_values.h"
#include "live2dstruct/struct/cblend_shape_constraints.h"
#include "live2dstruct/struct/cblend_shape_keyform_bindings.h"
#include "live2dstruct/struct/cblend_shapes_art_meshes.h"
#include "live2dstruct/struct/cblend_shapes_warp_deformers.h"
#include "live2dstruct/struct/cblend_shape_parameter_bindings.h"
#include "live2dstruct/struct/cblend_shapes_art_meshes.h"
#include "live2dstruct/struct/ckeyform_screen_colors.h"
#include "live2dstruct/struct/ckeyform_multiply_colors.h"
#include <QVector>
#include "csectionoffsettable.h"
#include "live2dstruct/util/cfilereader.h"
#include "live2dstruct/struct/cblend_shape_constraint_indices.h"



namespace live2dstruct {
class CSectionTable
{
public:

    CSectionTable(CReader& bytes);
    ~CSectionTable();
    bool IsValid() const { return m_valid; }

    CSectionOffsetTable* sectionOffsetTable = nullptr;
    CCount_info* count_info = nullptr;
    CCanvas_info* canvas_info = nullptr;
    QVector<CParts*> parts;
    QVector<CDeformers*> deformers;
    QVector<CWarp_deformers*> warp_deformers;//变形
    QVector<CRotation_deformers*> rotation_deformers;//转动
    QVector<CArt_meshes*> art_meshes;
    QVector<CParameters*> parameters;
    QVector<CPart_keyforms*> part_keyforms;
    QVector<CWarp_deformer_keyforms*> warp_deformer_keyforms;
    QVector<CRotation_deformer_keyforms*> rotation_deformer_keyforms;
    QVector<CArt_mesh_keyforms*> art_mesh_keyforms;
    CKeyform_positions* keyform_positions = nullptr;
    QVector<CParameter_binding_indices*> parameter_binding_indices;
    QVector<CKeyform_bindings*> keyform_bindings;
    QVector<CParameter_bindings*> parameter_bindings;
    CKeys* keys = nullptr;
    CUvs* uvs = nullptr;
    CPosition_indices* position_indices = nullptr;
    QVector<CDrawable_masks*> drawable_masks;
    QVector<CDraw_order_groups*> draw_order_groups;
    QVector<CDraw_order_group_objects*> draw_order_group_objects;
    QVector<CGlue*> glue;
    QVector<CGlue_info*> glue_info;
    QVector<CGlue_keyforms*> glue_keyforms;


    //采用偏移的方式
    QVector<CBlend_shape_constraint_indices*> blend_shape_constraint_indices;
    QVector<CBlend_shape_constraint_values*> blend_shape_constraint_values;
    QVector<CBlend_shape_constraints*> blend_shape_constraints;
    QVector<CBlend_shape_keyform_bindings*> blend_shape_keyform_bindings;
    QVector<CBlend_shape_parameter_bindings*> blend_shape_parameter_bindings;
    QVector<CBlend_shapes_art_meshes*> blend_shapes_art_meshes;
    QVector<CBlend_shapes_warp_deformers*> blend_shapes_warp_deformers;

    QVector<CKeyform_multiply_colors*> keyform_multiply_colors;
    QVector<CKeyform_screen_colors*> keyform_screen_colors;

private:
    bool m_valid = false;


};
}

#endif // CSECTIONTABLE_H
