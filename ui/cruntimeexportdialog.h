#ifndef CRUNTIMEEXPORTDIALOG_H
#define CRUNTIMEEXPORTDIALOG_H

#include <QDialog>

class QCheckBox;
class QDialogButtonBox;
class QLabel;
class QLineEdit;
class QScrollArea;

class CRuntimeExportDialog : public QDialog
{
    Q_OBJECT
public:
    explicit CRuntimeExportDialog(const QString& currentModelPath, bool hasTimeline, QWidget* parent = nullptr);
    QString TargetDirectory() const;
    bool UseCurrentValues() const;
    bool IncludeTimeline() const;
    void SetInitialDirectory(const QString& targetDirectory);

public slots:
    void accept() override;
    void done(int result) override;

private:
    QString ValidateTarget() const;
    void RefreshValidation();
    void ChooseParentDirectory();

    QLineEdit* m_target = nullptr;
    QCheckBox* m_currentValues = nullptr;
    QCheckBox* m_timeline = nullptr;
    QLabel* m_error = nullptr;
    QScrollArea* m_scroll = nullptr;
    QDialogButtonBox* m_buttons = nullptr;
    QString m_defaultName;
};

#endif
