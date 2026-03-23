#ifndef COFFSCREENLIVE_H
#define COFFSCREENLIVE_H

#include <QOpenGLFunctions_3_3_Core>
#include <QImage>


//live2d进行离开屏幕渲染
//需要那些内容
//比如说
class COffScreenLive : public QOpenGLFunctions_3_3_Core
{
public:
    explicit COffScreenLive();
    ~COffScreenLive();

    //上下文内容
    void setGlOwningContext(const QOpenGLContext *context);

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

    QImage ReadFrame(int width,int height);
private:
    GLuint      _renderTexture;         ///< レンダリングターゲットとしてのアドレス
    GLuint      _colorBuffer;           ///< 描画の際使用するテクスチャとしてのアドレス
    GLint       _oldFBO;                ///< 旧フレームバッファ
    int   _bufferWidth;           ///< Create時に指定された幅
    int   _bufferHeight;          ///< Create時に指定された高さ
    bool _isColorBufferInherited;    ///< 引数によって設定されたカラーバッファか？




};

#endif // COFFSCREENLIVE_H
