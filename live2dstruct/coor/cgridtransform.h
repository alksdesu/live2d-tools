#ifndef CGRIDTRANSFORM_H
#define CGRIDTRANSFORM_H
#include <QTransform>
#include <QVector2D>
#include <vector>
#include <cmath>

namespace live2dstruct {
class CGridTransform :public QTransform
{
public:

    CGridTransform(QRectF rect,std::vector<float>& points, int Col,int Row);
    void inverseTransform(float* var1, int var2,  float* var3, int var4, int var5, bool var6);


    QVector2D inverseTransform(const QVector2D& var1,QVector2D& var2, bool var3);

    QVector2D inverseTransform_3_3_Quad(const QVector2D& src, QVector2D& dst);


    QVector2D inverseTransform_2_1( QVector2D var1,  QVector2D var2) ;

    bool  get01_ex_quad(float var1, float var2,  QVector2D* var3,  float* var4);
    float* get01_ex_quad_Nation(QVector2D var1,  QVector2D var2, QVector2D var3,  QVector2D var4, QVector2D var5,  float* var6) ;


    float* getPt(float var1, float var2, float* var3);

    void getPtCRectangleGrid(float var1, float var2,  float* var3) ;
    QRectF getBoundsRect() ;
private:
    int row = -1;
    int col = -1;
    std::vector<float> points;
    QRectF rect;
};
}
#endif // CGRIDTRANSFORM_H
