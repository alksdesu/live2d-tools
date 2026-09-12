#ifndef CTRIANGL_H
#define CTRIANGL_H
#include <QVector2D>
#include <QVector>
#include <opencv2/opencv.hpp>
#include <QOpenGLFunctions_3_3_Core>
#include <QMatrix4x4>
#include <QPolygonF>

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
    CTrianglMesh(int ebosize,const short* vetebo,int uvsize,const float* vetuv,const float* vetpos);
    ~CTrianglMesh();
    void normalizing();
    bool AddPoint(const QVector2D& pos);
    bool SetPositions(const QVector<float>& positions);
    void DeleteEdge(int satat,int end);
    const short* GetEBOIndex() const;
    const int GetEBOSize()const ;
    const float* GetUV() const;
    const float* GetPos() const ;
    const std::vector<CPoint*>& GetWolrd() const;
    const std::vector<CEdge*>& GetEdge() const;
    std::vector<CPoint*>& GetCpoint();
    const QRectF& GetRectF(){
        return m_meshRect;
    }
    const QRectF& GetUvRectF(){
        return m_UvMeshRect;
    }
    bool GetContentPoint(const QPointF& point){
        return m_poly.containsPoint(point,Qt::WindingFill);
    }
private:

    QMatrix4x4 cvMatToQMatrix4x4(const cv::Mat& mat);

    QVector2D GetNewTexPoint(const QVector2D& pos,const CTriangl& tri);//
    float crossProduct(const QVector2D& a, const QVector2D& b);
    bool isSameSide(const QVector2D& p1, const QVector2D& p2, const QVector2D& a, const QVector2D& b);
    bool isPointInTriangle(const QVector2D& p, const QVector2D& a, const QVector2D& b, const QVector2D& c);

    std::vector<CEdge*> vetEdge;//1号边有点  0 ，1
    std::vector<CPoint*> vetPosPoint;//0号点的位置，1号点的位置  世界坐标系的位置
    std::vector<QVector2D*> vetTexPoint;//纹理位置点
    std::vector<CTriangl*> vetTriangl;//一系列组合三角形


    std::vector<short> vetebo;    //这个通过计算获取
    std::vector<float> vetuv;
    std::vector<float> vetpos;
    QRectF m_meshRect;
    QRectF m_UvMeshRect;
    QPolygonF m_poly;
};




#endif // CTRIANGL_H
