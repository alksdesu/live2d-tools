#ifndef CMOTIONPANEL_H
#define CMOTIONPANEL_H

#include <QDockWidget>
#include <QListWidget>
#include <QCheckBox>
#include <QComboBox>
#include <QSlider>
#include <QVBoxLayout>
#include <QLineEdit>

class QLabel;

class CMotionPanel : public QDockWidget
{
    Q_OBJECT
public:
    explicit CMotionPanel(QWidget *parent = nullptr);
    void BuildLists();
    void BuildPoseGroups(const QString& poseJsonPath);

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
    void PosePartChanged(const QString& showPartId, const QStringList& hidePartIds);

private:
    QListWidget* m_motionList;
    QListWidget* m_exprList;
    QLineEdit* m_motionSearch = nullptr;
    QLineEdit* m_exprSearch = nullptr;
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
    QVector<QString> m_exprNames;

    // Pose 组数据：每组是一个互斥 Part 列表
    struct PoseGroup {
        QStringList partIds;
        QComboBox* combo;
    };
    QVector<PoseGroup> m_poseGroups;
};

#endif // CMOTIONPANEL_H
