#include "cmotionpanel.h"
#include "cmotioncurveview.h"
#include "cmotionsettingsdialog.h"
#include "ctableeditdelegate.h"
#include <QDebug>
#include <QAbstractButton>
#include <QKeyEvent>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QLabel>
#include <QSlider>
#include <QSplitter>
#include <QTabWidget>
#include <QGridLayout>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QScrollArea>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonParseError>
#include <QInputDialog>
#include <QMessageBox>
#include <QSaveFile>
#include <QDir>
#include <QDialog>
#include <QDialogButtonBox>
#include <QTableWidget>
#include <QHeaderView>
#include <QGroupBox>
#include <QFileDialog>
#include <QTimer>
#include <QSignalBlocker>
#include <QScopedValueRollback>
#include <QUndoStack>
#include <QUndoCommand>
#include <functional>
#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>
#include <exception>
#include <type_traits>


namespace {
class CTimelineEdit : public QUndoCommand
{
public:
    CTimelineEdit(const QString& label, std::function<void(bool)> apply)
        : QUndoCommand(label), m_apply(std::move(apply)) {}
    void undo() override { m_apply(false); }
    void redo() override {
        if (m_initialRedo) m_initialRedo = false;
        else m_apply(true);
    }
private:
    std::function<void(bool)> m_apply;
    bool m_initialRedo = true;
};

void clearLayoutItems(QLayout* layout)
{
    if (!layout) return;
    while (QLayoutItem* item = layout->takeAt(0)) {
        if (QWidget* widget = item->widget()) {
            delete widget;
        } else if (QLayout* child = item->layout()) {
            clearLayoutItems(child);
            delete child;
        }
        delete item;
    }
}

}

struct CMotionPanel::ParameterTrackArchive::Data
{
    QPointer<CMotionPanel> owner;
    quint64 generation = 0, serial = 0;
    QSet<QString> ids;
    CMotionTimeline::ParameterTracks current;
    QMap<quint64, CMotionTimeline::ParameterTracks> snapshots;
};

struct CMotionPanel::PreparedParameterTracks::Data
{
    struct Change {
        std::weak_ptr<TimelineSnapshotSlot> slot;
        quint64 serial = 0, revision = 0;
        TimelineSnapshot candidate;
        QMap<quint64, quint64> origins;
    };
    QPointer<CMotionPanel> owner;
    quint64 generation = 0, revision = 0, registryRevision = 0;
    int index = 0, count = 0, interval = 0;
    bool loop = false, playing = false, recording = false, eventStartPending = false;
    double time = 0, speed = 1;
    qint64 playbackLastNs = 0;
    CMotionTimeline timeline;
    QVector<Change> changes;
    QMap<quint64, std::weak_ptr<const ParameterTrackArchive::Data>> archives;
    QMap<quint64, quint64> origins;
    quint64 serial = 0;
    ParameterTrackArchive archive;
    bool changed = false;
};

CMotionPanel::PreparedParameterTracks::PreparedParameterTracks() = default;
CMotionPanel::PreparedParameterTracks::~PreparedParameterTracks() = default;
CMotionPanel::PreparedParameterTracks::PreparedParameterTracks(PreparedParameterTracks&&) noexcept = default;
CMotionPanel::PreparedParameterTracks& CMotionPanel::PreparedParameterTracks::operator=(PreparedParameterTracks&&) noexcept = default;
bool CMotionPanel::PreparedParameterTracks::IsValid() const
{
    return m_data && m_data->owner && m_data->owner->PreparedParameterTracksMatch(*this);
}
bool CMotionPanel::PreparedParameterTracks::HasChanges() const { return m_data && m_data->changed; }
const CMotionTimeline* CMotionPanel::PreparedParameterTracks::Timeline() const { return m_data ? &m_data->timeline : nullptr; }
bool CMotionPanel::PreparedParameterTracks::Loop() const { return m_data && m_data->loop; }
double CMotionPanel::PreparedParameterTracks::Time() const { return m_data ? m_data->time : 0; }
bool CMotionPanel::ParameterTrackArchive::IsValid() const
{
    return m_data && m_data->owner && m_data->generation == m_data->owner->m_historyGeneration;
}

bool CMotionPanel::PrepareParameterTrackRemoval(const QSet<QString>& ids, PreparedParameterTracks& result, QString* error) const
{
    return PrepareParameterTracks(ids, nullptr, result, error);
}

bool CMotionPanel::PrepareParameterTrackRestoration(const ParameterTrackArchive& archive,
                                                   PreparedParameterTracks& result, QString* error) const
{
    if (!archive.IsValid() || archive.m_data->owner != this ||
        m_parameterTrackArchives.value(archive.m_data->serial).lock() != archive.m_data) {
        if (error) *error = QStringLiteral("参数轨道历史已过期或属于其他时间轴");
        return false;
    }
    return PrepareParameterTracks(archive.m_data->ids, &archive, result, error);
}

bool CMotionPanel::PrepareParameterTracks(const QSet<QString>& ids, const ParameterTrackArchive* restoring,
                                        PreparedParameterTracks& result, QString* error) const
{
    if (error) error->clear();
    const auto fail = [&](const QString& message) { if (error) *error = message; return false; };
    if (m_transport || m_transportTransition || m_timelineMutation || m_restoringTimeline || m_refreshingParameterTracks ||
        m_timelineDelegate->HasPendingChanges() || (m_editGroupActive && !m_timeline.IsRecording()))
        return fail(QStringLiteral("请先结束当前时间轴编辑或导出暂停"));
    if (!m_timelineTimer || !std::isfinite(m_timelineTime) || !std::isfinite(m_timelinePlaybackSpeed) ||
        m_timelineTime < 0 || m_timelinePlaybackSpeed <= 0 || m_recordTimelineCb->isChecked() != m_timeline.IsRecording())
        return fail(QStringLiteral("时间轴播放或录制状态无效"));
    try {
        auto prepared = std::make_unique<PreparedParameterTracks::Data>();
        prepared->owner = const_cast<CMotionPanel*>(this);
        prepared->generation = m_historyGeneration;
        prepared->revision = m_timelineRevision;
        prepared->registryRevision = m_registryRevision;
        prepared->index = m_timelineHistory->index();
        prepared->count = m_timelineHistory->count();
        prepared->loop = m_loopCb->isChecked();
        prepared->time = m_timelineTime;
        prepared->speed = m_timelinePlaybackSpeed;
        prepared->playing = m_timelineTimer->isActive();
        prepared->interval = m_timelineTimer->interval();
        prepared->recording = m_timeline.IsRecording();
        prepared->eventStartPending = m_eventStartPending;
        prepared->playbackLastNs = m_playbackLastNs;
        prepared->timeline = m_timeline;
        auto archive = std::make_shared<ParameterTrackArchive::Data>();
        archive->owner = const_cast<CMotionPanel*>(this);
        archive->generation = m_historyGeneration;
        archive->ids = ids;
        prepared->archives = m_parameterTrackArchives;
        prepared->origins = m_currentTrackOrigins;
        prepared->serial = m_parameterTrackSerial;
        const auto inheritedTracks = [&](quint64 origin) -> const CMotionTimeline::ParameterTracks* {
            if (!restoring) return nullptr;
            if (!origin) return &restoring->m_data->current;
            const auto found = restoring->m_data->snapshots.constFind(origin);
            return found == restoring->m_data->snapshots.cend() ? nullptr : &*found;
        };
        if (restoring) {
            const auto* tracks = inheritedTracks(m_currentTrackOrigins.value(restoring->m_data->serial));
            if (!tracks) return fail(QStringLiteral("当前时间轴缺少待恢复的参数轨道版本"));
            if (!prepared->timeline.RestoreParameterTracks(*tracks, error)) return false;
            prepared->changed = !tracks->IsEmpty();
            prepared->archive = *restoring;
            prepared->archives.remove(restoring->m_data->serial);
            prepared->origins.remove(restoring->m_data->serial);
        } else {
            if (m_parameterTrackSerial == std::numeric_limits<quint64>::max())
                return fail(QStringLiteral("参数轨道历史标识已用尽"));
            archive->serial = ++prepared->serial;
            if (!prepared->timeline.RemoveParameterTracks(ids, archive->current, error)) return false;
            prepared->changed = !archive->current.IsEmpty();
            prepared->archive.m_data = archive;
            prepared->archives.insert(archive->serial, prepared->archive.m_data);
            prepared->origins.insert(archive->serial, 0);
        }
        prepared->changed = prepared->changed || !prepared->timeline.HasSameContent(m_timeline);
        QSet<quint64> visited;
        const auto prepareSlot = [&](const std::shared_ptr<TimelineSnapshotSlot>& slot) {
            if (!slot || visited.contains(slot->serial)) return true;
            visited.insert(slot->serial);
            PreparedParameterTracks::Data::Change change{slot, slot->serial, slot->revision, slot->value, slot->origins};
            if (restoring) {
                const auto* tracks = inheritedTracks(slot->origins.value(restoring->m_data->serial));
                if (!tracks) return fail(QStringLiteral("历史快照缺少待恢复的参数轨道版本"));
                if (!change.candidate.timeline.RestoreParameterTracks(*tracks, error)) return false;
                prepared->changed = prepared->changed || !tracks->IsEmpty();
                change.origins.remove(restoring->m_data->serial);
            } else {
                CMotionTimeline::ParameterTracks removed;
                if (!change.candidate.timeline.RemoveParameterTracks(ids, removed, error)) return false;
                prepared->changed = prepared->changed || !removed.IsEmpty();
                archive->snapshots.insert(slot->serial, std::move(removed));
                change.origins.insert(archive->serial, slot->serial);
            }
            prepared->changed = prepared->changed || !change.candidate.SameContent(slot->value);
            prepared->changes.append(std::move(change));
            return true;
        };
        for (const auto& weak : m_timelineSnapshots) if (!prepareSlot(weak.lock())) return false;
        if (m_editGroupActive && !prepareSlot(m_editGroupBefore)) return false;
        result.m_data = std::move(prepared);
        return true;
    } catch (const std::exception&) { return fail(QStringLiteral("无法准备时间轴参数轨道修改")); }
}

bool CMotionPanel::PreparedParameterTracksMatch(const PreparedParameterTracks& prepared) const noexcept
{
    if (!prepared.m_data) return false;
    const auto& state = *prepared.m_data;
    if (state.owner != this || state.generation != m_historyGeneration || state.revision != m_timelineRevision ||
        state.registryRevision != m_registryRevision || m_transport || m_transportTransition || m_timelineMutation || m_restoringTimeline ||
        m_refreshingParameterTracks || !m_timelineTimer || state.index != m_timelineHistory->index() ||
        state.count != m_timelineHistory->count() || state.time != m_timelineTime || state.loop != m_loopCb->isChecked() ||
        state.speed != m_timelinePlaybackSpeed || state.playing != m_timelineTimer->isActive() ||
        state.interval != m_timelineTimer->interval() || state.recording != m_timeline.IsRecording() ||
        state.eventStartPending != m_eventStartPending || state.playbackLastNs != m_playbackLastNs) return false;
    for (const auto& change : state.changes) {
        const auto slot = change.slot.lock();
        if (!slot || slot->serial != change.serial || slot->revision != change.revision) return false;
    }
    return true;
}

bool CMotionPanel::ValidatePreparedParameterTracks(const PreparedParameterTracks& prepared, QString* error) const
{
    if (error) error->clear();
    if (!PreparedParameterTracksMatch(prepared) || m_timelineDelegate->HasPendingChanges() ||
        (m_editGroupActive && !m_timeline.IsRecording())) {
        if (error) *error = QStringLiteral("准备后时间轴或历史已改变，请重新操作");
        return false;
    }
    return true;
}

bool CMotionPanel::CommitPreparedParameterTracks(PreparedParameterTracks& prepared, ParameterTrackArchive& archive) noexcept
{
    static_assert(std::is_nothrow_swappable_v<CMotionTimeline>);
    static_assert(std::is_nothrow_swappable_v<TimelineSnapshot>);
    if (!PreparedParameterTracksMatch(prepared)) return false;
    auto& state = *prepared.m_data;
    using std::swap;
    m_timeline.SwapContent(state.timeline);
    for (auto& change : state.changes) {
        const auto slot = change.slot.lock();
        swap(slot->value, change.candidate);
        slot->origins.swap(change.origins);
        ++slot->revision;
    }
    m_parameterTrackArchives.swap(state.archives);
    m_currentTrackOrigins.swap(state.origins);
    m_parameterTrackSerial = state.serial;
    archive = std::move(state.archive);
    ++m_registryRevision;
    if (state.changed) {
        ++m_timelineRevision;
        m_parameterTrackUiPending = true;
    }
    prepared.m_data.reset();
    return true;
}

void CMotionPanel::RefreshParameterTrackUi()
{
    if (!m_parameterTrackUiPending) return;
    m_parameterTrackUiPending = false;
    const QScopedValueRollback<bool> refreshing(m_refreshingParameterTracks, true);
    RefreshTimelineTable();
    RefreshTimelineHistory();
    emit MotionTimelineChanged();
}

std::shared_ptr<CMotionPanel::TimelineSnapshotSlot> CMotionPanel::RegisterTimelineSnapshot(TimelineSnapshot state)
{
    auto slot = std::make_shared<TimelineSnapshotSlot>();
    slot->serial = ++m_snapshotSerial;
    slot->value = std::move(state);
    for (auto archive = m_parameterTrackArchives.begin(); archive != m_parameterTrackArchives.end();) {
        if (archive->expired()) {
            m_currentTrackOrigins.remove(archive.key());
            archive = m_parameterTrackArchives.erase(archive);
        } else ++archive;
    }
    for (auto origin = m_currentTrackOrigins.begin(); origin != m_currentTrackOrigins.end();) {
        if (!m_parameterTrackArchives.contains(origin.key())) origin = m_currentTrackOrigins.erase(origin);
        else ++origin;
    }
    slot->origins = m_currentTrackOrigins;
    m_timelineSnapshots.erase(std::remove_if(m_timelineSnapshots.begin(), m_timelineSnapshots.end(),
        [](const auto& weak) { return weak.expired(); }), m_timelineSnapshots.end());
    m_timelineSnapshots.append(slot);
    ++m_registryRevision;
    return slot;
}

struct CMotionPanel::PreviewTransportState
{
    quint64 serial = 0, revision = 0;
    bool playing = false, recording = false, resumeRecordingClock = false;
    bool explicitlyDisabled = false, signalsBlocked = false, timerSignalsBlocked = false;
    bool eventStartPending = false, endEnabled = false, playEnabled = false, loop = false;
    bool timerSingleShot = false;
    int interval = 0, speedSlider = 0, playheadSlider = 0;
    Qt::TimerType timerType = Qt::CoarseTimer;
    double time = 0, speed = 1;
    QString playText, timeText, speedText, eventText, eventTooltip, statusText;
    QVector<QPair<QPointer<QAbstractButton>, bool>> checks;
    QVector<QPair<QPointer<QWidget>, bool>> enabledStates;
};

CMotionPanel::PreviewTransportLease::PreviewTransportLease(PreviewTransportLease&& other) noexcept
    : m_owner(std::exchange(other.m_owner, {})), m_serial(std::exchange(other.m_serial, 0)) {}

CMotionPanel::PreviewTransportLease& CMotionPanel::PreviewTransportLease::operator=(PreviewTransportLease&& other) noexcept
{
    if (this != &other) {
        m_owner = std::exchange(other.m_owner, {});
        m_serial = std::exchange(other.m_serial, 0);
    }
    return *this;
}

bool CMotionPanel::PreviewTransportLease::IsValid() const
{
    return m_owner && m_owner->m_transport && m_serial == m_owner->m_transport->serial;
}

bool CMotionPanel::IsPreviewTransportSuspended() const { return bool(m_transport); }

bool CMotionPanel::RequireTransportAvailable(QString* error) const
{
    if (!m_transport) return true;
    if (error) *error = QStringLiteral("时间轴正为临时导出保持暂停");
    return false;
}

bool CMotionPanel::SuspendPreviewTransport(PreviewTransportLease& lease, QString* error)
{
    const auto fail = [&](const QString& message) { if (error) *error = message; return false; };
    if (lease.IsValid() || m_transport || m_transportTransition) return fail(QStringLiteral("时间轴已经暂停，不能重复挂起"));
    const QScopedValueRollback<bool> changing(m_transportTransition, true);
    if (m_timelineDelegate->HasPendingChanges() || (m_editGroupActive && !m_timeline.IsRecording()))
        return fail(QStringLiteral("请先结束当前时间轴编辑"));
    if (!m_timelineTimer || m_timelineTimer->interval() < 0 || !std::isfinite(m_timelineTime) ||
        !std::isfinite(m_timelinePlaybackSpeed) || m_timelinePlaybackSpeed <= 0 ||
        m_recordTimelineCb->isChecked() != m_timeline.IsRecording() ||
        (m_timelineTimer->isActive() && (!m_playbackClock.isValid() || m_timeline.IsRecording())) ||
        m_transportSerial == std::numeric_limits<quint64>::max())
        return fail(QStringLiteral("时间轴播放状态无效"));
    std::unique_ptr<PreviewTransportState> state;
    QPointer<CMotionPanel> owner;
    try {
        state = std::make_unique<PreviewTransportState>();
        owner = this;
        state->serial = m_transportSerial + 1;
        state->revision = m_timelineRevision;
        state->playing = m_timelineTimer->isActive();
        state->recording = m_timeline.IsRecording();
        state->resumeRecordingClock = state->recording && !m_timeline.IsRecordingClockPaused();
        state->interval = m_timelineTimer->interval();
        state->timerType = m_timelineTimer->timerType();
        state->timerSingleShot = m_timelineTimer->isSingleShot();
        state->timerSignalsBlocked = m_timelineTimer->signalsBlocked();
        state->explicitlyDisabled = widget()->testAttribute(Qt::WA_ForceDisabled);
        state->signalsBlocked = signalsBlocked();
        state->time = m_timelineTime;
        state->speed = m_timelinePlaybackSpeed;
        state->speedSlider = m_speedSlider->value();
        state->playheadSlider = m_timelinePlayhead->value();
        state->eventStartPending = m_eventStartPending;
        state->loop = m_loopCb->isChecked();
        state->endEnabled = !m_endPreviewButton->testAttribute(Qt::WA_ForceDisabled);
        state->playEnabled = !m_playTimelineButton->testAttribute(Qt::WA_ForceDisabled);
        state->playText = m_playTimelineButton->text();
        state->timeText = m_timelineTimeLabel->text();
        state->speedText = m_speedLabel->text();
        state->eventText = m_eventStatus->text();
        state->eventTooltip = m_eventStatus->toolTip();
        state->statusText = m_timelineStatus->text();
        for (auto* check : {m_recordTimelineCb, m_loopCb, m_previewFades, m_eyeBlinkCb, m_breathCb, m_mouseFollowCb, m_physicsCb})
            state->checks.append({check, check->isChecked()});
        for (auto* control : widget()->findChildren<QWidget*>())
            state->enabledStates.append({control, control->testAttribute(Qt::WA_ForceDisabled)});
    } catch (const std::exception&) {
        return fail(QStringLiteral("无法保存时间轴播放状态"));
    }
    if (state->resumeRecordingClock && !m_timeline.PauseRecordingClock())
        return fail(QStringLiteral("无法暂停录制时钟"));
    m_transportSerial = state->serial;
    m_transport = std::move(state);
    lease.m_owner = std::move(owner);
    lease.m_serial = m_transportSerial;
    blockSignals(true);
    m_timelineTimer->stop();
    widget()->setEnabled(false);
    if (error) error->clear();
    return true;
}

void CMotionPanel::RestoreTransportControls()
{
    if (!m_transport) return;
    for (const auto& check : m_transport->checks) if (check.first) {
        const QSignalBlocker blocker(check.first);
        check.first->setChecked(check.second);
    }
    const QSignalBlocker speed(m_speedSlider), playhead(m_timelinePlayhead);
    m_speedSlider->setValue(m_transport->speedSlider);
    m_timelinePlayhead->setValue(m_transport->playheadSlider);
    m_playTimelineButton->setText(m_transport->playText);
    m_playTimelineButton->setEnabled(m_transport->playEnabled);
    m_endPreviewButton->setEnabled(m_transport->endEnabled);
    m_timelineTimeLabel->setText(m_transport->timeText);
    m_speedLabel->setText(m_transport->speedText);
    m_eventStatus->setText(m_transport->eventText);
    m_eventStatus->setToolTip(m_transport->eventTooltip);
    m_timelineStatus->setText(m_transport->statusText);
    for (const auto& control : m_transport->enabledStates)
        if (control.first) control.first->setEnabled(!control.second);
}

bool CMotionPanel::ResumePreviewTransport(PreviewTransportLease& lease, QString* error)
{
    const auto fail = [&](const QString& message) { if (error) *error = message; return false; };
    if (m_transportTransition || !m_transport || lease.m_owner != this || lease.m_serial != m_transport->serial)
        return fail(QStringLiteral("时间轴暂停凭据已失效、正在恢复或不属于当前面板"));
    if (m_timelineRevision != m_transport->revision || m_timeline.IsRecording() != m_transport->recording)
        return fail(QStringLiteral("时间轴已改变，继续保持暂停"));
    const QScopedValueRollback<bool> changing(m_transportTransition, true);
    const auto refreeze = [this] {
        blockSignals(true);
        m_timelineTimer->blockSignals(true);
        try { m_timelineTimer->stop(); }
        catch (...) {}
        try { widget()->setEnabled(false); }
        catch (...) {}
    };
    try {
        RestoreTransportControls();
        m_timelineTimer->stop();
        m_timelineTimer->setInterval(m_transport->interval);
        m_timelineTimer->setTimerType(m_transport->timerType);
        m_timelineTimer->setSingleShot(m_transport->timerSingleShot);
        m_timelineTimer->blockSignals(m_transport->timerSignalsBlocked);
        widget()->setEnabled(!m_transport->explicitlyDisabled);
        if (widget()->testAttribute(Qt::WA_ForceDisabled) != m_transport->explicitlyDisabled) {
            refreeze();
            return fail(QStringLiteral("时间轴控件仍被禁用，继续保持暂停"));
        }
        if (m_transport->resumeRecordingClock && !m_timeline.ResumeRecordingClock()) {
            refreeze();
            return fail(QStringLiteral("无法恢复录制时钟，继续保持暂停"));
        }
        m_timelineTime = m_transport->time;
        m_timelinePlaybackSpeed = m_transport->speed;
        m_eventStartPending = m_transport->eventStartPending;
        if (m_transport->playing) {
            m_playbackClock.start();
            m_playbackLastNs = 0;
            m_timelineTimer->start();
        }
    } catch (const std::exception& exception) {
        refreeze();
        return fail(QStringLiteral("无法恢复时间轴控件，继续保持暂停：%1").arg(QString::fromUtf8(exception.what())));
    } catch (...) {
        refreeze();
        return fail(QStringLiteral("无法恢复时间轴控件，继续保持暂停"));
    }
    auto state = std::move(m_transport);
    lease.m_owner.clear();
    lease.m_serial = 0;
    blockSignals(state->signalsBlocked);
    if (error) error->clear();
    return true;
}

bool CMotionPanel::ModelHasPart(const QString& id, bool* available) const
{
    if (available) *available = m_modelPartsKnown;
    return m_modelPartsKnown && m_modelPartIds.contains(id);
}

CMotionPanel::CMotionPanel(QWidget *parent)
    : QDockWidget("动作/表情", parent)
{
    setObjectName(QStringLiteral("motionPanel"));
    m_timelineHistory = new QUndoStack(this);
    m_timelineHistory->setUndoLimit(100);
    QWidget* wrapper = new QWidget(this);
    QVBoxLayout* mainLayout = new QVBoxLayout(wrapper);

    m_workspaceSplitter = new QSplitter(Qt::Horizontal, wrapper);
    m_workspaceSplitter->setObjectName(QStringLiteral("animationWorkspaceSplitter"));
    m_workspaceSplitter->setChildrenCollapsible(false);
    m_assetTabs = new QTabWidget(m_workspaceSplitter);
    m_assetTabs->setObjectName(QStringLiteral("animationAssetTabs"));
    m_assetTabs->setDocumentMode(true);

    // 动作部分
    QWidget* motionWidget = new QWidget();
    QVBoxLayout* motionLayout = new QVBoxLayout(motionWidget);
    motionLayout->addWidget(new QLabel("动作列表"));
    m_motionSearch = new QLineEdit(motionWidget);
    m_motionSearch->setPlaceholderText(QStringLiteral("搜索动作或分组"));
    m_motionSearch->setClearButtonEnabled(true);
    motionLayout->addWidget(m_motionSearch);
    m_motionList = new QListWidget();
    m_motionList->setObjectName(QStringLiteral("motionCatalog"));
    motionLayout->addWidget(m_motionList);
    QPushButton* playBtn = new QPushButton("▶ 播放");
    playBtn->setObjectName("primaryBtn");
    playBtn->setEnabled(false);
    playBtn->setAccessibleName(QStringLiteral("播放选中的动作"));
    QPushButton* stopBtn = new QPushButton("■ 停止");
    auto* loadLipSyncBtn = new QPushButton(QStringLiteral("加载口型 WAV"), motionWidget);
    auto* stopLipSyncBtn = new QPushButton(QStringLiteral("停止口型"), motionWidget);
    m_lipSyncWavStatus = new QLabel(QStringLiteral("口型：未加载"), motionWidget);
    loadLipSyncBtn->setObjectName(QStringLiteral("loadLipSyncWav"));
    stopLipSyncBtn->setObjectName(QStringLiteral("stopLipSyncWav"));
    auto* motionActions = new QGridLayout();
    motionActions->addWidget(playBtn, 0, 0);
    motionActions->addWidget(stopBtn, 0, 1);
    motionActions->addWidget(loadLipSyncBtn, 1, 0);
    motionActions->addWidget(stopLipSyncBtn, 1, 1);
    motionLayout->addLayout(motionActions);
    motionLayout->addWidget(m_lipSyncWavStatus);
    m_assetTabs->addTab(motionWidget, QStringLiteral("动作"));
    connect(m_motionList, &QListWidget::currentRowChanged, this, [this, playBtn](int row){
        playBtn->setEnabled(row >= 0 && row < m_motionEntries.size());
    });

    connect(playBtn, &QPushButton::clicked, this, [this](){
        int row = m_motionList->currentRow();
        if(row >= 0 && row < m_motionEntries.size()){
            emit PlayMotion(m_motionEntries[row].group, m_motionEntries[row].no);
        }
    });
    connect(stopBtn, &QPushButton::clicked, this, &CMotionPanel::StopMotions);
    connect(loadLipSyncBtn, &QPushButton::clicked, this, &CMotionPanel::LoadLipSyncWav);
    connect(stopLipSyncBtn, &QPushButton::clicked, this, &CMotionPanel::StopLipSyncWav);

    connect(m_motionList, &QListWidget::itemDoubleClicked, this, [this](){
        int row = m_motionList->currentRow();
        if(row >= 0 && row < m_motionEntries.size()){
            emit PlayMotion(m_motionEntries[row].group, m_motionEntries[row].no);
        }
    });

    // 表情部分
    QWidget* exprWidget = new QWidget();
    QVBoxLayout* exprLayout = new QVBoxLayout(exprWidget);
    exprLayout->addWidget(new QLabel("表情列表"));
    m_exprSearch = new QLineEdit(exprWidget);
    m_exprSearch->setPlaceholderText(QStringLiteral("搜索表情"));
    m_exprSearch->setClearButtonEnabled(true);
    exprLayout->addWidget(m_exprSearch);
    m_exprList = new QListWidget();
    m_exprList->setObjectName(QStringLiteral("expressionCatalog"));
    exprLayout->addWidget(m_exprList);
    QPushButton* applyBtn = new QPushButton("✦ 应用");
    applyBtn->setObjectName("primaryBtn");
    applyBtn->setEnabled(false);
    applyBtn->setAccessibleName(QStringLiteral("应用选中的表情"));
    m_editExpressionButton = new QPushButton(QStringLiteral("编辑表情"), exprWidget);
    m_editExpressionButton->setEnabled(false);
    auto* expressionActions = new QHBoxLayout();
    expressionActions->addWidget(applyBtn);
    expressionActions->addWidget(m_editExpressionButton);
    exprLayout->addLayout(expressionActions);
    m_assetTabs->addTab(exprWidget, QStringLiteral("表情"));

    // Pose 区域
    m_poseWidget = new QWidget();
    QVBoxLayout* poseLayout = new QVBoxLayout(m_poseWidget);
    poseLayout->addWidget(new QLabel("Pose 切换"));
    auto* poseTools = new QGridLayout();
    m_addPosePartButton = new QPushButton(QStringLiteral("添加 Part"), m_poseWidget);
    m_removePosePartButton = new QPushButton(QStringLiteral("删除 Part"), m_poseWidget);
    m_editPoseLinksButton = new QPushButton(QStringLiteral("编辑 Link"), m_poseWidget);
    m_savePoseButton = new QPushButton(QStringLiteral("保存 Pose"), m_poseWidget);
    m_addPosePartButton->setEnabled(false);
    m_removePosePartButton->setEnabled(false);
    m_editPoseLinksButton->setEnabled(false);
    m_savePoseButton->setEnabled(false);
    poseTools->addWidget(m_addPosePartButton, 0, 0);
    poseTools->addWidget(m_removePosePartButton, 0, 1);
    poseTools->addWidget(m_editPoseLinksButton, 1, 0);
    poseTools->addWidget(m_savePoseButton, 1, 1);
    poseLayout->addLayout(poseTools);
    connect(m_addPosePartButton, &QPushButton::clicked, this, &CMotionPanel::AddPosePart);
    connect(m_removePosePartButton, &QPushButton::clicked, this, &CMotionPanel::RemovePosePart);
    connect(m_editPoseLinksButton, &QPushButton::clicked, this, &CMotionPanel::EditPoseLinks);
    connect(m_savePoseButton, &QPushButton::clicked, this, [this](){
        QString error;
        if (!SavePose(&error)) QMessageBox::warning(this, QStringLiteral("保存 Pose 失败"), error);
    });
    auto* poseScroll = new QScrollArea(m_assetTabs);
    poseScroll->setWidgetResizable(true);
    poseScroll->setWidget(m_poseWidget);
    m_posePage = poseScroll;
    m_assetTabs->addTab(m_posePage, QStringLiteral("Pose"));
    m_assetTabs->setTabVisible(m_assetTabs->indexOf(m_posePage), false);
    connect(m_assetTabs, &QTabWidget::currentChanged, this, [this](int index){ m_savedAssetTab = index; });

    connect(applyBtn, &QPushButton::clicked, this, [this](){
        int row = m_exprList->currentRow();
        if(row >= 0 && row < m_exprEntries.size()){
            emit ApplyExpression(m_exprEntries[row].name);
        }
    });

    connect(m_exprList, &QListWidget::itemDoubleClicked, this, [this](){
        int row = m_exprList->currentRow();
        if(row >= 0 && row < m_exprEntries.size()){
            emit ApplyExpression(m_exprEntries[row].name);
        }
    });

    connect(m_editExpressionButton, &QPushButton::clicked, this, &CMotionPanel::EditSelectedExpression);
    connect(m_exprList, &QListWidget::currentRowChanged, this, [this, applyBtn](int row){
        m_editExpressionButton->setEnabled(row >= 0 && row < m_exprEntries.size());
        applyBtn->setEnabled(row >= 0 && row < m_exprEntries.size());
    });

    connect(m_motionSearch, &QLineEdit::textChanged, this, [this](const QString& text){
        const QString query = text.trimmed().toLower();
        for (int i = 0; i < m_motionList->count(); ++i)
            m_motionList->item(i)->setHidden(!query.isEmpty() && !m_motionList->item(i)->text().toLower().contains(query));
    });
    connect(m_exprSearch, &QLineEdit::textChanged, this, [this](const QString& text){
        const QString query = text.trimmed().toLower();
        for (int i = 0; i < m_exprList->count(); ++i)
            m_exprList->item(i)->setHidden(!query.isEmpty() && !m_exprList->item(i)->text().toLower().contains(query));
    });

    mainLayout->addWidget(m_workspaceSplitter, 1);

    auto* timelineBox = new QGroupBox(QStringLiteral("动画时间轴"), wrapper);
    m_timelineBox = timelineBox;
    auto* timelineLayout = new QVBoxLayout(timelineBox);
    auto* timelineTools = new QHBoxLayout();
    m_recordTimelineCb = new QCheckBox(QStringLiteral("录制"), timelineBox);
    m_recordTimelineCb->setObjectName(QStringLiteral("recordTimeline"));
    auto* clearTimelineButton = new QPushButton(QStringLiteral("清空"), timelineBox);
    auto* removeTimelineButton = new QPushButton(QStringLiteral("删除"), timelineBox);
    removeTimelineButton->setToolTip(QStringLiteral("删除选中的关键帧"));
    auto* addTimelineButton = new QPushButton(QStringLiteral("添加"), timelineBox);
    addTimelineButton->setObjectName(QStringLiteral("addTimelineKeyframe"));
    addTimelineButton->setToolTip(QStringLiteral("添加参数、模型或 Part 关键帧"));
    connect(addTimelineButton, &QPushButton::clicked, this, &CMotionPanel::AddTimelineKeyframe);
    auto* exportTimelineButton = new QPushButton(QStringLiteral("导出"), timelineBox);
    auto* importTimelineButton = new QPushButton(QStringLiteral("导入"), timelineBox);
    exportTimelineButton->setToolTip(QStringLiteral("导出 motion3 动作文件"));
    importTimelineButton->setToolTip(QStringLiteral("导入 motion3 动作文件"));
    m_playTimelineButton = new QPushButton(QStringLiteral("预览"), timelineBox);
    m_playTimelineButton->setObjectName(QStringLiteral("previewTimeline"));
    m_playTimelineButton->setEnabled(false);
    m_playTimelineButton->setToolTip(QStringLiteral("从播放头预览动作；勾选“混合”可查看参数淡入淡出，Part 直接显示透明度"));
    timelineTools->addWidget(m_playTimelineButton);
    timelineTools->addWidget(m_recordTimelineCb);
    m_endPreviewButton = new QPushButton(QStringLiteral("还原"), timelineBox);
    m_endPreviewButton->setObjectName(QStringLiteral("endTimelinePreview"));
    m_endPreviewButton->setToolTip(QStringLiteral("结束曲线预览并恢复原姿态"));
    m_endPreviewButton->setEnabled(false);
    timelineTools->addWidget(m_endPreviewButton);
    connect(m_endPreviewButton, &QPushButton::clicked, this, &CMotionPanel::StopTimelinePreview);
    timelineTools->addStretch(1);
    timelineTools->addWidget(importTimelineButton);
    timelineTools->addWidget(exportTimelineButton);
    timelineLayout->addLayout(timelineTools);
    auto* editTools = new QHBoxLayout();
    m_undoTimelineButton = new QPushButton(QStringLiteral("撤销"), timelineBox);
    m_redoTimelineButton = new QPushButton(QStringLiteral("重做"), timelineBox);
    m_undoTimelineButton->setObjectName(QStringLiteral("undoMotionTimeline"));
    m_redoTimelineButton->setObjectName(QStringLiteral("redoMotionTimeline"));
    removeTimelineButton->setObjectName(QStringLiteral("removeTimelineKeyframe"));
    editTools->addWidget(m_undoTimelineButton);
    editTools->addWidget(m_redoTimelineButton);
    editTools->addWidget(addTimelineButton);
    editTools->addWidget(removeTimelineButton);
    editTools->addWidget(clearTimelineButton);
    m_previewFades = new QCheckBox(QStringLiteral("混合"), timelineBox);
    m_previewFades->setObjectName(QStringLiteral("previewMotionFades"));
    m_previewFades->setAccessibleName(QStringLiteral("预览参数淡入淡出"));
    m_previewFades->setToolTip(QStringLiteral("将参数曲线按动作与曲线淡入淡出混合到基础姿态；Part 仍直接预览透明度"));
    editTools->addWidget(m_previewFades);
    editTools->addStretch(1);
    connect(m_previewFades, &QCheckBox::toggled, this, [this](){
        const QScopedValueRollback<bool> mutation(m_timelineMutation, true);
        if (m_transport) { RestoreTransportControls(); return; }
        ++m_timelineRevision;
        emit TimelineActivated();
        EmitTimelinePreview();
    });
    auto* settingsButton = new QPushButton(QStringLiteral("设置"), timelineBox);
    settingsButton->setObjectName(QStringLiteral("editMotionSettings"));
    settingsButton->setToolTip(QStringLiteral("编辑事件、帧率、时长和淡入淡出"));
    timelineTools->addWidget(settingsButton);
    connect(settingsButton, &QPushButton::clicked, this, &CMotionPanel::EditTimelineSettings);
    m_timelineStatus = new QLabel(QStringLiteral("暂无关键帧"), timelineBox);
    m_targetFilter = new QComboBox(timelineBox);
    m_targetFilter->setObjectName(QStringLiteral("motionTargetFilter"));
    m_targetFilter->setAccessibleName(QStringLiteral("曲线类型筛选"));
    m_targetFilter->addItem(QStringLiteral("全部曲线"), -1);
    m_targetFilter->addItem(QStringLiteral("参数"), static_cast<int>(CMotionTarget::Parameter));
    m_targetFilter->addItem(QStringLiteral("模型"), static_cast<int>(CMotionTarget::Model));
    m_targetFilter->addItem(QStringLiteral("Part 透明度"), static_cast<int>(CMotionTarget::PartOpacity));

    timelineLayout->addLayout(editTools);
    connect(m_undoTimelineButton, &QPushButton::clicked, this, &CMotionPanel::UndoTimeline);
    connect(m_redoTimelineButton, &QPushButton::clicked, this, &CMotionPanel::RedoTimeline);
    m_curveSplitter = new QSplitter(Qt::Vertical, timelineBox);
    m_curveSplitter->setObjectName(QStringLiteral("animationCurveSplitter"));
    m_curveSplitter->setChildrenCollapsible(false);
    m_curveView = new CMotionCurveView(m_curveSplitter);
    m_timelineTable = new QTableWidget(0, 4, timelineBox);
    m_timelineTable->setObjectName(QStringLiteral("motionTimelineTable"));
    m_timelineDelegate = new CTableEditDelegate(m_timelineTable);
    m_timelineTable->setItemDelegate(m_timelineDelegate);
    m_timelineTable->setHorizontalHeaderLabels({QStringLiteral("时间"), QStringLiteral("曲线"), QStringLiteral("值"), QStringLiteral("插值")});
    m_timelineTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_timelineTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_timelineTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_timelineTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_timelineTable->verticalHeader()->setVisible(false);
    m_timelineTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_timelineTable->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed |
                                     QAbstractItemView::SelectedClicked);
    m_detailTabs = new QTabWidget(m_curveSplitter);
    m_detailTabs->setObjectName(QStringLiteral("animationDetailTabs"));
    m_detailTabs->setTabBarAutoHide(true);
    m_detailTabs->addTab(m_timelineTable, QStringLiteral("关键帧"));
    m_eventTable = new QTableWidget(0, 2, m_detailTabs);
    m_eventTable->setObjectName(QStringLiteral("timelineEventList"));
    m_eventTable->setHorizontalHeaderLabels({QStringLiteral("时间"), QStringLiteral("事件")});
    m_eventTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_eventTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_eventTable->verticalHeader()->setVisible(false);
    m_eventTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_eventTable->installEventFilter(this);
    m_eventTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_eventTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_eventTable->setToolTip(QStringLiteral("双击事件或按 Enter 定位；在“设置”中编辑事件"));
    m_detailTabs->addTab(m_eventTable, QStringLiteral("事件"));
    m_detailTabs->setTabVisible(1, false);
    connect(m_eventTable, &QTableWidget::cellActivated, this, [this](int row, int){ ActivateTimelineEvent(row); });
    m_curveSplitter->setStretchFactor(0, 1);
    m_curveSplitter->setStretchFactor(1, 1);
    timelineLayout->addWidget(m_curveSplitter, 1);
    connect(m_targetFilter, &QComboBox::currentIndexChanged, this, [this](){
        if (IsPreviewTransportSuspended()) return;
        m_curveView->SetTargetFilter(m_targetFilter->currentData().toInt());
        RefreshTimelineTable();
    });
    m_timelinePlayhead = new QSlider(Qt::Horizontal, timelineBox);
    m_timelinePlayhead->setObjectName(QStringLiteral("motionTimelinePlayhead"));
    m_timelinePlayhead->setAccessibleName(QStringLiteral("时间轴播放头"));
    m_timelinePlayhead->setToolTip(QStringLiteral("拖动播放头预览当前时间的曲线"));
    m_timelinePlayhead->setRange(0, 1000);
    m_timelinePlayhead->setEnabled(false);
    m_timelineTimeLabel = new QLabel(QStringLiteral("0.000s"), timelineBox);
    auto* playheadRow = new QHBoxLayout();
    playheadRow->addWidget(m_timelineStatus);
    playheadRow->addWidget(m_targetFilter);
    playheadRow->addWidget(m_timelinePlayhead, 1);
    playheadRow->addWidget(m_timelineTimeLabel);
    timelineLayout->addLayout(playheadRow);
    m_eventStatus = new QLabel(timelineBox);
    m_eventStatus->setObjectName(QStringLiteral("timelineEventStatus"));
    m_eventStatus->setTextFormat(Qt::PlainText);
    m_eventStatus->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_eventStatus->hide();
    timelineLayout->addWidget(m_eventStatus);
    m_workspaceSplitter->addWidget(timelineBox);
    m_workspaceSplitter->setStretchFactor(0, 0);
    m_workspaceSplitter->setStretchFactor(1, 1);
    connect(m_recordTimelineCb, &QCheckBox::toggled, this, [this](bool recording){
        const QScopedValueRollback<bool> mutation(m_timelineMutation, true);
        if (m_transport) { RestoreTransportControls(); return; }
        if (!m_restoringTimeline) {
            ++m_timelineRevision;
            emit TimelineActivated();
        }
        if (recording) {
            StopTimelinePreview();
            if (!m_restoringTimeline) {
                m_curveView->CancelDrag();
                BeginTimelineEditGroup(QStringLiteral("录制参数"));
            }
            m_timeline.StartRecording();
            m_timelineStatus->setText(QStringLiteral("正在录制"));
        } else {
            m_timeline.StopRecording();
            if (!m_restoringTimeline) EndTimelineEditGroup();
            m_timelineStatus->setText(m_timeline.TotalPointCount() > 0
                                      ? QStringLiteral("已录制 %1 个关键帧").arg(m_timeline.TotalPointCount())
                                      : QStringLiteral("暂无关键帧"));
        }
        m_timelineTable->setEnabled(!recording);
        m_curveView->setEnabled(!recording);
    });
    connect(m_recordTimelineCb, &QCheckBox::toggled, addTimelineButton, &QPushButton::setDisabled);
    connect(clearTimelineButton, &QPushButton::clicked, this, &CMotionPanel::ClearTimeline);
    connect(removeTimelineButton, &QPushButton::clicked, this, &CMotionPanel::RemoveTimelineKeyframe);
    connect(exportTimelineButton, &QPushButton::clicked, this, &CMotionPanel::ExportTimeline);
    connect(importTimelineButton, &QPushButton::clicked, this, &CMotionPanel::ImportTimeline);
    connect(m_timelineTable, &QTableWidget::cellChanged, this, &CMotionPanel::ApplyTimelineCellEdit);
    connect(m_timelineTable, &QTableWidget::currentCellChanged, this, [this](int row, int, int, int){
        if (!m_curveView || row < 0 || row >= m_timelineRows.size()) return;
        const TimelineRow& selected = m_timelineRows.at(row);
        m_curveView->SetSelectedPoint(selected.track, selected.pointIndex);
    });
    connect(m_curveView, &CMotionCurveView::PointSelected, this, [this](const CMotionTrack& track, int pointIndex){
        if (!m_timelineTable) return;
        for (int row = 0; row < m_timelineRows.size(); ++row) {
            const TimelineRow& candidate = m_timelineRows.at(row);
            if (candidate.track == track && candidate.pointIndex == pointIndex) {
                m_timelineTable->selectRow(row);
                return;
            }
        }
    });
    connect(m_curveView, &CMotionCurveView::EventSelected, this, [this](double time, const QString& value){
        if (IsPreviewTransportSuspended()) return;
        SeekTimeline(time);
        m_eventStatus->setText(QStringLiteral("定位 %1s：%2").arg(time, 0, 'g', 8).arg(value));
    });
    connect(m_curveView, &CMotionCurveView::PointEdited, this, &CMotionPanel::ApplyTimelineCurveEdit);
    connect(m_curveView, &CMotionCurveView::EditStarted, this, [this](){
        if (IsPreviewTransportSuspended()) return;
        emit TimelineActivated();
        BeginTimelineEditGroup(QStringLiteral("移动关键帧"));
    });
    connect(m_curveView, &CMotionCurveView::EditFinished, this, &CMotionPanel::EndTimelineEditGroup);
    connect(m_timelinePlayhead, &QSlider::valueChanged, this, &CMotionPanel::SetTimelineTimeFromSlider);
    m_timelineTimer = new QTimer(this);
    m_timelineTimer->setInterval(33);
    connect(m_playTimelineButton, &QPushButton::clicked, this, &CMotionPanel::ToggleTimelinePlayback);
    connect(m_timelineTimer, &QTimer::timeout, this, &CMotionPanel::TickTimelinePlayback);
    connect(this, &CMotionPanel::MotionTimelineChanged, this, [this](){
        if (IsPreviewTransportSuspended() || m_refreshingParameterTracks) return;
        ++m_timelineRevision;
        if (!m_restoringTimeline) RefreshTimelineHistory();
        if (m_recordTimelineCb && m_recordTimelineCb->isChecked()) return;
        EmitTimelinePreview();
    });

    // 选项
    QHBoxLayout* optLayout = new QHBoxLayout();
    m_eyeBlinkCb = new QCheckBox("自动眨眼");
    m_eyeBlinkCb->setChecked(true);
    m_breathCb = new QCheckBox("自动呼吸");
    m_breathCb->setChecked(true);
    m_mouseFollowCb = new QCheckBox("鼠标追踪");
    m_mouseFollowCb->setChecked(false);
    m_physicsCb = new QCheckBox("物理模拟");
    m_physicsCb->setChecked(true);
    m_loopCb = new QCheckBox(QStringLiteral("动作循环"));
    m_speedSlider = new QSlider(Qt::Horizontal, wrapper);
    m_speedSlider->setObjectName(QStringLiteral("motionPlaybackSpeed"));
    m_speedSlider->setRange(25, 200);
    m_speedSlider->setValue(100);
    m_speedSlider->setToolTip(QStringLiteral("动作播放速度：25%–200%"));
    m_speedLabel = new QLabel(QStringLiteral("100%"), wrapper);
    m_speedLabel->setMinimumWidth(42);
    optLayout->addWidget(m_eyeBlinkCb);
    optLayout->addWidget(m_breathCb);
    optLayout->addWidget(m_mouseFollowCb);
    optLayout->addWidget(m_physicsCb);
    optLayout->addWidget(m_loopCb);
    optLayout->addWidget(new QLabel(QStringLiteral("速度"), wrapper));
    optLayout->addWidget(m_speedSlider, 1);
    optLayout->addWidget(m_speedLabel);
    mainLayout->addLayout(optLayout);

    connect(m_eyeBlinkCb, &QCheckBox::toggled, this, [this](bool enabled) {
        if (m_transport) { RestoreTransportControls(); return; }
        emit AutoEyeBlinkChanged(enabled);
    });
    connect(m_breathCb, &QCheckBox::toggled, this, [this](bool enabled) {
        if (m_transport) { RestoreTransportControls(); return; }
        emit AutoBreathChanged(enabled);
    });
    connect(m_mouseFollowCb, &QCheckBox::toggled, this, [this](bool enabled) {
        if (m_transport) { RestoreTransportControls(); return; }
        emit MouseFollowChanged(enabled);
    });
    connect(m_physicsCb, &QCheckBox::toggled, this, [this](bool enabled) {
        if (m_transport) { RestoreTransportControls(); return; }
        emit PhysicsChanged(enabled);
    });
    connect(m_loopCb, &QCheckBox::toggled, this, [this](bool loop){
        const QScopedValueRollback<bool> mutation(m_timelineMutation, true);
        if (m_transport) { RestoreTransportControls(); return; }
        emit MotionPlaybackChanged(m_speedSlider->value() / 100.0f, loop);
        if (!m_restoringTimeline && m_timeline.HasAnimation()) {
            auto before = CurrentTimelineSnapshot();
            before.loop = !loop;
            emit TimelineActivated();
            PushTimelineEdit(before, QStringLiteral("修改循环设置"));
            emit MotionTimelineChanged();
        }
    });
    connect(m_speedSlider, &QSlider::valueChanged, this, [this](int value){
        const QScopedValueRollback<bool> mutation(m_timelineMutation, true);
        if (m_transport) { RestoreTransportControls(); return; }
        TickTimelinePlayback();
        m_timelinePlaybackSpeed = value / 100.0;
        m_speedLabel->setText(QStringLiteral("%1%").arg(value));
        emit MotionPlaybackChanged(value / 100.0f, m_loopCb->isChecked());
    });

    clearTimelineButton->setObjectName(QStringLiteral("clearMotionTimeline"));
    exportTimelineButton->setObjectName(QStringLiteral("exportMotionTimeline"));
    importTimelineButton->setObjectName(QStringLiteral("importMotionTimeline"));
    m_loopCb->setObjectName(QStringLiteral("motionLoop"));
    connect(m_timelineHistory, &QUndoStack::indexChanged, this, &CMotionPanel::RefreshTimelineHistory);
    connect(m_timelineHistory, &QUndoStack::cleanChanged, this, &CMotionPanel::RefreshTimelineHistory);
    m_savedTimelineContent = CurrentTimelineSnapshot();
    RefreshTimelineHistory();
    setWidget(wrapper);
}

CMotionPanel::~CMotionPanel()
{
    m_timelineTimer->stop();
    // 子控件销毁会发出信号，须在派生成员释放前断开。
    for (QObject* child : findChildren<QObject*>()) QObject::disconnect(child, nullptr, this, nullptr);
    delete m_timelineHistory;
}

bool CMotionPanel::BuildLists(const QString& modelJsonPath, const QStringList& partIds, QString* error)
{
    if (!RequireTransportAvailable(error)) return false;
    QFile file(modelJsonPath);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = file.errorString();
        return false;
    }
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject() ||
        !document.object().value(QStringLiteral("FileReferences")).isObject()) {
        if (error) *error = QStringLiteral("model3 动作列表无效");
        return false;
    }
    const auto references = document.object().value(QStringLiteral("FileReferences")).toObject();
    m_modelPartIds = QSet<QString>(partIds.cbegin(), partIds.cend());
    m_modelPartsKnown = true;
    m_motionList->clear();
    m_exprList->clear();
    m_motionEntries.clear();
    m_exprEntries.clear();
    m_editExpressionButton->setEnabled(false);
    const auto groups = references.value(QStringLiteral("Motions")).toObject();
    for (auto group = groups.begin(); group != groups.end(); ++group) {
        const auto motions = group.value().toArray();
        for (int index = 0; index < motions.size(); ++index) {
            m_motionEntries.append({group.key(), index});
            m_motionList->addItem(QStringLiteral("%1 [%2]").arg(group.key()).arg(index));
        }
    }
    const QDir directory = QFileInfo(modelJsonPath).absoluteDir();
    for (const auto& expression : references.value(QStringLiteral("Expressions")).toArray()) {
        const auto entry = expression.toObject();
        const QString name = entry.value(QStringLiteral("Name")).toString();
        if (name.isEmpty()) continue;
        const QString path = entry.value(QStringLiteral("File")).toString();
        m_exprEntries.append({name, path.isEmpty() ? QString() : directory.filePath(path)});
        m_exprList->addItem(name);
    }
    if (m_motionEntries.isEmpty()) m_motionList->addItem(QStringLiteral("(此模型无动作定义)"));
    if (m_exprEntries.isEmpty()) m_exprList->addItem(QStringLiteral("(此模型无表情定义)"));
    return true;
}

void CMotionPanel::RecordParameterValue(const QString& parameterId, float value)
{
    const QScopedValueRollback<bool> mutation(m_timelineMutation, true);
    if (IsPreviewTransportSuspended()) return;
    if (!m_timeline.IsRecording() || (m_timelineTimer && m_timelineTimer->isActive())) return;
    const int before = m_timeline.TotalPointCount();
    m_timeline.Record(parameterId, value);
    if (m_timeline.TotalPointCount() != before) {
        RefreshTimelineTable();
        m_timelineStatus->setText(QStringLiteral("正在录制 · %1 个关键帧").arg(m_timeline.TotalPointCount()));
        emit MotionTimelineChanged();
    }
}

void CMotionPanel::RefreshTimelineTable()
{
    if (!m_timelineTable) return;
    for (auto* table : {m_timelineTable, m_eventTable})
        table->setMinimumHeight(table->horizontalHeader()->sizeHint().height() +
                                2 * table->verticalHeader()->defaultSectionSize() + 2 * table->frameWidth());
    const double duration = m_timeline.Timing().duration;
    m_timelineTime = qBound(0.0, m_timelineTime, duration);
    if (m_timelinePlayhead) {
        const QSignalBlocker blocker(m_timelinePlayhead);
        m_timelinePlayhead->setValue(duration > 0.0 ? qRound(m_timelineTime * 1000.0 / duration) : 0);
    }
    QVector<TimelineRow> points;
    for (auto it = m_timeline.Tracks().cbegin(); it != m_timeline.Tracks().cend(); ++it) {
        if (m_targetFilter->currentData().toInt() >= 0 &&
            m_targetFilter->currentData().toInt() != static_cast<int>(it.key().target)) continue;
        for (int index = 0; index < it.value().size(); ++index)
            points.append({it.key(), index, it.value().at(index), m_timeline.SegmentType(it.key(), index)});
    }
    std::sort(points.begin(), points.end(), [](const auto& left, const auto& right){
        if (left.point.x() != right.point.x()) return left.point.x() < right.point.x();
        return left.track < right.track;
    });
    m_timelineRows = points;
    if (m_curveView) {
        m_curveView->SetTimeline(&m_timeline);
        m_curveView->SetTime(m_timelineTime);
    }
    RefreshTimelineEvents();
    if (m_playTimelineButton) m_playTimelineButton->setEnabled(m_timeline.HasAnimation() || (m_timelineTimer && m_timelineTimer->isActive()));
    if (m_timelinePlayhead) m_timelinePlayhead->setEnabled(m_timeline.HasAnimation());
    if (m_timelineTimeLabel) m_timelineTimeLabel->setText(QStringLiteral("%1s").arg(m_timelineTime, 0, 'f', 3));
    m_updatingTimeline = true;
    m_timelineTable->setRowCount(points.size());
    for (int row = 0; row < points.size(); ++row) {
        m_timelineTable->setItem(row, 0, new QTableWidgetItem(QString::number(points[row].point.x(), 'f', 3)));
        auto* parameter = new QTableWidgetItem(points[row].track.TargetName() + QStringLiteral(" · ") + points[row].track.id);
        parameter->setFlags(parameter->flags() & ~Qt::ItemIsEditable);
        m_timelineTable->setItem(row, 1, parameter);
        m_timelineTable->setItem(row, 2, new QTableWidgetItem(QString::number(points[row].point.y(), 'g', 6)));
        auto* interpolation = new QTableWidgetItem(points[row].pointIndex == 0
            ? QStringLiteral("起点") : points[row].segmentType == 1 ? QStringLiteral("Bézier")
            : points[row].segmentType == 2 ? QStringLiteral("阶梯")
            : points[row].segmentType == 3 ? QStringLiteral("反阶梯") : QStringLiteral("线性"));
        if (points[row].pointIndex == 0) interpolation->setFlags(interpolation->flags() & ~Qt::ItemIsEditable);
        interpolation->setToolTip(QStringLiteral("0 线性，1 Bézier，2 阶梯，3 反阶梯"));
        m_timelineTable->setItem(row, 3, interpolation);
    }
    m_updatingTimeline = false;
}

bool CMotionPanel::eventFilter(QObject* watched, QEvent* event)
{
    if (IsPreviewTransportSuspended() && event->type() == QEvent::KeyPress) return true;
    if (watched == m_eventTable && event->type() == QEvent::KeyPress) {
        const auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) {
            ActivateTimelineEvent(m_eventTable->currentRow());
            return true;
        }
    }
    return QDockWidget::eventFilter(watched, event);
}

void CMotionPanel::ActivateTimelineEvent(int row)
{
    if (IsPreviewTransportSuspended()) return;
    if (row < 0 || row >= m_displayedEvents.size()) return;
    const auto event = m_displayedEvents.at(row).toObject();
    const double time = event.value(QStringLiteral("Time")).toDouble();
    SeekTimeline(time);
    m_eventStatus->setText(QStringLiteral("定位 %1s：%2").arg(time, 0, 'g', 8)
                           .arg(event.value(QStringLiteral("Value")).toString()));
}

void CMotionPanel::RefreshTimelineEvents()
{
    const auto events = m_timeline.Events();
    if (events == m_displayedEvents) {
        m_displayedEvents = events;
        return;
    }
    m_displayedEvents = events;
    m_eventStartPending = true;
    m_eventTable->setRowCount(events.size());
    for (int row = 0; row < events.size(); ++row) {
        const auto event = events.at(row).toObject();
        m_eventTable->setItem(row, 0, new QTableWidgetItem(QString::number(event.value(QStringLiteral("Time")).toDouble(), 'g', 12)));
        m_eventTable->setItem(row, 1, new QTableWidgetItem(event.value(QStringLiteral("Value")).toString()));
    }
    m_detailTabs->setTabVisible(1, !events.isEmpty());
    m_detailTabs->setTabText(1, QStringLiteral("事件 (%1)").arg(events.size()));
    m_eventStatus->setVisible(!events.isEmpty());
    m_eventStatus->setText(QStringLiteral("播放时显示事件；双击列表可定位"));
    m_eventStatus->setToolTip(QString());
    if (!m_timeline.HasCurves() && !events.isEmpty()) m_detailTabs->setCurrentIndex(1);
}

void CMotionPanel::ShowTimelineEvents(const CMotionEventBatch& events)
{
    if (IsPreviewTransportSuspended()) return;
    if (!events.valid || events.occurrences.isEmpty()) return;
    const auto& last = events.occurrences.last();
    QString message = QStringLiteral("事件 %1s：%2").arg(last.time, 0, 'g', 8).arg(last.value.left(160));
    if (events.countSaturated) message += QStringLiteral("（大量事件已省略）");
    else if (events.omittedCount) message += QStringLiteral("（另 %1 条未显示）").arg(events.omittedCount);
    m_eventStatus->setText(message);
    QStringList details;
    for (const auto& event : events.occurrences)
        details.append(QStringLiteral("%1s：%2").arg(event.time, 0, 'g', 12).arg(event.value.left(1000)));
    if (events.omittedCount) details.append(events.countSaturated
        ? QStringLiteral("还有大量重复事件未列出")
        : QStringLiteral("另有 %1 条事件未列出").arg(events.omittedCount));
    m_eventStatus->setToolTip(QStringLiteral("<qt>%1</qt>").arg(details.join(QLatin1Char('\n')).toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>"))));
    if (last.sourceIndex >= 0 && last.sourceIndex < m_eventTable->rowCount()) {
        m_eventTable->selectRow(last.sourceIndex);
        m_eventTable->scrollToItem(m_eventTable->item(last.sourceIndex, 0));
    }
    emit TimelineEventsTriggered(events);
}

void CMotionPanel::ToggleTimelinePlayback()
{
    const QScopedValueRollback<bool> mutation(m_timelineMutation, true);
    if (IsPreviewTransportSuspended()) return;
    if (!m_timelineTimer || (!m_timeline.HasAnimation() && !m_timelineTimer->isActive())) return;
    ++m_timelineRevision;
    emit TimelineActivated();
    if (m_timelineTimer->isActive()) {
        TickTimelinePlayback();
        m_timelineTimer->stop();
        m_playTimelineButton->setText(QStringLiteral("预览"));
        return;
    }
    m_recordTimelineCb->setChecked(false);
    if (m_timelineTime >= m_timeline.Timing().duration) {
        m_timelineTime = 0.0;
        m_eventStartPending = true;
    }
    m_playbackClock.start();
    m_playbackLastNs = 0;
    m_timelineTimer->start();
    m_playTimelineButton->setText(QStringLiteral("暂停"));
    if (m_eventStartPending) {
        ShowTimelineEvents(m_timeline.EventsBetween(m_timelineTime, 0, m_loopCb->isChecked(), true));
        m_eventStartPending = false;
    }
    if (m_timelinePlayhead) {
        QSignalBlocker blocker(m_timelinePlayhead);
        const double duration = m_timeline.Timing().duration;
        m_timelinePlayhead->setValue(duration > 0.0 ? qRound(m_timelineTime * 1000.0 / duration) : 0);
    }
    if (m_timelineTimeLabel) m_timelineTimeLabel->setText(QStringLiteral("%1s").arg(m_timelineTime, 0, 'f', 3));
    if (m_curveView) m_curveView->SetTime(m_timelineTime);
    EmitTimelinePreview();
}

void CMotionPanel::TickTimelinePlayback()
{
    const QScopedValueRollback<bool> mutation(m_timelineMutation, true);
    if (IsPreviewTransportSuspended()) return;
    if (!m_timelineTimer || !m_timelineTimer->isActive() || !m_playbackClock.isValid()) return;
    const double duration = m_timeline.Timing().duration;
    if (duration <= 0.0f) {
        m_timelineTimer->stop();
        m_playTimelineButton->setText(QStringLiteral("预览"));
        return;
    }
    const qint64 now = m_playbackClock.nsecsElapsed();
    const double advance = (now - m_playbackLastNs) / 1e9 * m_timelinePlaybackSpeed;
    const auto events = m_timeline.EventsBetween(m_timelineTime, advance, m_loopCb->isChecked());
    m_timelineTime += advance;
    m_playbackLastNs = now;
    if (m_timelineTime >= duration) {
        if (m_loopCb && m_loopCb->isChecked()) m_timelineTime = std::fmod(m_timelineTime, duration);
        else {
            m_timelineTime = duration;
            m_timelineTimer->stop();
            m_playTimelineButton->setText(QStringLiteral("预览"));
        }
    }
    if (m_timelinePlayhead) {
        QSignalBlocker blocker(m_timelinePlayhead);
        m_timelinePlayhead->setValue(duration > 0.0f
            ? qBound(0, qRound(static_cast<double>(m_timelineTime) * 1000.0 / duration), 1000) : 0);
    }
    if (m_timelineTimeLabel) m_timelineTimeLabel->setText(QStringLiteral("%1s").arg(m_timelineTime, 0, 'f', 3));
    if (m_curveView) m_curveView->SetTime(m_timelineTime);
    ShowTimelineEvents(events);
    EmitTimelinePreview();
}

void CMotionPanel::EmitTimelinePreview()
{
    const QScopedValueRollback<bool> mutation(m_timelineMutation, true);
    if (IsPreviewTransportSuspended()) return;
    const auto frame = m_timeline.EvaluateFrame(m_timelineTime);
    m_endPreviewButton->setEnabled(m_timeline.HasAnimation());
    emit TimelineFrameChanged(frame);
    emit MotionTimelinePreviewChanged(frame.parameters);
}

void CMotionPanel::PauseTimelinePreview()
{
    if (IsPreviewTransportSuspended()) return;
    ++m_timelineRevision;
    m_timelineTimer->stop();
    m_playTimelineButton->setText(QStringLiteral("预览"));
}

void CMotionPanel::StopTimelinePreview()
{
    if (IsPreviewTransportSuspended()) return;
    PauseTimelinePreview();
    m_endPreviewButton->setEnabled(false);
    emit TimelinePreviewStopped();
}

void CMotionPanel::SetTimelinePreviewAvailable(bool available)
{
    if (IsPreviewTransportSuspended()) return;
    m_endPreviewButton->setEnabled(available);
}

bool CMotionPanel::AddTimelinePoint(const CMotionTrack& track, float time, float value)
{
    const QScopedValueRollback<bool> mutation(m_timelineMutation, true);
    if (!RequireTransportAvailable()) return false;
    if (m_timeline.IsRecording()) return false;
    m_timelineDelegate->CommitEditor();
    const auto before = CurrentTimelineSnapshot();
    if (!m_timeline.InsertPoint(track, time, value)) return false;
    emit TimelineActivated();
    PushTimelineEdit(before, QStringLiteral("添加关键帧"));
    if (m_targetFilter->currentData().toInt() >= 0 &&
        m_targetFilter->currentData().toInt() != static_cast<int>(track.target))
        m_targetFilter->setCurrentIndex(m_targetFilter->findData(static_cast<int>(track.target)));
    RefreshTimelineTable();
    for (int row = 0; row < m_timelineRows.size(); ++row) {
        const auto& candidate = m_timelineRows.at(row);
        if (candidate.track == track && m_timeline.Tracks().value(track).at(candidate.pointIndex).x() == time) {
            m_timelineTable->selectRow(row);
            m_timelineTable->scrollToItem(m_timelineTable->item(row, 0));
            break;
        }
    }
    emit MotionTimelineChanged();
    return true;
}

void CMotionPanel::EditTimelineSettings()
{
    const QScopedValueRollback<bool> mutation(m_timelineMutation, true);
    if (IsPreviewTransportSuspended()) return;
    m_timelineDelegate->CommitEditor();
    m_recordTimelineCb->setChecked(false);
    TickTimelinePlayback();
    m_timelineTimer->stop();
    m_playTimelineButton->setText(QStringLiteral("预览"));
    const auto before = CurrentTimelineSnapshot();
    CMotionSettingsDialog dialog(m_timeline, m_timelineTime, this);
    if (dialog.exec() != QDialog::Accepted || m_timeline.HasSameContent(dialog.Timeline())) return;
    m_timeline = dialog.Timeline();
    m_timelineTime = qMin(m_timelineTime, m_timeline.Timing().duration);
    emit TimelineActivated();
    PushTimelineEdit(before, QStringLiteral("修改动作设置"));
    RefreshTimelineTable();
    emit MotionTimelineChanged();
}

void CMotionPanel::AddTimelineKeyframe()
{
    const QScopedValueRollback<bool> mutation(m_timelineMutation, true);
    if (IsPreviewTransportSuspended()) return;
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("添加关键帧"));
    auto* layout = new QFormLayout(&dialog);
    auto* target = new QComboBox(&dialog);
    target->setObjectName(QStringLiteral("keyframeTarget"));
    target->addItem(QStringLiteral("参数"), static_cast<int>(CMotionTarget::Parameter));
    target->addItem(QStringLiteral("模型"), static_cast<int>(CMotionTarget::Model));
    target->addItem(QStringLiteral("Part 透明度"), static_cast<int>(CMotionTarget::PartOpacity));
    auto* id = new QComboBox(&dialog);
    id->setObjectName(QStringLiteral("keyframeId"));
    auto* time = new QDoubleSpinBox(&dialog);
    time->setObjectName(QStringLiteral("keyframeTime"));
    time->setDecimals(6);
    time->setRange(0, std::numeric_limits<float>::max());
    time->setSingleStep(1.0 / m_timeline.Fps());
    time->setValue(m_timelineTime);
    auto* value = new QDoubleSpinBox(&dialog);
    value->setObjectName(QStringLiteral("keyframeValue"));
    value->setDecimals(6);
    value->setRange(-std::numeric_limits<float>::max(), std::numeric_limits<float>::max());
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addRow(QStringLiteral("类型"), target);
    layout->addRow(QStringLiteral("ID"), id);
    layout->addRow(QStringLiteral("时间（秒）"), time);
    layout->addRow(QStringLiteral("数值"), value);
    layout->addRow(buttons);
    const auto populate = [this, target, id, buttons](){
        const auto kind = static_cast<CMotionTarget>(target->currentData().toInt());
        QStringList ids = kind == CMotionTarget::Parameter ? m_parameterIds : kind == CMotionTarget::PartOpacity
            ? m_modelPartIds.values() : QStringList{"Opacity", "EyeBlink", "LipSync"};
        for (const auto& track : m_timeline.Tracks().keys()) if (track.target == kind) ids.append(track.id);
        ids.removeDuplicates();
        ids.sort();
        id->clear();
        id->addItems(ids);
        buttons->button(QDialogButtonBox::Ok)->setEnabled(!ids.isEmpty());
    };
    const auto defaultValue = [this, target, id, time, value](){
        const CMotionTrack track{static_cast<CMotionTarget>(target->currentData().toInt()), id->currentText()};
        bool valid = false;
        const float current = m_timeline.Evaluate(track, time->value(), &valid);
        value->setValue(valid ? current : track.target == CMotionTarget::PartOpacity ||
                        (track.target == CMotionTarget::Model && track.id != QLatin1String("LipSync")) ? 1.0 : 0.0);
    };
    connect(target, &QComboBox::currentIndexChanged, &dialog, populate);
    connect(id, &QComboBox::currentTextChanged, &dialog, defaultValue);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    populate();
    if (dialog.exec() != QDialog::Accepted) return;
    if (!AddTimelinePoint({static_cast<CMotionTarget>(target->currentData().toInt()), id->currentText()},
                          time->value(), value->value()))
        QMessageBox::warning(this, QStringLiteral("无法添加关键帧"), QStringLiteral("时间点重复或关键帧数值无效"));
}

void CMotionPanel::SetTimelineTimeFromSlider(int value)
{
    if (m_transport) { RestoreTransportControls(); return; }
    if (!m_timelinePlayhead || !m_timeline.HasAnimation()) return;
    SeekTimeline(m_timeline.Timing().duration * qBound(0, value, 1000) / 1000.0);
}

void CMotionPanel::SeekTimeline(double time)
{
    const QScopedValueRollback<bool> mutation(m_timelineMutation, true);
    if (IsPreviewTransportSuspended()) return;
    if (!std::isfinite(time) || !m_timeline.HasAnimation()) return;
    m_recordTimelineCb->setChecked(false);
    ++m_timelineRevision;
    emit TimelineActivated();
    m_timelineTimer->stop();
    m_playTimelineButton->setText(QStringLiteral("预览"));
    const double duration = m_timeline.Timing().duration;
    m_timelineTime = qBound(0.0, time, duration);
    m_eventStartPending = true;
    m_eventStatus->setText(QStringLiteral("播放时显示事件；双击列表可定位"));
    m_eventStatus->setToolTip(QString());
    {
        const QSignalBlocker blocker(m_timelinePlayhead);
        m_timelinePlayhead->setValue(duration > 0 ? qRound(m_timelineTime * 1000.0 / duration) : 0);
    }
    m_timelineTimeLabel->setText(QStringLiteral("%1s").arg(m_timelineTime, 0, 'f', 3));
    m_curveView->SetTime(m_timelineTime);
    EmitTimelinePreview();
}

void CMotionPanel::ApplyTimelineCurveEdit(const CMotionTrack& track, int pointIndex, float time, float value)
{
    const QScopedValueRollback<bool> mutation(m_timelineMutation, true);
    if (IsPreviewTransportSuspended()) return;
    if (m_updatingTimeline) return;
    const auto before = CurrentTimelineSnapshot();
    if (!m_timeline.UpdatePoint(track, pointIndex, time, value)) return;
    emit TimelineActivated();
    PushTimelineEdit(before, QStringLiteral("移动关键帧"));
    RefreshTimelineTable();
    emit MotionTimelineChanged();
}

void CMotionPanel::ApplyTimelineCellEdit(int row, int column)
{
    const QScopedValueRollback<bool> mutation(m_timelineMutation, true);
    if (IsPreviewTransportSuspended()) return;
    if (m_updatingTimeline || row < 0 || row >= m_timelineRows.size() ||
        (column != 0 && column != 2 && column != 3)) return;
    const TimelineRow reference = m_timelineRows.at(row);
    const auto before = CurrentTimelineSnapshot();
    if (column == 3) {
        const QString text = m_timelineTable->item(row, column)
            ? m_timelineTable->item(row, column)->text().trimmed().toLower() : QString();
        int segmentType = -1;
        if (text == QStringLiteral("0") || text == QStringLiteral("线性") || text == QStringLiteral("linear")) segmentType = 0;
        else if (text == QStringLiteral("1") || text == QStringLiteral("bezier") || text == QStringLiteral("bézier")) segmentType = 1;
        else if (text == QStringLiteral("2") || text == QStringLiteral("阶梯") || text == QStringLiteral("stepped")) segmentType = 2;
        else if (text == QStringLiteral("3") || text == QStringLiteral("反阶梯") || text == QStringLiteral("inverse stepped")) segmentType = 3;
        if (segmentType < 0 || !m_timeline.SetSegmentType(reference.track, reference.pointIndex, segmentType)) {
            RefreshTimelineTable();
            return;
        }
        emit TimelineActivated();
        PushTimelineEdit(before, QStringLiteral("修改插值"));
        RefreshTimelineTable();
        emit MotionTimelineChanged();
        return;
    }
    const float oldValue = column == 0 ? static_cast<float>(reference.point.x())
                                       : static_cast<float>(reference.point.y());
    bool ok = false;
    const float value = m_timelineTable->item(row, column)
        ? static_cast<float>(m_timelineTable->item(row, column)->text().toDouble(&ok)) : oldValue;
    if (!ok || !std::isfinite(value) ||
        !m_timeline.UpdatePoint(reference.track, reference.pointIndex,
                                column == 0 ? value : static_cast<float>(reference.point.x()),
                                column == 2 ? value : static_cast<float>(reference.point.y()))) {
        RefreshTimelineTable();
        return;
    }
    emit TimelineActivated();
    PushTimelineEdit(before, QStringLiteral("编辑关键帧"));
    RefreshTimelineTable();
    emit MotionTimelineChanged();
}

void CMotionPanel::RemoveTimelineKeyframe()
{
    const QScopedValueRollback<bool> mutation(m_timelineMutation, true);
    if (IsPreviewTransportSuspended()) return;
    if (!m_timelineTable || m_timelineTable->currentRow() < 0 ||
        m_timelineTable->currentRow() >= m_timelineRows.size()) return;
    m_timelineDelegate->CommitEditor();
    if (m_timelineTable->currentRow() < 0 || m_timelineTable->currentRow() >= m_timelineRows.size()) return;
    const TimelineRow reference = m_timelineRows.at(m_timelineTable->currentRow());
    const auto before = CurrentTimelineSnapshot();
    if (!m_timeline.RemovePoint(reference.track, reference.pointIndex)) return;
    emit TimelineActivated();
    PushTimelineEdit(before, QStringLiteral("删除关键帧"));
    RefreshTimelineTable();
    m_timelineStatus->setText(m_timeline.TotalPointCount() > 0
                              ? QStringLiteral("已录制 %1 个关键帧").arg(m_timeline.TotalPointCount())
                              : QStringLiteral("暂无关键帧"));
    emit MotionTimelineChanged();
}

void CMotionPanel::ClearTimeline()
{
    const QScopedValueRollback<bool> mutation(m_timelineMutation, true);
    if (IsPreviewTransportSuspended()) return;
    if (!ConfirmTimelineReplacement()) return;
    if (m_timelineTimer) m_timelineTimer->stop();
    if (m_playTimelineButton) m_playTimelineButton->setText(QStringLiteral("预览"));
    if (m_recordTimelineCb) m_recordTimelineCb->setChecked(false);
    m_curveView->CancelDrag();
    const auto before = CurrentTimelineSnapshot();
    m_timeline.Clear();
    m_timelineTime = 0.0f;
    {
        const QSignalBlocker blocker(m_timelinePlayhead);
        m_timelinePlayhead->setValue(0);
    }
    RefreshTimelineTable();
    if (m_timelineStatus) m_timelineStatus->setText(QStringLiteral("暂无关键帧"));
    emit TimelineActivated();
    PushTimelineEdit(before, QStringLiteral("清空动画"));
    emit MotionTimelineChanged();
}

bool CMotionPanel::HasUnsavedTimelineChanges() const
{
    return (m_savedTimelineContent && !m_savedTimelineContent->SameContent(CurrentTimelineSnapshot())) ||
        PendingTimelineEdit() || m_timelineDelegate->HasPendingChanges();
}

void CMotionPanel::SetTimelineDirty(bool dirty)
{
    if (IsPreviewTransportSuspended()) return;
    if (m_timelineDirty == dirty) return;
    m_timelineDirty = dirty;
    emit TimelineDirtyChanged(dirty);
}

void CMotionPanel::MarkTimelineSaved()
{
    if (IsPreviewTransportSuspended()) return;
    m_timelineDelegate->CommitEditor();
    FlushTimelineEditGroup();
    m_savedTimelineContent = CurrentTimelineSnapshot();
    m_timelineHistory->setClean();
    RefreshTimelineHistory();
}

bool CMotionPanel::CaptureTimelineState(QJsonObject* state, QString* error)
{
    if (m_transport) return m_timeline.SaveState(state, m_transport->loop, m_transport->time, error);
    m_timelineDelegate->CommitEditor();
    return m_timeline.SaveState(state, m_loopCb->isChecked(), m_timelineTime, error);
}

bool CMotionPanel::RestoreTimelineState(const QJsonObject& state, QString* error)
{
    if (!RequireTransportAvailable(error)) return false;
    CMotionTimeline candidate;
    if (!candidate.RestoreState(state, nullptr, error)) return false;
    const double playhead = state.value(QStringLiteral("playhead")).toDouble();
    m_restoringTimeline = true;
    m_editGroupActive = false;
    m_curveView->CancelDrag();
    ++m_historyGeneration;
    ++m_registryRevision;
    m_timelineSnapshots.clear();
    m_parameterTrackArchives.clear();
    m_currentTrackOrigins.clear();
    m_editGroupBefore.reset();
    m_parameterTrackUiPending = false;
    m_timelineHistory->clear();
    m_timelineTimer->stop();
    m_recordTimelineCb->setChecked(false);
    m_timeline = std::move(candidate);
    m_eventStartPending = true;
    m_timelineTime = playhead;
    if (!state.isEmpty()) m_loopCb->setChecked(m_timeline.Loop());
    m_playTimelineButton->setText(QStringLiteral("预览"));
    {
        const QSignalBlocker blocker(m_timelinePlayhead);
        const double duration = m_timeline.Timing().duration;
        m_timelinePlayhead->setValue(duration > 0.0 ? qRound(playhead * 1000.0 / duration) : 0);
    }
    RefreshTimelineTable();
    m_timelineStatus->setText(QStringLiteral("动画草稿已恢复"));
    emit MotionTimelineChanged();
    MarkTimelineSaved();
    m_restoringTimeline = false;
    return true;
}

CMotionPanel::TimelineSnapshot CMotionPanel::CurrentTimelineSnapshot() const
{
    TimelineSnapshot state{m_timeline, m_loopCb->isChecked(), m_timelineTime};
    state.timeline.StopRecording();
    return state;
}

bool CMotionPanel::PendingTimelineEdit() const
{
    return m_editGroupActive && m_editGroupBefore && !m_editGroupBefore->value.SameContent(CurrentTimelineSnapshot());
}

bool CMotionPanel::CanUndoTimeline() const
{
    return m_timelineHistory->canUndo() || PendingTimelineEdit() || m_timelineDelegate->HasPendingChanges();
}

bool CMotionPanel::CanRedoTimeline() const
{
    return m_timelineHistory->canRedo() && !PendingTimelineEdit() && !m_timelineDelegate->HasPendingChanges();
}

bool CMotionPanel::IsTimelineWidget(const QWidget* widget) const
{
    return widget && (widget == m_loopCb || widget == m_timelineBox || m_timelineBox->isAncestorOf(widget));
}

QByteArray CMotionPanel::SaveLayout() const
{
    return QJsonDocument(QJsonObject{
        {QStringLiteral("assets"), QString::fromLatin1(m_workspaceSplitter->saveState().toBase64())},
        {QStringLiteral("curves"), QString::fromLatin1(m_curveSplitter->saveState().toBase64())},
        {QStringLiteral("tab"), m_assetTabs->currentIndex()},
        {QStringLiteral("fades"), m_previewFades->isChecked()}
    }).toJson(QJsonDocument::Compact);
}

void CMotionPanel::RestoreLayout(const QByteArray& state)
{
    if (IsPreviewTransportSuspended()) return;
    const auto object = QJsonDocument::fromJson(state).object();
    if (object.isEmpty()) return;
    m_workspaceSplitter->restoreState(QByteArray::fromBase64(object.value(QStringLiteral("assets")).toString().toLatin1()));
    m_curveSplitter->restoreState(QByteArray::fromBase64(object.value(QStringLiteral("curves")).toString().toLatin1()));
    m_previewFades->setChecked(object.value(QStringLiteral("fades")).toBool(false));
    const int tab = object.value(QStringLiteral("tab")).toInt(-1);
    if (tab < 0 || tab >= m_assetTabs->count()) return;
    m_savedAssetTab = tab;
    if (m_assetTabs->isTabVisible(tab)) m_assetTabs->setCurrentIndex(tab);
}

void CMotionPanel::RefreshTimelineHistory()
{
    if (IsPreviewTransportSuspended()) return;
    SetTimelineDirty((m_savedTimelineContent && !m_savedTimelineContent->SameContent(CurrentTimelineSnapshot())) || PendingTimelineEdit());
    m_undoTimelineButton->setEnabled(CanUndoTimeline());
    m_redoTimelineButton->setEnabled(CanRedoTimeline());
    m_undoTimelineButton->setToolTip(PendingTimelineEdit() ? m_editGroupLabel : m_timelineHistory->undoText());
    m_redoTimelineButton->setToolTip(m_timelineHistory->redoText());
    emit TimelineHistoryChanged(CanUndoTimeline(), CanRedoTimeline());
}

void CMotionPanel::PushTimelineEdit(const TimelineSnapshot& before, const QString& label)
{
    if (IsPreviewTransportSuspended()) return;
    if (m_restoringTimeline || m_editGroupActive) return;
    const auto after = CurrentTimelineSnapshot();
    if (before.SameContent(after)) return;
    const auto previous = RegisterTimelineSnapshot(before);
    const auto next = RegisterTimelineSnapshot(after);
    m_timelineHistory->push(new CTimelineEdit(label, [this, previous, next](bool forward){
        const auto& selected = forward ? next : previous;
        m_currentTrackOrigins = selected->origins;
        ApplyTimelineSnapshot(selected->value);
    }));
}

void CMotionPanel::PushTimelineEdit(const std::shared_ptr<TimelineSnapshotSlot>& before, const QString& label)
{
    if (!before || IsPreviewTransportSuspended() || m_restoringTimeline || m_editGroupActive) return;
    const auto after = CurrentTimelineSnapshot();
    if (before->value.SameContent(after)) return;
    const auto next = RegisterTimelineSnapshot(after);
    m_timelineHistory->push(new CTimelineEdit(label, [this, before, next](bool forward) {
        const auto& selected = forward ? next : before;
        m_currentTrackOrigins = selected->origins;
        ApplyTimelineSnapshot(selected->value);
    }));
}

void CMotionPanel::BeginTimelineEditGroup(const QString& label)
{
    const QScopedValueRollback<bool> mutation(m_timelineMutation, true);
    if (IsPreviewTransportSuspended()) return;
    EndTimelineEditGroup();
    m_editGroupBefore = RegisterTimelineSnapshot(CurrentTimelineSnapshot());
    m_editGroupLabel = label;
    m_editGroupActive = true;
}

void CMotionPanel::EndTimelineEditGroup()
{
    const QScopedValueRollback<bool> mutation(m_timelineMutation, true);
    if (IsPreviewTransportSuspended()) return;
    if (!m_editGroupActive) return;
    m_editGroupActive = false;
    PushTimelineEdit(m_editGroupBefore, m_editGroupLabel);
    m_editGroupBefore.reset();
    RefreshTimelineHistory();
}

void CMotionPanel::FlushTimelineEditGroup()
{
    if (IsPreviewTransportSuspended()) return;
    if (!m_editGroupActive) return;
    const auto label = m_editGroupLabel;
    EndTimelineEditGroup();
    BeginTimelineEditGroup(label);
}

void CMotionPanel::ApplyTimelineSnapshot(const TimelineSnapshot& snapshot)
{
    if (IsPreviewTransportSuspended()) return;
    m_restoringTimeline = true;
    m_timelineTimer->stop();
    m_recordTimelineCb->setChecked(false);
    m_timeline = snapshot.timeline;
    m_eventStartPending = true;
    m_loopCb->setChecked(snapshot.loop);
    m_timelineTime = qBound(0.0, snapshot.playhead, m_timeline.Timing().duration);
    m_playTimelineButton->setText(QStringLiteral("预览"));
    {
        const QSignalBlocker blocker(m_timelinePlayhead);
        const double duration = m_timeline.Timing().duration;
        m_timelinePlayhead->setValue(duration > 0.0 ? qRound(m_timelineTime * 1000.0 / duration) : 0);
    }
    RefreshTimelineTable();
    m_timelineStatus->setText(m_timeline.TotalPointCount() > 0
        ? QStringLiteral("%1 个关键帧").arg(m_timeline.TotalPointCount())
        : !m_timeline.Events().isEmpty() ? QStringLiteral("%1 个事件").arg(m_timeline.Events().size()) : QStringLiteral("暂无关键帧"));
    emit MotionTimelineChanged();
    m_restoringTimeline = false;
}

void CMotionPanel::UndoTimeline()
{
    const QScopedValueRollback<bool> mutation(m_timelineMutation, true);
    if (IsPreviewTransportSuspended()) return;
    emit TimelineActivated();
    m_timelineDelegate->CommitEditor();
    m_recordTimelineCb->setChecked(false);
    m_curveView->CancelDrag();
    EndTimelineEditGroup();
    m_timelineHistory->undo();
    RefreshTimelineHistory();
}

void CMotionPanel::RedoTimeline()
{
    const QScopedValueRollback<bool> mutation(m_timelineMutation, true);
    if (IsPreviewTransportSuspended()) return;
    emit TimelineActivated();
    m_timelineDelegate->CommitEditor();
    m_recordTimelineCb->setChecked(false);
    m_curveView->CancelDrag();
    EndTimelineEditGroup();
    m_timelineHistory->redo();
    RefreshTimelineHistory();
}

bool CMotionPanel::ConfirmTimelineReplacement()
{
    if (!RequireTransportAvailable()) return false;
    m_timelineDelegate->CommitEditor();
    if (!HasUnsavedTimelineChanges()) return true;
    QMessageBox prompt(this);
    prompt.setWindowTitle(QStringLiteral("动画草稿未保存"));
    prompt.setText(QStringLiteral("替换当前时间轴前，是否保存工作会话？"));
    prompt.setStandardButtons(QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
    prompt.button(QMessageBox::Save)->setText(QStringLiteral("保存会话"));
    prompt.button(QMessageBox::Discard)->setText(QStringLiteral("放弃修改"));
    prompt.button(QMessageBox::Cancel)->setText(QStringLiteral("取消"));
    prompt.setDefaultButton(QMessageBox::Save);
    const auto result = prompt.exec();
    if (result == QMessageBox::Discard) return true;
    if (result != QMessageBox::Save) return false;
    emit SaveSessionRequested();
    return !HasUnsavedTimelineChanges();
}

bool CMotionPanel::SaveTimeline(const QString& path, QString* error)
{
    if (!RequireTransportAvailable(error)) return false;
    m_timelineDelegate->CommitEditor();
    if (!m_timeline.Save(path, m_timeline.Fps(), m_loopCb->isChecked(), error)) return false;
    MarkTimelineSaved();
    m_timelineStatus->setText(QStringLiteral("已导出 motion3"));
    return true;
}

void CMotionPanel::ExportTimeline()
{
    if (IsPreviewTransportSuspended()) return;
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("导出 motion3"),
                                                       QString(), QStringLiteral("Motion (*.motion3.json)"));
    if (path.isEmpty()) return;
    QString error;
    if (!SaveTimeline(path, &error)) QMessageBox::warning(this, QStringLiteral("导出 motion3 失败"), error);
}

bool CMotionPanel::LoadTimeline(const QString& path, QString* error)
{
    if (!RequireTransportAvailable(error)) return false;
    CMotionTimeline candidate;
    if (!candidate.Load(path, error)) return false;
    if (!ConfirmTimelineReplacement()) return false;
    QJsonObject state;
    if (!candidate.SaveState(&state, candidate.Loop(), 0, error)) return false;
    if (!RestoreTimelineState(state, error)) return false;
    emit TimelineActivated();
    m_timelineStatus->setText(m_timeline.TotalPointCount() > 0
        ? QStringLiteral("已导入 %1 个关键帧").arg(m_timeline.TotalPointCount())
        : QStringLiteral("已导入 %1 个事件").arg(m_timeline.Events().size()));
    return true;
}

void CMotionPanel::ImportTimeline()
{
    if (IsPreviewTransportSuspended()) return;
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("导入 motion3"),
                                                       QString(), QStringLiteral("Motion (*.motion3.json)"));
    if (path.isEmpty()) return;
    QString error;
    if (!LoadTimeline(path, &error) && !error.isEmpty())
        QMessageBox::warning(this, QStringLiteral("导入 motion3 失败"), error);
}

bool CMotionPanel::BuildPoseGroups(const QString& poseJsonPath, QString* error)
{
    if (!RequireTransportAvailable(error)) return false;
    QJsonObject candidate;
    if (!poseJsonPath.isEmpty()) {
        QFile file(poseJsonPath);
        if (!file.open(QIODevice::ReadOnly)) {
            if (error) *error = QStringLiteral("无法打开 Pose 配置：%1").arg(file.errorString());
            return false;
        }
        QJsonParseError parseError;
        const auto document = QJsonDocument::fromJson(file.readAll(), &parseError);
        if (file.error() != QFileDevice::NoError || parseError.error != QJsonParseError::NoError ||
            !document.isObject() || !document.object().value(QStringLiteral("Groups")).isArray()) {
            if (error) *error = QStringLiteral("Pose 配置 JSON 或 Groups 字段无效");
            return false;
        }
        candidate = document.object();
        const auto fade = candidate.value(QStringLiteral("FadeInTime"));
        if (!fade.isUndefined() && (!fade.isDouble() || !std::isfinite(fade.toDouble()) ||
            fade.toDouble() < 0.0 || fade.toDouble() > std::numeric_limits<float>::max())) {
            if (error) *error = QStringLiteral("Pose 淡入时间无效");
            return false;
        }
        for (const auto& group : candidate.value(QStringLiteral("Groups")).toArray()) {
            if (!group.isArray()) {
                if (error) *error = QStringLiteral("Pose 组必须是数组");
                return false;
            }
            for (const auto& entry : group.toArray()) {
                const auto object = entry.toObject();
                const auto links = object.value(QStringLiteral("Link"));
                if (!entry.isObject() || object.value(QStringLiteral("Id")).toString().trimmed().isEmpty() ||
                    (!links.isUndefined() && !links.isArray())) {
                    if (error) *error = QStringLiteral("Pose Part 或 Link 字段无效");
                    return false;
                }
                for (const auto& link : links.toArray()) {
                    if (!link.isString() || link.toString().trimmed().isEmpty()) {
                        if (error) *error = QStringLiteral("Pose Link ID 无效");
                        return false;
                    }
                }
            }
        }
    }
    m_poseJsonPath = poseJsonPath;
    m_poseDocument = {};
    m_poseGroups.clear();
    m_activePoseGroup = -1;
    m_poseDirty = false;
    m_savePoseButton->setText(QStringLiteral("保存 Pose"));
    m_addPosePartButton->setEnabled(false);
    m_removePosePartButton->setEnabled(false);
    m_editPoseLinksButton->setEnabled(false);
    m_savePoseButton->setEnabled(false);
    // 清除 poseWidget 中旧的 combo
    QLayout* layout = m_poseWidget->layout();
    while(layout->count() > 2){
        QLayoutItem* item = layout->takeAt(2);
        if (QWidget* widget = item->widget()) {
            delete widget;
        } else if (QLayout* child = item->layout()) {
            clearLayoutItems(child);
            delete child;
        }
        delete item;
    }

    if(poseJsonPath.isEmpty()){
        m_assetTabs->setTabVisible(m_assetTabs->indexOf(m_posePage), false);
        return true;
    }

    m_poseDocument = candidate;
    QJsonArray groups = candidate.value(QStringLiteral("Groups")).toArray();
    if(groups.isEmpty()){
        m_assetTabs->setTabVisible(m_assetTabs->indexOf(m_posePage), false);
        return true;
    }

    for(int g = 0; g < groups.size(); g++){
        QJsonArray group = groups[g].toArray();
        if(group.size() < 2) continue; // 只有一个选项没意义

        PoseGroup pg;
        pg.sourceIndex = g;
        QComboBox* combo = new QComboBox();
        combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        combo->setMinimumContentsLength(10);
        for(int i = 0; i < group.size(); i++){
            const QJsonObject part = group[i].toObject();
            QString partId = part["Id"].toString();
            if(partId.isEmpty()) continue;
            pg.partIds << partId;
            QStringList links;
            for (const QJsonValue& link : part["Link"].toArray()) {
                const QString linkId = link.toString();
                if (!linkId.isEmpty() && linkId != partId && !links.contains(linkId)) links.append(linkId);
            }
            pg.links.append(links);
            combo->addItem(QString("组%1: %2").arg(g + 1).arg(partId), partId);
        }
        if (pg.partIds.size() < 2) {
            delete combo;
            continue;
        }
        pg.combo = combo;
        m_poseGroups.append(pg);
        const int poseIndex = m_poseGroups.size() - 1;
        layout->addWidget(combo);

        connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, poseIndex](int index){
            if(poseIndex >= m_poseGroups.size()) return;
            m_activePoseGroup = poseIndex;
            m_addPosePartButton->setEnabled(true);
            m_removePosePartButton->setEnabled(m_poseGroups[poseIndex].partIds.size() > 1);
            m_editPoseLinksButton->setEnabled(true);
            EmitPoseSelection(poseIndex, index);
        });
    }

    const int poseTab = m_assetTabs->indexOf(m_posePage);
    m_assetTabs->setTabVisible(poseTab, !m_poseGroups.isEmpty());
    if (!m_poseGroups.isEmpty() && m_savedAssetTab == poseTab) m_assetTabs->setCurrentIndex(poseTab);
    m_savePoseButton->setEnabled(!m_poseGroups.isEmpty());
    if (!m_poseGroups.isEmpty()) {
        m_activePoseGroup = 0;
        m_addPosePartButton->setEnabled(true);
        m_removePosePartButton->setEnabled(m_poseGroups[0].partIds.size() > 1);
        m_editPoseLinksButton->setEnabled(true);
    }
    return true;
}

void CMotionPanel::EmitPoseSelection(int poseIndex, int optionIndex)
{
    if (IsPreviewTransportSuspended()) return;
    if (poseIndex < 0 || poseIndex >= m_poseGroups.size()) return;
    const PoseGroup& group = m_poseGroups.at(poseIndex);
    if (optionIndex < 0 || optionIndex >= group.partIds.size()) return;
    QStringList hideIds;
    const QString showId = group.partIds.at(optionIndex);
    QStringList showLinks = optionIndex < group.links.size() ? group.links.at(optionIndex) : QStringList();
    for (int i = 0; i < group.partIds.size(); ++i) {
        if (i == optionIndex) continue;
        hideIds << group.partIds.at(i);
        if (i < group.links.size()) hideIds.append(group.links.at(i));
    }
    hideIds.removeDuplicates();
    showLinks.removeDuplicates();
    hideIds.removeAll(showId);
    for (const QString& link : showLinks) hideIds.removeAll(link);
    QStringList showIds{showId};
    showIds.append(showLinks);
    showIds.removeDuplicates();
    emit PosePartChanged(showIds, hideIds);
}

void CMotionPanel::EditPoseLinks()
{
    if (IsPreviewTransportSuspended()) return;
    if (m_activePoseGroup < 0 || m_activePoseGroup >= m_poseGroups.size()) return;
    PoseGroup& group = m_poseGroups[m_activePoseGroup];
    const int optionIndex = group.combo ? group.combo->currentIndex() : -1;
    if (optionIndex < 0 || optionIndex >= group.partIds.size()) return;
    const QString currentId = group.partIds.at(optionIndex);

    QStringList candidates;
    QSet<QString> candidateSet;
    for (const QString& id : m_modelPartIds) {
        candidateSet.insert(id);
        candidates.append(id);
    }
    for (const QString& id : group.partIds) if (!candidateSet.contains(id)) { candidateSet.insert(id); candidates.append(id); }
    for (const QStringList& links : group.links)
        for (const QString& id : links) if (!candidateSet.contains(id)) { candidateSet.insert(id); candidates.append(id); }
    candidates.sort(Qt::CaseInsensitive);

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("编辑 Link：%1").arg(currentId));
    dialog.resize(360, 420);
    auto* layout = new QVBoxLayout(&dialog);
    auto* list = new QListWidget(&dialog);
    const QStringList currentLinks = optionIndex < group.links.size() ? group.links.at(optionIndex) : QStringList();
    for (const QString& id : candidates) {
        if (id == currentId) continue;
        auto* item = new QListWidgetItem(id, list);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(currentLinks.contains(id) ? Qt::Checked : Qt::Unchecked);
    }
    layout->addWidget(list);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return;

    QStringList links;
    for (int row = 0; row < list->count(); ++row) {
        QListWidgetItem* item = list->item(row);
        if (item && item->checkState() == Qt::Checked) links.append(item->text());
    }
    links.removeDuplicates();
    if (group.links.size() < group.partIds.size()) group.links.resize(group.partIds.size());
    if (group.links.at(optionIndex) == links) return;
    group.links[optionIndex] = links;
    m_savePoseButton->setText(QStringLiteral("保存 Pose"));
    m_poseDirty = true;
    emit PoseConfigChanged();
    EmitPoseSelection(m_activePoseGroup, optionIndex);
}

bool CMotionPanel::SavePose(QString* error)
{
    if (m_poseJsonPath.isEmpty() || m_poseDocument.isEmpty()) {
        if (error) *error = QStringLiteral("没有可保存的 Pose 配置");
        return false;
    }
    QJsonArray groups = m_poseDocument.value(QStringLiteral("Groups")).toArray();
    bool modelAvailable = false;
    for (const PoseGroup& group : m_poseGroups) {
        for (int i = 0; i < group.partIds.size(); ++i) {
            if (!ModelHasPart(group.partIds.at(i), &modelAvailable)) {
                if (!modelAvailable) break;
                if (error) *error = QStringLiteral("Pose 包含不存在的 Part：%1").arg(group.partIds.at(i));
                return false;
            }
            if (i >= group.links.size()) continue;
            for (const QString& link : group.links.at(i)) {
                if (link == group.partIds.at(i)) {
                    if (error) *error = QStringLiteral("Pose Link 不能指向自身：%1").arg(link);
                    return false;
                }
                if (!ModelHasPart(link, &modelAvailable)) {
                    if (!modelAvailable) break;
                    if (error) *error = QStringLiteral("Pose 包含不存在的 Link：%1").arg(link);
                    return false;
                }
            }
            if (!modelAvailable) break;
        }
        if (!modelAvailable && !m_poseGroups.isEmpty()) break;
    }
    for (const PoseGroup& group : m_poseGroups) {
        QJsonArray entries;
        for (int i = 0; i < group.partIds.size(); ++i) {
            QJsonObject entry;
            entry.insert(QStringLiteral("Id"), group.partIds[i]);
            if (i < group.links.size() && !group.links[i].isEmpty()) {
                QJsonArray links;
                for (const QString& link : group.links[i]) links.append(link);
                entry.insert(QStringLiteral("Link"), links);
            }
            entries.append(entry);
        }
        if (group.sourceIndex >= 0 && group.sourceIndex < groups.size())
            groups[group.sourceIndex] = entries;
        else
            groups.append(entries);
    }
    m_poseDocument.insert(QStringLiteral("Groups"), groups);
    const QByteArray data = QJsonDocument(m_poseDocument).toJson(QJsonDocument::Indented);
    QSaveFile file(m_poseJsonPath);
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) {
        if (error) *error = QStringLiteral("无法写入文件：%1").arg(file.errorString());
        return false;
    }
    m_savePoseButton->setText(QStringLiteral("已保存 Pose"));
    m_poseDirty = false;
    emit PoseSaved();
    return true;
}

void CMotionPanel::AddPosePart()
{
    if (IsPreviewTransportSuspended()) return;
    if (m_activePoseGroup < 0 || m_activePoseGroup >= m_poseGroups.size()) return;
    bool accepted = false;
    const QString partId = QInputDialog::getText(this, QStringLiteral("添加 Pose Part"),
                                                  QStringLiteral("Part ID"), QLineEdit::Normal,
                                                  QString(), &accepted).trimmed();
    if (!accepted || partId.isEmpty()) return;
    bool modelAvailable = false;
    if (!ModelHasPart(partId, &modelAvailable)) {
        QMessageBox::warning(this, QStringLiteral("添加 Pose Part"),
                             modelAvailable ? QStringLiteral("当前模型不存在该 Part") : QStringLiteral("模型尚未准备好"));
        return;
    }
    PoseGroup& group = m_poseGroups[m_activePoseGroup];
    if (group.partIds.contains(partId)) {
        QMessageBox::information(this, QStringLiteral("添加 Pose Part"), QStringLiteral("该 Part 已在当前组中"));
        return;
    }
    group.partIds.append(partId);
    group.links.append(QStringList());
    group.combo->addItem(partId, partId);
    group.combo->setCurrentIndex(group.combo->count() - 1);
    m_removePosePartButton->setEnabled(group.partIds.size() > 1);
    m_savePoseButton->setText(QStringLiteral("保存 Pose"));
    m_poseDirty = true;
    emit PoseConfigChanged();
}

void CMotionPanel::RemovePosePart()
{
    if (IsPreviewTransportSuspended()) return;
    if (m_activePoseGroup < 0 || m_activePoseGroup >= m_poseGroups.size()) return;
    PoseGroup& group = m_poseGroups[m_activePoseGroup];
    const int index = group.combo->currentIndex();
    if (index < 0 || index >= group.partIds.size() || group.partIds.size() <= 1) return;
    group.partIds.removeAt(index);
    if (index < group.links.size()) group.links.removeAt(index);
    group.combo->removeItem(index);
    m_removePosePartButton->setEnabled(group.partIds.size() > 1);
    m_savePoseButton->setText(QStringLiteral("保存 Pose"));
    m_poseDirty = true;
    emit PoseConfigChanged();
}

void CMotionPanel::LoadLipSyncWav()
{
    if (IsPreviewTransportSuspended()) return;
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("加载口型 WAV"),
                                                       QString(), QStringLiteral("WAV 音频 (*.wav)"));
    if (path.isEmpty()) return;
    emit LipSyncWavChanged(path);
}

void CMotionPanel::StopLipSyncWav()
{
    if (IsPreviewTransportSuspended()) return;
    emit LipSyncWavChanged(QString());
}

void CMotionPanel::SetLipSyncWavStatus(bool loaded, const QString& path)
{
    if (IsPreviewTransportSuspended()) return;
    if (!m_lipSyncWavStatus) return;
    m_lipSyncWavStatus->setText(loaded
                                ? QStringLiteral("口型：%1").arg(QFileInfo(path).fileName())
                                : QStringLiteral("口型：未加载"));
}

void CMotionPanel::EditSelectedExpression()
{
    if (IsPreviewTransportSuspended()) return;
    const int expressionIndex = m_exprList->currentRow();
    if (expressionIndex < 0 || expressionIndex >= m_exprEntries.size()) return;
    const ExpressionEntry entry = m_exprEntries.at(expressionIndex);
    if (entry.path.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("编辑表情失败"), QStringLiteral("当前表情没有有效文件路径"));
        return;
    }

    QFile file(entry.path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QMessageBox::warning(this, QStringLiteral("编辑表情失败"), file.errorString());
        return;
    }
    QJsonParseError parseError;
    const QJsonDocument source = QJsonDocument::fromJson(file.readAll(), &parseError);
    file.close();
    if (parseError.error != QJsonParseError::NoError || !source.isObject()) {
        QMessageBox::warning(this, QStringLiteral("编辑表情失败"),
                             QStringLiteral("Expression JSON 无效：%1").arg(parseError.errorString()));
        return;
    }
    const QJsonObject root = source.object();
    const QJsonValue parametersValue = root.value(QStringLiteral("Parameters"));
    if (!parametersValue.isArray()) {
        QMessageBox::warning(this, QStringLiteral("编辑表情失败"), QStringLiteral("Expression 缺少 Parameters 数组"));
        return;
    }
    const QJsonArray sourceParameters = parametersValue.toArray();

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("编辑表情：%1").arg(entry.name));
    dialog.resize(560, 420);
    auto* layout = new QVBoxLayout(&dialog);
    auto* table = new QTableWidget(&dialog);
    table->setColumnCount(3);
    table->setHorizontalHeaderLabels({QStringLiteral("参数 ID"), QStringLiteral("值"), QStringLiteral("Blend")});
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    table->verticalHeader()->setVisible(false);
    table->setRowCount(sourceParameters.size());
    const QStringList blendTypes = {QStringLiteral("Add"), QStringLiteral("Multiply"), QStringLiteral("Overwrite")};
    for (int row = 0; row < sourceParameters.size(); ++row) {
        const QJsonValue parameterValue = sourceParameters.at(row);
        if (!parameterValue.isObject()) {
            QMessageBox::warning(this, QStringLiteral("编辑表情失败"),
                                 QStringLiteral("Expression 参数 %1 不是对象").arg(row));
            return;
        }
        const QJsonObject parameter = parameterValue.toObject();
        const QString id = parameter.value(QStringLiteral("Id")).toString();
        const QJsonValue value = parameter.value(QStringLiteral("Value"));
        if (id.isEmpty() || !value.isDouble() || !std::isfinite(value.toDouble())) {
            QMessageBox::warning(this, QStringLiteral("编辑表情失败"),
                                 QStringLiteral("Expression 参数 %1 无效").arg(row));
            return;
        }
        auto* idItem = new QTableWidgetItem(id);
        idItem->setFlags(idItem->flags() & ~Qt::ItemIsEditable);
        table->setItem(row, 0, idItem);
        table->setItem(row, 1, new QTableWidgetItem(QString::number(value.toDouble(), 'g', 9)));
        auto* blend = new QComboBox(table);
        const QString blendType = parameter.value(QStringLiteral("Blend")).toString(QStringLiteral("Add"));
        if (!blendTypes.contains(blendType)) {
            QMessageBox::warning(this, QStringLiteral("编辑表情失败"),
                                 QStringLiteral("Expression 参数 %1 的 Blend 无效").arg(row));
            return;
        }
        blend->addItems(blendTypes);
        blend->setCurrentText(blendType);
        table->setCellWidget(row, 2, blend);
    }
    layout->addWidget(table);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return;

    QJsonArray parameters = sourceParameters;
    QSet<QString> ids;
    for (int row = 0; row < table->rowCount(); ++row) {
        const QString id = table->item(row, 0) ? table->item(row, 0)->text().trimmed() : QString();
        bool ok = false;
        const double value = table->item(row, 1) ? table->item(row, 1)->text().toDouble(&ok) : 0.0;
        auto* blend = qobject_cast<QComboBox*>(table->cellWidget(row, 2));
        if (id.isEmpty() || ids.contains(id) || !ok || !std::isfinite(value) || !blend) {
            QMessageBox::warning(this, QStringLiteral("保存表情失败"),
                                 QStringLiteral("第 %1 行的参数值或 ID 无效").arg(row + 1));
            return;
        }
        ids.insert(id);
        QJsonObject parameter = parameters.at(row).toObject();
        parameter.insert(QStringLiteral("Id"), id);
        parameter.insert(QStringLiteral("Value"), value);
        parameter.insert(QStringLiteral("Blend"), blend->currentText());
        parameters[row] = parameter;
    }

    QJsonObject updated = root;
    updated.insert(QStringLiteral("Parameters"), parameters);
    const QByteArray data = QJsonDocument(updated).toJson(QJsonDocument::Indented);
    QSaveFile output(entry.path);
    if (!output.open(QIODevice::WriteOnly) || output.write(data) != data.size() || !output.commit()) {
        QMessageBox::warning(this, QStringLiteral("保存表情失败"), output.errorString());
        return;
    }

    emit ExpressionSaved(entry.name, entry.path);
}
