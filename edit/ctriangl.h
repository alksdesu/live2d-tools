#ifndef CTRIANGL_H
#define CTRIANGL_H
#include <QVector2D>
#include <QVector>
#include <QRectF>
#include <vector>

class CPoint{
public:
    CPoint(const QVector2D& point);
    void SetPoint(const QVector2D& point);
    const QVector2D& GetPoint() const;
    const bool GetIsSelect() const;
    void SetIsSelect(bool select);

private:
    bool m_IsSelect;
    QVector2D point;
};

class CEdge{
public:
    CEdge(int start,int end);
    int start;
    int end;
    bool IsEAdd;//是额外添加的吗
};

class CTriangl
{
public:
    CTriangl(int a,int b,int c);
    int pointA;
    int pointB;
    int pointC;//索引
};


class CTrianglMesh{
public:
    CTrianglMesh(int ebosize,const quint16* vetebo,int uvsize,const float* vetuv,const float* vetpos);
    ~CTrianglMesh();
    CTrianglMesh(const CTrianglMesh&) = delete;
    CTrianglMesh& operator=(const CTrianglMesh&) = delete;
    void normalizing();
    bool AddPoint(const QVector2D& pos);
    bool SetPositions(const QVector<float>& positions);
    bool SetIndices(const QVector<quint16>& indices);
    bool SelectNearestPoint(const QVector2D& position, float radius);
    void MoveSelected(const QVector2D& delta);
    QVector<float> Positions() const;
    void DeleteEdge(int satat,int end);
    const quint16* GetEBOIndex() const;
    const int GetEBOSize()const ;
    const float* GetUV() const;
    const float* GetPos() const ;
    const std::vector<CPoint*>& GetWolrd() const;
    const std::vector<CEdge*>& GetEdge() const;
    std::vector<CPoint*>& GetCpoint();
    const QRectF& GetRectF() const{
        return m_meshRect;
    }
    const QRectF& GetUvRectF() const{
        return m_UvMeshRect;
    }
    bool GetContentPoint(const QPointF& point) const;
private:
    bool TriangleWeights(const QPointF& position, const CTriangl& triangle, double (&weights)[3]) const;
    void RebuildEdges();

    std::vector<CEdge*> vetEdge;//1号边有点  0 ，1
    std::vector<CPoint*> vetPosPoint;//0号点的位置，1号点的位置  世界坐标系的位置
    std::vector<QVector2D*> vetTexPoint;//纹理位置点
    std::vector<CTriangl*> vetTriangl;//一系列组合三角形


    std::vector<quint16> vetebo;    //这个通过计算获取
    std::vector<float> vetuv;
    std::vector<float> vetpos;
    QRectF m_meshRect;
    QRectF m_UvMeshRect;
};




#endif // CTRIANGL_H
