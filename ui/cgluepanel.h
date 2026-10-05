#ifndef CGLUEPANEL_H
#define CGLUEPANEL_H

#include "live2dstruct/cglueeditcontroller.h"
#include <QDockWidget>
#include <functional>

class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QTableWidget;

namespace live2dstruct {
class CModelDocument;
struct CModelEdit;
}

class CGluePanel : public QDockWidget
{
    Q_OBJECT
public:
    using Publisher = live2dstruct::CGlueEditController::Publisher;
    using HistoryHandler = std::function<bool(bool, live2dstruct::CModelEdit&, QString*)>;

    explicit CGluePanel(QWidget* parent = nullptr);
    ~CGluePanel() override = default;

    void SetDocument(live2dstruct::CModelDocument* document);
    void SetPublisher(Publisher publisher);
    void SetHistoryHandler(HistoryHandler handler);
    void Refresh();
    void MarkSaved();
    bool CanUndo() const;
    bool CanRedo() const;
    bool Undo(QString* error = nullptr);
    bool Redo(QString* error = nullptr);
    const live2dstruct::CGlueEditController::Snapshot& CurrentGlues() const;

signals:
    void HistoryChanged(bool canUndo, bool canRedo);
    void EditRejected(const QString& reason);

private:
    void RefreshEditor();
    void RefreshGlueList(const QString& selectedId = {});
    void RefreshPairTable();
    void RejectEdit(const QString& error);
    int SelectedGlueIndex() const;
    void RefreshWeightControls();
    void ApplyWeights();
    bool MoveHistory(bool undo, QString* error);

    live2dstruct::CModelDocument* m_document = nullptr;
    live2dstruct::CModelDocument* m_initializedDocument = nullptr;
    live2dstruct::CGlueEditController m_editor;
    HistoryHandler m_historyHandler;
    QComboBox* m_glueSelector = nullptr;
    QTableWidget* m_pairTable = nullptr;
    QLabel* m_statusLabel = nullptr;
    QLabel* m_emptyLabel = nullptr;
    QPushButton* m_undoButton = nullptr;
    QPushButton* m_redoButton = nullptr;
    QPushButton* m_applyButton = nullptr;
    QDoubleSpinBox* m_weightA = nullptr;
    QDoubleSpinBox* m_weightB = nullptr;
    bool m_updating = false;
    bool m_refreshPending = false;
};

#endif
