#include "cselectstruct.h"

CSelectStruct::CSelectStruct() {}

void CSelectStruct::AddSelect(const QString &name, bool select)
{
    m_select.insert(name,select);
    m_sort.push_back(name);



}

void CSelectStruct::Clear()
{
    m_select.clear();//查找位置
    m_sort.clear();//放置位置

}
