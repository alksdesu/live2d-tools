#ifndef CGLUE_INFO_H
#define CGLUE_INFO_H
#include "live2dstruct/util/cfilereader.h"
namespace live2dstruct {
class CGlue_info
{
public:

    class Offset{
    public:
        int weights;
        int position_indices;

        Offset(CReader& bytes) {
            weights = bytes.readInt();
            position_indices= bytes.readInt();
        }
    };



    CGlue_info(CReader& bytes,const Offset& glueInfo, int i) {
        bytes.seek(glueInfo.weights+i*4);
        weights = bytes.readFloat();
        bytes.seek(glueInfo.position_indices+i*2);
        position_indices = bytes.readUShort();
    }
    float weights;
    int position_indices;
};
}
#endif // CGLUE_INFO_H
