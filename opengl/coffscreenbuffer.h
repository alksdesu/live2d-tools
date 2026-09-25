#ifndef COFFSCREENBUFFER_H
#define COFFSCREENBUFFER_H

#include <QOpenGLContext>
#include <QOpenGLFunctions_3_3_Core>
#include <QPointer>

class COffscreenBuffer
{
public:
    COffscreenBuffer() = default;
    ~COffscreenBuffer();
    COffscreenBuffer(const COffscreenBuffer&) = delete;
    COffscreenBuffer& operator=(const COffscreenBuffer&) = delete;

    bool setGlOwningContext(const QOpenGLContext* context);
    bool CreateOffscreenSurface(int width, int height, GLuint borrowed = 0);
    bool BeginDraw(GLint restoreFBO = -1);
    bool EndDraw();
    bool DestroyOffscreenSurface();
    bool Clear(float red, float green, float blue, float alpha);

    GLuint GetRenderTexture() const { return m_framebuffer; }
    GLuint GetColorBuffer() const { return m_texture; }
    int GetBufferWidth() const { return m_width; }
    int GetBufferHeight() const { return m_height; }
    bool IsValid() const;

private:
    QOpenGLFunctions_3_3_Core* functions() const;
    void reset();

    QPointer<QOpenGLContext> m_context;
    GLuint m_framebuffer = 0;
    GLuint m_texture = 0;
    int m_width = 0;
    int m_height = 0;
    GLint m_restoreRead = 0;
    GLint m_restoreDraw = 0;
    bool m_ownsTexture = false;
    bool m_drawing = false;
};

#endif
