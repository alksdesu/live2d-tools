#ifndef CGEOMETRYOBJECTPAIRS_H
#define CGEOMETRYOBJECTPAIRS_H

#include "cmodelgeometryedit.h"
#include <QByteArray>
#include <QJsonObject>
#include <QMetaType>
#include <QPointF>
#include <QString>
#include <QVector>

namespace live2dstruct {
class CModelDocument;

class CGeometryObjectPairs
{
public:
    using Type = CModelGeometryKeyform::Type;
    struct Object {
        Type type = Type::ArtMesh;
        QString id;
        bool operator==(const Object& other) const { return type == other.type && id == other.id; }
        bool operator!=(const Object& other) const { return !(*this == other); }
    };
    struct Axis {
        int index = -1;
        QString parameterId;
        QVector<float> keys;
        int parameterType = 0;
        bool operator==(const Axis& other) const;
        bool operator!=(const Axis& other) const { return !(*this == other); }
    };
    struct Endpoint {
        Object object;
        int vertexCount = 0;
        int rows = 0, columns = 0;
        QVector<quint16> triangles;
        QVector<Axis> axes;
        int keyformCount = 0;
        bool operator==(const Endpoint& other) const;
        bool operator!=(const Endpoint& other) const { return !(*this == other); }
    };
    struct AxisMapping {
        int firstAxis = -1, secondAxis = -1;
        QVector<int> firstKeyToSecondKey;
        bool operator==(const AxisMapping& other) const { return firstAxis == other.firstAxis && secondAxis == other.secondAxis && firstKeyToSecondKey == other.firstKeyToSecondKey; }
    };
    struct Pair {
        QString id;
        Endpoint first, second;
        QVector<int> firstVertexToSecondVertex;
        QVector<AxisMapping> axes;
        QPointF linePoint;
        QPointF lineDirection = QPointF(0, 1);
        bool reversedWinding = false;
        bool enabled = true;
        bool requiresReview = false;
        QPointF rotationLocalX = QPointF(1, 0);
        QPointF rotationLocalY = QPointF(0, 1);
        bool operator==(const Pair& other) const;
        bool operator!=(const Pair& other) const { return !(*this == other); }
    };
    struct State {
        QByteArray sourceDigest;
        QVector<Pair> pairs;
        bool operator==(const State& other) const { return sourceDigest == other.sourceDigest && pairs == other.pairs; }
        bool operator!=(const State& other) const { return !(*this == other); }
    };
    struct VertexRemap {
        Object object;
        QVector<int> oldToNew;
    };
    struct AxisRemap {
        Object object;
        int oldAxis = -1, newAxis = -1;
        QVector<int> oldKeyToNewKey;
    };

    static constexpr int MaximumPairs = 10000;
    static constexpr int MaximumVertices = 1000000;
    static constexpr int MaximumMappings = 2000000;
    static constexpr int MaximumTextLength = 1024;
    static constexpr qint64 MaximumBytes = 32 * 1024 * 1024;
    static constexpr double RotationFrameTolerance = 1e-10;

    static QString TypeName(Type type);
    static bool Normalize(const State& state, State& result, QString* error = nullptr);
    static bool DescribeEndpoint(const CModelDocument& document, const Object& object,
                                 Endpoint& result, QString* error = nullptr);
    static bool Validate(const State& state, const CModelDocument& document, QString* error = nullptr);
    static bool ResolveKeyform(const Pair& pair, int keyform, bool firstToSecond,
                               int& result, QString* error = nullptr);
    static bool Reconcile(const State& state, const CModelDocument& before, const CModelDocument& after,
                          const QVector<VertexRemap>& vertices, const QVector<AxisRemap>& axes,
                          State& result, QString* error = nullptr);
    static bool Reanchor(const State& state, const CModelDocument& before, const CModelDocument& after,
                         State& result, QString* error = nullptr);
    static bool ToJson(const State& state, QJsonObject& result, QString* error = nullptr);
    static bool FromJson(const QJsonObject& object, State& result, QString* error = nullptr);
};
}

Q_DECLARE_METATYPE(live2dstruct::CGeometryObjectPairs::State)

#endif
