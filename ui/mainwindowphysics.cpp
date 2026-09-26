#include "mainwindow.h"
#include "ui/cphysicspanel.h"
#include "ui/cphysicsbakedialog.h"
#include "ui/cmotionpanel.h"
#include "ui/cparampanel.h"
#include "ui/cmodeltreepanel.h"
#include "ui/cblendshapepanel.h"
#include "ui/ctexturepanel.h"
#include "ui/cinspectorpanel.h"
#include "ui/cmodeldiagnosticspanel.h"
#include "ui/cuserdatapanel.h"
#include "ui/cexporttaskrunner.h"
#include "ui/psdbatchjob.h"
#include "live2d/cphysicsbake.h"
#include "live2d/LAppModel.hpp"
#include "live2d/LAppLive2DManager.hpp"
#include "live2dstruct/cphysicsdocument.h"
#include "std/cexportcontrol.h"
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProgressDialog>
#include <QScopeGuard>
#include <QScopedValueRollback>
#include <QStatusBar>
#include <QTimer>
#include <exception>

namespace {
LAppModel* currentPhysicsModel()
{
    auto* manager = LAppLive2DManager::GetInstance();
    return manager && manager->GetModelNum() ? manager->GetModel(0) : nullptr;
}

bool fail(QString* error, const QString& message)
{
    if (error) *error = message;
    return false;
}

QSet<QString> conflictingOutputs(const CMotionTimeline& timeline, const QSet<QString>& eye, const QSet<QString>& lip)
{
    QSet<QString> result;
    if (timeline.Tracks().contains({CMotionTarget::Model, QStringLiteral("EyeBlink")})) result.unite(eye);
    if (timeline.Tracks().contains({CMotionTarget::Model, QStringLiteral("LipSync")})) result.unite(lip);
    return result;
}

bool matchingPhysics(LAppModel& model, const QJsonObject& document, QJsonObject& result, QString* error)
{
    QJsonObject expected, actual;
    if (!live2dstruct::CPhysicsDocument::Normalize(document, expected, error) ||
        !model.CapturePhysicsConfiguration(actual, error)) return false;
    const auto completeArrays = [](QJsonObject& configuration) {
        auto groups = configuration.value(QStringLiteral("PhysicsSettings")).toArray();
        for (int i = 0; i < groups.size(); ++i) {
            auto group = groups[i].toObject();
            for (const auto& name : {QStringLiteral("Input"), QStringLiteral("Output")})
                if (!group.contains(name)) group.insert(name, QJsonArray{});
            groups.replace(i, group);
        }
        configuration.insert(QStringLiteral("PhysicsSettings"), groups);
    };
    completeArrays(expected);
    completeArrays(actual);
    if (expected != actual) return fail(error, QStringLiteral("面板物理配置与当前模拟不一致，请重新打开模型或恢复会话后再烘焙"));
    result = std::move(actual);
    return true;
}

struct PhysicsBakePreview
{
    QPointer<OpengGlForm> surface;
    QPointer<CMotionPanel> motion;
    LAppModel* model = nullptr;
    CMotionPanel::PreviewTransportLease transport;
    bool surfacePaused = false;
    bool timerActive = false;
    int timerInterval = 0;
    QVector<QPair<QPointer<QWidget>, bool>> widgets;
    QVector<QPair<QPointer<QObject>, bool>> signalStates;
    QVector<QPointer<QAction>> actions;
};
}

void MainWindow::ConfigurePhysicsTools()
{
    auto* timer = new QTimer(m_physicsPanel);
    timer->setObjectName(QStringLiteral("physicsObservationTimer"));
    timer->setInterval(50);
    connect(timer, &QTimer::timeout, m_physicsPanel, [this] { RefreshPhysicsObservation(); });
    connect(m_physicsPanel, &CPhysicsPanel::ObservationGroupChanged, m_physicsPanel,
            [this](int) { RefreshPhysicsObservation(); });
    connect(m_physicsPanel, &CPhysicsPanel::BakeRequested, this, &MainWindow::BakePhysicsAnimation);
    timer->start();
}

void MainWindow::RestorePhysicsBakeActions()
{
    for (const auto& action : m_physicsBakeActions) if (action) action->setEnabled(true);
    m_physicsBakeActions.clear();
}

void MainWindow::RefreshPhysicsObservation()
{
    if (!m_physicsPanel || m_closingModel) return;
    const bool ready = m_runtimePanelsInitialized && m_openglwindows && m_openglwindows->IsModelReady();
    const bool busy = m_batchExporting || m_psdBatch || m_previewExport.IsBusy() || m_applyingSession || m_applyingParameterReferences;
    auto* model = ready ? currentPhysicsModel() : nullptr;
    const bool editable = model && !model->HasEditorPreview() && !model->HasExportPreview() && !model->GetSnapshotMode();
    m_physicsPanel->SetBakeAvailable(editable && !busy && m_motionPanel);
    if (busy) return;
    if (!model) {
        m_physicsPanel->ClearObservation(QStringLiteral("等待模型预览就绪"));
        return;
    }
    if (m_workspaceMode != 2 || !m_physicsPanel->isVisible()) {
        model->SetPhysicsObservationGroup(-1);
        m_physicsPanel->ClearObservation(QStringLiteral("进入物理工作区后开始监视"));
        return;
    }
    QString error;
    Csm::PhysicsObservation observation;
    if (!model->SetPhysicsObservationGroup(m_physicsPanel->SelectedGroupIndex()) ||
        !model->CapturePhysicsObservation(observation, &error)) {
        m_physicsPanel->ClearObservation(error.isEmpty() ? QStringLiteral("选择物理组以开始监视") : error);
        return;
    }
    const bool paused = !model->GetAutoPhysics() || !editable;
    m_physicsPanel->SetObservation(observation, paused ? QStringLiteral("物理模拟已暂停，显示最后一次采样") : QString{});
}

void MainWindow::BakePhysicsAnimation()
{
    QString error;
    const auto report = [this](const QString& message) { statusBar()->showMessage(message, 10000); };
    if (m_closingModel || m_batchExporting || m_psdBatch || m_previewExport.IsBusy() || m_activeExportControl ||
        m_applyingSession || m_applyingParameterReferences || !m_runtimePanelsInitialized ||
        !m_physicsPanel || !m_motionPanel || !m_openglwindows || !m_openglwindows->IsModelReady()) {
        report(QStringLiteral("请等待模型就绪并结束当前操作后再烘焙"));
        return;
    }
    auto* model = currentPhysicsModel();
    if (!model || model->HasEditorPreview() || model->HasExportPreview() || model->GetSnapshotMode()) {
        report(QStringLiteral("请先退出固定姿态编辑，再烘焙物理动画"));
        return;
    }
    QPointer<CMotionPanel> motion = m_motionPanel;
    QPointer<CPhysicsPanel> physics = m_physicsPanel;
    QPointer<OpengGlForm> surface = m_openglwindows;
    CPhysicsPanel::Snapshot physicsSnapshot;
    CMotionTimeline timeline;
    quint64 revision = 0;
    QJsonObject physicsConfiguration;
    if (!RuntimeMatchesDocument(&error) || !physics->CaptureSnapshot(physicsSnapshot, &error) ||
        !motion->CaptureBakeTimeline(timeline, revision, &error) ||
        !matchingPhysics(*model, physicsSnapshot.document, physicsConfiguration, &error)) {
        report(error);
        return;
    }
    QMap<QString, QString> outputNames;
    for (const auto& setting : physicsSnapshot.document.value(QStringLiteral("PhysicsSettings")).toArray()) {
        for (const auto& output : setting.toObject().value(QStringLiteral("Output")).toArray()) {
            const auto id = output.toObject().value(QStringLiteral("Destination")).toObject().value(QStringLiteral("Id")).toString();
            if (id.isEmpty()) continue;
            const auto name = m_document.ParameterMetadata(id).displayName;
            outputNames.insert(id, name.isEmpty() && m_cidJson ? m_cidJson->getParaName(id) : name);
        }
    }
    if (outputNames.isEmpty()) { report(QStringLiteral("当前物理配置没有可烘焙的输出参数")); return; }
    QSet<QString> eye, lip;
    model->GetTimelineEffectIds(eye, lip);
    CPhysicsBakeDialog dialog(outputNames, timeline, this);
    dialog.SetConflictingOutputs(conflictingOutputs(timeline, eye, lip));
    if (dialog.exec() != QDialog::Accepted) return;
    const auto options = dialog.Selection();
    if (!surface || surface != m_openglwindows || !motion || motion != m_motionPanel || !physics ||
        physics != m_physicsPanel || model != currentPhysicsModel() || m_closingModel || m_batchExporting ||
        m_psdBatch || m_previewExport.IsBusy() || !RuntimeMatchesDocument(&error)) {
        report(error.isEmpty() ? QStringLiteral("模型状态已改变，请重新开始烘焙") : error);
        return;
    }
    CPhysicsPanel::Snapshot currentPhysics;
    CMotionTimeline currentTimeline;
    quint64 currentRevision = 0;
    if (!physics->CaptureSnapshot(currentPhysics, &error) || !motion->CaptureBakeTimeline(currentTimeline, currentRevision, &error)) {
        report(error); return;
    }
    if (!physicsSnapshot.SameDocument(currentPhysics) || revision != currentRevision || !timeline.HasSameContent(currentTimeline)) {
        report(QStringLiteral("物理配置或动画已改变，请重新开始烘焙")); return;
    }
    if (!matchingPhysics(*model, currentPhysics.document, physicsConfiguration, &error)) { report(error); return; }
    model->GetTimelineEffectIds(eye, lip);
    if (!options.newAnimation && options.outputIds.intersects(conflictingOutputs(timeline, eye, lip))) {
        report(QStringLiteral("所选输出与全局眨眼或口型轨道冲突，请选择生成独立动画")); return;
    }
    CPhysicsBake::Request request;
    try {
        request.mocBytes = model->GetPreviewMocBytes().isEmpty() ? model->GetSourceMocBytes() : model->GetPreviewMocBytes();
        request.physicsBytes = QJsonDocument(physicsConfiguration).toJson(QJsonDocument::Compact);
        request.expectedMocDigest = QCryptographicHash::hash(request.mocBytes, QCryptographicHash::Sha256);
        request.expectedPhysicsDigest = QCryptographicHash::hash(request.physicsBytes, QCryptographicHash::Sha256);
        Csm::CubismModel::RuntimeValueSnapshot values;
        if (!model->GetModel() || !model->GetModel()->CaptureRuntimeValues(values, &error)) { report(error); return; }
        if (model->HasTimelinePreview()) request.baseParameters = model->GetTimelineBaseParameters();
        for (int i = 0; i < values.parameterIds.size(); ++i)
            if (!request.baseParameters.contains(values.parameterIds[i]))
                request.baseParameters.insert(values.parameterIds[i], values.HasSavedParameter(i) ? values.savedParameters[i] : values.parameterValues[i]);
        request.timeline = timeline;
        request.outputIds = options.outputIds;
        request.eyeBlinkIds = eye;
        request.lipSyncIds = lip;
        request.useFades = motion->UsesMotionFades();
        request.loop = motion->TimelineLoop();
        request.newAnimation = options.newAnimation;
        request.fps = options.fps;
        request.duration = options.duration;
        request.warmupCycles = options.warmupCycles;
    } catch (const std::exception&) {
        report(QStringLiteral("无法准备烘焙输入，原动画保持不变")); return;
    }
    auto state = std::make_shared<PhysicsBakePreview>();
    state->surface = surface;
    state->motion = motion;
    state->model = model;
    state->timerActive = surface->m_timer.isActive();
    state->timerInterval = surface->m_timer.interval();
    for (auto* widget : QList<QWidget*>{m_treePanel, m_paramPanel, m_motionPanel, m_physicsPanel, m_blendShapePanel,
            m_texturePanel, m_inspectorPanel, m_diagnosticsPanel, m_userDataPanel, surface.data()}) {
        if (!widget) continue;
        state->widgets.append({widget, widget->testAttribute(Qt::WA_ForceDisabled)});
        state->signalStates.append({widget, widget->signalsBlocked()});
    }
    for (auto* action : findChildren<QAction*>())
        if (action->isEnabled()) state->actions.append(action);
    m_physicsBakeActions = state->actions;
    auto control = std::make_shared<CExportControl>();
    const QScopedValueRollback<bool> exporting(m_batchExporting, true);
    m_activeExportControl = control;
    auto* progress = new QProgressDialog(QStringLiteral("正在准备物理烘焙"), QStringLiteral("取消"), 0, 1000, this);
    progress->setObjectName(QStringLiteral("physicsBakeProgress"));
    progress->setWindowModality(Qt::WindowModal);
    progress->setAutoClose(false);
    progress->setAutoReset(false);
    progress->setMinimumDuration(0);
    auto* label = new QLabel(progress);
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    progress->setLabel(label);
    m_singleExportProgress = progress;
    connect(progress, &QProgressDialog::canceled, this, [control] { control->Cancel(); });
    const auto finish = qScopeGuard([&] {
        m_activeExportControl.reset();
        if (m_singleExportProgress) delete m_singleExportProgress;
        m_batchExporting = false;
        if (!m_previewExport.NeedsRecovery()) m_physicsBakeActions.clear();
        RefreshExportActions();
        RefreshPhysicsObservation();
        if (m_closeAfterExport) QTimer::singleShot(0, this, [this] { m_closeAfterExport = false; close(); });
    });
    QTimer progressTimer;
    progressTimer.setInterval(50);
    connect(&progressTimer, &QTimer::timeout, progress, [progress, control] {
        const auto sample = control->Snapshot();
        progress->setValue(sample.total > 0 ? int(1000 * sample.current / sample.total) : 0);
        progress->setLabelText(control->IsCancellationRequested() ? QStringLiteral("正在取消并恢复预览")
            : QStringLiteral("正在计算物理动画\n%1 / %2 步").arg(sample.current).arg(sample.total));
    });
    auto result = std::make_shared<CPhysicsBake::Result>();
    CPreviewExportTransaction::Steps steps;
    steps.pause = [state](QString* detail) {
        if (!state->surface || !state->motion) return fail(detail, QStringLiteral("预览窗口已关闭"));
        if (!state->motion->SuspendPreviewTransport(state->transport, detail)) return false;
        if (!state->surface->SetExportSuspended(true, detail)) return false;
        state->surfacePaused = true;
        state->surface->m_timer.stop();
        for (const auto& entry : state->signalStates) if (entry.first) entry.first->blockSignals(true);
        for (const auto& entry : state->widgets) if (entry.first) entry.first->setEnabled(false);
        for (const auto& action : state->actions) if (action) action->setEnabled(false);
        return true;
    };
    steps.exportFile = [request, control, result](QString* detail) {
        CPhysicsBake baker;
        return CExportTaskRunner::Run([&](const CExportTaskRunner::GuiCall& gui, QString* taskError) {
            if (!gui([&](QString* reason) { return baker.Prepare(request, reason); }, taskError)) return false;
            bool complete = false;
            while (!complete) {
                if (!control->Checkpoint()) return fail(taskError, control->Error());
                if (!gui([&](QString* reason) {
                    QElapsedTimer budget; budget.start();
                    do {
                        if (!control->Checkpoint()) { baker.Cancel(); return fail(reason, control->Error()); }
                        if (!baker.Step(1, reason)) return false;
                        complete = baker.IsFinished();
                    } while (!complete && budget.elapsed() < 8);
                    control->Report(CExportControl::Phase::Rendering, baker.CompletedSteps(), baker.TotalSteps());
                    return true;
                }, taskError)) return false;
            }
            return gui([&](QString* reason) { return baker.CaptureResult(*result, reason); }, taskError);
        }, control.get(), detail);
    };
    steps.restorePreview = [this, state](QString* detail) {
        if (!state->surface || state->surface != m_openglwindows || !state->motion || state->motion != m_motionPanel ||
            currentPhysicsModel() != state->model)
            return fail(detail, QStringLiteral("原预览模型已改变，无法恢复烘焙前状态"));
        return true;
    };
    steps.resume = [state](QString* detail) {
        if (!state->surface || !state->motion || currentPhysicsModel() != state->model ||
            (state->motion->IsPreviewTransportSuspended() && !state->transport.IsValid()))
            return fail(detail, QStringLiteral("预览暂停状态已失效"));
        bool complete = false;
        const auto refreeze = qScopeGuard([&] {
            if (complete) return;
            try {
                state->surface->m_timer.stop();
                state->surface->SetExportSuspended(true);
                if (!state->motion->IsPreviewTransportSuspended()) state->motion->SuspendPreviewTransport(state->transport);
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
        for (const auto& entry : state->signalStates) if (entry.first) entry.first->blockSignals(entry.second);
        if (state->surfacePaused && state->timerActive) state->surface->m_timer.start(state->timerInterval);
        state->surfacePaused = false;
        complete = true;
        return true;
    };
    progress->show();
    progressTimer.start();
    const bool baked = m_previewExport.Run(std::move(steps), &error);
    progressTimer.stop();
    progress->hide();
    if (!baked) {
        report(control->WasCanceled() && !m_previewExport.NeedsRecovery() ? QStringLiteral("已取消烘焙，动画保持不变") : error);
        return;
    }
    CPhysicsPanel::Snapshot finalPhysics;
    if (!physics || !physics->CaptureSnapshot(finalPhysics, &error) || !physicsSnapshot.SameDocument(finalPhysics) ||
        !surface || surface != m_openglwindows || model != currentPhysicsModel() || !RuntimeMatchesDocument(&error)) {
        report(error.isEmpty() ? QStringLiteral("烘焙输入已改变，结果未写入动画") : error); return;
    }
    model->GetTimelineEffectIds(eye, lip);
    QJsonObject finalConfiguration;
    const auto& currentMoc = model->GetPreviewMocBytes().isEmpty() ? model->GetSourceMocBytes() : model->GetPreviewMocBytes();
    if (!matchingPhysics(*model, finalPhysics.document, finalConfiguration, &error)) { report(error); return; }
    if (currentMoc != request.mocBytes || finalConfiguration != physicsConfiguration ||
        eye != request.eyeBlinkIds || lip != request.lipSyncIds) {
        report(QStringLiteral("模型或物理求值配置已改变，结果未写入动画")); return;
    }
    if (!options.newAnimation && options.outputIds.intersects(conflictingOutputs(timeline, eye, lip))) {
        report(QStringLiteral("动画效果分组已改变，结果未写入动画")); return;
    }
    if (!control->BeginCommit()) { report(QStringLiteral("已取消烘焙，动画保持不变")); return; }
    const bool applied = motion && motion->ApplyBakedParameterSamples(timeline, revision, result->curves, options.newAnimation, result->fps, &error);
    control->CommitFinished(applied, error);
    if (!applied) { report(error); return; }
    m_batchExporting = false;
    SetWorkspaceMode(1);
    m_motionPanel->raise();
    report(QStringLiteral("已烘焙 %1 条物理曲线，可一次撤销；请保存动画或会话").arg(result->curves.size()));
}
