#ifndef CREADOPENGL_H
#define CREADOPENGL_H
#include <QOpenGLFunctions_3_3_Core>

#include "coffscreenmap.h"
#include <QMatrix4x4>
#include <QVector3D>
#include <QVector4D>
#include <QMatrix3x3>
#include <QDebug>
#include <QPair>
#include <QOpenGLShaderProgram>
#include "cmesh.h"
#include "framework/Model/CubismModel.hpp"
using namespace Live2D::Cubism::Framework;
class CReadOpengl : public QOpenGLFunctions_3_3_Core
{
public:

    CReadOpengl();

public:
    void setGlOwningContext(const QOpenGLContext *context);
    //更新当前帧缓冲区
    void UpdateFrame(GLuint tex,int w,int h);

    void WriteONE(const QString& fileName,int width ,int height);
    void Write(const QString& fileName,int width ,int height);
    QPair<QImage*,GLubyte*>   GetImage(int x,int y ,int width, int height);
    //void UpdateBuffer(int width, int height);
    void PrintImage(int width, int height);

    //模型
    //const CubismModel& model, const csmInt32 index    //还要一个矩阵
    COffScreenNode GetScreenNode(CubismModel& model, const int index,const float* mvp,int w,int h,bool IsPrint);//传递过来的宽，高


private:
    static bool IsWrite;
    static int m_sIndex;
    QOpenGLShaderProgram shaderProgramOffset;//要有个shader

public:
    void BeginDraw(GLint restoreFBO= -1);
    void EndDraw();
    void Clear(float r, float g, float b, float a);
    bool CreateOffscreenSurface(int displayBufferWidth, int displayBufferHeight, GLuint colorBuffer = 0);
    void DestroyOffscreenSurface();
    GLuint GetRenderTexture() const;
    GLuint GetColorBuffer() const;
    int GetBufferWidth() const;
    int GetBufferHeight() const;
    bool IsValid() const;
    void PreDraw();

private:
    GLuint      _renderTexture;         ///< レンダリングターゲットとしてのアドレス
    GLuint      _colorBuffer;           ///< 描画の際使用するテクスチャとしてのアドレス
    GLint       _oldFBO;                ///< 旧フレームバッファ
    int   _bufferWidth;           ///< Create時に指定された幅
    int   _bufferHeight;          ///< Create時に指定された高さ
    bool _isColorBufferInherited;    ///< 引数によって設定されたカラーバッファか？



    //坐标全屏映射到全屏
    GLfloat m_uvVertex[8]={
        0.0f, 0.0f, // 第一个UV坐标
        1.0f, 0.0f, // 第二个UV坐标
        1.0f, 1.0f, // 第三个UV坐标
        0.0f, 1.0f, // 第四个UV坐标
    };
    GLfloat m_positionVertex[8] = {
        -1.0f, -1.0f,  // 第一个顶点的坐标
        1.0f, -1.0f,  // 第二个顶点的坐标
        1.0f,  1.0f, // 第三个顶点的坐标
        -1.0f,  1.0f,  // 第四个顶点的坐标
    };
    GLushort m_indices[6] = {
        0, 1, 3, // first triangle
        1, 2, 3 // second triangle
    };


};

#endif // CREADOPENGL_H
