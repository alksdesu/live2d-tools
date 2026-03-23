#ifndef CROTATIONCOORTYPE_H
#define CROTATIONCOORTYPE_H
#include <QtCore/qmath.h>
#include <QTransform>
#include <QVector3D>
#include "ccoortypeobject.h"
#include "live2dstruct/keyform/crotationnodekeyform.h"

namespace live2dstruct {
class CRotationCoorType : public CCoorTypeObject
{
public:
    CRotationCoorType(float base_angles,CRotationNodeKeyform& keyform);
    virtual ~CRotationCoorType();

    float GetOriginX() ;
    float GetOriginY();
    float GetAngle_forRotation() ;
    bool GetisReflectX();
    bool GetisReflectY();
    float GetCalculatedTotalScale();

    virtual void transformLocalToCanvas(float* src, float* des, int vet, int offset, int step);
    virtual void transformCanvasToLocal(float* src, float* des, int vet);

    void Test();
private:
    float Scale;
    bool isReflectX;
    bool isReflectY;
    float angle;
    float OriginX;
    float OriginY;
    float base_angles;
};
}
#endif // CROTATIONCOORTYPE_H
