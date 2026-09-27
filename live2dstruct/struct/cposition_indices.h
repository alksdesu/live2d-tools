#ifndef CPOSITION_INDICES_H
#define CPOSITION_INDICES_H
#include "live2dstruct/util/cfilereader.h"
#include <QtGlobal>
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

    std::vector<quint16> indices;
    CPosition_indices(CReader& bytes,const Offset& positionIndices, int size) {
        bytes.seek(positionIndices.indices);
        indices.resize(size,0);
        for(int i = 0 ;i < size;i++){
            indices[i] = bytes.readUShort();
        }


    }
    quint16 GetData(int index) const {
        return index >= 0 && index < static_cast<int>(indices.size()) ? indices[index] : 0;
    }

};
}

#endif // CPOSITION_INDICES_H
