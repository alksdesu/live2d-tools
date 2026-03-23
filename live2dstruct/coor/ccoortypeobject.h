#ifndef CCOORTYPEOBJECT_H
#define CCOORTYPEOBJECT_H

namespace live2dstruct {

class CCoorTypeObject
{
public:
    CCoorTypeObject();
    virtual ~CCoorTypeObject();
    virtual void transformLocalToCanvas(float* src, float* des, int vetSize, int offset, int step)=0;
    virtual void transformCanvasToLocal(float* src, float* des, int len) =0;


};
}

#endif // CCOORTYPEOBJECT_H
