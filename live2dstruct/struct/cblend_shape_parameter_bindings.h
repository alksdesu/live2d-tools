#ifndef CBLEND_SHAPE_PARAMETER_BINDINGS_H
#define CBLEND_SHAPE_PARAMETER_BINDINGS_H

#include "live2dstruct/util/cfilereader.h"
namespace live2dstruct {
class CBlend_shape_parameter_bindings
{
public:
    class Offset{
    public:
        int keys_sources_begin_indices;
        int keys_sources_counts;
        int base_key_indices;

        Offset(CReader& bytes) {
            keys_sources_begin_indices =bytes.readInt();
            keys_sources_counts=bytes.readInt();
            base_key_indices=bytes.readInt();

        }
    };


    CBlend_shape_parameter_bindings();
    int keys_sources_begin_indices;
    int keys_sources_counts;
    int base_key_indices;
    CBlend_shape_parameter_bindings(CReader& bytes, const Offset& drawOrderGroups, int i) {
        bytes.seek(drawOrderGroups.keys_sources_begin_indices + i*4);
        keys_sources_begin_indices = bytes.readInt();

        bytes.seek(drawOrderGroups.keys_sources_counts + i*4);
        keys_sources_counts = bytes.readInt();

        bytes.seek(drawOrderGroups.base_key_indices + i*4);
        base_key_indices = bytes.readInt();


    }


};
}
#endif // CBLEND_SHAPE_PARAMETER_BINDINGS_H
