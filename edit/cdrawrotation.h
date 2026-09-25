#ifndef CDRAWROTATION_H
#define CDRAWROTATION_H
#include <QVector2D>
#include <QMatrix4x4>
#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLShaderProgram>
#include <QRectF>
#include "live2dstruct/tree/crotationnode.h"
#include <QTransform>
#include <QOpenGLTexture>
#include "cdrawobject.h"
#include "opengl/cglmeshbuffer.h"
#include <QPointer>
#include <memory>
using namespace live2dstruct;
class CDrawRotation : public CDrawObject
{
public:
    CDrawRotation();
    virtual ~CDrawRotation();
    virtual void Draw(const QMatrix4x4 &mv);
    virtual void SetDraw(bool flag);
    virtual  void InitOpenGl();
    bool Init(CRotationNode* node);
    bool IsShown() const { return m_IsDraw; }
    bool PrepareOpenGl();

private:
    friend class EditorOverlayRenderTests;
    bool EnsureOpenGl();
    bool ClaRect();
    CRotationNodeKeyform cur;
    float base_angles = 0;
    bool m_IsDraw = false;


    std::vector<quint16> vetIndex;
    std::vector<float> vetTex;//采样纹理固定的
    std::vector<float> vetrclPos;//圆的放置位置
    std::vector<float> vetdirPos;//加载方向的放置位置
    std::unique_ptr<QOpenGLShaderProgram> shader;
    std::unique_ptr<QOpenGLTexture> m_rclTex;
    std::unique_ptr<QOpenGLTexture> m_dirTex;
    QPointer<QOpenGLContext> m_context;
    CGlMeshBuffer m_originGeometry;
    CGlMeshBuffer m_directionGeometry;

};

#endif // CDRAWROTATION_H
