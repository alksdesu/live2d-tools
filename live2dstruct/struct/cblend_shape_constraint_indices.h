#ifndef CBLEND_SHAPE_CONSTRAINT_INDICES_H
#define CBLEND_SHAPE_CONSTRAINT_INDICES_H
#include "live2dstruct/util/cfilereader.h"
namespace live2dstruct {
class CBlend_shape_constraint_indices
{
public:
    class Offset{
    public:
        int blend_shape_constraint_sources_indices;

        Offset(CReader& bytes) {
            blend_shape_constraint_sources_indices =bytes.readInt();

        }
    };

    CBlend_shape_constraint_indices(CReader& bytes,const Offset& drawOrderGroups, int i) {
        bytes.seek(drawOrderGroups.blend_shape_constraint_sources_indices + i*4);
        blend_shape_constraint_sources_indices = bytes.readInt();
    }


    int blend_shape_constraint_sources_indices;
};
}
#endif // CBLEND_SHAPE_CONSTRAINT_INDICES_H
