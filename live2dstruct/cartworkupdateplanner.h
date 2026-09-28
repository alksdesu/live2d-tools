#ifndef CARTWORKUPDATEPLANNER_H
#define CARTWORKUPDATEPLANNER_H

#include "cartworksourceimport.h"
#include "catlasrepackplanner.h"

namespace live2dstruct {
class CArtworkUpdatePlanner
{
public:
    enum class Action { Keep, Associate, Update, Detach };
    struct Snapshot {
        QByteArray sourceDigest;
        quint64 documentRevision = 0;
        CTextureAssets::Catalog sources;
        CModelAtlasState baseline;
        QVector<CAtlasRepackPlanner::Mesh> meshes;
        CAtlasRepackPlanner::Limits limits;
    };
    struct Decision {
        QString meshId;
        Action action = Action::Keep;
        QString layerId;
        QTransform atlasToDocument;
        bool confirmed = false;
        bool applyLayerOpacity = true;
    };
    struct Request {
        CArtworkSourceImport::SourceCandidate incoming;
        QString familyName;
        QVector<Decision> decisions;
    };
    struct Impact {
        QString meshId;
        Action action = Action::Keep;
        QString layerId;
        int oldSlot = -1, newSlot = -1;
        QStringList notes;
    };
    struct Plan {
        CModelAtlasState state;
        QMap<int, QImage> images;
        QVector<Impact> impacts;
        QStringList warnings;
        QSet<QString> unconfirmed;
        QByteArray token;
    private:
        struct Verification;
        std::shared_ptr<const Verification> m_verification;
        friend struct CArtworkUpdateVerification;
    };
    static bool Build(const Snapshot& snapshot, const Request& request, Plan& result,
                      QString* error = nullptr, CExportControl* control = nullptr);
    static bool Validate(const Snapshot& snapshot, const Plan& plan, QString* error = nullptr,
                         bool requireConfirmed = false);
    static bool PreparedSource(const Snapshot& snapshot, const Plan& plan, CArtworkSourceImport::SourceCandidate& result,
                               QString* error = nullptr);
};
}
#endif
