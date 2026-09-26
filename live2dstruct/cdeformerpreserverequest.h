#ifndef CDEFORMERPRESERVEREQUEST_H
#define CDEFORMERPRESERVEREQUEST_H

#include "cmocdeformerstructurebuilder.h"

namespace live2dstruct {
struct CDeformerPreserveRequest
{
    std::variant<CMocDeformerStructureBuilder::InsertRequest,
                 CMocDeformerStructureBuilder::RemoveRequest> operation;
    QByteArray sourceDigest;
    QByteArray expectedAuthoringDigest;
};
}

#endif
