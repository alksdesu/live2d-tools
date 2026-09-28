#include "mainwindow.h"
#include "cpsdrecoverydialog.h"
#include "cexportresultspanel.h"
#include "cexporttaskrunner.h"
#include "cmo3/ccmo3artworkreader.h"
#include "psd/cpsdsourcewriter.h"
#include "live2dstruct/cartworkpsdrecovery.h"
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
    progress->setWindowTitle(QStringLiteral("导出来源 PSD"));
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
    QString path = catalog.sourcePath; QByteArray digest = catalog.sourceDigest;
    if (index >= 0 && index < catalog.documents.size()) {
        const auto& document = catalog.documents[index]; append(document.diagnostics);
        if (const auto* reconstructed = document.Reconstructed()) append(reconstructed->diagnostics);
        if (!document.origin.sourcePath.isEmpty() || document.Original()) path = document.origin.sourcePath;
        if (document.origin.sourceDigest.size() == 32) digest = document.origin.sourceDigest;
        if (const auto* original = document.Original(); original && original->Directory()) digest = original->Directory()->sha256;
    }
    return CExportDiagnostics::Capture(result, path, digest);
}
}

void MainWindow::ConfigurePsdRecoveryAction()
{
    m_psdRecoveryAction = new QAction(QStringLiteral("从工程恢复 PSD…"), this);
    m_psdRecoveryAction->setObjectName(QStringLiteral("recoverPsdFromProject"));
    m_psdRecoveryAction->setStatusTip(QStringLiteral("读取 CMO3 中保存的来源文档与图层，不改变当前模型"));
    connect(m_psdRecoveryAction, &QAction::triggered, this, [this] { RecoverPsdFromProject(); });
    m_savedPsdRecoveryAction = new QAction(QStringLiteral("另存已保存的 PSD…"), this);
    m_savedPsdRecoveryAction->setObjectName(QStringLiteral("recoverSavedPsdSources"));
    m_savedPsdRecoveryAction->setStatusTip(QStringLiteral("原样另存项目保存的 PSD 版本，不改变当前素材关联"));
    m_savedPsdRecoveryAction->setEnabled(false);
    connect(m_savedPsdRecoveryAction, &QAction::triggered, this, &MainWindow::RecoverSavedPsdSources);
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
    const auto protectedPaths = PsdRecoveryProtectedPaths();
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
    catalog->protectedPaths.append(protectedPaths);
    ShowPsdRecoveryCatalog(std::move(catalog));
}

void MainWindow::ShowPsdRecoveryCatalog(std::shared_ptr<const CPsdSourceCatalog> frozen)
{
    const QPointer<MainWindow> alive(this);
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

bool MainWindow::HasSavedPsdSources() const
{
    if (!m_document.IsLoaded()) return false;
    const auto& artwork = m_document.Artwork();
    for (const auto& revision : artwork.revisions) {
        const auto family = artwork.families.constFind(revision.familyId);
        if (family != artwork.families.cend() && family->kind == live2dstruct::CModelArtwork::SourceKind::Psd &&
            revision.sourceDigest.size() == 32 && revision.sourceBytes.startsWith("8BPS")) return true;
    }
    return false;
}

QStringList MainWindow::PsdRecoveryProtectedPaths() const
{
    QStringList paths = m_protectedSessionPaths;
    paths.append(m_currentMoc); paths.append(m_document.SourcePath());
    if (m_openglwindows) paths.append(m_openglwindows->GetCurrentModelJson());
    for (const auto& source : m_document.TextureSources()) paths.append(source.path);
    for (const auto& revision : m_document.Artwork().revisions) paths.append(revision.path);
    return CPsdRecoveryDialog::CaptureProtectedPaths(paths).values();
}

void MainWindow::RecoverSavedPsdSources()
{
    const QPointer<MainWindow> alive(this);
    if (m_psdRecoveryDialog) { m_psdRecoveryDialog->show(); if (alive && m_psdRecoveryDialog) m_psdRecoveryDialog->raise(); return; }
    if (!HasSavedPsdSources() || m_psdRecoveryActive || m_batchExporting || !m_psdBatch.isNull() || m_previewExport.IsBusy() ||
        m_activeExportControl || m_closingModel || m_artworkLoading || m_applyingSession ||
        m_materialEditor.IsActive() || m_atlasEditor.IsActive()) return;
    live2dstruct::CModelArtwork::State artwork;
    QString error;
    if (!live2dstruct::CModelArtwork::Freeze(m_document.Artwork(), artwork, &error)) {
        statusBar()->showMessage(QStringLiteral("无法读取已保存的 PSD 来源：%1").arg(error), 15000); return;
    }
    const auto source = m_document.SourceDigest(); const auto revision = m_document.Revision();
    const auto protectedPaths = PsdRecoveryProtectedPaths();
    m_psdRecoveryActive = true; m_batchExporting = true;
    auto control = std::make_shared<CExportControl>(); m_activeExportControl = control;
    RefreshExportActions(); if (!alive) return;
    auto catalog = std::make_shared<CPsdSourceCatalog>();
    const bool prepared = runSourceTask(this, control, QStringLiteral("正在准备项目保存的 PSD 来源版本"),
        [artwork = std::move(artwork), catalog, control](const CExportTaskRunner::GuiCall&, QString* detail) {
            return live2dstruct::CArtworkPsdRecovery::Prepare(artwork, *catalog, detail, control.get());
        }, &error);
    if (!alive) return;
    m_activeExportControl.reset();
    const bool changed = m_document.SourceDigest() != source || m_document.Revision() != revision;
    if (!prepared || changed || m_closeAfterExport) {
        const bool canceled = control->WasCanceled() || m_closeAfterExport;
        FinishPsdRecovery(); if (!alive) return;
        statusBar()->showMessage(canceled ? QStringLiteral("PSD 来源准备已取消") : changed ?
            QStringLiteral("项目已改变，请重新选择已保存的 PSD 来源") : QStringLiteral("PSD 来源准备失败：%1").arg(error), 15000);
        return;
    }
    catalog->protectedPaths.append(protectedPaths);
    ShowPsdRecoveryCatalog(std::move(catalog));
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
    QStringList sources = catalog->protectedPaths; sources.append(catalog->sourcePath);
    for (const auto& item : catalog->documents) sources.append(item.origin.sourcePath);
    const auto protectedPaths = CPsdRecoveryDialog::CaptureProtectedPaths(sources);
    if (!CPsdRecoveryDialog::ValidateProtectedOutputPath(protectedPaths, frozenPath, error)) return false;
    const auto& document = catalog->documents[documentIndex];
    const bool original = document.Original() != nullptr;
    if (original && !document.Original()->IsValid()) { if (error) *error = QStringLiteral("这个 PSD 来源版本不完整，不能原样另存"); return false; }
    const QPointer<MainWindow> alive(this);
    const QPointer<CPsdRecoveryDialog> dialog(m_psdRecoveryDialog);
    const auto diagnostics = sourceDiagnostics(*catalog, documentIndex);
    if (diagnostics.ErrorCount()) { if (error) *error = diagnostics.FirstError(); return false; }
    auto control = std::make_shared<CExportControl>(); m_activeExportControl = control;
    control->SetHandler([protectedPaths, frozenPath](const CExportControl::Progress&, QString* reason) {
        return CPsdRecoveryDialog::ValidateProtectedOutputPath(protectedPaths, frozenPath, reason);
    });
    PsdBatchFileResult result;
    CExportSourceInfo::Details sourceInfo;
    sourceInfo.method = original ? CExportSourceInfo::Method::OriginalPsd : CExportSourceInfo::Method::ReconstructedLayers;
    sourceInfo.documentId = original ? document.origin.revisionId : document.Reconstructed()->id;
    sourceInfo.documentName = original ? document.origin.fileName : document.Reconstructed()->name;
    sourceInfo.familyId = document.origin.familyId; sourceInfo.revisionId = document.origin.revisionId;
    sourceInfo.inputDigest = original ? document.Original()->Directory()->sha256 :
        document.origin.sourceDigest.isEmpty() ? catalog->sourceDigest : document.origin.sourceDigest;
    result.sourceInfo = CExportSourceInfo::Capture(sourceInfo);
    result.index = 0; result.sourcePath = document.origin.sourcePath.isEmpty() && !original ? catalog->sourcePath : document.origin.sourcePath; result.outputPath = frozenPath;
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
    const bool written = runSourceTask(dialog, control, original ? QStringLiteral("正在原样另存 PSD 原文件") : QStringLiteral("正在重建所选 PSD 文档"),
        [catalog, documentIndex, protectedPaths, frozenPath, control](const CExportTaskRunner::GuiCall&, QString* reason) {
            if (!CPsdRecoveryDialog::ValidateProtectedOutputPath(protectedPaths, frozenPath, reason)) return false;
            const auto& selected = catalog->documents[documentIndex];
            if (const auto* source = selected.Original()) return source->Write(frozenPath, control.get(), reason);
            return CPsdSourceWriter::Write(frozenPath, *selected.Reconstructed(), control.get(), reason);
        }, &detail);
    if (!alive) return false;
    m_activeExportControl.reset();
    result.elapsedMs = elapsed.elapsed(); result.outputCommitted = control->IsCommitted();
    if (original && written && result.outputCommitted) {
        sourceInfo.outputDigest = sourceInfo.inputDigest;
        result.sourceInfo = CExportSourceInfo::Capture(sourceInfo);
    }
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
    statusBar()->showMessage(written ? (original ? QStringLiteral("PSD 原文件已原样另存：%1") : QStringLiteral("来源图层已重建为 PSD：%1")).arg(frozenPath) :
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
