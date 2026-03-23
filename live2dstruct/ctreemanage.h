#ifndef CTREEMANAGE_H
#define CTREEMANAGE_H
#include "live2dstruct/tree/c2dtree.h"

namespace live2dstruct {
class CTreeManage
{
public:
    static C2DTree* GetInstance();
    static void ReleaseInstance();
    static void InitOpenLive2d(const QString& name);


private:
    CTreeManage();
    static void Init();
    static C2DTree* m_Tree;
    static QString InitName;

};
}
#endif // CTREEMANAGE_H
