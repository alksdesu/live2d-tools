#include "cwarpcoortype.h"

namespace live2dstruct {
CWarpCoorType::CWarpCoorType(int Col, int Row, std::vector<float> &positions, bool isQuadTransform)
{
    this->Col = Col;
    this->Row = Row;
    this->Positions = positions;
    this->isQuadTransform = isQuadTransform;
}

CWarpCoorType::~CWarpCoorType()
{

}

int CWarpCoorType::GetCol()
{
    return Col;
}

int CWarpCoorType::GetRow()
{
    return Row;
}

std::vector<float>& CWarpCoorType::GetPositions()
{
    return Positions;
}

bool CWarpCoorType::GetIsQuadTransform()
{
    return isQuadTransform;
}


void CWarpCoorType::transformLocalToCanvas(float* src, float* des, int vetSize, int offset, int step)
{
    int col = GetCol();
    int row = GetRow();
    std::vector<float>& positions = GetPositions();
    if(positions.size() > 0){
        transformLocalToCanvas_nation(src, des, vetSize, offset, step, positions, col, row, GetIsQuadTransform());

    }
}

void CWarpCoorType::transformLocalToCanvas_nation(float* src, float* des, int vetNum, int offset, int step, std::vector<float>& positions, int col, int row, bool IsQuad)
{
    int length = vetNum * step;
    float var12 = 0.0F;
    float var13 = 0.0F;
    float var14 = 0.0F;
    float var15 = 0.0F;
    float var16 = 0.0F;
    float var17 = 0.0F;
    float var18 = 0.0F;
    float var19 = 0.0F;
    bool var20 = false;

    for(int i = offset; i < length; i += step) {
        float var22 = 0.0F;
        float var23 = 0.0F;
        float var24 = 0.0F;
        float var25 = 0.0F;
        var24 = src[i];
        var25 = src[i + 1];
        var22 = var24 * (float)col;
        var23 = var25 * (float)row;
        float var27;
        if (!(var22 < 0.0F) && !(var23 < 0.0F) && !((float)col <= var22) && !((float)row <= var23)) {
            var12 = var22 - (float)((int)var22);
            var13 = var23 - (float)((int)var23);
            int var40 = 2 * ((int)var22 + (int)var23 * (col + 1));
            if (IsQuad) {
                float var41 = 1.0F - var12;
                var27 = 1.0F - var13;
                des[i] = positions[var40] * var41 * var27 + positions[var40 + 2] * var12 * var27 + positions[var40 + 2 * (col + 1)] * var41 * var13 + positions[var40 + 2 * (col + 1) + 2] * var12 * var13;
                des[i + 1] = positions[var40 + 1] * var41 * var27 + positions[var40 + 3] * var12 * var27 + positions[var40 + 2 * (col + 1) + 1] * var41 * var13 + positions[var40 + 2 * (col + 1) + 3] * var12 * var13;
            } else if (var12 + var13 < 1.0F) {
                des[i] = positions[var40] * (1.0F - var12 - var13) + positions[var40 + 2] * var12 + positions[var40 + 2 * (col + 1)] * var13;
                des[i + 1] = positions[var40 + 1] * (1.0F - var12 - var13) + positions[var40 + 3] * var12 + positions[var40 + 2 * (col + 1) + 1] * var13;
            } else {
                des[i] = positions[var40 + 2 * (col + 1) + 2] * (var12 - 1.0F + var13) + positions[var40 + 2 * (col + 1)] * (1.0F - var12) + positions[var40 + 2] * (1.0F - var13);
                des[i + 1] = positions[var40 + 2 * (col + 1) + 3] * (var12 - 1.0F + var13) + positions[var40 + 2 * (col + 1) + 1] * (1.0F - var12) + positions[var40 + 3] * (1.0F - var13);
            }
        } else {
            int var26 = col + 1;
            float var28;
            float var29;
            float var30;
            if (!var20) {
                var20 = true;
                var14 = 0.25F * (positions[(0 + 0 * var26) * 2] + positions[(col + 0 * var26) * 2] + positions[(0 + row * var26) * 2] + positions[(col + row * var26) * 2]);
                var15 = 0.25F * (positions[(0 + 0 * var26) * 2 + 1] + positions[(col + 0 * var26) * 2 + 1] + positions[(0 + row * var26) * 2 + 1] + positions[(col + row * var26) * 2 + 1]);
                var27 = positions[(col + row * var26) * 2] - positions[(0 + 0 * var26) * 2];
                var28 = positions[(col + row * var26) * 2 + 1] - positions[(0 + 0 * var26) * 2 + 1];
                var29 = positions[(col + 0 * var26) * 2] - positions[(0 + row * var26) * 2];
                var30 = positions[(col + 0 * var26) * 2 + 1] - positions[(0 + row * var26) * 2 + 1];
                var16 = (var27 + var29) * 0.5F;
                var17 = (var28 + var30) * 0.5F;
                var18 = (var27 - var29) * 0.5F;
                var19 = (var28 - var30) * 0.5F;
                if (var16 == 0.0F && var17 == 0.0F) {
                }

                if (var18 == 0.0F && var19 == 0.0F) {
                }

                var14 -= 0.5F * (var16 + var18);
                var15 -= 0.5F * (var17 + var19);
            }

            if (-2.0F < var24 && var24 < 3.0F && -2.0F < var25 && var25 < 3.0F) {
                float var31;
                float var32;
                float var33;
                float var34;
                float var35;
                float var36;
                float var37;
                float var38;
                float var39;
                int var42;
                if (var24 <= 0.0F) {
                    if (var25 <= 0.0F) {
                        var27 = positions[(0 + 0 * var26) * 2];
                        var28 = positions[(0 + 0 * var26) * 2 + 1];
                        var29 = var14 - 2.0F * var16;
                        var30 = var15 - 2.0F * var17;
                        var31 = var14 - 2.0F * var18;
                        var32 = var15 - 2.0F * var19;
                        var33 = var14 - 2.0F * var16 - 2.0F * var18;
                        var34 = var15 - 2.0F * var17 - 2.0F * var19;
                        var35 = 0.5F * (var24 - (float)-2);
                        var36 = 0.5F * (var25 - (float)-2);
                        if (var35 + var36 <= 1.0F) {
                            des[i] = var33 + (var31 - var33) * var35 + (var29 - var33) * var36;
                            des[i + 1] = var34 + (var32 - var34) * var35 + (var30 - var34) * var36;
                        } else {
                            des[i] = var27 + (var29 - var27) * ((float)1 - var35) + (var31 - var27) * ((float)1 - var36);
                            des[i + 1] = var28 + (var30 - var28) * ((float)1 - var35) + (var32 - var28) * ((float)1 - var36);
                        }
                    } else if (var25 >= 1.0F) {
                        var27 = positions[(0 + row * var26) * 2];
                        var28 = positions[(0 + row * var26) * 2 + 1];
                        var29 = var14 - 2.0F * var16 + 1.0F * var18;
                        var30 = var15 - 2.0F * var17 + 1.0F * var19;
                        var31 = var14 + 3.0F * var18;
                        var32 = var15 + 3.0F * var19;
                        var33 = var14 - 2.0F * var16 + 3.0F * var18;
                        var34 = var15 - 2.0F * var17 + 3.0F * var19;
                        var35 = 0.5F * (var24 - (float)-2);
                        var36 = 0.5F * (var25 - (float)1);
                        if (var35 + var36 <= 1.0F) {
                            des[i] = var29 + (var27 - var29) * var35 + (var33 - var29) * var36;
                            des[i + 1] = var30 + (var28 - var30) * var35 + (var34 - var30) * var36;
                        } else {
                            des[i] = var31 + (var33 - var31) * ((float)1 - var35) + (var27 - var31) * ((float)1 - var36);
                            des[i + 1] = var32 + (var34 - var32) * ((float)1 - var35) + (var28 - var32) * ((float)1 - var36);
                        }
                    } else {
                        var42 = (int)var23;
                        if (var42 == row) {
                            var42 = row - 1;
                        }

                        var28 = 0.5F * (var24 - (float)-2);
                        var29 = var23 - (float)var42;
                        var30 = (float)var42 / (float)row;
                        var31 = (float)(var42 + 1) / (float)row;
                        var32 = positions[(0 + var42 * var26) * 2];
                        var33 = positions[(0 + var42 * var26) * 2 + 1];
                        var34 = positions[(0 + (var42 + 1) * var26) * 2];
                        var35 = positions[(0 + (var42 + 1) * var26) * 2 + 1];
                        var36 = var14 - 2.0F * var16 + var30 * var18;
                        var37 = var15 - 2.0F * var17 + var30 * var19;
                        var38 = var14 - 2.0F * var16 + var31 * var18;
                        var39 = var15 - 2.0F * var17 + var31 * var19;
                        if (var28 + var29 <= 1.0F) {
                            des[i] = var36 + (var32 - var36) * var28 + (var38 - var36) * var29;
                            des[i + 1] = var37 + (var33 - var37) * var28 + (var39 - var37) * var29;
                        } else {
                            des[i] = var34 + (var38 - var34) * ((float)1 - var28) + (var32 - var34) * ((float)1 - var29);
                            des[i + 1] = var35 + (var39 - var35) * ((float)1 - var28) + (var33 - var35) * ((float)1 - var29);
                        }
                    }
                } else if (1.0F <= var24) {
                    if (var25 <= 0.0F) {
                        var27 = positions[(col + 0 * var26) * 2];
                        var28 = positions[(col + 0 * var26) * 2 + 1];
                        var29 = var14 + 3.0F * var16;
                        var30 = var15 + 3.0F * var17;
                        var31 = var14 + 1.0F * var16 - 2.0F * var18;
                        var32 = var15 + 1.0F * var17 - 2.0F * var19;
                        var33 = var14 + 3.0F * var16 - 2.0F * var18;
                        var34 = var15 + 3.0F * var17 - 2.0F * var19;
                        var35 = 0.5F * (var24 - (float)1);
                        var36 = 0.5F * (var25 - (float)-2);
                        if (var35 + var36 <= 1.0F) {
                            des[i] = var31 + (var33 - var31) * var35 + (var27 - var31) * var36;
                            des[i + 1] = var32 + (var34 - var32) * var35 + (var28 - var32) * var36;
                        } else {
                            des[i] = var29 + (var27 - var29) * ((float)1 - var35) + (var33 - var29) * ((float)1 - var36);
                            des[i + 1] = var30 + (var28 - var30) * ((float)1 - var35) + (var34 - var30) * ((float)1 - var36);
                        }
                    } else if (var25 >= 1.0F) {
                        var27 = positions[(col + row * var26) * 2];
                        var28 = positions[(col + row * var26) * 2 + 1];
                        var29 = var14 + 3.0F * var16 + 1.0F * var18;
                        var30 = var15 + 3.0F * var17 + 1.0F * var19;
                        var31 = var14 + 1.0F * var16 + 3.0F * var18;
                        var32 = var15 + 1.0F * var17 + 3.0F * var19;
                        var33 = var14 + 3.0F * var16 + 3.0F * var18;
                        var34 = var15 + 3.0F * var17 + 3.0F * var19;
                        var35 = 0.5F * (var24 - (float)1);
                        var36 = 0.5F * (var25 - (float)1);
                        if (var35 + var36 <= 1.0F) {
                            des[i] = var27 + (var29 - var27) * var35 + (var31 - var27) * var36;
                            des[i + 1] = var28 + (var30 - var28) * var35 + (var32 - var28) * var36;
                        } else {
                            des[i] = var33 + (var31 - var33) * ((float)1 - var35) + (var29 - var33) * ((float)1 - var36);
                            des[i + 1] = var34 + (var32 - var34) * ((float)1 - var35) + (var30 - var34) * ((float)1 - var36);
                        }
                    } else {
                        var42 = (int)var23;
                        if (var42 == row) {
                            var42 = row - 1;
                        }

                        var28 = 0.5F * (var24 - (float)1);
                        var29 = var23 - (float)var42;
                        var30 = (float)var42 / (float)row;
                        var31 = (float)(var42 + 1) / (float)row;
                        var32 = positions[(col + var42 * var26) * 2];
                        var33 = positions[(col + var42 * var26) * 2 + 1];
                        var34 = positions[(col + (var42 + 1) * var26) * 2];
                        var35 = positions[(col + (var42 + 1) * var26) * 2 + 1];
                        var36 = var14 + 3.0F * var16 + var30 * var18;
                        var37 = var15 + 3.0F * var17 + var30 * var19;
                        var38 = var14 + 3.0F * var16 + var31 * var18;
                        var39 = var15 + 3.0F * var17 + var31 * var19;
                        if (var28 + var29 <= 1.0F) {
                            des[i] = var32 + (var36 - var32) * var28 + (var34 - var32) * var29;
                            des[i + 1] = var33 + (var37 - var33) * var28 + (var35 - var33) * var29;
                        } else {
                            des[i] = var38 + (var34 - var38) * ((float)1 - var28) + (var36 - var38) * ((float)1 - var29);
                            des[i + 1] = var39 + (var35 - var39) * ((float)1 - var28) + (var37 - var39) * ((float)1 - var29);
                        }
                    }
                } else if (var25 <= 0.0F) {
                    var42 = (int)var22;
                    if (var42 == col) {
                        var42 = col - 1;
                    }

                    var28 = var22 - (float)var42;
                    var29 = 0.5F * (var25 - (float)-2);
                    var30 = (float)var42 / (float)col;
                    var31 = (float)(var42 + 1) / (float)col;
                    var32 = positions[(var42 + 0 * var26) * 2];
                    var33 = positions[(var42 + 0 * var26) * 2 + 1];
                    var34 = positions[(var42 + 1 + 0 * var26) * 2];
                    var35 = positions[(var42 + 1 + 0 * var26) * 2 + 1];
                    var36 = var14 + var30 * var16 - 2.0F * var18;
                    var37 = var15 + var30 * var17 - 2.0F * var19;
                    var38 = var14 + var31 * var16 - 2.0F * var18;
                    var39 = var15 + var31 * var17 - 2.0F * var19;
                    if (var28 + var29 <= 1.0F) {
                        des[i] = var36 + (var38 - var36) * var28 + (var32 - var36) * var29;
                        des[i + 1] = var37 + (var39 - var37) * var28 + (var33 - var37) * var29;
                    } else {
                        des[i] = var34 + (var32 - var34) * ((float)1 - var28) + (var38 - var34) * ((float)1 - var29);
                        des[i + 1] = var35 + (var33 - var35) * ((float)1 - var28) + (var39 - var35) * ((float)1 - var29);
                    }
                } else if (var25 >= 1.0F) {
                    var42 = (int)var22;
                    if (var42 == col) {
                        var42 = col - 1;
                    }

                    var28 = var22 - (float)var42;
                    var29 = 0.5F * (var25 - (float)1);
                    var30 = (float)var42 / (float)col;
                    var31 = (float)(var42 + 1) / (float)col;
                    var32 = positions[(var42 + row * var26) * 2];
                    var33 = positions[(var42 + row * var26) * 2 + 1];
                    var34 = positions[(var42 + 1 + row * var26) * 2];
                    var35 = positions[(var42 + 1 + row * var26) * 2 + 1];
                    var36 = var14 + var30 * var16 + 3.0F * var18;
                    var37 = var15 + var30 * var17 + 3.0F * var19;
                    var38 = var14 + var31 * var16 + 3.0F * var18;
                    var39 = var15 + var31 * var17 + 3.0F * var19;
                    if (var28 + var29 <= 1.0F) {
                        des[i] = var32 + (var34 - var32) * var28 + (var36 - var32) * var29;
                        des[i + 1] = var33 + (var35 - var33) * var28 + (var37 - var33) * var29;
                    } else {
                        des[i] = var38 + (var36 - var38) * ((float)1 - var28) + (var34 - var38) * ((float)1 - var29);
                        des[i + 1] = var39 + (var37 - var39) * ((float)1 - var28) + (var35 - var39) * ((float)1 - var29);
                    }
                } else {
                    qDebug() << "error calc : %.4f , %.4f\t\t\t\t\t@@BDBoxGrid\n";

                }
            } else {
                des[i] = var14 + var24 * var16 + var25 * var18;
                des[i + 1] = var15 + var24 * var17 + var25 * var19;
            }
        }
    }
}

void CWarpCoorType::transformCanvasToLocal(float src[], float des[], int len)
{
        int col = GetCol();
        int row = GetRow();

        if (GetIsQuadTransform()) {
            QRectF rect = QRectF(0.0F, 0.0F, 1.0F, 1.0F);
            std::vector<float>& positions = GetPositions();
            CGridTransform var11 = CGridTransform(rect, positions, col, row);
            var11.inverseTransform(src, 0, des, 0, len, true);
        } else {
            // QRectF rect =  QRectF(0.0F, 0.0F, 1.0F, 1.0F);
            // std::vector<float>& positions = GetPositions();
            // GridTransform var18 = GridTransform(rect, CoorTypeTool.floatTodouble(positions), col, row);
            // double[] var13 = new double[des.length];
            // var18.inverseTransform(CoorTypeTool.floatTodouble(src), 0, var13, 0, len);
            // System.arraycopy(CoorTypeTool.doubleTofloat(var13), 0, des, 0, len * 2);


            QRectF rect = QRectF(0.0F, 0.0F, 1.0F, 1.0F);
            std::vector<float>& positions = GetPositions();
            CGridTransform var11 = CGridTransform(rect, positions, col, row);
            var11.inverseTransform(src, 0, des, 0, len, true);
        }
}

bool CWarpCoorType::Test()
{
    QString data ="553.0, 367.0, 393.0, 367.0, 553.0, 678.0, 393.0, 678.0";
    //String data = "0.54448783 0.34266493 0.37087673 0.34266493 0.54448783 0.68012154 0.37087673 0.68012154";
    QString pos = "51.2 51.2 235.51999 51.2 419.84 51.2 604.16 51.2 788.48 51.2 972.8 51.2 51.2 235.51999 235.51999 235.51999 419.84 235.51999 604.16 235.51999 788.48 235.51999 972.8 235.51999 51.2 419.84 235.51999 419.84 419.84 419.84 604.16 419.84 788.48 419.84 972.8 419.84 51.2 604.16 235.51999 604.16 419.84 604.16 604.16 604.16 788.48 604.16 972.8 604.16 51.2 788.48 235.51999 788.48 419.84 788.48 604.16 788.48 788.48 788.48 972.8 788.48 51.2 972.8 235.51999 972.8 419.84 972.8 604.16 972.8 788.48 972.8 972.8 972.8";

    QStringList strs =pos.split(" ");


    std::vector<float> positions;
    positions.resize(strs.size(),0.0f);
    for (int i = 0 ;i < strs.size() ;i++){
        positions[i] = strs[i].toFloat();
    }
    strs =data.split(", ");
    float* src = new float[strs.size()];
    float* des = new float[strs.size()];
    int srcLen = strs.size()/2;
    for (int i = 0 ;i < strs.size() ;i++){
        src[i] = strs[i].toFloat();
    }
    CWarpCoorType warpCoorType= CWarpCoorType(5,5,positions,true);
    warpCoorType.transformCanvasToLocal(src,des,srcLen);
    for(int i = 0 ;i < srcLen*2;i++){
        qDebug() << des[i];
    }

    return true;
}

}
