#ifndef CMODELSAVEFINGERPRINT_H
#define CMODELSAVEFINGERPRINT_H

#include <QByteArray>
#include <QString>

namespace live2dstruct {
class CModelDocument;

class CModelSaveFingerprint
{
public:
    static bool Capture(const CModelDocument& document, QByteArray& result, QString* error = nullptr);
};
}

#endif
