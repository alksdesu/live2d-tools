#ifndef CROTATION_DEFORMER_KEYFORMS_H
#define CROTATION_DEFORMER_KEYFORMS_H

#include "live2dstruct/util/cfilereader.h"
namespace live2dstruct {
class CRotation_deformer_keyforms
{
public:
    class Offset{
    public:
        int opacities;
        int angles;
        int origin_x;
        int origin_y;
        int scales;
        int is_reflect_x;
        int is_reflect_y;

        int multiply_color_sources_indices = 0;
        int screen_color_sources_indices = 0;

        void ReadColorOffsets(CReader& bytes)
        {
            multiply_color_sources_indices = bytes.readInt();
            screen_color_sources_indices = bytes.readInt();
        }

        Offset(CReader& bytes) {
            opacities = bytes.readInt();
            angles= bytes.readInt();
            origin_x= bytes.readInt();
            origin_y= bytes.readInt();
            scales= bytes.readInt();
            is_reflect_x= bytes.readInt();
            is_reflect_y= bytes.readInt();
        }
    };
    int multiply_color_sources_index = -1;
    int screen_color_sources_index = -1;

    float opacities;
    float angles;
    float origin_x;
    float origin_y;
    float  scales;
    bool is_reflect_x;
    bool is_reflect_y;

    CRotation_deformer_keyforms(CReader& bytes,const Offset& rotationDeformerKeyforms, int i) {
        if (rotationDeformerKeyforms.multiply_color_sources_indices) {
            bytes.seek(rotationDeformerKeyforms.multiply_color_sources_indices + i * 4);
            multiply_color_sources_index = bytes.readInt();
        }
        if (rotationDeformerKeyforms.screen_color_sources_indices) {
            bytes.seek(rotationDeformerKeyforms.screen_color_sources_indices + i * 4);
            screen_color_sources_index = bytes.readInt();
        }

        bytes.seek(rotationDeformerKeyforms.opacities + i*4);
        opacities = bytes.readFloat();

        bytes.seek(rotationDeformerKeyforms.angles + i*4);
        angles = bytes.readFloat();

        bytes.seek(rotationDeformerKeyforms.origin_x + i*4);
        origin_x = bytes.readFloat();

        bytes.seek(rotationDeformerKeyforms.origin_y + i*4);
        origin_y = bytes.readFloat();

        bytes.seek(rotationDeformerKeyforms.scales + i*4);
        scales = bytes.readFloat();

        bytes.seek(rotationDeformerKeyforms.is_reflect_x + i*4);
        is_reflect_x = bytes.readBoolean();

        bytes.seek(rotationDeformerKeyforms.is_reflect_y + i*4);
        is_reflect_y = bytes.readBoolean();
    }


};
}
#endif // CROTATION_DEFORMER_KEYFORMS_H
