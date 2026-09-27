#ifndef CRUNTIMEPACKAGE_H
#define CRUNTIMEPACKAGE_H

#include "cmodeldocument.h"
#include "cparameterresourceedits.h"
#include "ctextureassets.h"
#include "std/cdirectoryexport.h"
#include <optional>

class CExportControl;

namespace live2dstruct {
class CRuntimePackage
{
public:
    struct Request {
        QString modelJsonPath;
        QByteArray modelSettingsDigest, sourceMocDigest, mocBytes;
        QByteArray loadedModelSettingsBytes;
        QString registeredUserDataPath;
        std::optional<CTextureAssets::Catalog> textureSources;
        CTextureAssets::State textureState;
        std::optional<CTextureWorkspace::State> textureWorkspace;
        CParameterResourceEdits::State references;
        QMap<QString, QByteArray> expectedSourceDigests;
        QHash<QString, CModelParameterMetadata> parameterMetadata;
        CParameterOrganization::State parameterOrganization;
        CGeometryObjectPairs::State geometryObjectPairs;
        std::optional<QByteArray> physics, pose, userData, timeline;
        QString timelineGroup = QStringLiteral("Edited");
        QString timelineSoundPath;
        qint64 maximumFileBytes = 2LL * 1024 * 1024 * 1024 - 1;
        qint64 maximumTotalBytes = 4LL * 1024 * 1024 * 1024;
        int maximumFiles = 10000;
        std::optional<CModelArtwork::State> artwork;
    };
    struct Source {
        QString path;
        QByteArray digest;
        qint64 size = 0;
    };
    struct Prepared {
        QString modelFile = QStringLiteral("model.model3.json");
        QVector<CDirectoryExport::Entry> entries;
        QVector<Source> sources;
        QStringList protectedPaths, notes;
        QJsonObject model;
        qint64 totalBytes = 0;
    };
    static bool Prepare(const Request& request, Prepared& prepared, CExportControl* control = nullptr,
                        QString* error = nullptr);
};
}

#endif
