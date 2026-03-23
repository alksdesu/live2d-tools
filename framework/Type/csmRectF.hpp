#ifndef CSMRECTF_H
#define CSMRECTF_H



#include "framework/CubismFramework.hpp"

//--------- LIVE2D NAMESPACE ------------
namespace Live2D { namespace Cubism { namespace Framework {

/**
 * @brief   矩形形状(座標・長さはfloat値)を定義するクラス
 *
 */
class csmRectF
{
public:

    /**
     * @brief   コンストラクタ
     *
     */
    csmRectF();

    /**
     * @brief   引数付きコンストラクタ
     *
     * @param[in]   x   ->  左端X座標
     * @param[in]   y   ->  上端Y座標
     * @param[in]   w   ->  幅
     * @param[in]   h   ->  高さ
     */
    csmRectF(csmFloat32 x, csmFloat32 y, csmFloat32 w, csmFloat32 h);

    /**
     * @brief   デストラクタ
     *
     */
    virtual ~csmRectF();

    /**
     * @brief   矩形中央のX座標を取得する
     */
    csmFloat32 GetCenterX() const { return X + 0.5f * Width; }

    /**
     * @brief   矩形中央のY座標を取得する
     */
    csmFloat32 GetCenterY() const { return Y + 0.5f * Height; }

    /**
     * @brief   右端のX座標を取得する
     */
    csmFloat32 GetRight() const { return X + Width; }

    /**
     * @brief   下端のY座標を取得する
     */
    csmFloat32 GetBottom() const { return Y + Height; }

    /**
     * @brief   矩形に値をセットする
     *
     * @param[in]   r   ->  矩形のインスタンス
     */
    void SetRect(csmRectF* r);

    /**
     * @brief   矩形中央を軸にして縦横を拡縮する
     *
     * @param[in]   w   ->  幅方向に拡縮する量
     * @param[in]   h   ->  高さ方向に拡縮する量
     */
    void Expand(csmFloat32 w, csmFloat32 h);

    csmFloat32 X;       ///< 左端X座標
    csmFloat32 Y;       ///< 上端Y座標
    csmFloat32 Width;   ///< 幅
    csmFloat32 Height;  ///< 高さ
};
}}}

//------------------------- LIVE2D NAMESPACE ------------

#endif // CSMRECTF_H
