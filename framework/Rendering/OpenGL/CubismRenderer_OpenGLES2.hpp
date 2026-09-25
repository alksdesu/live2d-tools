#pragma once
#include <QOpenGLFunctions_3_3_Core>
#include "../CubismRenderer.hpp"
#include "../CubismClippingManager.hpp"
#include "framework/CubismFramework.hpp"
#include "framework/Rendering/OpenGL/CubismOffscreenSurface_OpenGLES2.hpp"
#include "framework/Rendering/OpenGL/CubismShader_OpenGLES2.hpp"
#include "framework/Type/csmVector.hpp"
#include "framework/Type/csmRectF.hpp"
#include "framework/Math/CubismVector2.hpp"
#include "framework/Type/csmMap.hpp"
#include "live2d/LAppDelegate.hpp"
#include <QDebug>
#include <QString>
#include <memory>
#include <vector>
#include "opengl/cglrenderstate.h"
#include "opengl/cglmeshbuffer.h"

class CubismRendererPreparationTests;

namespace Live2D { namespace Cubism { namespace Framework { namespace Rendering {

//  前方宣言
class CubismRenderer_OpenGLES2;
class CubismClippingContext_OpenGLES2;
class CubismShader_OpenGLES2;

/**
 * @brief  クリッピングマスクの処理を実行するクラス
 *
 */
class CubismClippingManager_OpenGLES2 : public CubismClippingManager<CubismClippingContext_OpenGLES2, CubismOffscreenSurface_OpenGLES2> ,QOpenGLFunctions_3_3_Core
{
public:


    bool setGlOwningContext(const QOpenGLContext *context){
        if (!context || QOpenGLContext::currentContext() != context || !context->isValid() ||
            (owningContext() && owningContext() != context)) return false;
        setOwningContext(context);
        return initializeOpenGLFunctions();
    }
    bool SetupClippingContext(CubismModel& model, CubismRenderer_OpenGLES2* renderer, const GLint lastViewport[4]);
};


class CubismClippingContext_OpenGLES2 : public CubismClippingContext
{
    friend class CubismClippingManager_OpenGLES2;
    friend class CubismRenderer_OpenGLES2;

public:
    /**
     * @brief   引数付きコンストラクタ
     *
     */
    CubismClippingContext_OpenGLES2(CubismClippingManager<CubismClippingContext_OpenGLES2, CubismOffscreenSurface_OpenGLES2>* manager, CubismModel& model, const csmInt32* clippingDrawableIndices, csmInt32 clipCount);

    /**
     * @brief   デストラクタ
     */
    virtual ~CubismClippingContext_OpenGLES2();

    /**
     * @brief   このマスクを管理するマネージャのインスタンスを取得する。
     *
     * @return  クリッピングマネージャのインスタンス
     */
    CubismClippingManager<CubismClippingContext_OpenGLES2, CubismOffscreenSurface_OpenGLES2>* GetClippingManager();

    CubismClippingManager<CubismClippingContext_OpenGLES2, CubismOffscreenSurface_OpenGLES2>* _owner;        ///< このマスクを管理しているマネージャのインスタンス
};

/**
 * @brief   Cubismモデルを描画する直前のOpenGLES2のステートを保持・復帰させるクラス
 *
 */
using CubismRendererProfile_OpenGLES2 = CGlRenderState;

/**
 * @brief   OpenGLES2用の描画命令を実装したクラス
 *
 */
class CubismRenderer_OpenGLES2 : public  CubismRenderer,  QOpenGLFunctions_3_3_Core
{
    friend class CubismRenderer;
    friend class CubismClippingManager_OpenGLES2;
    friend class CubismShader_OpenGLES2;
    friend class ::CubismRendererPreparationTests;

public:
    bool setGlOwningContext(const QOpenGLContext *context){
        if (!context || QOpenGLContext::currentContext() != context || !context->isValid() ||
            (owningContext() && owningContext() != context)) return false;
        setOwningContext(context);
        return initializeOpenGLFunctions();
    }

    /**
     * @brief    レンダラの初期化処理を実行する<br>
     *           引数に渡したモデルからレンダラの初期化処理に必要な情報を取り出すことができる
     *
     * @param[in]  model -> モデルのインスタンス
     */
    csmBool Initialize(Framework::CubismModel* model) override;

    csmBool Initialize(Framework::CubismModel* model, csmInt32 maskBufferCount) override;

    bool PrepareModelReplacement(CubismModel* model, const CubismRenderer_OpenGLES2& previous,
                                 QString* error = nullptr);

    /**
     * @brief   OpenGLテクスチャのバインド処理<br>
     *           CubismRendererにテクスチャを設定し、CubismRenderer中でその画像を参照するためのIndex値を戻り値とする
     *
     * @param[in]   modelTextureIndex  ->  セットするモデルテクスチャの番号
     * @param[in]   glTextureIndex     ->  OpenGLテクスチャの番号
     *
     */
    void BindTexture(csmUint32 modelTextureIndex, GLuint glTextureIndex);

    /**
     * @brief   OpenGLにバインドされたテクスチャのリストを取得する
     *
     * @return  テクスチャのアドレスのリスト
     */
    const csmMap<csmInt32, GLuint>& GetBindedTextures() const;

    /**
     * @brief  クリッピングマスクバッファのサイズを設定する<br>
     *         マスク用のOffscreenSurfaceを破棄・再作成するため処理コストは高い。
     *
     * @param[in]  size -> クリッピングマスクバッファのサイズ
     *
     */
    void SetClippingMaskBufferSize(csmFloat32 width, csmFloat32 height);

    /**
     * @brief  レンダーテクスチャの枚数を取得する。
     *
     * @return  レンダーテクスチャの枚数
     *
     */
    csmInt32 GetRenderTextureCount() const;

    /**
     * @brief  クリッピングマスクバッファのサイズを取得する
     *
     * @return クリッピングマスクバッファのサイズ
     *
     */
    CubismVector2 GetClippingMaskBufferSize() const;

    /**
     * @brief  クリッピングマスクのバッファを取得する
     *
     * @return クリッピングマスクのバッファへのポインタ
     *
     */
    CubismOffscreenSurface_OpenGLES2* GetMaskBuffer(csmInt32 index);

protected:
    /**
     * @brief   コンストラクタ
     */
    CubismRenderer_OpenGLES2();

    /**
     * @brief   デストラクタ
     */
    virtual ~CubismRenderer_OpenGLES2();

    /**
     * @brief   モデルを描画する実際の処理
     *
     */
    csmBool DoDrawModel() override;

    /**
     * @brief    描画オブジェクト（アートメッシュ）を描画する。
     *
     * @param[in]   model       ->  描画対象のモデル
     * @param[in]   index       ->  描画対象のメッシュのインデックス
     *
     */
    bool DrawMeshOpenGL(const CubismModel& model, const csmInt32 index,bool IsAll, csmFloat32 opacityOverride = -1.0f);

#ifdef CSM_TARGET_ANDROID_ES2
public:
    /**
     * @brief   Tegraプロセッサ対応。拡張方式による描画の有効・無効
     *
     * @param[in]   extMode     ->  trueなら拡張方式で描画する
     * @param[in]   extPAMode   ->  trueなら拡張方式のPA設定を有効にする
     */
    static void SetExtShaderMode(csmBool extMdoe, csmBool extPAMode = false);

    /**
     * @brief   Android-Tegra対応. シェーダプログラムをリロードする。
     */
    static void ReloadShader();
#endif

private:
    // Prevention of copy Constructor
    CubismRenderer_OpenGLES2(const CubismRenderer_OpenGLES2&);
    CubismRenderer_OpenGLES2& operator=(const CubismRenderer_OpenGLES2&);
    static void DoStaticRelease();
    struct PreparedMaskResources;
    bool PrepareMaskResources(CubismModel& model, csmInt32 count, const CubismVector2& size,
                              PreparedMaskResources& resources, QString* error);
    bool InitializeResources(CubismModel* model, csmInt32 maskBufferCount,
                             const CubismVector2& maskSize, bool preuploadGeometry, QString* error);
    bool UploadDrawableGeometry(const CubismModel& model, csmInt32 index, const float* positions,
                                std::unique_ptr<CGlMeshBuffer>& mesh, QString* error);
    void PreDraw();
    void PostDraw(){};
    csmBool SaveProfile() override;
    csmBool RestoreProfile() override;
    bool DrawHighPrecisionMask(CubismClippingContext_OpenGLES2* clipContext);
    bool WritePsdLayer(csmInt32 drawableIndex, const QString& drawName,
                       csmFloat32 opacityOverride, bool hasMask);
    void SetClippingContextBufferForMask(CubismClippingContext_OpenGLES2* clip);
    CubismClippingContext_OpenGLES2* GetClippingContextBufferForMask() const;
    void SetClippingContextBufferForDraw(CubismClippingContext_OpenGLES2* clip);
    CubismClippingContext_OpenGLES2* GetClippingContextBufferForDraw() const;
    const csmBool inline IsGeneratingMask() const;
    GLuint GetBindedTextureId(csmInt32 textureId);



    csmMap<csmInt32, GLuint> _textures;                      ///< モデルが参照するテクスチャとレンダラでバインドしているテクスチャとのマップ
    csmVector<csmInt32> _sortedDrawableIndexList;       ///< 描画オブジェクトのインデックスを描画順に並べたリスト
    CubismRendererProfile_OpenGLES2 _rendererProfile;               ///< OpenGLのステートを保持するオブジェクト
    CubismClippingManager_OpenGLES2* _clippingManager;               ///< クリッピングマスク管理オブジェクト
    CubismClippingContext_OpenGLES2* _clippingContextBufferForMask;  ///< マスクテクスチャに描画するためのクリッピングコンテキスト
    CubismClippingContext_OpenGLES2* _clippingContextBufferForDraw;  ///< 画面上描画するためのクリッピングコンテキスト

    std::vector<std::unique_ptr<CubismOffscreenSurface_OpenGLES2>> _offscreenSurfaces;
    std::vector<std::unique_ptr<CGlMeshBuffer>> _meshBuffers;
    quint64 _maskIndicesRevision = 0;
    bool _forceMaskRedraw = false;
    bool _maskTopologyUpdated = false;
    bool _maskBuffersNeedRedraw = false;
};

}}}}
//------------ LIVE2D NAMESPACE ------------
