#ifndef CMOCPREVIEWBUILDER_H
#define CMOCPREVIEWBUILDER_H

#include "cmodeldocument.h"
#include <QByteArray>
#include <QHash>
#include <QString>
#include <QVector>
#include <memory>

namespace live2dstruct {
class CMocPreviewBuilder
{
public:
    struct Overrides
    {
        QHash<QString, QVector<QVector<float>>> artMeshPositions;
        QHash<QString, QVector<CModelWarpKeyform>> warpKeyforms;
        QHash<QString, QVector<CModelRotationKeyform>> rotationKeyforms;
        QHash<QString, int> deformerParents;
        QHash<QString, QVector<int>> drawableMasks;
        QHash<QString, bool> drawableMaskInverted;
        QHash<QString, CModelParameterMetadata> parameterMetadata;
    };

    bool Initialize(const QByteArray& baseline, QString* error = nullptr);
    bool Initialize(const QByteArray& baseline, const QHash<QString, int>& originalParents,
                    QString* error = nullptr);
    QByteArray SourceDigest() const;
    bool BuildOverrides(const Overrides& overrides, QByteArray& result, QString* error = nullptr) const;
    bool Build(const QHash<QString, QVector<QVector<float>>>& overrides,
               QByteArray& result, QString* error = nullptr) const;
    bool Build(const QHash<QString, QVector<QVector<float>>>& artMeshPositions,
               const QHash<QString, QVector<CModelWarpKeyform>>& warpKeyforms,
               const QHash<QString, QVector<CModelRotationKeyform>>& rotationKeyforms,
               const QHash<QString, int>& deformerParents,
               QByteArray& result, QString* error = nullptr) const;

private:
    struct Snapshot;
    std::shared_ptr<const Snapshot> m_snapshot;
};
}

#endif
