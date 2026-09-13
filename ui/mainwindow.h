#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include "CPrecompileHead.h"
#include "std/cmodeljson.h"
#include "live2dstruct/cmodeldocument.h"
#include "live2dstruct/cmodelsession.h"
#include "live2dstruct/coor/cwarpcoortype.h"
#include "live2dstruct/ctreemanage.h"
#include <QAction>
#include <QLabel>
#include <QPushButton>
#include <QToolBar>
#include <QByteArray>
#include <QVector>

class CModelTreePanel;
class CParamPanel;
class CMotionPanel;
class CPhysicsPanel;
class CBlendShapePanel;
class CTexturePanel;
class CInspectorPanel;
class CModelDiagnosticsPanel;
class CCidJson;
class QCloseEvent;
class QActionGroup;

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
    void on_actionSaveSession_triggered();
    void on_actionLoadSession_triggered();
    void doExportOne(const QString& modelPath, const QString& outPath, bool isAll, bool isFenzu, bool isTexExtract = false, bool cubismCompat = false);



private:
    void RestDock(const QString& name);
    bool CloseModel();
    bool ConfirmCloseWithUnsavedChanges();
    void OpenModelWithConfirm(const QString& jsonPath);
    QString ResolveMocToJson(const QString& mocPath);
    bool ConfirmExportReady(const QString& operation);
    void UndoDocumentEdit();
    void RedoDocumentEdit();
    void ApplyDocumentEditToUi(const live2dstruct::CModelEdit& edit, bool forward);
    void SetWorkspaceMode(int mode);




    //一些成员
private:
    OpengGlForm* m_openglwindows;//主窗口保存一个模型
    QAction *myOpenAction;  // 打开模型
    QAction *myAction;  // 输出psd
    QAction *myBatchAction; // 批量导出psd
    QAction *myCloseAction; // 关闭模型
    QAction *myCmo3Action = nullptr; // 导出 cmo3 工程
    QAction *m_undoAction = nullptr;
    QAction *m_redoAction = nullptr;
    QAction *m_saveSessionAction = nullptr;
    QAction *m_loadSessionAction = nullptr;
    QAction *m_themeAction = nullptr;
    QActionGroup* m_workspaceActionGroup = nullptr;
    QVector<QAction*> m_workspaceActions;
    int m_workspaceMode = 0;
    bool m_darkTheme = false;
    bool m_sessionDirty = false;
    bool m_poseDirty = false;
    bool m_applyingSession = false;
    bool m_syncingVisibility = false;
    enum class HistorySource { None, Parameters, Inspector };
    HistorySource m_historySource = HistorySource::None;
    QByteArray m_savedWindowState;
    QString m_currentMoc;   // 当前模型 moc3 路径（供 cmo3 导出）
    QString m_currentModelDir;
    void createActions();
    void setupUI();
    QWidget* createEmptyState();
    OpengGlForm* m_headless = nullptr; // 后台离屏导出上下文
    QWidget* m_placeholder = nullptr;
    QWidget* m_canvasHost = nullptr;
    live2dstruct::CModelDocument m_document;

    // Dock 面板
    CModelTreePanel* m_treePanel = nullptr;
    CParamPanel* m_paramPanel = nullptr;
    CMotionPanel* m_motionPanel = nullptr;
    CPhysicsPanel* m_physicsPanel = nullptr;
    CBlendShapePanel* m_blendShapePanel = nullptr;
    CTexturePanel* m_texturePanel = nullptr;
    CInspectorPanel* m_inspectorPanel = nullptr;
    CModelDiagnosticsPanel* m_diagnosticsPanel = nullptr;

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

    // 显示名称映射（持久化到模型关闭）
    CCidJson* m_cidJson = nullptr;

signals:
    void oneExportFinished(bool ok, const QString& outPath);

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
