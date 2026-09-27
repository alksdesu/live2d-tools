#ifndef CATLASREPACKPLANNER_H
#define CATLASREPACKPLANNER_H

#include "cmodelatlasedit.h"
#include <QImage>
#include <QPointF>
#include <QRect>
#include <QSet>
#include <QStringList>
#include <memory>

class CExportControl;

namespace live2dstruct {
class CAtlasRepackPlanner
{
public:
    struct Mesh {
        CModelDrawableMaterial material;
        QVector<quint16> triangles;
        bool editable = true;
        QString reason;
    };
    struct Limits {
        int maximumDimension = 16384;
        qint64 maximumPixels = CTextureWorkspace::MaximumDecodedBytes / 4;
        qint64 maximumOperations = 512LL * 1024 * 1024;
    };
    struct Snapshot {
        QByteArray sourceDigest;
        quint64 documentRevision = 0;
        QByteArray topologyDigest;
        CTextureAssets::Catalog sources;
        CTextureWorkspace::State workspace;
        QVector<Mesh> meshes;
        Limits limits;
        CModelArtwork::State artwork;
    };
    struct Plan {
        CModelAtlasState state;
        QMap<QString, QVector<QPointF>> pixelCoordinates;
        QMap<int, QImage> images;
        QSet<QString> changedMeshes;
        QSet<int> changedSlots;
        QStringList warnings;
        QByteArray token;

    private:
        struct Verification;
        std::shared_ptr<const Verification> m_verification;
        friend struct CAtlasPlanVerification;
    };
    static bool Initialize(const Snapshot& snapshot, Plan& result, QString* error = nullptr,
                           CExportControl* control = nullptr);
    static bool Validate(const Snapshot& snapshot, const Plan& plan, QString* error = nullptr);
    static bool SelectionBounds(const Snapshot& snapshot, const Plan& plan, const QStringList& ids,
                                int gutter, QRect& bounds, int& slot, QString* error = nullptr);
    static bool AddAtlas(const Snapshot& snapshot, const Plan& plan, const QString& name, QSize size,
                         Plan& result, QString* error = nullptr, CExportControl* control = nullptr);
    static bool ResizeAtlas(const Snapshot& snapshot, const Plan& plan, int slot, QSize size,
                            Plan& result, QString* error = nullptr, CExportControl* control = nullptr);
    static bool Move(const Snapshot& snapshot, const Plan& plan, const QStringList& ids, int targetSlot,
                     QPoint topLeft, int quarterTurns, int gutter, Plan& result,
                     QString* error = nullptr, CExportControl* control = nullptr);
    static bool Pack(const Snapshot& snapshot, const Plan& plan, const QStringList& ids,
                     const QVector<int>& targetSlots, int gutter, bool rotate90, Plan& result,
                     QString* error = nullptr, CExportControl* control = nullptr);
    static bool RemoveAtlas(const Snapshot& snapshot, const Plan& plan, int slot, Plan& result,
                            QString* error = nullptr, CExportControl* control = nullptr);
    static bool RenameAtlas(const Snapshot& snapshot, const Plan& plan, int slot, const QString& name,
                            Plan& result, QString* error = nullptr);
};
}

#endif
