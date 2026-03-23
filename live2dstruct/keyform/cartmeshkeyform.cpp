#include "cartmeshkeyform.h"
namespace live2dstruct {
CArtMeshKeyForm::CArtMeshKeyForm() {}

CArtMeshKeyForm::CArtMeshKeyForm(float opacities, float draw_orders, const std::vector<float> &position) : opacities(opacities),
    draw_orders(draw_orders),
    position(position)
{}

CArtMeshKeyForm::~CArtMeshKeyForm()
{

}
}
