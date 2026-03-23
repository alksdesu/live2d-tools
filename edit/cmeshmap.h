#ifndef CMESHMAP_H
#define CMESHMAP_H
#include <QString>
#include "cdrawmesh.h"

class CMeshMap
{
public:
    CMeshMap();

    void insert(const QString& name,CDrawMesh* drawMesh){
        m_vetDrawMesh.insert(name,drawMesh);
    }
    void insertReaderSort(const QString& name){
        m_readerSort.push_back(name);
    };
    void clear();
    void DrawMesh(const QString& drawKey,const QMatrix4x4 &mvp);
    void SetShow(const QString& artName);
    QString SetOver(const QPointF& point,bool IsOver);

    CDrawMesh* GetDrawMesh(const QString& name){
        return m_vetDrawMesh[name];
    }

    QMap<QString,CDrawMesh*>& GetMapMesh(){
        return m_vetDrawMesh;
    }
private:
    QMap<QString,CDrawMesh*>  m_vetDrawMesh;
    QVector<QString> m_readerSort;//绘制顺序
    QMap<QString,int> m_vetImage;//这个块在那个图层中

};

#endif // CMESHMAP_H
