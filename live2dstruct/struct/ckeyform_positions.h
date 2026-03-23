#ifndef CKEYFORM_POSITIONS_H
#define CKEYFORM_POSITIONS_H
#include "live2dstruct/util/cfilereader.h"
#include <vector>
namespace live2dstruct {
class CKeyform_positions
{
public:
    class Offset{
    public:
        int xys;
        Offset(CReader& bytes) {
            xys = bytes.readInt();
        }
    };
    std::vector<float> xys;
    //float xys;//坐标
    CKeyform_positions(CReader& bytes,const Offset& keyformPositions,int size) {
        xys.resize(size,0.0f);
        bytes.seek(keyformPositions.xys);
        for(int i = 0 ; i < size;i++){
            xys[i] = bytes.readFloat();
        }
    }
    float GetData(int index){
        return xys[index];
    }


};
}
#endif // CKEYFORM_POSITIONS_H
