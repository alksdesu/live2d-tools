#ifndef CMESHTOOLBAR_H
#define CMESHTOOLBAR_H

#include <QToolBar>

class QActionGroup;
class QLabel;

class CMeshToolBar : public QToolBar
{
    Q_OBJECT
public:
    enum class Tool { Select, Insert, Connect, Delete };
    Q_ENUM(Tool)
    explicit CMeshToolBar(QWidget* parent = nullptr);
    void SetContext(const QString& name, int vertices, int selected, bool available, const QString& reason = {});
    void SetEditing(bool editing);
    void SetTool(Tool tool);
    bool IsEditing() const;
    bool PreservesBoundary() const;
    Tool CurrentTool() const;

signals:
    void EditingRequested(bool editing);
    void ToolRequested(CMeshToolBar::Tool tool);
    void RetriangulateRequested();
    void AutoGenerateRequested();

private:
    void RefreshActions();
    QAction* m_edit = nullptr;
    QActionGroup* m_tools = nullptr;
    QAction* m_optimize = nullptr;
    QAction* m_autoGenerate = nullptr;
    QAction* m_preserveBoundary = nullptr;
    QLabel* m_context = nullptr;
    bool m_available = false;
    QString m_reason;
};

#endif
