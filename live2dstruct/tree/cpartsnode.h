#ifndef CPARTSNODE_H
#define CPARTSNODE_H
#include "c2dtreenode.h"
#include <QString>
namespace live2dstruct {
class CPartsNode : public C2DTreeNode
{
public:
    CPartsNode(const QString& name);
    virtual ~CPartsNode();


};
}

#endif // CPARTSNODE_H
