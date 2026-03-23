#ifndef CDRAWOBJECT_H
#define CDRAWOBJECT_H
#include <QMatrix4x4>
#include <QOpenGLFunctions_3_3_Core>


class CDrawObject : public QOpenGLFunctions_3_3_Core
{
public:
    CDrawObject();
    virtual ~CDrawObject();
    virtual void Draw(const QMatrix4x4 &mv) = 0;
    virtual void InitOpenGl() = 0;
    virtual void SetDraw(bool flag) = 0;

};

#endif // CDRAWOBJECT_H
