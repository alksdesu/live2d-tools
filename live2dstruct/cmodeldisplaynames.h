#ifndef CMODELDISPLAYNAMES_H
#define CMODELDISPLAYNAMES_H

#include <QJsonObject>
#include <QMap>
#include <QMetaType>
#include <QString>

namespace live2dstruct {
class CModelDisplayNames
{
public:
    using Map = QMap<QString, QString>;
    struct State {
        Map parts, artMeshes, deformers;
        bool operator==(const State& other) const {
            return parts == other.parts && artMeshes == other.artMeshes && deformers == other.deformers;
        }
        bool operator!=(const State& other) const { return !(*this == other); }
    };
    static constexpr int MaximumNameLength = 1024;
    static bool ValidateName(const QString& name, QString* error = nullptr);
    static bool ValidateMap(const Map& names, QString* error = nullptr);
    static bool NormalizeMap(const Map& names, Map& result, QString* error = nullptr);
    static bool MapFromJson(const QJsonObject& object, Map& result, QString* error = nullptr);
    static bool MapToJson(const Map& names, QJsonObject& result, QString* error = nullptr);
    static bool Validate(const State& names, QString* error = nullptr);
    static bool Normalize(const State& names, State& result, QString* error = nullptr);
    static bool FromJson(const QJsonObject& object, State& result, QString* error = nullptr);
    static bool ToJson(const State& names, QJsonObject& result, QString* error = nullptr);
    static QString EffectiveName(const State& names, const QString& type, const QString& id);
};
}

Q_DECLARE_METATYPE(live2dstruct::CModelDisplayNames::State)

#endif
