#ifndef CMODELATLASEDIT_H
#define CMODELATLASEDIT_H

#include "ctextureworkspace.h"
#include "cmodelmaterialedit.h"
#include "cmodelartwork.h"

namespace live2dstruct {
struct CModelAtlasState
{
    CTextureWorkspace::State workspace;
    CModelMaterialState materials;
    CModelArtwork::State artwork;
    bool operator==(const CModelAtlasState& other) const;
    bool operator!=(const CModelAtlasState& other) const { return !(*this == other); }
};
}
Q_DECLARE_METATYPE(live2dstruct::CModelAtlasState)
#endif
