#ifndef CPHYSICSDOCUMENT_H
#define CPHYSICSDOCUMENT_H

#include <QJsonObject>

namespace live2dstruct {
class CPhysicsDocument
{
public:
    static bool Validate(const QJsonObject& document, QString* error = nullptr);
    static bool Normalize(const QJsonObject& document, QJsonObject& result, QString* error = nullptr);
};
}

#endif
