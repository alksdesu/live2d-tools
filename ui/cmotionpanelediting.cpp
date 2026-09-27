#include "cmotionpanel.h"
#include "cmotioncurveview.h"
#include "cmotionkeymodel.h"
#include "ctableeditdelegate.h"
#include "live2dstruct/cmotionkeyclipboard.h"
#include <QApplication>
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
    const QSignalBlocker block(m_timelineTable->selectionModel());
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
        rows.select(m_keyModel->index(first, 0), m_keyModel->index(last, 3));
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
    connect(m_curveView, &CMotionCurveView::BezierDragStarted, this,
            [this](const auto& track, int index) { BeginCurveGesture({{track, index}}, true); });
    connect(m_curveView, &CMotionCurveView::BezierControlsEdited, this, &CMotionPanel::EditCurveControls);
    connect(m_curveView, &CMotionCurveView::BezierDragFinished, this, &CMotionPanel::FinishCurveGesture);
    connect(m_curveView, &CMotionCurveView::BezierDragCanceled, this, &CMotionPanel::CancelCurveGesture);
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
    for (const auto& ref : m_gesturePoints) {
        const auto found = candidate.Tracks().constFind(ref.track);
        if (found == candidate.Tracks().cend() || ref.index < 0 || ref.index >= found->size()) return;
        edit.updates.append({ref, found->at(ref.index) + QPointF(time, value)});
    }
    CMotionTimeline::BatchResult result;
    QString error;
    if (!candidate.EditPoints(edit, result, &error)) { m_timelineStatus->setText(error); return; }
    PublishCurveGesture(std::move(candidate), result);
}

void CMotionPanel::EditCurveControls(const CMotionTrack& track, int index, const QVector<QPointF>& controls)
{
    if (m_curveGesture != CurveGesture::Bezier || !m_editGroupBefore ||
        m_gesturePoints != CMotionPointSelection{{track, index}}) return;
    CMotionTimeline candidate = m_editGroupBefore->value.timeline;
    CMotionTimeline::BatchEdit edit;
    edit.segments.append({{track, index}, 1, controls});
    CMotionTimeline::BatchResult result;
    QString error;
    if (!candidate.EditPoints(edit, result, &error)) { m_timelineStatus->setText(error); return; }
    PublishCurveGesture(std::move(candidate), result);
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
    m_gesturePoints.clear();
    EndTimelineEditGroup();
}

void CMotionPanel::CancelCurveGesture()
{
    if (m_curveGesture == CurveGesture::None) return;
    m_curveGesture = CurveGesture::None;
    m_gesturePoints.clear();
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
    for (auto* button : tools->findChildren<QToolButton*>()) button->setFocusPolicy(Qt::StrongFocus);
    return tools;
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
    auto* status = new QLabel(QStringLiteral("以最早选中时间 %1 秒为缩放中心").arg(pivot, 0, 'g', 9), dialog.data());
    status->setTextFormat(Qt::PlainText);
    status->setWordWrap(true);
    layout->addRow(status);
    layout->addRow(QStringLiteral("时间偏移（秒）"), offset);
    layout->addRow(QStringLiteral("时间倍率"), scale);
    layout->addRow(QStringLiteral("数值偏移"), value);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dialog.data());
    layout->addRow(buttons);
    connect(buttons, &QDialogButtonBox::rejected, dialog.data(), &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, dialog.data(), [&, this] {
        if (m_timelineRevision != revision || !m_timeline.HasSameContent(source) || m_selectedPoints != selection || HasPendingTimelineCells()) {
            status->setText(QStringLiteral("时间轴或选区已改变，请取消后重新打开")); return;
        }
        CMotionTimeline::BatchEdit edit;
        for (const auto& ref : selection) {
            const auto point = source.Tracks().value(ref.track).at(ref.index);
            edit.updates.append({ref, QPointF(pivot + (point.x() - pivot) * scale->value() + offset->value(), point.y() + value->value())});
        }
        QString error;
        if (!ApplyKeyframeBatch(edit, QStringLiteral("平移 / 缩放关键帧"), &error)) {
            if (alive && dialog) status->setText(error);
            return;
        }
        if (alive && dialog) dialog->accept();
    });
    dialog->exec();
}
