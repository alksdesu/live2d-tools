#ifndef CMODELGEOMETRYEDIT_H
#define CMODELGEOMETRYEDIT_H

#include <QMetaType>
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
}

Q_DECLARE_METATYPE(live2dstruct::CModelGeometryState)

#endif
