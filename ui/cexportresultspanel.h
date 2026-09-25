#ifndef CEXPORTRESULTSPANEL_H
#define CEXPORTRESULTSPANEL_H

#include "psdbatchresult.h"
#include <QDockWidget>
#include <QJsonObject>
#include <array>

class QLabel;
class QPlainTextEdit;
class QPushButton;
class QTableWidget;

class CExportResultsPanel : public QDockWidget
{
    Q_OBJECT
public:
    explicit CExportResultsPanel(QWidget* parent = nullptr);
    bool SetResults(const PsdBatchResults& results);
    bool UpdateResult(int index, const PsdBatchFileResult& result);
    void SetBusy(bool busy);
    void SetRetryAvailable(bool available, const QString& reason = {});
    PsdBatchResults Results() const { return m_results; }
    QJsonObject ReportJson() const;
    QString ReportText() const;

signals:
    void OpenOutputRequested(const QString& path);
    void RetryRequested(const QStringList& sourcePaths);

private:
    void WriteRow(int index);
    void RefreshSummary();
    void RefreshSelection();
    QStringList RetrySources() const;
    bool CanOpen(int index) const;

    PsdBatchResults m_results;
    std::array<int, 5> m_counts{};
    int m_retryable = 0;
    bool m_busy = false;
    bool m_retryAvailable = true;
    QTableWidget* m_table = nullptr;
    QLabel* m_summary = nullptr;
    QPlainTextEdit* m_details = nullptr;
    QPushButton* m_open = nullptr;
    QPushButton* m_retry = nullptr;
    QPushButton* m_copy = nullptr;
};

#endif
