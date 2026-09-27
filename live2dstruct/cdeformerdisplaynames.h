#ifndef CDEFORMERDISPLAYNAMES_H
#define CDEFORMERDISPLAYNAMES_H

#include <QJsonObject>
#include <QMap>
#include <QString>

namespace live2dstruct {
class CDeformerDisplayNames
{
public:
    using State = QMap<QString, QString>;
    static constexpr int MaximumNameLength = 1024;
    static bool ValidateName(const QString& name, QString* error = nullptr);
    static bool Validate(const State& names, QString* error = nullptr);
    static bool Normalize(const State& names, State& result, QString* error = nullptr);
    static bool ToJson(const State& names, QJsonObject& result, QString* error = nullptr);
    static bool FromJson(const QJsonObject& object, State& result, QString* error = nullptr);
};
}

#endif
