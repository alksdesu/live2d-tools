#ifndef CMOCMESHSTRUCTUREBUILDER_H
#define CMOCMESHSTRUCTUREBUILDER_H

#include "cmocparameterstructurebuilder.h"
#include <QVector>
#include <QStringList>

namespace live2dstruct {
class CMocMeshStructureBuilder
{
public:
    static constexpr int MaximumVertices = 32768;
    static constexpr double WeightTolerance = 1e-9;
    struct VertexSource {
        QVector<int> indices;
        QVector<double> weights;
    };
    struct MeshEdit {
        QString artMeshId;
        QVector<VertexSource> vertices;
        QVector<int> oldToNew;
        QVector<quint16> indices;
    };
    enum class GlueEndpointPolicy { Reject, DropPair, Retarget };
    struct GlueRetarget {
        enum Side { A, B };
        int glueIndex = -1;
        int pairIndex = -1;
        Side side = A;
        int newVertex = -1;
    };
    struct Request {
        QVector<MeshEdit> meshes;
        GlueEndpointPolicy gluePolicy = GlueEndpointPolicy::Reject;
        QVector<GlueRetarget> glueRetargets;
    };
    struct MeshRemap {
        QString artMeshId;
        int oldVertexCount = 0;
        int newVertexCount = 0;
        QVector<int> oldToNew;
        QVector<int> newToOld;
    };
    struct GlueChange {
        int glueIndex = -1;
        QString glueId;
        int oldPairCount = 0;
        int newPairCount = 0;
        QVector<int> droppedPairs;
        QVector<GlueRetarget> retargetedEndpoints;
    };
    struct Impact {
        QVector<MeshRemap> meshes;
        QVector<GlueChange> glues;
        QStringList blockingReasons;
    };
    struct Result {
        QByteArray bytes;
        CMocParameterStructureBuilder::ColumnState state;
        QVector<MeshRemap> meshes;
        QVector<GlueChange> glues;
    };
    static bool Analyze(const QByteArray& base, const Request& request, Impact& result, QString* error = nullptr);
    static bool Build(const QByteArray& base, const Request& request, Result& result, QString* error = nullptr);
};
}

#endif
