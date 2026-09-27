#ifndef CVERTEXSELECTIONTOOLS_H
#define CVERTEXSELECTIONTOOLS_H

#include "live2dstruct/cvertexselection.h"
#include <QWidget>

class QAction;
class QActionGroup;
class QCheckBox;
class QComboBox;
class QLineEdit;
class QFrame;
class QLabel;
class QMenu;
class QScrollArea;
class QSlider;
class QToolBar;
class QToolButton;

class CVertexSelectionTools : public QWidget
{
    Q_OBJECT
public:
    using Selection = live2dstruct::CVertexSelection;
    struct Context {
        QString target, type, reason;
        bool available = false;
        bool editing = false;
        bool busy = false;
        bool influenceReady = true;
        bool canUndoStroke = false;
        bool canRedoStroke = false;
        int hardCount = 0;
        int affectedCount = 0;
        Selection::Source source = Selection::Source::Hard;
    };
    explicit CVertexSelectionTools(QWidget* parent = nullptr);
    ~CVertexSelectionTools() override;
    void SetContext(const Context& context);
    bool SetSettings(const Selection::Settings& settings, QString* error = nullptr);
    const Selection::Settings& Settings() const { return m_settings; }
    const Context& CurrentContext() const { return m_context; }
    void SetError(const QString& error);

signals:
    void EditingRequested(bool editing);
    void SettingsRequested(const live2dstruct::CVertexSelection::Settings& settings);
    void UndoStrokeRequested();
    void RedoStrokeRequested();

protected:
    bool eventFilter(QObject* object, QEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    void Refresh();
    void RefreshInputs();
    void RequestSettings(const Selection::Settings& candidate);
    void SubmitInputs();
    void OpenSettings();
    void PositionPopup();
    bool CanEdit() const;
    bool ReadInputs(Selection::Settings& result, QString* error) const;
    Selection::Settings m_settings;
    Context m_context;
    QString m_error;
    bool m_refreshing = false;
    QToolBar* m_toolbar = nullptr;
    QAction* m_edit = nullptr;
    QActionGroup* m_tools = nullptr;
    QActionGroup* m_combinations = nullptr;
    QAction* m_soft = nullptr;
    QAction* m_options = nullptr;
    QAction* m_undo = nullptr;
    QAction* m_redo = nullptr;
    QToolButton* m_combineButton = nullptr;
    QMenu* m_combineMenu = nullptr;
    QLabel* m_status = nullptr;
    QFrame* m_popup = nullptr;
    QScrollArea* m_scroll = nullptr;
    QWidget* m_softFields = nullptr;
    QWidget* m_brushFields = nullptr;
    QLabel* m_popupHint = nullptr;
    QLabel* m_popupError = nullptr;
    QCheckBox* m_softEnabled = nullptr;
    QLineEdit* m_softRadius = nullptr;
    QComboBox* m_distance = nullptr;
    QComboBox* m_falloff = nullptr;
    QLineEdit* m_brushRadius = nullptr;
    QLineEdit* m_strength = nullptr;
    QLineEdit* m_hardness = nullptr;
    QSlider* m_radiusSlider = nullptr;
};

#endif
