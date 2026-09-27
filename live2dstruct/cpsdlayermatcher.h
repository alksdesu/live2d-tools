#ifndef CPSDLAYERMATCHER_H
#define CPSDLAYERMATCHER_H

#include "cmodelartwork.h"

class CExportControl;

namespace live2dstruct {
class CPsdLayerMatcher
{
public:
    enum class Status { Matched, NeedsConfirmation, Missing, Incompatible };
    struct Candidate {
        QString layerId;
        QStringList reasons;
        bool compatible = false;
    };
    struct Match {
        QString oldLayerId;
        Status status = Status::Missing;
        QString matchedLayerId;
        QVector<Candidate> candidates;
        QStringList reasons;
    };
    struct Result {
        QVector<Match> matches;
        QStringList unmatchedNewLayerIds;
    };
    static bool Compare(const CModelArtwork::Revision& previous, const CModelArtwork::Revision& candidate,
                        Result& result, QString* error = nullptr, CExportControl* control = nullptr);
};
}

#endif
