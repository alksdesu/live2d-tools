#ifndef CKEYFORM_BINDINGS_H
#define CKEYFORM_BINDINGS_H
#include "live2dstruct/util/cfilereader.h"
namespace live2dstruct {
class CKeyform_bindings
{
public:
    class Offset{
    public:
        int parameter_binding_index_sources_begin_indices;
        int parameter_binding_index_sources_counts;
        Offset(CReader& bytes) {
            parameter_binding_index_sources_begin_indices = bytes.readInt();
            parameter_binding_index_sources_counts= bytes.readInt();
        }
    };

    int parameter_binding_index_sources_begin_indices;//关联参数索引
    int parameter_binding_index_sources_counts;//关联参数数量
    CKeyform_bindings(CReader& bytes,const Offset& keyformBindings, int i) {
        bytes.seek(keyformBindings.parameter_binding_index_sources_begin_indices + i*4);
        parameter_binding_index_sources_begin_indices = bytes.readInt();

        bytes.seek(keyformBindings.parameter_binding_index_sources_counts + i*4);
        parameter_binding_index_sources_counts = bytes.readInt();


    }


};
}
#endif // CKEYFORM_BINDINGS_H
