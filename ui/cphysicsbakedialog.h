#ifndef CPHYSICSBAKEDIALOG_H
#define CPHYSICSBAKEDIALOG_H

#include "live2dstruct/cmotiontimeline.h"
#include <QDialog>
#include <QMap>
#include <QSet>

class QCheckBox;
class QComboBox;
class QDialogButtonBox;
class QDoubleSpinBox;
class QLabel;
class QListWidget;
class QScrollArea;
class QSpinBox;

class CPhysicsBakeDialog : public QDialog
{
    Q_OBJECT
public:
    struct Options {
        enum class FpsSource { Scene, Physics, Custom };
        QSet<QString> outputIds;
        double fps = 30.0;
        FpsSource fpsSource = FpsSource::Scene;
        double duration = 1.0;
        int warmupCycles = 1;
        bool newAnimation = false;
    };
    explicit CPhysicsBakeDialog(QMap<QString, QString> outputNames, const CMotionTimeline& timeline,
                                QWidget* parent = nullptr, double physicsFps = 0.0);
    Options Selection() const;
    void SetConflictingOutputs(const QSet<QString>& ids);

public slots:
    void accept() override;
    void done(int result) override;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void RefreshValidation();
    QString Validate(Options& options, qint64& pointsPerOutput, qint64& retainedPoints, double& endTime) const;
    QListWidget* m_outputs = nullptr;
    QCheckBox* m_newAnimation = nullptr;
    QComboBox* m_fpsSource = nullptr;
    QLabel* m_destinationHint = nullptr;
    QSet<QString> m_conflictingOutputs;
    QDoubleSpinBox* m_fps = nullptr;
    QDoubleSpinBox* m_duration = nullptr;
    QSpinBox* m_warmup = nullptr;
    QLabel* m_summary = nullptr;
    QLabel* m_error = nullptr;
    QScrollArea* m_scroll = nullptr;
    QDialogButtonBox* m_buttons = nullptr;
    QMap<QString, int> m_replacedPointCounts;
    qint64 m_originalPoints = 0;
    double m_sceneDuration = 0;
    double m_initialFps = 30;
    double m_physicsFps = 0;
    double m_initialDuration = 1;
    bool m_hasAnimation = false;
    bool m_fpsChanged = false;
    bool m_durationChanged = false;
    bool m_sourceValid = true;
};

#endif
