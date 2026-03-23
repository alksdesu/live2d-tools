#include "crotationcoortype.h"

namespace live2dstruct {



float CRotationCoorType::GetOriginX()
{
    return OriginX;
}

float CRotationCoorType::GetOriginY()
{
    return OriginY;
}

float CRotationCoorType::GetAngle_forRotation()
{
    return angle+base_angles;
}

bool CRotationCoorType::GetisReflectX()
{
    return isReflectX;
}

bool CRotationCoorType::GetisReflectY()
{
    return isReflectY;
}

float CRotationCoorType::GetCalculatedTotalScale()
{
    return Scale;
}



CRotationCoorType::CRotationCoorType(float base_angles, CRotationNodeKeyform &keyform)
{
    this->base_angles = base_angles;
    this->Scale =keyform.Getscales();
    this->angle =keyform.Getangles();
    this->OriginX =keyform.Getorigin_x();
    this->OriginY =keyform.Getorigin_y();
    this->isReflectX =keyform.Getis_reflect_x();
    this->isReflectY =keyform.Getis_reflect_y();
}

CRotationCoorType::~CRotationCoorType()
{

}

void CRotationCoorType::transformLocalToCanvas(float* src, float* des, int vet, int offset, int step)
{
    double d = 0.017453292F;//弧度
    double var7 = (double)(d * GetAngle_forRotation());
    float var9 = (float)sin(var7);
    float var10 = (float)cos(var7);

    float var11 = GetCalculatedTotalScale();
    float var12 = (float)(GetisReflectX() ? -1 : 1);
    float var13 = (float)(GetisReflectY() ? -1 : 1);
    float var14 = var10 * var11 * var12;
    float var15 = -var9 * var11 * var13;
    float var16 = var9 * var11 * var12;
    float var17 = var10 * var11 * var13;
    float var18 = GetOriginX();
    float var19 = GetOriginY();

    int size = vet * step;
    for(int var21 = offset; var21 < size ; var21 += step) {
        float var22 = src[var21];
        float var23 = src[var21+1];
        des[var21] =var14 * var22 + var15 * var23 + var18;
        des[var21+1] =var16 * var22 + var17 * var23 + var19;
    }
}

void CRotationCoorType::transformCanvasToLocal(float* src, float* des, int vet)
{
    QTransform var7 ;
    var7.translate((double)GetOriginX(), (double)GetOriginY());
    double d = 0.017453292F;//弧度
    var7.rotate(GetAngle_forRotation() * d );
    var7.scale(this->GetCalculatedTotalScale() * (GetisReflectX() ? -1.0 : 1.0),this->GetCalculatedTotalScale() * (GetisReflectY() ? -1.0 : 1.0));
    QTransform temp = var7.inverted();
    int step =2;
    int size = vet * step;
    for(int i = 0 ;i < size ;i+=step){
        QPointF point =temp.map(QPointF(src[i],src[i+1]));
        des[i] =point.x();
        des[i+1] =point.y();
    }
}

void CRotationCoorType::Test()
{
    // CRotationCoorType coorType;
    // float src[]={-31.433937f,56.89307f,49.248287f,33.013363f,-58.61208f,-17.54509f,24.617792f,-43.050705f};
    // float des[] =new float[src.length];
    // coorType.transformLocalToCanvas(src,des,src.length/2,0,2);
    // System.out.println(Arrays.toString(des) );
}
}
