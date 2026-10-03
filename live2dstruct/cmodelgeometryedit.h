#ifndef CMODELGEOMETRYEDIT_H
#define CMODELGEOMETRYEDIT_H

#include <QMetaType>
#include <QByteArray>
#include <QString>
#include <QVector>
#include <variant>

namespace live2dstruct {
struct CModelRotationTransform
{
    float angle = 0.0f;
    float originX = 0.0f;
    float originY = 0.0f;
    float scale = 1.0f;
    bool reflectX = false;
    bool reflectY = false;

    bool operator==(const CModelRotationTransform& other) const;
    bool operator!=(const CModelRotationTransform& other) const { return !(*this == other); }
};

struct CModelGeometryKeyform
{
    enum Type : int { ArtMesh, Warp, Rotation };
    Type type = ArtMesh;
    QString id;
    int keyform = -1;
    std::variant<QVector<float>, CModelRotationTransform> value;

    QString TypeName() const;
    bool SameEndpoint(const CModelGeometryKeyform& other) const;
    bool operator==(const CModelGeometryKeyform& other) const;
    bool operator!=(const CModelGeometryKeyform& other) const { return !(*this == other); }
};

using CModelGeometryState = QVector<CModelGeometryKeyform>;

struct CModelBlendShapeGeometryEntry
{
    QString meshId;
    int shapeIndex = -1;
    int bindingOffset = -1;
    int keyformOffset = -1;
    int sourceKeyformIndex = -1;
    QByteArray sourceDigest;
    QVector<float> deltas;

    bool operator==(const CModelBlendShapeGeometryEntry& other) const
    {
        return meshId == other.meshId && shapeIndex == other.shapeIndex && bindingOffset == other.bindingOffset &&
            keyformOffset == other.keyformOffset && sourceKeyformIndex == other.sourceKeyformIndex &&
            sourceDigest == other.sourceDigest && deltas == other.deltas;
    }
};

using CModelBlendShapeGeometryState = QVector<CModelBlendShapeGeometryEntry>;
}

Q_DECLARE_METATYPE(live2dstruct::CModelGeometryState)
Q_DECLARE_METATYPE(live2dstruct::CModelBlendShapeGeometryState)

#endif
