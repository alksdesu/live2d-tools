#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include "CPrecompileHead.h"
#include "std/cmodeljson.h"
#include "live2dstruct/cmodeldocument.h"
#include "live2dstruct/cmodelsession.h"
#include "live2dstruct/catlaseditcontroller.h"
#include "live2dstruct/cmaterialeditcontroller.h"
#include "live2dstruct/ckeyformeditcontroller.h"
#include "live2dstruct/cparametervalueeditcontroller.h"
#include "live2dstruct/coor/cwarpcoortype.h"
#include "live2dstruct/ctreemanage.h"
#include "ui/cpreviewexporttransaction.h"
#include "ui/cworkspacelayouts.h"
#include "ui/psdbatchresult.h"
#include "live2dstruct/cruntimepackage.h"
#include "live2dstruct/cparameterselectionquery.h"
#include "live2dstruct/conionskin.h"
#include <QAction>
#include <QLabel>
#include <QPushButton>
#include <QToolBar>
#include <QByteArray>
#include <QVector>
#include <QPointer>
#include <memory>

class CModelTreePanel;
class CParamPanel;
class CMotionPanel;
class CPhysicsPanel;
class CBlendShapePanel;
class CTexturePanel;
class CUvEditDialog;
class CAtlasEditDialog;
class CPsdImportDialog;
class CInspectorPanel;
class CModelDiagnosticsPanel;
class CUserDataPanel;
class CCidJson;
class QCloseEvent;
class QActionGroup;
class PsdBatchJob;
class QProgressDialog;
class CExportResultsPanel;
class CExportControl;
class CMeshToolBar;
class CVertexSelectionTools;
class CAlphaMeshDialog;
class CDeformerRenameDialog;
class CDeformerGridDialog;
class QVBoxLayout;
struct PreviewExportState;
struct CMeshGeometryMigration;
struct MeshToolCommitState;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

private slots:

    void on_actionOpen_triggered();
    void on_actionpsdPrint_triggered();
    void on_actionBatchPsd_triggered();
    void on_actionCloseModel_triggered();
    void on_actionCmo3Export_triggered();
    void ExportRuntimePackage();
    void on_actionSaveSession_triggered();
    void on_actionLoadSession_triggered();
    bool doExportOne(const QString& modelPath, const QString& outPath, bool isAll, bool isFenzu,
                     bool isTexExtract = false, bool cubismCompat = false, QString* error = nullptr,
                     PsdExportStage* stage = nullptr, bool* outputCommitted = nullptr,
                     bool savedParameterSnapshot = false, bool* canceled = nullptr,
                     CExportDiagnostics* diagnostics = nullptr);



private:
    void RestDock(const QString& name);
    bool CloseModel();
    void StopPsdBatch();
    void StartPsdBatch(const QStringList& models, const QString& outputDirectory, bool all, bool groups,
                       bool textures, bool cubismCompat);
    void ReportExportStage(const QString& outputPath, PsdExportStage stage);
    void RecoverExportPreview();
    void RefreshExportActions();
    bool ConfirmCloseWithUnsavedChanges(bool replacingSession = false, bool replacingPhysics = false);
    bool UsesSessionPhysics() const;
    bool HasUnsavedPhysicsChanges() const;
    void ConfigurePhysicsSaving();
    void ConfigurePhysicsTools();
    void RefreshPhysicsObservation();
    void BakePhysicsAnimation();
    void RestorePhysicsBakeActions();
    bool SaveSession();
    void OpenModelWithConfirm(const QString& jsonPath);
    QString ResolveMocToJson(const QString& mocPath);
    bool ConfirmExportReady(const QString& operation);
    bool CaptureRuntimePackage(bool currentDefaults, bool includeTimeline,
                               live2dstruct::CRuntimePackage::Request& request, QString* error,
                               bool includeArtwork = false) const;
    bool RuntimeMatchesDocument(QString* error = nullptr) const;
    void SyncDocumentToRuntime();
    void RefreshRelatedParameters();
    live2dstruct::CParameterSelectionQuery m_parameterSelectionQuery;
    void ConfigureParameterValueEditing();
    bool FinishParameterValueEditing(QString* error = nullptr);
    bool CancelParameterValueEditing(QString* error = nullptr);
    void ResetParameterValueEditing();
    bool ApplyParameterValueHistory(bool undo, live2dstruct::CModelEdit& edit, QString* error = nullptr);
    bool ParameterValueEditingReady(QString* error) const;
    bool CaptureParameterValueBase(QMap<QString, float>& values, QString* error) const;
    QByteArray ParameterValueContextDigest() const;
    bool PublishParameterValues(const live2dstruct::CParameterValueEditController::Publication& publication, QString* error);
    void SyncParameterValueUi(const QMap<QString, float>& values);
    struct ParameterValueEditState;
    std::shared_ptr<ParameterValueEditState> m_parameterValueEdit;
    bool m_publishingParameterValues = false;
    void InitializeRuntimePanels(OpengGlForm* expectedWindow, bool loaded);
    bool AuthoringReady(QString* error = nullptr) const;
    bool SetAuthoringReady(bool ready);
    void RefreshSessionActions();
    void RefreshTextureGeometry();
    bool InitializeTextureEditing(QString* error);
    bool RestoreArtworkPackage(OpengGlForm* expectedWindow, QString* warning);
    bool ReplaceTexture(int slot, const QByteArray& png, QString* error);
    void RefreshTextureEditing(bool changed = false);
    void EditAtlas(const QString& selectedMesh);
    void ConfigureArtworkActions();
    void RefreshArtworkActions();
    void ImportPsd(bool reimport = false);
    void ManageArtwork();
    void EditArtwork(const QString& familyId, const QString& path = {}, const QString& revisionId = {});
    QPointer<CPsdImportDialog> m_artworkDialog;
    std::shared_ptr<CExportControl> m_artworkLoadControl;
    bool m_artworkLoading = false;
    QString m_artworkPackageWarning;
    QAction *m_importPsdAction = nullptr, *m_reimportPsdAction = nullptr, *m_manageArtworkAction = nullptr;
    bool PublishAtlasPreview(const live2dstruct::CModelDocument& before,
                             const live2dstruct::CModelDocument& after,
                             const QSet<QString>& ids, QString* error);
    live2dstruct::CAtlasEditController m_atlasEditor;
    QPointer<CAtlasEditDialog> m_atlasDialog;
    void EditUv(const QString& selectedMesh);
    QString DrawableMaterialLockReason(const live2dstruct::CModelDrawable& mesh) const;
    void RefreshMaterialEditing(bool changed);
    bool PublishDrawableMaterials(const live2dstruct::CModelDocument& before,
                                  const live2dstruct::CModelDocument& after,
                                  const QSet<QString>& ids, QString* error);
    live2dstruct::CMaterialEditController m_materialEditor;
    QPointer<CUvEditDialog> m_uvDialog;
    void ClearTimelinePreview();
    void ConfigureOnionSkin();
    void RefreshOnionSkin();
    bool CaptureOnionSkinRequest(live2dstruct::COnionSkin::Request& request, QString* error) const;
    bool PublishKeyformPreview(const live2dstruct::CModelDocument& document,
                              const QMap<QString, float>& pose, bool editing, QString* error);
    bool PublishParameterDefinitions(const live2dstruct::CModelDocument& document,
                                     const QMap<QString, float>& values, bool editing, QString* error);
    bool PublishParameterStructure(const live2dstruct::CModelDocument& document, const live2dstruct::CModelEdit& command,
                                   bool forward, bool restoring, const QMap<QString, float>& values,
                                   bool editing, QMap<QString, float>& pose, QString* error);
    bool PrepareMeshMigration(const live2dstruct::CModelDocument& document, const live2dstruct::CModelEdit* command,
                              bool forward, CMeshGeometryMigration& migration, QVariant* leaving, QString* error) const;
    bool PublishMeshStructure(const live2dstruct::CModelDocument& document, const live2dstruct::CModelEdit& command,
                              bool forward, QString* error);
    bool PublishDeformerStructure(const live2dstruct::CModelDocument& document, const live2dstruct::CModelEdit& command,
                                  bool forward, QString* error);
    void CreateDeformer();
    void RemoveDeformer();
    void RenameDeformer(const QString& id, const QString& type);
    void ResizeWarpGrid(const QString& id);
    bool InitializeDeformerDisplayNames(live2dstruct::CModelDocument& document, QString* error) const;
    void RefreshDeformerDisplayNames();
    bool CancelDeformerDialogs();
    void RefreshDeformerActions();
    void RefreshDeformerStructureUi(const live2dstruct::CModelEdit& command, bool forward);
    QPointer<CDeformerRenameDialog> m_deformerRenameDialog;
    QPointer<CDeformerGridDialog> m_deformerGridDialog;
    void InitializeMeshTools(QVBoxLayout* layout);
    void RefreshMeshTools();
    void RefreshVertexSelectionTools();
    bool SetMeshToolsEnabled(bool enabled, QString* error);
    bool ApplyMeshTool(OpengGlForm::MeshTool tool, const OpengGlForm::MeshToolSnapshot& snapshot, QString* error);
    void OptimizeSelectedMesh();
    void GenerateSelectedMesh();
    bool CancelAutomaticMesh();
    bool PrepareParameterStructurePose(const live2dstruct::CModelDocument& document, const live2dstruct::CModelEdit& command,
                                       bool forward, QMap<QString, float>& pose, QString* error) const;
    QStringList ParameterReferences(const QString& id, bool includeTimeline = true) const;
    bool PrepareParameterDeletion(const QString& id, QStringList& removals, QString* error) const;
    void RefreshParameterStructureUi();
    void SyncParameterDefinitionPreview();
    bool ActivateKeyform(const QString& id, int keyform, QString* error);
    bool ActivateKeyform(const QString& id, const QString& type, int keyform, QString* error, bool allowLockedPreview = false);
    bool EndKeyformEditing(QString* error = nullptr, bool finishParameters = true);
    bool SubmitInspectorEdit(const live2dstruct::CModelEdit& edit, QString* error);
    bool ApplyDocumentHistory(bool undo, live2dstruct::CModelEdit& edit, QString* error);
    void RefreshKeyformUi(bool changed = false, bool markSession = true, bool syncParameterValues = true);
    void SelectModelObjects(const QStringList& ids, const QStringList& types);
    void RefreshModelTree();
    void EditGeometryPairs();
    void MirrorGeometryPair();
    void ViewGeometryPair();
    void RefreshGeometryPairActions();
    bool m_geometryPairsOpen = false;
    QString m_parameterBeingEdited;
    live2dstruct::CParameterResourceEdits::State m_parameterResources;
    const live2dstruct::CModelSession* m_restoringSession = nullptr;
    bool m_applyingParameterReferences = false;
    bool m_refreshParameterTracks = false;
    bool m_refreshPhysicsReferences = false;
    bool m_physicsInSession = false;
    QJsonObject m_savedPhysicsDraft;
    QString m_savedPhysicsPath;
    quint64 m_timelinePreviewStopRevision = 0;
    QStringList m_selectedObjectIds, m_selectedObjectTypes;
    bool HasSessionChanges() const;
    void MarkDocumentSaved();
    QByteArray m_savedAuthoringFingerprint;
    QJsonObject m_savedParameterResources;
    mutable QByteArray m_dirtyComparedSource;
    mutable QString m_dirtyComparedPath;
    mutable quint64 m_dirtyComparedRevision = 0;
    mutable bool m_dirtyComparisonReady = false;
    mutable bool m_documentDirty = false;
    void RefreshWindowTitle();
    void RefreshUndoActions();
    void UndoDocumentEdit();
    void RedoDocumentEdit();
    void ApplyDocumentEditToUi(const live2dstruct::CModelEdit& edit, bool forward);
    void ApplyVisibilityValues(const QMap<QString, bool>& values);
    void SetWorkspaceMode(int mode);
    void ApplyDefaultWorkspaceLayout(int mode);
    void ApplyDefaultWorkspaceSizes(int mode);




    //一些成员
private:
    QPointer<OpengGlForm> m_openglwindows;
    QAction *myOpenAction;  // 打开模型
    QAction *myAction;  // 输出psd
    QAction *myBatchAction; // 批量导出psd
    QAction *myCloseAction; // 关闭模型
    QAction *myCmo3Action = nullptr; // 导出 cmo3 工程
    QAction* m_runtimeExportAction = nullptr;
    QAction *m_undoAction = nullptr;
    QAction *m_redoAction = nullptr;
    QAction *m_saveSessionAction = nullptr;
    QAction *m_loadSessionAction = nullptr;
    QAction *m_themeAction = nullptr;
    QActionGroup* m_workspaceActionGroup = nullptr;
    QVector<QAction*> m_workspaceActions;
    int m_workspaceMode = 0;
    bool m_darkTheme = false;
    bool m_highPrecisionMasks = false;
    bool m_sessionDirty = false;
    bool m_closingModel = false;
    bool m_poseDirty = false;
    bool m_applyingSession = false;
    bool m_workspaceReady = false;
    bool m_runtimePanelsInitialized = false;
    bool m_initializingRuntimePanels = false;
    bool m_syncingVisibility = false;
    enum class HistorySource { None, Parameters, Inspector, Timeline, Physics };
    HistorySource m_historySource = HistorySource::None;
    std::optional<HistorySource> m_keyformDragHistorySource;
    CWorkspaceLayouts m_workspaceLayouts;
    QString m_currentMoc;   // 当前模型 moc3 路径（供 cmo3 导出）
    QString m_currentModelDir;
    void createActions();
    void setupUI();
    QWidget* createEmptyState();
    QPointer<OpengGlForm> m_headless;
    QPointer<PsdBatchJob> m_psdBatch;
    QPointer<QProgressDialog> m_batchProgress;
    QPointer<QProgressDialog> m_singleExportProgress;
    std::shared_ptr<CExportControl> m_activeExportControl;
    PsdExportStage m_activeExportStage = PsdExportStage::Queued;
    bool m_batchExporting = false;
    bool m_closeAfterExport = false;
    CPreviewExportTransaction m_previewExport;
    QVector<QPointer<QAction>> m_physicsBakeActions;
    std::shared_ptr<PreviewExportState> m_exportState;
    CExportResultsPanel* m_exportResultsPanel = nullptr;
    QAction* m_recoverPreviewAction = nullptr;
    QAction* m_exportResultsAction = nullptr;
    struct BatchOptions {
        QString outputDirectory;
        bool all = false;
        bool groups = false;
        bool textures = false;
        bool cubismCompat = false;
    } m_lastBatchOptions;
    QWidget* m_placeholder = nullptr;
    QWidget* m_canvasHost = nullptr;
    live2dstruct::CModelDocument m_document;
    live2dstruct::CKeyformEditController m_keyformEditor;
    QMap<QString, float> m_keyformBaseValues;
    QPointer<QPushButton> m_finishKeyformButton;
    QPointer<CMeshToolBar> m_meshTools;
    QPointer<CVertexSelectionTools> m_vertexSelectionTools;
    std::shared_ptr<const MeshToolCommitState> m_meshToolCommit;
    QPointer<CAlphaMeshDialog> m_alphaMeshDialog;
    std::shared_ptr<CExportControl> m_alphaMeshLoadControl;

    // Dock 面板
    CModelTreePanel* m_treePanel = nullptr;
    CParamPanel* m_paramPanel = nullptr;
    CMotionPanel* m_motionPanel = nullptr;
    CPhysicsPanel* m_physicsPanel = nullptr;
    CBlendShapePanel* m_blendShapePanel = nullptr;
    CTexturePanel* m_texturePanel = nullptr;
    CInspectorPanel* m_inspectorPanel = nullptr;
    CModelDiagnosticsPanel* m_diagnosticsPanel = nullptr;
    CUserDataPanel* m_userDataPanel = nullptr;

    // 背景色状态
    int m_bgState = 0;
    QAction* m_bgAction = nullptr;
    void applyBgColor();

    // 面板切换按钮
    QPushButton* m_btnToggleTree = nullptr;
    QPushButton* m_btnToggleParam = nullptr;
    QPushButton* m_btnToggleMotion = nullptr;
    QPushButton* m_btnTogglePhysics = nullptr;
    QPushButton* m_btnToggleBlend = nullptr;
    QPushButton* m_btnToggleTexture = nullptr;
    QPushButton* m_btnToggleInspector = nullptr;
    QPushButton* m_btnToggleDiagnostics = nullptr;
    QPushButton* m_btnToggleUserData = nullptr;

    // 显示名称映射（持久化到模型关闭）
    CCidJson* m_cidJson = nullptr;

private:
    QToolBar* m_titleBar = nullptr;
    QPushButton* m_btnMin = nullptr;
    QPushButton* m_btnMax = nullptr;
    QPushButton* m_btnClose = nullptr;

protected:
    virtual void dragEnterEvent(QDragEnterEvent *event) override;
    virtual void dropEvent(QDropEvent *event) override;
    bool nativeEvent(const QByteArray &eventType, void *message, qintptr *result) override;
    bool eventFilter(QObject* obj, QEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
};
#endif // MAINWINDOW_H
