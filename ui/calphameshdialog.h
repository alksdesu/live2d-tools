#ifndef CALPHAMESHDIALOG_H
#define CALPHAMESHDIALOG_H

#include "live2dstruct/calphameshsource.h"
#include <QDialog>
#include <functional>
#include <memory>

class CAlphaMeshCanvas;
class QLabel;
class QPlainTextEdit;
class QDoubleSpinBox;
class QSpinBox;
class QCheckBox;
class QPushButton;
class QDialogButtonBox;
class QSplitter;
class QCloseEvent;
class QResizeEvent;

class CAlphaMeshDialog : public QDialog
{
    Q_OBJECT
public:
    using Source = live2dstruct::CAlphaMeshSource::Source;
    using Planner = live2dstruct::CAlphaMeshPlanner;
    using Plan = Planner::Plan;
    using SubmissionHandler = std::function<bool(const Plan&, QString*)>;
    explicit CAlphaMeshDialog(const Source& source, QWidget* parent = nullptr);
    ~CAlphaMeshDialog() override;
    void SetSubmissionHandler(SubmissionHandler handler);
    bool IsValid() const { return m_valid; }
    bool IsReady() const;
    bool IsBusy() const;
    bool IsSubmitting() const { return m_submitting; }
    QString Error() const { return m_error; }
    const Plan& Result() const { return m_plan; }
    void accept() override;
    void reject() override;
    void done(int result) override;

signals:
    void Ready();

protected:
    void closeEvent(QCloseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    struct Job;
    Planner::Settings Settings() const;
    bool InputsValid(QString* error = nullptr) const;
    void RequestGeneration();
    void StartGeneration();
    void StopGeneration();
    void SelectDensity(Planner::Density density);
    void Refresh();
    void RefreshStatus();
    void SetError(const QString& error);
    void UpdateOrientation();
    Source m_source;
    Plan m_plan;
    Planner::Settings m_planSettings;
    SubmissionHandler m_submit;
    std::shared_ptr<Job> m_job;
    quint64 m_generation = 0;
    bool m_valid = true, m_hasPlan = false, m_queued = false, m_refreshing = false;
    bool m_submitting = false, m_finished = false;
    QString m_error;
    CAlphaMeshCanvas* m_canvas = nullptr;
    QDoubleSpinBox *m_boundary = nullptr, *m_interior = nullptr, *m_margin = nullptr;
    QSpinBox* m_threshold = nullptr;
    QCheckBox *m_showOld = nullptr, *m_showContour = nullptr;
    QVector<QPushButton*> m_density;
    QPushButton *m_generate = nullptr, *m_stop = nullptr;
    QLabel *m_status = nullptr, *m_counts = nullptr, *m_units = nullptr;
    QPlainTextEdit* m_feedback = nullptr;
    QSplitter* m_splitter = nullptr;
    QWidget* m_controls = nullptr;
    QDialogButtonBox* m_buttons = nullptr;
};

#endif
