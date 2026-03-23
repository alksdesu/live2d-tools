#ifndef CBLEND_SHAPE_KEYFORM_BINDINGS_H
#define CBLEND_SHAPE_KEYFORM_BINDINGS_H
#include "live2dstruct/util/cfilereader.h"
namespace live2dstruct {
class CBlend_shape_keyform_bindings
{
public:
    class Offset{
    public:
        int parameter_binding_sources_indices;
        int keyform_sources_blend_shape_indices;
        int keyform_sources_blend_shape_counts;
        int blend_shape_constraint_index_sources_begin_indices;
        int blend_shape_constraint_index_sources_counts;

        Offset(CReader& bytes) {
            parameter_binding_sources_indices = bytes.readInt();
            keyform_sources_blend_shape_indices=bytes.readInt();
            keyform_sources_blend_shape_counts=bytes.readInt();
            blend_shape_constraint_index_sources_begin_indices=bytes.readInt();
            blend_shape_constraint_index_sources_counts=bytes.readInt();

        }
    };


    CBlend_shape_keyform_bindings(CReader& bytes,const Offset& drawOrderGroups, int i) {
        bytes.seek(drawOrderGroups.parameter_binding_sources_indices + i*4);
        parameter_binding_sources_indices = bytes.readInt();

        bytes.seek(drawOrderGroups.keyform_sources_blend_shape_indices + i*4);
        keyform_sources_blend_shape_indices = bytes.readInt();

        bytes.seek(drawOrderGroups.keyform_sources_blend_shape_counts + i*4);
        keyform_sources_blend_shape_counts = bytes.readInt();

        bytes.seek(drawOrderGroups.blend_shape_constraint_index_sources_begin_indices + i*4);
        blend_shape_constraint_index_sources_begin_indices = bytes.readInt();

        bytes.seek(drawOrderGroups.blend_shape_constraint_index_sources_counts + i*4);
        blend_shape_constraint_index_sources_counts = bytes.readInt();

    }
    int parameter_binding_sources_indices;
    int keyform_sources_blend_shape_indices;
    int keyform_sources_blend_shape_counts;
    int blend_shape_constraint_index_sources_begin_indices;
    int blend_shape_constraint_index_sources_counts;



};
}
#endif // CBLEND_SHAPE_KEYFORM_BINDINGS_H
