#ifndef CMESHEDITPLANNER_H
#define CMESHEDITPLANNER_H

#include "cmocmeshstructurebuilder.h"
#include <QPointF>

class CExportControl;

namespace live2dstruct {
class CMeshEditPlanner
{
public:
    static constexpr qint64 MaximumWork = 16000000;
    struct Edge {
        int a = -1;
        int b = -1;
        bool operator==(const Edge& other) const { return a == other.a && b == other.b; }
    };
    struct Input {
        QString artMeshId;
        QVector<float> positions;
        QVector<quint16> indices;
        QVector<Edge> protectedEdges;
    };
    enum class BoundaryPolicy { PreserveBoundary, ReplaceWithChord };
    enum class CrossingPolicy { RejectProtected, SplitProtected };
    enum class Code {
        None, InvalidInput, NonFinite, Degenerate, DuplicateTriangle,
        NonManifold, InconsistentWinding, Intersection, InvalidConstraint,
        OutsideMesh, AmbiguousLocation, InvalidSelection, BoundaryChange,
        ProtectedEdge, InvalidCavity, EmptyTopology, PrecisionLoss, LimitExceeded
    };
    struct Diagnostic {
        Code code = Code::None;
        QString message;
        QVector<int> vertices;
        QVector<int> triangles;
        QVector<Edge> edges;
    };
    struct Result {
        CMocMeshStructureBuilder::MeshEdit edit;
        QVector<Edge> protectedEdges;
        QVector<int> selectedVertices;
        QVector<int> insertedVertices;
        QVector<int> affectedTriangles;
        bool boundaryChanged = false;
    };
    struct ConstrainedInput
    {
        QVector<QPointF> points;
        QVector<Edge> constraints;
        qint64 maximumWork = MaximumWork;
        qint64 maximumWorkingBytes = 512LL * 1024 * 1024;
        int maximumVertices = CMocMeshStructureBuilder::MaximumVertices;
    };
    struct ConstrainedResult
    {
        QVector<QPointF> points;
        QVector<quint16> indices;
        qint64 operations = 0;
    };

    static bool Insert(const Input&, QPointF point, int seedTriangle,
                       Result&, Diagnostic* = nullptr);
    static bool Delete(const Input&, const QVector<int>& vertices, BoundaryPolicy,
                       Result&, Diagnostic* = nullptr);
    static bool Connect(const Input&, int first, int second, CrossingPolicy,
                        Result&, Diagnostic* = nullptr);
    static bool Retriangulate(const Input&, const QVector<int>& triangles,
                              Result&, Diagnostic* = nullptr);
    static bool TriangulateVertices(const Input&, Result&, Diagnostic* = nullptr);
    static bool TriangulateConstraints(
        const ConstrainedInput&, ConstrainedResult&, Diagnostic* = nullptr, CExportControl* = nullptr);
};
}

#endif
