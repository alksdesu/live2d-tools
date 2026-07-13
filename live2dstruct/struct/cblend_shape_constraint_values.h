#ifndef CBLEND_SHAPE_CONSTRAINT_VALUES_H
#define CBLEND_SHAPE_CONSTRAINT_VALUES_H
#include "live2dstruct/util/cfilereader.h"
namespace live2dstruct {
class CBlend_shape_constraint_values
{
public:
    class Offset{
    public:
        int keys;
        int weights;
        Offset(CReader& bytes) {
            keys = bytes.readInt();
            weights = bytes.readInt();
        }
    };


    CBlend_shape_constraint_values(CReader& bytes,const Offset& drawOrderGroups, int i) {
        bytes.seek(drawOrderGroups.keys + i*4);
        keys = bytes.readFloat();

        bytes.seek(drawOrderGroups.weights + i*4);
        weights = bytes.readFloat();
    }

    float keys;
    float weights;

};
}

#endif // CBLEND_SHAPE_CONSTRAINT_VALUES_H
