#ifndef CPSDIMPORTDIALOG_H
#define CPSDIMPORTDIALOG_H

#include "live2dstruct/cartworkupdateplanner.h"
#include <QDialog>
#include <functional>
#include <memory>

class CArtworkSourceView;
class CPsdImportLayerModel;
class CPsdImportMeshModel;
class QCheckBox;
class QComboBox;
class QDialogButtonBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QListView;
class QPlainTextEdit;
class QTabWidget;
class QScrollArea;
class QPushButton;
class QSlider;
class QSortFilterProxyModel;
class QSplitter;
class QTreeView;
class QCloseEvent;
class QResizeEvent;

class CPsdImportDialog : public QDialog
{
    Q_OBJECT
public:
    using Planner = live2dstruct::CArtworkUpdatePlanner;
    using Plan = Planner::Plan;
    using Request = Planner::Request;
    struct Context {
        Planner::Snapshot snapshot;
        live2dstruct::CArtworkSourceImport::SourceCandidate incoming;
        QString familyName;
        QStringList selectedMeshIds;
        QMap<int, QImage> atlasImages;
    };
    using Handler = std::function<bool(const Plan&, QString*)>;
    using CancellationHandler = std::function<bool(QString*)>;
    explicit CPsdImportDialog(const Context& context, QWidget* parent = nullptr);
    ~CPsdImportDialog() override;
    bool IsValid() const { return m_valid; }
    bool IsReady() const { return m_ready; }
    bool IsBusy() const { return m_busy || bool(m_job); }
    QString Error() const;
    const Plan& Result() const { return m_plan; }
    const Request& DraftRequest() const { return m_request; }
    void SetPreviewHandler(Handler handler);
    void SetSubmissionHandler(Handler handler);
    void SetCancellationHandler(CancellationHandler handler);
    void accept() override;
    void reject() override;
    void done(int result) override;

signals:
    void Ready();

protected:
    void closeEvent(QCloseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    struct Job;
    struct ViewData;
    struct HistoryEntry { Request request; Plan plan; bool documentAlignmentConfirmed = true; };
    void Initialize();
    void Build(Request request, bool submit = false);
    void LoadView();
    bool Publish(Plan plan, Request request, bool remember);
    void Submit(const Plan& plan);
    void Refresh();
    void RefreshDecision();
    void SelectLayer();
    void SelectMeshes();
    void ApplySelection();
    void PreviewRequest();
    void ConfirmKnownMatches();
    void SaveSourceOnly();
    void ProposeAlignment();
    void ApplyAlignment();
    void MarkMappingEdited();
    void ApplyDocumentAlignment();
    void RestoreInputs();
    void DiscardInput();
    void Undo();
    void Redo();
    void StopCalculation();
    void ShowError(const QString& error);
    void RefreshViewOpacity();
    bool CheckInput();
    bool HasPendingInput() const;
    QStringList SelectedMeshes() const;
    int LayerIndex(const QString& id) const;
    Planner::Decision DecisionFor(const QString& id) const;
    QTransform InitialMapping(const QString& id) const;

    Context m_context;
    Request m_request, m_acceptedRequest;
    Plan m_plan;
    QVector<HistoryEntry> m_history;
    int m_historyIndex = 0;
    quint64 m_generation = 0;
    std::shared_ptr<Job> m_job;
    std::unique_ptr<ViewData> m_viewData;
    QHash<QString, QString> m_suggestedLayers, m_matchReasons;
    QSet<QString> m_knownMatches, m_selectedMeshes;
    QString m_layerId;
    QTransform m_alignment;
    Handler m_preview, m_submit;
    CancellationHandler m_cancel;
    CPsdImportLayerModel* m_layerModel = nullptr;
    CPsdImportMeshModel* m_meshModel = nullptr;
    QSortFilterProxyModel *m_layerProxy = nullptr, *m_meshProxy = nullptr;
    QTreeView* m_layers = nullptr;
    QListView* m_meshes = nullptr;
    CArtworkSourceView* m_source = nullptr;
    QLineEdit *m_layerSearch = nullptr, *m_meshSearch = nullptr, *m_familyName = nullptr;
    QComboBox *m_layerFilter = nullptr, *m_action = nullptr, *m_documentAlignment = nullptr, *m_colorPolicy = nullptr;
    QCheckBox *m_confirm = nullptr, *m_layerOpacity = nullptr;
    QDoubleSpinBox *m_x = nullptr, *m_y = nullptr, *m_angle = nullptr, *m_sx = nullptr, *m_sy = nullptr;
    QDoubleSpinBox *m_documentX = nullptr, *m_documentY = nullptr;
    QSlider *m_sourceOpacity = nullptr, *m_referenceOpacity = nullptr;
    QLabel *m_info = nullptr, *m_coordinates = nullptr, *m_selectionInfo = nullptr, *m_alignmentInfo = nullptr;
    QLabel *m_documentInfo = nullptr, *m_status = nullptr, *m_error = nullptr;
    QPlainTextEdit* m_impacts = nullptr;
    QByteArray m_displayedImpactToken;
    QPushButton *m_undo = nullptr, *m_redo = nullptr, *m_stop = nullptr, *m_previewButton = nullptr;
    QPushButton *m_applySelection = nullptr, *m_confirmKnown = nullptr, *m_discard = nullptr, *m_sourceOnly = nullptr;
    QSplitter* m_splitter = nullptr;
    QWidget *m_left = nullptr, *m_right = nullptr, *m_center = nullptr;
    QTabWidget* m_compact = nullptr;
    QScrollArea* m_inspector = nullptr;
    QDialogButtonBox* m_buttons = nullptr;
    bool m_valid = true, m_ready = false, m_busy = false, m_refreshing = false, m_finished = false, m_closeWhenIdle = false;
    bool m_inputDirty = false, m_numericDirty = false, m_requestDirty = false, m_mixedMapping = false;
    bool m_documentAlignmentConfirmed = true, m_reusesRevision = false, m_published = false, m_recipeDirty = false;
};

#endif
