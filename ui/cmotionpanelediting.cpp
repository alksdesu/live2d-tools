#include "cmotionpanel.h"
#include "cmotioncurveview.h"
#include "cmotionkeymodel.h"
#include "ctableeditdelegate.h"
#include "live2dstruct/cmotionkeyclipboard.h"
#include <QApplication>
#include <QActionGroup>
#include <QPushButton>
#include <QSpinBox>
#include <QClipboard>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QItemSelectionModel>
#include <QLabel>
#include <QMenu>
#include <QMimeData>
#include <QScrollArea>
#include <QScopeGuard>
#include <QSplitter>
#include <QTabWidget>
#include <QSignalBlocker>
#include <QTableView>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <algorithm>
#include <cmath>
#include <limits>

namespace {
CMotionPointSelection remapped(const CMotionPointSelection& selection, const CMotionTimeline::BatchResult& result)
{
    CMotionPointSelection mapped;
    for (const auto& ref : selection) {
        const auto found = result.oldToNew.constFind(ref.track);
        if (found == result.oldToNew.cend()) mapped.append(ref);
        else if (ref.index >= 0 && ref.index < found->size() && found->at(ref.index) >= 0)
            mapped.append({ref.track, found->at(ref.index)});
    }
    return mapped;
}
}

void CMotionPanel::UpdateWorkspaceOrientation()
{
    if (!m_workspaceScroll || !m_timelineBox || !m_assetTabs || !m_workspaceSplitter) return;
    const auto* content = m_workspaceScroll->widget();
    const auto margins = content && content->layout() ? content->layout()->contentsMargins() : QMargins();
    const int available = m_workspaceScroll->viewport()->width() - margins.left() - margins.right();
    const int required = m_assetTabs->minimumSizeHint().width() + m_timelineBox->minimumSizeHint().width() +
                         m_workspaceSplitter->handleWidth();
    const auto orientation = available >= required ? Qt::Horizontal : Qt::Vertical;
    if (m_workspaceSplitter->orientation() == orientation) return;
    m_workspaceSplitter->setOrientation(orientation);
    m_workspaceSplitter->setStretchFactor(0, 0);
    m_workspaceSplitter->setStretchFactor(1, 1);
}

bool CMotionPanel::HasPendingTimelineCells() const
{
    return (m_timelineDelegate && m_timelineDelegate->HasPendingChanges()) ||
           (m_keyModel && m_keyModel->HasRejectedDraft());
}

bool CMotionPanel::TryCommitTimelineCells(QString* error)
{
    if (error) error->clear();
    if (!RequireTransportAvailable(error)) return false;
    if (!HasPendingTimelineCells()) return true;
    if (!CheckParameterEditGuard(error)) return false;
    const ScopedFlag parameterEdit(this, m_parameterEditApproved, true);
    const QPointer<CMotionPanel> alive(this);
    const ScopedFlag mutation(this, m_timelineMutation, true);
    const auto before = m_timeline;
    const bool dirty = m_timelineDirty;
    {
        QSignalBlocker blocked(this);
        m_timelineDelegate->CommitEditor();
        if (!alive) { blocked.dismiss(); return false; }
    }
    m_timelineDirty = dirty;
    if (!m_timeline.HasSameContent(before)) {
        emit TimelineActivated();
        if (!alive) return false;
        emit MotionTimelineChanged();
    } else RefreshTimelineHistory();
    if (!alive) return false;
    if (!HasPendingTimelineCells()) return true;
    const QString message = m_keyModel->HasRejectedDraft() ? m_keyModel->DraftError()
        : QStringLiteral("请先确认或取消正在输入的关键点");
    if (error) *error = message;
    m_timelineStatus->setText(message);
    return false;
}

void CMotionPanel::SetKeyframeSelection(CMotionPointSelection selection)
{
    selection.erase(std::remove_if(selection.begin(), selection.end(), [this](const auto& ref) {
        const auto found = m_timeline.Tracks().constFind(ref.track);
        return found == m_timeline.Tracks().cend() || ref.index < 0 || ref.index >= found->size() ||
            (m_targetFilter->currentData().toInt() >= 0 &&
             m_targetFilter->currentData().toInt() != static_cast<int>(ref.track.target));
    }), selection.end());
    std::sort(selection.begin(), selection.end());
    selection.erase(std::unique(selection.begin(), selection.end()), selection.end());
    m_selectedPoints = std::move(selection);
    const QPointer<CMotionPanel> alive(this);
    QSignalBlocker block(m_timelineTable->selectionModel());
    QItemSelection rows;
    QVector<int> indices;
    indices.reserve(m_selectedPoints.size());
    for (const auto& ref : m_selectedPoints) {
        const int row = m_keyModel->RowFor(ref);
        if (row >= 0) indices.append(row);
    }
    std::sort(indices.begin(), indices.end());
    for (int index = 0; index < indices.size();) {
        const int first = indices[index];
        int last = first;
        while (++index < indices.size() && indices[index] == last + 1) last = indices[index];
        rows.select(m_keyModel->index(first, 0), m_keyModel->index(last, m_keyModel->columnCount() - 1));
    }
    m_timelineTable->selectionModel()->select(rows, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    if (!m_selectedPoints.isEmpty()) {
        const auto current = m_timelineTable->currentIndex();
        if (!rows.contains(current)) {
            const int row = m_keyModel->RowFor(m_selectedPoints.first());
            if (row >= 0) m_timelineTable->selectionModel()->setCurrentIndex(
                m_keyModel->index(row, current.isValid() ? current.column() : 0), QItemSelectionModel::NoUpdate);
        }
    } else m_timelineTable->selectionModel()->clearCurrentIndex();
    m_curveView->SetSelectedPoints(m_selectedPoints);
    if (!alive) { block.dismiss(); return; }
    RefreshTangentActions();
    if (!alive) block.dismiss();
}

void CMotionPanel::ConnectKeyframeEditing()
{
    m_keyModel->SetEditHandler([this](const auto& ref, int column, const QString& text, QString* error) {
        return ApplyTimelineCellEdit(ref, column, text, error);
    });
    connect(m_keyModel, &CMotionKeyModel::DraftChanged, this, &CMotionPanel::RefreshTimelineHistory);
    connect(m_keyModel, &CMotionKeyModel::EditRejected, m_timelineStatus, &QLabel::setText);
    connect(m_timelineDelegate, &QAbstractItemDelegate::closeEditor, this,
            [this](QWidget*, QAbstractItemDelegate::EndEditHint hint) {
        if (hint == QAbstractItemDelegate::RevertModelCache) m_keyModel->DiscardDraft();
        QTimer::singleShot(0, this, &CMotionPanel::RefreshTimelineHistory);
    });
    connect(m_timelineTable->selectionModel(), &QItemSelectionModel::selectionChanged, this, [this] {
        if (m_updatingTimeline) return;
        CMotionPointSelection selection;
        for (const auto& index : m_timelineTable->selectionModel()->selectedRows())
            if (const auto* row = m_keyModel->RowAt(index.row())) selection.append(row->ref);
        SetKeyframeSelection(std::move(selection));
    });
    connect(m_curveView, &CMotionCurveView::SelectionChanged, this, &CMotionPanel::SetKeyframeSelection);
    connect(m_curveView, &CMotionCurveView::SelectionDragStarted, this,
            [this](const auto& selection) { BeginCurveGesture(selection, false); });
    connect(m_curveView, &CMotionCurveView::SelectionTranslated, this, &CMotionPanel::TranslateCurveSelection);
    connect(m_curveView, &CMotionCurveView::SelectionDragFinished, this, &CMotionPanel::FinishCurveGesture);
    connect(m_curveView, &CMotionCurveView::SelectionDragCanceled, this, &CMotionPanel::CancelCurveGesture);
    connect(m_curveView, &CMotionCurveView::TangentDragStarted, this, &CMotionPanel::BeginTangentGesture);
    connect(m_curveView, &CMotionCurveView::TangentHandleEdited, this, &CMotionPanel::EditTangentHandle);
    connect(m_curveView, &CMotionCurveView::TangentDragFinished, this, &CMotionPanel::FinishCurveGesture);
    connect(m_curveView, &CMotionCurveView::TangentDragCanceled, this, &CMotionPanel::CancelCurveGesture);
}

bool CMotionPanel::ApplyTimelineCellEdit(const CMotionPointRef& ref, int column, const QString& input, QString* error)
{
    if (!CheckParameterEditGuard(error)) return false;
    const ScopedFlag parameterEdit(this, m_parameterEditApproved, true);
    const auto fail = [&](const QString& message) { if (error) *error = message; return false; };
    if (!RequireTransportAvailable(error)) return false;
    const auto found = m_timeline.Tracks().constFind(ref.track);
    if (m_updatingTimeline || found == m_timeline.Tracks().cend() || ref.index < 0 || ref.index >= found->size())
        return fail(QStringLiteral("关键点已改变，请重新选择"));
    CMotionTimeline::BatchEdit edit;
    if (column == 3) {
        const QString text = input.trimmed().toLower();
        int type = -1;
        if (text == QLatin1String("0") || text == QLatin1String("linear") || text == QStringLiteral("线性")) type = 0;
        if (text == QLatin1String("1") || text == QLatin1String("bezier") || text == QStringLiteral("bézier")) type = 1;
        if (text == QLatin1String("2") || text == QLatin1String("stepped") || text == QStringLiteral("阶梯")) type = 2;
        if (text == QLatin1String("3") || text == QLatin1String("inverse stepped") || text == QStringLiteral("反阶梯")) type = 3;
        if (type < 0) return fail(QStringLiteral("请输入 0 线性、1 Bézier、2 阶梯或 3 反阶梯"));
        edit.segments.append({ref, type, type == 1 ? m_timeline.BezierControls(ref.track, ref.index) : QVector<QPointF>()});
    } else {
        bool valid = false;
        const double value = input.trimmed().toDouble(&valid);
        if (!valid || !std::isfinite(value) || !std::isfinite(static_cast<float>(value)))
            return fail(QStringLiteral("请输入有限且在浮点范围内的数值"));
        auto point = found->at(ref.index);
        if (column == 0) point.setX(value);
        else if (column == 2) point.setY(value);
        else return fail(QStringLiteral("此列不能编辑"));
        edit.updates.append({ref, point});
    }
    return ApplyKeyframeBatch(edit, column == 3 ? QStringLiteral("修改插值") : QStringLiteral("编辑关键帧"), error);
}

bool CMotionPanel::ApplyKeyframeBatch(const CMotionTimeline::BatchEdit& edit, const QString& label, QString* error)
{
    if (!CheckParameterEditGuard(error)) return false;
    const ScopedFlag parameterEdit(this, m_parameterEditApproved, true);
    if (!RequireTransportAvailable(error)) return false;
    if (m_timeline.IsRecording() || m_editGroupActive || m_restoringTimeline || m_updatingTimeline) {
        if (error) *error = QStringLiteral("请先结束录制或曲线拖动");
        return false;
    }
    CMotionTimeline candidate = m_timeline;
    CMotionTimeline::BatchResult result;
    if (!candidate.EditPoints(edit, result, error)) return false;
    if (!result.changed) return true;
    auto selection = result.inserted.isEmpty() ? remapped(m_selectedPoints, result) : result.inserted;
    return PublishKeyframeEdit(std::move(candidate), std::move(selection), label, error);
}

void CMotionPanel::BeginCurveGesture(const CMotionPointSelection& selection, bool bezier)
{
    const QPointer<CMotionCurveView> view(m_curveView);
    if (!CheckParameterEditGuard()) { if (view) view->CancelDrag(); return; }
    const ScopedFlag parameterEdit(this, m_parameterEditApproved, true);
    const QPointer<CMotionPanel> alive(this);
    const ScopedFlag mutation(this, m_timelineMutation, true);
    const bool pending = HasPendingTimelineCells();
    if (!TryCommitTimelineCells() || !alive || pending || m_timeline.IsRecording() || m_restoringTimeline || m_updatingTimeline) {
        if (alive) m_curveView->CancelDrag();
        return;
    }
    PauseTimelinePreview();
    if (!alive) return;
    emit TimelineActivated();
    if (!alive) return;
    BeginTimelineEditGroup(bezier ? QStringLiteral("编辑 Bézier 手柄") : QStringLiteral("移动关键帧"));
    if (!alive) return;
    m_gesturePoints = selection;
    m_curveGesture = bezier ? CurveGesture::Bezier : CurveGesture::Points;
}

void CMotionPanel::TranslateCurveSelection(double time, double value)
{
    if (m_curveGesture != CurveGesture::Points || !m_editGroupBefore) return;
    CMotionTimeline candidate = m_editGroupBefore->value.timeline;
    CMotionTimeline::BatchEdit edit;
    edit.transform = CMotionTimeline::PointTransform{m_gesturePoints, {}, {time, value}, 1, 1};
    CMotionTimeline::BatchResult result;
    QString error;
    if (!candidate.EditPoints(edit, result, &error)) { m_timelineStatus->setText(error); return; }
    PublishCurveGesture(std::move(candidate), result);
}

void CMotionPanel::BeginTangentGesture(CMotionPointRef key, CMotionTimeline::HandleSide side, bool linkForGesture)
{
    const QPointer<CMotionPanel> alive(this);
    BeginCurveGesture({key}, true);
    if (!alive) return;
    if (m_curveGesture != CurveGesture::Bezier || !m_editGroupBefore) return;
    const auto handle = m_editGroupBefore->value.timeline.TangentHandle(key, side);
    if (!handle) { m_curveView->CancelDrag(); return; }
    m_gestureHandle = CMotionTimeline::HandleEdit{key, side, *handle, linkForGesture}; m_activeHandle = m_gestureHandle;
    m_activeHandleKeyPoint = m_timeline.Tracks().value(key.track).value(key.index);
    RefreshTangentActions(); if (alive) ScheduleOnionRequest();
}

void CMotionPanel::EditTangentHandle(CMotionPointRef key, CMotionTimeline::HandleSide side, QPointF position)
{
    if (m_curveGesture != CurveGesture::Bezier || !m_editGroupBefore || !m_gestureHandle || m_gestureHandle->ref != key || m_gestureHandle->side != side) return;
    CMotionTimeline candidate = m_editGroupBefore->value.timeline;
    CMotionTimeline::BatchEdit edit; edit.handles.append({key, side, position, m_gestureHandle->linkForGesture});
    CMotionTimeline::BatchResult result; QString error;
    if (!candidate.EditPoints(edit, result, &error)) { m_timelineStatus->setText(error); return; }
    const bool automatic = m_editGroupBefore->value.timeline.Tangent(key.track, key.index).mode == CMotionTimeline::TangentMode::AutoClamped;
    const bool converted = automatic && candidate.Tangent(key.track, key.index).mode == CMotionTimeline::TangentMode::Aligned;
    const bool linked = m_gestureHandle->linkForGesture;
    const QPointer<CMotionPanel> alive(this); PublishCurveGesture(std::move(candidate), result);
    if (!alive || m_curveGesture != CurveGesture::Bezier || !m_gestureHandle) return;
    if (converted) m_timelineStatus->setText(QStringLiteral("自动切线已转为对齐；Esc 取消并恢复自动模式"));
    else if (linked && m_timeline.Tangent(key.track, key.index).mode == CMotionTimeline::TangentMode::Broken)
        m_timelineStatus->setText(QStringLiteral("本次联动两侧手柄，独立模式保持；Esc 取消"));
}

void CMotionPanel::PublishCurveGesture(CMotionTimeline candidate, const CMotionTimeline::BatchResult& result)
{
    if (candidate.HasSameContent(m_timeline)) return;
    const ScopedFlag mutation(this, m_timelineMutation, true);
    const QPointer<CMotionPanel> alive(this);
    m_selectedPoints = remapped(m_editGroupBefore->value.selection, result);
    m_timeline.SwapContent(candidate);
    RefreshTimelineTable();
    if (!alive) return;
    m_timelineStatus->setText(QStringLiteral("正在编辑 %1 个关键点；Esc 取消").arg(m_gesturePoints.size()));
    emit MotionTimelineChanged();
}

void CMotionPanel::FinishCurveGesture()
{
    if (m_curveGesture == CurveGesture::None) return;
    m_curveGesture = CurveGesture::None;
    m_gesturePoints.clear(); m_gestureHandle.reset();
    EndTimelineEditGroup();
}

void CMotionPanel::CancelCurveGesture()
{
    if (m_curveGesture == CurveGesture::None) return;
    m_curveGesture = CurveGesture::None;
    m_gesturePoints.clear(); m_gestureHandle.reset();
    const auto before = m_editGroupBefore;
    const QPointer<CMotionPanel> alive(this);
    m_editGroupActive = false;
    m_editGroupBefore.reset();
    if (before && !m_restoringTimeline) {
        m_currentTrackOrigins = before->origins;
        ApplyTimelineSnapshot(before->value);
        if (alive) RefreshTimelineHistory();
    }
}

QToolBar* CMotionPanel::CreateKeyframeTools(QWidget* parent)
{
    auto* tools = new QToolBar(parent);
    tools->setObjectName(QStringLiteral("motionKeyframeTools"));
    tools->setMovable(false);
    tools->setFloatable(false);
    const auto action = [tools](const QString& text, const QString& name) {
        auto* result = tools->addAction(text);
        result->setObjectName(name);
        return result;
    };
    connect(action(QStringLiteral("复制"), QStringLiteral("copyMotionKeys")), &QAction::triggered, this, [this] { CopyKeyframes(); });
    connect(action(QStringLiteral("剪切"), QStringLiteral("cutMotionKeys")), &QAction::triggered, this, [this] { CopyKeyframes(true); });
    auto* pasteAction = action(QStringLiteral("粘贴"), QStringLiteral("pasteMotionKeyAction"));
    auto* menu = new QMenu(tools);
    auto* replace = menu->addAction(QStringLiteral("替换重合关键点"));
    replace->setCheckable(true);
    replace->setObjectName(QStringLiteral("replaceCoincidentMotionKeys"));
    auto* intoTrack = menu->addAction(QStringLiteral("粘贴到选中轨道"));
    intoTrack->setObjectName(QStringLiteral("pasteMotionKeysIntoTrack"));
    auto* originalTracks = menu->addAction(QStringLiteral("粘贴到原轨道"));
    pasteAction->setMenu(menu);
    auto* paste = qobject_cast<QToolButton*>(tools->widgetForAction(pasteAction));
    paste->setObjectName(QStringLiteral("pasteMotionKeys"));
    paste->setPopupMode(QToolButton::MenuButtonPopup);
    connect(pasteAction, &QAction::triggered, this, [this, replace] { PasteKeyframes(false, replace->isChecked()); });
    connect(originalTracks, &QAction::triggered, this, [this, replace] { PasteKeyframes(false, replace->isChecked()); });
    connect(intoTrack, &QAction::triggered, this, [this, replace] { PasteKeyframes(true, replace->isChecked()); });
    connect(action(QStringLiteral("平移 / 缩放"), QStringLiteral("transformMotionKeys")), &QAction::triggered, this, &CMotionPanel::TransformKeyframes);
    auto* interpolation = action(QStringLiteral("插值"), QStringLiteral("motionInterpolation"));
    auto* modeMenu = new QMenu(tools);
    const QStringList labels{QStringLiteral("线性"), QStringLiteral("Bézier"), QStringLiteral("阶梯"), QStringLiteral("反阶梯")};
    for (int type = 0; type < labels.size(); ++type) {
        auto* item = modeMenu->addAction(labels[type]);
        item->setObjectName(QStringLiteral("motionInterpolation%1").arg(type));
        connect(item, &QAction::triggered, this, [this, type] { SetKeyframeInterpolation(type); });
    }
    interpolation->setMenu(modeMenu);
    qobject_cast<QToolButton*>(tools->widgetForAction(interpolation))->setPopupMode(QToolButton::InstantPopup);
    m_tangentAction = action(QStringLiteral("切线"), QStringLiteral("motionTangentMode"));
    auto* tangents = new QMenu(tools); auto* modes = new QActionGroup(tangents); modes->setExclusive(true);
    for (const auto mode : {CMotionTimeline::TangentMode::Broken, CMotionTimeline::TangentMode::Aligned, CMotionTimeline::TangentMode::Flat, CMotionTimeline::TangentMode::AutoClamped}) {
        auto* item = tangents->addAction(CMotionKeyModel::TangentName(mode)); item->setObjectName(QStringLiteral("motionTangentMode%1").arg(int(mode)));
        item->setCheckable(true); item->setData(int(mode)); modes->addAction(item); m_tangentModes.append(item);
        connect(item, &QAction::triggered, this, [this, mode] { SetTangentMode(mode); });
    }
    tangents->addSeparator(); m_tangentNumbers = tangents->addAction(QStringLiteral("编辑手柄数值…")); m_tangentNumbers->setObjectName(QStringLiteral("motionTangentNumbers"));
    connect(m_tangentNumbers, &QAction::triggered, this, &CMotionPanel::EditTangentNumbers);
    m_tangentUnrestricted = tangents->addAction(QStringLiteral("转换为自由 Bézier"));
    m_tangentUnrestricted->setObjectName(QStringLiteral("motionConvertUnrestricted"));
    m_tangentUnrestricted->setToolTip(QStringLiteral("将整条动作的受限 Bézier 转换为自由 Bézier，保留曲线形状并允许横向编辑手柄；可撤销"));
    connect(m_tangentUnrestricted, &QAction::triggered, this, [this] {
        const QPointer<CMotionPanel> alive(this);
        if (!TryCommitTimelineCells() || !alive || m_editGroupActive || m_timeline.IsRecording()) return;
        auto candidate = m_timeline;
        QString error;
        if (!candidate.ConvertToUnrestricted(&error) ||
            !PublishKeyframeEdit(std::move(candidate), m_selectedPoints, QStringLiteral("转换为自由 Bézier"), &error)) {
            if (alive) m_timelineStatus->setText(error);
        }
    });
    m_tangentAction->setMenu(tangents); qobject_cast<QToolButton*>(tools->widgetForAction(m_tangentAction))->setPopupMode(QToolButton::InstantPopup);
    m_timelineTable->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_timelineTable, &QWidget::customContextMenuRequested, this, [this, tangents](QPoint position) {
        if (HasPendingTimelineCells() || m_editGroupActive) return;
        const QPointer<CMotionPanel> alive(this);
        const auto index = m_timelineTable->indexAt(position);
        if (index.isValid() && !m_timelineTable->selectionModel()->isRowSelected(index.row(), {})) {
            const auto* row = m_keyModel->RowAt(index.row()); if (row) SetKeyframeSelection({row->ref});
        }
        if (!alive) return;
        RefreshTangentActions(); if (!alive) return;
        tangents->popup(m_timelineTable->viewport()->mapToGlobal(position));
    });
    m_snapFrames = action(QStringLiteral("帧吸附"), QStringLiteral("motionFrameSnapping"));
    m_snapFrames->setCheckable(true);
    m_snapFrames->setObjectName(QStringLiteral("motionFrameSnapping"));
    m_snapFrames->setToolTip(QStringLiteral("拖动时对齐动作帧率；Alt 临时关闭，Shift 限制移动轴"));
    m_snapFrames->setChecked(true);
    m_curveView->SetFrameSnapping(true);
    connect(m_snapFrames, &QAction::toggled, this, [this](bool enabled) {
        if (IsPreviewTransportSuspended()) {
            const QSignalBlocker block(m_snapFrames);
            m_snapFrames->setChecked(m_curveView->FrameSnapping());
            return;
        }
        m_curveView->SetFrameSnapping(enabled);
    });
    ConfigureOnionUi(parent, tools);
    tools->insertAction(tools->actions().first(), m_onionEnabled);
    tools->insertAction(tools->actions().first(), m_tangentAction);
    qobject_cast<QToolButton*>(tools->widgetForAction(m_tangentAction))->setPopupMode(QToolButton::InstantPopup);
    qobject_cast<QToolButton*>(tools->widgetForAction(m_onionEnabled))->setPopupMode(QToolButton::MenuButtonPopup);
    for (auto* button : tools->findChildren<QToolButton*>()) button->setFocusPolicy(Qt::StrongFocus);
    RefreshTangentActions();
    return tools;
}

void CMotionPanel::RefreshTangentActions()
{
    if (!m_tangentAction) return;
    int eligible = 0; bool mixed = false; std::optional<CMotionTimeline::TangentMode> common;
    for (const auto& key : m_selectedPoints) {
        const auto capability = m_timeline.Capabilities(key);
        if (!common) common = capability.state.mode; else if (*common != capability.state.mode) mixed = true;
        if (capability.valid && (capability.incoming.available || capability.outgoing.available)) ++eligible;
    }
    if (m_activeHandle && (!m_selectedPoints.contains(m_activeHandle->ref) ||
        m_timeline.Tracks().value(m_activeHandle->ref.track).value(m_activeHandle->ref.index) != m_activeHandleKeyPoint)) m_activeHandle.reset();
    const bool enabled = !IsPreviewTransportSuspended() && !m_timeline.IsRecording() && !m_editGroupActive && !m_timelineMutation && !m_restoringTimeline && !HasPendingTimelineCells();
    const QPointer<CMotionPanel> alive(this);
    m_tangentAction->setText(!common ? QStringLiteral("切线") : QStringLiteral("切线：%1").arg(mixed ? QStringLiteral("混合") : CMotionKeyModel::TangentName(*common)));
    if (!alive) return;
    m_tangentAction->setToolTip(QStringLiteral("已选 %1 点，%2 点有 Bézier 手柄；模式应用到可用点，其余点保持。Ctrl/⌘ 开始拖动可临时联动独立手柄；自动柄拖动会转为对齐。")
        .arg(m_selectedPoints.size()).arg(eligible));
    m_tangentAction->setEnabled(enabled && eligible > 0); if (!alive) return;
    for (auto* item : m_tangentModes) {
        const QSignalBlocker block(item); item->setChecked(common && !mixed && int(*common) == item->data().toInt());
    }
    m_tangentNumbers->setEnabled(enabled && eligible == 1 && m_selectedPoints.size() == 1);
    if (!alive) return;
    m_tangentUnrestricted->setEnabled(enabled && m_timeline.AreBeziersRestricted());
}

void CMotionPanel::SetTangentMode(CMotionTimeline::TangentMode mode)
{
    if (!CheckParameterEditGuard() || m_editGroupActive || m_timelineMutation || m_timeline.IsRecording()) return;
    const ScopedFlag approved(this, m_parameterEditApproved, true); const QPointer<CMotionPanel> alive(this);
    if (!TryCommitTimelineCells() || !alive || m_selectedPoints.isEmpty()) return;
    CMotionTimeline::BatchEdit edit;
    for (const auto& key : m_selectedPoints) {
        const auto capability = m_timeline.Capabilities(key);
        if (!capability.valid || (!capability.incoming.available && !capability.outgoing.available)) continue;
        const auto side = m_activeHandle && m_activeHandle->ref == key ? std::optional<CMotionTimeline::HandleSide>(m_activeHandle->side) : std::nullopt;
        edit.tangents.append({key, mode, side});
    }
    if (edit.tangents.isEmpty()) { m_timelineStatus->setText(QStringLiteral("所选关键点没有可用的 Bézier 手柄；请先显式修改前段插值")); RefreshTangentActions(); return; }
    const auto skipped = m_selectedPoints.size() - edit.tangents.size(); QString error;
    if (!ApplyKeyframeBatch(edit, QStringLiteral("修改关键点切线模式"), &error)) { if (alive) { m_timelineStatus->setText(error); RefreshTangentActions(); } return; }
    if (!alive) return;
    m_timelineStatus->setText(QStringLiteral("%1 点已应用%2切线%3").arg(edit.tangents.size()).arg(CMotionKeyModel::TangentName(mode), skipped ? QStringLiteral("；%1 点没有 Bézier 手柄，保持原状态").arg(skipped) : QString()));
    ScheduleOnionRequest();
}

void CMotionPanel::EditTangentNumbers()
{
    if (!CheckParameterEditGuard() || m_editGroupActive || m_timelineMutation || m_timeline.IsRecording()) return;
    const ScopedFlag approved(this, m_parameterEditApproved, true); const QPointer<CMotionPanel> alive(this);
    if (!TryCommitTimelineCells() || !alive || m_selectedPoints.size() != 1) return;
    PauseTimelinePreview(); if (!alive) return;
    const auto key = m_selectedPoints.first(); const auto capability = m_timeline.Capabilities(key); const auto source = m_timeline; const auto revision = m_timelineRevision;
    if (!capability.incoming.available && !capability.outgoing.available) return;
    QPointer<QDialog> dialog = new QDialog(this); const auto cleanup = qScopeGuard([dialog] { if (dialog) dialog->deleteLater(); });
    dialog->setObjectName(QStringLiteral("motionTangentValues")); dialog->setWindowTitle(QStringLiteral("编辑切线手柄"));
    auto* form = new QFormLayout(dialog); form->setRowWrapPolicy(QFormLayout::WrapLongRows); form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    auto* side = new QComboBox(dialog); side->setObjectName(QStringLiteral("motionTangentSide"));
    if (capability.incoming.available) side->addItem(QStringLiteral("入柄"), int(CMotionTimeline::HandleSide::Incoming));
    if (capability.outgoing.available) side->addItem(QStringLiteral("出柄"), int(CMotionTimeline::HandleSide::Outgoing));
    if (m_activeHandle && m_activeHandle->ref == key) side->setCurrentIndex(qMax(0, side->findData(int(m_activeHandle->side))));
    auto* time = new QLineEdit(dialog); time->setObjectName(QStringLiteral("motionHandleTime"));
    auto* value = new QLineEdit(dialog); value->setObjectName(QStringLiteral("motionHandleValue"));
    auto* linked = new QCheckBox(QStringLiteral("本次联动另一侧，独立模式保持"), dialog); linked->setObjectName(QStringLiteral("motionHandleLinked")); linked->setEnabled(capability.state.mode == CMotionTimeline::TangentMode::Broken);
    auto* status = new QLabel(dialog); status->setObjectName(QStringLiteral("motionHandleStatus")); status->setTextFormat(Qt::PlainText); status->setWordWrap(true); status->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    const auto update = [=] {
        const auto which = CMotionTimeline::HandleSide(side->currentData().toInt()); const auto point = source.TangentHandle(key, which);
        const auto allowed = which == CMotionTimeline::HandleSide::Incoming ? capability.incoming : capability.outgoing;
        time->setText(point ? QString::number(point->x(), 'g', 17) : QString()); value->setText(point ? QString::number(point->y(), 'g', 17) : QString());
        time->setReadOnly(!allowed.moveTime); value->setReadOnly(!allowed.moveValue);
        status->setText(capability.state.mode == CMotionTimeline::TangentMode::AutoClamped ? QStringLiteral("有效修改会将自动切线转为对齐；取消保留原模式") : capability.restricted ? QStringLiteral("受限 Bézier 的手柄时间固定；需要横向调整时，请先显式转换为自由 Bézier") : QStringLiteral("时间为秒，数值与轨道相同；支持科学计数法"));
    };
    connect(side, &QComboBox::currentIndexChanged, dialog, update); update();
    form->addRow(QStringLiteral("手柄"), side); form->addRow(QStringLiteral("时间（秒）"), time); form->addRow(QStringLiteral("值"), value); form->addRow(linked); form->addRow(status);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dialog); form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, dialog, [=] {
        if (!alive || !dialog) return;
        if (m_timelineRevision != revision || !m_timeline.HasSameContent(source) || m_selectedPoints != CMotionPointSelection{key} || HasPendingTimelineCells()) { status->setText(QStringLiteral("时间轴或选区已改变，请取消后重新打开")); return; }
        bool validTime = false, validValue = false; const double x = time->text().toDouble(&validTime), y = value->text().toDouble(&validValue);
        if (!validTime || !validValue || !std::isfinite(x) || !std::isfinite(y)) { status->setText(QStringLiteral("请输入有限的手柄时间与数值")); return; }
        CMotionTimeline::BatchEdit edit; const auto which = CMotionTimeline::HandleSide(side->currentData().toInt()); edit.handles.append({key, which, {x, y}, linked->isEnabled() && linked->isChecked()}); QString error;
        if (!ApplyKeyframeBatch(edit, QStringLiteral("编辑切线手柄"), &error)) { if (alive && dialog) status->setText(error); return; }
        if (!alive || !dialog) return;
        m_activeHandle = edit.handles.first(); m_activeHandleKeyPoint = m_timeline.Tracks().value(key.track).value(key.index); dialog->accept();
    });
    dialog->resize(fontMetrics().averageCharWidth() * 62, dialog->sizeHint().height()); dialog->exec();
}

live2dstruct::COnionSkin::Settings CMotionPanel::OnionSettings() const
{
    live2dstruct::COnionSkin::Settings result; if (!m_onionEnabled) return result;
    result.enabled = m_onionEnabled->isChecked(); result.duringPlayback = m_onionPlayback->isChecked();
    result.before = m_onionBefore->value(); result.after = m_onionAfter->value(); result.stepFrames = m_onionInterval->value();
    result.beforeOpacity = float(m_onionBeforeOpacity->value() / 100); result.afterOpacity = float(m_onionAfterOpacity->value() / 100); return result;
}

void CMotionPanel::ConfigureOnionUi(QWidget* parent, QToolBar* tools)
{
    m_onionEnabled = tools->addAction(QStringLiteral("洋葱皮")); m_onionEnabled->setObjectName(QStringLiteral("motionOnionSkin")); m_onionEnabled->setCheckable(true);
    m_onionEnabled->setToolTip(QStringLiteral("在主画布当前模型后方显示时间轴前后帧；不改变当前播放或参数"));
    auto* menu = new QMenu(tools); auto* settings = menu->addAction(QStringLiteral("设置…")); settings->setObjectName(QStringLiteral("motionOnionSettings")); settings->setCheckable(true);
    m_onionEnabled->setMenu(menu); qobject_cast<QToolButton*>(tools->widgetForAction(m_onionEnabled))->setPopupMode(QToolButton::MenuButtonPopup);
    m_onionSettingsPanel = new QWidget(parent); m_onionSettingsPanel->setObjectName(QStringLiteral("motionOnionSettingsPanel"));
    auto* form = new QFormLayout(m_onionSettingsPanel); form->setRowWrapPolicy(QFormLayout::WrapLongRows); form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    const auto integer = [&](const char* name, const QString& label, int minimum, int maximum, int value) {
        auto* input = new QSpinBox(m_onionSettingsPanel); input->setObjectName(QString::fromLatin1(name)); input->setAccessibleName(label); input->setRange(minimum, maximum); input->setValue(value); input->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        form->addRow(label, input); connect(input, &QSpinBox::valueChanged, this, &CMotionPanel::RefreshOnionSettings); return input;
    };
    m_onionBefore = integer("motionOnionBefore", QStringLiteral("前帧数量"), 0, live2dstruct::COnionSkin::MaximumPerSide, 1);
    m_onionAfter = integer("motionOnionAfter", QStringLiteral("后帧数量"), 0, live2dstruct::COnionSkin::MaximumPerSide, 1);
    m_onionInterval = integer("motionOnionInterval", QStringLiteral("间隔（动作帧）"), 1, std::numeric_limits<int>::max(), 1);
    const auto opacity = [&](const char* name, const QString& label) {
        auto* input = new QDoubleSpinBox(m_onionSettingsPanel); input->setObjectName(QString::fromLatin1(name)); input->setAccessibleName(label); input->setRange(0, 100); input->setDecimals(1); input->setValue(25); input->setSuffix(QStringLiteral("%")); input->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        form->addRow(label, input); connect(input, &QDoubleSpinBox::valueChanged, this, &CMotionPanel::RefreshOnionSettings); return input;
    };
    m_onionBeforeOpacity = opacity("motionOnionBeforeOpacity", QStringLiteral("前帧透明度")); m_onionAfterOpacity = opacity("motionOnionAfterOpacity", QStringLiteral("后帧透明度"));
    m_onionPlayback = new QCheckBox(QStringLiteral("播放时显示"), m_onionSettingsPanel); m_onionPlayback->setObjectName(QStringLiteral("motionOnionDuringPlayback")); form->addRow(m_onionPlayback);
    connect(m_onionPlayback, &QCheckBox::toggled, this, &CMotionPanel::RefreshOnionSettings);
    m_onionIntervalLabel = new QLabel(m_onionSettingsPanel); m_onionIntervalLabel->setObjectName(QStringLiteral("motionOnionDescription")); m_onionIntervalLabel->setTextFormat(Qt::PlainText); m_onionIntervalLabel->setWordWrap(true); m_onionIntervalLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred); form->addRow(m_onionIntervalLabel);
    m_onionStatusLabel = new QLabel(parent); m_onionStatusLabel->setObjectName(QStringLiteral("motionOnionStatus")); m_onionStatusLabel->setTextFormat(Qt::PlainText); m_onionStatusLabel->setWordWrap(true); m_onionStatusLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred); m_onionStatusLabel->hide();
    auto* layout = qobject_cast<QVBoxLayout*>(parent->layout()); layout->insertWidget(2, m_onionSettingsPanel); layout->insertWidget(3, m_onionStatusLabel); m_onionSettingsPanel->hide();
    connect(settings, &QAction::toggled, m_onionSettingsPanel, &QWidget::setVisible);
    connect(m_onionEnabled, &QAction::toggled, this, [this](bool enabled) { if (enabled && !m_onionCaptureError.isEmpty()) m_onionContentDirty = true; RefreshOnionSettings(); });
    connect(this, &CMotionPanel::MotionTimelineChanged, this, [this] { ScheduleOnionRequest(true); });
    connect(this, &CMotionPanel::TimelineFrameChanged, this, [this] { ScheduleOnionRequest(); });
    connect(this, &CMotionPanel::TimelinePreviewStopped, this, [this] { ScheduleOnionRequest(); });
    connect(m_keyModel, &CMotionKeyModel::DraftChanged, this, [this] { ScheduleOnionRequest(); });
    connect(m_recordTimelineCb, &QCheckBox::toggled, this, [this] { ScheduleOnionRequest(); });
    connect(m_playTimelineButton, &QPushButton::clicked, this, [this] { ScheduleOnionRequest(); });
    RefreshOnionSettings();
}

void CMotionPanel::RefreshOnionSettings()
{
    if (!m_onionIntervalLabel) return;
    const double fps = m_timeline.Timing().fps; const double seconds = std::isfinite(fps) && fps > 0 ? m_onionInterval->value() / fps : 0;
    m_onionIntervalLabel->setText(QStringLiteral("当前间隔 %1 秒。原色参考帧位于当前模型后方，依据时间轴曲线。实时物理可先烘焙，眨眼和呼吸等效果通过参数关键点加入。")
        .arg(seconds, 0, 'g', 8));
    m_onionStatusLabel->setVisible(m_onionEnabled->isChecked());
    if (m_onionEnabled->isChecked() && m_onionStatusLabel->text().isEmpty()) m_onionStatusLabel->setText(QStringLiteral("等待时间轴参考帧"));
    ScheduleOnionRequest();
}

void CMotionPanel::ScheduleOnionRequest(bool contentChanged)
{
    if (contentChanged) { m_onionContentDirty = true; m_onionCaptureError.clear(); }
    if (m_onionRequestQueued) return; m_onionRequestQueued = true;
    QTimer::singleShot(0, this, [this] {
        m_onionRequestQueued = false; const QPointer<CMotionPanel> alive(this); RefreshTangentActions();
        if (!alive) return;
        if (m_onionIntervalLabel) {
            const double fps = m_timeline.Timing().fps;
            m_onionIntervalLabel->setText(QStringLiteral("当前间隔 %1 秒。原色参考帧位于当前模型后方，依据时间轴曲线。实时物理可先烘焙，眨眼和呼吸等效果通过参数关键点加入。")
                .arg(std::isfinite(fps) && fps > 0 ? m_onionInterval->value() / fps : 0, 0, 'g', 8));
        }
        emit OnionSkinRequestChanged();
    });
}

bool CMotionPanel::CaptureOnionSkinRequest(live2dstruct::COnionSkin::Request& request, QString* error) const
{
    if (error) error->clear(); live2dstruct::COnionSkin::Request candidate;
    candidate.settings = OnionSettings(); candidate.time = m_timelineTime; candidate.useFades = m_previewFades && m_previewFades->isChecked(); candidate.loop = m_loopCb && m_loopCb->isChecked();
    candidate.playing = m_timelineTimer && m_timelineTimer->isActive(); candidate.recording = m_timeline.IsRecording();
    if (!candidate.settings.enabled) { request = std::move(candidate); return true; }
    if (IsPreviewTransportSuspended()) candidate.suspensionReason = QStringLiteral("预览传输暂停中，参考帧暂停");
    else if (candidate.recording) candidate.suspensionReason = QStringLiteral("正在录制，参考帧暂停");
    else if (m_editGroupActive || m_curveGesture != CurveGesture::None || m_timelineMutation || m_restoringTimeline || m_updatingTimeline) candidate.suspensionReason = QStringLiteral("正在编辑或恢复时间轴，结束后更新参考帧");
    else if (HasPendingTimelineCells()) candidate.suspensionReason = QStringLiteral("请先确认或取消关键点输入，参考帧已暂停");
    else if (!m_timeline.HasCurves()) candidate.suspensionReason = QStringLiteral("时间轴暂无可采样的模型曲线");
    candidate.suspended = !candidate.suspensionReason.isEmpty();
    if (candidate.suspended) { request = std::move(candidate); return true; }
    if (m_onionContentDirty) {
        m_onionContentDirty = false; m_onionCaptureError.clear();
        if (!live2dstruct::COnionSkin::CaptureTimeline(m_timeline, m_onionTimeline, &m_onionCaptureError)) m_onionTimeline = {};
    }
    if (!m_onionCaptureError.isEmpty()) { if (error) *error = m_onionCaptureError; return false; }
    candidate.timeline = m_onionTimeline; request = std::move(candidate); return true;
}

void CMotionPanel::SetOnionSkinStatus(const live2dstruct::COnionSkin::Status& status)
{
    if (status.generation < m_onionStatus.generation || status == m_onionStatus) return;
    m_onionStatus = status; if (!m_onionStatusLabel) return;
    using State = live2dstruct::COnionSkin::Status::State;
    QString text = status.message;
    if (status.state == State::Ready) {
        QStringList times; for (double time : status.sampleTimes) times.append(QStringLiteral("%1s").arg(time, 0, 'g', 8));
        text = QStringLiteral("已显示 %1 个参考帧%2").arg(times.size()).arg(times.isEmpty() ? QString() : QStringLiteral("：") + times.join(QStringLiteral("、")));
        if (status.omitted) text += QStringLiteral("；边界或重复采样省略 %1 帧").arg(status.omitted);
        if (status.reducedResolution) text += QStringLiteral("；参考分辨率 %1 × %2").arg(status.renderedSize.width()).arg(status.renderedSize.height());
    } else if (text.isEmpty()) {
        if (status.state == State::Preparing) text = QStringLiteral("正在生成时间轴参考帧…");
        else if (status.state == State::Suspended) text = QStringLiteral("参考帧已暂停");
        else if (status.state == State::Failed) text = QStringLiteral("参考帧生成失败；当前模型保持不变，可重新启用后重试");
    }
    m_onionStatusLabel->setText(text); m_onionStatusLabel->setVisible(m_onionEnabled && m_onionEnabled->isChecked());
}

void CMotionPanel::CopyKeyframes(bool cut)
{
    const QPointer<CMotionPanel> alive(this);
    if (!TryCommitTimelineCells() || !alive || m_selectedPoints.isEmpty() || m_editGroupActive || m_timeline.IsRecording()) return;
    QByteArray bytes;
    QString error;
    if (!CMotionKeyClipboard::Capture(m_timeline, m_selectedPoints, bytes, &error)) { m_timelineStatus->setText(error); return; }
    const auto selection = m_selectedPoints;
    const auto revision = m_timelineRevision;
    auto* mime = new QMimeData;
    mime->setData(CMotionKeyClipboard::MimeType, bytes);
    mime->setText(QString::fromUtf8(bytes));
    QApplication::clipboard()->setMimeData(mime);
    if (!alive) return;
    if (cut && revision == m_timelineRevision && selection == m_selectedPoints) RemoveTimelineKeyframe();
    else m_timelineStatus->setText(cut ? QStringLiteral("已复制；时间轴或选区已改变，未删除关键点")
                                     : QStringLiteral("已复制 %1 个关键点").arg(selection.size()));
}

void CMotionPanel::PasteKeyframes(bool selectedTrack, bool replaceCoincident)
{
    if (!CheckParameterEditGuard()) return;
    const ScopedFlag parameterEdit(this, m_parameterEditApproved, true);
    const QPointer<CMotionPanel> alive(this);
    if (!TryCommitTimelineCells() || !alive || m_editGroupActive || m_timeline.IsRecording()) return;
    const auto* mime = QApplication::clipboard()->mimeData();
    if (!mime) return;
    const auto bytes = mime->hasFormat(CMotionKeyClipboard::MimeType) ? mime->data(CMotionKeyClipboard::MimeType) : mime->text().toUtf8();
    std::optional<CMotionTrack> destination;
    if (selectedTrack) {
        if (m_selectedPoints.isEmpty()) { m_timelineStatus->setText(QStringLiteral("请先选择目标轨道的关键点")); return; }
        destination = m_selectedPoints.first().track;
        for (const auto& ref : m_selectedPoints) if (ref.track != *destination) {
            m_timelineStatus->setText(QStringLiteral("只能选择一条目标轨道")); return;
        }
    }
    CMotionKeyClipboard::Result result;
    QString error;
    if (!CMotionKeyClipboard::Paste(m_timeline, bytes, m_timelineTime, destination, replaceCoincident, result, &error)) {
        m_timelineStatus->setText(error); return;
    }
    for (const auto& track : result.timeline.Tracks().keys()) {
        if (m_timeline.Tracks().contains(track)) continue;
        const bool valid = track.target == CMotionTarget::Parameter ? m_parameterIds.contains(track.id)
            : track.target == CMotionTarget::PartOpacity ? m_modelPartIds.contains(track.id)
            : QStringList{"Opacity", "EyeBlink", "LipSync"}.contains(track.id);
        if (!valid) { m_timelineStatus->setText(QStringLiteral("模型中不存在轨道：%1 · %2").arg(track.TargetName(), track.id)); return; }
    }
    if (!result.changed) { SetKeyframeSelection(result.selection); return; }
    if (!PublishKeyframeEdit(std::move(result.timeline), result.selection, QStringLiteral("粘贴关键帧"), &error)) {
        if (alive) m_timelineStatus->setText(error);
        return;
    }
    if (!alive) return;
    if (result.convertedToUnrestricted)
        m_timelineStatus->setText(QStringLiteral("已粘贴；原有曲线已按等价控制点转换为自由 Bézier"));
}

void CMotionPanel::SetKeyframeInterpolation(int type)
{
    if (!CheckParameterEditGuard()) return;
    const ScopedFlag parameterEdit(this, m_parameterEditApproved, true);
    const QPointer<CMotionPanel> alive(this);
    if (!TryCommitTimelineCells() || !alive || m_selectedPoints.isEmpty()) return;
    CMotionTimeline::BatchEdit edit;
    for (const auto& ref : m_selectedPoints) if (ref.index > 0)
        edit.segments.append({ref, type, type == 1 ? m_timeline.BezierControls(ref.track, ref.index) : QVector<QPointF>()});
    if (edit.segments.isEmpty()) return;
    QString error;
    if (!ApplyKeyframeBatch(edit, QStringLiteral("修改关键帧插值"), &error) && alive) m_timelineStatus->setText(error);
}

void CMotionPanel::TransformKeyframes()
{
    if (!CheckParameterEditGuard()) return;
    const ScopedFlag parameterEdit(this, m_parameterEditApproved, true);
    const QPointer<CMotionPanel> alive(this);
    if (!TryCommitTimelineCells() || !alive || m_selectedPoints.isEmpty() || m_editGroupActive || m_timeline.IsRecording()) return;
    PauseTimelinePreview();
    const auto revision = m_timelineRevision;
    const auto selection = m_selectedPoints;
    const auto source = m_timeline;
    double pivot = std::numeric_limits<double>::infinity();
    for (const auto& ref : selection) pivot = qMin(pivot, source.Tracks().value(ref.track).at(ref.index).x());
    QPointer<QDialog> dialog = new QDialog(this);
    const auto cleanup = qScopeGuard([dialog] { if (dialog) delete dialog.data(); });
    dialog->setWindowTitle(QStringLiteral("平移 / 缩放关键帧"));
    auto* layout = new QFormLayout(dialog.data());
    const auto field = [dialog](const QString& name, double minimum, double maximum, double value) {
        auto* spin = new QDoubleSpinBox(dialog.data());
        spin->setObjectName(name);
        spin->setDecimals(9);
        spin->setRange(minimum, maximum);
        spin->setValue(value);
        return spin;
    };
    auto* offset = field(QStringLiteral("motionTimeOffset"), -std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), 0);
    auto* scale = field(QStringLiteral("motionTimeScale"), 0.000000001, std::numeric_limits<float>::max(), 1);
    auto* value = field(QStringLiteral("motionValueOffset"), -std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), 0);
    auto* valueScale = field(QStringLiteral("motionValueScale"), -std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), 1);
    auto* status = new QLabel(QStringLiteral("以最早选中时间 %1 秒为缩放中心").arg(pivot, 0, 'g', 9), dialog.data());
    status->setTextFormat(Qt::PlainText);
    status->setWordWrap(true);
    layout->addRow(status);
    layout->addRow(QStringLiteral("时间偏移（秒）"), offset);
    layout->addRow(QStringLiteral("时间倍率"), scale);
    layout->addRow(QStringLiteral("数值偏移"), value);
    layout->addRow(QStringLiteral("数值倍率（以 0 为中心）"), valueScale);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dialog.data());
    layout->addRow(buttons);
    connect(buttons, &QDialogButtonBox::rejected, dialog.data(), &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, dialog.data(), [&, this] {
        if (m_timelineRevision != revision || !m_timeline.HasSameContent(source) || m_selectedPoints != selection || HasPendingTimelineCells()) {
            status->setText(QStringLiteral("时间轴或选区已改变，请取消后重新打开")); return;
        }
        CMotionTimeline::BatchEdit edit;
        edit.transform = CMotionTimeline::PointTransform{selection, {pivot, 0}, {offset->value(), value->value()}, scale->value(), valueScale->value()};
        QString error;
        if (!ApplyKeyframeBatch(edit, QStringLiteral("平移 / 缩放关键帧"), &error)) {
            if (alive && dialog) status->setText(error);
            return;
        }
        if (alive && dialog) dialog->accept();
    });
    dialog->exec();
}
