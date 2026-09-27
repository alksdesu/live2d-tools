#ifndef CPARAMETERORGANIZATION_H
#define CPARAMETERORGANIZATION_H

#include <QJsonObject>
#include <QMap>
#include <QMetaType>
#include <QSet>
#include <QStringList>
#include <QVector>

namespace live2dstruct {
class CModelDocument;

class CParameterOrganization
{
public:
    struct Node {
        enum Kind { Folder, Parameter };
        Kind kind = Parameter;
        QString id;
        QString parentFolder;
        QString name;
        QString color;
        bool operator==(const Node& other) const {
            return kind == other.kind && id == other.id && parentFolder == other.parentFolder &&
                name == other.name && color == other.color;
        }
    };
    struct ParameterPair {
        QString first, second;
        bool reversed = false;
        bool enabled = true;
        bool operator==(const ParameterPair& other) const {
            return first == other.first && second == other.second && reversed == other.reversed && enabled == other.enabled;
        }
    };
    struct State {
        bool configured = false;
        QVector<Node> nodes;
        QVector<ParameterPair> pairs;
        bool operator==(const State& other) const { return configured == other.configured && nodes == other.nodes && pairs == other.pairs; }
        bool operator!=(const State& other) const { return !(*this == other); }
    };
    static constexpr int MaximumNodes = 100000;
    static constexpr int MaximumTextLength = 1024;
    static constexpr qint64 MaximumBytes = 16 * 1024 * 1024;
    static bool Normalize(const State& state, State& result, QString* error = nullptr);
    static bool Validate(const State& state, const QSet<QString>& parameterIds, QString* error = nullptr);
    static bool Reconcile(const State& state, const QStringList& beforeIds, const QStringList& afterIds,
                          State& result, QString* error = nullptr);
    static bool ExpandUserValues(const CModelDocument& document, const QMap<QString, float>& explicitValues,
                                 QMap<QString, float>& expanded, QString* error = nullptr);
    static bool ToJson(const State& state, QJsonObject& result, QString* error = nullptr);
    static bool FromJson(const QJsonObject& object, State& result, QString* error = nullptr);
};
}

Q_DECLARE_METATYPE(live2dstruct::CParameterOrganization::State)

#endif
