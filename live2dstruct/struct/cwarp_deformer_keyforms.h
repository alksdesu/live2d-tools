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

        int multiply_color_sources_indices = 0;
        int screen_color_sources_indices = 0;

        void ReadColorOffsets(CReader& bytes)
        {
            multiply_color_sources_indices = bytes.readInt();
            screen_color_sources_indices = bytes.readInt();
        }

        Offset(CReader& bytes) {
            opacities = bytes.readInt();
            keyform_position_sources_begin_indices= bytes.readInt();
        }
    };
    int multiply_color_sources_index = -1;
    int screen_color_sources_index = -1;


    float opacities;
    int keyform_position_sources_begin_indices;
    CWarp_deformer_keyforms(CReader& bytes,const Offset& warpDeformerKeyforms, int i) {
        if (warpDeformerKeyforms.multiply_color_sources_indices) {
            bytes.seek(warpDeformerKeyforms.multiply_color_sources_indices + i * 4);
            multiply_color_sources_index = bytes.readInt();
        }
        if (warpDeformerKeyforms.screen_color_sources_indices) {
            bytes.seek(warpDeformerKeyforms.screen_color_sources_indices + i * 4);
            screen_color_sources_index = bytes.readInt();
        }

        bytes.seek(warpDeformerKeyforms.opacities + i*4);
        opacities = bytes.readFloat();

        bytes.seek(warpDeformerKeyforms.keyform_position_sources_begin_indices + i*4);
        keyform_position_sources_begin_indices = bytes.readInt();

    }

};
}
#endif // CWARP_DEFORMER_KEYFORMS_H
