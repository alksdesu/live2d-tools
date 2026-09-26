#ifndef CPARAMETERREFERENCES_H
#define CPARAMETERREFERENCES_H

#include <QJsonObject>
#include <QStringList>

namespace live2dstruct {
class CParameterReferences
{
public:
    static bool ScanModel(const QString& modelJsonPath, const QString& parameterId,
                          QStringList& references, QString* error = nullptr);
    static bool ScanPhysics(const QJsonObject& root, const QString& parameterId,
                            QStringList& references, QString* error = nullptr);
};
}

#endif
