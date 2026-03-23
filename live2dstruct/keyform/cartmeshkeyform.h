#ifndef CARTMESHKEYFORM_H
#define CARTMESHKEYFORM_H
#include <vector>
#include "ckeyformobject.h"

namespace live2dstruct {
class CArtMeshKeyForm : public CKeyFormObject
{
public:
    CArtMeshKeyForm();
    CArtMeshKeyForm(float opacities, float draw_orders, const std::vector<float> &position);
    virtual ~CArtMeshKeyForm();

    float getDraw_orders() {
        return draw_orders;
    }

    float getOpacities() {
        return opacities;
    }

    std::vector<float>& GetPosition() {
        return position;
    }
    void SetPosition(std::vector<float>& pos){
        position = pos;
    }

private:
    float opacities;
    float draw_orders;
    std::vector<float>  position;



};
}
#endif // CARTMESHKEYFORM_H
