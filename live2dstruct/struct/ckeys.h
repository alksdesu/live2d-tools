#ifndef CKEYS_H
#define CKEYS_H
#include "live2dstruct/util/cfilereader.h"
#include <vector>
namespace live2dstruct {
class CKeys
{
public:

    class Offset{
    public:
        int values;
        Offset(CReader& bytes) {
            values = bytes.readInt();
        }
    };

    std::vector<float> values;//参数值
    CKeys(CReader& bytes,const Offset& keys, int size) {
        bytes.seek(keys.values);
        values.resize(size,0.0f);
        for(int i = 0 ;i < size;i++){
            values[i] = bytes.readFloat();
        }
    }
    float GetData(int index) const {
        return index >= 0 && index < static_cast<int>(values.size()) ? values[index] : 0.0f;
    }

};
}
#endif // CKEYS_H
