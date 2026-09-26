#ifndef CPARAMETERREFERENCEPLAN_H
#define CPARAMETERREFERENCEPLAN_H

#include "framework/Model/CubismParameterReferences.hpp"
#include "live2dstruct/cparameterresourceedits.h"

class CParameterReferencePlan
{
public:
    struct Prepared {
        live2dstruct::CParameterResourceEdits::Prepared resources;
        ParameterReferenceState runtime;
    };
    static bool Prepare(const QString& modelJsonPath, const live2dstruct::CParameterResourceEdits::State& current,
                        const QSet<QString>& deletedIds, const ParameterReferenceCatalog& catalog,
                        Prepared& prepared, QString* error = nullptr);
};

#endif
