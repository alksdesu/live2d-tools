#ifndef CPARAMETER_BINDINGS_H
#define CPARAMETER_BINDINGS_H
#include "live2dstruct/util/cfilereader.h"
namespace live2dstruct {
class CParameter_bindings
{
public:
    class Offset{
    public:
        int keys_sources_begin_indices;
        int keys_sources_counts;

        Offset(CReader& bytes) {
            keys_sources_begin_indices = bytes.readInt();
            keys_sources_counts= bytes.readInt();
        }
    };
public:

    int keys_sources_begin_indices;
    int keys_sources_counts;
    CParameter_bindings(CReader& bytes,const Offset& parameterBindings, int i) {
        bytes.seek(parameterBindings.keys_sources_begin_indices + i* 4);
        keys_sources_begin_indices = bytes.readInt();

        bytes.seek(parameterBindings.keys_sources_counts + i* 4);
        keys_sources_counts = bytes.readInt();

    }

};
}

#endif // CPARAMETER_BINDINGS_H
