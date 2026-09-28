#ifndef OPENGGLFORM_H
#define OPENGGLFORM_H
#include <optional>
#include <functional>


#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLWidget>
#include "QOpenGLContext"
#include "live2d/LAppDelegate.hpp"
#include <QTimer>
#include <QEvent>
#include <QMouseEvent>
#include <ui/para/ccidjson.h>
#include "live2d/LAppLive2DManager.hpp"
#include "live2d/LAppModel.hpp"
#include "QFileDialog"
#include "QMessageBox"
#include "cselectstruct.h"
#include <QDesktopServices>
#include <QKeyEvent>
#include <QWheelEvent>
#include "edit/ceditstate.h"
#include "edit/csettingconfig.h"
#include "edit/cdrawrect.h"
#include "live2d/LAppView.hpp"
#include "cdeformerhandleoverlay.h"
#include "live2dstruct/cvertexselection.h"

using Live2D::Cubism::Framework::CubismModel;
class COnionSkinRenderer;
class CPsdExportCoverage;
class CVertexSelectionOverlay;


class OpengGlForm : public QOpenGLWidget
{
    //支持信号和槽
    Q_OBJECT
public:
    enum class MeshTool { Select, Insert, Connect, Delete };
    Q_ENUM(MeshTool)
    struct MeshToolSnapshot {
        QString artMeshId;
        int keyformIndex = -1;
        QByteArray previewDigest;
        QMatrix4x4 mvp;
        QSize viewportSize;
        QMap<QString, float> parameterValues;
        QVector<float> coreVertices;
        QVector<quint16> indices;
        QVector<int> selectedVertices;
        QVector<int> operationVertices;
        QPointF pixelPosition;
        QPointF modelPosition;
        std::optional<QVector<double>> paintedWeights;
    };
    using MeshToolHandler = std::function<bool(MeshTool, const MeshToolSnapshot&, QString*)>;
    bool SetMeshTool(MeshTool tool, const QString& target, QString* error = nullptr);
    bool CanSetMeshTool(MeshTool tool, const QString& target, QString* error = nullptr) const;
    bool ClearMeshTool(QString* error = nullptr);
    void SetMeshToolHandler(MeshToolHandler handler);
    MeshTool CurrentMeshTool() const { return m_meshTool; }
    const QString& MeshToolTarget() const { return m_meshToolTarget; }
    bool MeshToolSnapshotMatches(const MeshToolSnapshot& snapshot, QString* error = nullptr) const;
    bool CaptureMeshToolSnapshot(const QPointF& pixel, MeshToolSnapshot& snapshot, QString* error = nullptr) const;
    bool SetMeshToolSelection(const MeshToolSnapshot& snapshot, const QVector<int>& selected, QString* error = nullptr);
    explicit OpengGlForm(QWidget *par = nullptr);
    ~OpengGlForm();

    //初始化第一次打开模型文件
    void SetInitOpenName(const QString& Name);
    //改变模型
    bool do_ModelChange(const QString& name);
    bool LoadExportModel(const QString& name, QString* error = nullptr);
    bool BeginTemporaryModel(const QString& name, QString* error = nullptr);
    bool EndTemporaryModel(QString* error = nullptr);
    bool SetExportSuspended(bool suspended, QString* error = nullptr);
    bool IsExportSuspended() const { return m_exportSuspended; }
    bool RecoverExportResources(QString* error = nullptr);
    bool IsModelReady() const { return m_ceditstate.IsContins(CEditState::ModelOpen); }
    bool HasInitializationResult() const { return m_initializationCompleted; }
    //进行离开屏幕渲染打印出psd文件
    bool do_OffSeceen(const QString& name,bool IsALl,bool IsFenzu,bool cubismCompat = false);
    // 批量导出静默版本（不弹窗、不自动打开文件夹）
    bool do_OffSeceenSilent(const QString& name,bool IsALl,bool IsFenzu,bool cubismCompat = false, QString* error = nullptr,
                            CExportControl* control = nullptr, const live2dstruct::CModelExportSnapshot& snapshot = {}, CPsdExportCoverage* coverage = nullptr);
    // 原始纹理提取导出（不需要OpenGL渲染）
    bool do_TextureExtract(const QString& name, bool isAll, bool isFenzu, bool cubismCompat = false);
    bool do_TextureExtractSilent(const QString& name, bool isAll, bool isFenzu, bool cubismCompat = false, QString* error = nullptr,
                                 CExportControl* control = nullptr, const live2dstruct::CModelExportSnapshot& snapshot = {}, CPsdExportCoverage* coverage = nullptr);
    const QString& GetCurrentModelJson() const { return m_currentModelJson; }
    float ViewScale() const { return m_config.GeteScalefactor(); }
    QVector2D ViewPosition() const;
    using MeshActivate = std::function<bool(const QString&, int, QString*)>;
    using MeshBegin = std::function<bool(const QVector<int>&, const QVector<float>&, QString*)>;
    using WeightedMeshBegin = std::function<bool(const QVector<double>&, const QVector<float>&, QString*)>;
    using MeshMove = std::function<bool(const QPointF&, QString*)>;
    using MeshFinish = std::function<bool(bool, QString*)>;
    void SetMeshEditHandlers(MeshActivate activate, MeshBegin begin, MeshMove move, MeshFinish finish);
    void SetWeightedMeshEditHandlers(MeshActivate activate, WeightedMeshBegin begin, MeshMove move, MeshFinish finish);
    struct DeformerEditTarget {
        QString id, type;
        int keyform = -1;
        QByteArray previewDigest;
        QMap<QString, float> pose;
        QVector<float> points;
        QPointF direction;
        int rows = 0, columns = 0;
    };
    using DeformerBegin = std::function<bool(CDeformerHandleOverlay::Handle, const QVector<int>&, QString*)>;
    using WeightedDeformerBegin = std::function<bool(CDeformerHandleOverlay::Handle, const QVector<double>&, QString*)>;
    using DeformerMove = std::function<bool(const QPointF&, double, QString*)>;
    void SetDeformerEditHandlers(DeformerBegin begin, DeformerMove move, MeshFinish finish);
    void SetWeightedDeformerEditHandlers(WeightedDeformerBegin begin, DeformerMove move, MeshFinish finish);
    bool SetDeformerEditTarget(const DeformerEditTarget& target, QString* error = nullptr);
    bool ClearDeformerEditTarget(QString* error = nullptr);
    bool FinishKeyformDrag(bool cancel, QString* error = nullptr);
    bool IsKeyformDragging() const { return IsMeshDragging() || m_deformerGesture.has_value() || m_vertexGesture.has_value(); }
    bool ApplyMocPreview(const QByteArray& bytes, const QByteArray& sourceDigest,
                         const QMap<QString, float>* pose, QString* error = nullptr);
    QByteArray CurrentPreviewDigest() const;
    bool CaptureMeshEditorState(CMeshEditorSnapshot& state, QString* error = nullptr) const;
    bool ApplyMeshGeometryPreview(const QByteArray& bytes, const QByteArray& sourceDigest,
                                   const CMeshGeometryMigration& migration, QString* error = nullptr);
    bool ApplyDrawableMaterialPreview(const QByteArray& bytes, const QByteArray& sourceDigest,
                                      const QByteArray& expectedPreviewDigest, const QSet<QString>& allowedDrawableIds,
                                      QString* error = nullptr);
    bool ApplyAtlasPreview(const QByteArray& bytes, const QByteArray& sourceDigest,
                           const QByteArray& expectedPreviewDigest, const QByteArray& expectedWorkspaceDigest,
                           const LAppModel::AtlasState& workspace, const QSet<QString>& allowedDrawableIds,
                           QString* error = nullptr);
    bool ApplyParameterDefinitionPreview(const QByteArray& bytes, const QByteArray& sourceDigest,
                                         const QMap<QString, float>& baseValues, bool keepEditorPreview = true,
                                         QString* error = nullptr);
    bool ApplyParameterStructurePreview(const QByteArray& bytes, const QByteArray& sourceDigest,
                                        const ParameterStructureSeeds& addedSeeds,
                                        const QMap<QString, float>& baseValues, bool keepEditorPreview,
                                        QMap<QString, float>& preparedPose, QString* error = nullptr,
                                        const ParameterReferenceState* references = nullptr,
                                        const LAppModel::EditorPreviewContext* restoreEditor = nullptr,
                                        const LAppModel::TimelinePreviewPreparation& prepareTimeline = {},
                                        const LAppModel::PhysicsDraft* physicsDraft = nullptr,
                                        const CMeshGeometryMigration* geometry = nullptr,
                                        const QMap<QString, float>* requestedEditorPose = nullptr,
                                        const LAppModel::TextureState* textureState = nullptr,
                                        const QSet<QString>* materialDrawableIds = nullptr,
                                        const LAppModel::AtlasState* atlasState = nullptr);
    bool ApplyTextureState(const LAppModel::TextureState& state, QString* error = nullptr);
    bool EndEditorPreview(QString* error = nullptr);
    bool FinishMeshDrag(bool cancel, QString* error = nullptr);
    bool CancelMeshDrag(QString* error = nullptr);
    bool IsMeshDragging() const { return m_meshDrag.has_value(); }

    using VertexSelection = live2dstruct::CVertexSelection;
    struct WarpState {
        int rows = 0, columns = 0;
        VertexSelection::State selection;
        bool operator==(const WarpState& other) const { return rows == other.rows && columns == other.columns && selection == other.selection; }
    };
    using WarpSelectionSnapshot = QMap<QString, WarpState>;
    struct VertexSelectionStatus {
        QString target, type;
        bool available = false, painted = false, busy = false;
        bool canUndoStroke = false, canRedoStroke = false;
        int hardCount = 0, affectedCount = 0;
    };
    bool SetVertexSelectionSettings(const VertexSelection::Settings& settings, QString* error = nullptr);
    const VertexSelection::Settings& VertexSelectionSettings() const { return m_vertexSettings; }
    VertexSelectionStatus SelectionStatus() const;
    bool UndoWeightStroke(QString* error = nullptr);
    bool RedoWeightStroke(QString* error = nullptr);
    WarpSelectionSnapshot CaptureWarpSelectionState() const { return m_warpSelections; }
    bool PrepareWarpSelectionState(const QMap<QString, QSize>& layouts, const WarpSelectionSnapshot* requested,
                                   WarpSelectionSnapshot& prepared, QString* error = nullptr) const;
    void CommitWarpSelectionState(WarpSelectionSnapshot&& prepared) noexcept;
    void ResetVertexSelections();
    void ClearWeightStrokeHistory() noexcept;

    //点击对应的art 显示对应的mesh网格
    //点击对应网格  显示对应item
    void do_ShowMesh(const QString& artName);
    void do_ShowWarp(const QString& artName);
public slots:
    //刷新每一帧    //跳转模型的位置
    void on_timeout();
    void do_setPoint(float x,float y);
    void do_change_para_value(const QString& name,float value,bool IsInit);
    void do_SetVisibility(const QString& name, const QString& type, bool visible);
    void SetHiddenDeformerDrawables(const QSet<QString>& names);
    void ClearEditorOverrides();
    void do_SetDrawableOpacity(const QString& name, float opacity);
    void do_SetDrawableDrawOrder(const QString& name, int drawOrder);
    void SetMeshEditEnabled(bool enabled);
    void SetPhysicsDragEnabled(bool enabled);
    void SetActiveKeyform(const QString& name, int keyformIndex);
    void do_ShowDeformers(const QStringList& names);
    void do_ShowDeformerMeshes(const QStringList& artMeshNames);
    void do_SetDeformerDragParams(const QStringList& paramIds, const QList<float>& minVals,
                                  const QList<float>& maxVals, const QList<float>& defaultVals);
    void do_ResetView();
    void SetViewState(float scale, const QVector2D& position);
    void PublishRenderOrder(const QStringList& names);
    void SetDrawOrderGroups(const QHash<QString, int>& groupIndices,
                            const QVector<live2dstruct::CDrawableOrderGroup>& groups);
    void SyncPreviewView();
    bool SetHighPrecisionMasks(bool enabled, QString* error = nullptr);
    bool HighPrecisionMasks() const;
public:
    using OnionSkinRequestProvider = std::function<bool(live2dstruct::COnionSkin::Request&, QString*)>;
    void SetOnionSkinRequestProvider(OnionSkinRequestProvider provider);
    bool SetOnionSkinRequest(const live2dstruct::COnionSkin::Request& request, QString* error = nullptr);
    void ClearOnionSkin(const QString& error = {});
    live2dstruct::COnionSkin::Status OnionSkinStatus() const { return m_onionStatus; }
public slots:
    void DrawOnionSkinUnderlay();


signals:
    void RuntimeInitialized(bool modelLoaded);
    void SendSelectMesh(const QString& artName);
    void RenderOrderChanged(const QStringList& names);
    void MeshEditRejected(const QString& error);
    void PreviewFailed(const QString& error);
    void KeyformEditingFinishedRequested();
    void MeshToolChanged(OpengGlForm::MeshTool tool, const QString& target);
    void MeshSelectionChanged(const QString& target, const QVector<int>& vertices);
    void VertexSelectionChanged();
    void OnionSkinStatusChanged(const live2dstruct::COnionSkin::Status& status);

protected:
    void initializeGL() override;
    void resizeGL(int w, int h) override;
    void paintGL() override;


public:
    QTimer m_timer;//用来刷新帧

protected:
    virtual void mousePressEvent(QMouseEvent *event) override;
    virtual void mouseReleaseEvent(QMouseEvent *event) override;
    virtual void mouseMoveEvent(QMouseEvent *event) override;
    virtual void keyPressEvent(QKeyEvent *event) Q_DECL_OVERRIDE;
    virtual void keyReleaseEvent(QKeyEvent *event) Q_DECL_OVERRIDE;
    virtual void wheelEvent(QWheelEvent *event) Q_DECL_OVERRIDE;
    void focusOutEvent(QFocusEvent* event) override;
    void hideEvent(QHideEvent* event) override;


    CEditState m_ceditstate;
    CSettingConfig m_config;
    QStringList m_lastRenderOrder;
    QString m_lastPreviewError;
    QString m_currentModelJson;
    struct SuspendedModel { QString path; bool ready; LAppModel* identity; };
    std::optional<SuspendedModel> m_suspendedModel;
    bool m_exportSuspended = false;
    std::optional<QSize> m_deferredResize;
    bool m_runtimeReady = false;
    bool m_initializationCompleted = false;
    std::shared_ptr<COnionSkinRenderer> m_onionRenderer;
    OnionSkinRequestProvider m_onionProvider;
    live2dstruct::COnionSkin::Request m_onionRequest;
    live2dstruct::COnionSkin::Status m_onionStatus;
    quint64 m_onionGeneration = 0;
    bool m_onionStatusQueued = false;
    QMetaObject::Connection m_onionContextConnection;
    void PublishOnionSkinStatus(const live2dstruct::COnionSkin::Status& status);
    bool MakeRuntimeCurrent();
    bool ChangeModel(const QString& name, QString* error);
    bool m_meshEditEnabled = false;
    QString m_meshEditName;
    int m_activeKeyformIndex = 0;
    struct MeshDrag
    {
        QString name;
        int keyform;
        QVector<int> selected;
        QPointF start;
        QMatrix4x4 mvp;
        QSize size;
        CDrawMesh::EditorState previousSelection;
        QPointF startPixel;
        QVector<double> weights;
    };
    std::optional<MeshDrag> m_meshDrag;
    MeshActivate m_meshActivate;
    MeshBegin m_meshBegin;
    WeightedMeshBegin m_weightedMeshBegin;
    MeshMove m_meshMove;
    MeshFinish m_meshFinish;
    bool m_meshEditCallbackActive = false;
    bool m_controlDragActive = false;
    bool m_physicsDragEnabled = false;
    bool m_physicsDragActive = false;
    QPointF m_physicsDragStart;
    QPointF m_pointerAnchor;
    MeshTool m_meshTool = MeshTool::Select;
    QString m_meshToolTarget;
    MeshToolHandler m_meshToolHandler;
    bool m_meshToolDispatchActive = false;
    bool m_meshSelectionNotificationPending = false;
    struct MeshToolGesture {
        MeshToolSnapshot snapshot;
        CMeshEditorSnapshot selection;
        QPointF currentPixel;
        Qt::KeyboardModifiers modifiers;
        int pickedVertex = -1;
        bool rectangle = false;
    };
    std::optional<MeshToolGesture> m_meshToolGesture;
    bool CaptureMeshToolState(const QString& target, const QPointF& pixel, bool requireEditor,
                             MeshToolSnapshot& snapshot, QString* error) const;
    bool SetToolSelection(const QVector<int>& selected, QString* error);
    void ResetMeshToolState();
    void NotifyMeshSelection();
    void CancelMeshToolGesture();
    bool BeginMeshToolGesture(QMouseEvent* event, QString* error);
    bool MoveMeshToolGesture(QMouseEvent* event, QString* error);
    void FinishMeshToolGesture(QMouseEvent* event);
    void DispatchMeshTool(MeshTool tool, MeshToolSnapshot snapshot);
    bool MoveMeshDrag(const QPointF& pixel, QString* error);
    bool MeshDragFrameMatches() const;
    bool BeginMeshDrag(const QString& name, const QPointF& pixel, QString* error, bool activate = true);
    bool FinishArtMeshDrag(bool cancel, QString* error);
    struct DeformerGesture {
        CDeformerHandleOverlay::Handle handle = CDeformerHandleOverlay::None;
        QMatrix4x4 mvp;
        QSize viewport;
        QByteArray acceptedDigest;
        QPointF startPixel, currentPixel, startModel, lastRay;
        QVector<int> previousSelection;
        VertexSelection::State previousVertexSelection;
        QVector<double> weights;
        Qt::KeyboardModifiers modifiers;
        double angle = 0;
        bool rectangle = false;
    };
    DeformerEditTarget m_deformerTarget;
    CDeformerHandleOverlay* m_deformerOverlay = nullptr;
    QMatrix4x4 m_deformerOverlayMvp;
    QSize m_deformerOverlaySize;
    bool m_deformerOverlayDirty = true;
    bool m_deformerCallbackActive = false;
    std::optional<DeformerGesture> m_deformerGesture;
    DeformerBegin m_deformerBegin;
    WeightedDeformerBegin m_weightedDeformerBegin;
    DeformerMove m_deformerMove;
    MeshFinish m_deformerFinish;
    bool DeformerFrameMatches() const;
    void RefreshDeformerOverlay();
    bool BeginDeformerGesture(QMouseEvent* event, QString* error);
    bool MoveDeformerGesture(QMouseEvent* event, QString* error);
    bool FinishDeformerGesture(bool cancel, QString* error);
    void FinishDeformerDrag();
    struct VertexFrame {
        const LAppModel* modelIdentity = nullptr;
        QSize warpLayout;
        QString id, type;
        QByteArray digest;
        QMatrix4x4 mvp;
        QSize viewport;
        QMap<QString, float> pose;
        VertexSelection::Geometry geometry;
        VertexSelection::State state;
    };
    struct VertexGesture {
        VertexFrame frame;
        VertexSelection::Tool tool = VertexSelection::Tool::Move;
        VertexSelection::Combine combine = VertexSelection::Combine::Replace;
        QPointF start, current;
        QVector<QPointF> path;
        VertexSelection::Stroke stroke;
        bool dragged = false;
        int picked = -1;
    };
    struct WeightHistory {
        QString id, type;
        int vertexCount = 0;
        QVector<VertexSelection::Edge> edges;
        QSize warpLayout;
        QVector<VertexSelection::State> states;
        int index = -1;
    };
    VertexSelection::Settings m_vertexSettings;
    WarpSelectionSnapshot m_warpSelections;
    std::optional<VertexGesture> m_vertexGesture;
    WeightHistory m_weightHistory;
    CVertexSelectionOverlay* m_vertexOverlay = nullptr;
    std::optional<QPointF> m_vertexHover;
    mutable std::optional<VertexFrame> m_influenceFrame;
    mutable VertexSelection::Soft m_influenceSoft;
    mutable VertexSelection::Influence m_influence;
    bool CaptureInfluence(const VertexFrame& frame, VertexSelection::Influence& influence, QString* error = nullptr) const;
    bool CaptureVertexFrame(VertexFrame& frame, QString* error = nullptr) const;
    bool VertexFrameMatches(const VertexFrame& frame) const;
    bool ApplyVertexSelection(const VertexFrame& frame, const VertexSelection::State& state, QString* error = nullptr);
    bool BeginVertexGesture(QMouseEvent* event, bool& consumed, QString* error);
    bool MoveVertexGesture(QMouseEvent* event, QString* error);
    bool FinishVertexGesture(bool cancel, QString* error);
    void RefreshVertexOverlay();
    void NotifyVertexSelection();
    void RememberWeightStroke(const VertexFrame& frame, const VertexSelection::State& state);
    bool StepWeightStroke(int direction, QString* error);
    VertexSelection::Combine SelectionCombine(Qt::KeyboardModifiers modifiers) const;
    QString SelectCoreMesh(const QPointF& pixel, bool hover);



};

#endif // OPENGGLFORM_H
