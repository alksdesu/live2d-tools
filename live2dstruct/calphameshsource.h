#ifndef CALPHAMESHSOURCE_H
#define CALPHAMESHSOURCE_H

#include "calphameshplanner.h"

class CExportControl;

namespace live2dstruct {
class CModelDocument;
class CAlphaMeshSource
{
public:
    struct Source {
        CAlphaMeshPlanner::Input input;
        QString label, detail;
        bool linkedSource = false;
    };
    static bool Capture(const CModelDocument& document, const QString& meshId, Source& result,
                        QString* error = nullptr, CExportControl* control = nullptr);
};
}

#endif
