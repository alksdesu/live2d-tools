#ifndef CDEFORMERSTRUCTUREREPLAY_H
#define CDEFORMERSTRUCTUREREPLAY_H

#include "cdeformerpreserverequest.h"

namespace live2dstruct {
class CDeformerStructureReplay
{
public:
    using Owner = CMocDeformerStructureBuilder::Owner;
    struct Impact
    {
        CMocDeformerStructureBuilder::Impact structure;
        QVector<Owner> coordinateOwners;
        QVector<Owner> scalarOwners;
        QStringList blockingReasons;
        QStringList numericalLimitations;
    };
    static bool Analyze(const QByteArray& base, const CDeformerPreserveRequest& request,
                        Impact& result, QString* error = nullptr);
    static bool Build(const QByteArray& base, const CDeformerPreserveRequest& request,
                      CMocDeformerStructureBuilder::Result& result, QString* error = nullptr);
};
}

#endif
