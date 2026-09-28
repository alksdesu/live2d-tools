#ifndef CARTWORKSOURCEIMPORT_H
#define CARTWORKSOURCEIMPORT_H

#include "cmodelartwork.h"
#include "psd/cpsdsnapshot.h"

class CExportControl;
class CPsdReadDocument;

namespace live2dstruct {
class CArtworkSourceImport
{
public:
    struct SourceCandidate {
        CModelArtwork::Revision revision;
        QMap<QByteArray, CModelArtwork::Image> images;
        QStringList diagnostics;
        CModelArtwork::SourceKind kind = CModelArtwork::SourceKind::Psd;
    };
    static bool Prepare(const CPsdReadDocument& document, const QString& familyId, const QString& revisionId,
                        CModelArtwork::ColorPolicy colorPolicy, const QTransform& documentToFamily,
                        SourceCandidate& result, QString* error = nullptr, CExportControl* control = nullptr);
    static bool Prepare(const CPsdSnapshot& snapshot, const QString& familyId, const QString& revisionId,
                        CModelArtwork::ColorPolicy colorPolicy, const QTransform& documentToFamily,
                        SourceCandidate& result, QString* error = nullptr, CExportControl* control = nullptr);
};
}

#endif
