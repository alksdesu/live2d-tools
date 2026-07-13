#ifndef CWARP_DEFORMER_KEYFORMS_H
#define CWARP_DEFORMER_KEYFORMS_H
#include "live2dstruct/util/cfilereader.h"
namespace live2dstruct {
class CWarp_deformer_keyforms
{
public:
    class Offset{
    public:
        int  opacities;
        int  keyform_position_sources_begin_indices;

        Offset(CReader& bytes) {
            opacities = bytes.readInt();
            keyform_position_sources_begin_indices= bytes.readInt();
        }
    };

    float opacities;
    int keyform_position_sources_begin_indices;
    CWarp_deformer_keyforms(CReader& bytes,const Offset& warpDeformerKeyforms, int i) {
        bytes.seek(warpDeformerKeyforms.opacities + i*4);
        opacities = bytes.readFloat();

        bytes.seek(warpDeformerKeyforms.keyform_position_sources_begin_indices + i*4);
        keyform_position_sources_begin_indices = bytes.readInt();

    }

};
}
#endif // CWARP_DEFORMER_KEYFORMS_H
