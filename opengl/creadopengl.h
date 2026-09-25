#ifndef CREADOPENGL_H
#define CREADOPENGL_H

#include <QImage>
#include <QOpenGLContext>
#include <QOpenGLFunctions_3_3_Core>
#include <QPointer>
#include <QRect>

class CReadOpengl
{
public:
    bool setGlOwningContext(const QOpenGLContext* context);
    bool PreDraw();
    bool UpdateFrame();
    bool GetScreenNode(const float* vertices, int vertexCount, const float* mvp,
                       int width, int height, QImage& image, QRect& psdRect);

private:
    bool CanUseContext() const;
    bool ClearTarget(bool alphaOnly);
    bool ReadImage(const QRect& rect, const QSize& canvasSize, QImage& image);

    QPointer<QOpenGLContext> m_context;
    QOpenGLFunctions_3_3_Core* m_gl = nullptr;
};

#endif
