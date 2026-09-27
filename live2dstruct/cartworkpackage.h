#ifndef CARTWORKPACKAGE_H
#define CARTWORKPACKAGE_H

#include "cmodeldocument.h"
#include "std/cdirectoryexport.h"

class CExportControl;

namespace live2dstruct {
class CArtworkPackage
{
public:
    struct Limits {
        qint64 maximumFileBytes = 2LL * 1024 * 1024 * 1024 - 1;
        qint64 maximumTotalBytes = 4LL * 1024 * 1024 * 1024;
        qint64 maximumIndexBytes = 256LL * 1024 * 1024;
        int maximumFiles = 10000;
    };
    struct Request {
        QString modelFile;
        QByteArray modelSettingsBytes, mocBytes;
        CTextureAssets::Catalog sources;
        CModelAtlasState state;
        Limits limits;
    };
    struct Prepared {
        QString manifestFile;
        QVector<CDirectoryExport::Entry> entries;
        QStringList protectedPaths;
        qint64 totalBytes = 0;
    };
    struct Loaded {
        bool present = false;
        CModelAtlasState state;
        QStringList notes;
    };
    static QString ManifestFile(const QString& modelFile);
    static bool Prepare(const Request& request, Prepared& result, CExportControl* control = nullptr,
                        QString* error = nullptr);
    static bool Load(const QString& modelJsonPath, const QByteArray& modelSettingsBytes,
                     const CModelDocument& document, Loaded& result, CExportControl* control = nullptr,
                     QString* error = nullptr);
    static bool Load(const QString& modelJsonPath, const QByteArray& modelSettingsBytes,
                     const CModelDocument& document, const Limits& limits, Loaded& result,
                     CExportControl* control = nullptr, QString* error = nullptr);
};
}

#endif
