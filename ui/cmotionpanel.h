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

class QLabel;
class QPushButton;
class QTableWidget;
class QTimer;
class CMotionCurveView;
class CTableEditDelegate;
class QUndoStack;
class QTabWidget;
class QSplitter;

class CMotionPanel : public QDockWidget
{
    Q_OBJECT
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

    explicit CMotionPanel(QWidget *parent = nullptr);
    ~CMotionPanel() override;
    bool BuildLists(const QString& modelJsonPath, const QStringList& partIds, QString* error = nullptr);
    bool CaptureTimelineState(QJsonObject* state, QString* error = nullptr);
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
    bool ConfirmTimelineReplacement();
    bool LoadTimeline(const QString& path, QString* error = nullptr);
    bool SaveTimeline(const QString& path, QString* error = nullptr);
    void SetParameterIds(const QStringList& ids) { if (!IsPreviewTransportSuspended()) m_parameterIds = ids; }
    bool AddTimelinePoint(const CMotionTrack& track, float time, float value);
    bool SuspendPreviewTransport(PreviewTransportLease& lease, QString* error = nullptr);
    bool ResumePreviewTransport(PreviewTransportLease& lease, QString* error = nullptr);
    bool IsPreviewTransportSuspended() const;
    void StopTimelinePreview();
    void PauseTimelinePreview();
    void SetTimelinePreviewAvailable(bool available);
    const CMotionTimeline& Timeline() const { return m_timeline; }
    double TimelineTime() const { return m_timelineTime; }
    bool UsesMotionFades() const { return m_previewFades->isChecked(); }
    bool TimelineLoop() const { return m_loopCb->isChecked(); }
    bool BuildPoseGroups(const QString& poseJsonPath, QString* error = nullptr);
    bool SavePose(QString* error = nullptr);
    void RecordParameterValue(const QString& parameterId, float value);
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
        QComboBox* combo;
        int sourceIndex = -1;
    };
    QVector<PoseGroup> m_poseGroups;
    QString m_poseJsonPath;
    QJsonObject m_poseDocument;
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
        bool SameContent(const TimelineSnapshot& other) const {
            return loop == other.loop && timeline.HasSameContent(other.timeline);
        }
    };
    TimelineSnapshot CurrentTimelineSnapshot() const;
    void ApplyTimelineSnapshot(const TimelineSnapshot& snapshot);
    void PushTimelineEdit(const TimelineSnapshot& before, const QString& label);
    void BeginTimelineEditGroup(const QString& label);
    void EndTimelineEditGroup();
    void FlushTimelineEditGroup();
    void RefreshTimelineHistory();
    bool PendingTimelineEdit() const;
    QUndoStack* m_timelineHistory = nullptr;
    QWidget* m_timelineBox = nullptr;
    QSplitter* m_workspaceSplitter = nullptr;
    QSplitter* m_curveSplitter = nullptr;
    QTabWidget* m_assetTabs = nullptr;
    int m_savedAssetTab = 0;
    QPushButton* m_undoTimelineButton = nullptr;
    QPushButton* m_redoTimelineButton = nullptr;
    bool m_editGroupActive = false;
    TimelineSnapshot m_editGroupBefore;
    QString m_editGroupLabel;
    quint64 m_timelineRevision = 0;
    CTableEditDelegate* m_timelineDelegate = nullptr;
    void SetTimelineDirty(bool dirty);
    bool ModelHasPart(const QString& id, bool* available) const;
    QCheckBox* m_recordTimelineCb = nullptr;
    QTableWidget* m_timelineTable = nullptr;
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
    void ApplyTimelineCellEdit(int row, int column);
    void RemoveTimelineKeyframe();
    void ToggleTimelinePlayback();
    void TickTimelinePlayback();
    void SetTimelineTimeFromSlider(int value);
    void ApplyTimelineCurveEdit(const CMotionTrack& track, int pointIndex, float time, float value);

    struct TimelineRow {
        CMotionTrack track;
        int pointIndex = -1;
        QPointF point;
        int segmentType = 0;
    };
    QVector<TimelineRow> m_timelineRows;
    bool m_updatingTimeline = false;
};

#endif // CMOTIONPANEL_H
