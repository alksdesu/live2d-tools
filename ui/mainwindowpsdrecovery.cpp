#include "mainwindow.h"
#include "cpsdrecoverydialog.h"
#include "cexportresultspanel.h"
#include "cexporttaskrunner.h"
#include "cmo3/ccmo3artworkreader.h"
#include "psd/cpsdsourcewriter.h"
#include "std/cexportcontrol.h"
#include <QElapsedTimer>
#include <QFileDialog>
#include <QLabel>
#include <QMessageBox>
#include <QProgressDialog>
#include <QScopeGuard>
#include <QStatusBar>
#include <QTimer>

namespace {
bool runSourceTask(QWidget* parent, const std::shared_ptr<CExportControl>& control,
                   const QString& title, const CExportTaskRunner::Task& task, QString* error)
{
    const QPointer<QWidget> owner(parent);
    QPointer<QProgressDialog> progress = new QProgressDialog(title, QStringLiteral("取消"), 0, 0, parent);
    progress->setObjectName(QStringLiteral("psdRecoveryProgress"));
    progress->setWindowTitle(QStringLiteral("从工程恢复 PSD"));
    progress->setWindowModality(Qt::ApplicationModal); progress->setMinimumDuration(0);
    progress->setAutoClose(false); progress->setAutoReset(false);
    for (auto* label : progress->findChildren<QLabel*>()) label->setTextFormat(Qt::PlainText);
    QObject::connect(progress, &QProgressDialog::canceled, progress, [control] { control->Cancel(); });
    QObject::connect(parent, &QObject::destroyed, progress, [control] { control->Cancel(); });
    const auto cleanup = qScopeGuard([progress] { if (progress) { progress->hide(); progress->deleteLater(); } });
    QTimer timer;
    timer.setInterval(50);
    QObject::connect(&timer, &QTimer::timeout, progress, [progress, control, title] {
        if (!progress) return;
        const auto state = control->Snapshot();
        QString text = control->IsCancellationRequested() ? QStringLiteral("正在取消，请稍候…") : title;
        if (state.total > 0) text += QStringLiteral("\n%1 / %2").arg(state.current).arg(state.total);
        if (!state.detail.isEmpty()) text += QLatin1Char('\n') + state.detail;
        progress->setLabelText(text);
    });
    progress->show(); if (!owner) { control->Cancel(); return false; }
    timer.start();
    return CExportTaskRunner::Run(task, control.get(), error);
}

CExportDiagnostics sourceDiagnostics(const CPsdSourceCatalog& catalog, int index)
{
    QVector<live2dstruct::CModelDiagnostic> result;
    const auto append = [&](const QVector<CPsdSourceDiagnostic>& entries) {
        for (const auto& item : entries) {
            if (item.severity == CPsdSourceDiagnostic::Severity::Information) continue;
            result.append({item.severity == CPsdSourceDiagnostic::Severity::Error ? live2dstruct::CModelDiagnostic::Error :
                live2dstruct::CModelDiagnostic::Warning, item.code, item.message, -1, QStringLiteral("source-layer"), item.layerId});
        }
    };
    append(catalog.diagnostics);
    if (index >= 0 && index < catalog.documents.size()) append(catalog.documents[index].diagnostics);
    return CExportDiagnostics::Capture(result, catalog.sourcePath, catalog.sourceDigest);
}
}

void MainWindow::ConfigurePsdRecoveryAction()
{
    m_psdRecoveryAction = new QAction(QStringLiteral("从工程恢复 PSD…"), this);
    m_psdRecoveryAction->setObjectName(QStringLiteral("recoverPsdFromProject"));
    m_psdRecoveryAction->setStatusTip(QStringLiteral("读取 CMO3 中保存的来源文档与图层，不改变当前模型"));
    connect(m_psdRecoveryAction, &QAction::triggered, this, [this] { RecoverPsdFromProject(); });
}

void MainWindow::FinishPsdRecovery()
{
    if (!m_psdRecoveryActive) return;
    if (m_activeExportControl) { m_activeExportControl->Cancel(); return; }
    const QPointer<MainWindow> alive(this);
    const auto dialog = m_psdRecoveryDialog;
    m_psdRecoveryDialog.clear(); m_psdRecoveryActive = false; m_batchExporting = false;
    if (dialog) { disconnect(dialog, nullptr, this, nullptr); dialog->deleteLater(); }
    RefreshExportActions(); if (!alive) return;
    if (m_closeAfterExport) QTimer::singleShot(0, this, [this] { m_closeAfterExport = false; close(); });
}

void MainWindow::RecoverPsdFromProject(const QString& requestedPath)
{
    const QPointer<MainWindow> alive(this);
    if (m_psdRecoveryDialog) { m_psdRecoveryDialog->show(); if (alive && m_psdRecoveryDialog) m_psdRecoveryDialog->raise(); return; }
    const auto unavailable = [&] { return m_psdRecoveryActive || m_batchExporting || !m_psdBatch.isNull() || m_previewExport.IsBusy() ||
        m_activeExportControl || m_closingModel || m_artworkLoading || m_applyingSession ||
        m_materialEditor.IsActive() || m_atlasEditor.IsActive(); };
    if (unavailable()) return;
    if (requestedPath.size() > 32768 || requestedPath.contains(QChar::Null)) {
        statusBar()->showMessage(QStringLiteral("来源工程路径过长或含无效字符"), 15000); return;
    }
    QString path(requestedPath.constData(), requestedPath.size());
    if (path.isEmpty()) path = QFileDialog::getOpenFileName(this, QStringLiteral("选择来源 CMO3 工程"), {}, QStringLiteral("Cubism 工程 (*.cmo3)"));
    if (!alive || path.isEmpty() || unavailable()) return;
    if (path.size() > 32768 || path.contains(QChar::Null)) {
        statusBar()->showMessage(QStringLiteral("来源工程路径过长或含无效字符"), 15000); return;
    }
    const QString frozenPath(path.constData(), path.size());
    m_psdRecoveryActive = true; m_batchExporting = true;
    auto control = std::make_shared<CExportControl>(); m_activeExportControl = control;
    RefreshExportActions(); if (!alive) return;
    auto catalog = std::make_shared<CPsdSourceCatalog>();
    QString error;
    const bool loaded = runSourceTask(this, control, QStringLiteral("正在读取工程中的来源素材"),
        [frozenPath, catalog, control](const CExportTaskRunner::GuiCall&, QString* detail) {
            return cmo3::CCmo3ArtworkReader::ReadFile(frozenPath, *catalog, detail, control.get());
        }, &error);
    if (!alive) return;
    m_activeExportControl.reset();
    if (!loaded || m_closeAfterExport) {
        const bool canceled = control->WasCanceled() || m_closeAfterExport;
        FinishPsdRecovery(); if (!alive) return;
        statusBar()->showMessage(canceled ? QStringLiteral("来源工程读取已取消") : QStringLiteral("来源工程读取失败：%1").arg(error), 15000);
        if (!alive) return;
        if (!canceled) {
            auto* message = new QMessageBox(QMessageBox::Warning, QStringLiteral("来源工程读取失败"), error, QMessageBox::Ok, this);
            message->setTextFormat(Qt::PlainText); message->setAttribute(Qt::WA_DeleteOnClose); message->open();
        }
        return;
    }
    std::shared_ptr<const CPsdSourceCatalog> frozen = catalog;
    auto* dialog = new CPsdRecoveryDialog(frozen, this); m_psdRecoveryDialog = dialog;
    const QPointer<CPsdRecoveryDialog> retained(dialog);
    dialog->SetExportHandler([alive, frozen](int index, const QString& output, QString* detail) {
        if (!alive) { if (detail) *detail = QStringLiteral("工作窗口已关闭"); return false; }
        return alive->ExportRecoveredPsd(frozen, index, output, detail);
    });
    connect(dialog, &CPsdRecoveryDialog::CancelRequested, this, [this] { if (m_activeExportControl) m_activeExportControl->Cancel(); });
    connect(dialog, &QDialog::finished, this, [this] { FinishPsdRecovery(); });
    connect(dialog, &QObject::destroyed, this, [this] { FinishPsdRecovery(); });
    dialog->show();
    if (alive && retained) retained->raise();
}

bool MainWindow::ExportRecoveredPsd(const std::shared_ptr<const CPsdSourceCatalog>& catalog, int documentIndex,
                                   const QString& path, QString* error)
{
    if (path.isEmpty() || path.size() > 32768 || path.contains(QChar::Null)) {
        if (error) *error = QStringLiteral("PSD 保存路径为空、过长或含无效字符");
        return false;
    }
    const QString frozenPath(path.constData(), path.size());
    if (error) error->clear();
    if (!m_psdRecoveryActive || !m_psdRecoveryDialog || m_activeExportControl || !catalog ||
        documentIndex < 0 || documentIndex >= catalog->documents.size()) {
        if (error) *error = QStringLiteral("来源导出状态已失效，请重新打开工程");
        return false;
    }
    if (!CPsdRecoveryDialog::ValidateOutputPath(catalog->sourcePath, frozenPath, error)) return false;
    const QPointer<MainWindow> alive(this);
    const QPointer<CPsdRecoveryDialog> dialog(m_psdRecoveryDialog);
    const auto diagnostics = sourceDiagnostics(*catalog, documentIndex);
    if (diagnostics.ErrorCount()) { if (error) *error = diagnostics.FirstError(); return false; }
    auto control = std::make_shared<CExportControl>(); m_activeExportControl = control;
    control->SetHandler([source = catalog->sourcePath, frozenPath](const CExportControl::Progress&, QString* reason) {
        return CPsdRecoveryDialog::ValidateOutputPath(source, frozenPath, reason);
    });
    PsdBatchFileResult result;
    result.index = 0; result.sourcePath = catalog->sourcePath; result.outputPath = frozenPath;
    result.state = PsdBatchFileState::Running; result.stage = PsdExportStage::Preparing;
    result.diagnostics = diagnostics;
    m_exportResultsAreRecovery = true;
    const bool started = !m_exportResultsPanel || m_exportResultsPanel->SetResults({result});
    if (!alive) return false;
    if (!started) {
        m_activeExportControl.reset();
        if (error) *error = QStringLiteral("无法建立导出记录，请关闭并重新打开恢复窗口");
        return false;
    }
    if (!alive) return false;
    RefreshExportActions(); if (!alive) return false;
    QElapsedTimer elapsed; elapsed.start();
    QString detail;
    const bool written = runSourceTask(dialog, control, QStringLiteral("正在写出所选来源文档"),
        [catalog, documentIndex, frozenPath, control](const CExportTaskRunner::GuiCall&, QString* reason) {
            if (!CPsdRecoveryDialog::ValidateOutputPath(catalog->sourcePath, frozenPath, reason)) return false;
            return CPsdSourceWriter::Write(frozenPath, catalog->documents[documentIndex], control.get(), reason);
        }, &detail);
    if (!alive) return false;
    m_activeExportControl.reset();
    result.elapsedMs = elapsed.elapsed(); result.outputCommitted = control->IsCommitted();
    const bool canceled = control->WasCanceled() && !result.outputCommitted;
    result.state = written ? PsdBatchFileState::Succeeded : canceled ? PsdBatchFileState::Canceled : PsdBatchFileState::Failed;
    const auto phase = control->Snapshot().phase;
    result.stage = written ? PsdExportStage::Done : phase == CExportControl::Phase::Preparing ? PsdExportStage::Preparing :
        phase == CExportControl::Phase::Encoding ? PsdExportStage::Encoding :
        phase == CExportControl::Phase::Writing || phase == CExportControl::Phase::Committing ? PsdExportStage::Writing : PsdExportStage::Exporting;
    result.error = written ? QString() : detail;
    if (error) *error = canceled ? QStringLiteral("导出已取消，未替换目标文件") : detail;
    const bool recorded = !m_exportResultsPanel || m_exportResultsPanel->UpdateResult(0, result);
    if (!alive) return written;
    statusBar()->showMessage(written ? QStringLiteral("来源 PSD 已保存：%1").arg(frozenPath) :
        canceled ? QStringLiteral("来源 PSD 导出已取消") : QStringLiteral("来源 PSD 导出失败：%1").arg(detail), 15000);
    if (!alive) return written;
    if (!recorded) {
        const QString warning = result.outputCommitted ? QStringLiteral("PSD 文件已保存，但无法更新导出记录") : QStringLiteral("无法更新导出记录：%1").arg(detail);
        statusBar()->showMessage(warning, 15000);
        if (!alive) return written;
        if (error) *error = warning;
    }
    if (!dialog || !dialog->isVisible()) QTimer::singleShot(0, this, &MainWindow::FinishPsdRecovery);
    else if (m_closeAfterExport) dialog->reject();
    return written;
}
