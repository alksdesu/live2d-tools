#ifndef CATLASEDITDIALOG_H
#define CATLASEDITDIALOG_H

#include "live2dstruct/catlasrepackplanner.h"
#include <QDialog>
#include <functional>
#include <memory>

class CAtlasCanvasView;
class CExportControl;
class QComboBox;
class QCheckBox;
class QDialogButtonBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QSpinBox;
class QSplitter;
class QTabWidget;
class QCloseEvent;
class QResizeEvent;

class CAtlasEditDialog : public QDialog
{
    Q_OBJECT
public:
    using Snapshot = live2dstruct::CAtlasRepackPlanner::Snapshot;
    using Plan = live2dstruct::CAtlasRepackPlanner::Plan;
    using Handler = std::function<bool(const Plan&, QString*)>;
    using CancellationHandler = std::function<bool(QString*)>;
    explicit CAtlasEditDialog(const Snapshot& snapshot, const QStringList& selectedMeshes = {}, QWidget* parent = nullptr);
    ~CAtlasEditDialog() override;
    bool IsValid() const { return m_valid; }
    bool IsReady() const { return m_ready; }
    bool IsBusy() const { return m_busy || bool(m_job); }
    QString Error() const;
    const Plan& Result() const { return m_plan; }
    void SetPreviewHandler(Handler handler);
    void SetSubmissionHandler(Handler handler);
    void SetCancellationHandler(CancellationHandler handler);
    bool SetMeshDisplayNames(const QMap<QString, QString>& names, QString* error = nullptr);
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
    using Operation = std::function<bool(const Snapshot&, const Plan&, Plan&, QString*, CExportControl*)>;
    void Run(Operation operation, bool initialize = false);
    bool Publish(const Plan& candidate, bool remember);
    void Refresh();
    void RefreshSelection();
    void ChangeSlot();
    void MoveSelection(int quarterTurns = 0);
    void MoveBy(const QStringList& ids, QPoint delta);
    void AddAtlas();
    void ResizeAtlas();
    void RenameAtlas();
    void RemoveAtlas();
    void Pack();
    void Undo();
    void Redo();
    void Reset();
    void CancelCalculation();
    void ShowError(const QString& error);
    void DiscardInputs();
    bool CheckInputs();
    bool HasPendingInput() const;
    QStringList Selection() const;
    QString ResizeImpact(int slot) const;
    Snapshot m_snapshot;
    Plan m_plan, m_initial;
    QVector<Plan> m_history;
    int m_historyIndex = 0;
    quint64 m_generation = 0;
    std::shared_ptr<Job> m_job;
    Handler m_preview, m_submit;
    CancellationHandler m_cancel;
    QSet<QString> m_selection;
    QMap<QString, QString> m_meshDisplayNames;
    CAtlasCanvasView* m_canvas = nullptr;
    QComboBox *m_slot = nullptr, *m_destination = nullptr;
    QListWidget *m_meshes = nullptr, *m_packTargets = nullptr;
    QLineEdit *m_search = nullptr, *m_name = nullptr, *m_newName = nullptr;
    QSpinBox *m_x = nullptr, *m_y = nullptr, *m_width = nullptr, *m_height = nullptr;
    QSpinBox *m_newWidth = nullptr, *m_newHeight = nullptr, *m_gutter = nullptr;
    QCheckBox* m_rotate = nullptr;
    QLabel *m_status = nullptr, *m_error = nullptr, *m_selectionInfo = nullptr, *m_resizeImpact = nullptr;
    QPushButton *m_undo = nullptr, *m_redo = nullptr, *m_stop = nullptr, *m_discard = nullptr;
    QSplitter *m_splitter = nullptr, *m_workspace = nullptr;
    QTabWidget* m_controls = nullptr;
    QWidget* m_sidebar = nullptr;
    QDialogButtonBox* m_buttons = nullptr;
    bool m_valid = true, m_ready = false, m_busy = false, m_refreshing = false;
    bool m_accepted = false, m_finished = false, m_closeWhenIdle = false, m_moveDirty = false, m_sizeDirty = false, m_nameDirty = false, m_newDirty = false;
};

#endif
