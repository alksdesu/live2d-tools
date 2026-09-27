#ifndef CCMO3ARTWORKREADER_H
#define CCMO3ARTWORKREADER_H

#include "psd/cpsdsourcedocument.h"

class CExportControl;

namespace cmo3 {
class CCmo3ArtworkReader
{
public:
    static bool ReadFile(const QString& path, CPsdSourceCatalog& result,
                         QString* error = nullptr, CExportControl* control = nullptr);
    static bool ReadBytes(const QByteArray& bytes, const QString& sourcePath, CPsdSourceCatalog& result,
                          QString* error = nullptr, CExportControl* control = nullptr);
};
}

#endif
