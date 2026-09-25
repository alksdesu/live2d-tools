#ifndef CROTATIONCOORTYPE_H
#define CROTATIONCOORTYPE_H
#include "ccoortypeobject.h"
#include "live2dstruct/keyform/crotationnodekeyform.h"

namespace live2dstruct {
class CRotationCoorType : public CCoorTypeObject
{
public:
    CRotationCoorType(float base_angles,CRotationNodeKeyform& keyform);
    virtual ~CRotationCoorType();

    float GetOriginX() const;
    float GetOriginY() const;
    double GetAngle_forRotation() const;
    bool GetisReflectX() const;
    bool GetisReflectY() const;
    float GetCalculatedTotalScale() const;
    bool IsIdentity() const;

    bool transformLocalToCanvas(float* src, float* des, int vet, int offset, int step) override;
    bool transformCanvasToLocal(float* src, float* des, int vet) override;

    void Test();
private:
    float Scale;
    bool isReflectX;
    bool isReflectY;
    float angle;
    float OriginX = 0.0f;
    float OriginY = 0.0f;
    float base_angles;
    bool m_valid = false;
    double m_cosine = 1.0;
    double m_sine = 0.0;
    double m_scaleX = 1.0;
    double m_scaleY = 1.0;
};
}
#endif // CROTATIONCOORTYPE_H
