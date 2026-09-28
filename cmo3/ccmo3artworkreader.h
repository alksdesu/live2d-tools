#ifndef CCMO3ARTWORKREADER_H
#define CCMO3ARTWORKREADER_H

#include "psd/cpsdsourcedocument.h"

class CExportControl;

namespace cmo3 {
class CCaffArchive;
namespace detail { class Xml; }
class CCmo3ArtworkReader
{
public:
    static bool ReadFile(const QString& path, CPsdSourceCatalog& result,
                         QString* error = nullptr, CExportControl* control = nullptr);
    static bool ReadBytes(const QByteArray& bytes, const QString& sourcePath, CPsdSourceCatalog& result,
                          QString* error = nullptr, CExportControl* control = nullptr);
private:
    friend class CCmo3Project;
    static CPsdSourceCatalog Extract(const CCaffArchive& archive, const detail::Xml& xml,
                                    const QString& path, const QByteArray& digest, CExportControl* control);
};
}

#endif
