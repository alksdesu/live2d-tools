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
using namespace live2dstruct;
class CDrawRotation : public CDrawObject
{
public:
    CDrawRotation();
    virtual ~CDrawRotation();
    virtual void Draw(const QMatrix4x4 &mv);
    virtual void SetDraw(bool flag);
    virtual  void InitOpenGl();
    void Init(CRotationNode* node);

private:
    void ClaRect();//根据当前帧计算二个rect
    CRotationNodeKeyform cur;
    float base_angles;
    bool m_IsDraw;


    std::vector<short> vetIndex;
    std::vector<float> vetTex;//采样纹理固定的
    std::vector<float> vetrclPos;//圆的放置位置
    std::vector<float> vetdirPos;//加载方向的放置位置
    QOpenGLShaderProgram* shader;
    QOpenGLTexture* m_rclTex;
    QOpenGLTexture* m_dirTex;

};

#endif // CDRAWROTATION_H
