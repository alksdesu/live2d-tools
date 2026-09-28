#ifndef CDISPLAYNAMEMETADATA_H
#define CDISPLAYNAMEMETADATA_H

#include "cmodeldisplaynames.h"
#include <optional>

namespace live2dstruct {
class CDisplayNameMetadata
{
public:
    struct Snapshot {
        std::optional<CModelDisplayNames::State> names;
        std::optional<CModelDisplayNames::Map> deformerNames;
    };
    static bool Read(const QJsonObject& root, Snapshot& result, QString* error = nullptr);
    static bool Write(const CModelDisplayNames::State& names, QJsonObject& root, QString* error = nullptr);
};
}

#endif
