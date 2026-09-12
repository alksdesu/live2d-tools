#ifndef CUVS_H
#define CUVS_H
#include "live2dstruct/util/cfilereader.h"
#include <vector>
namespace live2dstruct {
class CUvs
{
public:
    class Offset{
    public:
        int uvs;
        Offset(CReader& bytes) {
            uvs = bytes.readInt();
        }
    };
    std::vector<float>  uvs;
    CUvs(CReader& bytes,const Offset& uvs, int size) {
        bytes.seek( uvs.uvs);
        this->uvs.resize(size,0);
        for(int i = 0 ;i < size;i++){
            this->uvs[i] =bytes.readFloat();
        }

    }
    float GetData(int index) const {
        return index >= 0 && index < static_cast<int>(uvs.size()) ? uvs[index] : 0.0f;
    }

};
}
#endif // CUVS_H
