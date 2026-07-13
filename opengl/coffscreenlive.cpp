#include "coffscreenlive.h"
#include <QDebug>



COffScreenLive::COffScreenLive()
{





}

COffScreenLive::~COffScreenLive()
{





}

void COffScreenLive::setGlOwningContext(const QOpenGLContext *context)
{
    initializeOpenGLFunctions();
    setOwningContext(context);
}

void COffScreenLive::BeginDraw(GLint restoreFBO)
{
    if (_renderTexture == 0)
    {
        return;
    }

    // バックバッファのサーフェイスを記憶しておく
    if (restoreFBO < 0)
    {
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &_oldFBO);//封装了，用于获取上一个帧缓冲区
    }
    else
    {
        _oldFBO = restoreFBO;
    }
    // マスク用RenderTextureをactiveにセット
    glBindFramebuffer(GL_FRAMEBUFFER, _renderTexture);//绘制之前先绑定
}

void COffScreenLive::EndDraw()
{
    if (_renderTexture == 0)
    {
        return;
    }

    // 描画対象を戻す
    glBindFramebuffer(GL_FRAMEBUFFER, _oldFBO);
}

void COffScreenLive::Clear(float r, float g, float b, float a)
{
    // マスクをクリアする
    glClearColor(r,g,b,a);
    glClear(GL_COLOR_BUFFER_BIT);

}

bool COffScreenLive::CreateOffscreenSurface(int displayBufferWidth, int displayBufferHeight, GLuint colorBuffer)
{
    // 一旦削除
    DestroyOffscreenSurface();
    //进行离屏幕渲染
    do
    {
        GLuint ret = 0;

        // 新しく生成する
        if (colorBuffer == 0)
        {
            glGenTextures(1, &_colorBuffer);

            glBindTexture(GL_TEXTURE_2D, _colorBuffer);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, displayBufferWidth, displayBufferHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glBindTexture(GL_TEXTURE_2D, 0);

            _isColorBufferInherited = false;
        }
        else
        {// 指定されたものを使用
            _colorBuffer = colorBuffer;

            _isColorBufferInherited = true;
        }

        GLint tmpFramebufferObject;
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &tmpFramebufferObject);//获得默认的帧缓冲

        glGenFramebuffers(1, &ret);
        glBindFramebuffer(GL_FRAMEBUFFER, ret);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, _colorBuffer, 0);//绑定帧缓冲区作为附件
        glBindFramebuffer(GL_FRAMEBUFFER, tmpFramebufferObject);

        _renderTexture = ret;//在什么时候进行绑定

        _bufferWidth = displayBufferWidth;
        _bufferHeight = displayBufferHeight;
        //使用的时候使用颜色附件就行
        // 成功
        return true;//帧缓冲区怎么使用

    } while (0);

    // 失敗したので削除
    DestroyOffscreenSurface();


    return false;
}

void COffScreenLive::DestroyOffscreenSurface()
{
    if (!_isColorBufferInherited && (_colorBuffer != 0))
    {
        glDeleteTextures(1, &_colorBuffer);
        _colorBuffer = 0;
    }

    if (_renderTexture!=0)
    {
        glDeleteFramebuffers(1, &_renderTexture);
        _renderTexture = 0;
    }

}

GLuint COffScreenLive::GetRenderTexture() const
{
    return _renderTexture;
}

GLuint COffScreenLive::GetColorBuffer() const
{
    return _colorBuffer;
}

int COffScreenLive::GetBufferWidth() const
{
    return _bufferWidth;
}

int COffScreenLive::GetBufferHeight() const
{
    return _bufferHeight;
}

bool COffScreenLive::IsValid() const
{
    return _renderTexture != 0;
}

void COffScreenLive::PreDraw()
{


    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_DEPTH_TEST);

    glEnable(GL_BLEND);//因为背景颜色是黑色，导致和屏幕混合的时候产生黑边
    glClearColor(1.0f, 1.0f, 1.0f, 0.0f);//白色，透明度为0
    glClear(GL_COLOR_BUFFER_BIT);//
    glColorMask(1, 1, 1, 1);
#ifdef CSM_TARGET_IPHONE_ES2
    glBindVertexArrayOES(0);
#endif

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ARRAY_BUFFER, 0); //前にバッファがバインドされていた

}

QImage COffScreenLive::ReadFrame(int width, int height)
{
    // QImage 自持像素内存；RGBA8888 每像素 4 字节，scanline 无 padding，可直写 bits()
    if (width <= 0 || height <= 0) {
        return QImage();
    }

    QImage image(width, height, QImage::Format_RGBA8888);
    if (image.isNull()) {
        qWarning("COffScreenLive::ReadFrame: 分配 %dx%d 图像失败", width, height);
        return image;
    }

    glReadBuffer(GL_COLOR_ATTACHMENT0);//读取附件0的帧缓冲区
    glReadPixels(0, 0, width, height, GLenum(GL_RGBA), GLenum(GL_UNSIGNED_BYTE), image.bits());

    return image;
}


