#ifndef CWARPNODEKEYFORM_H
#define CWARPNODEKEYFORM_H
#include <vector>
#include "ckeyformobject.h"
namespace live2dstruct {

class CWarpNodeKeyform : public CKeyFormObject
{
public:
    CWarpNodeKeyform(float opacities, std::vector<float>& keyform_position);
    CWarpNodeKeyform();
    virtual ~CWarpNodeKeyform();

    std::vector<float>& GetPosition(){
        return keyform_position;
    }
    const std::vector<float>& GetPosition() const {
        return keyform_position;
    }
    float GetOpacities(){
        return opacities;
    }
    void SetPosition(std::vector<float>& pos){
        keyform_position = pos;
    }
private:
    float opacities = 1.0f;
    std::vector<float> keyform_position;//这个暂时未知
};
}
#endif // CWARPNODEKEYFORM_H
