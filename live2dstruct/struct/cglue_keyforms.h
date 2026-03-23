#ifndef CGLUE_KEYFORMS_H
#define CGLUE_KEYFORMS_H
#include "live2dstruct/util/cfilereader.h"
namespace live2dstruct {
class CGlue_keyforms
{
public:
    class Offset{
    public:
        int intensities;
        Offset(CReader& bytes) {
            intensities = bytes.readInt();
        }
    };

    int intensities;
    CGlue_keyforms(CReader& bytes,const Offset& glueKeyforms, int i) {
        bytes.seek(glueKeyforms.intensities+i*4);
        intensities = bytes.readInt();
    }



};
}
#endif // CGLUE_KEYFORMS_H
