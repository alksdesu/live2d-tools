#ifndef CPARAMETERREFERENCEEDITOR_H
#define CPARAMETERREFERENCEEDITOR_H

#include <QByteArray>
#include <QJsonObject>
#include <QMap>
#include <QSet>
#include <QString>
#include <QVector>

namespace live2dstruct {
class CParameterReferenceEditor
{
public:
    enum class Kind { ModelSettings, Motion, Expression, Physics, Timeline };
    struct Reference {
        QString kind, owner, parameterId;
        int row = -1;
    };
    struct Prepared {
        Kind kind = Kind::ModelSettings;
        QByteArray sourceBytes, sourceDigest, effectiveBytes;
        QJsonObject source, effective;
        QSet<QString> suppressedIds, eyeBlink, lipSync;
        QMap<int, QString> parameterRows, inputRows, outputRows;
        QVector<Reference> references;
    };
    static bool Prepare(Kind kind, const QByteArray& source, const QSet<QString>& suppressedIds,
                        Prepared& prepared, QString* error = nullptr);
};
}

#endif
