#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include "CPrecompileHead.h"
#include "std/cmodeljson.h"
#include "live2dstruct/coor/cwarpcoortype.h"
#include "live2dstruct/ctreemanage.h"
#include <QAction>
#include <QLabel>
#include <QPushButton>
#include <QToolBar>

class CModelTreePanel;
class CParamPanel;
class CMotionPanel;

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
    void doExportOne(const QString& modelPath, const QString& outPath, bool isAll, bool isFenzu, bool isTexExtract = false);



private:
    void RestDock(const QString& name);
    void CloseModel();




    //一些成员
private:
    OpengGlForm* m_openglwindows;//主窗口保存一个模型
    QAction *myOpenAction;  // 打开模型
    QAction *myAction;  // 输出psd
    QAction *myBatchAction; // 批量导出psd
    QAction *myCloseAction; // 关闭模型
    void createActions();
    void setupUI();
    OpengGlForm* m_headless = nullptr; // 后台离屏导出上下文
    QLabel* m_placeholder = nullptr;   // 空状态占位提示

    // Dock 面板
    CModelTreePanel* m_treePanel = nullptr;
    CParamPanel* m_paramPanel = nullptr;
    CMotionPanel* m_motionPanel = nullptr;

    // 面板切换按钮
    QPushButton* m_btnToggleTree = nullptr;
    QPushButton* m_btnToggleParam = nullptr;
    QPushButton* m_btnToggleMotion = nullptr;

signals:
    void oneExportFinished();

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
};
#endif // MAINWINDOW_H
