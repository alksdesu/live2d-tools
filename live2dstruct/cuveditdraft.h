#ifndef CUVEDITDRAFT_H
#define CUVEDITDRAFT_H

#include "cmodelmaterialedit.h"
#include <QByteArray>
#include <QMap>
#include <QPointF>
#include <QRectF>
#include <QSet>
#include <QSize>
#include <QStringList>

namespace live2dstruct {
class CUvEditDraft
{
public:
    struct Mesh {
        CModelDrawableMaterial material;
        QVector<quint16> triangles;
        bool editable = true;
        QString reason;
    };
    struct Snapshot {
        QByteArray sourceDigest;
        quint64 documentRevision = 0;
        QByteArray topologyDigest;
        QVector<Mesh> meshes;
        QMap<int, QSize> atlasSizes;
    };
    struct Selection {
        QString meshId;
        QSet<int> vertices;
    };
    struct Transform {
        QPointF translation;
        QPointF pivot;
        QPointF scale = {1, 1};
        double angleDegrees = 0;
        bool snapToPixels = false;
    };
    enum class SlotCoordinates { Normalized, Pixels };
    static constexpr int MaximumVertices = 1000000;

    bool Initialize(const Snapshot& snapshot, QString* error = nullptr);
    const Snapshot& Source() const { return m_snapshot; }
    CModelMaterialState InitialState() const;
    bool ValidateState(const CModelMaterialState& state, QString* error = nullptr) const;
    bool TransformSelection(const CModelMaterialState& state, const Selection& selection,
                            const Transform& transform, CModelMaterialState& result, QString* error = nullptr) const;
    bool SetPointPixels(const CModelMaterialState& state, const Selection& selection, QPointF point,
                        CModelMaterialState& result, QString* error = nullptr) const;
    bool AssignTexture(const CModelMaterialState& state, const QString& meshId, int slot,
                       SlotCoordinates coordinates, CModelMaterialState& result, QString* error = nullptr) const;
    bool Reset(const CModelMaterialState& state, const Selection& selection, bool allMeshes,
               CModelMaterialState& result, QString* error = nullptr) const;
    bool SelectionBounds(const CModelMaterialState& state, const Selection& selection, QRectF& result,
                         QString* error = nullptr) const;
    QSet<int> ConnectedIsland(const QString& meshId, int vertex) const;
    QStringList Diagnostics(const CModelMaterialState& state, const QString& meshId) const;
    int MeshIndex(const QString& id) const;

private:
    bool CheckSelection(const CModelMaterialState& state, const Selection& selection, int& index, QString* error) const;
    Snapshot m_snapshot;
};
}

#endif
