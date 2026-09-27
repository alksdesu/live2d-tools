#ifndef CMODELDIAGNOSTIC_H
#define CMODELDIAGNOSTIC_H

#include <QString>

namespace live2dstruct {
struct CModelDiagnostic
{
    enum Severity { Warning, Error };
    Severity severity = Error;
    QString code;
    QString message;
    int index = -1;
    QString objectType;
    QString objectId;
    bool operator==(const CModelDiagnostic& other) const
    {
        return severity == other.severity && code == other.code && message == other.message && index == other.index
            && objectType == other.objectType && objectId == other.objectId;
    }
    bool operator!=(const CModelDiagnostic& other) const { return !(*this == other); }
};
}

#endif
