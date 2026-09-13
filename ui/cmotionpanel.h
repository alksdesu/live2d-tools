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
#include "cmotiontimeline.h"

class QLabel;
class QPushButton;
class QTableWidget;

class CMotionPanel : public QDockWidget
{
    Q_OBJECT
public:
    explicit CMotionPanel(QWidget *parent = nullptr);
    void BuildLists();
    bool BuildPoseGroups(const QString& poseJsonPath, QString* error = nullptr);
    bool SavePose(QString* error = nullptr);
    void RecordParameterValue(const QString& parameterId, float value);
    bool HasUnsavedPoseChanges() const { return m_poseDirty; }

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

private:
    QListWidget* m_motionList;
    QListWidget* m_exprList;
    QLineEdit* m_motionSearch = nullptr;
    QLineEdit* m_exprSearch = nullptr;
    QPushButton* m_editExpressionButton = nullptr;
    QCheckBox* m_eyeBlinkCb;
    QCheckBox* m_breathCb;
    QCheckBox* m_mouseFollowCb;
    QCheckBox* m_physicsCb;
    QCheckBox* m_loopCb;
    QSlider* m_speedSlider;
    QLabel* m_speedLabel;
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
    int m_activePoseGroup = -1;
    bool m_poseDirty = false;
    CMotionTimeline m_timeline;
    QCheckBox* m_recordTimelineCb = nullptr;
    QTableWidget* m_timelineTable = nullptr;
    QLabel* m_timelineStatus = nullptr;

    void AddPosePart();
    void RemovePosePart();
    void EditSelectedExpression();
    void ExportTimeline();
    void ClearTimeline();
    void RefreshTimelineTable();
    void ApplyTimelineCellEdit(int row, int column);
    void RemoveTimelineKeyframe();

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
