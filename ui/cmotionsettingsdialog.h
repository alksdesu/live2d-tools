#ifndef CMOTIONSETTINGSDIALOG_H
#define CMOTIONSETTINGSDIALOG_H

#include <QDialog>
#include "live2dstruct/cmotiontimeline.h"

class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QTableWidget;
class CTableEditDelegate;
class COptionalMotionTime;

class CMotionSettingsDialog : public QDialog
{
    Q_OBJECT
public:
    explicit CMotionSettingsDialog(const CMotionTimeline& timeline, double playhead = 0,
                                   QWidget* parent = nullptr);
    const CMotionTimeline& Timeline() const { return m_timeline; }

public slots:
    void accept() override;

private:
    CMotionTimeline m_timeline;
    double m_playhead = 0;
    QDoubleSpinBox* m_fps;
    QDoubleSpinBox* m_duration;
    bool m_fpsChanged = false;
    bool m_durationChanged = false;
    COptionalMotionTime* m_fadeIn;
    COptionalMotionTime* m_fadeOut;
    COptionalMotionTime* m_curveFadeIn;
    COptionalMotionTime* m_curveFadeOut;
    QComboBox* m_curve;
    std::optional<CMotionTrack> m_currentTrack;
    QMap<CMotionTrack, CMotionFadeTimes> m_curveFades;
    QTableWidget* m_events;
    CTableEditDelegate* m_eventDelegate;
    QVector<QJsonObject> m_eventSources;
    QLabel* m_error;
    void StoreCurveFade();
    void AddEvent(const QJsonObject& event);
};

#endif
