#ifndef CMOTIONPANEL_H
#define CMOTIONPANEL_H

#include <QDockWidget>
#include <QListWidget>
#include <QCheckBox>
#include <QComboBox>
#include <QSlider>
#include <QVBoxLayout>
#include <QLineEdit>
#include <QJsonObject>
#include <QMap>
#include <QSet>
#include "live2dstruct/cmotiontimeline.h"

class QLabel;
class QPushButton;
class QTableWidget;
class QTimer;
class CMotionCurveView;
class CTableEditDelegate;

class CMotionPanel : public QDockWidget
{
    Q_OBJECT
public:
    explicit CMotionPanel(QWidget *parent = nullptr);
    bool BuildLists(const QString& modelJsonPath, const QStringList& partIds, QString* error = nullptr);
    bool CaptureTimelineState(QJsonObject* state, QString* error = nullptr);
    bool RestoreTimelineState(const QJsonObject& state, QString* error = nullptr);
    bool HasUnsavedTimelineChanges() const;
    void MarkTimelineSaved();
    quint64 TimelineRevision() const { return m_timelineRevision; }
    bool ConfirmTimelineReplacement();
    bool LoadTimeline(const QString& path, QString* error = nullptr);
    bool SaveTimeline(const QString& path, QString* error = nullptr);
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
    void SaveSessionRequested();
    void ExpressionSaved(const QString& name, const QString& path);
    void MotionTimelinePreviewChanged(const QMap<QString, float>& values);
    void LipSyncWavChanged(const QString& path);

private:
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
    QSet<QString> m_modelPartIds;
    bool m_modelPartsKnown = false;
    bool m_timelineDirty = false;
    bool m_restoringTimeline = false;
    quint64 m_timelineRevision = 0;
    CTableEditDelegate* m_timelineDelegate = nullptr;
    void SetTimelineDirty(bool dirty);
    bool ModelHasPart(const QString& id, bool* available) const;
    QCheckBox* m_recordTimelineCb = nullptr;
    QTableWidget* m_timelineTable = nullptr;
    QLabel* m_timelineStatus = nullptr;
    QPushButton* m_playTimelineButton = nullptr;
    QTimer* m_timelineTimer = nullptr;
    float m_timelineTime = 0.0f;

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
    void ApplyTimelineCurveEdit(const QString& parameterId, int pointIndex, float time, float value);

    struct TimelineRow {
        QString parameterId;
        int pointIndex = -1;
        QPointF point;
        int segmentType = 0;
    };
    QVector<TimelineRow> m_timelineRows;
    bool m_updatingTimeline = false;
};

#endif // CMOTIONPANEL_H
