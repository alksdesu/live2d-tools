#include "cmodelatlasedit.h"

namespace live2dstruct {
bool CModelAtlasState::operator==(const CModelAtlasState& other) const
{
    return workspace == other.workspace && materials == other.materials;
}
}
