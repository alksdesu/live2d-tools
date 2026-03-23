#ifndef CSELECTSTRUCT_H
#define CSELECTSTRUCT_H

#include <QVector>
#include <QMap>
class CSelectStruct
{
public:
    CSelectStruct();
    //按顺序添加select
    void AddSelect(const QString& name,bool select);
    void Clear();
    const int Size() const{
        return m_sort.size();
    }
    const QString& GetName(int index) const{
        return m_sort[index];
    }
    bool IsSelect(const QString& name) const{
        return m_select[name];
    }



private:
    QMap<QString,bool> m_select;//查找位置
    QVector<QString> m_sort;//放置位置

};

#endif // CSELECTSTRUCT_H
