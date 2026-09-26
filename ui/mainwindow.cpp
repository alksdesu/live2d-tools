#include "mainwindow.h"


#include "QPainter"
#include "QLineEdit"
#include <QAction>
#include <QActionGroup>
#include <QCheckBox>
#include <QMenu>
#include <QMenuBar>
#include <QToolBar>
#include <QStatusBar>
#include <QDir>
#include <QCoreApplication>
#include <QEventLoop>
#include <QDirIterator>
#include "ui/cpsdbatchdialog.h"
#include "ui/psdbatchjob.h"
#include "ui/cexportresultspanel.h"
#include "ui/cmodeltreepanel.h"
#include "ui/cparampanel.h"
#include "ui/cmotionpanel.h"
#include "ui/cphysicspanel.h"
#include "ui/cblendshapepanel.h"
#include "ui/ctexturepanel.h"
#include "ui/cinspectorpanel.h"
#include "ui/cmodeldiagnosticspanel.h"
#include "ui/cuserdatapanel.h"
#include "ui/para/ccidjson.h"
#include "std/cmodeljsonwriter.h"
#include "std/cexportcontrol.h"
#include "cmo3/ccmo3builder.h"
#include "live2dstruct/cparameterreferences.h"
#include "ui/theme.h"
#include <QFileDialog>
#include "live2d/LAppDefine.hpp"
#include "live2d/LAppLive2DManager.hpp"
#include "live2d/LAppModel.hpp"
#include "live2d/LAppTextureManager.hpp"
#include <framework/CubismFramework.hpp>
#include <framework/Id/CubismIdManager.hpp>
#include <QProgressDialog>
#include <QProgressBar>
#include <QElapsedTimer>
#include <QScopeGuard>
#include <QTimer>
#include <QApplication>
#include <QVBoxLayout>
#include <QFrame>
#include <QFileInfo>
#include <QSettings>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QScopedValueRollback>
#include <QJsonArray>
#include <QFile>
#include <QCloseEvent>
#include <algorithm>
#include <exception>
#ifdef Q_OS_WIN
#include <windows.h>
#include <windowsx.h>
#endif

struct PreviewExportState
{
    QPointer<OpengGlForm> surface;
    QPointer<CMotionPanel> motion;
    bool hadMotionPanel = false;
    CMotionPanel::PreviewTransportLease transport;
    LAppModel* originalModel = nullptr;
    LAppModel* exportModel = nullptr;
    bool timelinePreview = false;
    bool timelineSaved = false;
    QMap<QString, float> timelineBaseline;
    QString physicsReload, userDataReload;
    bool surfacePaused = false;
    bool timerActive = false;
    int timerInterval = 0;
    float scale = 1;
    QVector2D position;
    PsdExportStage failureStage = PsdExportStage::Preparing;
    bool outputCommitted = false;
    QVector<QPair<QPointer<QWidget>, bool>> widgets;
    QVector<QPair<QPointer<QObject>, bool>> signalStates;
    QVector<QPointer<QAction>> actions;
    QSet<QString> hiddenDrawables, hiddenDeformerDrawables, dragTargets;
    QHash<QString, float> drawableOpacity;
    QHash<QString, int> drawableDrawOrder, drawableOrderGroups;
    QVector<live2dstruct::CDrawableOrderGroup> drawOrderGroups;
    QHash<QString, QHash<int, QVector<float>>> meshOverlayPositions;
    QHash<QString, int> meshOverlayKeyformIndices;
    QHash<QString, QVector<quint16>> meshIndexOverrides;
};

namespace {
PsdExportStage exportStage(CExportControl::Phase phase)
{
    switch (phase) {
    case CExportControl::Phase::Preparing: return PsdExportStage::Preparing;
    case CExportControl::Phase::Rendering:
    case CExportControl::Phase::Extracting: return PsdExportStage::Exporting;
    case CExportControl::Phase::Encoding: return PsdExportStage::Encoding;
    case CExportControl::Phase::Writing:
    case CExportControl::Phase::Committing: return PsdExportStage::Writing;
    }
    return PsdExportStage::Exporting;
}

QProgressDialog* exportProgress(QWidget* parent, int maximum)
{
    auto* progress = new QProgressDialog(QStringLiteral("正在准备导出"), QStringLiteral("取消导出"),
                                         0, maximum, parent);
    auto* bar = new QProgressBar(progress);
    bar->setTextVisible(false);
    progress->setBar(bar);
    auto* label = new QLabel(progress);
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    progress->setLabel(label);
    progress->setWindowModality(Qt::ApplicationModal);
    progress->setAutoClose(false);
    progress->setAutoReset(false);
    progress->setMinimumDuration(0);
    return progress;
}
}

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)

{
    setWindowFlags(Qt::FramelessWindowHint | Qt::Window);
    this->setAcceptDrops(true);
    this->resize(1400, 900);
    this->setWindowTitle("live2d2psd");
    QSettings settings(QStringLiteral("live2d2psd"), QStringLiteral("live2d2psd"));
    m_highPrecisionMasks = settings.value(QStringLiteral("highPrecisionMasks"), false).toBool();
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
    m_closingModel = true;
    StopPsdBatch();
    QSettings settings(QStringLiteral("live2d2psd"), QStringLiteral("live2d2psd"));
    settings.setValue(QStringLiteral("windowGeometry"), saveGeometry());
    settings.setValue(QStringLiteral("windowState"), saveState());
    if (m_motionPanel) settings.setValue(QStringLiteral("motionPanelLayout"), m_motionPanel->SaveLayout());
    if (m_blendShapePanel) settings.setValue(QStringLiteral("blendShapePanelLayout"), m_blendShapePanel->SaveLayout());

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
    if (m_closingModel || m_batchExporting || m_psdBatch || m_previewExport.IsBusy()) { event->ignore(); return; }
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
    if (m_closingModel || m_batchExporting || m_psdBatch || m_previewExport.IsBusy()) { event->ignore(); return; }
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
    if (m_closingModel || m_batchExporting || m_psdBatch || m_previewExport.IsBusy()) return;
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
    if (m_closingModel || m_batchExporting || m_psdBatch || m_previewExport.IsBusy()) return;
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
    if (!m_openglwindows) {
        QMessageBox::information(this, QStringLiteral("PSD 导出"), QStringLiteral("请先打开模型"));
        return;
    }
    if (!ConfirmExportReady(QStringLiteral("PSD 导出"))) return;
    const QString output = CFileOption::GetSvaePsdFile(this);
    if (output.isEmpty()) return;
    CPsdPrintDialog dialog(this);
    dialog.SetName(output);
    if (dialog.exec() != QDialog::Accepted || !dialog.m_IsPrint) return;
    QString error;
    PsdExportStage stage = PsdExportStage::Preparing;
    bool committed = false;
    bool canceled = false;
    bool ok = false;
    const QString modelPath = m_openglwindows->GetCurrentModelJson();
    {
        const QScopedValueRollback<bool> exporting(m_batchExporting, true);
        m_singleExportProgress = exportProgress(this, 0);
        connect(m_singleExportProgress, &QProgressDialog::canceled, this, [this] {
            if (m_activeExportControl) m_activeExportControl->Cancel();
            statusBar()->showMessage(QStringLiteral("正在取消导出并恢复预览"));
        });
        const auto closeProgress = qScopeGuard([this] {
            if (m_singleExportProgress) delete m_singleExportProgress;
        });
        m_singleExportProgress->show();
        try {
            ok = doExportOne(modelPath, output, dialog.m_IsAll, dialog.m_IsFenZu,
                             dialog.m_IsTexExtract, dialog.m_IsCubismCompat, &error, &stage, &committed,
                             dialog.m_IsSnapshot, &canceled);
        } catch (const std::exception& exception) {
            error = QStringLiteral("导出发生异常：%1").arg(QString::fromUtf8(exception.what()));
        } catch (...) {
            error = QStringLiteral("导出发生未知异常");
        }
    }
    RefreshExportActions();
    if (m_closeAfterExport) {
        QTimer::singleShot(0, this, [this] { m_closeAfterExport = false; close(); });
        return;
    }
    if (ok) {
        QMessageBox::information(this, QStringLiteral("PSD 导出完成"), QStringLiteral("已保存：%1").arg(output));
        QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(output).absolutePath()));
    } else if (canceled) {
        statusBar()->showMessage(QStringLiteral("导出已取消，目标文件未被替换"), 8000);
    } else {
        const QString message = committed ? QStringLiteral("文件已写出，但预览恢复未完成：\n%1\n%2").arg(output, error) : error;
        QMessageBox::warning(this, QStringLiteral("PSD 导出失败"), message);
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
    if (m_userDataPanel && m_userDataPanel->HasUnsavedChanges()) {
        const auto saveUserData = QMessageBox::question(
            this, QStringLiteral("网格标签未保存"),
            QStringLiteral("保存当前网格标签后导出 cmo3？"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
        if (saveUserData != QMessageBox::Yes) return;
        QString error;
        if (!m_userDataPanel->SaveUserData(&error)) {
            QMessageBox::warning(this, QStringLiteral("保存 UserData 失败"), error);
            return;
        }
    }

    const QString defaultName = QFileInfo(m_currentMoc).completeBaseName() + ".cmo3";
    const QString outPath = QFileDialog::getSaveFileName(
        this, "导出 cmo3 工程", defaultName, "Cubism 工程 (*.cmo3)");
    if(outPath.isEmpty()) return;

    QMessageBox confirmation(QMessageBox::Question, QStringLiteral("导出 cmo3 工程"),
        "将从 moc3 运行时数据恢复 Cubism Editor 工程。\n\n"
        "以下信息在 moc3 中不存在，无法恢复：\n"
        "· 变形器路径整理、画师备注\n"
        "· 原始 PSD 关联（贴图以每网格图层重建）\n"
        "· 参数分组的折叠状态等编辑器偏好\n\n"
        "是否继续？",
        QMessageBox::Yes | QMessageBox::No, this);
    confirmation.setDefaultButton(QMessageBox::Yes);
    auto* currentDefaults = new QCheckBox(QStringLiteral("将当前参数值作为默认值"), &confirmation);
    currentDefaults->setToolTip(QStringLiteral("勾选后会替代模型定义的默认值。工作会话始终可以单独保存当前参数值。"));
    confirmation.setCheckBox(currentDefaults);
    if (confirmation.exec() != QMessageBox::Yes) return;

    statusBar()->showMessage("正在生成 cmo3 工程…");
    QApplication::setOverrideCursor(Qt::WaitCursor);

    cmo3::CCmo3Builder::Options options;
    options.parameterDefaultMode = currentDefaults->isChecked()
        ? cmo3::CCmo3Builder::ParameterDefaultMode::CurrentValues
        : cmo3::CCmo3Builder::ParameterDefaultMode::ModelDefinition;
    options.mocPath = m_currentMoc;
    options.outPath = outPath;
    {
        CModelJson modelJson;
        modelJson.Parse(m_openglwindows->GetCurrentModelJson());
        if (!modelJson.IsOpen()) {
            QApplication::restoreOverrideCursor();
            statusBar()->showMessage(QStringLiteral("cmo3 导出失败"));
            QMessageBox::warning(this, QStringLiteral("导出失败"),
                                 QStringLiteral("无法读取当前 model3.json，请重新打开模型后再导出。"));
            return;
        }
        const QDir dir(m_currentModelDir);
        if (!modelJson.m_DisplayInfo.isEmpty())
            options.cdiPath = dir.filePath(modelJson.m_DisplayInfo);
        if (!modelJson.m_Physics.isEmpty())
            options.physicsPath = dir.filePath(modelJson.m_Physics);
        if (!modelJson.m_UserData.isEmpty())
            options.userDataPath = dir.filePath(modelJson.m_UserData);
    }
    options.parameterValues = m_document.ParameterValues();
    if (options.parameterDefaultMode == cmo3::CCmo3Builder::ParameterDefaultMode::CurrentValues) {
        auto values = m_paramPanel ? m_paramPanel->CurrentValues() : QMap<QString, float>();
        auto* manager = m_openglwindows->IsModelReady() ? LAppLive2DManager::GetInstance() : nullptr;
        auto* model = manager && manager->GetModelNum() ? manager->GetModel(0) : nullptr;
        if (model) values = model->GetParameterValues();
        for (const auto& parameter : m_document.Parameters())
            options.parameterValues.insert(parameter.id, values.value(parameter.id, m_document.ParameterValue(parameter.id)));
    }
    options.parameterMetadata = m_document.ParameterMetadataOverrides();
    options.hiddenIds = m_document.HiddenIds();
    options.objectVisibility = m_document.VisibilityState();
    options.drawableOpacityOverrides = m_document.DrawableOpacityOverrides();
    options.drawableDrawOrderOverrides = m_document.DrawableDrawOrderOverrides();
    options.drawableIndexOverrides = m_document.DrawableIndexOverrides();

    QString error;
    const bool ok = m_document.BuildPreviewMoc(options.mocBytes, &error) && cmo3::CCmo3Builder::Build(options, &error);
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
    SaveSession();
}

bool MainWindow::SaveSession()
{
    if (!m_workspaceReady || !m_openglwindows || !m_paramPanel || m_currentMoc.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("保存会话"), QStringLiteral("请先打开模型并等待工作区准备完成"));
        return false;
    }
    const QString defaultPath = QFileInfo(m_currentMoc).absolutePath() + "/" +
        QFileInfo(m_currentMoc).completeBaseName() + QStringLiteral(".live2d-session.json");
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("保存工作会话"),
                                                       defaultPath, QStringLiteral("Live2D 会话 (*.live2d-session.json);;JSON (*.json)"));
    if (path.isEmpty()) return false;

    live2dstruct::CModelSession session;
    QString animationError;
    if (m_motionPanel && !m_motionPanel->CaptureTimelineState(&session.animation, &animationError)) {
        QMessageBox::warning(this, QStringLiteral("保存会话失败"), animationError);
        return false;
    }
    session.sourceMoc = QFileInfo(m_currentMoc).absoluteFilePath();
    session.sourceDigest = m_document.SourceDigest();
    session.structure = m_document.ParameterStructure();
    const QMap<QString, float> currentValues = m_keyformEditor.IsActive()
        ? m_keyformBaseValues : m_paramPanel->CurrentValues();
    for (const auto& parameter : m_document.Parameters())
        session.parameters.insert(parameter.id, m_document.ParameterValue(parameter.id));
    for (auto value = currentValues.cbegin(); value != currentValues.cend(); ++value)
        session.parameters.insert(value.key(), value.value());
    if (m_exportState && m_exportState->timelinePreview) {
        session.parameters = m_exportState->timelineBaseline;
    } else if (m_openglwindows->isValid() && RuntimeMatchesDocument()) {
        auto* manager = LAppLive2DManager::GetInstance();
        auto* model = manager && manager->GetModelNum() > 0 ? manager->GetModel(0) : nullptr;
        if (model && model->HasTimelinePreview()) session.parameters = model->GetTimelineBaseParameters();
    }
    session.parameterMetadata = m_document.ParameterMetadataOverrides();
    session.visibility = m_document.VisibilityState();
    if (m_treePanel) {
        session.locked = m_treePanel->LockedState();
    }
    session.drawableOpacity = m_document.DrawableOpacityOverrides();
    session.drawableDrawOrder = m_document.DrawableDrawOrderOverrides();
    session.drawableKeyformPositions = m_document.DrawableKeyformPositionOverrides();
    session.drawableIndices = m_document.DrawableIndexOverrides();
    session.drawableMaskIndices = m_document.DrawableMaskIndexOverrides();
    session.deformerParentIndices = m_document.DeformerParentOverrides();
    session.deformerRotationKeyforms = m_document.DeformerRotationKeyformOverrides();
    session.deformerWarpKeyforms = m_document.DeformerWarpKeyformOverrides();
    session.drawableMaskInverted = m_document.DrawableMaskInvertedOverrides();
    session.viewScale = m_exportState ? m_exportState->scale : m_openglwindows->ViewScale();
    const QVector2D position = m_exportState ? m_exportState->position : m_openglwindows->ViewPosition();
    session.viewPosition = QPointF(position.x(), position.y());
    QString saveError;
    if (!session.Save(path, &saveError)) {
        QMessageBox::warning(this, QStringLiteral("保存会话失败"), saveError);
        return false;
    }
    m_sessionDirty = false;
    if (m_exportState && m_motionPanel && m_motionPanel->IsPreviewTransportSuspended()) m_exportState->timelineSaved = true;
    if (m_motionPanel) m_motionPanel->MarkTimelineSaved();
    setWindowTitle(QStringLiteral("live2d2psd"));
    statusBar()->showMessage(QStringLiteral("会话已保存：%1").arg(path), 5000);
    return !m_motionPanel || m_motionPanel->IsPreviewTransportSuspended() || !m_motionPanel->HasUnsavedTimelineChanges();
}

void MainWindow::on_actionLoadSession_triggered()
{
    if (m_closingModel || m_batchExporting || m_psdBatch || m_previewExport.IsBusy()) return;
    if (!m_workspaceReady || !m_openglwindows || !m_motionPanel) {
        QMessageBox::information(this, QStringLiteral("载入会话"), QStringLiteral("请先打开模型并等待工作区准备完成"));
        return;
    }
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("载入工作会话"),
                                                       QString(), QStringLiteral("Live2D 会话 (*.live2d-session.json);;JSON (*.json)"));
    if (path.isEmpty()) return;
    live2dstruct::CModelSession session;
    QString loadError;
    if (!live2dstruct::CModelSession::Load(path, &session, &loadError)) {
        QMessageBox::warning(this, QStringLiteral("载入会话失败"), loadError);
        return;
    }
    if (session.sourceMoc.isEmpty() ||
        QFileInfo(m_currentMoc).absoluteFilePath() != QFileInfo(session.sourceMoc).absoluteFilePath()) {
        QMessageBox::warning(this, QStringLiteral("载入会话失败"), QStringLiteral("会话对应的模型不是当前打开的模型"));
        return;
    }
    live2dstruct::CModelDocument candidate;
    if (!RuntimeMatchesDocument(&loadError) || !session.RestoreDocument(m_document, candidate, &loadError)) {
        QMessageBox::warning(this, QStringLiteral("载入会话失败"), loadError);
        return;
    }
    if (!ConfirmCloseWithUnsavedChanges(true)) return;
    const QScopedValueRollback<bool> restoring(m_applyingSession, true);
    if (m_openglwindows && !m_openglwindows->FinishMeshDrag(false, &loadError)) {
        QMessageBox::warning(this, QStringLiteral("载入会话失败"), loadError);
        return;
    }
    if (!m_keyformEditor.Restore(std::move(candidate), &loadError)) {
        QMessageBox::warning(this, QStringLiteral("载入会话失败"), loadError);
        return;
    }
    m_keyformBaseValues.clear();
    ClearTimelinePreview();
    RefreshKeyformUi();
    if (!m_motionPanel->RestoreTimelineState(session.animation, &loadError)) {
        QMessageBox::warning(this, QStringLiteral("载入会话失败"), loadError);
        return;
    }
    RefreshParameterStructureUi();
    if (m_paramPanel) {
        const QSignalBlocker blocker(m_paramPanel);
        QMap<QString, float> values;
        for (const auto& parameter : m_document.Parameters())
            values.insert(parameter.id, m_document.ParameterValue(parameter.id));
        m_paramPanel->ClearPreviewValues(values);
        for (auto value = values.cbegin(); value != values.cend(); ++value)
            m_paramPanel->ApplyExternalValue(value.key(), value.value(), false, false, false);
    }
    if (m_blendShapePanel) {
        m_blendShapePanel->RefreshParameterMetadata();
        for (const auto& parameter : m_document.Parameters())
            m_blendShapePanel->SyncParameterValue(parameter.id, m_document.ParameterValue(parameter.id));
    }
    if (m_treePanel) {
        const QSignalBlocker blocker(m_treePanel);
        m_treePanel->ApplyLockedState(session.locked);
        QMap<QString, bool> visibility;
        for (const auto& part : m_document.Parts())
            visibility.insert(QStringLiteral("part|") + part.id, part.visible);
        for (const auto& drawable : m_document.Drawables())
            visibility.insert(QStringLiteral("artmesh|") + drawable.id, drawable.visible);
        for (const auto& deformer : m_document.Deformers())
            visibility.insert((deformer.type == live2dstruct::CModelDeformer::Warp
                ? QStringLiteral("warp|") : QStringLiteral("rotation|")) + deformer.id, deformer.visible);
        m_treePanel->ApplyVisibilityState(visibility);
    }
    if (m_inspectorPanel) m_inspectorPanel->SetLockedState(m_document.LockedKeys());
    m_openglwindows->SetViewState(session.viewScale, QVector2D(session.viewPosition.x(), session.viewPosition.y()));
    SyncDocumentToRuntime();
    if (m_inspectorPanel) {
        m_inspectorPanel->RefreshSelection();
        m_inspectorPanel->ClearHistory();
    }
    if (m_diagnosticsPanel) m_diagnosticsPanel->Refresh();
    m_historySource = HistorySource::None;
    RefreshUndoActions();
    m_sessionDirty = false;
    setWindowTitle(QStringLiteral("live2d2psd"));
    statusBar()->showMessage(QStringLiteral("会话已载入"), 5000);
}

bool MainWindow::RuntimeMatchesDocument(QString* error) const
{
    if (error) error->clear();
    if (m_previewExport.IsBusy()) {
        if (error) *error = QStringLiteral("导出正在占用预览，请等待或恢复预览");
        return false;
    }
    if (!m_document.IsLoaded()) {
        if (error) *error = QStringLiteral("尚未加载模型数据");
        return false;
    }
    if (!m_openglwindows || !m_openglwindows->IsModelReady()) return true;
    auto* manager = LAppLive2DManager::GetInstance();
    auto* model = manager && manager->GetModelNum() > 0 ? manager->GetModel(0) : nullptr;
    if (!model || model->GetSourceMocDigest() != m_document.SourceDigest()) {
        if (error) *error = QStringLiteral("预览模型与编辑数据的源文件不同，请重新打开模型");
        return false;
    }
    return true;
}

bool MainWindow::PublishKeyformPreview(const live2dstruct::CModelDocument& document,
                                      const QMap<QString, float>& pose, bool editing, QString* error)
{
    if (m_closingModel || m_batchExporting || m_psdBatch || m_previewExport.IsBusy()) {
        if (error) *error = QStringLiteral("模型正在关闭或导出，请稍后编辑");
        return false;
    }
    if (!RuntimeMatchesDocument(error)) return false;
    if (!m_openglwindows || !m_openglwindows->IsModelReady()) return true;
    QByteArray bytes;
    if (!document.BuildPreviewMoc(bytes, error)) return false;
    if (!editing) {
        QMap<QString, float> values;
        if (!m_runtimePanelsInitialized)
            for (const auto& parameter : document.Parameters()) values.insert(parameter.id, document.ParameterValue(parameter.id));
        if (!m_runtimePanelsInitialized && (!document.ParameterStructure().columns.isEmpty() || !document.ParameterStructure().counts.isEmpty())) {
            auto* manager = LAppLive2DManager::GetInstance();
            auto* model = manager && manager->GetModelNum() ? manager->GetModel(0) : nullptr;
            if (!model) { if (error) *error = QStringLiteral("模型预览尚未就绪"); return false; }
            const auto previous = model->GetParameterValues();
            for (auto value = previous.cbegin(); value != previous.cend(); ++value) if (!values.contains(value.key())) {
                const auto references = ParameterReferences(value.key());
                if (!references.isEmpty()) { if (error) *error = references.join(QLatin1Char('\n')); return false; }
            }
            ParameterStructureSeeds entering;
            for (auto value = values.cbegin(); value != values.cend(); ++value)
                if (!previous.contains(value.key())) entering.insert(value.key(), ParameterStructureSeed());
            QMap<QString, float> prepared;
            return m_openglwindows->ApplyParameterStructurePreview(bytes, document.SourceDigest(), entering, values, false, prepared, error);
        }
        return m_openglwindows->ApplyParameterDefinitionPreview(bytes, document.SourceDigest(), values, false, error);
    }
    return m_openglwindows->ApplyMocPreview(bytes, document.SourceDigest(), editing ? &pose : nullptr, error);
}

bool MainWindow::PublishParameterDefinitions(const live2dstruct::CModelDocument& document,
                                              const QMap<QString, float>& values, bool editing, QString* error)
{
    if (m_closingModel || m_batchExporting || m_psdBatch || m_previewExport.IsBusy()) {
        if (error) *error = QStringLiteral("模型正在关闭或导出，请稍后编辑参数定义");
        return false;
    }
    if (!RuntimeMatchesDocument(error)) return false;
    QByteArray bytes;
    if (!document.BuildPreviewMoc(bytes, error)) return false;
    if (!m_openglwindows || !m_openglwindows->IsModelReady()) return true;
    return m_openglwindows->ApplyParameterDefinitionPreview(bytes, document.SourceDigest(), values, editing, error);
}

QStringList MainWindow::ParameterReferences(const QString& id, bool includeTimeline) const
{
    QStringList references;
    QString error;
    if (!m_openglwindows || !live2dstruct::CParameterReferences::ScanModel(
            m_openglwindows->GetCurrentModelJson(), id, references, &error))
        return {error.isEmpty() ? QStringLiteral("尚未加载模型引用，无法检查参数依赖") : error};
    if (includeTimeline && m_motionPanel) for (auto track = m_motionPanel->Timeline().Tracks().cbegin();
                           track != m_motionPanel->Timeline().Tracks().cend(); ++track)
        if (track.key().target == CMotionTarget::Parameter && track.key().id == id)
            references.append(QStringLiteral("当前时间轴包含此参数轨道"));
    if (m_physicsPanel && m_physicsPanel->HasUnsavedChanges()) {
        QStringList physics;
        if (!live2dstruct::CParameterReferences::ScanPhysics(m_physicsPanel->DocumentSnapshot(), id, physics, &error))
            references.append(QStringLiteral("无法检查物理草稿：%1").arg(error));
        else for (const auto& reference : physics) references.append(QStringLiteral("物理草稿：%1").arg(reference));
    }
    references.removeDuplicates();
    return references;
}

bool MainWindow::PrepareParameterStructurePose(const live2dstruct::CModelDocument& document,
                                               const live2dstruct::CModelEdit& command, bool forward,
                                               QMap<QString, float>& pose, QString* error) const
{
    if (!command.runtime) { if (error) *error = QStringLiteral("缺少参数预览恢复状态"); return false; }
    const QVariant& archived = forward ? command.runtime->after : command.runtime->before;
    if (!archived.isValid()) return true;
    if (!archived.canConvert<ParameterStructureSeeds>()) {
        if (error) *error = QStringLiteral("参数历史的预览恢复值无效");
        return false;
    }
    QSet<QString> previousIds;
    for (const auto& parameter : m_document.Parameters()) previousIds.insert(parameter.id);
    const auto seeds = archived.value<ParameterStructureSeeds>();
    for (const auto& parameter : document.Parameters()) {
        const auto seed = seeds.constFind(parameter.id);
        if (previousIds.contains(parameter.id) || seed == seeds.cend() || seed->mode == ParameterStructureSeed::Mode::Defaults) continue;
        if (seed->mode != ParameterStructureSeed::Mode::Values) {
            if (error) *error = QStringLiteral("参数恢复模式无效");
            return false;
        }
        const auto metadata = document.ParameterMetadata(parameter.id);
        float value = seed->editorPose.value_or(seed->runtime.current);
        if (seed->runtime.minimum != metadata.minimum || seed->runtime.maximum != metadata.maximum)
            value = std::clamp(value, metadata.minimum, metadata.maximum);
        pose[parameter.id] = value;
    }
    return true;
}

bool MainWindow::PublishParameterStructure(const live2dstruct::CModelDocument& document,
                                           const live2dstruct::CModelEdit& command, bool forward, bool restoring,
                                           const QMap<QString, float>& values, bool editing,
                                           QMap<QString, float>& pose, QString* error)
{
    if (m_closingModel || m_batchExporting || m_psdBatch || m_previewExport.IsBusy() || !command.runtime) {
        if (error) *error = QStringLiteral("模型正在关闭或导出，请稍后编辑参数");
        return false;
    }
    if (!RuntimeMatchesDocument(error)) return false;
    QSet<QString> previousIds, nextIds;
    for (const auto& parameter : m_document.Parameters()) previousIds.insert(parameter.id);
    for (const auto& parameter : document.Parameters()) nextIds.insert(parameter.id);
    QStringList removed;
    for (const auto& id : previousIds) if (!nextIds.contains(id)) {
        const auto references = ParameterReferences(id, !restoring);
        if (!references.isEmpty()) {
            if (error) *error = QStringLiteral("无法移除参数 %1：\n%2").arg(id, references.join(QLatin1Char('\n')));
            return false;
        }
        removed.append(id);
    }
    QByteArray bytes;
    if (!document.BuildPreviewMoc(bytes, error)) return false;
    if (!m_openglwindows || !m_openglwindows->IsModelReady()) {
        QMap<QString, float> candidatePose;
        for (const auto& parameter : document.Parameters())
            candidatePose.insert(parameter.id, pose.value(parameter.id, document.ParameterValue(parameter.id)));
        pose.swap(candidatePose);
        return true;
    }
    auto* manager = LAppLive2DManager::GetInstance();
    auto* model = manager && manager->GetModelNum() ? manager->GetModel(0) : nullptr;
    if (!model) { if (error) *error = QStringLiteral("模型预览尚未就绪"); return false; }
    ParameterStructureSeeds leaving;
    if (!model->CaptureParameterStructureSeeds(removed, leaving, error)) return false;
    const QVariant& targetState = forward ? command.runtime->after : command.runtime->before;
    if (targetState.isValid() && !targetState.canConvert<ParameterStructureSeeds>()) {
        if (error) *error = QStringLiteral("参数历史的预览恢复值无效");
        return false;
    }
    const auto archived = targetState.value<ParameterStructureSeeds>();
    ParameterStructureSeeds entering;
    for (const auto& id : nextIds) if (!previousIds.contains(id))
        entering.insert(id, restoring ? ParameterStructureSeed() : archived.value(id));
    QVariant captured = QVariant::fromValue(leaving);
    if (!m_openglwindows->ApplyParameterStructurePreview(bytes, document.SourceDigest(), entering, values, editing, pose, error))
        return false;
    (forward ? command.runtime->before : command.runtime->after).swap(captured);
    return true;
}

void MainWindow::RefreshParameterStructureUi()
{
    QByteArray bytes;
    QString error;
    if (!m_document.BuildPreviewMoc(bytes, &error)) { statusBar()->showMessage(error, 8000); return; }
    if (m_blendShapePanel && !m_blendShapePanel->BuildBlendShapesBytes(bytes, m_cidJson, &error, m_document.SourceDigest()))
        statusBar()->showMessage(error, 8000);
    const QSet<QString>* blendShapes = m_blendShapePanel ? &m_blendShapePanel->getBlendShapeParamIds() : nullptr;
    if (m_paramPanel) m_paramPanel->ReconcileParameters(m_cidJson, blendShapes);
    if (m_motionPanel) {
        QStringList ids;
        for (const auto& parameter : m_document.Parameters()) ids.append(parameter.id);
        m_motionPanel->SetParameterIds(ids);
    }
    RefreshModelTree();
    if (m_inspectorPanel) {
        m_inspectorPanel->SetDocument(&m_document);
        m_inspectorPanel->SetSelections(m_selectedObjectIds, m_selectedObjectTypes);
        m_inspectorPanel->SetLockedState(m_document.LockedKeys());
    }
    SyncParameterDefinitionPreview();
    RefreshKeyformUi();
    if (m_diagnosticsPanel) m_diagnosticsPanel->Refresh();
    RefreshUndoActions();
}

void MainWindow::SyncParameterDefinitionPreview()
{
    if (!m_paramPanel || m_previewExport.IsBusy() || !m_openglwindows || !m_openglwindows->IsModelReady()) return;
    auto* manager = LAppLive2DManager::GetInstance();
    auto* model = manager && manager->GetModelNum() ? manager->GetModel(0) : nullptr;
    if (!model) return;
    const auto values = model->GetParameterValues();
    const bool editor = model->HasEditorPreview();
    const bool timeline = model->HasTimelinePreview();
    {
        const QSignalBlocker blocker(m_paramPanel);
        if (editor || timeline) {
            const auto baseline = timeline ? model->GetTimelineBaseParameters() : model->GetEditorBaseParameters();
            m_paramPanel->ClearPreviewValues(values);
            m_paramPanel->SetPreviewValues(values, baseline);
        } else if (!m_keyformBaseValues.isEmpty()) m_paramPanel->ClearPreviewValues(values);
        else m_paramPanel->SyncFromModel();
    }
    m_keyformBaseValues = editor ? model->GetEditorBaseParameters() : QMap<QString, float>();
    if (m_blendShapePanel) {
        const auto displayed = m_paramPanel->CurrentValues();
        for (auto value = displayed.cbegin(); value != displayed.cend(); ++value)
            m_blendShapePanel->SyncParameterValue(value.key(), value.value());
    }
}

bool MainWindow::ActivateKeyform(const QString& id, int keyform, QString* error)
{
    return ActivateKeyform(id, QStringLiteral("artmesh"), keyform, error);
}

bool MainWindow::ActivateKeyform(const QString& id, const QString& type, int keyform, QString* error)
{
    if (!m_openglwindows || !m_openglwindows->FinishMeshDrag(false, error)) return false;
    if (m_keyformEditor.IsActive() && m_keyformEditor.Target() == id && m_keyformEditor.Type() == type && m_keyformEditor.Keyform() == keyform)
        return true;
    const bool wasActive = m_keyformEditor.IsActive();
    QMap<QString, float> values = m_paramPanel ? m_paramPanel->CurrentValues() : QMap<QString, float>();
    if (wasActive) values = m_keyformEditor.Pose();
    else if (m_openglwindows->IsModelReady()) {
        auto* model = LAppLive2DManager::GetInstance()->GetModel(0);
        if (model) values = model->GetParameterValues();
    }
    if (!m_keyformEditor.Activate(id, type, keyform, values, error)) return false;
    if (m_motionPanel) m_motionPanel->PauseTimelinePreview();
    if (!wasActive) m_keyformBaseValues = values;
    RefreshKeyformUi();
    statusBar()->showMessage(QStringLiteral("正在编辑 %1 的关键形 %2；Esc 取消拖拽，再按 Esc 结束编辑")
                            .arg(id).arg(keyform));
    return true;
}

bool MainWindow::EndKeyformEditing(QString* error)
{
    QString message;
    QString* result = error ? error : &message;
    if (m_openglwindows && !m_openglwindows->FinishMeshDrag(false, result)) {
        if (!error) statusBar()->showMessage(message, 8000);
        return false;
    }
    const bool active = m_keyformEditor.IsActive();
    if (!active) return true;
    if (!m_keyformEditor.End(result)) {
        if (!error) statusBar()->showMessage(message, 8000);
        return false;
    }
    auto values = m_keyformBaseValues;
    if (m_openglwindows && m_openglwindows->IsModelReady()) {
        auto* model = LAppLive2DManager::GetInstance()->GetModel(0);
        if (model) values = model->GetParameterValues();
    }
    if (m_paramPanel) {
        auto* manager = LAppLive2DManager::GetInstance();
        auto* model = manager && manager->GetModelNum() > 0 ? manager->GetModel(0) : nullptr;
        if (model && model->HasTimelinePreview())
            m_paramPanel->SetPreviewValues(values, model->GetTimelineBaseParameters());
        else m_paramPanel->ClearPreviewValues(values, m_parameterBeingEdited);
    }
    if (m_blendShapePanel) for (auto it = values.cbegin(); it != values.cend(); ++it)
        if (it.key() != m_parameterBeingEdited) m_blendShapePanel->SyncParameterValue(it.key(), it.value());
    m_keyformBaseValues.clear();
    if (m_finishKeyformButton) m_finishKeyformButton->hide();
    return true;
}

bool MainWindow::SubmitInspectorEdit(const live2dstruct::CModelEdit& edit, QString* error)
{
    if (m_previewExport.IsBusy()) {
        if (error) *error = QStringLiteral("导出正在占用预览，请稍后编辑");
        return false;
    }
    if (edit.field != live2dstruct::CModelEdit::ParameterMetadata && edit.field != live2dstruct::CModelEdit::ParameterStructure &&
        m_openglwindows && !m_openglwindows->FinishMeshDrag(false, error)) return false;
    if (live2dstruct::CKeyformEditController::Handles(edit)) {
        const bool active = m_keyformEditor.IsActive();
        auto values = m_paramPanel ? m_paramPanel->CurrentValues() : QMap<QString, float>();
        if (m_openglwindows && m_openglwindows->IsModelReady()) {
            auto* model = LAppLive2DManager::GetInstance()->GetModel(0);
            if (model) values = model->GetParameterValues();
        }
        const int previewKeyform = edit.field == live2dstruct::CModelEdit::KeyformPositions &&
            m_inspectorPanel && m_inspectorPanel->GetSelectionId() == edit.name
                ? m_inspectorPanel->GetActiveKeyformIndex() : edit.index;
        if (!m_keyformEditor.Submit(edit, values, previewKeyform, error)) return false;
        if (edit.field == live2dstruct::CModelEdit::DeformerParent) RefreshModelTree();
        if (edit.field != live2dstruct::CModelEdit::ParameterMetadata && edit.field != live2dstruct::CModelEdit::ParameterStructure &&
            m_keyformEditor.IsActive() && m_motionPanel)
            m_motionPanel->PauseTimelinePreview();
        if (!active && m_keyformEditor.IsActive()) m_keyformBaseValues = values;
        RefreshKeyformUi(true);
        return true;
    }
    if (!EndKeyformEditing(error)) return false;
    if (m_document.ExecuteEdit(edit)) return true;
    if (error) *error = QStringLiteral("属性修改未通过模型校验");
    return false;
}

bool MainWindow::ApplyDocumentHistory(bool undo, live2dstruct::CModelEdit& edit, QString* error)
{
    if (m_previewExport.IsBusy()) {
        if (error) *error = QStringLiteral("导出正在占用预览，请稍后编辑");
        return false;
    }
    live2dstruct::CModelEdit pending;
    if (!(undo ? m_document.PeekUndoEdit(&pending) : m_document.PeekRedoEdit(&pending))) return false;
    if (pending.field != live2dstruct::CModelEdit::ParameterMetadata && pending.field != live2dstruct::CModelEdit::ParameterStructure &&
        m_openglwindows && !m_openglwindows->FinishMeshDrag(false, error)) return false;
    if (live2dstruct::CKeyformEditController::Handles(pending)) {
        if (!m_keyformEditor.History(undo, edit, error)) return false;
        if (edit.field == live2dstruct::CModelEdit::DeformerParent) RefreshModelTree();
        RefreshKeyformUi(true);
    } else {
        if (!EndKeyformEditing(error)) return false;
        if (!(undo ? m_document.UndoEdit(&edit) : m_document.RedoEdit(&edit))) return false;
    }
    m_sessionDirty = true;
    setWindowTitle(QStringLiteral("live2d2psd · 未保存"));
    RefreshUndoActions();
    return true;
}

void MainWindow::RefreshKeyformUi(bool changed)
{
    if (m_finishKeyformButton) m_finishKeyformButton->setVisible(m_keyformEditor.IsActive());
    if (m_keyformEditor.IsActive()) {
        if (m_inspectorPanel)
            m_inspectorPanel->SyncActiveKeyform(m_keyformEditor.Target(), m_keyformEditor.Type(), m_keyformEditor.Keyform());
        if (m_openglwindows) {
            if (m_keyformEditor.Type() == QLatin1String("artmesh"))
                m_openglwindows->SetActiveKeyform(m_keyformEditor.Target(), m_keyformEditor.Keyform());
            else {
                m_openglwindows->do_ShowMesh({});
                m_openglwindows->do_ShowDeformers({m_keyformEditor.Target()});
            }
        }
        if (m_paramPanel) m_paramPanel->SetPreviewValues(m_keyformEditor.Pose(), m_keyformBaseValues);
        if (m_blendShapePanel) for (auto it = m_keyformEditor.Pose().cbegin(); it != m_keyformEditor.Pose().cend(); ++it)
            m_blendShapePanel->SyncParameterValue(it.key(), it.value());
    }
    if (!changed) return;
    m_sessionDirty = true;
    m_historySource = HistorySource::Inspector;
    setWindowTitle(QStringLiteral("live2d2psd · 未保存"));
    if (m_inspectorPanel) {
        const QSignalBlocker blocker(m_inspectorPanel);
        m_inspectorPanel->RefreshSelection();
        m_inspectorPanel->SyncHistoryState();
    }
    if (m_paramPanel) m_paramPanel->SyncHistoryState();
    m_historySource = HistorySource::Inspector;
    if (m_diagnosticsPanel) m_diagnosticsPanel->Refresh();
    RefreshUndoActions();
}

void MainWindow::RefreshModelTree()
{
    if (!m_treePanel) return;
    QByteArray bytes;
    QString error;
    if (!m_document.BuildPreviewMoc(bytes, &error)) { statusBar()->showMessage(error, 8000); return; }
    const QSignalBlocker blocker(m_treePanel);
    if (!m_treePanel->BuildTreeBytes(bytes, m_cidJson, &error)) { statusBar()->showMessage(error, 8000); return; }
    QMap<QString, bool> visibility, locks;
    const auto add = [&](const QString& type, const QString& id, bool visible) {
        const auto key = type + QLatin1Char('|') + id;
        visibility.insert(key, visible);
        locks.insert(key, m_document.IsLocked(type, id));
    };
    for (const auto& part : m_document.Parts()) add(QStringLiteral("part"), part.id, part.visible);
    for (const auto& mesh : m_document.Drawables()) add(QStringLiteral("artmesh"), mesh.id, mesh.visible);
    for (const auto& deformer : m_document.Deformers())
        add(deformer.type == live2dstruct::CModelDeformer::Warp ? QStringLiteral("warp") : QStringLiteral("rotation"),
            deformer.id, deformer.visible);
    m_treePanel->ApplyVisibilityState(visibility);
    m_treePanel->ApplyLockedState(locks);
    if (m_inspectorPanel && m_selectedObjectIds.size() <= 1)
        m_treePanel->HighlightObject(m_inspectorPanel->GetSelectionId(), m_inspectorPanel->GetSelectionType());
}

void MainWindow::SelectModelObjects(const QStringList& ids, const QStringList& types)
{
    if (m_keyformEditor.IsActive() && (ids.size() != 1 || types.value(0) != m_keyformEditor.Type() ||
        ids.value(0) != m_keyformEditor.Target()) && !EndKeyformEditing()) {
        if (m_treePanel) {
            const QSignalBlocker blocker(m_treePanel);
            m_treePanel->HighlightObject(m_keyformEditor.Target(), m_keyformEditor.Type());
        }
        if (m_texturePanel) m_texturePanel->HighlightMesh(m_keyformEditor.Type() == QLatin1String("artmesh")
            ? m_keyformEditor.Target() : QString());
        return;
    }
    if (!m_inspectorPanel) return;
    m_selectedObjectIds = ids;
    m_selectedObjectTypes = types;
    if (m_paramPanel) {
        using Owner = live2dstruct::CMocParameterStructureBuilder::Owner;
        QVector<Owner> targets;
        bool supported = ids.size() == types.size();
        for (int i = 0; i < ids.size() && supported; ++i) {
            if (types[i] == QLatin1String("artmesh")) targets.append({Owner::ArtMesh, ids[i]});
            else if (types[i] == QLatin1String("warp")) targets.append({Owner::Warp, ids[i]});
            else if (types[i] == QLatin1String("rotation")) targets.append({Owner::Rotation, ids[i]});
            else supported = false;
        }
        m_paramPanel->SetBindingTargets(supported ? targets : QVector<Owner>());
        if (!supported) statusBar()->showMessage(QStringLiteral("参数绑定支持网格、弯曲和旋转变形器，请重新选择绑定对象"), 6000);
    }
    m_inspectorPanel->SetSelections(ids, types);
    if (ids.size() == 1 && types.value(0) == QLatin1String("artmesh")) {
        if (m_openglwindows) m_openglwindows->do_ShowMesh(ids.first());
        if (m_texturePanel) m_texturePanel->HighlightMesh(ids.first());
    } else if (ids.isEmpty()) {
        if (m_openglwindows) {
            m_openglwindows->do_ShowMesh({});
            m_openglwindows->do_ShowDeformers({});
        }
        if (m_texturePanel) m_texturePanel->HighlightMesh({});
    }
}

void MainWindow::SyncDocumentToRuntime()
{
    if (!m_openglwindows || !m_openglwindows->IsModelReady() || !RuntimeMatchesDocument()) return;
    m_openglwindows->ClearEditorOverrides();
    if (!m_keyformEditor.IsActive()) for (const auto& parameter : m_document.Parameters())
        m_openglwindows->do_change_para_value(parameter.id, m_document.ParameterValue(parameter.id), false);
    auto* model = LAppLive2DManager::GetInstance()->GetModel(0);
    for (const auto& part : m_document.Parts()) {
        if (!part.visible || model->GetHiddenPartIds().contains(part.id))
            m_openglwindows->do_SetVisibility(part.id, QStringLiteral("part"), part.visible);
    }
    for (const auto& drawable : m_document.Drawables())
        m_openglwindows->do_SetVisibility(drawable.id, QStringLiteral("artmesh"), drawable.visible);
    m_openglwindows->SetHiddenDeformerDrawables(m_document.HiddenDeformerDrawables());
    const auto& opacity = m_document.DrawableOpacityOverrides();
    for (auto it = opacity.cbegin(); it != opacity.cend(); ++it)
        m_openglwindows->do_SetDrawableOpacity(it.key(), it.value());
    const auto& order = m_document.DrawableDrawOrderOverrides();
    for (auto it = order.cbegin(); it != order.cend(); ++it)
        m_openglwindows->do_SetDrawableDrawOrder(it.key(), it.value());
    const auto& indices = m_document.DrawableIndexOverrides();
    for (auto it = indices.cbegin(); it != indices.cend(); ++it)
        m_openglwindows->do_SetDrawableIndices(it.key(), it.value());
    m_openglwindows->SetViewState(m_openglwindows->ViewScale(), m_openglwindows->ViewPosition());
}

bool MainWindow::ConfirmExportReady(const QString& operation)
{
    if (m_closingModel || m_batchExporting || m_psdBatch) return false;
    if (m_previewExport.IsBusy()) {
        statusBar()->showMessage(QStringLiteral("请先恢复或关闭当前预览，再继续导出"));
        return false;
    }
    QString dragError;
    if (m_openglwindows && !m_openglwindows->FinishMeshDrag(false, &dragError)) {
        statusBar()->showMessage(dragError, 8000);
        return false;
    }
    if (!m_document.IsLoaded()) return true;
    QString sourceError;
    if (!RuntimeMatchesDocument(&sourceError) || !m_document.SourceMatchesDisk(&sourceError)) {
        QMessageBox::warning(this, operation + QStringLiteral("已阻止"), sourceError);
        return false;
    }
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

void MainWindow::ApplyVisibilityValues(const QMap<QString, bool>& values)
{
    const QScopedValueRollback<bool> syncing(m_syncingVisibility, true);
    if (m_treePanel) m_treePanel->ApplyVisibilityState(values);
    if (m_inspectorPanel) {
        const QSignalBlocker blocker(m_inspectorPanel);
        m_inspectorPanel->RefreshSelection();
    }
    if (!m_openglwindows || !RuntimeMatchesDocument()) return;
    if (m_motionPanel) m_motionPanel->StopTimelinePreview();
    for (auto it = values.cbegin(); it != values.cend(); ++it) {
        const int separator = it.key().indexOf(QLatin1Char('|'));
        if (separator > 0) m_openglwindows->do_SetVisibility(it.key().mid(separator + 1), it.key().left(separator), it.value());
    }
    m_openglwindows->SetHiddenDeformerDrawables(m_document.HiddenDeformerDrawables());
}

void MainWindow::ApplyDocumentEditToUi(const live2dstruct::CModelEdit& edit, bool forward)
{
    const QVariant& value = forward ? edit.after : edit.before;
    if (edit.field == live2dstruct::CModelEdit::ParameterStructure) {
        RefreshParameterStructureUi();
    } else if (edit.field == live2dstruct::CModelEdit::ParameterValue) {
        const float parameterValue = value.toFloat();
        if (m_paramPanel) m_paramPanel->ApplyExternalValue(edit.name, parameterValue, false, false, false);
        else if (m_openglwindows) m_openglwindows->do_change_para_value(edit.name, parameterValue, false);
    } else if (edit.field == live2dstruct::CModelEdit::ParameterMetadata) {
        if (m_paramPanel) {
            m_paramPanel->RefreshParameterMetadata();
            auto* manager = m_openglwindows && m_openglwindows->IsModelReady() ? LAppLive2DManager::GetInstance() : nullptr;
            auto* model = manager && manager->GetModelNum() ? manager->GetModel(0) : nullptr;
            const bool preview = model && (model->HasEditorPreview() || model->HasTimelinePreview());
            if (!preview && edit.before.toMap().value(QStringLiteral("value")) != edit.after.toMap().value(QStringLiteral("value"))) {
                const QSignalBlocker blocker(m_paramPanel);
                m_paramPanel->ApplyExternalValue(edit.name, m_document.ParameterValue(edit.name), false, false, false);
            }
            SyncParameterDefinitionPreview();
        }
    } else if (edit.field == live2dstruct::CModelEdit::ParameterValues) {
        const QMap<QString, float> values = value.value<QMap<QString, float>>();
        if (m_paramPanel) {
            for (auto it = values.cbegin(); it != values.cend(); ++it)
                m_paramPanel->ApplyExternalValue(it.key(), it.value(), false, false, false);
        } else if (m_openglwindows) {
            for (auto it = values.cbegin(); it != values.cend(); ++it)
                m_openglwindows->do_change_para_value(it.key(), it.value(), false);
        }
    } else if (edit.field == live2dstruct::CModelEdit::Visibility || edit.field == live2dstruct::CModelEdit::VisibilityValues) {
        ApplyVisibilityValues(edit.field == live2dstruct::CModelEdit::VisibilityValues ? value.value<QMap<QString, bool>>() :
            QMap<QString, bool>{{edit.type + QLatin1Char('|') + edit.name, value.toBool()}});
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
    } else if (edit.field == live2dstruct::CModelEdit::MeshIndices) {
        if (m_openglwindows) m_openglwindows->do_SetDrawableIndices(edit.name, value.value<QVector<quint16>>());
        if (m_inspectorPanel) m_inspectorPanel->RefreshSelection();
    } else if (edit.field == live2dstruct::CModelEdit::MaskIndices) {
        if (m_inspectorPanel) m_inspectorPanel->RefreshSelection();
    } else if (edit.field == live2dstruct::CModelEdit::DeformerParent) {
        if (m_inspectorPanel) m_inspectorPanel->RefreshSelection();
    } else if (edit.field == live2dstruct::CModelEdit::RotationKeyforms ||
               edit.field == live2dstruct::CModelEdit::WarpKeyforms) {
        if (m_inspectorPanel) m_inspectorPanel->RefreshSelection();
    } else if (edit.field == live2dstruct::CModelEdit::MaskInverted) {
        if (m_inspectorPanel) m_inspectorPanel->RefreshSelection();
    }
    if (m_paramPanel) m_paramPanel->SyncHistoryState();
    if (m_inspectorPanel) m_inspectorPanel->SyncHistoryState();
}

void MainWindow::SetWorkspaceMode(int mode)
{
    if (mode != m_workspaceMode && !EndKeyformEditing()) {
        for (int i = 0; i < m_workspaceActions.size(); ++i)
            m_workspaceActions[i]->setChecked(i == m_workspaceMode);
        return;
    }
    m_workspaceMode = qBound(0, mode, 2);
    if (m_workspaceActionGroup) {
        for (int i = 0; i < m_workspaceActions.size(); ++i)
            m_workspaceActions[i]->setChecked(i == m_workspaceMode);
    }
    QSettings settings(QStringLiteral("live2d2psd"), QStringLiteral("live2d2psd"));
    settings.setValue(QStringLiteral("workspaceMode"), m_workspaceMode);
    if (m_openglwindows) m_openglwindows->SetMeshEditEnabled(m_workspaceMode == 0);
    if (!m_treePanel) return;

    const bool modeling = m_workspaceMode == 0;
    const bool animation = m_workspaceMode == 1;
    const bool physics = m_workspaceMode == 2;
    if (!animation && m_motionPanel) m_motionPanel->StopTimelinePreview();
    m_treePanel->setVisible(true);
    m_paramPanel->setVisible(modeling || animation);
    m_inspectorPanel->setVisible(modeling || animation);
    m_motionPanel->setVisible(animation);
    m_physicsPanel->setVisible(physics);
    m_blendShapePanel->setVisible(modeling);
    m_texturePanel->setVisible(modeling);
    m_diagnosticsPanel->setVisible(false);
    if (m_userDataPanel) m_userDataPanel->setVisible(false);
    m_historySource = animation ? HistorySource::Timeline : HistorySource::Parameters;
    RefreshUndoActions();
}

void MainWindow::ClearTimelinePreview()
{
    if (m_previewExport.IsBusy()) return;
    if (!EndKeyformEditing()) {
        auto* manager = LAppLive2DManager::GetInstance();
        auto* model = manager && manager->GetModelNum() > 0 ? manager->GetModel(0) : nullptr;
        if (m_motionPanel && model && model->HasTimelinePreview()) m_motionPanel->SetTimelinePreviewAvailable(true);
        return;
    }
    if (!m_openglwindows || !m_openglwindows->isValid()) return;
    auto* manager = LAppLive2DManager::GetInstance();
    auto* model = manager && manager->GetModelNum() > 0 ? manager->GetModel(0) : nullptr;
    if (!model || !model->HasTimelinePreview()) return;
    model->ClearTimelinePreview();
    if (!m_paramPanel) return;
    const auto values = model->GetParameterValues();
    m_openglwindows->SyncPreviewView();
    m_paramPanel->ClearPreviewValues(values, m_parameterBeingEdited);
    if (m_blendShapePanel) for (auto it = values.cbegin(); it != values.cend(); ++it)
        if (it.key() != m_parameterBeingEdited) m_blendShapePanel->SyncParameterValue(it.key(), it.value());
}

void MainWindow::RefreshUndoActions()
{
    const bool loaded = m_openglwindows && !m_closingModel;
    const bool timeline = m_historySource == HistorySource::Timeline && m_motionPanel;
    if (m_undoAction) m_undoAction->setEnabled(loaded && (timeline ? m_motionPanel->CanUndoTimeline() : m_document.CanUndoEdit()));
    if (m_redoAction) m_redoAction->setEnabled(loaded && (timeline ? m_motionPanel->CanRedoTimeline() : m_document.CanRedoEdit()));
}

void MainWindow::UndoDocumentEdit()
{
    if (m_historySource == HistorySource::Timeline && m_motionPanel) {
        m_motionPanel->UndoTimeline();
        RefreshUndoActions();
        return;
    }
    live2dstruct::CModelEdit edit;
    QString error;
    if (!ApplyDocumentHistory(true, edit, &error)) {
        if (!error.isEmpty()) statusBar()->showMessage(error, 8000);
        return;
    }
    ApplyDocumentEditToUi(edit, false);
}

void MainWindow::RedoDocumentEdit()
{
    if (m_historySource == HistorySource::Timeline && m_motionPanel) {
        m_motionPanel->RedoTimeline();
        RefreshUndoActions();
        return;
    }
    live2dstruct::CModelEdit edit;
    QString error;
    if (!ApplyDocumentHistory(false, edit, &error)) {
        if (!error.isEmpty()) statusBar()->showMessage(error, 8000);
        return;
    }
    ApplyDocumentEditToUi(edit, true);
}

void MainWindow::on_actionBatchPsd_triggered()
{
    if (m_psdBatch || m_batchExporting || m_closingModel || m_previewExport.IsBusy()) return;
    QString inDir = CFileOption::GetSelectDirectory(this);
    if(inDir.isEmpty()) return;

    QStringList modelFiles;
    QDirIterator it(inDir, QStringList() << "*.model3.json", QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) modelFiles << it.next();

    if(modelFiles.isEmpty()){
        QMessageBox::information(this, "提示", "未找到任何 model3.json 文件");
        return;
    }

    CPsdBatchDialog dlg(this);
    dlg.setFileList(modelFiles);
    if(dlg.exec() != QDialog::Accepted) return;
    QStringList selected = dlg.getSelectedFiles();
    if(selected.isEmpty()){
        QMessageBox::information(this, "提示", "未选择任何文件");
        return;
    }

    QString outDir = CFileOption::GetSelectDirectory(this);
    if(outDir.isEmpty()) return;

    bool isAll = dlg.isAllChecked();
    bool isFenzu = dlg.isFenzuChecked();
    bool isTexExtract = dlg.isTexExtractChecked();
    bool isCubism = dlg.isCubismChecked();

    StartPsdBatch(selected, outDir, isAll, isFenzu, isTexExtract, isCubism);
}

void MainWindow::StartPsdBatch(const QStringList& models, const QString& outputDirectory,
                              bool all, bool groups, bool textures, bool cubismCompat)
{
    if (models.isEmpty() || outputDirectory.isEmpty() || m_psdBatch || m_batchExporting ||
        m_closingModel || m_previewExport.IsBusy() || !EndKeyformEditing()) return;
    m_lastBatchOptions = {outputDirectory, all, groups, textures, cubismCompat};
    auto* job = new PsdBatchJob(this);
    m_psdBatch = job;
    auto* progress = exportProgress(this, models.size());
    m_batchProgress = progress;
    RefreshExportActions();

    connect(progress, &QProgressDialog::canceled, job, &PsdBatchJob::cancel);
    connect(job, &PsdBatchJob::cancelCurrentRequested, this, [this, job] {
        if (m_psdBatch != job) return;
        if (m_activeExportControl) m_activeExportControl->Cancel();
        statusBar()->showMessage(QStringLiteral("正在取消导出并恢复预览"));
    });
    connect(job, &PsdBatchJob::progress, this, [this, job, progress](int current, int total, const QString& name) {
        if (m_psdBatch != job) return;
        progress->setMaximum(total);
        progress->setLabelText(QStringLiteral("正在导出 %1/%2：%3").arg(current).arg(total).arg(name));
    });
    connect(job, &PsdBatchJob::fileResultChanged, this,
            [this, job, progress](int index, const PsdBatchFileResult& result) {
        if (m_psdBatch != job) return;
        m_exportResultsPanel->UpdateResult(index, result);
        if (result.state == PsdBatchFileState::Running) {
            progress->setLabelText(QStringLiteral("%1：%2\n已处理 %3/%4")
                .arg(PsdExportStageText(result.stage), QFileInfo(result.sourcePath).fileName())
                .arg(index).arg(progress->maximum()));
        } else if (result.state == PsdBatchFileState::Succeeded || result.state == PsdBatchFileState::Failed ||
                   result.state == PsdBatchFileState::Canceled) {
            progress->setValue(index + 1);
        }
    });
    connect(job, &PsdBatchJob::requestExport, this,
            [this, job](const QString& model, const QString& output, bool all, bool groups, bool textures, bool cubism) {
        if (m_psdBatch != job || m_closingModel) return;
        QString error;
        PsdExportStage stage = PsdExportStage::Preflight;
        bool committed = false;
        bool canceled = false;
        const auto refresh = qScopeGuard([this] { RefreshExportActions(); });
        const QScopedValueRollback<bool> exporting(m_batchExporting, true);
        bool ok = false;
        try {
            ok = doExportOne(model, output, all, groups, textures, cubism, &error, &stage, &committed, false, &canceled);
        } catch (const std::exception& exception) {
            error = QStringLiteral("导出发生异常：%1").arg(QString::fromUtf8(exception.what()));
        } catch (...) {
            error = QStringLiteral("导出发生未知异常");
        }
        if (m_psdBatch != job) return;
        if (m_previewExport.NeedsRecovery()) job->cancel();
        if (canceled) job->completeCanceled(output, stage, error);
        else job->completeDetailed(ok, output, error, stage, committed);
        if (m_closeAfterExport) {
            StopPsdBatch();
            QTimer::singleShot(0, this, [this] { m_closeAfterExport = false; close(); });
        }
    });
    connect(job, &PsdBatchJob::resultsReady, this,
            [this, job](const PsdBatchResults& results, bool canceled) {
        if (m_psdBatch != job) return;
        m_exportResultsPanel->SetResults(results);
        statusBar()->showMessage(canceled ? QStringLiteral("批量导出已停止，已处理与未执行结果均已保留")
                                        : QStringLiteral("批量导出完成，详见导出记录"), 8000);
    });
    connect(job, &PsdBatchJob::finished, this, [this, job, progress](const QStringList&, bool) {
        if (m_psdBatch != job) return;
        m_exportResultsPanel->SetResults(job->results());
        m_psdBatch.clear();
        m_batchProgress.clear();
        progress->close();
        progress->deleteLater();
        job->deleteLater();
        RefreshExportActions();
        if (m_closeAfterExport) {
            QTimer::singleShot(0, this, [this] { m_closeAfterExport = false; close(); });
            return;
        }
        m_exportResultsPanel->show();
        m_exportResultsPanel->raise();
    });
    if (!job->start(models, outputDirectory, all, groups, textures, cubismCompat)) {
        StopPsdBatch();
        return;
    }
    m_exportResultsPanel->SetResults(job->results());
    m_exportResultsPanel->show();
    progress->show();
}

void MainWindow::StopPsdBatch()
{
    if (m_psdBatch) {
        auto* job = m_psdBatch.data();
        m_psdBatch.clear();
        job->shutdown();
        if (m_exportResultsPanel) m_exportResultsPanel->SetResults(job->results());
        job->deleteLater();
    }
    if (m_batchProgress) {
        m_batchProgress->close();
        m_batchProgress->deleteLater();
        m_batchProgress.clear();
    }
    RefreshExportActions();
}

void MainWindow::ReportExportStage(const QString& outputPath, PsdExportStage stage)
{
    if (IsPsdExportStageTransitionValid(m_activeExportStage, stage)) m_activeExportStage = stage;
    if (m_psdBatch) m_psdBatch->setStage(outputPath, stage);
    if (m_singleExportProgress) m_singleExportProgress->setLabelText(
        QStringLiteral("%1：%2").arg(PsdExportStageText(stage), QFileInfo(outputPath).fileName()));
}

void MainWindow::RefreshExportActions()
{
    const bool exporting = m_psdBatch || m_batchExporting;
    const bool busy = exporting || m_previewExport.IsBusy();
    if (myBatchAction) myBatchAction->setEnabled(!busy && !m_closingModel);
    if (!busy && !m_closingModel && !m_runtimePanelsInitialized && m_openglwindows) {
        const QPointer<OpengGlForm> surface = m_openglwindows;
        QTimer::singleShot(0, surface, [this, surface] {
            if (surface) InitializeRuntimePanels(surface, surface->IsModelReady());
        });
    }
    if (m_exportResultsPanel) {
        m_exportResultsPanel->SetBusy(exporting);
        m_exportResultsPanel->SetRetryAvailable(!m_previewExport.IsBusy(),
            QStringLiteral("请先恢复或关闭当前预览，再重试失败项。"));
    }
    if (m_recoverPreviewAction) {
        m_recoverPreviewAction->setVisible(m_previewExport.NeedsRecovery());
        m_recoverPreviewAction->setEnabled(m_previewExport.NeedsRecovery() && !m_batchExporting);
    }
    if (m_previewExport.NeedsRecovery()) {
        if (myCloseAction) myCloseAction->setEnabled(true);
        if (m_saveSessionAction) m_saveSessionAction->setEnabled(m_workspaceReady);
    }
}

void MainWindow::RecoverExportPreview()
{
    if (m_batchExporting || !m_previewExport.NeedsRecovery()) return;
    QString error;
    bool recovered;
    {
        const QScopedValueRollback<bool> exporting(m_batchExporting, true);
        recovered = m_previewExport.Recover(&error);
    }
    if (recovered) {
        m_exportState.reset();
        statusBar()->showMessage(QStringLiteral("预览已恢复"), 5000);
    } else {
        statusBar()->showMessage(error);
    }
    RefreshExportActions();
    if (m_closeAfterExport) QTimer::singleShot(0, this, [this] { m_closeAfterExport = false; close(); });
}

bool MainWindow::doExportOne(const QString& modelPath, const QString& outPath, bool isAll, bool isFenzu,
                             bool isTexExtract, bool cubismCompat, QString* error,
                             PsdExportStage* stage, bool* outputCommitted, bool savedParameterSnapshot, bool* canceled)
{
    if (error) error->clear();
    if (stage) *stage = PsdExportStage::Preflight;
    if (outputCommitted) *outputCommitted = false;
    if (canceled) *canceled = false;
    const auto fail = [&](PsdExportStage failedStage, const QString& reason) {
        if (error) *error = reason;
        if (stage) *stage = failedStage;
        return false;
    };
    if (m_previewExport.IsBusy() || m_activeExportControl)
        return fail(PsdExportStage::Restoring, QStringLiteral("请先恢复或关闭当前预览，再继续导出"));
    if (outPath.isEmpty()) return fail(PsdExportStage::Preflight, QStringLiteral("未指定输出路径"));
    const QScopedValueRollback<bool> exporting(m_batchExporting, true);
    auto control = std::make_shared<CExportControl>();
    m_activeExportControl = control;
    m_activeExportStage = PsdExportStage::Preflight;
    const auto finish = qScopeGuard([&] {
        if (outputCommitted && control->IsCommitted()) *outputCommitted = true;
        if (canceled) *canceled = control->WasCanceled() && !control->IsCommitted() && !m_previewExport.NeedsRecovery();
        m_activeExportControl.reset();
    });
    QTimer progressTimer;
    progressTimer.setInterval(50);
    connect(&progressTimer, &QTimer::timeout, this, [this, control, outPath] {
        if (m_activeExportStage >= PsdExportStage::Restoring) return;
        const auto progress = control->Snapshot();
        ReportExportStage(outPath, exportStage(progress.phase));
        QProgressDialog* dialog = m_singleExportProgress ? m_singleExportProgress.data() : m_batchProgress.data();
        if (!dialog) return;
        QString label = QStringLiteral("%1：%2").arg(PsdExportStageText(m_activeExportStage), QFileInfo(outPath).fileName());
        if (control->IsCancellationRequested() && !control->IsCommitted()) label = QStringLiteral("正在取消导出并恢复预览");
        else if (progress.total > 0) label += QStringLiteral("\n当前步骤 %1/%2").arg(progress.current).arg(progress.total);
        if (m_batchProgress) label += QStringLiteral("\n已处理 %1/%2")
            .arg(qMax(0, m_batchProgress->value())).arg(m_batchProgress->maximum());
        if (!progress.detail.isEmpty()) label += QLatin1Char('\n') + progress.detail;
        dialog->setLabelText(label);
    });
    progressTimer.start();
    ReportExportStage(outPath, PsdExportStage::Preflight);
    const bool currentModel = m_openglwindows && m_openglwindows->GetCurrentModelJson() == modelPath;
    live2dstruct::CModelDocument preflight;
    const live2dstruct::CModelDocument* validation = &preflight;
    QString reason;
    if (currentModel && m_document.IsLoaded()) {
        if (!RuntimeMatchesDocument(&reason)) return fail(PsdExportStage::Preflight, reason);
        validation = &m_document;
    } else {
        CModelJson modelJson;
        modelJson.Parse(modelPath);
        if (!modelJson.IsOpen() || modelJson.m_moc.isEmpty())
            return fail(PsdExportStage::Preflight, QStringLiteral("无法读取 model3.json 或缺少 Moc 文件引用：%1").arg(modelPath));
        const QString moc = QDir(modelJson.m_parentPath).filePath(modelJson.m_moc);
        if (!preflight.Load(moc, &reason)) return fail(PsdExportStage::Preflight, reason);
    }
    for (const auto& diagnostic : validation->Validate())
        if (diagnostic.severity == live2dstruct::CModelDiagnostic::Error)
            return fail(PsdExportStage::Preflight, diagnostic.message);
    if (!control->Checkpoint()) return fail(PsdExportStage::Preflight, control->Error());

    ReportExportStage(outPath, PsdExportStage::Preparing);
    if (m_openglwindows) {
        auto state = std::make_shared<PreviewExportState>();
        state->surface = m_openglwindows;
        state->motion = m_motionPanel;
        state->hadMotionPanel = m_motionPanel != nullptr;
        auto* manager = LAppLive2DManager::GetInstance();
        state->originalModel = manager && manager->GetModelNum() ? manager->GetModel(0) : nullptr;
        if (!state->surface->IsModelReady() || !state->originalModel)
            return fail(PsdExportStage::Preparing, QStringLiteral("当前预览模型尚未就绪"));
        try {
            state->timelinePreview = state->originalModel->HasTimelinePreview();
            if (state->timelinePreview) state->timelineBaseline = state->originalModel->GetTimelineBaseParameters();
            state->timerActive = state->surface->m_timer.isActive();
            state->timerInterval = state->surface->m_timer.interval();
            state->scale = state->surface->ViewScale();
            state->position = state->surface->ViewPosition();
            for (auto* widget : QList<QWidget*>{m_treePanel, m_paramPanel, m_physicsPanel, m_blendShapePanel,
                    m_texturePanel, m_inspectorPanel, m_diagnosticsPanel, m_userDataPanel, m_openglwindows}) {
                if (!widget) continue;
                state->widgets.append({widget, widget->testAttribute(Qt::WA_ForceDisabled)});
                state->signalStates.append({widget, widget->signalsBlocked()});
            }
            const auto* resultsToggle = m_exportResultsPanel ? m_exportResultsPanel->toggleViewAction() : nullptr;
            for (auto* action : findChildren<QAction*>())
                if (action != m_recoverPreviewAction && action != m_exportResultsAction &&
                    action != resultsToggle && action->isEnabled()) state->actions.append(action);
            auto* delegate = LAppDelegate::GetInstance();
            state->hiddenDrawables = delegate->m_hiddenDrawables;
            state->hiddenDeformerDrawables = delegate->m_hiddenDeformerDrawables;
            state->drawableOpacity = delegate->m_drawableOpacityOverrides;
            state->drawableDrawOrder = delegate->m_drawableDrawOrderOverrides;
            state->drawableOrderGroups = delegate->m_drawableOrderGroupIndices;
            state->drawOrderGroups = delegate->m_drawOrderGroups;
            state->meshOverlayPositions = delegate->m_meshOverlayPositions;
            state->meshOverlayKeyformIndices = delegate->m_meshOverlayKeyformIndices;
            state->meshIndexOverrides = delegate->m_meshIndexOverrides;
            state->dragTargets = delegate->m_dragTargetDrawables;
        } catch (const std::exception&) {
            return fail(PsdExportStage::Preparing, QStringLiteral("无法保存当前预览状态"));
        }
        m_exportState = state;
        const auto endExportPose = [state](QString* detail) {
            if (!state->surface || !state->surface->RecoverExportResources(detail)) return false;
            if (!state->exportModel) return true;
            auto* manager = LAppLive2DManager::GetInstance();
            if (!manager || manager->GetModel(0) != state->exportModel) {
                if (detail) *detail = QStringLiteral("导出模型已改变，无法还原参数状态");
                return false;
            }
            if (state->exportModel->HasExportPreview() && !state->exportModel->EndExportPreview(detail)) return false;
            state->exportModel = nullptr;
            return true;
        };
        CPreviewExportTransaction::Steps steps;
        steps.pause = [state](QString* detail) {
            if (!state->surface) {
                if (detail) *detail = QStringLiteral("预览窗口已关闭");
                return false;
            }
            if (state->motion && !state->motion->SuspendPreviewTransport(state->transport, detail)) return false;
            if (!state->surface->SetExportSuspended(true, detail)) return false;
            state->surfacePaused = true;
            state->surface->m_timer.stop();
            for (const auto& entry : state->signalStates) if (entry.first) entry.first->blockSignals(true);
            for (const auto& entry : state->widgets) if (entry.first) entry.first->setEnabled(false);
            for (const auto& action : state->actions) if (action) action->setEnabled(false);
            return true;
        };
        if (!currentModel) {
            steps.beginTemporary = [state, modelPath, control](QString* detail) {
                if (!control->Checkpoint()) { if (detail) *detail = control->Error(); return false; }
                if (!state->surface) { if (detail) *detail = QStringLiteral("预览窗口已关闭"); return false; }
                return state->surface->BeginTemporaryModel(modelPath, detail);
            };
            steps.endTemporary = [this, state, outPath, endExportPose](QString* detail) {
                ReportExportStage(outPath, PsdExportStage::Restoring);
                if (!endExportPose(detail)) return false;
                if (!state->surface) { if (detail) *detail = QStringLiteral("预览窗口已关闭"); return false; }
                return state->surface->EndTemporaryModel(detail);
            };
        }
        steps.exportFile = [this, state, outPath, isAll, isFenzu, isTexExtract, cubismCompat,
                            savedParameterSnapshot, currentModel, control](QString* detail) {
            if (!control->Checkpoint()) { if (detail) *detail = control->Error(); return false; }
            if (!state->surface) { if (detail) *detail = QStringLiteral("预览窗口已关闭"); return false; }
            if (!currentModel && !state->surface->SetHighPrecisionMasks(m_highPrecisionMasks, detail)) return false;
            auto* manager = LAppLive2DManager::GetInstance();
            auto* model = manager && manager->GetModelNum() ? manager->GetModel(0) : nullptr;
            if (!model) { if (detail) *detail = QStringLiteral("没有可导出的模型"); return false; }
            if (!model->BeginExportPreview(savedParameterSnapshot, detail)) return false;
            state->exportModel = model;
            state->failureStage = PsdExportStage::Exporting;
            const bool exported = isTexExtract
                ? state->surface->do_TextureExtractSilent(outPath, isAll, isFenzu, cubismCompat, detail, control.get())
                : state->surface->do_OffSeceenSilent(outPath, isAll, isFenzu, cubismCompat, detail, control.get());
            state->outputCommitted = exported || control->IsCommitted();
            if (!exported) state->failureStage = exportStage(control->Snapshot().phase);
            return exported;
        };
        steps.restorePreview = [this, state, outPath, endExportPose](QString* detail) {
            ReportExportStage(outPath, PsdExportStage::Restoring);
            if (!endExportPose(detail)) return false;
            auto* manager = LAppLive2DManager::GetInstance();
            if (!state->surface || state->surface != m_openglwindows || !manager ||
                    manager->HasTemporaryScene() || manager->GetModel(0) != state->originalModel) {
                if (detail) *detail = QStringLiteral("原预览模型尚未恢复");
                return false;
            }
            auto* delegate = LAppDelegate::GetInstance();
            delegate->m_hiddenDrawables = state->hiddenDrawables;
            delegate->m_hiddenDeformerDrawables = state->hiddenDeformerDrawables;
            delegate->m_drawableOpacityOverrides = state->drawableOpacity;
            delegate->m_drawableDrawOrderOverrides = state->drawableDrawOrder;
            delegate->m_drawableOrderGroupIndices = state->drawableOrderGroups;
            delegate->m_drawOrderGroups = state->drawOrderGroups;
            delegate->m_meshOverlayPositions = state->meshOverlayPositions;
            delegate->m_meshOverlayKeyformIndices = state->meshOverlayKeyformIndices;
            delegate->m_meshIndexOverrides = state->meshIndexOverrides;
            delegate->m_dragTargetDrawables = state->dragTargets;
            state->surface->SetViewState(state->scale, state->position);
            return true;
        };
        steps.resume = [state](QString* detail) {
            if (!state->surface || (state->hadMotionPanel && !state->motion) ||
                (state->transport.IsValid() && !state->motion) ||
                (state->motion && state->motion->IsPreviewTransportSuspended() && !state->transport.IsValid())) {
                if (detail) *detail = QStringLiteral("预览暂停状态已失效");
                return false;
            }
            auto* manager = LAppLive2DManager::GetInstance();
            if (!manager || manager->GetModel(0) != state->originalModel) {
                if (detail) *detail = QStringLiteral("原预览模型已改变");
                return false;
            }
            if (!state->physicsReload.isEmpty()) {
                if (!state->originalModel->ReloadPhysicsFromFile(state->physicsReload)) {
                    if (detail) *detail = QStringLiteral("无法重新载入已保存的物理配置：%1").arg(state->physicsReload);
                    return false;
                }
                state->physicsReload.clear();
            }
            if (!state->userDataReload.isEmpty()) {
                if (!state->originalModel->ReloadUserDataFromFile(state->userDataReload)) {
                    if (detail) *detail = QStringLiteral("无法重新载入已保存的网格标签：%1").arg(state->userDataReload);
                    return false;
                }
                state->userDataReload.clear();
            }
            bool complete = false;
            const auto refreeze = qScopeGuard([&] {
                if (complete) return;
                try {
                    if (state->surface) {
                        state->surface->m_timer.stop();
                        state->surface->SetExportSuspended(true);
                    }
                    if (state->motion && !state->motion->IsPreviewTransportSuspended())
                        state->motion->SuspendPreviewTransport(state->transport);
                    for (const auto& entry : state->signalStates) if (entry.first) entry.first->blockSignals(true);
                    for (const auto& entry : state->widgets) if (entry.first) entry.first->setEnabled(false);
                    for (const auto& action : state->actions) if (action) action->setEnabled(false);
                } catch (...) {
                    // 异常清理不能再次抛出。
                }
            });
            if (state->surfacePaused && !state->surface->SetExportSuspended(false, detail)) return false;
            for (const auto& entry : state->widgets) if (entry.first) entry.first->setEnabled(!entry.second);
            for (const auto& action : state->actions) if (action) action->setEnabled(true);
            if (state->transport.IsValid() && !state->motion->ResumePreviewTransport(state->transport, detail)) return false;
            if (state->timelineSaved && state->motion) {
                state->motion->MarkTimelineSaved();
                state->timelineSaved = false;
            }
            for (const auto& entry : state->signalStates) if (entry.first) entry.first->blockSignals(entry.second);
            if (state->surfacePaused && state->timerActive) state->surface->m_timer.start(state->timerInterval);
            state->surfacePaused = false;
            complete = true;
            return true;
        };
        const bool ok = m_previewExport.Run(std::move(steps), &reason);
        if (outputCommitted) *outputCommitted = state->outputCommitted;
        if (stage) *stage = ok ? PsdExportStage::Done :
            (m_previewExport.NeedsRecovery() ? PsdExportStage::Restoring : state->failureStage);
        if (error) *error = reason;
        if (!m_previewExport.NeedsRecovery()) m_exportState.reset();
        RefreshExportActions();
        return ok;
    }

    if (stage) *stage = PsdExportStage::Preparing;
    if (!m_headless) {
        m_headless = new OpengGlForm();
        m_headless->m_timer.stop();
        if (!m_headless->SetExportSuspended(true, &reason)) {
            delete m_headless;
            return fail(PsdExportStage::Preparing, reason);
        }
        m_headless->setAttribute(Qt::WA_DontShowOnScreen);
        m_headless->resize(1, 1);
        m_headless->SetInitOpenName(modelPath);
        const QPointer<OpengGlForm> surface = m_headless;
        QEventLoop initialized;
        QTimer timeout;
        timeout.setSingleShot(true);
        connect(surface, &OpengGlForm::RuntimeInitialized, &initialized, &QEventLoop::quit);
        connect(surface, &QObject::destroyed, &initialized, &QEventLoop::quit);
        connect(&timeout, &QTimer::timeout, &initialized, &QEventLoop::quit);
        connect(&progressTimer, &QTimer::timeout, &initialized, [&] {
            if (control->IsCancellationRequested()) initialized.quit();
        });
        m_headless->show();
        if (surface && !surface->HasInitializationResult()) {
            timeout.start(10000);
            initialized.exec();
        }
        if (!control->Checkpoint() || m_closeAfterExport || !surface || m_headless != surface || !surface->IsModelReady()) {
            if (surface && m_headless == surface) delete m_headless;
            if (control->WasCanceled()) return fail(PsdExportStage::Preparing, control->Error());
            return fail(PsdExportStage::Preparing, QStringLiteral("离屏模型初始化未完成、失败或已取消"));
        }
        m_headless->m_timer.stop();
    } else {
        try {
            if (!m_headless->LoadExportModel(modelPath, &reason)) return fail(PsdExportStage::Preparing, reason);
        } catch (const std::exception& exception) {
            return fail(PsdExportStage::Preparing, QStringLiteral("离屏模型载入异常：%1").arg(QString::fromUtf8(exception.what())));
        } catch (...) {
            return fail(PsdExportStage::Preparing, QStringLiteral("离屏模型载入发生未知异常"));
        }
    }
    auto state = std::make_shared<PreviewExportState>();
    state->surface = m_headless;
    m_exportState = state;
    CPreviewExportTransaction::Steps steps;
    steps.pause = [state](QString* detail) {
        if (!state->surface) { if (detail) *detail = QStringLiteral("离屏导出窗口已关闭"); return false; }
        state->surface->m_timer.stop();
        return state->surface->SetExportSuspended(true, detail);
    };
    steps.exportFile = [this, state, outPath, isAll, isFenzu, isTexExtract, cubismCompat, control](QString* detail) {
        if (!control->Checkpoint()) { if (detail) *detail = control->Error(); return false; }
        if (!state->surface || !state->surface->SetHighPrecisionMasks(m_highPrecisionMasks, detail)) return false;
        auto* manager = LAppLive2DManager::GetInstance();
        auto* model = manager && manager->GetModelNum() ? manager->GetModel(0) : nullptr;
        if (!model) { if (detail) *detail = QStringLiteral("没有可导出的模型"); return false; }
        if (!model->BeginExportPreview(false, detail)) return false;
        state->exportModel = model;
        state->failureStage = PsdExportStage::Exporting;
        const bool exported = isTexExtract
            ? state->surface->do_TextureExtractSilent(outPath, isAll, isFenzu, cubismCompat, detail, control.get())
            : state->surface->do_OffSeceenSilent(outPath, isAll, isFenzu, cubismCompat, detail, control.get());
        state->outputCommitted = exported || control->IsCommitted();
        if (!exported) state->failureStage = exportStage(control->Snapshot().phase);
        return exported;
    };
    steps.restorePreview = [this, state, outPath](QString* detail) {
        ReportExportStage(outPath, PsdExportStage::Restoring);
        if (!state->surface || state->surface != m_headless) {
            if (detail) *detail = QStringLiteral("离屏导出窗口已改变");
            return false;
        }
        if (!state->surface->RecoverExportResources(detail)) return false;
        if (!state->exportModel) return true;
        auto* manager = LAppLive2DManager::GetInstance();
        if (!manager || manager->GetModel(0) != state->exportModel) {
            if (detail) *detail = QStringLiteral("离屏导出模型已改变");
            return false;
        }
        if (!state->exportModel->EndExportPreview(detail)) return false;
        state->exportModel = nullptr;
        return true;
    };
    steps.resume = [](QString*) { return true; };
    const bool ok = m_previewExport.Run(std::move(steps), &reason);
    if (outputCommitted) *outputCommitted = state->outputCommitted;
    if (stage) *stage = ok ? PsdExportStage::Done :
        (m_previewExport.NeedsRecovery() ? PsdExportStage::Restoring : state->failureStage);
    if (error) *error = reason;
    if (!m_previewExport.NeedsRecovery()) m_exportState.reset();
    RefreshExportActions();
    return ok;
}


void MainWindow::on_actionCloseModel_triggered()
{
    CloseModel();
}

bool MainWindow::ConfirmCloseWithUnsavedChanges(bool replacingSession)
{
    const bool physicsDirty = !replacingSession && m_physicsPanel && m_physicsPanel->HasUnsavedChanges();
    const bool poseDirty = !replacingSession && (m_poseDirty || (m_motionPanel && m_motionPanel->HasUnsavedPoseChanges()));
    const bool userDataDirty = !replacingSession && m_userDataPanel && m_userDataPanel->HasUnsavedChanges();
    const bool animationDirty = m_motionPanel && m_motionPanel->HasUnsavedTimelineChanges() &&
        !(m_exportState && m_exportState->timelineSaved);
    if (!m_sessionDirty && !physicsDirty && !poseDirty && !userDataDirty && !animationDirty) return true;
    QMessageBox prompt(this);
    prompt.setWindowTitle(QStringLiteral("未保存的编辑"));
    prompt.setText(QStringLiteral("当前模型有未保存的编辑。"));
    QStringList pending;
    if (m_sessionDirty) pending.append(QStringLiteral("工作会话"));
    if (animationDirty) pending.append(QStringLiteral("动画草稿"));
    if (physicsDirty) pending.append(QStringLiteral("物理配置"));
    if (poseDirty) pending.append(QStringLiteral("Pose 配置"));
    if (userDataDirty) pending.append(QStringLiteral("网格标签"));
    prompt.setInformativeText((replacingSession ? QStringLiteral("载入会话前是否保存：%1？")
                                                : QStringLiteral("关闭前是否保存：%1？"))
                                  .arg(pending.join(QStringLiteral("、"))));
    QPushButton* save = prompt.addButton(QStringLiteral("保存"), QMessageBox::AcceptRole);
    prompt.addButton(QStringLiteral("放弃"), QMessageBox::DestructiveRole);
    prompt.addButton(QStringLiteral("取消"), QMessageBox::RejectRole);
    prompt.exec();
    if (prompt.clickedButton() == save) {
        const bool sessionSaved = !(m_sessionDirty || animationDirty) || SaveSession();
        const bool physicsSaved = !physicsDirty || (m_physicsPanel && m_physicsPanel->SavePhysics());
        if (physicsDirty && physicsSaved && m_exportState)
            m_exportState->physicsReload = m_physicsPanel->FilePath();
        QString poseError;
        const bool poseSaved = !poseDirty || (m_motionPanel && m_motionPanel->SavePose(&poseError));
        if (poseSaved) m_poseDirty = false;
        if (!poseSaved && !poseError.isEmpty())
            QMessageBox::warning(this, QStringLiteral("保存 Pose 失败"), poseError);
        QString userDataError;
        const bool userDataSaved = !userDataDirty || m_userDataPanel->SaveUserData(&userDataError);
        if (userDataDirty && userDataSaved && m_exportState)
            m_exportState->userDataReload = m_userDataPanel->FilePath();
        if (!userDataSaved)
            QMessageBox::warning(this, QStringLiteral("保存 UserData 失败"), userDataError);
        return sessionSaved && physicsSaved && poseSaved && userDataSaved;
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
            recentButton->setToolTip(QStringLiteral("<qt>") + path.toHtmlEscaped() + QStringLiteral("</qt>"));
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
    if (m_batchExporting || m_closingModel) return false;
    if (m_psdBatch) StopPsdBatch();
    if (!m_openglwindows) {
        StopPsdBatch();
        if (m_headless) delete m_headless;
        m_previewExport.ResetAfterPreviewClosed();
        m_exportState.reset();
        RefreshExportActions();
        return true;
    }
    QString dragError;
    if (!m_previewExport.NeedsRecovery() && !m_openglwindows->FinishMeshDrag(false, &dragError)) {
        statusBar()->showMessage(dragError, 8000);
        return false;
    }
    const auto refreshAfterClose = qScopeGuard([this] {
        if (m_openglwindows) RefreshUndoActions();
        RefreshExportActions();
    });
    const QScopedValueRollback<bool> closing(m_closingModel, true);
    if (!ConfirmCloseWithUnsavedChanges()) return false;
    if (m_exportState) for (const auto& action : m_exportState->actions) if (action) action->setEnabled(true);
    m_workspaceReady = false;
    m_runtimePanelsInitialized = false;
    m_saveSessionAction->setEnabled(false);
    m_loadSessionAction->setEnabled(false);
    StopPsdBatch();
    if (m_motionPanel || m_blendShapePanel) {
        QSettings settings(QStringLiteral("live2d2psd"), QStringLiteral("live2d2psd"));
        if (m_motionPanel) settings.setValue(QStringLiteral("motionPanelLayout"), m_motionPanel->SaveLayout());
        if (m_blendShapePanel) settings.setValue(QStringLiteral("blendShapePanelLayout"), m_blendShapePanel->SaveLayout());
    }
    m_historySource = HistorySource::None;

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
    if (m_userDataPanel) { removeDockWidget(m_userDataPanel); delete m_userDataPanel; m_userDataPanel = nullptr; }
    if (m_undoAction) m_undoAction->setEnabled(false);
    if (m_redoAction) m_redoAction->setEnabled(false);
    m_sessionDirty = false;
    m_poseDirty = false;
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
    m_btnToggleUserData->setVisible(false);

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
    m_previewExport.ResetAfterPreviewClosed();
    m_exportState.reset();
    RefreshExportActions();
    if (m_canvasHost) {
        delete m_canvasHost;
        m_canvasHost = nullptr;
    }

    m_keyformEditor.SetDocument(nullptr);
    m_keyformBaseValues.clear();
    m_document.Clear();
    m_placeholder = createEmptyState();
    setCentralWidget(m_placeholder);

    statusBar()->showMessage("模型已关闭");
    return true;
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    if (m_batchExporting || m_psdBatch) {
        m_closeAfterExport = true;
        if (m_activeExportControl) m_activeExportControl->Cancel();
        if (m_psdBatch) m_psdBatch->cancel();
        event->ignore();
        return;
    }
    if (CloseModel()) event->accept();
    else event->ignore();
}

void MainWindow::InitializeRuntimePanels(OpengGlForm* expectedWindow, bool loaded)
{
    if (!loaded || m_runtimePanelsInitialized || m_openglwindows != expectedWindow || m_psdBatch || m_batchExporting || m_previewExport.IsBusy()) return;
    QString sourceError;
    if (!RuntimeMatchesDocument(&sourceError)) {
        statusBar()->showMessage(sourceError);
        return;
    }
    if (m_keyformEditor.IsActive() && (!m_document.ParameterStructure().columns.isEmpty() ||
                                      !m_document.ParameterStructure().counts.isEmpty()) && !EndKeyformEditing(&sourceError)) {
        statusBar()->showMessage(QStringLiteral("编辑预览同步失败：%1").arg(sourceError), 8000);
        return;
    }
    if (!PublishKeyformPreview(m_document, m_keyformEditor.Pose(), m_keyformEditor.IsActive(), &sourceError)) {
        statusBar()->showMessage(QStringLiteral("编辑预览同步失败：%1").arg(sourceError), 8000);
        return;
    }
    SyncDocumentToRuntime();
    RefreshKeyformUi();
    if (m_texturePanel && m_document.SourceMatchesDisk(&sourceError)) {
        auto* textures = LAppDelegate::GetInstance()->GetTextureManager();
        if (textures) m_texturePanel->LoadTextures(textures->GetModelTex(), m_currentMoc);
    }
    applyBgColor();
    m_runtimePanelsInitialized = true;
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
    m_keyformEditor.SetDocument(&m_document);
    m_keyformBaseValues.clear();
    m_keyformEditor.SetPublisher(
        [this](const auto& document, const auto& pose, bool editing, QString* error) {
            return PublishKeyformPreview(document, pose, editing, error);
        }, [this](QString* error) {
            return !m_openglwindows || m_openglwindows->EndEditorPreview(error);
        });
    m_keyformEditor.SetDefinitionPublisher([this](const auto& document, const auto& values, bool editing, QString* error) {
        return PublishParameterDefinitions(document, values, editing, error);
    });
    m_keyformEditor.SetStructurePublisher([this](const auto& document, const auto& command, bool forward, bool restoring,
                                               const auto& values, bool editing, auto& pose, QString* error) {
        return PublishParameterStructure(document, command, forward, restoring, values, editing, pose, error);
    }, [this](const auto& document, const auto& command, bool forward, auto& pose, QString* error) {
        return PrepareParameterStructurePose(document, command, forward, pose, error);
    });
    m_selectedObjectIds.clear();
    m_selectedObjectTypes.clear();
    m_workspaceReady = false;
    m_runtimePanelsInitialized = false;
    m_saveSessionAction->setEnabled(false);
    m_loadSessionAction->setEnabled(false);
    if (m_headless) {
        m_headless->m_timer.stop();
        m_headless->makeCurrent();
        delete m_headless;
        m_headless = nullptr;
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
    CTreeManage::Snapshot tree;
    QMap<QString, float> initialPose;
    for (const auto& parameter : m_document.Parameters()) initialPose.insert(parameter.id, m_document.ParameterValue(parameter.id));
    if (CTreeManage::PrepareSnapshot(m_document.SourceBytes(), initialPose, tree, &documentError))
        CTreeManage::SwapSnapshot(tree);
    else statusBar()->showMessage(documentError, 8000);

    m_openglwindows = new OpengGlForm();
    m_openglwindows->SetInitOpenName(name);
    QHash<QString, int> groupIndices;
    QVector<live2dstruct::CDrawableOrderGroup> groups;
    groups.reserve(m_document.DrawOrderGroups().size());
    for (int groupIndex = 0; groupIndex < m_document.DrawOrderGroups().size(); ++groupIndex) {
        const auto& sourceGroup = m_document.DrawOrderGroups()[groupIndex];
        groups.append({sourceGroup.minimumDrawOrder, sourceGroup.maximumDrawOrder});
        if (!sourceGroup.objectsValid) continue;
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
    canvasTitle->setTextFormat(Qt::PlainText);
    canvasTitle->setToolTip(QStringLiteral("<qt>%1</qt>").arg(QFileInfo(name).fileName().toHtmlEscaped()));
    canvasTitle->setMinimumWidth(0);
    canvasTitle->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    auto* canvasMeta = new QLabel("预览", canvasToolbar);
    canvasMeta->setObjectName("canvasMeta");
    auto* canvasSpacer = new QWidget(canvasToolbar);
    canvasSpacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    auto* maskQuality = new QCheckBox(QStringLiteral("高精度蒙版"), canvasToolbar);
    maskQuality->setObjectName(QStringLiteral("highPrecisionMasks"));
    maskQuality->setToolTip(QStringLiteral("优先保持蒙版边缘精度，可能降低帧率"));
    maskQuality->setChecked(m_highPrecisionMasks);
    maskQuality->setEnabled(false);
    connect(maskQuality, &QCheckBox::toggled, this, [this, maskQuality](bool enabled) {
        QString error;
        if (!m_openglwindows || !m_openglwindows->SetHighPrecisionMasks(enabled, &error)) {
            const QSignalBlocker blocker(maskQuality);
            maskQuality->setChecked(m_openglwindows && m_openglwindows->HighPrecisionMasks());
            if (!error.isEmpty()) statusBar()->showMessage(error, 8000);
            return;
        }
        m_highPrecisionMasks = enabled;
        QSettings settings(QStringLiteral("live2d2psd"), QStringLiteral("live2d2psd"));
        settings.setValue(QStringLiteral("highPrecisionMasks"), enabled);
    });
    auto* resetCanvas = new QPushButton("重置视图", canvasToolbar);
    resetCanvas->setEnabled(false);
    resetCanvas->setToolTip("恢复模型缩放和位置（Home）");
    connect(resetCanvas, &QPushButton::clicked, this, [this](){
        if (m_openglwindows) m_openglwindows->do_ResetView();
    });
    connect(m_openglwindows, &OpengGlForm::RuntimeInitialized, canvasMeta,
            [this, canvasMeta, resetCanvas, maskQuality, expectedWindow = m_openglwindows](bool loaded) {
        if (m_openglwindows != expectedWindow) return;
        canvasMeta->setText(loaded ? QStringLiteral("预览") : QStringLiteral("预览不可用"));
        resetCanvas->setEnabled(loaded);
        maskQuality->setEnabled(loaded);
        if (loaded) {
            QString error;
            if (!m_openglwindows->SetHighPrecisionMasks(m_highPrecisionMasks, &error)) {
                const QSignalBlocker blocker(maskQuality);
                maskQuality->setChecked(m_openglwindows->HighPrecisionMasks());
                statusBar()->showMessage(error, 8000);
            }
        }
        if (!loaded) {
            canvasMeta->setToolTip(QStringLiteral("请检查模型、纹理文件与图形环境"));
            statusBar()->showMessage(QStringLiteral("模型数据已打开，但运行时预览加载失败"));
        }
    }, Qt::QueuedConnection);
    canvasToolbarLayout->addWidget(canvasTitle, 1);
    canvasToolbarLayout->addWidget(canvasMeta);
    canvasToolbarLayout->addWidget(canvasSpacer);
    m_finishKeyformButton = new QPushButton(QStringLiteral("结束关键形编辑"), canvasToolbar);
    m_finishKeyformButton->setToolTip(QStringLiteral("恢复编辑前的参数与自动动作（Esc）"));
    m_finishKeyformButton->hide();
    connect(m_finishKeyformButton, &QPushButton::clicked, this, [this] { EndKeyformEditing(); });
    canvasToolbarLayout->addWidget(m_finishKeyformButton);
    canvasToolbarLayout->addWidget(maskQuality);
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
    m_treePanel->BuildTreeBytes(m_document.SourceBytes(), m_cidJson);

    // 右侧面板：参数控制 + 物理设置（tabify）
    m_paramPanel = new CParamPanel(this);
    m_paramPanel->SetDocument(&m_document);
    addDockWidget(Qt::RightDockWidgetArea, m_paramPanel);
    connect(m_paramPanel, &CParamPanel::HistoryChanged, this, [this](bool canUndo, bool){
        if (m_historySource != HistorySource::Timeline) m_historySource = HistorySource::Parameters;
        RefreshUndoActions();
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
    QStringList parameterIds;
    for (const auto& parameter : m_document.Parameters()) parameterIds.append(parameter.id);
    m_motionPanel->SetParameterIds(parameterIds);
    addDockWidget(Qt::BottomDockWidgetArea, m_motionPanel);
    QStringList partIds;
    for (const auto& part : m_document.Parts()) partIds.append(part.id);
    QString catalogError;
    if (!m_motionPanel->BuildLists(name, partIds, &catalogError)) statusBar()->showMessage(catalogError, 8000);
    connect(m_motionPanel, &CMotionPanel::SaveSessionRequested, this, &MainWindow::on_actionSaveSession_triggered);
    connect(m_motionPanel, &CMotionPanel::TimelineHistoryChanged, this, [this](bool, bool){
        if (m_closingModel) return;
        RefreshUndoActions();
    });
    connect(m_motionPanel, &CMotionPanel::TimelineActivated, this, [this](){
        if (m_closingModel) return;
        m_historySource = HistorySource::Timeline;
        RefreshUndoActions();
    });
    connect(m_motionPanel, &CMotionPanel::TimelineDirtyChanged, this, [this](bool dirty){
        if (dirty) {
            setWindowTitle(QStringLiteral("live2d2psd · 未保存"));
            statusBar()->showMessage(QStringLiteral("动画草稿有未保存的修改"));
        } else if (!m_sessionDirty && !m_poseDirty &&
                   (!m_physicsPanel || !m_physicsPanel->HasUnsavedChanges()) &&
                   (!m_userDataPanel || !m_userDataPanel->HasUnsavedChanges())) {
            setWindowTitle(QStringLiteral("live2d2psd"));
        }
    });
    connect(m_motionPanel, &CMotionPanel::ExpressionSaved, this, [this](const QString& expression, const QString& path){
        LAppLive2DManager* manager = LAppLive2DManager::GetInstance();
        LAppModel* model = manager && manager->GetModelNum() > 0 ? manager->GetModel(0) : nullptr;
        if (!model || !model->ReloadExpressionFromFile(expression, path))
            QMessageBox::warning(this, QStringLiteral("表情已保存但运行时刷新失败"),
                                 QStringLiteral("下次重新打开模型后会读取新配置"));
    });

    m_blendShapePanel = new CBlendShapePanel(this);
    m_blendShapePanel->SetDocument(&m_document);
    addDockWidget(Qt::BottomDockWidgetArea, m_blendShapePanel);
    tabifyDockWidget(m_motionPanel, m_blendShapePanel);
    connect(m_paramPanel, &CParamPanel::ParameterMetadataChanged, m_blendShapePanel, &CBlendShapePanel::RefreshParameterMetadata);
    connect(m_blendShapePanel, &CBlendShapePanel::ParameterSelected, this, [this](const QString& id) {
        if (!m_paramPanel) return;
        m_paramPanel->show();
        m_paramPanel->raise();
        if (!m_paramPanel->FocusParameter(id)) statusBar()->showMessage(QStringLiteral("当前模型中没有该参数"), 5000);
    });
    connect(m_blendShapePanel, &CBlendShapePanel::ObjectSelected, m_treePanel, &CModelTreePanel::HighlightObject);

    m_texturePanel = new CTexturePanel(this);
    addDockWidget(Qt::BottomDockWidgetArea, m_texturePanel);
    tabifyDockWidget(m_blendShapePanel, m_texturePanel);

    m_inspectorPanel = new CInspectorPanel(this);
    addDockWidget(Qt::RightDockWidgetArea, m_inspectorPanel);
    tabifyDockWidget(m_paramPanel, m_inspectorPanel);
    m_inspectorPanel->SetDocument(&m_document);
    m_inspectorPanel->SetLockedState(m_document.LockedKeys());
    m_inspectorPanel->SetEditHandler([this](const auto& edit, QString* error) {
        return SubmitInspectorEdit(edit, error);
    });
    m_inspectorPanel->SetHistoryHandler([this](bool undo, auto& edit, QString* error) {
        if (!ApplyDocumentHistory(undo, edit, error)) return false;
        if (edit.field == live2dstruct::CModelEdit::ParameterValue ||
            edit.field == live2dstruct::CModelEdit::ParameterValues ||
            edit.field == live2dstruct::CModelEdit::ParameterMetadata ||
            edit.field == live2dstruct::CModelEdit::ParameterStructure)
            ApplyDocumentEditToUi(edit, !undo);
        return true;
    });
    m_inspectorPanel->SetKeyformHandler([this](const QString& id, int keyform, QString* error) {
        return ActivateKeyform(id, keyform, error);
    });
    m_inspectorPanel->SetDeformerKeyformHandler([this](const QString& id, const QString& type, int keyform, QString* error) {
        return ActivateKeyform(id, type, keyform, error);
    });
    connect(m_inspectorPanel, &CInspectorPanel::EditRejected, this,
            [this](const QString& error) { statusBar()->showMessage(error, 8000); });

    connect(m_inspectorPanel, &CInspectorPanel::HistoryChanged, this, [this](bool canUndo, bool){
        if (m_openglwindows && RuntimeMatchesDocument())
            m_openglwindows->SetHiddenDeformerDrawables(m_document.HiddenDeformerDrawables());
        if (m_historySource != HistorySource::Timeline) m_historySource = HistorySource::Inspector;
        RefreshUndoActions();
        if (canUndo && !m_applyingSession) {
            m_sessionDirty = true;
            setWindowTitle(QStringLiteral("live2d2psd · 未保存"));
            statusBar()->showMessage(QStringLiteral("有未保存的属性修改"));
        }
    });

    m_diagnosticsPanel = new CModelDiagnosticsPanel(this);
    addDockWidget(Qt::BottomDockWidgetArea, m_diagnosticsPanel);
    tabifyDockWidget(m_texturePanel, m_diagnosticsPanel);
    m_diagnosticsPanel->SetDocument(&m_document);
    m_diagnosticsPanel->hide();

    m_userDataPanel = new CUserDataPanel(this);
    addDockWidget(Qt::BottomDockWidgetArea, m_userDataPanel);
    tabifyDockWidget(m_texturePanel, m_userDataPanel);
    QSet<QString> drawableIds;
    for (const auto& drawable : m_document.Drawables()) drawableIds.insert(drawable.id);
    m_userDataPanel->SetValidArtMeshIds(drawableIds);
    QString userDataError;
    if (!m_userDataPanel->LoadModel(name, &userDataError))
        statusBar()->showMessage(QStringLiteral("UserData 加载失败：%1").arg(userDataError), 8000);
    m_userDataPanel->hide();
    connect(m_userDataPanel, &CUserDataPanel::DirtyChanged, this, [this](bool dirty){
        if (dirty) statusBar()->showMessage(QStringLiteral("网格标签有未保存的修改"));
    });
    connect(m_userDataPanel, &CUserDataPanel::UserDataSaved, this, [this](const QString& path){
        LAppLive2DManager* manager = LAppLive2DManager::GetInstance();
        LAppModel* model = manager && manager->GetModelNum() > 0 ? manager->GetModel(0) : nullptr;
        if (model && model->ReloadUserDataFromFile(path)) {
            statusBar()->showMessage(QStringLiteral("网格标签已保存并更新：%1").arg(path), 5000);
        } else {
            QMessageBox::warning(this, QStringLiteral("UserData 已保存"),
                                 QStringLiteral("文件保存成功，但运行时刷新失败。请重新打开模型。"));
        }
    });
    connect(m_userDataPanel, &CUserDataPanel::ArtMeshSelected, m_treePanel, &CModelTreePanel::HighlightMesh);

    m_motionPanel->raise();

    // 加载物理设置（立即，不需要 GL）
    if(m_physicsPanel && !modeljson.m_Physics.isEmpty()){
        QString physicsError;
        if (!m_physicsPanel->LoadPhysics(modeljson.m_parentPath + "/" + modeljson.m_Physics, &physicsError))
            statusBar()->showMessage(physicsError, 8000);
    }

    // 加载融合变形信息（立即，不需要 GL）
    if(m_blendShapePanel){
        QString relationError;
        if (!m_blendShapePanel->BuildBlendShapesBytes(m_document.SourceBytes(), m_cidJson, &relationError))
            statusBar()->showMessage(relationError, 8000);
    }

    const QSet<QString>* bsParams = m_blendShapePanel ? &m_blendShapePanel->getBlendShapeParamIds() : nullptr;
    m_paramPanel->BuildParams(m_cidJson, bsParams);
    if (m_blendShapePanel) {
        const auto values = m_paramPanel->CurrentValues();
        for (auto it = values.cbegin(); it != values.cend(); ++it)
            m_blendShapePanel->SyncParameterValue(it.key(), it.value());
    }
    const QString posePath = modeljson.m_Pose.isEmpty() ? QString() : (modeljson.m_parentPath + "/" + modeljson.m_Pose);
    if (!posePath.isEmpty()) {
        QString poseError;
        if (!m_motionPanel->BuildPoseGroups(posePath, &poseError))
            statusBar()->showMessage(poseError, 8000);
    }
    const auto initializeRuntimePanels = [this, expectedWindow = m_openglwindows](bool loaded) {
        InitializeRuntimePanels(expectedWindow, loaded);
    };
    connect(m_openglwindows, &OpengGlForm::RuntimeInitialized, m_paramPanel,
            initializeRuntimePanels, Qt::QueuedConnection);
    if (m_openglwindows->HasInitializationResult()) {
        const bool loaded = m_openglwindows->IsModelReady();
        QTimer::singleShot(0, m_paramPanel, [initializeRuntimePanels, loaded] { initializeRuntimePanels(loaded); });
    }

    // 参数面板实时同步定时器（200ms 间隔，低频避免性能问题）
    QTimer* syncTimer = new QTimer(m_paramPanel);
    connect(syncTimer, &QTimer::timeout, m_paramPanel, [this] {
        if (m_runtimePanelsInitialized && RuntimeMatchesDocument()) m_paramPanel->SyncFromModel();
    });
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
    m_btnToggleUserData->setVisible(true);

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
    wireToggle(m_btnToggleUserData, m_userDataPanel);

    m_motionPanel->RestoreLayout(settings.value(QStringLiteral("motionPanelLayout")).toByteArray());
    m_blendShapePanel->RestoreLayout(settings.value(QStringLiteral("blendShapePanelLayout")).toByteArray());
    if (!m_savedWindowState.isEmpty()) {
        restoreState(m_savedWindowState);
        m_savedWindowState.clear();
    }
    SetWorkspaceMode(m_workspaceMode);

    // 信号连接：树面板 → OpenGL
    connect(m_treePanel, &CModelTreePanel::ObjectSelected, this,
            [this](const QString& id, const QString& type) { SelectModelObjects({id}, {type}); });
    connect(m_treePanel, &CModelTreePanel::ObjectsSelected, this, &MainWindow::SelectModelObjects);
    connect(m_treePanel, &CModelTreePanel::WarpSelected, m_openglwindows, &OpengGlForm::do_ShowWarp);
    connect(m_treePanel, &CModelTreePanel::ShowDeformers, this, [this](const QStringList& ids) {
        if (!m_keyformEditor.IsActive()) m_openglwindows->do_ShowDeformers(ids);
    });
    connect(m_treePanel, &CModelTreePanel::ShowDeformerMeshes, this, [this](const QStringList& ids) {
        if (!m_keyformEditor.IsActive()) m_openglwindows->do_ShowDeformerMeshes(ids);
    });
    connect(m_treePanel, &CModelTreePanel::DeformerDragParamsChanged, m_openglwindows, &OpengGlForm::do_SetDeformerDragParams);

    m_paramPanel->SetPreEditGuard([this](QString* error) {
        if (m_previewExport.IsBusy()) {
            if (error) *error = QStringLiteral("导出正在占用预览，请稍后编辑");
            return false;
        }
        return EndKeyformEditing(error);
    });
    m_paramPanel->SetMetadataEditHandler([this](const auto& edit, QString* error) {
        if (!SubmitInspectorEdit(edit, error)) return false;
        m_historySource = HistorySource::Parameters;
        RefreshUndoActions();
        return true;
    });
    m_paramPanel->SetStructureEditHandler([this](const auto& edit, QString* error) {
        if (!SubmitInspectorEdit(edit, error)) return false;
        m_historySource = HistorySource::Parameters;
        RefreshUndoActions();
        return true;
    });
    m_paramPanel->SetParameterReferenceProvider([this](const QString& id) { return ParameterReferences(id); });
    connect(m_paramPanel, &CParamPanel::StructureChanged, this, &MainWindow::RefreshParameterStructureUi);
    m_paramPanel->SetHistoryHandler([this](bool undo, auto& edit, QString* error) {
        return ApplyDocumentHistory(undo, edit, error);
    });
    connect(m_paramPanel, &CParamPanel::ParameterMetadataChanged, this, &MainWindow::SyncParameterDefinitionPreview);
    connect(m_paramPanel, &CParamPanel::EditRejected, this,
            [this](const QString& error) { statusBar()->showMessage(error, 8000); });

    // 信号连接：参数面板 → OpenGL
    connect(m_paramPanel, &CParamPanel::ParamChanged, this, [this](const QString& name, float value, bool isInit){
        if (m_previewExport.IsBusy()) return;
        const QScopedValueRollback<QString> editing(m_parameterBeingEdited, name);
        if (!isInit && m_motionPanel) m_motionPanel->StopTimelinePreview();
        if (m_openglwindows) m_openglwindows->do_change_para_value(name, value, isInit);
    });
    connect(m_paramPanel, &CParamPanel::ParamChanged, m_blendShapePanel,
            [this](const QString& name, float value, bool){ m_blendShapePanel->SyncParameterValue(name, value); });
    connect(m_paramPanel, &CParamPanel::ParamChanged, m_motionPanel,
            [this](const QString& name, float value, bool isInit){
                if (!isInit) m_motionPanel->RecordParameterValue(name, value);
            });
    connect(m_motionPanel, &CMotionPanel::TimelineFrameChanged, this, [this](const CMotionPreviewFrame& frame){
        if (m_applyingSession || m_previewExport.IsBusy()) return;
        if (!EndKeyformEditing()) { m_motionPanel->PauseTimelinePreview(); return; }
        if (frame.IsEmpty()) { ClearTimelinePreview(); return; }
        if (!m_openglwindows || !m_openglwindows->isValid()) return;
        auto* manager = LAppLive2DManager::GetInstance();
        auto* model = manager && manager->GetModelNum() > 0 ? manager->GetModel(0) : nullptr;
        if (!model || !m_motionPanel || !m_paramPanel) return;
        const auto* fades = m_motionPanel->UsesMotionFades() ? &m_motionPanel->Timeline() : nullptr;
        if (!model->SetTimelinePreview(frame, fades, m_motionPanel->TimelineTime(), m_motionPanel->TimelineLoop())) return;
        const auto values = model->GetParameterValues();
        m_openglwindows->SyncPreviewView();
        m_paramPanel->SetPreviewValues(values, model->GetTimelineBaseParameters());
        if (m_blendShapePanel) for (auto it = values.cbegin(); it != values.cend(); ++it)
            m_blendShapePanel->SyncParameterValue(it.key(), it.value());
    });
    connect(m_motionPanel, &CMotionPanel::TimelinePreviewStopped, this, &MainWindow::ClearTimelinePreview);
    connect(m_blendShapePanel, &CBlendShapePanel::ParamChanged, this, [this](const QString& name, float value, bool){
        if (!m_paramPanel || m_previewExport.IsBusy()) return;
        m_paramPanel->ApplyExternalValue(name, value, true);
        float accepted;
        if (m_blendShapePanel && m_paramPanel->CurrentValue(name, accepted)) m_blendShapePanel->SyncParameterValue(name, accepted);
    });

    // 信号连接：OpenGL → 树面板高亮
    connect(m_openglwindows, &OpengGlForm::SendSelectMesh, m_treePanel, &CModelTreePanel::HighlightMesh);

    // 信号连接：纹理面板点击图集 → 树面板 + 主视图 + 面板自身高亮同步
    connect(m_texturePanel, &CTexturePanel::AtlasMeshPicked, m_treePanel, &CModelTreePanel::HighlightMesh);
    // 反向：主视图点击 mesh → 纹理面板高亮
    connect(m_openglwindows, &OpengGlForm::RenderOrderChanged, m_texturePanel, &CTexturePanel::SetRenderOrder);

    connect(m_treePanel, &CModelTreePanel::VisibilityBatchChanged, this, [this](const QMap<QString, bool>& values) {
        if (m_syncingVisibility) return;
        const auto current = m_document.VisibilityState();
        QMap<QString, bool> before, after;
        QString error;
        for (auto it = values.cbegin(); it != values.cend(); ++it) {
            const auto existing = current.constFind(it.key());
            if (existing == current.cend()) {
                error = QStringLiteral("可见性对象已改变，请重新选择");
                break;
            }
            if (existing.value() == it.value()) continue;
            before.insert(it.key(), existing.value());
            after.insert(it.key(), it.value());
        }
        bool accepted = error.isEmpty() && after.isEmpty();
        if (error.isEmpty() && !after.isEmpty() && EndKeyformEditing(&error)) {
            try {
                auto candidate = m_document;
                const live2dstruct::CModelEdit command{QStringLiteral("selection"), QStringLiteral("objects"),
                    live2dstruct::CModelEdit::VisibilityValues, QVariant::fromValue(before), QVariant::fromValue(after), -1, false};
                if (candidate.ExecuteEdit(command)) {
                    m_document.CommitCandidate(std::move(candidate));
                    accepted = true;
                }
            } catch (const std::exception&) {
                error = QStringLiteral("无法准备批量可见性修改");
            }
        }
        if (!accepted) {
            const QScopedValueRollback<bool> restoring(m_syncingVisibility, true);
            if (m_treePanel) m_treePanel->ApplyVisibilityState(m_document.VisibilityState());
            statusBar()->showMessage(error.isEmpty() ? QStringLiteral("可见性修改失败") : error, 8000);
            return;
        }
        if (after.isEmpty()) return;
        ApplyVisibilityValues(after);
        if (m_inspectorPanel) m_inspectorPanel->SyncHistoryState();
        if (!m_applyingSession) {
            m_sessionDirty = true;
            setWindowTitle(QStringLiteral("live2d2psd · 未保存"));
        }
        RefreshUndoActions();
    });
    connect(m_inspectorPanel, &CInspectorPanel::VisibilityChanged, this,
            [this](const QString& name, const QString& type, bool visible) {
        ApplyVisibilityValues({{type + QLatin1Char('|') + name, visible}});
    });
    connect(m_inspectorPanel, &CInspectorPanel::VisibilityBatchChanged, this, &MainWindow::ApplyVisibilityValues);
    connect(m_treePanel, &CModelTreePanel::LocksBatchChanged, this, [this](const QMap<QString, bool>& values) {
        QString error;
        const auto revision = m_document.Revision();
        if ((m_openglwindows && !m_openglwindows->FinishMeshDrag(false, &error)) || !m_document.SetLockedValues(values)) {
            if (m_treePanel) {
                auto locks = m_document.VisibilityState();
                for (auto it = locks.begin(); it != locks.end(); ++it) it.value() = m_document.LockedKeys().contains(it.key());
                m_treePanel->ApplyLockedState(locks);
            }
            statusBar()->showMessage(error.isEmpty() ? QStringLiteral("锁定对象已改变，请重新选择") : error, 8000);
            return;
        }
        if (m_inspectorPanel) m_inspectorPanel->SetLockedState(m_document.LockedKeys());
        if (!m_applyingSession && revision != m_document.Revision()) {
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
    connect(m_inspectorPanel, &CInspectorPanel::DrawableKeyformActivated, this,
            [this](const QString& name, int keyformIndex){
                if (m_openglwindows) m_openglwindows->SetActiveKeyform(name, keyformIndex);
            });
    connect(m_inspectorPanel, &CInspectorPanel::DrawableIndicesChanged,
            m_openglwindows, &OpengGlForm::do_SetDrawableIndices);
    const auto markSessionDirty = [this]() {
        if (m_applyingSession) return;
        m_sessionDirty = true;
        setWindowTitle(QStringLiteral("live2d2psd · 未保存"));
        statusBar()->showMessage(QStringLiteral("有未保存的模型编辑"));
    };
    m_openglwindows->SetMeshEditHandlers(
        [this](const QString& id, int keyform, QString* error) {
            if (!m_inspectorPanel || m_inspectorPanel->GetSelectionId() != id) {
                if (error) *error = QStringLiteral("对象选择未成功，请重新选择网格");
                return false;
            }
            return ActivateKeyform(id, keyform, error);
        },
        [this](const QVector<int>& selected, const QVector<float>& positions, QString* error) {
            return m_keyformEditor.BeginDrag(selected, positions, error);
        }, [this](const QPointF& delta, QString* error) {
            const auto revision = m_document.Revision();
            if (!m_keyformEditor.DragBy(delta, error)) return false;
            RefreshKeyformUi(m_document.Revision() != revision);
            return true;
        }, [this](bool cancel, QString* error) {
            const auto revision = m_document.Revision();
            if (cancel) {
                if (!m_keyformEditor.CancelDrag(error)) return false;
            } else m_keyformEditor.FinishDrag();
            RefreshKeyformUi(m_document.Revision() != revision);
            return true;
        });
    connect(m_openglwindows, &OpengGlForm::MeshEditRejected, this,
            [this](const QString& error) { statusBar()->showMessage(error, 8000); });
    connect(m_openglwindows, &OpengGlForm::PreviewFailed, this,
            [this](const QString& error) { statusBar()->showMessage(error, 8000); });
    connect(m_openglwindows, &OpengGlForm::KeyformEditingFinishedRequested, this,
            [this] { EndKeyformEditing(); });
    connect(m_treePanel, &CModelTreePanel::VisibilityChanged, m_diagnosticsPanel, &CModelDiagnosticsPanel::Refresh);
    connect(m_inspectorPanel, &CInspectorPanel::DrawableOpacityChanged, this, markSessionDirty);
    connect(m_inspectorPanel, &CInspectorPanel::DrawableDrawOrderChanged, this, markSessionDirty);
    connect(m_inspectorPanel, &CInspectorPanel::DrawableKeyformPositionsChanged, this,
            [markSessionDirty](const QString&, int, const QVector<float>&){ markSessionDirty(); });
    connect(m_inspectorPanel, &CInspectorPanel::DrawableMaskIndicesChanged, this,
            [markSessionDirty](const QString&, const QVector<int>&){ markSessionDirty(); });
    connect(m_physicsPanel, &CPhysicsPanel::PhysicsConfigChanged, this, [this](){
        if (m_applyingSession) return;
        setWindowTitle(QStringLiteral("live2d2psd · 未保存"));
        statusBar()->showMessage(QStringLiteral("有未保存的物理配置"));
    });
    connect(m_physicsPanel, &CPhysicsPanel::PhysicsSaved, this, [this](const QString& path){
        LAppLive2DManager* mgr = LAppLive2DManager::GetInstance();
        LAppModel* model = (mgr && mgr->GetModelNum() > 0) ? mgr->GetModel(0) : nullptr;
        if (model && !model->ReloadPhysicsFromFile(path)) {
            statusBar()->showMessage(QStringLiteral("物理配置已保存，但运行时重载失败"), 8000);
            return;
        }
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
    connect(m_motionPanel, &CMotionPanel::PlayMotion, this, [this](const QString& group, int no){
        if (!EndKeyformEditing()) return;
        m_motionPanel->StopTimelinePreview();
        LAppLive2DManager* mgr = LAppLive2DManager::GetInstance();
        if(mgr && mgr->GetModelNum() > 0 && mgr->GetModel(0)){
            mgr->GetModel(0)->StartMotion(group.toUtf8().constData(), no, LAppDefine::PriorityForce);
        }
    });

    connect(m_motionPanel, &CMotionPanel::StopMotions, this, [this](){
        if (!EndKeyformEditing()) return;
        m_motionPanel->StopTimelinePreview();
        LAppLive2DManager* mgr = LAppLive2DManager::GetInstance();
        if(mgr && mgr->GetModelNum() > 0 && mgr->GetModel(0)) mgr->GetModel(0)->StopAllMotions();
    });

    connect(m_motionPanel, &CMotionPanel::ApplyExpression, this, [this](const QString& name){
        if (!EndKeyformEditing()) return;
        m_motionPanel->StopTimelinePreview();
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

    connect(m_motionPanel, &CMotionPanel::MotionPlaybackChanged, this, [](float speed, bool loop){
        LAppLive2DManager* mgr = LAppLive2DManager::GetInstance();
        if(mgr && mgr->GetModelNum() > 0 && mgr->GetModel(0)){
            mgr->GetModel(0)->SetMotionSpeed(speed);
            mgr->GetModel(0)->SetMotionLoop(loop);
        }
    });
    connect(m_motionPanel, &CMotionPanel::LipSyncWavChanged, this, [this](const QString& path){
        LAppLive2DManager* manager = LAppLive2DManager::GetInstance();
        LAppModel* model = manager && manager->GetModelNum() > 0 ? manager->GetModel(0) : nullptr;
        if (!model) return;
        if (path.isEmpty()) {
            model->StopLipSyncWav();
            m_motionPanel->SetLipSyncWavStatus(false);
            return;
        }
        if (!model->StartLipSyncWav(path)) {
            m_motionPanel->SetLipSyncWavStatus(false);
            statusBar()->showMessage(QStringLiteral("口型 WAV 加载失败：%1").arg(path), 8000);
            return;
        }
        m_motionPanel->SetLipSyncWavStatus(true, path);
        statusBar()->showMessage(QStringLiteral("已加载口型 WAV：%1").arg(path), 5000);
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
    connect(m_motionPanel, &CMotionPanel::PoseConfigChanged, this, [this](){
        m_poseDirty = true;
        setWindowTitle(QStringLiteral("live2d2psd · 未保存"));
        statusBar()->showMessage(QStringLiteral("有未保存的 Pose 配置"));
    });
    connect(m_motionPanel, &CMotionPanel::PoseSaved, this, [this](){
        m_poseDirty = false;
        if (!m_sessionDirty) setWindowTitle(QStringLiteral("live2d2psd"));
    });
    connect(m_motionPanel, &CMotionPanel::PosePartChanged, this, [this](const QStringList& showIds, const QStringList& hideIds){
        if (!EndKeyformEditing()) return;
        m_motionPanel->StopTimelinePreview();
        LAppLive2DManager* mgr = LAppLive2DManager::GetInstance();
        if(!mgr || mgr->GetModelNum() <= 0 || !mgr->GetModel(0) || showIds.isEmpty()) return;
        LAppModel* model = mgr->GetModel(0);
        for (const QString& showId : showIds) {
            if (!showId.isEmpty()) model->SetPartVisible(showId, true);
        }
        for(const QString& hideId : hideIds) {
            if (!hideId.isEmpty()) model->SetPartVisible(hideId, false);
        }
    });
    m_workspaceReady = true;
    m_saveSessionAction->setEnabled(true);
    m_loadSessionAction->setEnabled(true);

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

    m_exportResultsAction = new QAction(QStringLiteral("导出记录"), this);
    connect(m_exportResultsAction, &QAction::triggered, this, [this] {
        if (!m_exportResultsPanel) return;
        m_exportResultsPanel->show();
        m_exportResultsPanel->raise();
    });
    m_recoverPreviewAction = new QAction(QStringLiteral("恢复预览"), this);
    m_recoverPreviewAction->setStatusTip(QStringLiteral("重新尝试恢复导出前的模型与播放状态"));
    m_recoverPreviewAction->setVisible(false);
    connect(m_recoverPreviewAction, &QAction::triggered, this, &MainWindow::RecoverExportPreview);

    m_undoAction = new QAction(tr("撤销"), this);
    m_undoAction->setShortcut(QKeySequence::Undo);
    m_undoAction->setEnabled(false);
    m_undoAction->setStatusTip(tr("撤销当前编辑区操作"));
    m_redoAction = new QAction(tr("重做"), this);
    m_redoAction->setShortcut(QKeySequence::Redo);
    m_redoAction->setEnabled(false);
    m_redoAction->setStatusTip(tr("重做当前编辑区操作"));
    connect(m_undoAction, &QAction::triggered, this, &MainWindow::UndoDocumentEdit, Qt::UniqueConnection);
    connect(m_redoAction, &QAction::triggered, this, &MainWindow::RedoDocumentEdit, Qt::UniqueConnection);
    connect(qApp, &QApplication::focusChanged, this, [this](QWidget*, QWidget* current){
        if (!current || m_closingModel || !m_openglwindows) return;
        if (m_motionPanel && m_motionPanel->IsTimelineWidget(current)) {
            m_historySource = HistorySource::Timeline;
        } else {
            const auto contains = [current](QWidget* editor){
                return editor && (editor == current || editor->isAncestorOf(current));
            };
            if (!contains(m_paramPanel) && !contains(m_inspectorPanel) && !contains(m_treePanel) &&
                !contains(m_blendShapePanel) && !contains(m_openglwindows)) return;
            m_historySource = HistorySource::Parameters;
        }
        RefreshUndoActions();
    });
    m_saveSessionAction = new QAction(tr("保存会话"), this);
    m_saveSessionAction->setEnabled(false);
    m_saveSessionAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_S));
    connect(m_saveSessionAction, &QAction::triggered, this, &MainWindow::on_actionSaveSession_triggered);
    m_loadSessionAction = new QAction(tr("载入会话"), this);
    m_loadSessionAction->setEnabled(false);
    m_loadSessionAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_O));
    connect(m_loadSessionAction, &QAction::triggered, this, &MainWindow::on_actionLoadSession_triggered);
    m_darkTheme = QSettings(QStringLiteral("live2d2psd"), QStringLiteral("live2d2psd"))
                      .value(QStringLiteral("darkTheme"), false).toBool();
    m_workspaceMode = QSettings(QStringLiteral("live2d2psd"), QStringLiteral("live2d2psd"))
                          .value(QStringLiteral("workspaceMode"), 0).toInt();
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
    m_exportResultsPanel = new CExportResultsPanel(this);
    addDockWidget(Qt::BottomDockWidgetArea, m_exportResultsPanel);
    m_exportResultsPanel->hide();
    connect(m_exportResultsPanel, &CExportResultsPanel::OpenOutputRequested, this, [this](const QString& path) {
        if (!QFileInfo(path).isFile() || !QDesktopServices::openUrl(QUrl::fromLocalFile(path)))
            statusBar()->showMessage(QStringLiteral("无法打开输出文件：%1").arg(path), 8000);
    });
    connect(m_exportResultsPanel, &CExportResultsPanel::RetryRequested, this, [this](const QStringList& sources) {
        const auto options = m_lastBatchOptions;
        StartPsdBatch(sources, options.outputDirectory, options.all, options.groups, options.textures, options.cubismCompat);
    });
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
    mainToolBar->addAction(m_exportResultsAction);
    mainToolBar->addAction(m_recoverPreviewAction);
    mainToolBar->addSeparator();
    m_workspaceActionGroup = new QActionGroup(this);
    m_workspaceActionGroup->setExclusive(true);
    const QStringList workspaceNames = {QStringLiteral("建模"), QStringLiteral("动画"), QStringLiteral("物理")};
    for (int i = 0; i < workspaceNames.size(); ++i) {
        QAction* action = new QAction(workspaceNames[i], this);
        action->setCheckable(true);
        action->setData(i);
        action->setStatusTip(QStringLiteral("切换到%1工作区").arg(workspaceNames[i]));
        m_workspaceActionGroup->addAction(action);
        m_workspaceActions.append(action);
        mainToolBar->addAction(action);
    }
    connect(m_workspaceActionGroup, &QActionGroup::triggered, this,
            [this](QAction* action){ SetWorkspaceMode(action->data().toInt()); });
    SetWorkspaceMode(m_workspaceMode);
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
    m_btnToggleUserData = new QPushButton(QStringLiteral("网格标签"), this);
    m_btnTogglePhysics->setCheckable(true);
    m_btnToggleBlend->setCheckable(true);
    m_btnToggleTexture->setCheckable(true);
    m_btnToggleInspector->setCheckable(true);
    m_btnToggleDiagnostics->setCheckable(true);
    m_btnToggleUserData->setCheckable(true);
    m_btnTogglePhysics->setChecked(true);
    m_btnToggleBlend->setChecked(true);
    m_btnToggleTexture->setChecked(true);
    m_btnToggleInspector->setChecked(true);
    m_btnToggleDiagnostics->setChecked(false);
    m_btnToggleUserData->setChecked(false);
    m_btnTogglePhysics->setFlat(true);
    m_btnToggleBlend->setFlat(true);
    m_btnToggleTexture->setFlat(true);
    m_btnToggleInspector->setFlat(true);
    m_btnToggleDiagnostics->setFlat(true);
    m_btnToggleUserData->setFlat(true);

    // 未创建面板前先隐藏
    m_btnToggleTree->setVisible(false);
    m_btnToggleParam->setVisible(false);
    m_btnToggleMotion->setVisible(false);
    m_btnTogglePhysics->setVisible(false);
    m_btnToggleBlend->setVisible(false);
    m_btnToggleTexture->setVisible(false);
    m_btnToggleInspector->setVisible(false);
    m_btnToggleDiagnostics->setVisible(false);
    m_btnToggleUserData->setVisible(false);

    statusBar()->addPermanentWidget(m_btnToggleTree);
    statusBar()->addPermanentWidget(m_btnToggleParam);
    statusBar()->addPermanentWidget(m_btnToggleMotion);
    statusBar()->addPermanentWidget(m_btnTogglePhysics);
    statusBar()->addPermanentWidget(m_btnToggleBlend);
    statusBar()->addPermanentWidget(m_btnToggleTexture);
    statusBar()->addPermanentWidget(m_btnToggleInspector);
    statusBar()->addPermanentWidget(m_btnToggleDiagnostics);
    statusBar()->addPermanentWidget(m_btnToggleUserData);
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
