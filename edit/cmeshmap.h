#ifndef CMESHMAP_H
#define CMESHMAP_H
#include <QString>
#include <QVector>
#include "cdrawmesh.h"

class CMeshMap
{
public:
    CMeshMap();

    void insert(const QString& name,CDrawMesh* drawMesh){
        m_vetDrawMesh.insert(name,drawMesh);
    }
    void SetRenderOrder(const QStringList& names);
    bool SetMeshPositions(const QString& name, const QVector<float>& positions);
    bool SetMeshIndices(const QString& name, const QVector<quint16>& indices);
    bool SelectNearestPoint(const QString& name, const QVector2D& position, float radius);
    bool MoveSelected(const QString& name, const QVector2D& delta);
    QVector<float> MeshPositions(const QString& name) const;
    void clear();
    void DrawMesh(const QString& drawKey,const QMatrix4x4 &mvp);
    void SetShow(const QString& artName);
    void SetShowMultiple(const QStringList& artNames);
    void HideAll();
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
