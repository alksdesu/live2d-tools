#ifndef CDRAWRECT_H
#define CDRAWRECT_H
#include <QVector2D>
#include <QMatrix4x4>
#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLShaderProgram>
#include <QRectF>
#include "cdrawobject.h"
#include "opengl/cglmeshbuffer.h"
#include <QPointer>
#include <memory>
class CDrawRect : public CDrawObject
{
public:
    CDrawRect(const QVector3D& color);

    virtual ~CDrawRect();
    virtual void InitOpenGl();
    virtual void SetDraw(bool flag);
    virtual void Draw(const QMatrix4x4& projection);


    void SetStartAndEndPos(const QVector2D& startPos,const QVector2D& endPos);
    void SetSelectRect(const QRectF& rect);
    //判断当前点击是不是在选择矩形里面
    bool IsContains(const QPointF& point);
    void Move(const QVector2D& vet);
    const QRectF& GetQQRectF();



private:
    friend class EditorOverlayRenderTests;
    bool EnsureOpenGl();
    void SetQRectF();
    QVector2D startPos;//世界坐标系下面的点
    QVector2D endPos;//世界坐标系下面的点
    QRectF m_rect;
    std::unique_ptr<QOpenGLShaderProgram> shader;
    QPointer<QOpenGLContext> m_context;
    CGlMeshBuffer m_geometry;
    QVector3D m_color;
    bool m_IsDraw = false;
};

#endif // CDRAWRECT_H
