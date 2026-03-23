#ifndef CPOSITION_INDICES_H
#define CPOSITION_INDICES_H
#include "live2dstruct/util/cfilereader.h"
#include <vector>
namespace live2dstruct {
class CPosition_indices
{
public:
    class Offset{
    public:
        int indices;

        Offset(CReader& bytes) {
            indices = bytes.readInt();
        }
    };

    std::vector<short> indices;
    CPosition_indices(CReader& bytes,const Offset& positionIndices, int size) {
        bytes.seek(positionIndices.indices);
        indices.resize(size,0);
        for(int i = 0 ;i < size;i++){
            indices[i] = bytes.readShort();
        }


    }
    short GetData(int index){
        return indices[index];
    }

};
}

#endif // CPOSITION_INDICES_H
