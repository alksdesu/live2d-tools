#ifndef CMOCMATERIALBUILDER_H
#define CMOCMATERIALBUILDER_H

#include "cmodelmaterialedit.h"
#include <QSet>

namespace live2dstruct {
class CSectionTable;
class CMocMaterialBuilder
{
public:
    static bool Capture(const CSectionTable& table, CModelMaterialOverrides& result, QString* error = nullptr);
    static bool Capture(const QByteArray& base, CModelMaterialOverrides& result, QString* error = nullptr);
    static bool Capture(const QByteArray& base, const QString& id, CModelDrawableMaterial& result, QString* error = nullptr);
    static bool Build(const QByteArray& base, const CModelMaterialOverrides& overrides, QByteArray& result, QString* error = nullptr);
    static bool ValidateTransition(const QByteArray& before, const QByteArray& after,
                                   const QSet<QString>& allowedIds, QString* error = nullptr);
};
}
#endif
