#ifndef CMOTIONPANEL_H
#define CMOTIONPANEL_H

#include <QDockWidget>
#include <QListWidget>
#include <QCheckBox>

class CMotionPanel : public QDockWidget
{
    Q_OBJECT
public:
    explicit CMotionPanel(QWidget *parent = nullptr);
    void BuildLists();

    bool isAutoEyeBlink() const { return m_eyeBlinkCb->isChecked(); }
    bool isAutoBreath() const { return m_breathCb->isChecked(); }

signals:
    void PlayMotion(const QString& group, int no);
    void ApplyExpression(const QString& name);
    void AutoEyeBlinkChanged(bool enabled);
    void AutoBreathChanged(bool enabled);

private:
    QListWidget* m_motionList;
    QListWidget* m_exprList;
    QCheckBox* m_eyeBlinkCb;
    QCheckBox* m_breathCb;

    struct MotionEntry {
        QString group;
        int no;
    };
    QVector<MotionEntry> m_motionEntries;
    QVector<QString> m_exprNames;
};

#endif // CMOTIONPANEL_H
