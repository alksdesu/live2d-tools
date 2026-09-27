#ifndef CMODELMATERIALEDIT_H
#define CMODELMATERIALEDIT_H

#include <QByteArray>
#include <QJsonObject>
#include <QMap>
#include <QMetaType>
#include <QString>
#include <QVector>

namespace live2dstruct {
struct CModelDrawableMaterial
{
    QString id;
    int textureIndex = -1;
    QVector<float> uvs;
    bool operator==(const CModelDrawableMaterial& other) const;
    bool operator!=(const CModelDrawableMaterial& other) const { return !(*this == other); }
};
using CModelMaterialState = QVector<CModelDrawableMaterial>;
using CModelMaterialOverrides = QMap<QString, CModelDrawableMaterial>;
class CModelMaterialEdit
{
public:
    static constexpr int MaximumValues = 2000000;
    static bool Validate(const CModelMaterialState& state, QString* error = nullptr);
    static bool ToJson(const CModelMaterialOverrides& state, QJsonObject& result, QString* error = nullptr);
    static bool FromJson(const QJsonObject& object, CModelMaterialOverrides& result, QString* error = nullptr);
    static QByteArray Fingerprint(const CModelMaterialOverrides& state);
};
}
Q_DECLARE_METATYPE(live2dstruct::CModelDrawableMaterial)
Q_DECLARE_METATYPE(live2dstruct::CModelMaterialState)
Q_DECLARE_METATYPE(live2dstruct::CModelMaterialOverrides)
#endif
