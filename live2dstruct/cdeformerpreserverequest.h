#ifndef CDEFORMERPRESERVEREQUEST_H
#define CDEFORMERPRESERVEREQUEST_H

#include "cmocdeformerstructurebuilder.h"

namespace live2dstruct {
struct CDeformerStructureRequest
{
    std::variant<CMocDeformerStructureBuilder::InsertRequest, CMocDeformerStructureBuilder::RemoveRequest,
        CMocDeformerStructureBuilder::ResizeWarpRequest>
        operation;
    QByteArray sourceDigest;
    QByteArray expectedAuthoringDigest;
};
using CDeformerPreserveRequest = CDeformerStructureRequest;
}

#endif
