#include "crotationnodekeyform.h"
namespace live2dstruct {
CRotationNodeKeyform::CRotationNodeKeyform() {
    keyform_position.resize(2,0.0f);
}

CRotationNodeKeyform::~CRotationNodeKeyform()
{

}

CRotationNodeKeyform::CRotationNodeKeyform(float opacities, float angles, float origin_x, float origin_y, float scales, bool is_reflect_x, bool is_reflect_y)
{

    keyform_position.resize(2,0.0f);
    this->angles = angles;
    this->is_reflect_x =is_reflect_x;
    this->is_reflect_y = is_reflect_y;
    this->opacities =opacities;

    keyform_position[0] =origin_x;
    keyform_position[1] =origin_y;

    this->scales = scales;

}
}
