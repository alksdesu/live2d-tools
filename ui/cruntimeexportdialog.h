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
    bool IncludeArtwork() const;
    void SetInitialDirectory(const QString& targetDirectory);
    void SetArtworkNotice(bool visible);
    void SetArtworkSummary(int revisionCount, qint64 materialBytes);

public slots:
    void accept() override;
    void done(int result) override;

private:
    QString ValidateTarget() const;
    void RefreshValidation();
    void ChooseParentDirectory();
    void RefreshArtworkNotice();

    QLineEdit* m_target = nullptr;
    QCheckBox* m_currentValues = nullptr;
    QCheckBox* m_timeline = nullptr;
    QCheckBox* m_artwork = nullptr;
    QLabel* m_error = nullptr;
    QLabel* m_artworkNotice = nullptr;
    QLabel* m_artworkSummary = nullptr;
    QScrollArea* m_scroll = nullptr;
    QDialogButtonBox* m_buttons = nullptr;
    QString m_defaultName;
    int m_artworkRevisions = 0;
    qint64 m_artworkBytes = 0;
    bool m_legacyArtworkNotice = false;
};

#endif
