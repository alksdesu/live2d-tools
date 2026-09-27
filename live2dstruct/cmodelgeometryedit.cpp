#include "cmodelgeometryedit.h"
#include <cstring>

namespace live2dstruct {
namespace {
bool sameFloat(float first, float second)
{
    quint32 a, b;
    std::memcpy(&a, &first, sizeof(a));
    std::memcpy(&b, &second, sizeof(b));
    return a == b;
}
}

bool CModelRotationTransform::operator==(const CModelRotationTransform& other) const
{
    return sameFloat(angle, other.angle) && sameFloat(originX, other.originX) &&
        sameFloat(originY, other.originY) && sameFloat(scale, other.scale) &&
        reflectX == other.reflectX && reflectY == other.reflectY;
}

QString CModelGeometryKeyform::TypeName() const
{
    switch (type) {
    case ArtMesh: return QStringLiteral("artmesh");
    case Warp: return QStringLiteral("warp");
    case Rotation: return QStringLiteral("rotation");
    }
    return {};
}

bool CModelGeometryKeyform::SameEndpoint(const CModelGeometryKeyform& other) const
{
    return type == other.type && id == other.id && keyform == other.keyform;
}

bool CModelGeometryKeyform::operator==(const CModelGeometryKeyform& other) const
{
    if (!SameEndpoint(other) || value.valueless_by_exception() || other.value.valueless_by_exception() ||
        value.index() != other.value.index()) return false;
    if (const auto* positions = std::get_if<QVector<float>>(&value)) {
        const auto& compared = std::get<QVector<float>>(other.value);
        if (positions->size() != compared.size()) return false;
        for (qsizetype index = 0; index < positions->size(); ++index)
            if (!sameFloat(positions->at(index), compared[index])) return false;
        return true;
    }
    return std::get<CModelRotationTransform>(value) == std::get<CModelRotationTransform>(other.value);
}
}
