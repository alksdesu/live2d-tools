#ifndef CDEFORMERRENAMEDIALOG_H
#define CDEFORMERRENAMEDIALOG_H

#include <QDialog>
#include <functional>

class QLabel;
class QLineEdit;
class QPushButton;

class CDeformerRenameDialog : public QDialog
{
    Q_OBJECT
public:
    using Submit = std::function<bool(const QString&, QString*)>;
    CDeformerRenameDialog(const QString& id, const QString& type, const QString& currentName,
                          QWidget* parent = nullptr);
    void SetSubmissionHandler(Submit handler);
    bool IsSubmitting() const { return m_submitting; }
    QString Name() const;
    QString Error() const;

public slots:
    void done(int result) override;

private:
    void Refresh();
    QLineEdit* m_name = nullptr;
    QLabel* m_error = nullptr;
    QPushButton* m_save = nullptr;
    Submit m_submit;
    bool m_submitting = false;
};

#endif
