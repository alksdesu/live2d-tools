#ifndef CPARAMETERRESOURCEEDITS_H
#define CPARAMETERRESOURCEEDITS_H

#include "cparameterreferenceeditor.h"
#include <QStringList>

namespace live2dstruct {
class CParameterResourceEdits
{
public:
    using Kind = CParameterReferenceEditor::Kind;
    struct Resource {
        Kind kind = Kind::Motion;
        QByteArray source;
        QSet<QString> suppressedIds;
    };
    struct State {
        QString modelPath;
        QByteArray modelSource;
        QSet<QString> modelSuppressedIds, breath, mouseFollow;
        QMap<QString, Resource> resources;
        bool IsEmpty() const;
    };
    struct Prepared {
        State state;
        CParameterReferenceEditor::Prepared model;
        QMap<QString, CParameterReferenceEditor::Prepared> resources;
        QStringList removals;
    };
    static bool Prepare(const QString& modelPath, const State& current, const QSet<QString>& deletedIds,
                        Prepared& prepared, QString* error = nullptr);
    static bool Validate(const State& state, QString* error = nullptr);
    static bool ToJson(const State& state, QJsonObject& object, QString* error = nullptr);
    static bool FromJson(const QJsonObject& object, State& state, QString* error = nullptr);
};
}

Q_DECLARE_METATYPE(live2dstruct::CParameterResourceEdits::State)

#endif
