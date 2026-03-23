#include "c2dtreenode.h"
namespace live2dstruct {

C2DTreeNode::C2DTreeNode(const QString &name)
{

    this->name = name;
}

C2DTreeNode::~C2DTreeNode()
{
    qDebug() << "C2DTreeNode::~C2DTreeNode()  " << this->name;
    // for(int i = 0 ;i < child.size();i++){
    //     if(child[i] != nullptr){
    //         delete child[i];
    //         child[i] = nullptr;
    //     }
    // }
    //child.clear();

}
}
