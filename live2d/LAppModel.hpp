/**
 * Copyright(c) Live2D Inc. All rights reserved.
 *
 * Use of this source code is governed by the Live2D Open Software license
 * that can be found at https://www.live2d.com/eula/live2d-open-software-license-agreement_en.html.
 */

#pragma once

#include <framework/CubismFramework.hpp>
#include <framework/Model/CubismUserModel.hpp>
#include <framework/ICubismModelSetting.hpp>
#include <framework/Type/csmRectF.hpp>
#include <framework/Rendering/OpenGL/CubismOffscreenSurface_OpenGLES2.hpp>

#include "LAppWavFileHandler.hpp"
#include <QRectF>

class LAppModel : public Csm::CubismUserModel
{
public:

    LAppModel();
    virtual ~LAppModel();
    void LoadAssets(const Csm::csmChar* dir, const  Csm::csmChar* fileName);
    void ReloadRenderer();
    void Update();
    void UpdateParaValue(const QString& parId,float value,bool IsInit);
    void Draw(Csm::CubismMatrix44& matrix);
    Csm::CubismMotionQueueEntryHandle StartMotion(const Csm::csmChar* group, Csm::csmInt32 no, Csm::csmInt32 priority, Csm::ACubismMotion::FinishedMotionCallback onFinishedMotionHandler = NULL);
    void StopAllMotions();
    Csm::CubismMotionQueueEntryHandle StartRandomMotion(const Csm::csmChar* group, Csm::csmInt32 priority, Csm::ACubismMotion::FinishedMotionCallback onFinishedMotionHandler = NULL);
    void SetExpression(const Csm::csmChar* expressionID);
    void SetRandomExpression();
    virtual void MotionEventFired(const Live2D::Cubism::Framework::csmString& eventValue);
    virtual Csm::csmBool HitTest(const Csm::csmChar* hitAreaName, Csm::csmFloat32 x, Csm::csmFloat32 y);
    Csm::Rendering::CubismOffscreenSurface_OpenGLES2& GetRenderBuffer();
    Csm::csmBool HasMocConsistencyFromFile(const Csm::csmChar* mocFileName);
    Csm::ICubismModelSetting* GetModelSetting(){
        return _modelSetting;
    }
    Csm::csmString  GetModelHomeDir(){

        return _modelHomeDir;
    }
    const QString& GetMocPathName(){
        return m_mocName;
    }
    QRectF GetCurModelRectF();

    void SetAutoEyeBlink(bool v) { _autoEyeBlink = v; }
    void SetAutoBreath(bool v) { _autoBreath = v; }
    void SetAutoPhysics(bool v) { _autoPhysics = v; }
    void SetMotionSpeed(float v);
    void SetMotionLoop(bool v);
    bool GetAutoPhysics() const { return _autoPhysics; }
    void SetEnableMouseFollow(bool v) { _enableMouseFollow = v; }
    bool GetEnableMouseFollow() const { return _enableMouseFollow; }
    void SetSnapshotMode(bool v) { _snapshotMode = v; }

    // 物理参数控制
    void SetPhysicsGravity(float x, float y);
    void SetPhysicsWind(float x, float y);

    // 物理组拖拽：设置拖拽驱动的参数列表
    void SetPhysicsDragParams(const QStringList& paramIds, const QStringList& types);
    void ApplyPhysicsDrag(float dx, float dy);

    // Part 可见性控制
    void SetPartVisible(const QString& partId, bool visible);
    const QSet<QString>& GetHiddenPartIds() const { return _hiddenPartIds; }

    // 变形器拖拽
    void SetDeformerDragParams(const QStringList& paramIds, const QList<float>& minVals,
                               const QList<float>& maxVals, const QList<float>& defaultVals);
    void ApplyDeformerDrag(float viewX, float viewY);
    void StopDeformerDrag() { _deformerDragActive = false; }
    const QStringList& GetDeformerDragParamIds() const { return _deformerDragParamIds; }
    // 运行时探测：找到影响指定 ArtMesh 的参数
    void ProbeParamsForArtMeshes(const QStringList& artMeshNames);

protected:
    void DoDraw();

private:

    void SetupModel(Csm::ICubismModelSetting* setting);

    void SetupTextures();


    void PreloadMotionGroup(const Csm::csmChar* group);

    void ReleaseMotionGroup(const Csm::csmChar* group) const;


    void ReleaseMotions();


    void ReleaseExpressions();

    Csm::ICubismModelSetting* _modelSetting; ///< モデルセッティング情報
    Csm::csmString _modelHomeDir; ///< モデルセッティングが置かれたディレクトリ
    Csm::csmFloat32 _userTimeSeconds; ///< デルタ時間の積算値[秒]
    Csm::csmVector<Csm::CubismIdHandle> _eyeBlinkIds; ///< モデルに設定されたまばたき機能用パラメータID
    Csm::csmVector<Csm::CubismIdHandle> _lipSyncIds; ///< モデルに設定されたリップシンク機能用パラメータID
    Csm::csmMap<Csm::csmString, Csm::ACubismMotion*>   _motions; ///< 読み込まれているモーションのリスト
    Csm::csmMap<Csm::csmString, Csm::ACubismMotion*>   _expressions; ///< 読み込まれている表情のリスト
    Csm::csmVector<Csm::csmRectF> _hitArea;
    Csm::csmVector<Csm::csmRectF> _userArea;
    const Csm::CubismId* _idParamAngleX; ///< パラメータID: ParamAngleX
    const Csm::CubismId* _idParamAngleY; ///< パラメータID: ParamAngleX
    const Csm::CubismId* _idParamAngleZ; ///< パラメータID: ParamAngleX
    const Csm::CubismId* _idParamBodyAngleX; ///< パラメータID: ParamBodyAngleX
    const Csm::CubismId* _idParamEyeBallX; ///< パラメータID: ParamEyeBallX
    const Csm::CubismId* _idParamEyeBallY; ///< パラメータID: ParamEyeBallXY

    LAppWavFileHandler _wavFileHandler; ///< wavファイルハンドラ

    Csm::Rendering::CubismOffscreenSurface_OpenGLES2  _renderBuffer;   ///< フレームバッファ以外の描画先

    QString m_mocName;
    QMap<QString,int> m_mapParaToIndex;
    bool _autoEyeBlink = true;
    bool _autoBreath = true;
    bool _autoPhysics = true;
    bool _enableMouseFollow = false;
    bool _snapshotMode = false;
    float _motionSpeed = 1.0f;
    bool _motionLoop = false;

    // 物理拖拽参数
    QStringList _physicsDragParamIds;
    QStringList _physicsDragTypes;

    // 变形器拖拽参数
    QStringList _deformerDragParamIds;
    QList<float> _deformerDragMinVals, _deformerDragMaxVals, _deformerDragDefaultVals;
    QList<float> _deformerDragValues; // 当前拖拽值
    bool _deformerDragActive = false;

    // Part 可见性控制
    QSet<QString> _hiddenPartIds;
};
