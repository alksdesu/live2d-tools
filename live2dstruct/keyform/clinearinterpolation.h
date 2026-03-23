#ifndef CLINEARINTERPOLATION_H
#define CLINEARINTERPOLATION_H

#include "live2dstruct/CTreePrecompile.h"


namespace live2dstruct {
class CLinearInterpolation
{
public:
    CLinearInterpolation();

    static void SetCKeyFormObject(C2DTreeNode* node);
private:
    //这个算法完成
    static bool CalOnePara(C2DTreeNode *node ,std::vector<CParameterNode*>& vetPara);
private:
    //测试二个参数控制的算法
    static bool CalTowPara(C2DTreeNode *node ,std::vector<CParameterNode*>& vetPara);
    static void CalZorePara(C2DTreeNode *node);
    static float GetOneInsertFun(float a,float x0 , float x1);
    static float GetTowInsertFun(float a,float b,float x0 , float y0,float x1,float y1);
    static float GetTowFun(float x,float y);
};
}
#endif // CLINEARINTERPOLATION_H
