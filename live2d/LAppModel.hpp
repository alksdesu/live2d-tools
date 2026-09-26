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
#include "cdeformerdrag.h"
#include <QRectF>
#include <QMap>
#include <QSet>
#include <QByteArray>
#include "live2dstruct/cmotiontimeline.h"
#include "framework/Model/CubismParameterReferences.hpp"

namespace Live2D { namespace Cubism { namespace Framework { class CubismMotion; }}}

class LAppModel : public Csm::CubismUserModel
{
    friend class LAppExportPreviewTests;
    friend class ModelReplacementTests;
public:

    struct EditorPreviewContext
    {
        bool active = false;
        QByteArray sourceDigest;
        Csm::CubismModel::RuntimeValueSnapshot entry;
        Csm::CubismModel::RuntimeValueSnapshot current;
        QMap<QString, float> pose;
        QMap<QString, float> base;
    };
    struct PreparedTimelinePreview
    {
        CMotionPreviewFrame frame;
        QSet<QString> explicitParameterIds;
        bool active = false;
    };
    using TimelinePreviewPreparation = std::function<bool(const QMap<QString, float>&, const QSet<QString>&,
        const QSet<QString>&, PreparedTimelinePreview&, QString*)>;

    LAppModel();
    virtual ~LAppModel();
    bool LoadAssets(const Csm::csmChar* dir, const Csm::csmChar* fileName);
    bool ReloadRenderer();
    const QMap<int, QString>& GetTexturePaths() const { return m_texturePaths; }
    const QByteArray& GetSourceMocBytes() const { return m_sourceMocBytes; }
    const QByteArray& GetSourceMocDigest() const { return m_sourceMocDigest; }
    const QByteArray& GetPreviewMocDigest() const { return m_previewMocDigest; }
    const QByteArray& GetPreviewMocBytes() const { return m_previewMocBytes; }
    bool ApplyPreviewMoc(const QByteArray& bytes, const QByteArray& sourceDigest, QString* error = nullptr);
    using ParameterDefinitionPreparation = std::function<bool(const Csm::CubismModel&, QString*)>;
    bool ApplyParameterDefinitionPreview(const QByteArray& bytes, const QByteArray& sourceDigest,
                                         const QMap<QString, float>& baseValues, bool keepEditorPreview = true,
                                         QString* error = nullptr, const ParameterDefinitionPreparation& preparePreview = {});
    bool CaptureParameterStructureSeeds(const QStringList& ids, ParameterStructureSeeds& result,
                                        QString* error = nullptr) const;
    bool CaptureEditorPreviewContext(EditorPreviewContext& result, QString* error = nullptr) const;
    const ParameterReferenceState& GetParameterReferenceState() const { return m_parameterReferences; }
    const QByteArray& GetModelSettingsDigest() const { return m_modelSettingsDigest; }
    const QByteArray& GetModelSettingsBytes() const { return m_modelSettingsBytes; }
    const QString& GetModelSettingsPath() const { return m_modelSettingsPath; }
    bool CaptureParameterReferenceCatalog(ParameterReferenceCatalog& result, QString* error = nullptr) const;
    bool ApplyParameterStructurePreview(const QByteArray& bytes, const QByteArray& sourceDigest,
                                        const ParameterStructureSeeds& addedSeeds,
                                        const QMap<QString, float>& baseValues, bool keepEditorPreview,
                                        QString* error = nullptr, const ParameterDefinitionPreparation& preparePreview = {},
                                        const ParameterReferenceState* references = nullptr,
                                        const EditorPreviewContext* restoreEditor = nullptr,
                                        const TimelinePreviewPreparation& prepareTimeline = {});
    bool ApplyEditorPreview(const QByteArray& bytes, const QByteArray& sourceDigest,
                            const QMap<QString, float>& pose, QString* error = nullptr);
    bool EndEditorPreview(QString* error = nullptr);
    bool HasEditorPreview() const { return m_editorEntryState != nullptr; }
    bool BeginExportPreview(bool savedParameterSnapshot, QString* error = nullptr);
    bool EndExportPreview(QString* error = nullptr);
    bool HasExportPreview() const { return m_exportEntryState != nullptr; }
    QMap<QString, float> GetEditorBaseParameters() const { return m_editorBaseParameters; }
    void CommitTextureCache() { m_pendingTextureIds.clear(); m_textureCacheCommitted = true; }
    void Update();
    void SetDragging(Csm::csmFloat32 x, Csm::csmFloat32 y) override
    { if (!HasExportPreview()) CubismUserModel::SetDragging(x, y); }
    bool SetTimelinePreview(const CMotionPreviewFrame& frame, const CMotionTimeline* fadeTimeline = nullptr,
                            double time = 0, bool loop = false);
    void ClearTimelinePreview();
    bool HasTimelinePreview() const { return _timelinePreviewActive; }
    PreparedTimelinePreview GetTimelinePreviewContext() const
    { return {_timelinePreview, m_timelineExplicitParameterIds, _timelinePreviewActive}; }
    QMap<QString, float> GetTimelineBaseParameters() const { return _timelineBaseParameters; }
    QMap<QString, float> GetParameterValues() const;
    void UpdateParaValue(const QString& parId,float value,bool IsInit);
    bool Draw(Csm::CubismMatrix44& matrix);
    Csm::CubismMotionQueueEntryHandle StartMotion(const Csm::csmChar* group, Csm::csmInt32 no, Csm::csmInt32 priority, Csm::ACubismMotion::FinishedMotionCallback onFinishedMotionHandler = NULL);
    void StopAllMotions();
    Csm::CubismMotionQueueEntryHandle StartRandomMotion(const Csm::csmChar* group, Csm::csmInt32 priority, Csm::ACubismMotion::FinishedMotionCallback onFinishedMotionHandler = NULL);
    void SetExpression(const Csm::csmChar* expressionID);
    bool ReloadExpressionFromFile(const QString& expressionId, const QString& path);
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

    void SetAutoEyeBlink(bool v) { if (!HasExportPreview()) _autoEyeBlink = v; }
    void SetAutoBreath(bool v) { if (!HasExportPreview()) _autoBreath = v; }
    void SetAutoPhysics(bool v) { if (!HasExportPreview()) _autoPhysics = v; }
    void SetMotionSpeed(float v);
    void SetMotionLoop(bool v);
    bool GetAutoPhysics() const { return _autoPhysics; }
    void SetEnableMouseFollow(bool v) { if (!HasExportPreview()) _enableMouseFollow = v; }
    bool GetEnableMouseFollow() const { return _enableMouseFollow; }
    void SetSnapshotMode(bool v) { if (!HasExportPreview()) _snapshotMode = v; }

    // 物理参数控制
    void SetPhysicsGravity(float x, float y);
    void SetPhysicsWind(float x, float y);
    bool ReloadPhysicsFromFile(const QString& path);
    bool ReloadUserDataFromFile(const QString& path, const QByteArray& expectedSourceDigest = {}, QString* error = nullptr);
    bool StartLipSyncWav(const QString& path);
    void StopLipSyncWav();

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
    void StopDeformerDrag() { if (!HasExportPreview()) m_deformerDrag.Stop(); }
    const QStringList& GetDeformerDragParamIds() const { return m_deformerDrag.Ids(); }
    // 运行时探测：找到影响指定 ArtMesh 的参数
    void ProbeParamsForArtMeshes(const QStringList& artMeshNames);

protected:
    bool DoDraw();

private:

    bool SetupModel(Csm::ICubismModelSetting* setting);
    void ApplyTimelinePreview();
    void ApplyPartVisibility();
    void ApplyPartVisibility(Csm::CubismModel& model) const;
    bool ApplyPreviewTransaction(const QByteArray& ownedBytes, const QByteArray& sourceDigest,
                                 const Csm::CubismModelReplacement::ModelPreparation& prepareModel,
                                 QString* error, Csm::CubismModelUpdatePolicy policy = Csm::CubismModelUpdatePolicy::AllowClippingEdits,
                                 const ParameterStructureSeeds& seeds = {},
                                 const ParameterPhysicsReferences* physicsReferences = nullptr);
    bool PrepareParameterReferences(const ParameterReferenceState& state, std::function<void()>& commit,
                                    QString* error);
    QString ReferenceSourcePath(const QString& path) const;
    bool ConfigureMotionReferences(Csm::CubismMotion* motion, const QString& path, QString* error = nullptr);
    bool ConfigureExpressionReferences(Csm::CubismExpressionMotion* expression, const QString& path, QString* error = nullptr);
    int FindPartIndex(const QString& id) const;
    CMotionPreviewFrame _timelinePreview;
    QSet<QString> m_timelineExplicitParameterIds;
    QMap<QString, float> _timelineParameters;
    QMap<QString, float> _timelineBaseParts;
    QMap<QString, float> _timelineBaseParameters;
    float _timelineBaseOpacity = 1.0f;
    bool _timelinePreviewActive = false;

    bool SetupTextures();
    void ReleasePendingTextures();


    void PreloadMotionGroup(const Csm::csmChar* group);



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
    QMap<int, QString> m_texturePaths;
    QByteArray m_sourceMocBytes;
    QByteArray m_sourceMocDigest;
    QByteArray m_previewMocDigest;
    QByteArray m_previewMocBytes;
    std::unique_ptr<Csm::CubismModel::RuntimeValueSnapshot> m_editorEntryState;
    std::unique_ptr<Csm::CubismModel::RuntimeValueSnapshot> m_exportEntryState;
    Csm::CubismModel* m_exportModel = nullptr;
    bool m_exportSavedParameters = false;
    bool m_exportPreviousSnapshotMode = false;
    Csm::csmFloat32 m_exportPreviousOpacity = 1.0f;
    QMap<QString, float> m_editorPose;
    QMap<QString, float> m_editorBaseParameters;
    ParameterReferenceState m_parameterReferences;
    QByteArray m_modelSettingsDigest;
    QByteArray m_modelSettingsBytes;
    QString m_modelSettingsPath;
    QString m_physicsReferencePath;
    QVector<quint8> m_suppressedLipSync;
    QVector<quint8> m_suppressedMouseFollow;
    QSet<Csm::csmUint32> m_pendingTextureIds;
    bool m_textureCacheCommitted = false;
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
    CDeformerDrag m_deformerDrag;

    // Part 可见性控制
    QSet<QString> _hiddenPartIds;
};

Q_DECLARE_METATYPE(LAppModel::EditorPreviewContext)
Q_DECLARE_METATYPE(LAppModel::PreparedTimelinePreview)
