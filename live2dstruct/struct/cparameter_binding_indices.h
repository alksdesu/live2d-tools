#ifndef CPARAMETER_BINDING_INDICES_H
#define CPARAMETER_BINDING_INDICES_H
#include "live2dstruct/util/cfilereader.h"
namespace live2dstruct {
class CParameter_binding_indices
{
public:
    class Offset{
    public:
        int binding_sources_indices;

        Offset(CReader& bytes) {
            binding_sources_indices =bytes.readInt();
        }
    };

    int binding_sources_indices;
    CParameter_binding_indices(CReader& bytes,const Offset& parameterBindingIndices, int i) {
        bytes.seek(parameterBindingIndices.binding_sources_indices + i*4);
        binding_sources_indices = bytes.readInt();
    }

};
}
#endif // CPARAMETER_BINDING_INDICES_H
