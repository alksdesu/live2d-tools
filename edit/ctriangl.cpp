#include "ctriangl.h"



CPoint::CPoint(const QVector2D &point)
{
    m_IsSelect = false;
    this->point = point;
}

void CPoint::SetPoint(const QVector2D &point)
{
    this->point = point;
}
const QVector2D &CPoint::GetPoint() const
{
    return point;
}

const bool CPoint::GetIsSelect() const
{
    return m_IsSelect;
}

void CPoint::SetIsSelect(bool select)
{
    m_IsSelect = select;
}

CEdge::CEdge(int start, int end)
{
    this->start = start;
    this->end = end;
    IsEAdd = false;
}

CTriangl::CTriangl(int a, int b, int c)
{
    pointA = a;
    pointB = b;
    pointC = c;
}


CTrianglMesh::CTrianglMesh(int ebosize, const short *vetebo, int uvsize, const float *vetuv, const float *vetpos)
{
    for(int i = 0 ;i < ebosize;i+=3){
        vetTriangl.push_back(new CTriangl(vetebo[i],vetebo[i+1],vetebo[i+2]) );
        vetEdge.push_back(new CEdge(vetebo[i],vetebo[i+1]));
        vetEdge.push_back(new CEdge(vetebo[i],vetebo[i+2]));
        vetEdge.push_back(new CEdge(vetebo[i+1],vetebo[i+2]));
    }

    for(int i = 0 ;i < uvsize;i+=2){
        vetPosPoint.push_back(new CPoint(  QVector2D(vetpos[i],vetpos[i+1]) ));
        vetTexPoint.push_back(new QVector2D(vetuv[i],vetuv[i+1]));
    }
    normalizing();


}

CTrianglMesh::~CTrianglMesh()
{
    qDebug() << "CTrianglMesh::~CTrianglMesh()";
    for(int i = 0 ;i < vetEdge.size();i++){
        delete vetEdge[i];
    }
    for(int i = 0 ;i < vetPosPoint.size();i++){
        delete vetPosPoint[i];
    }

    for(int i = 0 ;i < vetTexPoint.size();i++){
        delete vetTexPoint[i];
    }
    for(int i = 0 ;i < vetTriangl.size();i++){
        delete vetTriangl[i];
    }

}

void CTrianglMesh::normalizing()
{
    vetebo.clear();
    vetuv.clear();
    vetpos.clear();

    for(int i = 0 ;i < vetTriangl.size();i++){
        vetebo.push_back(vetTriangl[i]->pointA);
        vetebo.push_back(vetTriangl[i]->pointB);
        vetebo.push_back(vetTriangl[i]->pointC);
    }

    std::vector<cv::Point2f> cvpoints;


    QPolygonF poly;
    QPolygonF uvpoly;
    for(int i = 0 ;i < vetPosPoint.size() && i < vetTexPoint.size();i++){
        vetuv.push_back(vetTexPoint[i]->x());
        vetuv.push_back(vetTexPoint[i]->y());
        uvpoly.push_back(QPointF(vetTexPoint[i]->x(),1.0f -vetTexPoint[i]->y()));
        poly.push_back(QPointF(vetPosPoint[i]->GetPoint().x(),vetPosPoint[i]->GetPoint().y()));

        cvpoints.push_back(cv::Point2f(vetPosPoint[i]->GetPoint().x(), vetPosPoint[i]->GetPoint().y()));
        vetpos.push_back(vetPosPoint[i]->GetPoint().x());
        vetpos.push_back(vetPosPoint[i]->GetPoint().y());
    }
    m_meshRect = poly.boundingRect();
    m_UvMeshRect = uvpoly.boundingRect();

    m_poly.clear();
    if(cvpoints.size() > 0){
        std::vector<cv::Point2f> hull;
        cv::approxPolyDP(cvpoints,hull,0,true);
        for(int  i = 0 ;i < hull.size();i++){
            m_poly.push_back(QPointF(hull[i].x,hull[i].y)) ;
        }
    }

    //m_poly = poly;
}

bool CTrianglMesh::AddPoint(const QVector2D &pos)
{
    //暂时只添加在三角形内部的点
    bool result = false;
    for(int i = 0 ;i < vetTriangl.size() ;i++){
        CPoint* pointA = vetPosPoint[vetTriangl[i]->pointA];
        CPoint* pointB = vetPosPoint[vetTriangl[i]->pointB];
        CPoint* pointC = vetPosPoint[vetTriangl[i]->pointC];

        result = isPointInTriangle(pos, pointA->GetPoint(), pointB->GetPoint(), pointC->GetPoint());
        if(!result){//不在三角形内部，就continue
            continue;
        }

        //添加一个pos点，三条边,一个纹理点,三个三角形
        //删除这个三角形
        //删除三条边
        vetPosPoint.push_back(new  CPoint(pos));
        int index = vetPosPoint.size() - 1;
        vetEdge.push_back(new  CEdge(vetTriangl[i]->pointA,index));
        vetEdge.push_back(new  CEdge(vetTriangl[i]->pointB,index));
        vetEdge.push_back(new  CEdge(vetTriangl[i]->pointC,index));

        QVector2D TexPoint = GetNewTexPoint(pos,*vetTriangl[i]);
        qDebug() << TexPoint;
        vetTexPoint.push_back(new QVector2D(TexPoint));


        vetTriangl.push_back(new  CTriangl(index,vetTriangl[i]->pointA,vetTriangl[i]->pointB));
        vetTriangl.push_back(new  CTriangl(index,vetTriangl[i]->pointA,vetTriangl[i]->pointC));
        vetTriangl.push_back(new  CTriangl(index,vetTriangl[i]->pointB,vetTriangl[i]->pointC));



        auto it = vetTriangl.begin() + i;
        // 使用erase函数删除元素
        delete (*it);
        vetTriangl.erase(it);


        if(result){
            qDebug() << "CMesh::AddPoint 点在三角形内部";
            break;//
        }

    }

    return result;
}

void CTrianglMesh::DeleteEdge(int satat, int end)
{

}

const short * CTrianglMesh::GetEBOIndex() const
{
    return vetebo.data();
}

const int CTrianglMesh::GetEBOSize() const
{
    return vetebo.size();
}

const float * CTrianglMesh::GetUV() const
{
    return vetuv.data();
}

const float * CTrianglMesh::GetPos() const
{
    return vetpos.data();
}

const std::vector<CPoint*> & CTrianglMesh::GetWolrd() const
{
    return vetPosPoint;
}

const std::vector<CEdge*> & CTrianglMesh::GetEdge() const
{
    return vetEdge;
}

std::vector<CPoint*> & CTrianglMesh::GetCpoint()
{
    return vetPosPoint;
}

QVector2D CTrianglMesh::GetNewTexPoint(const QVector2D &pos, const CTriangl &tri)
{
    CPoint* pointA = vetPosPoint[tri.pointA];
    CPoint* pointB = vetPosPoint[tri.pointB];
    CPoint* pointC = vetPosPoint[tri.pointC];

    QVector2D* texA = vetTexPoint[tri.pointA];
    QVector2D* texB = vetTexPoint[tri.pointB];
    QVector2D* texC = vetTexPoint[tri.pointC];

    QVector2D result;

    cv::Point2f mop1[3];
    cv::Point2f mop2[3];
    // // 初始化点坐标
    mop1[0] = cv::Point2f(pointA->GetPoint().x(), pointA->GetPoint().y());
    mop1[1] = cv::Point2f(pointB->GetPoint().x(), pointB->GetPoint().y());
    mop1[2] = cv::Point2f(pointC->GetPoint().x(), pointC->GetPoint().y());


    mop2[0] = cv::Point2f(texA->x(), texA->y());
    mop2[1] = cv::Point2f(texB->x(), texB->y());
    mop2[2] = cv::Point2f(texC->x(), texC->y());


    cv::Mat mat = getAffineTransform(mop1, mop2);

    if (mat.type() == CV_64FC1) {

        QMatrix4x4 data = cvMatToQMatrix4x4(mat);

        QVector4D out = data * QVector4D(pos,0,1.0f);
        result = QVector2D(out.x(),out.y()  );

    } else {
        qDebug() << "矩阵数据类型不正确，无法按预期访问元素" ;
    }


    return result;
}

float CTrianglMesh::crossProduct(const QVector2D &a, const QVector2D &b)
{
    return a.x() * b.y() - a.y() * b.x();
}

bool CTrianglMesh::isSameSide(const QVector2D &p1, const QVector2D &p2, const QVector2D &a, const QVector2D &b)
{
    QVector2D ab = b - a;
    QVector2D ap1 = p1 - a;
    QVector2D ap2 = p2 - a;

    float cross1 = crossProduct(ab, ap1);
    float cross2 = crossProduct(ab, ap2);

    return (cross1 * cross2 >= 0);  // 同号则在同侧
}

bool CTrianglMesh::isPointInTriangle(const QVector2D &p, const QVector2D &a, const QVector2D &b, const QVector2D &c)
{
    return isSameSide(p, a, b, c) &&
           isSameSide(p, b, c, a) &&
           isSameSide(p, c, a, b);
}

QMatrix4x4 CTrianglMesh::cvMatToQMatrix4x4(const cv::Mat &mat)
{
    // 确保输入的cv::Mat是2行3列
    if (mat.rows!= 2 || mat.cols!= 3)
    {
        // 如果不符合要求，可以抛出异常或者返回一个默认的QMatrix4x4
        qWarning("输入的cv::Mat矩阵维度不符合2行3列要求");
        return QMatrix4x4();
    }

    QMatrix4x4 qMatrix;
    qMatrix(0, 0) = mat.at<double>(0, 0);
    qMatrix(0, 1) = mat.at<double>(0, 1);
    qMatrix(0, 2) = 0;
    qMatrix(0, 3) = mat.at<double>(0, 2);

    qMatrix(1, 0) = mat.at<double>(1, 0);
    qMatrix(1, 1) = mat.at<double>(1, 1);
    qMatrix(1, 2) = 0;
    qMatrix(1, 3) = mat.at<double>(1, 2);

    qMatrix(2, 0) = 0;
    qMatrix(2, 1) = 0;
    qMatrix(2, 2) = 1;
    qMatrix(2, 3) = 0;

    qMatrix(3, 0) = 0;
    qMatrix(3, 1) = 0;
    qMatrix(3, 2) = 0;
    qMatrix(2, 3) = 1;


    return qMatrix;
}



