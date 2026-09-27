#ifndef CPSDSOURCEWRITER_H
#define CPSDSOURCEWRITER_H

#include "cpsdsourcedocument.h"

class CExportControl;

class CPsdSourceWriter
{
public:
    static bool Validate(const CPsdSourceDocument& document, QString* error = nullptr);
    static bool Write(const QString& path, const CPsdSourceDocument& document,
                      CExportControl* control = nullptr, QString* error = nullptr);
};

#endif
