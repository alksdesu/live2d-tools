#ifndef CDRAWABLE_MASKS_H
#define CDRAWABLE_MASKS_H
#include "live2dstruct/util/cfilereader.h"
namespace live2dstruct {
class CDrawable_masks
{
public:
    class Offset{
    public:
        int art_mesh_sources_indices;

        Offset(CReader& bytes) {
            art_mesh_sources_indices = bytes.readInt();
        }
    };

    CDrawable_masks(CReader& bytes,const  Offset& drawableMasks, int i) {
        bytes.seek(drawableMasks.art_mesh_sources_indices + i*4);
        art_mesh_sources_indices = bytes.readInt();

    }
    int art_mesh_sources_indices;


};
}

#endif // CDRAWABLE_MASKS_H
