/**
 * Copyright(c) Live2D Inc. All rights reserved.
 *
 * Use of this source code is governed by the Live2D Open Software license
 * that can be found at https://www.live2d.com/eula/live2d-open-software-license-agreement_en.html.
 */

#pragma once
#include <QOpenGLFunctions_3_3_Core>
#include <framework/Math/CubismMatrix44.hpp>
#include <framework/Math/CubismViewMatrix.hpp>
#include "framework/CubismFramework.hpp"
#include <framework/Rendering/OpenGL/CubismOffscreenSurface_OpenGLES2.hpp>
#include <QMatrix4x4>
#include "edit/cdrawrect.h"
#include "edit/cdrawwarp.h"
#include "edit/cdrawrotation.h"
#include "live2dstruct/ctreemanage.h"

class TouchManager;
class LAppSprite;
class LAppModel;


class LAppView
{
public:

    enum SelectTarget
    {
        SelectTarget_None,                ///< デフォルトのフレームバッファにレンダリング
        SelectTarget_ModelFrameBuffer,    ///< LAppModelが各自持つフレームバッファにレンダリング
        SelectTarget_ViewFrameBuffer,     ///< LAppViewの持つフレームバッファにレンダリング
    };

    LAppView();
    ~LAppView();

    void Initialize();
    void Render();
    void InitializeSprite();
    void ResizeSprite();

    void PreModelDraw(LAppModel& refModel);
    void PostModelDraw(LAppModel& refModel);
    float GetSpriteAlpha(int assign) const;
    void SwitchRenderingTarget(SelectTarget targetType);
    void SetRenderTargetClearColor(float r, float g, float b);

    //来自世界坐标系的
    QString GetSelectMesh(const QVector2D& pos,bool IsOver);

    QVector4D GetWorldPoint(int posX, int posY);
    QMatrix4x4 GetProjectionView();
    QMatrix4x4 GetMVP();
    CDrawRect*  GetSelectrect();
    CDrawRect* GetMouseSelectRect();
    void SetViewMatrix(const QMatrix4x4& temp);
    QMatrix4x4 GetViewMatrix();
    void InitWraRotationKey(CWarpNode* node);
    void AddWrapKey(CWarpNode* node);
    void AddWrapKeyTest(CWarpNode* node);
    void AddRotationKey(CRotationNode* node);
    void InitWrapAndRotation();
    void ShowWarp(const QString& warpName);
private:
    //这里可以
    TouchManager* _touchManager;                 ///< タッチマネージャー
    Csm::CubismMatrix44* _viewMatrix;      ///< viewMatrix               ///< モードによっては_renderBufferのテクスチャを描画
    Csm::CubismMatrix44* m_projection;
    SelectTarget _renderTarget;     ///< レンダリング先の選択肢
    float _clearColor[4];           ///< レンダリングターゲットのクリアカラー

    CDrawRect*  m_selectrect;//被选择的点构成选择的矩形
    CDrawRect*  m_mouseselectrect;//鼠标框选构成的矩形
    QMap<QString,CDrawObject*> m_drawWarpMap;


};
