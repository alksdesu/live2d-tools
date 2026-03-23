#ifndef CBLEND_SHAPE_CONSTRAINTS_H
#define CBLEND_SHAPE_CONSTRAINTS_H
#include "live2dstruct/util/cfilereader.h"
namespace live2dstruct {
class CBlend_shape_constraints
{
public:
    class Offset{
    public:
        int parameter_indices;
        int blend_shape_constraint_value_sources_begin_indices;
        int blend_shape_constraint_value_sources_counts;

        Offset(CReader bytes) {
            parameter_indices = bytes.readInt();
            blend_shape_constraint_value_sources_begin_indices=bytes.readInt();
            blend_shape_constraint_value_sources_counts=bytes.readInt();

        }
    };


    int parameter_indices;
    int blend_shape_constraint_value_sources_begin_indices;
    int blend_shape_constraint_value_sources_counts;
    CBlend_shape_constraints(CReader& bytes, const Offset& drawOrderGroups, int i) {
        bytes.seek(drawOrderGroups.parameter_indices + i*4);
        parameter_indices = bytes.readInt();

        bytes.seek(drawOrderGroups.blend_shape_constraint_value_sources_begin_indices + i*4);
        blend_shape_constraint_value_sources_begin_indices = bytes.readInt();

        bytes.seek(drawOrderGroups.blend_shape_constraint_value_sources_counts + i*4);
        blend_shape_constraint_value_sources_counts = bytes.readInt();


    }




};
}
#endif // CBLEND_SHAPE_CONSTRAINTS_H
