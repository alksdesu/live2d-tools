#include "coffscreenlive.h"



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
    GLint readType, readFormat;
    GLubyte *pixels;
    glGetIntegerv ( GL_IMPLEMENTATION_COLOR_READ_TYPE, &readType );
    glGetIntegerv ( GL_IMPLEMENTATION_COLOR_READ_FORMAT, &readFormat );
    unsigned int bytesPerPixel = 0;
    switch ( readType )
    {
    case GL_UNSIGNED_BYTE:
    case GL_BYTE:
        switch ( readFormat )
        {
        case GL_RGBA:
            bytesPerPixel = 4;
            break;
        case GL_RGB:
        case GL_RGB_INTEGER:
            bytesPerPixel = 3;
            break;
        case GL_RG:
        case GL_RG_INTEGER:
        case GL_LUMINANCE_ALPHA:
            bytesPerPixel = 2;
            break;
        case GL_RED:
        case GL_RED_INTEGER:
        case GL_ALPHA:
        case GL_LUMINANCE:
            bytesPerPixel = 1;
            break;
        default:
            // Undetected format/error
            break;
        }
        break;
    case GL_FLOAT:
    case GL_UNSIGNED_INT:
    case GL_INT:
        switch ( readFormat )
        {
        case GL_RGBA:
        case GL_RGBA_INTEGER:
            bytesPerPixel = 16;
            break;
        case GL_RGB:
        case GL_RGB_INTEGER:
            bytesPerPixel = 12;
            break;
        case GL_RG:
        case GL_RG_INTEGER:
            bytesPerPixel = 8;
            break;
        case GL_RED:
        case GL_RED_INTEGER:
        case GL_DEPTH_COMPONENT:
            bytesPerPixel = 4;
            break;
        default:
            // Undetected format/error
            break;
        }
        break;
    case GL_HALF_FLOAT:
    case GL_UNSIGNED_SHORT:
    case GL_SHORT:
        switch ( readFormat )
        {
        case GL_RGBA:
        case GL_RGBA_INTEGER:
            bytesPerPixel = 8;
            break;
        case GL_RGB:
        case GL_RGB_INTEGER:
            bytesPerPixel = 6;
            break;
        case GL_RG:
        case GL_RG_INTEGER:
            bytesPerPixel = 4;
            break;
        case GL_RED:
        case GL_RED_INTEGER:
            bytesPerPixel = 2;
            break;
        default:
            // Undetected format/error
            break;
        }
        break;
    case GL_FLOAT_32_UNSIGNED_INT_24_8_REV: // GL_DEPTH_STENCIL
        bytesPerPixel = 8;
        break;
        // GL_RGBA, GL_RGBA_INTEGER format
    case GL_UNSIGNED_INT_2_10_10_10_REV:
    case GL_UNSIGNED_INT_10F_11F_11F_REV: // GL_RGB format
    case GL_UNSIGNED_INT_5_9_9_9_REV: // GL_RGB format
    case GL_UNSIGNED_INT_24_8: // GL_DEPTH_STENCIL format
        bytesPerPixel = 4;
        break;
    case GL_UNSIGNED_SHORT_4_4_4_4: // GL_RGBA format
    case GL_UNSIGNED_SHORT_5_5_5_1: // GL_RGBA format
    case GL_UNSIGNED_SHORT_5_6_5: // GL_RGB format
        bytesPerPixel = 2;
        break;
    default:
        break;
        // Undetected type/error
    }
    pixels = ( GLubyte* ) malloc( width * height * bytesPerPixel );

    glReadBuffer ( GL_COLOR_ATTACHMENT0 );//读取附件0的帧缓冲区
    //glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, width, height, GLenum(GL_RGBA),GLenum(GL_UNSIGNED_BYTE), pixels);//将读出的像素保存在m_pBGRBuf中。

    QImage image (pixels, width, height, QImage::Format_RGBA8888);
    return image;
}


