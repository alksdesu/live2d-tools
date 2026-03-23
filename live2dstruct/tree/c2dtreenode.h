#ifndef C2DTREENODE_H
#define C2DTREENODE_H
#include <QString>
#include <QList>
#include <QDebug>
// 析构过程
// 当一个对象被销毁时，C++的析构过程遵循以下步骤：
// 调用派生类的析构函数：首先调用最派生类的析构函数（即对象实际类型的析构函数）。
// 调用基类的析构函数：然后逐级向上调用基类的析构函数，直到基类对象的析构函数被调用。
// 释放内存：如果对象是动态分配的，那么在所有析构函数执行完毕后，内存会被释放。
namespace live2dstruct {
class C2DTreeNode
{

public:
    C2DTreeNode(const QString& name);
    //虚析构函数析构过程
    virtual ~C2DTreeNode();


    void AddFather(C2DTreeNode* node){
        father.push_back(node);
    }
    void AddChild(C2DTreeNode* node){
        child.push_back(node);
    }

    const QString& getName() const{
        return  name;
    }

    QList<C2DTreeNode*>& getChilds() {
        return child;
    }
    QList<C2DTreeNode*>& getFathers() {
        return father;
    }

    const QList<C2DTreeNode*>& getChilds() const {
        return child;
    }
    const QList<C2DTreeNode*>& getFathers() const {
        return father;
    }

private:
    QString name;
    QList<C2DTreeNode*> father;
    QList<C2DTreeNode*> child;
    QString type;





};
}
#endif // C2DTREENODE_H
