#include "cwarpnodekeyform.h"

namespace live2dstruct {
CWarpNodeKeyform::CWarpNodeKeyform(float opacities, std::vector<float> &keyform_position)
{
    this->opacities = opacities;
    this->keyform_position = keyform_position;//这个暂时未知
}

CWarpNodeKeyform::CWarpNodeKeyform()
{

}

CWarpNodeKeyform::~CWarpNodeKeyform()
{

}
}
