#ifndef CARTWORKSOURCELIBRARYPLANNER_H
#define CARTWORKSOURCELIBRARYPLANNER_H

#include "cartworkupdateplanner.h"

namespace live2dstruct {
class CArtworkSourceLibraryPlanner
{
public:
    using Snapshot = CArtworkUpdatePlanner::Snapshot;
    struct Source {
        CArtworkSourceImport::SourceCandidate incoming;
        QString familyName;
    };
    struct Plan {
        CModelAtlasState state;
        QByteArray token;
        int documentCount = 0, layerCount = 0;
    private:
        struct Verification;
        std::shared_ptr<const Verification> m_verification;
        friend class CArtworkSourceLibraryPlanner;
    };
    static bool Build(const Snapshot& snapshot, const QVector<Source>& sources, Plan& result,
                      QString* error = nullptr, CExportControl* control = nullptr);
    static bool Validate(const Snapshot& snapshot, const Plan& plan, QString* error = nullptr);
};
}

#endif
