#include "cwarpcoortype.h"
#include <cmath>
#include <limits>

namespace live2dstruct {
CWarpCoorType::CWarpCoorType(int Col, int Row, const std::vector<float>& positions, bool isQuadTransform)
    : Col(Col), Row(Row), Positions(positions), isQuadTransform(isQuadTransform),
      m_grid(QRectF(0, 0, 1, 1), Positions, Col, Row)
{
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

const std::vector<float>& CWarpCoorType::GetPositions() const
{
    return Positions;
}

bool CWarpCoorType::GetIsQuadTransform()
{
    return isQuadTransform;
}


bool CWarpCoorType::transformLocalToCanvas(float* src, float* des, int vetSize, int offset, int step)
{
    if (vetSize < 0 || vetSize > 1000000 || offset < 0 || step < 2 ||
        (vetSize > 0 && static_cast<qint64>(offset) + static_cast<qint64>(vetSize - 1) * step + 1 >
            std::numeric_limits<int>::max())) return false;
    if (!m_grid.IsValid()) return false;
    if (vetSize == 0) return true;
    if (!src || !des) return false;
    std::vector<float> input;
    input.reserve(static_cast<size_t>(vetSize) * 2);
    for (int index = 0; index < vetSize; ++index) {
        const int position = offset + index * step;
        if (!std::isfinite(src[position]) || !std::isfinite(src[position + 1])) return false;
        input.push_back(src[position]);
        input.push_back(src[position + 1]);
    }
    std::vector<float> result(input.size());
    transformLocalToCanvas_nation(input.data(), result.data(), vetSize, 0, 2, Positions, Col, Row, isQuadTransform);
    for (float value : result) if (!std::isfinite(value)) return false;
    for (int index = 0; index < vetSize; ++index) {
        const int position = offset + index * step;
        des[position] = result[index * 2];
        des[position + 1] = result[index * 2 + 1];
    }
    return true;
}

void CWarpCoorType::transformLocalToCanvas_nation(float* src, float* des, int vetNum, int offset, int step, const std::vector<float>& positions, int col, int row, bool IsQuad)
{
    const auto coordinate = [&positions](size_t index) { return static_cast<double>(positions[index]); };
    const auto output = [](double value) {
        return std::isfinite(value) && std::abs(value) <= std::numeric_limits<float>::max()
            ? static_cast<float>(value) : std::numeric_limits<float>::quiet_NaN();
    };
    int length = vetNum * step;
    double var12 = 0.0;
    double var13 = 0.0;
    double var14 = 0.0;
    double var15 = 0.0;
    double var16 = 0.0;
    double var17 = 0.0;
    double var18 = 0.0;
    double var19 = 0.0;
    bool var20 = false;

    for(int i = offset; i < length; i += step) {
        double var22 = 0.0;
        double var23 = 0.0;
        double var24 = 0.0;
        double var25 = 0.0;
        var24 = src[i];
        var25 = src[i + 1];
        var22 = var24 * (double)col;
        var23 = var25 * (double)row;
        double var27;
        if (!(var22 < 0.0) && !(var23 < 0.0) && !((double)col <= var22) && !((double)row <= var23)) {
            var12 = var22 - (double)((int)var22);
            var13 = var23 - (double)((int)var23);
            int var40 = 2 * ((int)var22 + (int)var23 * (col + 1));
            if (IsQuad) {
                double var41 = 1.0 - var12;
                var27 = 1.0 - var13;
                des[i] = output(coordinate(var40) * var41 * var27 + coordinate(var40 + 2) * var12 * var27 + coordinate(var40 + 2 * (col + 1)) * var41 * var13 + coordinate(var40 + 2 * (col + 1) + 2) * var12 * var13);
                des[i + 1] = output(coordinate(var40 + 1) * var41 * var27 + coordinate(var40 + 3) * var12 * var27 + coordinate(var40 + 2 * (col + 1) + 1) * var41 * var13 + coordinate(var40 + 2 * (col + 1) + 3) * var12 * var13);
            } else if (var12 + var13 < 1.0) {
                des[i] = output(coordinate(var40) * (1.0 - var12 - var13) + coordinate(var40 + 2) * var12 + coordinate(var40 + 2 * (col + 1)) * var13);
                des[i + 1] = output(coordinate(var40 + 1) * (1.0 - var12 - var13) + coordinate(var40 + 3) * var12 + coordinate(var40 + 2 * (col + 1) + 1) * var13);
            } else {
                des[i] = output(coordinate(var40 + 2 * (col + 1) + 2) * (var12 - 1.0 + var13) + coordinate(var40 + 2 * (col + 1)) * (1.0 - var12) + coordinate(var40 + 2) * (1.0 - var13));
                des[i + 1] = output(coordinate(var40 + 2 * (col + 1) + 3) * (var12 - 1.0 + var13) + coordinate(var40 + 2 * (col + 1) + 1) * (1.0 - var12) + coordinate(var40 + 3) * (1.0 - var13));
            }
        } else {
            int var26 = col + 1;
            double var28;
            double var29;
            double var30;
            if (!var20) {
                var20 = true;
                var14 = 0.25 * (coordinate((0 + 0 * var26) * 2) + coordinate((col + 0 * var26) * 2) + coordinate((0 + row * var26) * 2) + coordinate((col + row * var26) * 2));
                var15 = 0.25 * (coordinate((0 + 0 * var26) * 2 + 1) + coordinate((col + 0 * var26) * 2 + 1) + coordinate((0 + row * var26) * 2 + 1) + coordinate((col + row * var26) * 2 + 1));
                var27 = coordinate((col + row * var26) * 2) - coordinate((0 + 0 * var26) * 2);
                var28 = coordinate((col + row * var26) * 2 + 1) - coordinate((0 + 0 * var26) * 2 + 1);
                var29 = coordinate((col + 0 * var26) * 2) - coordinate((0 + row * var26) * 2);
                var30 = coordinate((col + 0 * var26) * 2 + 1) - coordinate((0 + row * var26) * 2 + 1);
                var16 = (var27 + var29) * 0.5;
                var17 = (var28 + var30) * 0.5;
                var18 = (var27 - var29) * 0.5;
                var19 = (var28 - var30) * 0.5;
                if (var16 == 0.0 && var17 == 0.0) {
                }

                if (var18 == 0.0 && var19 == 0.0) {
                }

                var14 -= 0.5 * (var16 + var18);
                var15 -= 0.5 * (var17 + var19);
            }

            if (-2.0 < var24 && var24 < 3.0 && -2.0 < var25 && var25 < 3.0) {
                double var31;
                double var32;
                double var33;
                double var34;
                double var35;
                double var36;
                double var37;
                double var38;
                double var39;
                int var42;
                if (var24 <= 0.0) {
                    if (var25 <= 0.0) {
                        var27 = coordinate((0 + 0 * var26) * 2);
                        var28 = coordinate((0 + 0 * var26) * 2 + 1);
                        var29 = var14 - 2.0 * var16;
                        var30 = var15 - 2.0 * var17;
                        var31 = var14 - 2.0 * var18;
                        var32 = var15 - 2.0 * var19;
                        var33 = var14 - 2.0 * var16 - 2.0 * var18;
                        var34 = var15 - 2.0 * var17 - 2.0 * var19;
                        var35 = 0.5 * (var24 - (double)-2);
                        var36 = 0.5 * (var25 - (double)-2);
                        if (var35 + var36 <= 1.0) {
                            des[i] = output(var33 + (var31 - var33) * var35 + (var29 - var33) * var36);
                            des[i + 1] = output(var34 + (var32 - var34) * var35 + (var30 - var34) * var36);
                        } else {
                            des[i] = output(var27 + (var29 - var27) * ((double)1 - var35) + (var31 - var27) * ((double)1 - var36));
                            des[i + 1] = output(var28 + (var30 - var28) * ((double)1 - var35) + (var32 - var28) * ((double)1 - var36));
                        }
                    } else if (var25 >= 1.0) {
                        var27 = coordinate((0 + row * var26) * 2);
                        var28 = coordinate((0 + row * var26) * 2 + 1);
                        var29 = var14 - 2.0 * var16 + 1.0 * var18;
                        var30 = var15 - 2.0 * var17 + 1.0 * var19;
                        var31 = var14 + 3.0 * var18;
                        var32 = var15 + 3.0 * var19;
                        var33 = var14 - 2.0 * var16 + 3.0 * var18;
                        var34 = var15 - 2.0 * var17 + 3.0 * var19;
                        var35 = 0.5 * (var24 - (double)-2);
                        var36 = 0.5 * (var25 - (double)1);
                        if (var35 + var36 <= 1.0) {
                            des[i] = output(var29 + (var27 - var29) * var35 + (var33 - var29) * var36);
                            des[i + 1] = output(var30 + (var28 - var30) * var35 + (var34 - var30) * var36);
                        } else {
                            des[i] = output(var31 + (var33 - var31) * ((double)1 - var35) + (var27 - var31) * ((double)1 - var36));
                            des[i + 1] = output(var32 + (var34 - var32) * ((double)1 - var35) + (var28 - var32) * ((double)1 - var36));
                        }
                    } else {
                        var42 = (int)var23;
                        if (var42 == row) {
                            var42 = row - 1;
                        }

                        var28 = 0.5 * (var24 - (double)-2);
                        var29 = var23 - (double)var42;
                        var30 = (double)var42 / (double)row;
                        var31 = (double)(var42 + 1) / (double)row;
                        var32 = coordinate((0 + var42 * var26) * 2);
                        var33 = coordinate((0 + var42 * var26) * 2 + 1);
                        var34 = coordinate((0 + (var42 + 1) * var26) * 2);
                        var35 = coordinate((0 + (var42 + 1) * var26) * 2 + 1);
                        var36 = var14 - 2.0 * var16 + var30 * var18;
                        var37 = var15 - 2.0 * var17 + var30 * var19;
                        var38 = var14 - 2.0 * var16 + var31 * var18;
                        var39 = var15 - 2.0 * var17 + var31 * var19;
                        if (var28 + var29 <= 1.0) {
                            des[i] = output(var36 + (var32 - var36) * var28 + (var38 - var36) * var29);
                            des[i + 1] = output(var37 + (var33 - var37) * var28 + (var39 - var37) * var29);
                        } else {
                            des[i] = output(var34 + (var38 - var34) * ((double)1 - var28) + (var32 - var34) * ((double)1 - var29));
                            des[i + 1] = output(var35 + (var39 - var35) * ((double)1 - var28) + (var33 - var35) * ((double)1 - var29));
                        }
                    }
                } else if (1.0 <= var24) {
                    if (var25 <= 0.0) {
                        var27 = coordinate((col + 0 * var26) * 2);
                        var28 = coordinate((col + 0 * var26) * 2 + 1);
                        var29 = var14 + 3.0 * var16;
                        var30 = var15 + 3.0 * var17;
                        var31 = var14 + 1.0 * var16 - 2.0 * var18;
                        var32 = var15 + 1.0 * var17 - 2.0 * var19;
                        var33 = var14 + 3.0 * var16 - 2.0 * var18;
                        var34 = var15 + 3.0 * var17 - 2.0 * var19;
                        var35 = 0.5 * (var24 - (double)1);
                        var36 = 0.5 * (var25 - (double)-2);
                        if (var35 + var36 <= 1.0) {
                            des[i] = output(var31 + (var33 - var31) * var35 + (var27 - var31) * var36);
                            des[i + 1] = output(var32 + (var34 - var32) * var35 + (var28 - var32) * var36);
                        } else {
                            des[i] = output(var29 + (var27 - var29) * ((double)1 - var35) + (var33 - var29) * ((double)1 - var36));
                            des[i + 1] = output(var30 + (var28 - var30) * ((double)1 - var35) + (var34 - var30) * ((double)1 - var36));
                        }
                    } else if (var25 >= 1.0) {
                        var27 = coordinate((col + row * var26) * 2);
                        var28 = coordinate((col + row * var26) * 2 + 1);
                        var29 = var14 + 3.0 * var16 + 1.0 * var18;
                        var30 = var15 + 3.0 * var17 + 1.0 * var19;
                        var31 = var14 + 1.0 * var16 + 3.0 * var18;
                        var32 = var15 + 1.0 * var17 + 3.0 * var19;
                        var33 = var14 + 3.0 * var16 + 3.0 * var18;
                        var34 = var15 + 3.0 * var17 + 3.0 * var19;
                        var35 = 0.5 * (var24 - (double)1);
                        var36 = 0.5 * (var25 - (double)1);
                        if (var35 + var36 <= 1.0) {
                            des[i] = output(var27 + (var29 - var27) * var35 + (var31 - var27) * var36);
                            des[i + 1] = output(var28 + (var30 - var28) * var35 + (var32 - var28) * var36);
                        } else {
                            des[i] = output(var33 + (var31 - var33) * ((double)1 - var35) + (var29 - var33) * ((double)1 - var36));
                            des[i + 1] = output(var34 + (var32 - var34) * ((double)1 - var35) + (var30 - var34) * ((double)1 - var36));
                        }
                    } else {
                        var42 = (int)var23;
                        if (var42 == row) {
                            var42 = row - 1;
                        }

                        var28 = 0.5 * (var24 - (double)1);
                        var29 = var23 - (double)var42;
                        var30 = (double)var42 / (double)row;
                        var31 = (double)(var42 + 1) / (double)row;
                        var32 = coordinate((col + var42 * var26) * 2);
                        var33 = coordinate((col + var42 * var26) * 2 + 1);
                        var34 = coordinate((col + (var42 + 1) * var26) * 2);
                        var35 = coordinate((col + (var42 + 1) * var26) * 2 + 1);
                        var36 = var14 + 3.0 * var16 + var30 * var18;
                        var37 = var15 + 3.0 * var17 + var30 * var19;
                        var38 = var14 + 3.0 * var16 + var31 * var18;
                        var39 = var15 + 3.0 * var17 + var31 * var19;
                        if (var28 + var29 <= 1.0) {
                            des[i] = output(var32 + (var36 - var32) * var28 + (var34 - var32) * var29);
                            des[i + 1] = output(var33 + (var37 - var33) * var28 + (var35 - var33) * var29);
                        } else {
                            des[i] = output(var38 + (var34 - var38) * ((double)1 - var28) + (var36 - var38) * ((double)1 - var29));
                            des[i + 1] = output(var39 + (var35 - var39) * ((double)1 - var28) + (var37 - var39) * ((double)1 - var29));
                        }
                    }
                } else if (var25 <= 0.0) {
                    var42 = (int)var22;
                    if (var42 == col) {
                        var42 = col - 1;
                    }

                    var28 = var22 - (double)var42;
                    var29 = 0.5 * (var25 - (double)-2);
                    var30 = (double)var42 / (double)col;
                    var31 = (double)(var42 + 1) / (double)col;
                    var32 = coordinate((var42 + 0 * var26) * 2);
                    var33 = coordinate((var42 + 0 * var26) * 2 + 1);
                    var34 = coordinate((var42 + 1 + 0 * var26) * 2);
                    var35 = coordinate((var42 + 1 + 0 * var26) * 2 + 1);
                    var36 = var14 + var30 * var16 - 2.0 * var18;
                    var37 = var15 + var30 * var17 - 2.0 * var19;
                    var38 = var14 + var31 * var16 - 2.0 * var18;
                    var39 = var15 + var31 * var17 - 2.0 * var19;
                    if (var28 + var29 <= 1.0) {
                        des[i] = output(var36 + (var38 - var36) * var28 + (var32 - var36) * var29);
                        des[i + 1] = output(var37 + (var39 - var37) * var28 + (var33 - var37) * var29);
                    } else {
                        des[i] = output(var34 + (var32 - var34) * ((double)1 - var28) + (var38 - var34) * ((double)1 - var29));
                        des[i + 1] = output(var35 + (var33 - var35) * ((double)1 - var28) + (var39 - var35) * ((double)1 - var29));
                    }
                } else if (var25 >= 1.0) {
                    var42 = (int)var22;
                    if (var42 == col) {
                        var42 = col - 1;
                    }

                    var28 = var22 - (double)var42;
                    var29 = 0.5 * (var25 - (double)1);
                    var30 = (double)var42 / (double)col;
                    var31 = (double)(var42 + 1) / (double)col;
                    var32 = coordinate((var42 + row * var26) * 2);
                    var33 = coordinate((var42 + row * var26) * 2 + 1);
                    var34 = coordinate((var42 + 1 + row * var26) * 2);
                    var35 = coordinate((var42 + 1 + row * var26) * 2 + 1);
                    var36 = var14 + var30 * var16 + 3.0 * var18;
                    var37 = var15 + var30 * var17 + 3.0 * var19;
                    var38 = var14 + var31 * var16 + 3.0 * var18;
                    var39 = var15 + var31 * var17 + 3.0 * var19;
                    if (var28 + var29 <= 1.0) {
                        des[i] = output(var32 + (var34 - var32) * var28 + (var36 - var32) * var29);
                        des[i + 1] = output(var33 + (var35 - var33) * var28 + (var37 - var33) * var29);
                    } else {
                        des[i] = output(var38 + (var36 - var38) * ((double)1 - var28) + (var34 - var38) * ((double)1 - var29));
                        des[i + 1] = output(var39 + (var37 - var39) * ((double)1 - var28) + (var35 - var39) * ((double)1 - var29));
                    }
                } else {
                    qDebug() << "error calc : %.4f , %.4f\t\t\t\t\t@@BDBoxGrid\n";

                }
            } else {
                des[i] = output(var14 + var24 * var16 + var25 * var18);
                des[i + 1] = output(var15 + var24 * var17 + var25 * var19);
            }
        }
    }
}

bool CWarpCoorType::transformCanvasToLocal(float src[], float des[], int len)
{
    return m_grid.inverseTransform(src, 0, des, 0, len, isQuadTransform);
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
