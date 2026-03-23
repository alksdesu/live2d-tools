#include "cgridtransform.h"
namespace live2dstruct {

CGridTransform::CGridTransform(QRectF rect, std::vector<float> &points, int Col, int Row)
{
    this->points = points;
    this->rect =rect ;
    this->row = Row;
    this->col = Col;
}

void CGridTransform::inverseTransform(float* var1, int var2, float* var3, int var4, int var5, bool var6)
{

    QVector2D var7 ;
    QVector2D var8 ;

    for(int var9 = 0; var9 < var5; ++var9) {
        int var10 = var2 + var9 * 2;
        var7.setX(var1[var10]);
        var7.setY(var1[var10 + 1]);
        inverseTransform(var7, var8, var6);
        int var11 = var4 + var9 * 2;
        var3[var11] = var8.x();
        var3[var11 + 1] = var8.y();
    }

}

QVector2D CGridTransform::inverseTransform(const QVector2D& var1,  QVector2D& var2, bool var3)
{
    return var3 ? inverseTransform_3_3_Quad(var1, var2) : inverseTransform_2_1(var1, var2);
}

QVector2D CGridTransform::inverseTransform_3_3_Quad(const QVector2D& src,QVector2D& dst)
{
    float var16 = src.x();
    float var5 = src.y();
    float var6[2] ;//out
    float var18[2] ;

    //先进行一次计算，在进行一次计算
    bool IsSecc = get01_ex_quad(var16, var5, nullptr, var6);
    if(IsSecc){
        getPtCRectangleGrid(var6[0], var6[1], var18);
        dst.setX(var18[0]);
        dst.setY(var18[1]);
    }
    return dst;
}

QVector2D CGridTransform::inverseTransform_2_1(QVector2D var1, QVector2D var2)
{
    return QVector2D();
}

bool CGridTransform::get01_ex_quad(float var1, float var2, QVector2D* var3, float *var4)
{
    int var5 = this->col + 1;
    QRectF var6 = this->getBoundsRect();
    if (!var6.contains(var1, var2)) {
        return false;
    } else {
        float var7 = NAN;
        float var8 = 0.0F;
        float var9 = 0.0F;
        QVector2D var10 ;
        QVector2D var11 ;
        QVector2D var12;
        QVector2D var13 ;
        QVector2D var14 = QVector2D(var1, var2);
        int var15 = 0;
        int var16 = this->row - 1;
        if (var15 <= var16) {
            while(true) {
                int var17 = 0;
                int var18 = this->col - 1;
                if (var17 <= var18) {
                    while(true) {
                        var10.setX(this->points[2 * (var17 + var15 * var5)]);
                        var10.setY(this->points[2 * (var17 + var15 * var5) + 1]);
                        var11.setX(this->points[2 * (var17 + 1 + var15 * var5)]);
                        var11.setY(this->points[2 * (var17 + 1 + var15 * var5) + 1]);
                        var12.setX(this->points[2 * (var17 + (var15 + 1) * var5)]);
                        var12.setY(this->points[2 * (var17 + (var15 + 1) * var5) + 1]);
                        var13.setX(this->points[2 * (var17 + 1 + (var15 + 1) * var5)]);
                        var13.setY(this->points[2 * (var17 + 1 + (var15 + 1) * var5) + 1]);

                        float* var19 = get01_ex_quad_Nation(var10, var11, var12, var13, var14, var4);

                        if (var19 != nullptr) {
                            var19[0] = ((float)var17 + var19[0]) / (float)this->col;
                            var19[1] = ((float)var15 + var19[1]) / (float)this->row;
                            if (var3 == nullptr) {
                                return var19;
                            }

                            float var22;
                            if (var7 == NAN) {
                                var7 = var19[0];
                                var8 = var19[1];
                                float var21 = var19[0];
                                var22 = var19[1];
                                var9 = (var3->x() - var21) * (var3->x() - var21) + (var3->y() - var22) * (var3->y() - var22);
                            } else {
                                var22 = var19[0];
                                float var25 = var19[1];
                                float var20 = (var3->x() - var22) * (var3->x() - var22) + (var3->y() - var25) * (var3->y() - var25);
                                if (var9 > var20) {
                                    var7 = var19[0];
                                    var8 = var19[1];
                                    var9 = var20;
                                }
                            }
                        }

                        if (var17 == var18) {
                            break;
                        }

                        ++var17;
                    }
                }

                if (var15 == var16) {
                    break;
                }

                ++var15;
            }
        }

        if (var7 ==NAN) {
            return false;
        } else {
            var4[0] = var7;
            var4[1] = var8;
            return true;
        }
    }

}

float *CGridTransform::get01_ex_quad_Nation(QVector2D var1, QVector2D var2, QVector2D var3, QVector2D var4, QVector2D var5, float *var6)
{
    float b =  1.0E-6F;//常量

    QVector2D var10 = var3 - var5;
    QVector2D var11 = var3 - var1;

    float var13 = var10.x();

    double var10000 = (double)var13;
    var13 = var11.y();

    var10000 *= (double)var13;
    var13 = var10.y();

    double var10001 = (double)var13;
    var13 = var11.x();

    double var7 = var10000 - var10001 * (double)var13;
    QVector2D var38 = var3 - var5;
    QVector2D var39 = var4 - var2;

    float var15 = var38.x();

    var10000 = (double)var15;
    var15 = var39.y();

    var10000 *= (double)var15;
    var15 = var38.y();

    var10001 = (double)var15;
    var15 = var39.x();

    var10000 -= var10001 * (double)var15;

    var38 = var4 - var5;
    var39 = var3 - var1;

    var15 = var38.x();

    var10001 = (double)var15;
    var15 = var39.y();

    var10001 *= (double)var15;
    var15 = var38.y();

    double var10002 = (double)var15;
    var15 = var39.x();

    double var35 = var10000 + (var10001 - var10002 * (double)var15);
    double var37 = var35 / 2.0;

    QVector2D var44 = var4 - var5;
    QVector2D var17 = var4 - var2;

    float var19 = var44.x();

    var10000 = (double)var19;
    var19 = var17.y();

    var10000 *= (double)var19;
    var19 = var44.y();

    var10001 = (double)var19;
    var19 = var17.x();

    double var40 = var10000 - var10001 * (double)var19;
    double var43 = var7 - var35 + var40;
    double var41 = abs(var43);
    double var45 = var37 * var37 - var7 * var40;
    double var21 = NAN;
    double var23 = 0.0;
    double var25 = 1.0E-6;
    bool var33;
    bool var34;
    if (!(var41 < var25) && !(var45 < 0.0)) {
        double var27 = sqrt(var45);
        double var29 = var7 - var37 + var27;
        double var31 = var7 - var37 - var27;
        var33 = var43 * var29 >= 0.0 && var41 >= abs(var29);
        var34 = var43 * var31 >= 0.0 && var41 >= abs(var31);
        if (!var33 && var34) {
            var21 = var31 / var43;
        } else {
            var21 = var29 / var43;
        }
    } else {
        var21 = var7 / (var7 - var40);
    }

    if (var21 == NAN) {
        var21 = var7 / (var7 - var40);
    }

    double var46[2] ;
    QVector2D var47 = var3 - var1;
    QVector2D var30 = var4 - var2;
    bool var51 = false;
    float var32 = var47.x();
    var33 = false;
    var10002 = (double)var32;
    var32 = var30.x();
    var33 = false;
    double var10003 = (double)var32;
    var32 = var47.x();
    var33 = false;
    var46[0] = var10002 + (var10003 - (double)var32) * var21;
    var32 = var47.y();
    var33 = false;
    var10002 = (double)var32;
    var32 = var30.y();
    var33 = false;
    var10003 = (double)var32;
    var32 = var47.y();
    var33 = false;
    var46[1] = var10002 + (var10003 - (double)var32) * var21;
    if (var46[0] == 0.0 && var46[1] == 0.0) {
        return nullptr;
    } else {

        bool var54;
    label132: {
        double var28[2] ;
        var30 = var3 - var5;
        QVector2D var52 = var4 - var5;
        var54 = false;
        float var56 = var30.x();
        var34 = false;
        var10002 = (double)var56;
        var56 = var52.x();
        var34 = false;
        var10003 = (double)var56;
        var56 = var30.x();
        var34 = false;
        var28[0] = var10002 + (var10003 - (double)var56) * var21;
        var56 = var30.y();
        var34 = false;
        var10002 = (double)var56;
        var56 = var52.y();
        var34 = false;
        var10003 = (double)var56;
        var56 = var30.y();
        var34 = false;
        var28[1] = var10002 + (var10003 - (double)var56) * var21;
        if (var46[1] == 0.0) {

            double var49 = var28[0];
            double var55 = 0.0;
            var34 = false;
            if (!(var49 - var25 <= var55) || !(var55 <= var49 + var25)) {
                if (var46[0] == 0.0) {
                    return nullptr;
                }

                var23 = 1.0 - var28[0] / var46[0];
                goto label132;
            }
        }

        if (var46[1] == 0.0) {
            return nullptr;
        }

        var23 = 1.0 - var28[1] / var46[1];
    }

        if (var23 == NAN) {
            return nullptr;
        } else if ((double)(0.0F - b) < var21 && var21 < (double)(1.0F + b) && (double)(0.0F - b) < var23 && var23 < (double)(1.0F + b)) {

            var54 = false;
            float var50 = (float)var21;
            float var53 = 0.0F;
            var32 = 1.0F;
            var33 = false;
            var6[0] = var50 < var53 ? var53 : (var50 > var32 ? var32 : var50);

            var54 = false;
            var50 = (float)var23;
            var53 = 0.0F;
            var32 = 1.0F;
            var33 = false;
            var6[1] = var50 < var53 ? var53 : (var50 > var32 ? var32 : var50);
            return var6;
        } else {
            return nullptr;
        }
    }
    return nullptr;
}

QRectF CGridTransform::getBoundsRect()
{
    int var1 = this->points.size();


    float var2 = 0;
    float var3 = 0;
    float var4 = 0;
    float var5 = 0;

    if(var1 > 1){

        var2 = this->points[0];
        var3 = this->points[1];
        var4 = this->points[0];
        var5 = this->points[1];
    }

    for(int var6 = 0; var6 < var1; var6 += 2) {
        float var7 = this->points[var6];
        float var8 = this->points[var6 + 1];
        if (var7 > var4) {
            var4 = var7;
        }

        if (var8 > var5) {
            var5 = var8;
        }

        if (var7 < var2) {
            var2 = var7;
        }
        if (var8 < var3) {
            var3 = var8;
        }
    }

    return  QRectF(var2, var3, var4 - var2, var5 - var3);
}

void CGridTransform::getPtCRectangleGrid(float var1, float var2, float *var3)
{

    var3[0] = this->rect.x() + this->rect.width() * var1;
    var3[1] = this->rect.y() + this->rect.height() * var2;

}

float *CGridTransform::getPt(float var1, float var2, float *var3)
{
    int var4 = (int)(var1 * (float)this->col);
    int var5 = (int)(var2 * (float)this->row);
    if (var4 < 0) {
        var4 = 0;
    } else if (var4 > this->col - 1) {
        var4 = this->col - 1;
    }

    if (var5 < 0) {
        var5 = 0;
    } else if (var5 > this->row - 1) {
        var5 = this->row - 1;
    }

    float var6 = var1 * (float)this->col - (float)var4;
    if (var6 < 0.0F) {
        var6 = 0.0F;
    } else if (var6 > 1.0F) {
        var6 = 1.0F;
    }

    float var7 = var2 * (float)this->row - (float)var5;
    if (var7 < 0.0F) {
        var7 = 0.0F;
    } else if (var7 > 1.0F) {
        var7 = 1.0F;
    }

    int var8 = this->col + 1;
    float var9;
    float var10;
    float var11;
    float var12;
    float var13;
    float var14;
    if (var6 + var7 < 1.0F) {
        var9 = this->points[2 * (var4 + var5 * var8)];
        var10 = this->points[2 * (var4 + var5 * var8) + 1];
        var11 = this->points[2 * (var4 + 1 + var5 * var8)];
        var12 = this->points[2 * (var4 + 1 + var5 * var8) + 1];
        var13 = this->points[2 * (var4 + (var5 + 1) * var8)];
        var14 = this->points[2 * (var4 + (var5 + 1) * var8) + 1];
        var3[0] = var9 + var6 * (var11 - var9) + var7 * (var13 - var9);
        var3[1] = var10 + var6 * (var12 - var10) + var7 * (var14 - var10);
    } else {
        var9 = this->points[2 * (var4 + 1 + var5 * var8)];
        var10 = this->points[2 * (var4 + 1 + var5 * var8) + 1];
        var11 = this->points[2 * (var4 + (var5 + 1) * var8)];
        var12 = this->points[2 * (var4 + (var5 + 1) * var8) + 1];
        var13 = this->points[2 * (var4 + 1 + (var5 + 1) * var8)];
        var14 = this->points[2 * (var4 + 1 + (var5 + 1) * var8) + 1];
        var3[0] = var13 + (1.0F - var6) * (var11 - var13) + (1.0F - var7) * (var9 - var13);
        var3[1] = var14 + (1.0F - var6) * (var12 - var14) + (1.0F - var7) * (var10 - var14);
    }

    return var3;

}
}
