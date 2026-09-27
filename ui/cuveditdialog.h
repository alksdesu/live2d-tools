#ifndef CUVEDITDIALOG_H
#define CUVEDITDIALOG_H

#include "live2dstruct/cuveditdraft.h"
#include <QDialog>
#include <QImage>
#include <functional>

class CUvCanvasView;
class QComboBox;
class QCheckBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QSplitter;
class QDialogButtonBox;
class QCloseEvent;
class QResizeEvent;

class CUvEditDialog : public QDialog
{
    Q_OBJECT
public:
    using State = live2dstruct::CModelMaterialState;
    using Snapshot = live2dstruct::CUvEditDraft::Snapshot;
    using Handler = std::function<bool(const State&, QString*)>;
    using CancellationHandler = std::function<bool(QString*)>;
    explicit CUvEditDialog(const Snapshot& snapshot, const QMap<int, QImage>& images,
                          const QString& selectedMesh = {}, QWidget* parent = nullptr);
    bool IsValid() const { return m_valid; }
    QString Error() const;
    const State& Result() const { return m_state; }
    void SetPreviewHandler(Handler handler);
    void SetSubmissionHandler(Handler handler);
    void SetCancellationHandler(CancellationHandler handler);
    void accept() override;
    void reject() override;

protected:
    void closeEvent(QCloseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    bool Publish(const State& state, bool remember);
    void Refresh();
    void SelectMesh();
    void ApplyTransform();
    void SetPoint();
    void AssignTexture();
    void ResetSelection(bool all);
    void Undo();
    void Redo();
    void BeginGesture();
    void PreviewGesture(QPointF delta, int axis, bool snap);
    void EndGesture(bool canceled);
    void ShowError(const QString& error);
    bool HasPendingInput() const;
    void DiscardPendingInput();
    live2dstruct::CUvEditDraft::Selection Selection() const;

    live2dstruct::CUvEditDraft m_draft;
    live2dstruct::CUvEditDraft::Selection m_lastSelection;
    State m_state, m_gestureBefore;
    QVector<State> m_history;
    int m_historyIndex = 0;
    Handler m_preview, m_submit;
    CancellationHandler m_cancel;
    CUvCanvasView* m_canvas = nullptr;
    QComboBox* m_mesh = nullptr;
    QComboBox* m_slot = nullptr;
    QComboBox* m_slotCoordinates = nullptr;
    QComboBox* m_pivotMode = nullptr;
    QDoubleSpinBox *m_x = nullptr, *m_y = nullptr, *m_dx = nullptr, *m_dy = nullptr;
    QDoubleSpinBox *m_angle = nullptr, *m_sx = nullptr, *m_sy = nullptr, *m_pivotX = nullptr, *m_pivotY = nullptr;
    QCheckBox *m_snap = nullptr, *m_uniform = nullptr;
    QLabel *m_status = nullptr, *m_error = nullptr, *m_coordinates = nullptr;
    QPushButton *m_undo = nullptr, *m_redo = nullptr, *m_pointApply = nullptr, *m_discardInput = nullptr;
    QSplitter* m_splitter = nullptr;
    QWidget* m_editor = nullptr;
    QDialogButtonBox* m_buttons = nullptr;
    bool m_valid = false, m_busy = false, m_refreshing = false, m_gesture = false, m_accepted = false;
    bool m_pointDirty = false, m_transformDirty = false, m_slotDirty = false;
    bool m_pointXDirty = false, m_pointYDirty = false;
};

#endif
