#ifndef CMOTIONPANEL_H
#define CMOTIONPANEL_H

#include <QDockWidget>
#include <QElapsedTimer>
#include <QListWidget>
#include <QCheckBox>
#include <QComboBox>
#include <QSlider>
#include <QVBoxLayout>
#include <QLineEdit>
#include <QJsonObject>
#include <QMap>
#include <QSet>
#include <QPointer>
#include <memory>
#include "live2dstruct/cmotiontimeline.h"
#include "live2dstruct/conionskin.h"

class QLabel;
class QPushButton;
class QTableWidget;
class QTableView;
class CMotionKeyModel;
class QToolBar;
class QScrollArea;
class QAction;
class QTimer;
class CMotionCurveView;
class CTableEditDelegate;
class QUndoStack;
class QTabWidget;
class QSplitter;
class QSpinBox;
class QDoubleSpinBox;

class CMotionPanel : public QDockWidget
{
    Q_OBJECT
    class ScopedFlag
    {
    public:
        ScopedFlag(CMotionPanel* owner, bool& flag, bool value)
            : m_owner(owner), m_flag(&flag), m_before(flag) { flag = value; }
        ~ScopedFlag() { if (m_owner) *m_flag = m_before; }
        Q_DISABLE_COPY_MOVE(ScopedFlag)
    private:
        QPointer<CMotionPanel> m_owner;
        bool* m_flag;
        bool m_before;
    };
public:
    class PreviewTransportLease
    {
    public:
        PreviewTransportLease() = default;
        PreviewTransportLease(PreviewTransportLease&& other) noexcept;
        PreviewTransportLease& operator=(PreviewTransportLease&& other) noexcept;
        PreviewTransportLease(const PreviewTransportLease&) = delete;
        PreviewTransportLease& operator=(const PreviewTransportLease&) = delete;
        bool IsValid() const;
    private:
        friend class CMotionPanel;
        QPointer<CMotionPanel> m_owner;
        quint64 m_serial = 0;
    };

    class ParameterTrackArchive
    {
        friend class CMotionPanel;
    public:
        bool IsValid() const;
    private:
        struct Data;
        std::shared_ptr<const Data> m_data;
    };
    class PreparedParameterTracks
    {
        friend class CMotionPanel;
    public:
        PreparedParameterTracks();
        ~PreparedParameterTracks();
        PreparedParameterTracks(PreparedParameterTracks&&) noexcept;
        PreparedParameterTracks& operator=(PreparedParameterTracks&&) noexcept;
        PreparedParameterTracks(const PreparedParameterTracks&) = delete;
        PreparedParameterTracks& operator=(const PreparedParameterTracks&) = delete;
        bool IsValid() const;
        bool HasChanges() const;
        const CMotionTimeline* Timeline() const;
        bool Loop() const;
        double Time() const;
    private:
        struct Data;
        std::unique_ptr<Data> m_data;
    };
    bool PrepareParameterTrackRemoval(const QSet<QString>& ids, PreparedParameterTracks& result, QString* error = nullptr) const;
    bool PrepareParameterTrackRestoration(const ParameterTrackArchive& archive, PreparedParameterTracks& result, QString* error = nullptr) const;
    bool ValidatePreparedParameterTracks(const PreparedParameterTracks& prepared, QString* error = nullptr) const;
    bool CommitPreparedParameterTracks(PreparedParameterTracks& prepared, ParameterTrackArchive& archive) noexcept;
    void RefreshParameterTrackUi();

    explicit CMotionPanel(QWidget *parent = nullptr);
    ~CMotionPanel() override;
    bool BuildLists(const QString& modelJsonPath, const QStringList& partIds, QString* error = nullptr);
    bool CaptureTimelineState(QJsonObject* state, QString* error = nullptr);
    bool CaptureTimelineBytes(QByteArray& bytes, QString* error = nullptr) const;
    bool CaptureBakeTimeline(CMotionTimeline& result, quint64& revision, QString* error = nullptr) const;
    bool ApplyBakedParameterSamples(const CMotionTimeline& expected, quint64 expectedRevision,
                                   const QMap<QString, QVector<QPointF>>& samples, bool newAnimation,
                                   double fps, QString* error = nullptr, bool copyScene = false);
    bool RestoreTimelineState(const QJsonObject& state, QString* error = nullptr);
    bool HasUnsavedTimelineChanges() const;
    void MarkTimelineSaved();
    void UndoTimeline();
    void RedoTimeline();
    bool CanUndoTimeline() const;
    bool CanRedoTimeline() const;
    bool IsTimelineWidget(const QWidget* widget) const;
    QByteArray SaveLayout() const;
    void RestoreLayout(const QByteArray& state);
    quint64 TimelineRevision() const { return m_timelineRevision; }
    bool CaptureOnionSkinRequest(live2dstruct::COnionSkin::Request& request, QString* error = nullptr) const;
    void SetOnionSkinStatus(const live2dstruct::COnionSkin::Status& status);
    bool ConfirmTimelineReplacement();
    bool LoadTimeline(const QString& path, QString* error = nullptr);
    bool SaveTimeline(const QString& path, QString* error = nullptr);
    void SetParameterIds(const QStringList& ids) { if (!IsPreviewTransportSuspended()) m_parameterIds = ids; }
    bool AddTimelinePoint(const CMotionTrack& track, float time, float value);
    bool SuspendPreviewTransport(PreviewTransportLease& lease, QString* error = nullptr);
    bool ResumePreviewTransport(PreviewTransportLease& lease, QString* error = nullptr);
    bool IsPreviewTransportSuspended() const;
    using ParameterEditGuard = std::function<bool(QString*)>;
    void SetParameterEditGuard(ParameterEditGuard guard);
    class ParameterEditTransportSnapshot
    {
    public:
        bool IsValid() const;
    private:
        friend class CMotionPanel;
        struct Data;
        std::shared_ptr<Data> m_data;
    };
    bool CaptureParameterEditTransport(ParameterEditTransportSnapshot& snapshot, QString* error = nullptr) const;
    bool ValidateParameterEditTransport(const ParameterEditTransportSnapshot& snapshot, QString* error = nullptr) const;
    bool PauseParameterEditTransport(const ParameterEditTransportSnapshot& snapshot, QString* error = nullptr);
    bool RestoreParameterEditTransport(const ParameterEditTransportSnapshot& snapshot, QString* error = nullptr);
    void StopTimelinePreview();
    void PauseTimelinePreview();
    void SetTimelinePreviewAvailable(bool available);
    const CMotionTimeline& Timeline() const { return m_timeline; }
    double TimelineTime() const { return m_timelineTime; }
    bool UsesMotionFades() const { return m_previewFades->isChecked(); }
    bool TimelineLoop() const { return m_loopCb->isChecked(); }
    bool BuildPoseGroups(const QString& poseJsonPath, QString* error = nullptr);
    bool SavePose(QString* error = nullptr);
    bool CapturePoseBytes(QByteArray& bytes, QString* error = nullptr) const;
    const QString& PoseSourcePath() const { return m_poseJsonPath; }
    const QByteArray& PoseSourceDigest() const { return m_poseSourceDigest; }
    void RecordParameterValue(const QString& parameterId, float value);
    void RecordParameterValues(const QMap<QString, float>& values);
    bool HasUnsavedPoseChanges() const { return m_poseDirty; }
    void SetLipSyncWavStatus(bool loaded, const QString& path = QString());

    bool isAutoEyeBlink() const { return m_eyeBlinkCb->isChecked(); }
    bool isAutoBreath() const { return m_breathCb->isChecked(); }

signals:
    void PlayMotion(const QString& group, int no);
    void StopMotions();
    void ApplyExpression(const QString& name);
    void AutoEyeBlinkChanged(bool enabled);
    void AutoBreathChanged(bool enabled);
    void MouseFollowChanged(bool enabled);
    void PhysicsChanged(bool enabled);
    void MotionPlaybackChanged(float speed, bool loop);
    void PosePartChanged(const QStringList& showPartIds, const QStringList& hidePartIds);
    void PoseConfigChanged();
    void PoseSaved();
    void MotionTimelineChanged();
    void OnionSkinRequestChanged();
    void TimelineDirtyChanged(bool dirty);
    void TimelineHistoryChanged(bool canUndo, bool canRedo);
    void TimelineActivated();
    void TimelineFrameChanged(const CMotionPreviewFrame& frame);
    void TimelinePreviewStopped();
    void TimelineEventsTriggered(const CMotionEventBatch& events);
    void SaveSessionRequested();
    void ExpressionSaved(const QString& name, const QString& path);
    void MotionTimelinePreviewChanged(const QMap<QString, float>& values);
    void LipSyncWavChanged(const QString& path);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    struct PreviewTransportState;
    std::unique_ptr<PreviewTransportState> m_transport;
    quint64 m_transportSerial = 0;
    bool m_transportTransition = false;
    quint64 m_recordValuesRevision = 0;
    ParameterEditGuard m_parameterEditGuard;
    bool m_checkingParameterEditGuard = false;
    bool m_parameterEditApproved = false;
    bool CheckParameterEditGuard(QString* error = nullptr);
    bool RequireTransportAvailable(QString* error = nullptr) const;
    void RestoreTransportControls();
    QListWidget* m_motionList;
    QListWidget* m_exprList;
    QLineEdit* m_motionSearch = nullptr;
    QLineEdit* m_exprSearch = nullptr;
    QPushButton* m_editExpressionButton = nullptr;
    QLabel* m_lipSyncWavStatus = nullptr;
    QCheckBox* m_eyeBlinkCb;
    QCheckBox* m_breathCb;
    QCheckBox* m_mouseFollowCb;
    QCheckBox* m_physicsCb;
    QCheckBox* m_loopCb;
    QSlider* m_speedSlider;
    QLabel* m_speedLabel;
    QSlider* m_timelinePlayhead = nullptr;
    QLabel* m_timelineTimeLabel = nullptr;
    CMotionCurveView* m_curveView = nullptr;
    QWidget* m_poseWidget;
    QWidget* m_posePage = nullptr;

    struct MotionEntry {
        QString group;
        int no;
    };
    QVector<MotionEntry> m_motionEntries;
    struct ExpressionEntry {
        QString name;
        QString path;
    };
    QVector<ExpressionEntry> m_exprEntries;

    // Pose 组数据：每组是一个互斥 Part 列表
    struct PoseGroup {
        QStringList partIds;
        QVector<QStringList> links;
        QVector<QJsonObject> fields;
        QComboBox* combo;
        int sourceIndex = -1;
    };
    bool BuildPoseDocument(QJsonObject& document, QString* error) const;
    QVector<PoseGroup> m_poseGroups;
    QString m_poseJsonPath;
    QJsonObject m_poseDocument;
    QByteArray m_poseSourceDigest;
    QPushButton* m_savePoseButton = nullptr;
    QPushButton* m_addPosePartButton = nullptr;
    QPushButton* m_removePosePartButton = nullptr;
    QPushButton* m_editPoseLinksButton = nullptr;
    int m_activePoseGroup = -1;
    bool m_poseDirty = false;
    CMotionTimeline m_timeline;
    QStringList m_parameterIds;
    QComboBox* m_targetFilter = nullptr;
    QPushButton* m_endPreviewButton = nullptr;
    void AddTimelineKeyframe();
    void EditTimelineSettings();
    void EmitTimelinePreview();
    void RefreshTimelineEvents();
    void ShowTimelineEvents(const CMotionEventBatch& events);
    void SeekTimeline(double time);
    void ActivateTimelineEvent(int row);
    QCheckBox* m_previewFades = nullptr;
    QTabWidget* m_detailTabs = nullptr;
    QTableWidget* m_eventTable = nullptr;
    QLabel* m_eventStatus = nullptr;
    QJsonArray m_displayedEvents;
    bool m_eventStartPending = true;
    QSet<QString> m_modelPartIds;
    bool m_modelPartsKnown = false;
    bool m_timelineDirty = false;
    bool m_restoringTimeline = false;
    struct TimelineSnapshot {
        CMotionTimeline timeline;
        bool loop = false;
        double playhead = 0;
        CMotionPointSelection selection;
        bool SameContent(const TimelineSnapshot& other) const {
            return loop == other.loop && timeline.HasSameContent(other.timeline);
        }
    };
    struct TimelineSnapshotSlot {
        quint64 serial = 0;
        quint64 revision = 0;
        TimelineSnapshot value;
        QMap<quint64, quint64> origins;
    };
    std::shared_ptr<TimelineSnapshotSlot> RegisterTimelineSnapshot(TimelineSnapshot state);
    bool PrepareParameterTracks(const QSet<QString>& ids, const ParameterTrackArchive* archive,
                                PreparedParameterTracks& result, QString* error) const;
    bool PreparedParameterTracksMatch(const PreparedParameterTracks& prepared) const noexcept;
    QVector<std::weak_ptr<TimelineSnapshotSlot>> m_timelineSnapshots;
    QMap<quint64, std::weak_ptr<const ParameterTrackArchive::Data>> m_parameterTrackArchives;
    QMap<quint64, quint64> m_currentTrackOrigins;
    quint64 m_parameterTrackSerial = 0;
    std::optional<TimelineSnapshot> m_savedTimelineContent;
    quint64 m_historyGeneration = 1;
    quint64 m_snapshotSerial = 0;
    quint64 m_registryRevision = 0;
    bool m_parameterTrackUiPending = false;
    bool m_refreshingParameterTracks = false;
    bool m_timelineMutation = false;
    TimelineSnapshot CurrentTimelineSnapshot() const;
    void ApplyTimelineSnapshot(const TimelineSnapshot& snapshot);
    void PushTimelineEdit(const TimelineSnapshot& before, const QString& label);
    void PushTimelineEdit(const std::shared_ptr<TimelineSnapshotSlot>& before, const QString& label);
    void BeginTimelineEditGroup(const QString& label);
    void EndTimelineEditGroup();
    void FlushTimelineEditGroup();
    void RefreshTimelineHistory();
    bool PendingTimelineEdit() const;
    QUndoStack* m_timelineHistory = nullptr;
    bool m_dispatchingTimelineHistory = false;
    QWidget* m_timelineBox = nullptr;
    QSplitter* m_workspaceSplitter = nullptr;
    QScrollArea* m_workspaceScroll = nullptr;
    void UpdateWorkspaceOrientation();
    QSplitter* m_curveSplitter = nullptr;
    QTabWidget* m_assetTabs = nullptr;
    int m_savedAssetTab = 0;
    QPushButton* m_undoTimelineButton = nullptr;
    QPushButton* m_redoTimelineButton = nullptr;
    bool m_editGroupActive = false;
    std::shared_ptr<TimelineSnapshotSlot> m_editGroupBefore;
    QString m_editGroupLabel;
    quint64 m_timelineRevision = 0;
    CTableEditDelegate* m_timelineDelegate = nullptr;
    void SetTimelineDirty(bool dirty);
    bool ModelHasPart(const QString& id, bool* available) const;
    QCheckBox* m_recordTimelineCb = nullptr;
    QTableView* m_timelineTable = nullptr;
    CMotionKeyModel* m_keyModel = nullptr;
    CMotionPointSelection m_selectedPoints;
    QAction* m_snapFrames = nullptr;
    QToolBar* CreateKeyframeTools(QWidget* parent);
    void ConnectKeyframeEditing();
    bool HasPendingTimelineCells() const;
    bool TryCommitTimelineCells(QString* error = nullptr);
    void SetKeyframeSelection(CMotionPointSelection selection);
    bool ApplyTimelineCellEdit(const CMotionPointRef& ref, int column, const QString& text, QString* error);
    bool ApplyKeyframeBatch(const CMotionTimeline::BatchEdit& edit, const QString& label, QString* error = nullptr);
    bool PublishKeyframeEdit(CMotionTimeline candidate, CMotionPointSelection selection,
                             const QString& label, QString* error);
    void CopyKeyframes(bool cut = false);
    void PasteKeyframes(bool selectedTrack, bool replaceCoincident);
    void TransformKeyframes();
    void SetKeyframeInterpolation(int type);
    void BeginCurveGesture(const CMotionPointSelection& selection, bool bezier);
    void TranslateCurveSelection(double time, double value);
    void BeginTangentGesture(CMotionPointRef key, CMotionTimeline::HandleSide side, bool linkForGesture);
    void EditTangentHandle(CMotionPointRef key, CMotionTimeline::HandleSide side, QPointF position);
    void SetTangentMode(CMotionTimeline::TangentMode mode);
    void EditTangentNumbers();
    void RefreshTangentActions();
    void ScheduleOnionRequest(bool contentChanged = false);
    void ConfigureOnionUi(QWidget* parent, QToolBar* tools);
    void RefreshOnionSettings();
    live2dstruct::COnionSkin::Settings OnionSettings() const;
    void FinishCurveGesture();
    void CancelCurveGesture();
    void PublishCurveGesture(CMotionTimeline candidate, const CMotionTimeline::BatchResult& result);
    enum class CurveGesture { None, Points, Bezier };
    CurveGesture m_curveGesture = CurveGesture::None;
    CMotionPointSelection m_gesturePoints;
    std::optional<CMotionTimeline::HandleEdit> m_gestureHandle;
    std::optional<CMotionTimeline::HandleEdit> m_activeHandle;
    QPointF m_activeHandleKeyPoint;
    QAction* m_tangentAction = nullptr;
    QAction* m_tangentUnrestricted = nullptr;
    QAction* m_tangentNumbers = nullptr;
    QVector<QAction*> m_tangentModes;
    QAction* m_onionEnabled = nullptr;
    QWidget* m_onionSettingsPanel = nullptr;
    QSpinBox *m_onionBefore = nullptr, *m_onionAfter = nullptr, *m_onionInterval = nullptr;
    QDoubleSpinBox *m_onionBeforeOpacity = nullptr, *m_onionAfterOpacity = nullptr;
    QCheckBox* m_onionPlayback = nullptr;
    QLabel *m_onionStatusLabel = nullptr, *m_onionIntervalLabel = nullptr;
    bool m_onionRequestQueued = false;
    mutable bool m_onionContentDirty = true;
    mutable QString m_onionCaptureError;
    mutable live2dstruct::COnionSkin::Timeline m_onionTimeline;
    live2dstruct::COnionSkin::Status m_onionStatus;
    QLabel* m_timelineStatus = nullptr;
    QPushButton* m_playTimelineButton = nullptr;
    QTimer* m_timelineTimer = nullptr;
    double m_timelineTime = 0.0;
    QElapsedTimer m_playbackClock;
    qint64 m_playbackLastNs = 0;
    double m_timelinePlaybackSpeed = 1.0;

    void AddPosePart();
    void RemovePosePart();
    void EditPoseLinks();
    void EmitPoseSelection(int poseIndex, int optionIndex);
    void EditSelectedExpression();
    void LoadLipSyncWav();
    void StopLipSyncWav();
    void ExportTimeline();
    void ImportTimeline();
    void ClearTimeline();
    void RefreshTimelineTable();
    void RemoveTimelineKeyframe();
    void ToggleTimelinePlayback();
    void TickTimelinePlayback();
    void SetTimelineTimeFromSlider(int value);

    struct TimelineRow {
        CMotionTrack track;
        int pointIndex = -1;
        QPointF point;
        int segmentType = 0;
    };
    QVector<TimelineRow> m_timelineRows;
    bool m_updatingTimeline = false;
};

Q_DECLARE_METATYPE(CMotionPanel::ParameterTrackArchive)

#endif // CMOTIONPANEL_H
