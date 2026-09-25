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
#include <map>
#include <memory>

class TouchManager;
class LAppSprite;
class LAppModel;


class LAppView
{
public:
    class PreparedDeformerOverlays
    {
    public:
        PreparedDeformerOverlays() = default;
        PreparedDeformerOverlays(PreparedDeformerOverlays&&) noexcept = default;
        PreparedDeformerOverlays& operator=(PreparedDeformerOverlays&&) noexcept = default;
        PreparedDeformerOverlays(const PreparedDeformerOverlays&) = delete;
        PreparedDeformerOverlays& operator=(const PreparedDeformerOverlays&) = delete;
        bool Prepare(const CTreeManage::Snapshot& snapshot, const PreparedDeformerOverlays& previous,
                     QString* error = nullptr);
        bool Update(const CTreeManage::Snapshot& snapshot, QString* error = nullptr);
        bool Synchronize(const QByteArray& mocDigest, const QMap<QString, float>& values, QString* error = nullptr);
        void Invalidate() noexcept { m_frameReady = false; }
        void Swap(PreparedDeformerOverlays& other) noexcept;
        void Draw(const QMatrix4x4& projection);
        void Show(const QStringList& names);
        CDrawObject* Find(const QString& name) const noexcept;
        qsizetype Count() const noexcept { return static_cast<qsizetype>(m_objects.size()); }

    private:
        friend class LAppView;
        std::map<QString, std::unique_ptr<CDrawObject>> m_objects;
        std::map<QString, QVector3D> m_warpColors;
        QSet<QString> m_visibleNames;
        QSet<QString> m_supportedNames;
        QPointer<QOpenGLContext> m_context;
        QByteArray m_snapshotDigest;
        const C2DTree* m_treeIdentity = nullptr;
        quint64 m_poseRevision = 0;
        bool m_frameReady = false;
    };

    bool PrepareDeformerOverlays(const CTreeManage::Snapshot& snapshot,
                                PreparedDeformerOverlays& result, QString* error = nullptr) const;
    void SwapDeformerOverlays(PreparedDeformerOverlays& candidate) noexcept;

    enum SelectTarget
    {
        SelectTarget_None,                ///< デフォルトのフレームバッファにレンダリング
        SelectTarget_ModelFrameBuffer,    ///< LAppModelが各自持つフレームバッファにレンダリング
        SelectTarget_ViewFrameBuffer,     ///< LAppViewの持つフレームバッファにレンダリング
    };

    LAppView();
    ~LAppView();

    void Initialize();
    bool Render();
    const QString& DeformerOverlayError() const noexcept { return m_deformerOverlayError; }
    void InitializeSprite();
    void ResizeSprite();
    Csm::CubismMatrix44 GetProjectionMatrix() const { return *m_projection; }
    void SetProjectionMatrix(const Csm::CubismMatrix44& matrix) { *m_projection = matrix; }

    void PreModelDraw(LAppModel& refModel);
    void PostModelDraw(LAppModel& refModel);
    float GetSpriteAlpha(int assign) const;
    void SwitchRenderingTarget(SelectTarget targetType);
    void SetRenderTargetClearColor(float r, float g, float b);

    //来自世界坐标系的
    QString GetSelectMesh(const QVector2D& pos,bool IsOver);
    bool SelectMeshPoint(const QString& name, const QVector2D& pos, float radius);
    QVector2D ModelDelta(const QVector2D& worldDelta) const;

    QVector4D GetWorldPoint(int posX, int posY);
    QMatrix4x4 GetProjectionView();
    QMatrix4x4 GetMVP();
    CDrawRect*  GetSelectrect();
    CDrawRect* GetMouseSelectRect();
    void SetViewMatrix(const QMatrix4x4& temp);
    QMatrix4x4 GetViewMatrix();
    void InitWraRotationKey(CWarpNode* node);
    void AddWrapKey(CWarpNode* node);
    void AddRotationKey(CRotationNode* node);
    void InitWrapAndRotation();
    void ShowWarp(const QString& warpName);
    void ShowWarps(const QStringList& names);
    void HideAllWarps();
private:
    //这里可以
    TouchManager* _touchManager;                 ///< タッチマネージャー
    Csm::CubismMatrix44* _viewMatrix;      ///< viewMatrix               ///< モードによっては_renderBufferのテクスチャを描画
    Csm::CubismMatrix44* m_projection;
    SelectTarget _renderTarget;     ///< レンダリング先の選択肢
    float _clearColor[4];           ///< レンダリングターゲットのクリアカラー

    CDrawRect*  m_selectrect;//被选择的点构成选择的矩形
    CDrawRect*  m_mouseselectrect;//鼠标框选构成的矩形
    PreparedDeformerOverlays m_deformerOverlays;
    QString m_deformerOverlayError;


};
