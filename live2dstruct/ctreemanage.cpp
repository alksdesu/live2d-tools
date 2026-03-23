#include "ctreemanage.h"
namespace live2dstruct {
C2DTree* CTreeManage::m_Tree = nullptr;
QString  CTreeManage::InitName = "";
C2DTree* CTreeManage::GetInstance()
{
    Init();
    return m_Tree;
}

void CTreeManage::ReleaseInstance()
{
    delete m_Tree;
    m_Tree = nullptr;
}

void CTreeManage::InitOpenLive2d(const QString &name)
{
    InitName = name;
    if(m_Tree != nullptr){
        delete m_Tree;
        m_Tree = nullptr;
    }
    Init();

}

CTreeManage::CTreeManage()
{

}

void CTreeManage::Init()
{
    if(m_Tree == nullptr){
        CReader* reader =  new  CReader(InitName);
        CSectionTable* sectionTable = new CSectionTable(*reader);
        m_Tree = new C2DTree(*sectionTable);
        delete reader;
        delete sectionTable;
    }
}
}
