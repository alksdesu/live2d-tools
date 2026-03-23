#ifndef CUBISMOFFSCREENSURFACE_OPENGLES2_H
#define CUBISMOFFSCREENSURFACE_OPENGLES2_H


#include <QOpenGLFunctions_3_3_Core>
#include "framework/CubismFramework.hpp"
#include "framework/Type/csmVector.hpp"
#include "framework/Type/csmRectF.hpp"
#include "framework/Type/csmMap.hpp"
#include <float.h>
#include "QOpenGLContext"



// #ifdef CSM_TARGET_ANDROID_ES2
// #include <jni.h>
// #include <errno.h>
// #include <GLES2/gl2.h>
// #include <GLES2/gl2ext.h>
// #endif

// #ifdef CSM_TARGET_IPHONE_ES2
// #include <OpenGLES/ES2/gl.h>
// #include <OpenGLES/ES2/glext.h>
// #endif

// #if defined(CSM_TARGET_WIN_GL) || defined(CSM_TARGET_LINUX_GL)
// #include <GL/glew.h>
// #include <GL/gl.h>
// #endif



// #ifdef CSM_TARGET_MAC_GL
// #ifndef CSM_TARGET_COCOS
// #include <GL/glew.h>
// #endif
// #include <OpenGL/gl.h>
// #endif

//------------ LIVE2D NAMESPACE ------------
namespace Live2D { namespace Cubism { namespace Framework { namespace Rendering {


/**
 * @brief  オフスクリーン描画用構造体
 */
//这是一个QOpenGLFunctions_3_3_Core 对象
class CubismOffscreenSurface_OpenGLES2 : public  QOpenGLFunctions_3_3_Core
{
public:

    void setGlOwningContext(const QOpenGLContext *context){
        initializeOpenGLFunctions();
        setOwningContext(context);
    }

    CubismOffscreenSurface_OpenGLES2();

    CubismOffscreenSurface_OpenGLES2(const CubismOffscreenSurface_OpenGLES2 &other) {
        // 复制其他对象的成员变量
        _renderTexture = other._renderTexture;         ///< レンダリングターゲットとしてのアドレス
        _colorBuffer= other._colorBuffer;           ///< 描画の際使用するテクスチャとしてのアドレス

        _oldFBO= other._oldFBO;              ///< 旧フレームバッファ

        _bufferWidth= other._bufferWidth;          ///< Create時に指定された幅
        _bufferHeight= other._bufferHeight;          ///< Create時に指定された高さ
        _isColorBufferInherited= other._isColorBufferInherited;           ///< 引数によって設定されたカラーバッファか？
        //this->setOwningContext(other.owningContext());

    }

    CubismOffscreenSurface_OpenGLES2 &operator=(const CubismOffscreenSurface_OpenGLES2 &other) {
        // 赋值操作
        _renderTexture = other._renderTexture;         ///< レンダリングターゲットとしてのアドレス
        _colorBuffer= other._colorBuffer;           ///< 描画の際使用するテクスチャとしてのアドレス

        _oldFBO= other._oldFBO;              ///< 旧フレームバッファ

        _bufferWidth= other._bufferWidth;          ///< Create時に指定された幅
        _bufferHeight= other._bufferHeight;          ///< Create時に指定された高さ
        _isColorBufferInherited= other._isColorBufferInherited;
        //this->setOwningContext(other.owningContext());
        return *this;
    }
    /**
     * @brief   指定の描画ターゲットに向けて描画開始
     *
     * @param   restoreFBO        0以上の場合、EndDrawでこの値をglBindFramebufferする
     */
    void BeginDraw(GLint restoreFBO = -1);

    /**
     * @brief   描画終了
     *
     */
    void EndDraw();

    /**
     * @brief   レンダリングターゲットのクリア
     *           呼ぶ場合はBeginDrawの後で呼ぶこと
     * @param   r   赤(0.0~1.0)
     * @param   g   緑(0.0~1.0)
     * @param   b   青(0.0~1.0)
     * @param   a   α(0.0~1.0)
     */
    void Clear(float r, float g, float b, float a);

    /**
     *  @brief  CubismOffscreenSurface作成
     *  @param  displayBufferWidth     作成するバッファ幅
     *  @param  displayBufferHeight    作成するバッファ高さ
     *  @param  colorBuffer            0以外の場合、ピクセル格納領域としてcolorBufferを使用する
     */
    csmBool CreateOffscreenSurface(csmUint32 displayBufferWidth, csmUint32 displayBufferHeight, GLuint colorBuffer = 0);

    /**
     * @brief   CubismOffscreenSurfaceの削除
     */
    void DestroyOffscreenSurface();

    /**
     * @brief   レンダーテクスチャメンバーへのアクセッサ
     */
    GLuint GetRenderTexture() const;

    /**
     * @brief   カラーバッファメンバーへのアクセッサ
     */
    GLuint GetColorBuffer() const;

    /**
     * @brief   バッファ幅取得
     */
    csmUint32 GetBufferWidth() const;

    /**
     * @brief   バッファ高さ取得
     */
    csmUint32 GetBufferHeight() const;

    /**
     * @brief   現在有効かどうか
     */
    csmBool IsValid() const;


private:
    GLuint      _renderTexture;         ///< レンダリングターゲットとしてのアドレス
    GLuint      _colorBuffer;           ///< 描画の際使用するテクスチャとしてのアドレス
    GLint       _oldFBO;                ///< 旧フレームバッファ
    csmUint32   _bufferWidth;           ///< Create時に指定された幅
    csmUint32   _bufferHeight;          ///< Create時に指定された高さ
    csmBool     _isColorBufferInherited;    ///< 引数によって設定されたカラーバッファか？
};


}}}}

//------------ LIVE2D NAMESPACE ------------

#endif // CUBISMOFFSCREENSURFACE_OPENGLES2_H
