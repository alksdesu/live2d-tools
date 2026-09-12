#include "mainwindow.h"


#include "QPainter"
#include "QLineEdit"
#include <QAction>
#include <QMenu>
#include <QMenuBar>
#include <QToolBar>
#include <QStatusBar>
#include <QDir>
#include <QCoreApplication>
#include <QEventLoop>
#include <QDirIterator>
#include "ui/cpsdbatchdialog.h"
#include "ui/psdbatchworker.h"
#include "ui/cmodeltreepanel.h"
#include "ui/cparampanel.h"
#include "ui/cmotionpanel.h"
#include "ui/cphysicspanel.h"
#include "ui/cblendshapepanel.h"
#include "ui/ctexturepanel.h"
#include "ui/cinspectorpanel.h"
#include "ui/cmodeldiagnosticspanel.h"
#include "ui/para/ccidjson.h"
#include "std/cmodeljsonwriter.h"
#include "cmo3/ccmo3builder.h"
#include "ui/theme.h"
#include <QFileDialog>
#include "live2d/LAppDefine.hpp"
#include "live2d/LAppLive2DManager.hpp"
#include "live2d/LAppModel.hpp"
#include "live2d/LAppTextureManager.hpp"
#include <framework/CubismFramework.hpp>
#include <framework/Id/CubismIdManager.hpp>
#include <QThread>
#include <QProgressDialog>
#include <QTimer>
#include <QApplication>
#include <QVBoxLayout>
#include <QFrame>
#include <QFileInfo>
#include <QSettings>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QFile>
#include <QCloseEvent>
#ifdef Q_OS_WIN
#include <windows.h>
#include <windowsx.h>
#endif

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)

{
    setWindowFlags(Qt::FramelessWindowHint | Qt::Window);
    this->setAcceptDrops(true);
    this->resize(1400, 900);
    this->setWindowTitle("live2d2psd");
    QSettings settings(QStringLiteral("live2d2psd"), QStringLiteral("live2d2psd"));
    if (settings.contains(QStringLiteral("windowGeometry"))) {
        restoreGeometry(settings.value(QStringLiteral("windowGeometry")).toByteArray());
    }
    m_openglwindows=nullptr;
    createActions();
    setupUI();
    m_savedWindowState = settings.value(QStringLiteral("windowState")).toByteArray();

    // 启动时显示占位提示
    m_placeholder = createEmptyState();
    setCentralWidget(m_placeholder);
}

MainWindow::~MainWindow()
{
    QSettings settings(QStringLiteral("live2d2psd"), QStringLiteral("live2d2psd"));
    settings.setValue(QStringLiteral("windowGeometry"), saveGeometry());
    settings.setValue(QStringLiteral("windowState"), saveState());

    if(m_openglwindows!=nullptr){
        m_openglwindows->makeCurrent();
        delete m_openglwindows;
    }
    if(m_headless!=nullptr){ delete m_headless; }
    if(m_cidJson!=nullptr){ delete m_cidJson; }

    CTreeManage::ReleaseInstance();

}







void MainWindow::dragEnterEvent(QDragEnterEvent *event)
{
    const auto list = event->mimeData()->urls();
    for (const QUrl& url : list) {
        const QString path = url.toLocalFile();
        if (path.endsWith(".model3.json", Qt::CaseInsensitive) ||
            path.endsWith(".moc3", Qt::CaseInsensitive)) {
            event->acceptProposedAction();
            return;
        }
    }
    event->ignore();
}

void MainWindow::dropEvent(QDropEvent *event)
{
    const auto list = event->mimeData()->urls();
    for (const QUrl& url : list) {
        const QString path = url.toLocalFile();
        if (path.endsWith(".model3.json", Qt::CaseInsensitive)) {
            OpenModelWithConfirm(path);
            event->accept();
            return;
        }
        if (path.endsWith(".moc3", Qt::CaseInsensitive)) {
            const QString jsonPath = ResolveMocToJson(path);
            if (!jsonPath.isEmpty()) {
                OpenModelWithConfirm(jsonPath);
            }
            event->accept();
            return;
        }
    }
    QMessageBox::about(this, "提示", "请拖入 .model3.json 或 .moc3 文件");
    event->ignore();
}

void MainWindow::OpenModelWithConfirm(const QString &jsonPath)
{
    if (m_openglwindows == nullptr) {
        RestDock(jsonPath);
        return;
    }
    auto reply = QMessageBox::question(this, "提示",
        "已加载模型，是否关闭当前模型并打开新模型？",
        QMessageBox::Yes | QMessageBox::No);
    if (reply == QMessageBox::Yes) {
        if (CloseModel()) RestDock(jsonPath);
    }
}

QString MainWindow::ResolveMocToJson(const QString &mocPath)
{
    QString error;
    QString jsonPath = CModelJsonWriter::GenerateFor(mocPath, &error);
    if (!jsonPath.isEmpty()) {
        return jsonPath;
    }

    // 同名 json 已存在但未引用该 moc：询问是否覆盖重新生成
    const QString targetPath = CModelJsonWriter::TargetJsonPath(mocPath);
    if (QFileInfo::exists(targetPath)) {
        auto reply = QMessageBox::question(this, "提示",
            QString("同名配置已存在但未引用该 moc3：\n%1\n是否覆盖重新生成？").arg(targetPath),
            QMessageBox::Yes | QMessageBox::No);
        if (reply == QMessageBox::Yes) {
            jsonPath = CModelJsonWriter::GenerateFor(mocPath, &error, true);
            if (!jsonPath.isEmpty()) {
                return jsonPath;
            }
        } else {
            return QString();
        }
    }

    QMessageBox::warning(this, "打开 moc3 失败", error);
    return QString();
}

















void MainWindow::on_actionOpen_triggered()
{
    QString fileName = CFileOption::GetDigModleFile(this);
    if(fileName.isEmpty()) return;

    if (fileName.endsWith(".moc3", Qt::CaseInsensitive)) {
        fileName = ResolveMocToJson(fileName);
        if (fileName.isEmpty()) return;
    }
    OpenModelWithConfirm(fileName);
}

void MainWindow::on_actionpsdPrint_triggered()
{
    if(m_openglwindows != nullptr){
        if (!ConfirmExportReady(QStringLiteral("PSD 导出"))) return;
        QString fileName = CFileOption::GetSvaePsdFile(this);
        if(!fileName.isEmpty()){
            CPsdPrintDialog* dialg = new  CPsdPrintDialog(this);
            dialg->SetName(fileName);
            dialg->exec();
            if(dialg->m_IsPrint){
                // 设置快照模式
                LAppLive2DManager* mgr = LAppLive2DManager::GetInstance();
                LAppModel* model = (mgr && mgr->GetModelNum() > 0) ? mgr->GetModel(0) : nullptr;
                if(model && dialg->m_IsSnapshot) model->SetSnapshotMode(true);

                if(dialg->m_IsTexExtract){
                    m_openglwindows->do_TextureExtract(fileName, dialg->m_IsAll, dialg->m_IsFenZu, dialg->m_IsCubismCompat);
                } else {
                    m_openglwindows->do_OffSeceen(fileName, dialg->m_IsAll, dialg->m_IsFenZu, dialg->m_IsCubismCompat);
                }

                // 恢复快照模式
                if(model) model->SetSnapshotMode(false);
            }
            delete dialg;
        }

    }else{
        QMessageBox::about(this,"请将模型拖入","请将模型拖入");
    }
}


void MainWindow::on_actionCmo3Export_triggered()
{
    if(m_currentMoc.isEmpty() || m_openglwindows == nullptr){
        QMessageBox::about(this, "请将模型拖入", "请先打开一个模型");
        return;
    }
    if (!ConfirmExportReady(QStringLiteral("cmo3 导出"))) return;
    if (m_physicsPanel && m_physicsPanel->HasUnsavedChanges()) {
        const auto savePhysics = QMessageBox::question(
            this, QStringLiteral("物理配置未保存"),
            QStringLiteral("导出 cmo3 前需要保存当前物理配置，是否现在保存？"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
        if (savePhysics != QMessageBox::Yes || !m_physicsPanel->SavePhysics()) return;
    }

    const QString defaultName = QFileInfo(m_currentMoc).completeBaseName() + ".cmo3";
    const QString outPath = QFileDialog::getSaveFileName(
        this, "导出 cmo3 工程", defaultName, "Cubism 工程 (*.cmo3)");
    if(outPath.isEmpty()) return;

    const auto answer = QMessageBox::question(this, "导出 cmo3 工程",
        "将从 moc3 运行时数据恢复 Cubism Editor 工程。\n\n"
        "以下信息在 moc3 中不存在，无法恢复：\n"
        "· 变形器路径整理、画师备注\n"
        "· 原始 PSD 关联（贴图以每网格图层重建）\n"
        "· 参数分组的折叠状态等编辑器偏好\n\n"
        "是否继续？",
        QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
    if(answer != QMessageBox::Yes) return;

    statusBar()->showMessage("正在生成 cmo3 工程…");
    QApplication::setOverrideCursor(Qt::WaitCursor);

    cmo3::CCmo3Builder::Options options;
    options.mocPath = m_currentMoc;
    options.outPath = outPath;
    {
        const QDir dir(m_currentModelDir);
        const QStringList cdis = dir.entryList({ "*.cdi3.json" }, QDir::Files);
        if(!cdis.isEmpty()) options.cdiPath = dir.filePath(cdis.first());
        const QStringList physics = dir.entryList({ "*.physics3.json" }, QDir::Files);
        if(!physics.isEmpty()) options.physicsPath = dir.filePath(physics.first());
    }
    options.parameterValues = m_document.ParameterValues();
    options.hiddenIds = m_document.HiddenIds();
    options.drawableOpacityOverrides = m_document.DrawableOpacityOverrides();
    options.drawableDrawOrderOverrides = m_document.DrawableDrawOrderOverrides();
    options.drawableKeyformPositionOverrides = m_document.DrawableKeyformPositionOverrides();

    QString error;
    const bool ok = cmo3::CCmo3Builder::Build(options, &error);
    QApplication::restoreOverrideCursor();

    if(ok){
        statusBar()->showMessage("cmo3 导出完成: " + outPath, 8000);
        QMessageBox::information(this, "导出完成",
            "已生成 Cubism Editor 工程：\n" + outPath);
    } else {
        statusBar()->showMessage("cmo3 导出失败", 8000);
        QMessageBox::warning(this, "导出失败", error);
    }
}

void MainWindow::on_actionSaveSession_triggered()
{
    if (!m_openglwindows || !m_paramPanel || m_currentMoc.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("保存会话"), QStringLiteral("请先打开模型"));
        return;
    }
    const QString defaultPath = QFileInfo(m_currentMoc).absolutePath() + "/" +
        QFileInfo(m_currentMoc).completeBaseName() + QStringLiteral(".live2d-session.json");
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("保存工作会话"),
                                                       defaultPath, QStringLiteral("Live2D 会话 (*.live2d-session.json);;JSON (*.json)"));
    if (path.isEmpty()) return;

    live2dstruct::CModelSession session;
    session.sourceMoc = QFileInfo(m_currentMoc).absoluteFilePath();
    session.sourceDigest = live2dstruct::CModelSession::ComputeSourceDigest(session.sourceMoc);
    if (session.sourceDigest.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("保存会话失败"), QStringLiteral("无法读取当前 moc3 文件指纹"));
        return;
    }
    const QMap<QString, float> currentValues = m_paramPanel->CurrentValues();
    session.parameters = currentValues;
    if (m_treePanel) {
        session.visibility = m_treePanel->VisibilityState();
        session.locked = m_treePanel->LockedState();
    }
    session.drawableOpacity = m_document.DrawableOpacityOverrides();
    session.drawableDrawOrder = m_document.DrawableDrawOrderOverrides();
    session.drawableKeyformPositions = m_document.DrawableKeyformPositionOverrides();
    session.viewScale = m_openglwindows->ViewScale();
    const QVector2D position = m_openglwindows->ViewPosition();
    session.viewPosition = QPointF(position.x(), position.y());
    QString saveError;
    if (!session.Save(path, &saveError)) {
        QMessageBox::warning(this, QStringLiteral("保存会话失败"), saveError);
        return;
    }
    m_sessionDirty = false;
    setWindowTitle(QStringLiteral("live2d2psd"));
    statusBar()->showMessage(QStringLiteral("会话已保存：%1").arg(path), 5000);
}

void MainWindow::on_actionLoadSession_triggered()
{
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("载入工作会话"),
                                                       QString(), QStringLiteral("Live2D 会话 (*.live2d-session.json);;JSON (*.json)"));
    if (path.isEmpty()) return;
    live2dstruct::CModelSession session;
    QString loadError;
    if (!live2dstruct::CModelSession::Load(path, &session, &loadError)) {
        QMessageBox::warning(this, QStringLiteral("载入会话失败"), loadError);
        return;
    }
    if (!m_openglwindows || session.sourceMoc.isEmpty() ||
        QFileInfo(m_currentMoc).absoluteFilePath() != QFileInfo(session.sourceMoc).absoluteFilePath()) {
        QMessageBox::warning(this, QStringLiteral("载入会话失败"), QStringLiteral("会话对应的模型不是当前打开的模型"));
        return;
    }
    if (!session.sourceDigest.isEmpty() &&
        session.sourceDigest != live2dstruct::CModelSession::ComputeSourceDigest(session.sourceMoc)) {
        QMessageBox::warning(this, QStringLiteral("载入会话失败"), QStringLiteral("会话对应的 moc3 文件内容已变化，无法安全恢复编辑状态"));
        return;
    }
    const QString sourceMoc = session.sourceMoc;
    auto apply = [this, sourceMoc, session]() {
        if (!m_openglwindows || QFileInfo(m_currentMoc).absoluteFilePath() != QFileInfo(sourceMoc).absoluteFilePath()) return;
        m_applyingSession = true;
        if (m_paramPanel) m_paramPanel->ApplyValues(session.parameters);
        m_document.ClearLocked();
        for (auto it = session.locked.cbegin(); it != session.locked.cend(); ++it) {
            const int separator = it.key().indexOf(QLatin1Char('|'));
            if (separator <= 0 || separator >= it.key().size() - 1) continue;
            m_document.SetLocked(it.key().left(separator), it.key().mid(separator + 1), it.value());
        }
        if (m_treePanel) m_treePanel->ApplyLockedState(session.locked);
        if (m_inspectorPanel) m_inspectorPanel->SetLockedState(m_document.LockedKeys());
        if (m_treePanel) m_treePanel->ApplyVisibilityState(session.visibility);
        for (auto it = session.drawableOpacity.cbegin(); it != session.drawableOpacity.cend(); ++it) {
            m_document.SetDrawableOpacity(it.key(), it.value());
            if (m_openglwindows) m_openglwindows->do_SetDrawableOpacity(it.key(), it.value());
        }
        for (auto it = session.drawableDrawOrder.cbegin(); it != session.drawableDrawOrder.cend(); ++it) {
            m_document.SetDrawableDrawOrder(it.key(), it.value());
            if (m_openglwindows) m_openglwindows->do_SetDrawableDrawOrder(it.key(), it.value());
        }
        for (auto meshIt = session.drawableKeyformPositions.cbegin(); meshIt != session.drawableKeyformPositions.cend(); ++meshIt) {
            for (int keyformIndex = 0; keyformIndex < meshIt.value().size(); ++keyformIndex) {
                m_document.SetDrawableKeyformPositions(meshIt.key(), keyformIndex, meshIt.value().at(keyformIndex));
            }
        }
        if (m_openglwindows) m_openglwindows->SetViewState(session.viewScale,
                                                            QVector2D(session.viewPosition.x(), session.viewPosition.y()));
        m_applyingSession = false;
        if (m_inspectorPanel) m_inspectorPanel->ClearHistory();
        m_historySource = HistorySource::None;
        if (m_undoAction) m_undoAction->setEnabled(false);
        if (m_redoAction) m_redoAction->setEnabled(false);
        m_sessionDirty = false;
        setWindowTitle(QStringLiteral("live2d2psd"));
        statusBar()->showMessage(QStringLiteral("会话已载入"), 5000);
    };
    apply();
    QTimer::singleShot(300, this, apply);
}

bool MainWindow::ConfirmExportReady(const QString& operation)
{
    if (!m_document.IsLoaded()) return true;
    int errors = 0;
    int warnings = 0;
    for (const auto& diagnostic : m_document.Validate()) {
        if (diagnostic.severity == live2dstruct::CModelDiagnostic::Error) ++errors;
        else ++warnings;
    }
    if (errors > 0) {
        QMessageBox::warning(this, operation + QStringLiteral("已阻止"),
                             QStringLiteral("模型检查发现 %1 个错误，请先在“模型检查”面板处理。").arg(errors));
        if (m_diagnosticsPanel) {
            m_diagnosticsPanel->show();
            m_diagnosticsPanel->raise();
        }
        return false;
    }
    if (warnings > 0) {
        const auto answer = QMessageBox::question(
            this, operation + QStringLiteral("存在警告"),
            QStringLiteral("模型检查发现 %1 个警告，仍要继续吗？").arg(warnings),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
        return answer == QMessageBox::Yes;
    }
    return true;
}

void MainWindow::ApplyDocumentEditToUi(const live2dstruct::CModelEdit& edit, bool forward)
{
    const QVariant& value = forward ? edit.after : edit.before;
    if (edit.field == live2dstruct::CModelEdit::ParameterValue) {
        const float parameterValue = value.toFloat();
        if (m_paramPanel) m_paramPanel->ApplyExternalValue(edit.name, parameterValue);
        else if (m_openglwindows) m_openglwindows->do_change_para_value(edit.name, parameterValue, false);
    } else if (edit.field == live2dstruct::CModelEdit::Visibility) {
        const bool visible = value.toBool();
        if (m_treePanel) {
            m_syncingVisibility = true;
            m_treePanel->SetVisibility(edit.name, edit.type, visible);
            m_syncingVisibility = false;
        } else if (m_openglwindows) {
            m_openglwindows->do_SetVisibility(edit.name, edit.type, visible);
        }
    } else if (edit.field == live2dstruct::CModelEdit::Opacity) {
        const float opacity = value.toFloat();
        if (m_openglwindows) m_openglwindows->do_SetDrawableOpacity(edit.name, opacity);
        if (m_inspectorPanel) m_inspectorPanel->RefreshSelection();
    } else if (edit.field == live2dstruct::CModelEdit::DrawOrder) {
        const int drawOrder = value.toInt();
        if (m_openglwindows) m_openglwindows->do_SetDrawableDrawOrder(edit.name, drawOrder);
        if (m_inspectorPanel) m_inspectorPanel->RefreshSelection();
    } else if (edit.field == live2dstruct::CModelEdit::KeyformPositions) {
        if (m_inspectorPanel) m_inspectorPanel->RefreshSelection();
    }
    if (m_paramPanel) m_paramPanel->SyncHistoryState();
    if (m_inspectorPanel) m_inspectorPanel->SyncHistoryState();
}

void MainWindow::UndoDocumentEdit()
{
    live2dstruct::CModelEdit edit;
    if (!m_document.UndoEdit(&edit)) return;
    ApplyDocumentEditToUi(edit, false);
}

void MainWindow::RedoDocumentEdit()
{
    live2dstruct::CModelEdit edit;
    if (!m_document.RedoEdit(&edit)) return;
    ApplyDocumentEditToUi(edit, true);
}

void MainWindow::on_actionBatchPsd_triggered()
{
    // 1. 选择输入文件夹
    QString inDir = CFileOption::GetSelectDirectory(this);
    if(inDir.isEmpty()) return;

    // 2. 遍历查找所有 *.model3.json
    QStringList modelFiles;
    QDirIterator it(inDir, QStringList() << "*.model3.json", QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) modelFiles << it.next();

    if(modelFiles.isEmpty()){
        QMessageBox::information(this, "提示", "未找到任何 model3.json 文件");
        return;
    }

    // 3. 弹出选择界面，允许勾选需要导出的文件
    CPsdBatchDialog dlg(this);
    dlg.setFileList(modelFiles);
    if(dlg.exec() != QDialog::Accepted) return;
    QStringList selected = dlg.getSelectedFiles();
    if(selected.isEmpty()){
        QMessageBox::information(this, "提示", "未选择任何文件");
        return;
    }

    // 4. 选择输出文件夹
    QString outDir = CFileOption::GetSelectDirectory(this);
    if(outDir.isEmpty()) return;

    bool isAll = dlg.isAllChecked();
    bool isFenzu = dlg.isFenzuChecked();
    bool isTexExtract = dlg.isTexExtractChecked();
    bool isCubism = dlg.isCubismChecked();

    // 5. 后台线程顺序导出（避免在主线程执行长循环）
    QThread* thread = new QThread(this);
    auto* worker = new PsdBatchWorker();
    worker->moveToThread(thread);
    worker->setTask(selected, outDir, isAll, isFenzu, isTexExtract, isCubism);

    // 进度对话框
    QProgressDialog* progress = new QProgressDialog("正在批量导出...", "取消", 0, selected.size(), this);
    progress->setWindowModality(Qt::ApplicationModal);
    progress->setAutoClose(false);
    progress->setAutoReset(false);
    progress->setValue(0);

    connect(progress, &QProgressDialog::canceled, worker, &PsdBatchWorker::cancel);
    connect(worker, &PsdBatchWorker::progress, this, [progress](int cur, int total, const QString& name){
        progress->setMaximum(total);
        progress->setValue(cur-1);
        progress->setLabelText(QString("正在导出 %1/%2: %3").arg(cur).arg(total).arg(name));
        QCoreApplication::processEvents();
    });

    // 请求导出信号 -> 在主线程中执行实际导出（GL上下文限制）
    connect(worker, &PsdBatchWorker::requestExport, this, &MainWindow::doExportOne, Qt::QueuedConnection);
    // 每个导出结束 -> 通知worker继续
    connect(this, &MainWindow::oneExportFinished, worker, &PsdBatchWorker::onOneExportFinished, Qt::QueuedConnection);

    // 线程生命周期管理
    connect(thread, &QThread::started, worker, &PsdBatchWorker::start);
    connect(worker, &PsdBatchWorker::finished, this, [this, thread, worker, progress, outDir](const QStringList& failedFiles){
        progress->setValue(progress->maximum());
        progress->close();
        progress->deleteLater();
        worker->deleteLater();
        thread->quit();
        thread->wait();
        thread->deleteLater();
        if (failedFiles.isEmpty()) {
            QMessageBox::information(this, "批量导出", "导出完成");
        } else {
            QMessageBox::warning(this, "批量导出",
                QString("导出完成，但以下 %1 个文件写入失败：\n%2")
                    .arg(failedFiles.size())
                    .arg(failedFiles.join("\n")));
        }
        QDesktopServices::openUrl(QUrl::fromLocalFile(outDir));
    });

    thread->start();
}

void MainWindow::doExportOne(const QString &modelPath, const QString &outPath, bool isAll, bool isFenzu, bool isTexExtract, bool cubismCompat)
{
    CModelJson preflightJson;
    preflightJson.Parse(modelPath);
    const QString preflightMoc = preflightJson.IsOpen()
        ? QDir(preflightJson.m_parentPath).filePath(preflightJson.m_moc) : QString();
    live2dstruct::CModelDocument preflight;
    QString preflightError;
    if (preflightMoc.isEmpty() || !preflight.Load(preflightMoc, &preflightError)) {
        emit oneExportFinished(false, outPath);
        return;
    }
    for (const auto& diagnostic : preflight.Validate()) {
        if (diagnostic.severity == live2dstruct::CModelDiagnostic::Error) {
            emit oneExportFinished(false, outPath);
            return;
        }
    }
    // 1) 优先复用已有窗口的 GL 上下文，暂停刷新以避免卡顿
    if(m_openglwindows){
        bool wasActive = m_openglwindows->m_timer.isActive();
        if (wasActive) m_openglwindows->m_timer.stop();
        m_openglwindows->do_ModelChange(modelPath);
        bool ok;
        if (isTexExtract) {
            ok = m_openglwindows->do_TextureExtractSilent(outPath, isAll, isFenzu, cubismCompat);
        } else {
            ok = m_openglwindows->do_OffSeceenSilent(outPath, isAll, isFenzu, cubismCompat);
        }
        if (wasActive) m_openglwindows->m_timer.start(1000/60);
        // 通知批量工作器继续
        emit oneExportFinished(ok, outPath);
        return;
    }

    // 2) 若无窗口，则使用隐藏的离屏导出实例（只初始化一次）
    if(!m_headless){
        m_headless = new OpengGlForm();
        m_headless->setAttribute(Qt::WA_DontShowOnScreen);
        m_headless->resize(1,1);
        // 初始化需要一个初始文件名
        m_headless->SetInitOpenName(modelPath);
        m_headless->show();
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    }else{
        m_headless->do_ModelChange(modelPath);
    }
    bool ok;
    if (isTexExtract) {
        ok = m_headless->do_TextureExtractSilent(outPath, isAll, isFenzu, cubismCompat);
    } else {
        ok = m_headless->do_OffSeceenSilent(outPath, isAll, isFenzu, cubismCompat);
    }
    // 通知批量工作器继续
    emit oneExportFinished(ok, outPath);
}







void MainWindow::on_actionCloseModel_triggered()
{
    CloseModel();
}

bool MainWindow::ConfirmCloseWithUnsavedChanges()
{
    const bool physicsDirty = m_physicsPanel && m_physicsPanel->HasUnsavedChanges();
    if (!m_sessionDirty && !physicsDirty) return true;
    QMessageBox prompt(this);
    prompt.setWindowTitle(QStringLiteral("未保存的编辑"));
    prompt.setText(QStringLiteral("当前模型有未保存的编辑。"));
    prompt.setInformativeText(physicsDirty
        ? QStringLiteral("关闭前是否保存工作会话和物理配置？")
        : QStringLiteral("关闭前是否保存工作会话？"));
    QPushButton* save = prompt.addButton(QStringLiteral("保存"), QMessageBox::AcceptRole);
    prompt.addButton(QStringLiteral("放弃"), QMessageBox::DestructiveRole);
    prompt.addButton(QStringLiteral("取消"), QMessageBox::RejectRole);
    prompt.exec();
    if (prompt.clickedButton() == save) {
        if (m_sessionDirty) on_actionSaveSession_triggered();
        const bool sessionSaved = !m_sessionDirty;
        const bool physicsSaved = !physicsDirty || (m_physicsPanel && m_physicsPanel->SavePhysics());
        return sessionSaved && physicsSaved;
    }
    return prompt.buttonRole(prompt.clickedButton()) == QMessageBox::DestructiveRole;
}

QWidget* MainWindow::createEmptyState()
{
    QWidget* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(32, 32, 32, 32);
    layout->addStretch(1);

    auto* card = new QFrame(page);
    card->setObjectName("emptyCard");
    card->setMaximumWidth(560);
    auto* cardLayout = new QVBoxLayout(card);
    cardLayout->setContentsMargins(40, 36, 40, 36);
    cardLayout->setSpacing(12);

    auto* kicker = new QLabel("Live2D 工作台", card);
    kicker->setObjectName("emptyKicker");
    auto* title = new QLabel("打开一个模型开始工作", card);
    title->setObjectName("emptyTitle");
    auto* hint = new QLabel("支持 .model3.json 和 .moc3。你可以调整参数、检查结构并导出 PSD 或 cmo3。", card);
    hint->setObjectName("emptyHint");
    hint->setWordWrap(true);
    auto* open = new QPushButton("打开模型", card);
    open->setObjectName("primaryBtn");
    open->setMinimumHeight(36);
    open->setToolTip("打开 model3.json 或 moc3 文件（Ctrl+O）");
    connect(open, &QPushButton::clicked, this, &MainWindow::on_actionOpen_triggered);

    cardLayout->addWidget(kicker);
    cardLayout->addWidget(title);
    cardLayout->addWidget(hint);
    cardLayout->addSpacing(8);
    cardLayout->addWidget(open);

    QSettings settings(QStringLiteral("live2d2psd"), QStringLiteral("live2d2psd"));
    QStringList recent = settings.value(QStringLiteral("recentModels")).toStringList();
    QStringList validRecent;
    for (const QString& path : recent) {
        if (QFileInfo(path).isFile() && !validRecent.contains(path)) validRecent.append(path);
    }
    if (!validRecent.isEmpty()) {
        auto* recentTitle = new QLabel(QStringLiteral("最近打开"), card);
        recentTitle->setObjectName(QStringLiteral("recentTitle"));
        cardLayout->addSpacing(8);
        cardLayout->addWidget(recentTitle);
        for (const QString& path : validRecent) {
            auto* recentButton = new QPushButton(QFileInfo(path).fileName(), card);
            recentButton->setObjectName(QStringLiteral("recentModel"));
            recentButton->setToolTip(path);
            connect(recentButton, &QPushButton::clicked, this, [this, path](){ OpenModelWithConfirm(path); });
            cardLayout->addWidget(recentButton);
        }
        if (validRecent != recent) settings.setValue(QStringLiteral("recentModels"), validRecent);
    }
    layout->addWidget(card, 0, Qt::AlignHCenter);
    layout->addStretch(1);
    return page;
}

bool MainWindow::CloseModel()
{
    if (!m_openglwindows) return true;
    if (!ConfirmCloseWithUnsavedChanges()) return false;

    // 停止渲染定时器
    m_openglwindows->m_timer.stop();

    // 删除 Dock 面板
    if(m_treePanel)   { removeDockWidget(m_treePanel);   delete m_treePanel;   m_treePanel = nullptr; }
    if(m_paramPanel)  { removeDockWidget(m_paramPanel);  delete m_paramPanel;  m_paramPanel = nullptr; }
    if(m_motionPanel) { removeDockWidget(m_motionPanel); delete m_motionPanel; m_motionPanel = nullptr; }
    if(m_physicsPanel) { removeDockWidget(m_physicsPanel); delete m_physicsPanel; m_physicsPanel = nullptr; }
    if(m_blendShapePanel) { removeDockWidget(m_blendShapePanel); delete m_blendShapePanel; m_blendShapePanel = nullptr; }
    if(m_texturePanel) { removeDockWidget(m_texturePanel); delete m_texturePanel; m_texturePanel = nullptr; }
    if(m_inspectorPanel) { removeDockWidget(m_inspectorPanel); delete m_inspectorPanel; m_inspectorPanel = nullptr; }
    if(m_diagnosticsPanel) { removeDockWidget(m_diagnosticsPanel); delete m_diagnosticsPanel; m_diagnosticsPanel = nullptr; }
    if (m_undoAction) m_undoAction->setEnabled(false);
    if (m_redoAction) m_redoAction->setEnabled(false);
    m_sessionDirty = false;
    setWindowTitle(QStringLiteral("live2d2psd"));

    // 隐藏面板切换按钮
    m_btnToggleTree->setVisible(false);
    m_btnToggleParam->setVisible(false);
    m_btnToggleMotion->setVisible(false);
    m_btnTogglePhysics->setVisible(false);
    m_btnToggleBlend->setVisible(false);
    m_btnToggleTexture->setVisible(false);
    m_btnToggleInspector->setVisible(false);
    m_btnToggleDiagnostics->setVisible(false);

    // 释放显示名称映射
    if(m_cidJson) { delete m_cidJson; m_cidJson = nullptr; }
    m_currentMoc.clear();
    m_currentModelDir.clear();

    // 释放 MOC3 解析树
    CTreeManage::ReleaseInstance();

    // 释放批量导出上下文
    if(m_headless) { delete m_headless; m_headless = nullptr; }

    // 删除 GL 控件（触发 LAppDelegate 完整清理）
    m_openglwindows->makeCurrent();
    delete m_openglwindows;
    m_openglwindows = nullptr;
    if (m_canvasHost) {
        delete m_canvasHost;
        m_canvasHost = nullptr;
    }

    m_document.Clear();
    m_placeholder = createEmptyState();
    setCentralWidget(m_placeholder);

    statusBar()->showMessage("模型已关闭");
    return true;
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    if (CloseModel()) event->accept();
    else event->ignore();
}

void MainWindow::RestDock(const QString &name)
{
    CModelJson modeljson;
    modeljson.Parse(name);
    if (!modeljson.IsOpen() || modeljson.m_moc.isEmpty()) {
        QMessageBox::warning(this, "打开模型失败", "model3.json 格式无效或缺少 Moc 文件引用");
        return;
    }
    const QString moc = QDir(modeljson.m_parentPath).filePath(modeljson.m_moc);
    QString documentError;
    if (!m_document.Load(moc, &documentError)) {
        QMessageBox::warning(this, "打开模型失败", documentError);
        return;
    }
    QSettings settings(QStringLiteral("live2d2psd"), QStringLiteral("live2d2psd"));
    QStringList recent = settings.value(QStringLiteral("recentModels")).toStringList();
    recent.removeAll(name);
    recent.prepend(name);
    while (recent.size() > 8) recent.removeLast();
    settings.setValue(QStringLiteral("recentModels"), recent);
    if(m_placeholder) { delete m_placeholder; m_placeholder = nullptr; }
    m_currentMoc = moc;
    m_currentModelDir = modeljson.m_parentPath;
    CTreeManage::InitOpenLive2d(moc);

    m_openglwindows = new OpengGlForm();
    m_openglwindows->SetInitOpenName(name);
    QHash<QString, int> groupIndices;
    QVector<live2dstruct::CDrawableOrderGroup> groups;
    groups.reserve(m_document.DrawOrderGroups().size());
    for (int groupIndex = 0; groupIndex < m_document.DrawOrderGroups().size(); ++groupIndex) {
        const auto& sourceGroup = m_document.DrawOrderGroups()[groupIndex];
        groups.append({sourceGroup.minimumDrawOrder, sourceGroup.maximumDrawOrder});
        if (!sourceGroup.objectsValid || sourceGroup.minimumDrawOrder != sourceGroup.maximumDrawOrder) continue;
        for (const auto& object : sourceGroup.objects) {
            if (object.type != 0 || object.index < 0 || object.index >= m_document.Drawables().size()) continue;
            groupIndices.insert(m_document.Drawables()[object.index].id, groupIndex);
        }
    }
    m_openglwindows->SetDrawOrderGroups(groupIndices, groups);


    m_openglwindows->setFocusPolicy(Qt::StrongFocus);
    m_canvasHost = new QWidget(this);
    auto* canvasLayout = new QVBoxLayout(m_canvasHost);
    canvasLayout->setContentsMargins(0, 0, 0, 0);
    canvasLayout->setSpacing(0);
    auto* canvasToolbar = new QFrame(m_canvasHost);
    canvasToolbar->setObjectName("canvasToolbar");
    auto* canvasToolbarLayout = new QHBoxLayout(canvasToolbar);
    canvasToolbarLayout->setContentsMargins(14, 8, 14, 8);
    canvasToolbarLayout->setSpacing(10);
    auto* canvasTitle = new QLabel(QFileInfo(name).fileName(), canvasToolbar);
    canvasTitle->setObjectName("canvasTitle");
    auto* canvasMeta = new QLabel("预览", canvasToolbar);
    canvasMeta->setObjectName("canvasMeta");
    auto* canvasSpacer = new QWidget(canvasToolbar);
    canvasSpacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    auto* resetCanvas = new QPushButton("重置视图", canvasToolbar);
    resetCanvas->setToolTip("恢复模型缩放和位置（Home）");
    connect(resetCanvas, &QPushButton::clicked, this, [this](){
        if (m_openglwindows) m_openglwindows->do_ResetView();
    });
    canvasToolbarLayout->addWidget(canvasTitle);
    canvasToolbarLayout->addWidget(canvasMeta);
    canvasToolbarLayout->addWidget(canvasSpacer);
    canvasToolbarLayout->addWidget(resetCanvas);
    canvasLayout->addWidget(canvasToolbar);
    canvasLayout->addWidget(m_openglwindows, 1);
    this->setCentralWidget(m_canvasHost);


    if(m_cidJson) { delete m_cidJson; m_cidJson = nullptr; }
    m_cidJson = new CCidJson();
    m_cidJson->Parse(modeljson.m_parentPath + "/" + modeljson.m_DisplayInfo);
    qDebug() << modeljson.m_parentPath + "/" + modeljson.m_DisplayInfo;

    // 创建模型结构树面板（左侧）
    m_treePanel = new CModelTreePanel(this);
    addDockWidget(Qt::LeftDockWidgetArea, m_treePanel);
    m_treePanel->BuildTree(moc, m_cidJson);

    // 右侧面板：参数控制 + 物理设置（tabify）
    m_paramPanel = new CParamPanel(this);
    m_paramPanel->SetDocument(&m_document);
    addDockWidget(Qt::RightDockWidgetArea, m_paramPanel);
    connect(m_paramPanel, &CParamPanel::HistoryChanged, this, [this](bool canUndo, bool canRedo){
        m_historySource = HistorySource::Parameters;
        if (m_undoAction) m_undoAction->setEnabled(canUndo);
        if (m_redoAction) m_redoAction->setEnabled(canRedo);
        if (canUndo) {
            m_sessionDirty = true;
            setWindowTitle(QStringLiteral("live2d2psd · 未保存"));
            statusBar()->showMessage(QStringLiteral("有未保存的参数修改"));
        }
    });

    m_physicsPanel = new CPhysicsPanel(this);
    addDockWidget(Qt::RightDockWidgetArea, m_physicsPanel);
    tabifyDockWidget(m_paramPanel, m_physicsPanel);
    m_paramPanel->raise();

    // 底部面板：动作/表情 + 融合变形 + 纹理（tabify）
    m_motionPanel = new CMotionPanel(this);
    addDockWidget(Qt::BottomDockWidgetArea, m_motionPanel);

    m_blendShapePanel = new CBlendShapePanel(this);
    addDockWidget(Qt::BottomDockWidgetArea, m_blendShapePanel);
    tabifyDockWidget(m_motionPanel, m_blendShapePanel);

    m_texturePanel = new CTexturePanel(this);
    addDockWidget(Qt::BottomDockWidgetArea, m_texturePanel);
    tabifyDockWidget(m_blendShapePanel, m_texturePanel);

    m_inspectorPanel = new CInspectorPanel(this);
    addDockWidget(Qt::RightDockWidgetArea, m_inspectorPanel);
    tabifyDockWidget(m_paramPanel, m_inspectorPanel);
    m_inspectorPanel->SetDocument(&m_document);
    m_inspectorPanel->SetLockedState(m_document.LockedKeys());
    connect(m_inspectorPanel, &CInspectorPanel::HistoryChanged, this, [this](bool canUndo, bool canRedo){
        m_historySource = HistorySource::Inspector;
        if (m_undoAction) m_undoAction->setEnabled(canUndo);
        if (m_redoAction) m_redoAction->setEnabled(canRedo);
        if (canUndo && !m_applyingSession) {
            m_sessionDirty = true;
            setWindowTitle(QStringLiteral("live2d2psd · 未保存"));
            statusBar()->showMessage(QStringLiteral("有未保存的属性修改"));
        }
    });
    connect(m_undoAction, &QAction::triggered, this, &MainWindow::UndoDocumentEdit);
    connect(m_redoAction, &QAction::triggered, this, &MainWindow::RedoDocumentEdit);

    m_diagnosticsPanel = new CModelDiagnosticsPanel(this);
    addDockWidget(Qt::BottomDockWidgetArea, m_diagnosticsPanel);
    tabifyDockWidget(m_texturePanel, m_diagnosticsPanel);
    m_diagnosticsPanel->SetDocument(&m_document);
    m_diagnosticsPanel->hide();

    m_motionPanel->raise();

    // 加载物理设置（立即，不需要 GL）
    if(m_physicsPanel && !modeljson.m_Physics.isEmpty()){
        m_physicsPanel->LoadPhysics(modeljson.m_parentPath + "/" + modeljson.m_Physics);
    }

    // 加载融合变形信息（立即，不需要 GL）
    if(m_blendShapePanel){
        m_blendShapePanel->BuildBlendShapes(moc, m_cidJson);
    }

    // 延迟构建参数和动作列表（等 GL 初始化完成）
    QString posePath = modeljson.m_Pose.isEmpty() ? QString() : (modeljson.m_parentPath + "/" + modeljson.m_Pose);
    QTimer::singleShot(200, this, [this, posePath, moc](){
        const QSet<QString>* bsParams = m_blendShapePanel ? &m_blendShapePanel->getBlendShapeParamIds() : nullptr;
        if(m_paramPanel) m_paramPanel->BuildParams(m_cidJson, bsParams);
        if(m_motionPanel) m_motionPanel->BuildLists();
        // Pose 切换
        if(m_motionPanel && !posePath.isEmpty()){
            m_motionPanel->BuildPoseGroups(posePath);
        }
        // 加载纹理（GL 初始化后 TextureManager 才有数据）；moc 用于 UV 线框解析
        if(m_texturePanel){
            LAppTextureManager* texMgr = LAppDelegate::GetInstance()->GetTextureManager();
            if(texMgr) m_texturePanel->LoadTextures(texMgr->GetModelTex(), moc);
        }
        // 恢复背景色设置到新 delegate
        applyBgColor();
    });

    // 参数面板实时同步定时器（200ms 间隔，低频避免性能问题）
    QTimer* syncTimer = new QTimer(m_paramPanel);
    connect(syncTimer, &QTimer::timeout, m_paramPanel, &CParamPanel::SyncFromModel);
    syncTimer->start(200);

    // 面板切换按钮 ↔ Dock 面板双向同步
    m_btnToggleTree->setVisible(true);
    m_btnToggleParam->setVisible(true);
    m_btnToggleMotion->setVisible(true);
    m_btnTogglePhysics->setVisible(true);
    m_btnToggleBlend->setVisible(true);
    m_btnToggleTexture->setVisible(true);
    m_btnToggleInspector->setVisible(true);
    m_btnToggleDiagnostics->setVisible(true);

    auto wireToggle = [](QPushButton* btn, QDockWidget* dock){
        QObject::connect(btn, &QPushButton::toggled, dock, &QDockWidget::setVisible);
        QObject::connect(dock, &QDockWidget::visibilityChanged, btn, &QPushButton::setChecked);
    };
    wireToggle(m_btnToggleTree, m_treePanel);
    wireToggle(m_btnToggleParam, m_paramPanel);
    wireToggle(m_btnToggleMotion, m_motionPanel);
    wireToggle(m_btnTogglePhysics, m_physicsPanel);
    wireToggle(m_btnToggleBlend, m_blendShapePanel);
    wireToggle(m_btnToggleTexture, m_texturePanel);
    wireToggle(m_btnToggleInspector, m_inspectorPanel);
    wireToggle(m_btnToggleDiagnostics, m_diagnosticsPanel);

    if (!m_savedWindowState.isEmpty()) {
        restoreState(m_savedWindowState);
        m_savedWindowState.clear();
    }

    // 信号连接：树面板 → OpenGL
    connect(m_treePanel, &CModelTreePanel::MeshSelected, m_openglwindows, &OpengGlForm::do_ShowMesh);
    connect(m_treePanel, &CModelTreePanel::ObjectSelected, m_inspectorPanel, &CInspectorPanel::SetSelection);
    connect(m_treePanel, &CModelTreePanel::ObjectsSelected, m_inspectorPanel, &CInspectorPanel::SetSelections);
    connect(m_treePanel, &CModelTreePanel::WarpSelected, m_openglwindows, &OpengGlForm::do_ShowWarp);
    connect(m_treePanel, &CModelTreePanel::ShowDeformers, m_openglwindows, &OpengGlForm::do_ShowDeformers);
    connect(m_treePanel, &CModelTreePanel::ShowDeformerMeshes, m_openglwindows, &OpengGlForm::do_ShowDeformerMeshes);
    connect(m_treePanel, &CModelTreePanel::DeformerDragParamsChanged, m_openglwindows, &OpengGlForm::do_SetDeformerDragParams);

    // 信号连接：参数面板 → OpenGL
    connect(m_paramPanel, &CParamPanel::ParamChanged, m_openglwindows, &OpengGlForm::do_change_para_value);
    connect(m_blendShapePanel, &CBlendShapePanel::ParamChanged, m_openglwindows, &OpengGlForm::do_change_para_value);

    // 信号连接：OpenGL → 树面板高亮
    connect(m_openglwindows, &OpengGlForm::SendSelectMesh, m_treePanel, &CModelTreePanel::HighlightMesh);

    // 信号连接：纹理面板点击图集 → 树面板 + 主视图 + 面板自身高亮同步
    connect(m_texturePanel, &CTexturePanel::AtlasMeshPicked, m_treePanel, &CModelTreePanel::HighlightMesh);
    connect(m_texturePanel, &CTexturePanel::AtlasMeshPicked, m_openglwindows, &OpengGlForm::do_ShowMesh);
    // 反向：主视图点击 mesh → 纹理面板高亮
    connect(m_openglwindows, &OpengGlForm::SendSelectMesh, m_texturePanel, &CTexturePanel::HighlightMesh);
    connect(m_openglwindows, &OpengGlForm::RenderOrderChanged, m_texturePanel, &CTexturePanel::SetRenderOrder);
    connect(m_treePanel, &CModelTreePanel::MeshSelected, m_texturePanel, &CTexturePanel::HighlightMesh);

    // 信号连接：树面板可见性 → OpenGL 渲染控制
    connect(m_treePanel, &CModelTreePanel::VisibilityChanged, m_openglwindows, &OpengGlForm::do_SetVisibility);
    connect(m_treePanel, &CModelTreePanel::VisibilityChanged, this, [this](const QString& name, const QString& type, bool visible){
        if (m_syncingVisibility) return;
        bool found = false;
        bool before = visible;
        if (type == QLatin1String("part")) {
            for (const auto& part : m_document.Parts()) if (part.id == name) { before = part.visible; found = true; break; }
        } else if (type == QLatin1String("artmesh")) {
            for (const auto& drawable : m_document.Drawables()) if (drawable.id == name) { before = drawable.visible; found = true; break; }
        } else if (type == QLatin1String("warp") || type == QLatin1String("rotation")) {
            for (const auto& deformer : m_document.Deformers()) if (deformer.id == name) { before = deformer.visible; found = true; break; }
        }
        if (!found || before == visible) return;
        const live2dstruct::CModelEdit command{name, type, live2dstruct::CModelEdit::Visibility, before, visible};
        if (m_document.ExecuteEdit(command) && m_inspectorPanel) m_inspectorPanel->SyncHistoryState();
    });
    connect(m_inspectorPanel, &CInspectorPanel::VisibilityChanged, this,
            [this](const QString& name, const QString& type, bool visible){
                m_syncingVisibility = true;
                if (m_treePanel) m_treePanel->SetVisibility(name, type, visible);
                m_syncingVisibility = false;
            });
    connect(m_treePanel, &CModelTreePanel::ObjectLockChanged, this,
            [this](const QString& name, const QString& type, bool locked){
                m_document.SetLocked(type, name, locked);
                if (m_treePanel) m_treePanel->ApplyLockedState(m_treePanel->LockedState());
                if (m_inspectorPanel) m_inspectorPanel->SetLockedState(m_document.LockedKeys());
                if (!m_applyingSession) {
                    m_sessionDirty = true;
                    setWindowTitle(QStringLiteral("live2d2psd · 未保存"));
                    statusBar()->showMessage(QStringLiteral("有未保存的锁定状态"));
                }
            });
    connect(m_inspectorPanel, &CInspectorPanel::DrawableOpacityChanged,
            m_openglwindows, &OpengGlForm::do_SetDrawableOpacity);
    connect(m_inspectorPanel, &CInspectorPanel::DrawableDrawOrderChanged,
            this, [this](const QString& name, int drawOrder){
                if (m_openglwindows) m_openglwindows->do_SetDrawableDrawOrder(name, drawOrder);
            });
    connect(m_inspectorPanel, &CInspectorPanel::DrawableKeyformPositionsChanged,
            m_openglwindows, &OpengGlForm::do_SetDrawableKeyformPositions);
    const auto markSessionDirty = [this]() {
        if (m_applyingSession) return;
        m_sessionDirty = true;
        setWindowTitle(QStringLiteral("live2d2psd · 未保存"));
        statusBar()->showMessage(QStringLiteral("有未保存的模型编辑"));
    };
    connect(m_treePanel, &CModelTreePanel::VisibilityChanged, this, markSessionDirty);
    connect(m_treePanel, &CModelTreePanel::VisibilityChanged, m_diagnosticsPanel, &CModelDiagnosticsPanel::Refresh);
    connect(m_inspectorPanel, &CInspectorPanel::DrawableOpacityChanged, this, markSessionDirty);
    connect(m_inspectorPanel, &CInspectorPanel::DrawableDrawOrderChanged, this, markSessionDirty);
    connect(m_inspectorPanel, &CInspectorPanel::DrawableKeyformPositionsChanged, this,
            [markSessionDirty](const QString&, int, const QVector<float>&){ markSessionDirty(); });
    connect(m_physicsPanel, &CPhysicsPanel::PhysicsConfigChanged, this, [this](){
        if (m_applyingSession) return;
        setWindowTitle(QStringLiteral("live2d2psd · 未保存"));
        statusBar()->showMessage(QStringLiteral("有未保存的物理配置"));
    });
    connect(m_physicsPanel, &CPhysicsPanel::PhysicsSaved, this, [this](const QString&){
        if (!m_sessionDirty) setWindowTitle(QStringLiteral("live2d2psd"));
        statusBar()->showMessage(QStringLiteral("物理配置已保存"), 5000);
    });
    connect(m_paramPanel, &CParamPanel::HistoryChanged, m_diagnosticsPanel, &CModelDiagnosticsPanel::Refresh);
    connect(m_inspectorPanel, &CInspectorPanel::HistoryChanged, m_diagnosticsPanel, &CModelDiagnosticsPanel::Refresh);

    // 信号连接：物理面板 → 模型
    connect(m_physicsPanel, &CPhysicsPanel::GravityChanged, this, [](float x, float y){
        LAppLive2DManager* mgr = LAppLive2DManager::GetInstance();
        if(mgr && mgr->GetModelNum() > 0 && mgr->GetModel(0)){
            mgr->GetModel(0)->SetPhysicsGravity(x, y);
        }
    });
    connect(m_physicsPanel, &CPhysicsPanel::WindChanged, this, [](float x, float y){
        LAppLive2DManager* mgr = LAppLive2DManager::GetInstance();
        if(mgr && mgr->GetModelNum() > 0 && mgr->GetModel(0)){
            mgr->GetModel(0)->SetPhysicsWind(x, y);
        }
    });

    // 信号连接：物理组拖拽参数
    connect(m_physicsPanel, &CPhysicsPanel::PhysicsDragParamsChanged, this, [](const QStringList& ids, const QStringList& types){
        LAppLive2DManager* mgr = LAppLive2DManager::GetInstance();
        if(mgr && mgr->GetModelNum() > 0 && mgr->GetModel(0)){
            mgr->GetModel(0)->SetPhysicsDragParams(ids, types);
        }
    });

    // 信号连接：动作面板 → 模型
    connect(m_motionPanel, &CMotionPanel::PlayMotion, this, [](const QString& group, int no){
        LAppLive2DManager* mgr = LAppLive2DManager::GetInstance();
        if(mgr && mgr->GetModelNum() > 0 && mgr->GetModel(0)){
            mgr->GetModel(0)->StartMotion(group.toUtf8().constData(), no, LAppDefine::PriorityForce);
        }
    });

    connect(m_motionPanel, &CMotionPanel::StopMotions, this, [](){
        LAppLive2DManager* mgr = LAppLive2DManager::GetInstance();
        if(mgr && mgr->GetModelNum() > 0 && mgr->GetModel(0)) mgr->GetModel(0)->StopAllMotions();
    });

    connect(m_motionPanel, &CMotionPanel::ApplyExpression, this, [](const QString& name){
        LAppLive2DManager* mgr = LAppLive2DManager::GetInstance();
        if(mgr && mgr->GetModelNum() > 0 && mgr->GetModel(0)){
            mgr->GetModel(0)->SetExpression(name.toUtf8().constData());
        }
    });

    connect(m_motionPanel, &CMotionPanel::AutoEyeBlinkChanged, this, [](bool enabled){
        LAppLive2DManager* mgr = LAppLive2DManager::GetInstance();
        if(mgr && mgr->GetModelNum() > 0 && mgr->GetModel(0)){
            mgr->GetModel(0)->SetAutoEyeBlink(enabled);
        }
    });

    connect(m_motionPanel, &CMotionPanel::AutoBreathChanged, this, [](bool enabled){
        LAppLive2DManager* mgr = LAppLive2DManager::GetInstance();
        if(mgr && mgr->GetModelNum() > 0 && mgr->GetModel(0)){
            mgr->GetModel(0)->SetAutoBreath(enabled);
        }
    });

    connect(m_motionPanel, &CMotionPanel::PhysicsChanged, this, [](bool enabled){
        LAppLive2DManager* mgr = LAppLive2DManager::GetInstance();
        if(mgr && mgr->GetModelNum() > 0 && mgr->GetModel(0)){
            mgr->GetModel(0)->SetAutoPhysics(enabled);
        }
    });

    connect(m_motionPanel, &CMotionPanel::MouseFollowChanged, this, [](bool enabled){
        LAppLive2DManager* mgr = LAppLive2DManager::GetInstance();
        if(mgr && mgr->GetModelNum() > 0 && mgr->GetModel(0)){
            mgr->GetModel(0)->SetEnableMouseFollow(enabled);
            if(!enabled){
                mgr->GetModel(0)->SetDragging(0.0f, 0.0f);
            }
        }
    });

    // Pose 切换
    connect(m_motionPanel, &CMotionPanel::PosePartChanged, this, [](const QString& showId, const QStringList& hideIds){
        LAppLive2DManager* mgr = LAppLive2DManager::GetInstance();
        if(!mgr || mgr->GetModelNum() <= 0 || !mgr->GetModel(0) || !mgr->GetModel(0)->GetModel()) return;
        LAppModel* model = mgr->GetModel(0);
        auto* cubismModel = model->GetModel();
        // 显示选中的 Part
        Csm::CubismIdHandle showPartId = Csm::CubismFramework::GetIdManager()->GetId(showId.toUtf8().constData());
        cubismModel->SetPartOpacity(showPartId, 1.0f);
        // 隐藏其他 Part
        for(const QString& hideId : hideIds){
            Csm::CubismIdHandle hid = Csm::CubismFramework::GetIdManager()->GetId(hideId.toUtf8().constData());
            cubismModel->SetPartOpacity(hid, 0.0f);
        }
    });

}

void MainWindow::applyBgColor()
{
    LAppDelegate* delegate = LAppDelegate::GetInstance();
    switch(m_bgState){
    case 0: delegate->SetClearColor(1.0f, 1.0f, 1.0f); if(m_bgAction) m_bgAction->setText("背景: 白"); break;
    case 1: delegate->SetClearColor(0.0f, 0.0f, 0.0f); if(m_bgAction) m_bgAction->setText("背景: 黑"); break;
    case 2: delegate->SetClearColor(0.5f, 0.5f, 0.5f); if(m_bgAction) m_bgAction->setText("背景: 灰"); break;
    }
}

void MainWindow::createActions()
{
    myOpenAction = new QAction(tr("打开模型"), this);
    myOpenAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_O));
    myOpenAction->setStatusTip(tr("选择 model3.json 或 moc3 文件打开"));
    connect(myOpenAction, &QAction::triggered, this, &MainWindow::on_actionOpen_triggered);

    myAction = new QAction(tr("输出psd"), this);
    myBatchAction = new QAction(tr("批量导出psd"), this);
    myAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_M));
    myBatchAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_M));
    myAction->setStatusTip(tr("输出psd"));
    myBatchAction->setStatusTip(tr("批量导出psd"));

    connect(myAction, &QAction::triggered, this, &MainWindow::on_actionpsdPrint_triggered);
    connect(myBatchAction, &QAction::triggered, this, &MainWindow::on_actionBatchPsd_triggered);

    myCloseAction = new QAction(tr("关闭模型"), this);
    myCloseAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_W));
    myCloseAction->setStatusTip(tr("关闭当前模型"));
    connect(myCloseAction, &QAction::triggered, this, &MainWindow::on_actionCloseModel_triggered);

    myCmo3Action = new QAction(tr("导出cmo3工程"), this);
    myCmo3Action->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_E));
    myCmo3Action->setStatusTip(tr("将当前 moc3 恢复为 Cubism Editor 可编辑工程"));
    connect(myCmo3Action, &QAction::triggered, this, &MainWindow::on_actionCmo3Export_triggered);

    m_undoAction = new QAction(tr("撤销"), this);
    m_undoAction->setShortcut(QKeySequence::Undo);
    m_undoAction->setEnabled(false);
    m_undoAction->setStatusTip(tr("撤销最近一次参数修改"));
    m_redoAction = new QAction(tr("重做"), this);
    m_redoAction->setShortcut(QKeySequence::Redo);
    m_redoAction->setEnabled(false);
    m_redoAction->setStatusTip(tr("重做最近一次参数修改"));
    m_saveSessionAction = new QAction(tr("保存会话"), this);
    m_saveSessionAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_S));
    connect(m_saveSessionAction, &QAction::triggered, this, &MainWindow::on_actionSaveSession_triggered);
    m_loadSessionAction = new QAction(tr("载入会话"), this);
    m_loadSessionAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_O));
    connect(m_loadSessionAction, &QAction::triggered, this, &MainWindow::on_actionLoadSession_triggered);
    m_darkTheme = QSettings(QStringLiteral("live2d2psd"), QStringLiteral("live2d2psd"))
                      .value(QStringLiteral("darkTheme"), false).toBool();
    m_themeAction = new QAction(m_darkTheme ? QStringLiteral("主题：深色") : QStringLiteral("主题：浅色"), this);
    m_themeAction->setStatusTip(tr("切换浅色/深色工作区"));
    connect(m_themeAction, &QAction::triggered, this, [this](){
        m_darkTheme = !m_darkTheme;
        qApp->setStyleSheet(Theme::globalStyleSheet(m_darkTheme));
        m_themeAction->setText(m_darkTheme ? QStringLiteral("主题：深色") : QStringLiteral("主题：浅色"));
        QSettings settings(QStringLiteral("live2d2psd"), QStringLiteral("live2d2psd"));
        settings.setValue(QStringLiteral("darkTheme"), m_darkTheme);
    });
}

void MainWindow::setupUI()
{
    // 标题栏（上排）
    m_titleBar = addToolBar("titlebar");
    m_titleBar->setMovable(false);
    m_titleBar->setFloatable(false);
    m_titleBar->setObjectName("titleBar");
    m_titleBar->setFixedHeight(32);

    QLabel* iconLabel = new QLabel(this);
    iconLabel->setPixmap(QPixmap(":/logo.ico").scaled(18, 18, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    iconLabel->setFixedSize(24, 24);
    iconLabel->setAlignment(Qt::AlignCenter);
    m_titleBar->addWidget(iconLabel);

    QLabel* titleLabel = new QLabel("live2d2psd", this);
    titleLabel->setObjectName("titleLabel");
    m_titleBar->addWidget(titleLabel);

    QWidget* spacer = new QWidget(this);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    m_titleBar->addWidget(spacer);

    m_btnMin = new QPushButton("—", this);
    m_btnMax = new QPushButton("□", this);
    m_btnClose = new QPushButton("×", this);
    m_btnMin->setObjectName("winBtnMin");
    m_btnMax->setObjectName("winBtnMax");
    m_btnClose->setObjectName("winBtnClose");
    m_btnMin->setFixedSize(40, 28);
    m_btnMax->setFixedSize(40, 28);
    m_btnClose->setFixedSize(40, 28);
    m_titleBar->addWidget(m_btnMin);
    m_titleBar->addWidget(m_btnMax);
    m_titleBar->addWidget(m_btnClose);

    m_titleBar->installEventFilter(this);

    addToolBarBreak();

    connect(m_btnMin, &QPushButton::clicked, this, &QWidget::showMinimized);
    connect(m_btnMax, &QPushButton::clicked, this, [this](){
        if(isMaximized()){
            showNormal();
            m_btnMax->setText("□");
        } else {
            showMaximized();
            m_btnMax->setText("❐");
        }
    });
    connect(m_btnClose, &QPushButton::clicked, this, &QWidget::close);

    // 工具栏（下排）
    QToolBar *mainToolBar = addToolBar(tr("主工具栏"));
    mainToolBar->setMovable(false);
    mainToolBar->setFloatable(false);
    mainToolBar->addAction(myOpenAction);
    mainToolBar->addSeparator();
    mainToolBar->addAction(myAction);
    mainToolBar->addAction(myBatchAction);
    mainToolBar->addAction(myCmo3Action);
    mainToolBar->addSeparator();
    mainToolBar->addAction(m_undoAction);
    mainToolBar->addAction(m_redoAction);
    mainToolBar->addAction(m_saveSessionAction);
    mainToolBar->addAction(m_loadSessionAction);
    mainToolBar->addAction(m_themeAction);
    mainToolBar->addSeparator();
    mainToolBar->addAction(myCloseAction);
    mainToolBar->addSeparator();

    // 重置视图
    QAction* resetViewAction = new QAction("重置视图", this);
    resetViewAction->setShortcut(QKeySequence(Qt::Key_Home));
    resetViewAction->setStatusTip("重置模型视图（缩放和位置）");
    mainToolBar->addAction(resetViewAction);
    connect(resetViewAction, &QAction::triggered, this, [this](){
        if(m_openglwindows) m_openglwindows->do_ResetView();
    });

    // 背景色切换
    m_bgAction = new QAction("背景: 白", this);
    m_bgAction->setStatusTip("切换背景色（白/黑/灰）");
    mainToolBar->addAction(m_bgAction);
    connect(m_bgAction, &QAction::triggered, this, [this](){
        m_bgState = (m_bgState + 1) % 3;
        applyBgColor();
    });

    // 设置状态栏
    statusBar()->showMessage(tr("就绪"));

    // 面板切换按钮（状态栏左侧）
    m_btnToggleTree = new QPushButton("模型结构", this);
    m_btnToggleParam = new QPushButton("参数控制", this);
    m_btnToggleMotion = new QPushButton("动作/表情", this);
    m_btnToggleTree->setCheckable(true);
    m_btnToggleParam->setCheckable(true);
    m_btnToggleMotion->setCheckable(true);
    m_btnToggleTree->setChecked(true);
    m_btnToggleParam->setChecked(true);
    m_btnToggleMotion->setChecked(true);
    m_btnToggleTree->setFlat(true);
    m_btnToggleParam->setFlat(true);
    m_btnToggleMotion->setFlat(true);
    // 未创建面板前先隐藏
    m_btnTogglePhysics = new QPushButton("物理设置", this);
    m_btnToggleBlend = new QPushButton("融合变形", this);
    m_btnToggleTexture = new QPushButton("纹理", this);
    m_btnToggleInspector = new QPushButton("属性", this);
    m_btnToggleDiagnostics = new QPushButton("检查", this);
    m_btnTogglePhysics->setCheckable(true);
    m_btnToggleBlend->setCheckable(true);
    m_btnToggleTexture->setCheckable(true);
    m_btnToggleInspector->setCheckable(true);
    m_btnToggleDiagnostics->setCheckable(true);
    m_btnTogglePhysics->setChecked(true);
    m_btnToggleBlend->setChecked(true);
    m_btnToggleTexture->setChecked(true);
    m_btnToggleInspector->setChecked(true);
    m_btnToggleDiagnostics->setChecked(false);
    m_btnTogglePhysics->setFlat(true);
    m_btnToggleBlend->setFlat(true);
    m_btnToggleTexture->setFlat(true);
    m_btnToggleInspector->setFlat(true);
    m_btnToggleDiagnostics->setFlat(true);

    // 未创建面板前先隐藏
    m_btnToggleTree->setVisible(false);
    m_btnToggleParam->setVisible(false);
    m_btnToggleMotion->setVisible(false);
    m_btnTogglePhysics->setVisible(false);
    m_btnToggleBlend->setVisible(false);
    m_btnToggleTexture->setVisible(false);
    m_btnToggleInspector->setVisible(false);
    m_btnToggleDiagnostics->setVisible(false);

    statusBar()->addPermanentWidget(m_btnToggleTree);
    statusBar()->addPermanentWidget(m_btnToggleParam);
    statusBar()->addPermanentWidget(m_btnToggleMotion);
    statusBar()->addPermanentWidget(m_btnTogglePhysics);
    statusBar()->addPermanentWidget(m_btnToggleBlend);
    statusBar()->addPermanentWidget(m_btnToggleTexture);
    statusBar()->addPermanentWidget(m_btnToggleInspector);
    statusBar()->addPermanentWidget(m_btnToggleDiagnostics);
}

bool MainWindow::nativeEvent(const QByteArray &eventType, void *message, qintptr *result)
{
#ifdef Q_OS_WIN
    if(eventType == "windows_generic_MSG"){
        MSG* msg = static_cast<MSG*>(message);
        if(msg->message == WM_NCHITTEST){
            QPoint pos = mapFromGlobal(QCursor::pos());

            const int border = 5;
            int w = width(), h = height();

            // 边缘缩放
            bool left   = pos.x() < border;
            bool right  = pos.x() > w - border;
            bool top    = pos.y() < border;
            bool bottom = pos.y() > h - border;

            if(top && left)     { *result = HTTOPLEFT;     return true; }
            if(top && right)    { *result = HTTOPRIGHT;    return true; }
            if(bottom && left)  { *result = HTBOTTOMLEFT;  return true; }
            if(bottom && right) { *result = HTBOTTOMRIGHT; return true; }
            if(left)            { *result = HTLEFT;        return true; }
            if(right)           { *result = HTRIGHT;       return true; }
            if(top)             { *result = HTTOP;         return true; }
            if(bottom)          { *result = HTBOTTOM;      return true; }

            // 标题栏拖拽由 eventFilter 处理
        }
    }
#endif
    return QMainWindow::nativeEvent(eventType, message, result);
}

bool MainWindow::eventFilter(QObject* obj, QEvent* event)
{
    if(obj == m_titleBar && event->type() == QEvent::MouseButtonPress){
        QMouseEvent* me = static_cast<QMouseEvent*>(event);
        if(me->button() == Qt::LeftButton){
            QWidget* child = m_titleBar->childAt(me->pos());
            if(!child || !qobject_cast<QPushButton*>(child)){
#ifdef Q_OS_WIN
                ReleaseCapture();
                SendMessage((HWND)winId(), WM_NCLBUTTONDOWN, HTCAPTION, 0);
#endif
                return true;
            }
        }
    }
    if(obj == m_titleBar && event->type() == QEvent::MouseButtonDblClick){
        QMouseEvent* me = static_cast<QMouseEvent*>(event);
        if(me->button() == Qt::LeftButton){
            QWidget* child = m_titleBar->childAt(me->pos());
            if(!child || !qobject_cast<QPushButton*>(child)){
                if(isMaximized()){
                    showNormal();
                    m_btnMax->setText("□");
                } else {
                    showMaximized();
                    m_btnMax->setText("❐");
                }
                return true;
            }
        }
    }
    return QMainWindow::eventFilter(obj, event);
}
