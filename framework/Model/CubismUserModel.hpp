#ifndef CUBISMUSERMODEL_H
#define CUBISMUSERMODEL_H


#include "framework/Effect/CubismPose.hpp"
#include "framework/Effect/CubismEyeBlink.hpp"
#include "framework/Effect/CubismBreath.hpp"
#include "framework/Math/CubismModelMatrix.hpp"
#include "framework/Math/CubismTargetPoint.hpp"
#include "framework/Model/CubismMoc.hpp"
#include "framework/Model/CubismModel.hpp"
#include "framework/Model/CubismModelReplacement.hpp"
#include "framework/Motion/CubismMotionManager.hpp"
#include "framework/Motion/CubismExpressionMotion.hpp"
#include "framework/Physics/CubismPhysics.hpp"
#include "framework/Rendering/CubismRenderer.hpp"
#include "framework/Model/CubismModelUserData.hpp"
#include "framework/Motion/CubismExpressionMotionManager.hpp"

namespace Live2D { namespace Cubism { namespace Framework {

/**
 * @brief ユーザーが実際に使用するモデル
 *
 * ユーザーが実際に使用するモデルの基底クラス。これを継承してユーザーが実装する。
 */
class CubismUserModel
{
public:
    /**
     * @brief コンストラクタ
     *
     * コンストラクタ。
     */
    CubismUserModel();

    /**
     * @brief デストラクタ
     *
     * デストラクタ。
     */
    virtual ~CubismUserModel();

    /**
     * @brief 初期化状態の取得
     *
     * 初期化されている状態か？
     *
     * @retval  true    初期化されている
     * @retval  false   初期化されていない
     */
    virtual csmBool         IsInitialized();


    virtual void            IsInitialized(csmBool v);


    virtual csmBool         IsUpdating();


    virtual void            IsUpdating(csmBool v);


    virtual void            SetDragging(csmFloat32 x, csmFloat32 y);

    virtual void            SetAcceleration(csmFloat32 x, csmFloat32 y, csmFloat32 z);


    CubismModelMatrix*      GetModelMatrix() const;


    virtual void            SetOpacity(csmFloat32 a);

    virtual csmFloat32      GetOpacity();

    virtual csmBool         LoadModel(const csmByte* buffer, csmSizeInt size, csmBool shouldCheckMocConsistency = false);

    virtual ACubismMotion*  LoadMotion(const csmByte* buffer, csmSizeInt size, const csmChar* name, ACubismMotion::FinishedMotionCallback onFinishedMotionHandler = NULL);

    virtual ACubismMotion*   LoadExpression(const csmByte* buffer, csmSizeInt size, const csmChar* name);

    /**
     * @brief ポーズデータの読み込み
     *
     * ポーズデータを読み込む。
     *
     * @param[in]   buffer  pose3.jsonが読み込まれているバッファ
     * @param[in]   size    バッファのサイズ
     */
    virtual void            LoadPose(const csmByte* buffer, csmSizeInt size);

    /**
     * @brief 物理演算データの読み込み
     *
     * 物理演算データを読み込む。
     *
     * @param[in]   buffer  physics3.jsonが読み込まれているバッファ
     * @param[in]   size    バッファのサイズ
     */
    virtual void            LoadPhysics(const csmByte* buffer, csmSizeInt size);

    /**
    * @brief モデルに付属するユーザーデータを読み込む
    *
    * ユーザーデータを読み込む。
    *
    * @param[in]   buffer  userdata3.jsonが読み込まれているバッファ
    * @param[in]   size    バッファのサイズ
    */
    virtual void            LoadUserData(const csmByte* buffer, csmSizeInt size);


    virtual csmBool         IsHit(CubismIdHandle drawableId, csmFloat32 pointX, csmFloat32 pointY);


    CubismModel*            GetModel() const;

    template<class T> T*    GetRenderer() { return dynamic_cast<T*>(_renderer); }


    csmBool CreateRenderer(csmInt32 maskBufferCount = 1);


    void DeleteRenderer();

    virtual void   MotionEventFired(const csmString& eventValue);


    static void   CubismDefaultMotionEventCallback(const CubismMotionQueueManager* caller, const csmString& eventValue, void* customData);
protected:
    bool ReplaceModel(const csmByte* bytes, csmSizeInt size,
                      const CubismModelReplacement::RendererPreparation& prepareRenderer,
                      QString* error = nullptr,
                      const CubismModelReplacement::ModelPreparation& prepareModel = {});
    bool ReplaceModel(const csmByte* bytes, csmSizeInt size,
                      const CubismModelReplacement::RendererPreparation& prepareRenderer,
                      CubismModelUpdatePolicy policy, QString* error = nullptr,
                      const CubismModelReplacement::ModelPreparation& prepareModel = {});

    CubismMoc*              _moc;                       ///< Mocデータ
    CubismModel*            _model;                     ///< Modelインスタンス

    CubismMotionManager*    _motionManager;             ///< モーション管理
    CubismExpressionMotionManager*    _expressionManager;         ///< 表情管理
    CubismEyeBlink*         _eyeBlink;                  ///< 自動まばたき
    CubismBreath*           _breath;                    ///< 呼吸
    CubismModelMatrix*      _modelMatrix;               ///< モデル行列
    CubismPose*             _pose;                      ///< ポーズ管理
    CubismTargetPoint*      _dragManager;               ///< マウスドラッグ
    CubismPhysics*          _physics;                   ///< 物理演算
    CubismModelUserData*    _modelUserData;             ///< ユーザデータ

    csmBool     _initialized;                   ///< 初期化されたかどうか
    csmBool     _updating;                      ///< 更新されたかどうか
    csmFloat32  _opacity;                       ///< 不透明度
    csmBool     _lipSync;                       ///< リップシンクするかどうか
    csmFloat32  _lastLipSyncValue;              ///< 最後のリップシンクの制御値
    csmFloat32  _dragX;                         ///< マウスドラッグのX位置
    csmFloat32  _dragY;                         ///< マウスドラッグのY位置
    csmFloat32  _accelerationX;                 ///< X軸方向の加速度
    csmFloat32  _accelerationY;                 ///< Y軸方向の加速度
    csmFloat32  _accelerationZ;                 ///< Z軸方向の加速度
    csmBool     _mocConsistency;                ///< MOC3整合性検証するかどうか
    csmBool     _debugMode;                     ///< デバッグモードかどうか

private:
    Rendering::CubismRenderer* _renderer;       ///< レンダラ
};

}}}

#endif // CUBISMUSERMODEL_H
