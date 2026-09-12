#ifndef CUBISMMODEL_H
#define CUBISMMODEL_H



#include "framework/CubismFramework.hpp"
#include "framework/Type/csmMap.hpp"
#include "framework/Type/csmVector.hpp"
#include "framework/Rendering/CubismRenderer.hpp"
#include "framework/Id/CubismId.hpp"
#include "edit/cmeshmap.h"
#include <QMap>
#include <QHash>

namespace live2dstruct {
struct CDrawableOrderGroup;
}

namespace Live2D { namespace Cubism { namespace Framework {

class CubismMoc;


class CubismModel
{
    friend class CubismMoc;
public:

    void InitMesh();
    void DrawMesh(const QString& drawKey,const QMatrix4x4& mvp);//绘制网格，使用mvp矩阵
    CMeshMap* GetMeshMap(){
        return &m_vetDrawMesh;
    }


    struct DrawableColorData
    {
        DrawableColorData(): IsOverwritten(false), Color() {};
        DrawableColorData(csmBool isOverwritten, Rendering::CubismRenderer::CubismTextureColor color): IsOverwritten(isOverwritten), Color(color) {};
        virtual ~DrawableColorData() {};
        csmBool IsOverwritten;
        Rendering::CubismRenderer::CubismTextureColor Color;

    };
    struct DrawableCullingData
    {

        DrawableCullingData()
            : IsOverwritten(false)
            , IsCulling(0) {};

        DrawableCullingData(csmBool isOverwritten, csmInt32 isCulling)
            : IsOverwritten(isOverwritten)
            , IsCulling(isCulling) {};


        virtual ~DrawableCullingData() {};

        csmBool IsOverwritten;
        csmInt32 IsCulling;

    };  // DrawableCullingData


    struct PartColorData
    {

        PartColorData()
            : IsOverwritten(false)
            , Color() {};


        PartColorData(csmBool isOverwritten, Rendering::CubismRenderer::CubismTextureColor color)
            : IsOverwritten(isOverwritten)
            , Color(color) {};

        virtual ~PartColorData() {};

        csmBool IsOverwritten;
        Rendering::CubismRenderer::CubismTextureColor Color;

    };  // PartColorData


    void    Update() const;
    csmFloat32  GetCanvasWidthPixel() const;
    csmFloat32  GetCanvasHeightPixel() const;
    csmFloat32  GetPixelsPerUnit() const;
    csmFloat32  GetCanvasWidth() const;
    csmFloat32  GetCanvasHeight() const;
    csmFloat32  GetCanvasOriginInPixelsWidth() const;
    csmFloat32  GetCanvasOriginInPixelsHeight() const;
    csmFloat32  GetCanvasPixelsWidth() const;
    csmFloat32  GetCanvasPixelsHeight() const;
    csmInt32    GetPartIndex(CubismIdHandle partId);
    CubismIdHandle    GetPartId(csmUint32 partIndex);
    csmInt32    GetPartCount() const;
    void        SetPartOpacity(CubismIdHandle partId, csmFloat32 opacity);
    void        SetPartOpacity(csmInt32 partIndex, csmFloat32 opacity);
    csmFloat32  GetPartOpacity(CubismIdHandle partId);
    csmFloat32  GetPartOpacity(csmInt32 partIndex);
    csmInt32    GetParameterIndex(CubismIdHandle parameterId);
    CubismIdHandle    GetParameterId(csmUint32 parameterIndex);
    const CubismIdHandle GetParameterId(csmUint32 parameterIndex) const;
    csmInt32    GetParameterCount() const;
    Core::csmParameterType GetParameterType(csmUint32 parameterIndex) const;
    csmFloat32  GetParameterMaximumValue(csmUint32 parameterIndex) const;
    csmFloat32  GetParameterMinimumValue(csmUint32 parameterIndex) const;
    csmFloat32  GetParameterDefaultValue(csmUint32 parameterIndex) const;
    csmFloat32  GetParameterValue(CubismIdHandle parameterId);
    csmFloat32  GetParameterValue(csmInt32 parameterIndex);
    void        SetParameterValue(CubismIdHandle parameterId, csmFloat32 value, csmFloat32 weight = 1.0f);
    void        SetParameterValue(csmInt32 parameterIndex, csmFloat32 value, csmFloat32 weight = 1.0f);
    void        AddParameterValue(CubismIdHandle parameterId, csmFloat32 value, csmFloat32 weight = 1.0f);
    void        AddParameterValue(csmInt32 parameterIndex, csmFloat32 value, csmFloat32 weight = 1.0f);
    void        MultiplyParameterValue(CubismIdHandle parameterId, csmFloat32 value, csmFloat32 weight = 1.0f);
    void        MultiplyParameterValue(csmInt32 parameterIndex, csmFloat32 value, csmFloat32 weight = 1.0f);
    csmInt32            GetDrawableIndex(CubismIdHandle drawableId) const;
    csmInt32            GetDrawableCount() const;
    CubismIdHandle      GetDrawableId(csmInt32 drawableIndex) const;
    const csmInt32*     GetDrawableRenderOrders() const;
    const csmInt32*     GetDrawableDrawOrders() const;
    QVector<int> GetSortedDrawableIndices(const QHash<QString, int>& overrides = {}) const;
    QVector<int> GetSortedDrawableIndices(const QHash<QString, int>& overrides,
                                         const QHash<QString, int>& groupIndices,
                                         const QVector<live2dstruct::CDrawableOrderGroup>& groups) const;
    csmInt32            GetDrawableTextureIndices(csmInt32 drawableIndex) const;
    csmInt32            GetDrawableTextureIndex(csmInt32 drawableIndex) const;
    csmInt32            GetDrawableVertexIndexCount(csmInt32 drawableIndex) const;
    csmInt32            GetDrawableVertexCount(csmInt32 drawableIndex) const;

    const csmFloat32*   GetDrawableVertices(csmInt32 drawableIndex) const;

    /**
     * @brief Drawableの頂点インデックスリストの取得
     *
     * Drawableの頂点インデックスリストを取得する。
     *
     * @param[in]   drawableIndex   Drawableのインデックス
     * @return  Drawableの頂点インデックスリスト
     */
    const csmUint16*            GetDrawableVertexIndices(csmInt32 drawableIndex) const;

    /**
     * @brief Drawableの頂点リストの取得
     *
     * Drawableの頂点リストを取得する。
     *
     * @param[in]   drawableIndex   Drawableのインデックス
     * @return  Drawableの頂点リスト
     */
    const Core::csmVector2*     GetDrawableVertexPositions(csmInt32 drawableIndex) const;

    /**
     * @brief Drawableの頂点のUVリストの取得
     *
     * Drawableの頂点のUVリストを取得する。
     *
     * @param[in]   drawableIndex   Drawableのインデックス
     * @return  Drawableの頂点のUVリスト
     */
    const Core::csmVector2*     GetDrawableVertexUvs(csmInt32 drawableIndex) const;

    /**
     * @brief Drawableの不透明度の取得
     *
     * Drawableの不透明度を取得する。
     *
     * @param[in]   drawableIndex   Drawableのインデックス
     * @return  Drawableの不透明度
     */
    csmFloat32                  GetDrawableOpacity(csmInt32 drawableIndex) const;


    csmFloat32                  GetDrawableOpacityAll(csmInt32 drawableIndex,bool IsALL) const;

    /**
     * @brief Drawableの乗算色の取得
     *
     * Drawableの乗算色を取得する。
     *
     * @param[in]   drawableIndex   Drawableのインデックス
     * @return  Drawableの乗算色
     */
    Core::csmVector4 GetDrawableMultiplyColor(csmInt32 drawableIndex) const;

    /**
     * @brief Drawableのスクリーン色の取得
     *
     * Drawableのスクリーン色を取得する。
     *
     * @param[in]   drawableIndex   Drawableのインデックス
     * @return  Drawableのスクリーン色
     */
    Core::csmVector4 GetDrawableScreenColor(csmInt32 drawableIndex) const;

    /**
     * @brief Drawableの親パーツのインデックスの取得
     *
     * Drawableの親パーツのインデックスを取得する。
     *
     * @param[in]   drawableIndex   Drawableのインデックス
     * @return drawableの親パーツのインデックス
     */
    csmInt32 GetDrawableParentPartIndex(csmUint32 drawableIndex) const;

    /**
     * @brief Drawableのブレンドモードの取得
     *
     * Drawableのブレンドモードを取得する。
     *
     * @param[in]   drawableIndex   Drawableのインデックス
     * @return  Drawableのブレンドモード
     */
    Rendering::CubismRenderer::CubismBlendMode   GetDrawableBlendMode(csmInt32 drawableIndex) const;

    /**
    * @brief Drawableのマスクの反転使用の取得
    *
    * Drawableのマスク使用時の反転設定を取得する。
    * マスクを使用しない場合は無視される
    *
    * @param[in]   drawableIndex   Drawableのインデックス
    * @return  Drawableのマスクの反転設定
    */
    csmBool                    GetDrawableInvertedMask(csmInt32 drawableIndex) const;

    /**
    * @brief Drawableの表示情報の取得
    *
    * Drawableの表示情報を取得する。
    *
    * @param[in]   drawableIndex   Drawableのインデックス
    * @return  true    Drawableが表示
    * @retval  false   Drawableが非表示
    */
    csmBool                  GetDrawableDynamicFlagIsVisible(csmInt32 drawableIndex) const;

    /**
    * @brief Drawableの表示状態の変化の取得
    *
    * 直近のCubismModel::Update関数でDrawableの表示状態が変化したかを取得する。
    *
    * @param[in]   drawableIndex   Drawableのインデックス
    * @retval  true    Drawableの表示状態が直近のCubismModel::Update関数で変化した
    * @retval  false   Drawableの表示状態が直近のCubismModel::Update関数で変化していない
    */
    csmBool                  GetDrawableDynamicFlagVisibilityDidChange(csmInt32 drawableIndex) const;

    /**
    * @brief Drawableの不透明度の変化情報の取得
    *
    * 直近のCubismModel::Update関数でDrawableの不透明度が変化したかを取得する。
    *
    * @param[in]   drawableIndex   Drawableのインデックス
    * @retval  true    Drawableの不透明度が直近のCubismModel::Update関数で変化した
    * @retval  false   Drawableの不透明度が直近のCubismModel::Update関数で変化していない
    */
    csmBool                  GetDrawableDynamicFlagOpacityDidChange(csmInt32 drawableIndex) const;

    /**
    * @brief DrawableのDrawOrderの変化情報の取得
    *
    * 直近のCubismModel::Update関数でDrawableのDrawOrderが変化したかを取得する。
    * DrawOrderはArtMesh上で指定する0から1000の情報
    *
    * @param[in]   drawableIndex   Drawableのインデックス
    * @retval  true    Drawableの不透明度が直近のCubismModel::Update関数で変化した
    * @retval  false   Drawableの不透明度が直近のCubismModel::Update関数で変化していない
    */
    csmBool                  GetDrawableDynamicFlagDrawOrderDidChange(csmInt32 drawableIndex) const;

    /**
    * @brief Drawableの描画順序の変化情報の取得
    *
    * 直近のCubismModel::Update関数でDrawableの描画の順序が変化したかを取得する。
    *
    * @param[in]   drawableIndex   Drawableのインデックス
    * @retval  true    Drawableの描画の順序が直近のCubismModel::Update関数で変化した
    * @retval  false   Drawableの描画の順序が直近のCubismModel::Update関数で変化していない
    */
    csmBool                  GetDrawableDynamicFlagRenderOrderDidChange(csmInt32 drawableIndex) const;

    /**
    * @brief DrawableのVertexPositionsの変化情報の取得
    *
    * 直近のCubismModel::Update関数でDrawableの頂点情報が変化したかを取得する。
    *
    * @param[in]   drawableIndex   Drawableのインデックス
    * @retval  true    Drawableの頂点情報が直近のCubismModel::Update関数で変化した
    * @retval  false   Drawableの頂点情報が直近のCubismModel::Update関数で変化していない
    */
    csmBool                  GetDrawableDynamicFlagVertexPositionsDidChange(csmInt32 drawableIndex) const;

    /**
    * @brief Drawableの乗算色・スクリーン色の変化情報の取得
    *
    * 直近のCubismModel::Update関数でDrawableの乗算色・スクリーン色が変化したかを取得する。
    *
    * @param[in]   drawableIndex   Drawableのインデックス
    * @retval  true    Drawableの乗算色・スクリーン色が直近のCubismModel::Update関数で変化した
    * @retval  false   Drawableの乗算色・スクリーン色が直近のCubismModel::Update関数で変化していない
    */
    csmBool                  GetDrawableDynamicFlagBlendColorDidChange(csmInt32 drawableIndex) const;

    /**
     * @brief Drawableのクリッピングマスクリストの取得
     *
     * Drawableのクリッピングマスクリストを取得する。
     *
     * @return  Drawableのクリッピングマスクリスト
     */
    const csmInt32**            GetDrawableMasks() const;

    /**
     * @brief Drawableのクリッピングマスクの個数リストの取得
     *
     * Drawableのクリッピングマスクの個数リストを取得する。
     *
     * @return  Drawableのクリッピングマスクの個数リスト
     */
    const csmInt32*             GetDrawableMaskCounts() const;
    csmBool     IsUsingMasking() const;
    void    LoadParameters();
    void    SaveParameters();
    Rendering::CubismRenderer::CubismTextureColor GetMultiplyColor(csmInt32 drawableIndex) const;
    Rendering::CubismRenderer::CubismTextureColor GetScreenColor(csmInt32 drawableIndex) const;

    /**
     * @brief   drawableの乗算色を設定する
     */
    void SetMultiplyColor(csmInt32 drawableIndex, const Rendering::CubismRenderer::CubismTextureColor& color);

    /**
     * @brief   drawableの乗算色を設定する
     */
    void SetMultiplyColor(csmInt32 drawableIndex, csmFloat32 r, csmFloat32 g, csmFloat32 b, csmFloat32 a = 1.0f);

    /**
     * @brief   drawableのスクリーン色を設定する
     */
    void SetScreenColor(csmInt32 drawableIndex, const Rendering::CubismRenderer::CubismTextureColor& color);

    /**
     * @brief   drawableのスクリーン色を設定する
     */
    void SetScreenColor(csmInt32 drawableIndex, csmFloat32 r, csmFloat32 g, csmFloat32 b, csmFloat32 a = 1.0f);

    /**
     * @brief   partの乗算色を取得する
     */
    Rendering::CubismRenderer::CubismTextureColor GetPartMultiplyColor(csmInt32 partIndex) const;

    /**
     * @brief   partの乗算色を取得する
     */
    Rendering::CubismRenderer::CubismTextureColor GetPartScreenColor(csmInt32 partIndex) const;

    /**
     * @brief   partのスクリーン色を設定する
     */
    void SetPartMultiplyColor(csmInt32 partIndex, const Rendering::CubismRenderer::CubismTextureColor& color);

    /**
     * @brief   partの乗算色を設定する
     */
    void SetPartMultiplyColor(csmInt32 partIndex, csmFloat32 r, csmFloat32 g, csmFloat32 b, csmFloat32 a = 1.0f);

    /**
     * @brief   partのスクリーン色を設定する
     */
    void SetPartScreenColor(csmInt32 partIndex, const Rendering::CubismRenderer::CubismTextureColor& color);

    /**
     * @brief   partのスクリーン色を設定する
     */
    void SetPartScreenColor(csmInt32 partIndex, csmFloat32 r, csmFloat32 g, csmFloat32 b, csmFloat32 a = 1.0f);

    /**
     * @brief SDKからモデル全体の乗算色を上書きするか。
     *
     * @retval  true    ->  SDK上の色情報を使用
     * @retval  false   ->  モデルの色情報を使用
     */
    csmBool GetOverwriteFlagForModelMultiplyColors() const;

    /**
     * @brief SDKからモデル全体のスクリーン色を上書きするか。
     *
     * @retval  true    ->  SDK上の色情報を使用
     * @retval  false   ->  モデルの色情報を使用
     */
    csmBool GetOverwriteFlagForModelScreenColors() const;

    /**
     * @brief SDKからモデル全体の乗算色を上書きするかをセットする
     *        SDK上の色情報を使うならtrue、モデルの色情報を使うならfalse
     */
    void SetOverwriteFlagForModelMultiplyColors(csmBool value);

    /**
     * @brief SDKからモデル全体のスクリーン色を上書きするかをセットする
     *        SDK上の色情報を使うならtrue、モデルの色情報を使うならfalse
     */
    void SetOverwriteFlagForModelScreenColors(csmBool value);

    /**
     * @brief SDKからdrawableの乗算色を上書きするか。
     *
     * @retval  true    ->  SDK上の色情報を使用
     * @retval  false   ->  モデルの色情報を使用
     */
    csmBool GetOverwriteFlagForDrawableMultiplyColors(csmInt32 drawableIndex) const;

    /**
     * @brief SDKからdrawableのスクリーン色を上書きするか。
     *
     * @retval  true    ->  SDK上の色情報を使用
     * @retval  false   ->  モデルの色情報を使用
     */
    csmBool GetOverwriteFlagForDrawableScreenColors(csmInt32 drawableIndex) const;

    /**
     * @brief SDKからdrawableの乗算色を上書きするかをセットする
     *        SDK上の色情報を使うならtrue、モデルの色情報を使うならfalse
     */
    void SetOverwriteFlagForDrawableMultiplyColors(csmUint32 drawableIndex, csmBool value);

    /**
     * @brief SDKからdrawableのスクリーン色を上書きするかをセットする
     *        SDK上の色情報を使うならtrue、モデルの色情報を使うならfalse
     */
    void SetOverwriteFlagForDrawableScreenColors(csmUint32 drawableIndex, csmBool value);

    /**
     * @brief SDKからpartの乗算色を上書きするか。
     *
     * @retval  true    ->  SDK上の色情報を使用
     * @retval  false   ->  モデルの色情報を使用
     */
    csmBool GetOverwriteColorForPartMultiplyColors(csmInt32 partIndex) const;

    /**
     * @brief SDKからpartのスクリーン色を上書きするか。
     *
     * @retval  true    ->  SDK上の色情報を使用
     * @retval  false   ->  モデルの色情報を使用
     */
    csmBool GetOverwriteColorForPartScreenColors(csmInt32 partIndex) const;

    /**
     * @brief SDKからpartの乗算色を上書きするかをセットする
     *        SDK上の色情報を使うならtrue、モデルの色情報を使うならfalse
     */
    void SetOverwriteColorForPartMultiplyColors(csmUint32 partIndex, csmBool value);

    /**
     * @brief SDKからpartのスクリーン色を上書きするかをセットする
     *        SDK上の色情報を使うならtrue、モデルの色情報を使うならfalse
     */
    void SetOverwriteColorForPartScreenColors(csmUint32 partIndex, csmBool value);

    /**
     * @brief Drawableのカリング情報の取得
     *
     * Drawableのカリング情報を取得する。
     *
     * @param[in]   drawableIndex   Drawableのインデックス
     * @return  Drawableのカリング情報
     */
    csmInt32 GetDrawableCulling(csmInt32 drawableIndex) const;

    /**
     * @brief   Drawableのカリング情報を設定する
     */
    void SetDrawableCulling(csmInt32 drawableIndex, csmInt32 isCulling);

    /**
     * @brief SDKからモデル全体のカリング設定を上書きするか。
     *
     * @retval  true    ->  SDK上のカリング設定を使用
     * @retval  false   ->  モデルのカリング設定を使用
     */
    csmBool GetOverwriteFlagForModelCullings() const;

    /**
     * @brief SDKからモデル全体のカリング設定を上書きするかをセットする
     *        SDK上のカリング設定を使うならtrue、モデルのカリング設定を使うならfalse
     */
    void SetOverwriteFlagForModelCullings(csmBool value);

    /**
     * @brief SDKからdrawableのカリング設定を上書きするか。
     *
     * @retval  true    ->  SDK上のカリング設定を使用
     * @retval  false   ->  モデルのカリング設定を使用
     */
    csmBool GetOverwriteFlagForDrawableCullings(csmInt32 drawableIndex) const;

    /**
     * @brief SDKからdrawableのカリング設定を上書きするかをセットする
     *        SDK上のカリング設定を使うならtrue、モデルのカリング設定を使うならfalse
     */
    void SetOverwriteFlagForDrawableCullings(csmUint32 drawableIndex, csmBool value);
    csmFloat32 GetModelOpacity();
    void SetModelOpacity(csmFloat32 value);

    Core::csmModel*     GetModel() const;

private:
    CubismModel(Core::csmModel* model);
    virtual ~CubismModel();
    CubismModel(const CubismModel&);
    CubismModel& operator=(const CubismModel&);
    void Initialize();
    void SetPartColor(
        csmUint32 partIndex,
        csmFloat32 r, csmFloat32 g, csmFloat32 b, csmFloat32 a,
        csmVector<PartColorData>& partColors,
        csmVector <DrawableColorData>& drawableColors);
    void SetOverwriteColorForPartColors(
        csmUint32 partIndex,
        csmBool value,
        csmVector<CubismModel::PartColorData>& partColors,
        csmVector <CubismModel::DrawableColorData>& drawableColors);

    csmMap<csmInt32, csmFloat32>        _notExistPartOpacities;             ///< 存在していないパーツの不透明度のリスト
    csmMap<CubismIdHandle, csmInt32>   _notExistPartId;                    ///< 存在していないパーツIDのリスト

    csmMap<csmInt32, csmFloat32>        _notExistParameterValues;           ///< 存在していないパラメータの値のリスト

    csmMap<CubismIdHandle, csmInt32>   _notExistParameterId;

    csmVector<csmFloat32>   _savedParameters;                   ///< 保存されたパラメータ

    Core::csmModel*     _model;                                 ///< モデル

    csmFloat32*         _parameterValues;                       ///< パラメータの値のリスト
    const csmFloat32*   _parameterMaximumValues;                ///< パラメータの最大値のリスト
    const csmFloat32*   _parameterMinimumValues;                ///< パラメータの最小値のリスト

    csmFloat32*         _partOpacities;                         ///< パーツの不透明度のリスト

    csmFloat32 _modelOpacity;                         ///< モデルの不透明度

    csmVector<CubismIdHandle> _parameterIds;
    csmVector<CubismIdHandle> _partIds;
    csmVector<CubismIdHandle> _drawableIds;
    csmVector<DrawableColorData> _userScreenColors; ///< Drawable 乗算色の配列
    csmVector<DrawableColorData> _userMultiplyColors; ///< Drawable スクリーン色の配列
    csmVector<DrawableCullingData> _userCullings; ///< カリング設定の配列
    csmVector<PartColorData> _userPartScreenColors; ///< Part 乗算色の配列
    csmVector<PartColorData> _userPartMultiplyColors; ///< Part スクリーン色の配列
    csmVector<csmVector<csmUint32> > _partChildDrawables; ///< Partの子DrawableIndexの配列
    csmBool _isOverwrittenModelMultiplyColors; ///< 乗算色を全て上書きするか？
    csmBool _isOverwrittenModelScreenColors; ///< スクリーン色を全て上書きするか？
    csmBool _isOverwrittenCullings; ///< モデルのカリング設定をすべて上書きするか？

    CMeshMap  m_vetDrawMesh;


};

}}}

#endif // CUBISMMODEL_H
