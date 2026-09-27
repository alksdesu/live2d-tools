#ifndef CPSDRECOVERYDIALOG_H
#define CPSDRECOVERYDIALOG_H

#include "psd/cpsdsourcedocument.h"
#include <QDialog>
#include <functional>
#include <memory>

class QComboBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QTreeView;

class CPsdRecoveryDialog : public QDialog
{
    Q_OBJECT
public:
    using ExportHandler = std::function<bool(int documentIndex, const QString& path, QString*)>;
    explicit CPsdRecoveryDialog(std::shared_ptr<const CPsdSourceCatalog> catalog, QWidget* parent = nullptr);
    void SetExportHandler(ExportHandler handler);
    int SelectedDocumentIndex() const;
    bool IsExporting() const { return m_exporting; }
    QString Error() const { return m_error; }
    static bool ValidateOutputPath(const QString& source, const QString& output, QString* error = nullptr);

public slots:
    bool ExportTo(const QString& path);
    void done(int result) override;

signals:
    void CancelRequested();
    void Exported(int documentIndex, const QString& path);

private:
    void SelectDocument();
    void SelectLayer();
    void ChooseOutput();
    void RefreshControls();
    void ShowError(const QString& error);
    std::shared_ptr<const CPsdSourceCatalog> m_catalog;
    ExportHandler m_export;
    QComboBox* m_documents = nullptr;
    QLabel* m_summary = nullptr;
    QLabel* m_status = nullptr;
    QTreeView* m_layers = nullptr;
    QPlainTextEdit* m_details = nullptr;
    QPushButton* m_save = nullptr;
    QPushButton* m_close = nullptr;
    QString m_error;
    bool m_exporting = false;
    bool m_closePending = false;
    bool m_validDocument = false;
    int m_exportIndex = -1;
};

#endif
