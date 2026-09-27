#include "mainwindow.h"
#include "ui/cparampanel.h"
#include "ui/cmotionpanel.h"
#include "ui/cblendshapepanel.h"
#include "ui/cinspectorpanel.h"
#include "ui/psdbatchjob.h"
#include "live2d/LAppModel.hpp"
#include "live2d/LAppLive2DManager.hpp"
#include <QCryptographicHash>
#include <QOpenGLContext>
#include <QSurface>
#include <QPointer>
#include <QScopeGuard>
#include <QSignalBlocker>
#include <QStatusBar>
#include <exception>
#include <optional>

using ValueController = live2dstruct::CParameterValueEditController;
using KeyformController = live2dstruct::CKeyformEditController;

namespace {
bool fail(QString* error, const QString& message) { if (error) *error = message; return false; }
LAppModel* currentModel()
{
    auto* manager = LAppLive2DManager::GetInstance();
    return manager && manager->GetModelNum() ? manager->GetModel(0) : nullptr;
}
class ContextRestore
{
public:
    ContextRestore() : previous(QOpenGLContext::currentContext()), surface(previous ? previous->surface() : nullptr),
        surfaceObject(dynamic_cast<QObject*>(surface)) {}
    ~ContextRestore()
    {
        if (QOpenGLContext::currentContext() == previous) return;
        if (auto* current = QOpenGLContext::currentContext()) current->doneCurrent();
        if (previous && previous->isValid() && surfaceObject) previous->makeCurrent(surface);
    }
private:
    QPointer<QOpenGLContext> previous;
    QSurface* surface;
    QPointer<QObject> surfaceObject;
};
bool makeCurrent(OpengGlForm* surface, QString* error)
{
    if (!surface || !surface->isValid() || !surface->context() || !surface->context()->isValid())
        return fail(error, QStringLiteral("模型预览的 OpenGL 上下文尚未就绪"));
    surface->makeCurrent();
    return QOpenGLContext::currentContext() == surface->context() || fail(error, QStringLiteral("无法激活模型预览上下文"));
}
}

struct MainWindow::ParameterValueEditState
{
    QPointer<OpengGlForm> surface;
    QPointer<CParamPanel> panel;
    QPointer<CMotionPanel> motion;
    LAppModel* model = nullptr;
    LAppModel::UserParameterContext runtime;
    KeyformController::LogicalState logical;
    CMotionPanel::ParameterEditTransportSnapshot transport;
    QMap<QString, float> keyformBase, displayBase;
    HistorySource history = HistorySource::None;
    ValueController::Publication publication;
    quint64 gesture = 0;
    bool snapshotMode = false, hadPreview = false, pendingUi = false;
};

bool MainWindow::ParameterValueEditingReady(QString* error) const
{
    if (m_closingModel || m_batchExporting || m_psdBatch || m_previewExport.IsBusy() || m_activeExportControl ||
        m_applyingSession || m_applyingParameterReferences || !m_runtimePanelsInitialized || !m_workspaceReady ||
        !m_paramPanel || !m_motionPanel || !m_openglwindows || !m_openglwindows->IsModelReady() ||
        m_openglwindows->IsExportSuspended() || m_motionPanel->IsPreviewTransportSuspended())
        return fail(error, QStringLiteral("请等待模型就绪并结束当前导出或恢复操作"));
    if (m_keyformEditor.IsDragging() || m_openglwindows->IsKeyformDragging())
        return fail(error, QStringLiteral("请先结束当前网格或变形器拖拽"));
    auto* model = currentModel();
    if (!model || model->HasExportPreview() || model->GetSourceMocDigest() != m_document.SourceDigest())
        return fail(error, QStringLiteral("参数编辑与当前模型快照不匹配"));
    return true;
}

bool MainWindow::CaptureParameterValueBase(QMap<QString, float>& values, QString* error) const
{
    if (!ParameterValueEditingReady(error)) return false;
    try {
        QMap<QString, float> candidate;
        if (!currentModel()->CaptureUserParameterBaseValues(candidate, error)) return false;
        const auto& overrides = m_document.ParameterValues();
        const auto& metadata = m_document.ParameterMetadataOverrides();
        for (const auto& parameter : m_document.Parameters()) {
            if (candidate.contains(parameter.id)) continue;
            const auto definition = metadata.constFind(parameter.id);
            candidate.insert(parameter.id, overrides.value(parameter.id,
                definition == metadata.cend() ? parameter.defaultValue : definition->defaultValue));
        }
        values.swap(candidate); return true;
    } catch (const std::exception&) { return fail(error, QStringLiteral("无法准备用户参数基础值")); }
}

QByteArray MainWindow::ParameterValueContextDigest() const
{
    auto* model = currentModel();
    if (!model || !m_openglwindows || (m_parameterValueEdit && !m_parameterValueEdit->runtime.IsValid())) return {};
    const auto identity = QByteArray::number(quintptr(model), 16) + ':' + model->GetSourceMocDigest() + model->GetPreviewMocDigest();
    return QCryptographicHash::hash(identity, QCryptographicHash::Sha256);
}

void MainWindow::ConfigureParameterValueEditing()
{
    if (!m_paramPanel) return;
    const QPointer<MainWindow> window(this);
    const QPointer<CParamPanel> panel(m_paramPanel);
    const QPointer<OpengGlForm> surface(m_openglwindows);
    const auto source = m_document.SourceDigest();
    const auto path = m_document.SourcePath();
    const auto valid = [window, panel, surface, source, path] {
        return window && panel && surface && window->m_paramPanel == panel && window->m_openglwindows == surface &&
            window->m_document.SourceDigest() == source && window->m_document.SourcePath() == path;
    };
    m_paramPanel->SetValueBaselineProvider([window, valid](auto& values, QString* error) {
        if (!valid() || window->m_publishingParameterValues) return fail(error, QStringLiteral("参数编辑上下文已改变"));
        return window->CaptureParameterValueBase(values, error);
    });
    m_paramPanel->SetValueContextDigestProvider([window, valid] { return valid() ? window->ParameterValueContextDigest() : QByteArray{}; });
    m_paramPanel->SetValuePublisher([window, valid](const auto& publication, QString* error) {
        return valid() ? window->PublishParameterValues(publication, error) : fail(error, QStringLiteral("参数发布目标已关闭或改变"));
    });
    connect(m_paramPanel, &CParamPanel::UserValuesApplied, this, &MainWindow::SyncParameterValueUi, Qt::UniqueConnection);
}

bool MainWindow::PublishParameterValues(const ValueController::Publication& publication, QString* error)
{
    if (error) error->clear();
    if (m_publishingParameterValues) return fail(error, QStringLiteral("参数值正在发布"));
    if (!ParameterValueEditingReady(error)) return false;
    const QPointer<MainWindow> alive(this);
    m_publishingParameterValues = true;
    const auto guard = qScopeGuard([alive] { if (alive) alive->m_publishingParameterValues = false; });
    const ContextRestore context;
    const bool starting = publication.phase == ValueController::Phase::Begin || publication.phase == ValueController::Phase::Single;
    if (!publication.restoreContext && !makeCurrent(m_openglwindows, error)) return false;
    auto state = m_parameterValueEdit;
    KeyformController::PreparedLogicalState logical;
    QMap<QString, float> displayBase;
    ValueController::Publication preparedPublication;
    try {
        preparedPublication = publication;
        if (starting) {
            if (state) return fail(error, QStringLiteral("请先结束已有参数事务"));
            if (!RuntimeMatchesDocument(error)) return false;
            state = std::make_shared<ParameterValueEditState>();
            state->surface = m_openglwindows; state->panel = m_paramPanel; state->motion = m_motionPanel; state->model = currentModel();
            state->gesture = publication.gestureId; state->keyformBase = m_keyformBaseValues; state->history = m_historySource;
            state->snapshotMode = state->model->GetSnapshotMode();
            state->hadPreview = state->model->HasEditorPreview() || state->model->HasTimelinePreview();
            if (!state->model->CaptureUserParameterContext(state->runtime, error) ||
                !m_keyformEditor.CaptureLogicalState(state->logical, error) ||
                !state->motion->CaptureParameterEditTransport(state->transport, error)) return false;
        }
        if (!state || state->gesture != publication.gestureId || state->model != currentModel() ||
            state->surface != m_openglwindows || state->panel != m_paramPanel || state->motion != m_motionPanel ||
            !state->runtime.IsValid()) return fail(error, QStringLiteral("参数事务的模型或面板已改变"));
        if (!state->motion->ValidateParameterEditTransport(state->transport, error)) return false;
        if (publication.phase != ValueController::Phase::Begin) {
            KeyformController::LogicalState desired;
            desired.sourceDigest = m_document.SourceDigest();
            if (publication.restoreContext) desired = state->logical;
            if (!m_keyformEditor.PrepareLogicalState(desired, logical, error)) return false;
            if (!publication.restoreContext) {
                if (!CaptureParameterValueBase(displayBase, error)) return false;
                for (auto value = publication.after.cbegin(); value != publication.after.cend(); ++value) displayBase[value.key()] = value.value();
            }
            if (!m_keyformEditor.ValidatePreparedLogicalState(logical, error)) return false;
        }
    } catch (const std::exception&) { return fail(error, QStringLiteral("无法准备参数值事务，原状态保持不变")); }
    if (publication.phase == ValueController::Phase::Begin) {
        if (!state->model->BeginUserParameterPreview(error)) return false;
        const bool paused = state->motion->PauseParameterEditTransport(state->transport);
        Q_ASSERT(paused);
        Q_UNUSED(paused);
        state->publication = std::move(preparedPublication);
        m_parameterValueEdit = std::move(state);
        return true;
    }
    if (publication.restoreContext) {
        if (!state->model->RestoreUserParameterContext(state->runtime, error)) return false;
        const bool restored = state->motion->RestoreParameterEditTransport(state->transport);
        Q_ASSERT(restored);
        Q_UNUSED(restored);
    } else {
        if (!state->model->ApplyUserParameterValues(publication.after, error)) return false;
        const bool paused = state->motion->PauseParameterEditTransport(state->transport);
        const bool frozen = state->model->BeginUserParameterPreview();
        Q_ASSERT(paused && frozen);
        Q_UNUSED(paused); Q_UNUSED(frozen);
    }
    m_keyformEditor.AdoptLogicalState(logical);
    state->displayBase.swap(displayBase);
    state->publication = std::move(preparedPublication); state->pendingUi = true;
    m_parameterValueEdit = std::move(state);
    return true;
}

void MainWindow::SyncParameterValueUi(const QMap<QString, float>& values)
{
    auto state = m_parameterValueEdit;
    if (!state || !state->pendingUi || state->model != currentModel() || !state->runtime.IsValid()) return;
    state->pendingUi = false;
    const QPointer<MainWindow> alive(this);
    const auto publication = state->publication;
    const bool finished = publication.phase == ValueController::Phase::Single || publication.phase == ValueController::Phase::Finish || publication.phase == ValueController::Phase::Cancel;
    if (publication.restoreContext) {
        m_keyformBaseValues = state->keyformBase;
        m_historySource = state->history;
    } else {
        m_keyformBaseValues.clear(); m_historySource = HistorySource::Parameters;
        if (state->hadPreview && m_paramPanel) m_paramPanel->ClearPreviewValues(state->displayBase);
    }
    if (!alive) return;
    if (finished && !publication.restoreContext) {
        if (publication.changed) state->model->StopAllMotions();
        state->model->EndUserParameterPreview(state->snapshotMode);
    }
    if (m_blendShapePanel) {
        const auto& display = !publication.restoreContext && state->hadPreview ? state->displayBase : values;
        for (auto value = display.cbegin(); value != display.cend(); ++value) {
            m_blendShapePanel->SyncParameterValue(value.key(), value.value());
            if (!alive) return;
        }
    }
    if (m_finishKeyformButton) m_finishKeyformButton->setVisible(m_keyformEditor.IsActive());
    if (m_openglwindows && m_openglwindows->isValid()) RefreshKeyformUi(false, false, false);
    if (!alive) return;
    if (m_openglwindows) m_openglwindows->update();
    if (finished && publication.changed && publication.phase != ValueController::Phase::Cancel) m_sessionDirty = true;
    if (finished && m_parameterValueEdit == state) m_parameterValueEdit.reset();
    RefreshWindowTitle();
    if (alive) RefreshUndoActions();
}

bool MainWindow::FinishParameterValueEditing(QString* error)
{
    if (m_materialEditor.IsActive()) return fail(error, QStringLiteral("请先保存或取消 UV 编辑"));
    if (m_publishingParameterValues || (m_parameterValueEdit && m_parameterValueEdit->pendingUi))
        return fail(error, QStringLiteral("参数事务正在更新界面，请稍后重试"));
    const QPointer<MainWindow> alive(this);
    const bool accepted = m_paramPanel && m_paramPanel->HasUserValueGesture()
        ? m_paramPanel->FinishUserValueGesture(error)
        : !m_parameterValueEdit || fail(error, QStringLiteral("参数事务尚未完成"));
    if (accepted && alive && m_blendShapePanel) {
        const auto values = m_paramPanel ? m_paramPanel->CurrentValues() : QMap<QString, float>();
        const QSignalBlocker block(m_blendShapePanel);
        m_blendShapePanel->CancelParameterGesture();
        for (auto value = values.cbegin(); value != values.cend(); ++value)
            m_blendShapePanel->SyncParameterValue(value.key(), value.value());
    }
    return accepted;
}

bool MainWindow::CancelParameterValueEditing(QString* error)
{
    if (m_publishingParameterValues || (m_parameterValueEdit && m_parameterValueEdit->pendingUi))
        return fail(error, QStringLiteral("参数事务正在更新界面，请稍后重试"));
    const QPointer<MainWindow> alive(this);
    const bool accepted = m_paramPanel && m_paramPanel->HasUserValueGesture()
        ? m_paramPanel->CancelUserValueGesture(error)
        : !m_parameterValueEdit || fail(error, QStringLiteral("参数事务尚未完成"));
    if (accepted && alive && m_blendShapePanel) {
        const auto values = m_paramPanel ? m_paramPanel->CurrentValues() : QMap<QString, float>();
        const QSignalBlocker block(m_blendShapePanel);
        m_blendShapePanel->CancelParameterGesture();
        for (auto value = values.cbegin(); value != values.cend(); ++value)
            m_blendShapePanel->SyncParameterValue(value.key(), value.value());
    }
    return accepted;
}

void MainWindow::ResetParameterValueEditing()
{
    if (m_publishingParameterValues) return;
    if (m_parameterValueEdit && m_parameterValueEdit->model == currentModel() && m_parameterValueEdit->runtime.IsValid())
        m_parameterValueEdit->model->EndUserParameterPreview(m_parameterValueEdit->snapshotMode);
    m_parameterValueEdit.reset();
}

bool MainWindow::ApplyParameterValueHistory(bool undo, live2dstruct::CModelEdit& edit, QString* error)
{
    if (!FinishParameterValueEditing(error) || !ParameterValueEditingReady(error)) return false;
    live2dstruct::CModelEdit pending;
    if (!(undo ? m_document.PeekUndoEdit(&pending) : m_document.PeekRedoEdit(&pending)) ||
        (pending.field != live2dstruct::CModelEdit::ParameterValue && pending.field != live2dstruct::CModelEdit::ParameterValues))
        return fail(error, QStringLiteral("当前历史不是参数值修改"));
    std::optional<live2dstruct::CModelDocument> candidate;
    live2dstruct::CModelEdit applied;
    QMap<QString, float> accepted;
    ValueController::Publication publication;
    try {
        candidate.emplace(m_document);
        if (!(undo ? candidate->UndoEdit(&applied) : candidate->RedoEdit(&applied))) return fail(error, QStringLiteral("参数历史与当前文档不匹配"));
        const QVariant& encoded = undo ? applied.before : applied.after;
        accepted = applied.field == live2dstruct::CModelEdit::ParameterValue
            ? QMap<QString, float>{{applied.name, encoded.toFloat()}} : encoded.value<QMap<QString, float>>();
        publication = {ValueController::Phase::Single, 0, {}, accepted, true, false};
    } catch (const std::exception&) { return fail(error, QStringLiteral("无法准备参数历史，原文档保持不变")); }
    if (!PublishParameterValues(publication, error)) return false;
    m_document.CommitCandidate(std::move(*candidate));
    edit = std::move(applied);
    SyncParameterValueUi(accepted);
    return true;
}
