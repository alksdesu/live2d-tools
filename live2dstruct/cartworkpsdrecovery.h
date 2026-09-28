#ifndef CARTWORKPSDRECOVERY_H
#define CARTWORKPSDRECOVERY_H

#include "cmodelartwork.h"
#include "psd/cpsdsourcedocument.h"

namespace live2dstruct {
class CArtworkPsdRecovery
{
public:
    static bool Prepare(const CModelArtwork::State& artwork, CPsdSourceCatalog& result,
                        QString* error = nullptr, CExportControl* control = nullptr);
};
}

#endif
