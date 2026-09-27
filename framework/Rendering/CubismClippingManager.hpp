#ifndef CUBISMCLIPPINGMANAGER_H
#define CUBISMCLIPPINGMANAGER_H


#include <float.h>
#include <algorithm>
#include "framework/CubismFramework.hpp"
#include "framework/Type/csmVector.hpp"
#include "framework/Type/csmRectF.hpp"
#include "framework/Math/CubismVector2.hpp"
#include "framework/Math/CubismMatrix44.hpp"
#include "framework/Model/CubismModel.hpp"

//------------ LIVE2D NAMESPACE ------------
namespace Live2D { namespace Cubism { namespace Framework { namespace Rendering {

///< ファイルスコープの変数宣言
#ifndef CSM_MASK_FILESCOPE
#define CSM_MASK_FILESCOPE
namespace {
const csmInt32 ColorChannelCount = 4;   // 実験時に1チャンネルの場合は1、RGBだけの場合は3、アルファも含める場合は4
const csmInt32 ClippingMaskMaxCountOnDefault = 36;  // 通常のフレームバッファ1枚あたりのマスク最大数
const csmInt32 ClippingMaskMaxCountOnMultiRenderTexture = 32;   // フレームバッファが2枚以上ある場合のフレームバッファ1枚あたりのマスク最大数
}
#endif

template <class T_ClippingContext, class T_OffscreenSurface>
class CubismClippingManager
{
public:
    /**
     * @brief    コンストラクタ
     */
    CubismClippingManager();

    /**
     * @brief    デストラクタ
     */
    virtual ~CubismClippingManager();

    /**
     * @brief    マネージャの初期化処理<br>
     *           クリッピングマスクを使う描画オブジェクトの登録を行う
     *
     * @param[in]   model           ->  モデルのインスタンス
     * @param[in]   maskBufferCount ->  バッファの生成数
     */
    void Initialize(CubismModel& model, const csmInt32 maskBufferCount);

    T_ClippingContext* FindSameClip(const csmInt32* drawableMasks, csmInt32 drawableMaskCounts) const;
    void SetupMatrixForHighPrecision(CubismModel& model, csmBool isRightHanded);
    void SetupMaskMatrix(CubismModel& model, T_ClippingContext* context, csmBool highPrecision, csmBool isRightHanded = false);
    void createMatrixForMask(csmBool isRightHanded, csmRectF* layoutBoundsOnTex01, csmFloat32 scaleX, csmFloat32 scaleY);

    /**
     * @brief   クリッピングコンテキストを配置するレイアウト。<br>
     *           ひとつのレンダーテクスチャを極力いっぱいに使ってマスクをレイアウトする。<br>
     *           マスクグループの数が4以下ならRGBA各チャンネルに１つずつマスクを配置し、5以上6以下ならRGBAを2,2,1,1と配置する。
     *
     * @param[in]   usingClipCount  ->  配置するクリッピングコンテキストの数
     */
    void SetupLayoutBounds(csmInt32 usingClipCount) const;

    /**
     * @brief   マスクされる描画オブジェクト群全体を囲む矩形(モデル座標系)を計算する
     *
     * @param[in]   model            ->  モデルのインスタンス
     * @param[in]   clippingContext  ->  クリッピングマスクのコンテキスト
     */
    void CalcClippedDrawTotalBounds(CubismModel& model, T_ClippingContext* clippingContext);

    /**
     * @brief   画面描画に使用するクリッピングマスクのリストを取得する
     *
     * @return  画面描画に使用するクリッピングマスクのリスト
     */
    csmVector<T_ClippingContext*>* GetClippingContextListForDraw();

    /**
     *@brief  クリッピングマスクバッファのサイズを取得する
     *
     *@return クリッピングマスクバッファのサイズ
     */
    CubismVector2 GetClippingMaskBufferSize() const;

    /**
     * このバッファのレンダーテクスチャの枚数を取得する。
     *
     * @return このバッファのレンダーテクスチャの枚数
     */
    csmInt32 GetRenderTextureCount();

    /**
     * @brief   カラーチャンネル(RGBA)のフラグを取得する
     *
     * @param[in]   channelIndex   ->   カラーチャンネル(RGBA)の番号(0:R , 1:G , 2:B, 3:A)
     */
    CubismRenderer::CubismTextureColor* GetChannelFlagAsColor(csmInt32 channelIndex);

    /**
     *@brief  クリッピングマスクバッファのサイズを設定する
     *
     *@param  size -> クリッピングマスクバッファのサイズ
     *
     */
    void SetClippingMaskBufferSize(csmFloat32 width, csmFloat32 height);

protected:
    T_OffscreenSurface* _currentMaskBuffer; /// オフスクリーンサーフェイスのアドレス
    csmVector<csmBool> _clearedMaskBufferFlags; /// マスクのクリアフラグの配列

    csmVector<CubismRenderer::CubismTextureColor*> _channelColors;
    csmVector<T_ClippingContext*> _clippingContextListForMask;   ///< マスク用クリッピングコンテキストのリスト
    csmVector<T_ClippingContext*> _clippingContextListForDraw;   ///< 描画用クリッピングコンテキストのリスト
    CubismVector2 _clippingMaskBufferSize;       ///< クリッピングマスクのバッファサイズ（初期値:256）
    csmInt32 _renderTextureCount;           ///< 生成するレンダーテクスチャの枚数

    CubismMatrix44 _tmpMatrix;              ///< マスク計算用の行列
    CubismMatrix44 _tmpMatrixForMask;       ///< マスク計算用の行列
    CubismMatrix44 _tmpMatrixForDraw;       ///< マスク計算用の行列
    csmRectF _tmpBoundsOnModel;       ///< マスク配置計算用の矩形
};

#include "CubismClippingManager.tpp"
}}}}

//------------ LIVE2D NAMESPACE ------------

#endif // CUBISMCLIPPINGMANAGER_H
