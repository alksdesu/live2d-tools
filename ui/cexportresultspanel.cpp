#include "cexportresultspanel.h"
#include <QApplication>
#include <QClipboard>
#include <QFileInfo>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTableWidget>
#include <QVBoxLayout>
#include <limits>

namespace {
bool retryable(const PsdBatchFileResult& result)
{
    return result.state == PsdBatchFileState::Failed && !result.outputCommitted;
}

QString elapsedText(const PsdBatchFileResult& result)
{
    if (result.state == PsdBatchFileState::Pending || result.state == PsdBatchFileState::NotRun) return QStringLiteral("—");
    return result.elapsedMs < 1000 ? QStringLiteral("%1 ms").arg(result.elapsedMs)
                                  : QStringLiteral("%1 s").arg(double(result.elapsedMs) / 1000.0, 0, 'f', 2);
}

QString detailText(const PsdBatchFileResult& result)
{
    QString summary = result.error;
    if (summary.isEmpty()) {
        switch (result.state) {
        case PsdBatchFileState::Pending: summary = QStringLiteral("等待开始导出。"); break;
        case PsdBatchFileState::Running: summary = QStringLiteral("正在%1。").arg(PsdExportStageText(result.stage)); break;
        case PsdBatchFileState::Succeeded: summary = QStringLiteral("导出成功。"); break;
        case PsdBatchFileState::Failed: summary = QStringLiteral("导出失败，未提供详细原因。"); break;
        case PsdBatchFileState::NotRun: summary = QStringLiteral("未执行导出。"); break;
        }
    }
    const QString writeStatus = result.outputCommitted ? QStringLiteral("文件已确认写出。") :
        result.state == PsdBatchFileState::Pending || result.state == PsdBatchFileState::NotRun
            ? QStringLiteral("尚未开始写入文件。") : QStringLiteral("文件写入未确认。");
    return QStringLiteral("%1\n%2\n\n源模型：%3\n输出文件：%4\n状态：%5\n阶段：%6\n耗时：%7")
        .arg(summary, writeStatus, result.sourcePath, result.outputPath, PsdBatchStateText(result.state),
             PsdExportStageText(result.stage), elapsedText(result));
}
}

CExportResultsPanel::CExportResultsPanel(QWidget* parent) : QDockWidget(QStringLiteral("导出结果"), parent)
{
    setObjectName(QStringLiteral("exportResultsPanel"));
    auto* content = new QWidget(this);
    auto* layout = new QVBoxLayout(content);
    m_summary = new QLabel(content);
    m_summary->setObjectName(QStringLiteral("exportResultsSummary"));
    m_summary->setTextFormat(Qt::PlainText);
    m_summary->setWordWrap(true);
    layout->addWidget(m_summary);
    auto* actions = new QHBoxLayout;
    m_open = new QPushButton(QStringLiteral("打开输出"), content);
    m_retry = new QPushButton(QStringLiteral("重试未写出的失败项"), content);
    m_copy = new QPushButton(QStringLiteral("复制报告"), content);
    m_open->setObjectName(QStringLiteral("exportOpenOutput"));
    m_retry->setObjectName(QStringLiteral("exportRetryFailed"));
    m_copy->setObjectName(QStringLiteral("exportCopyReport"));
    actions->addWidget(m_open);
    actions->addWidget(m_retry);
    actions->addStretch(1);
    actions->addWidget(m_copy);
    layout->addLayout(actions);
    auto* splitter = new QSplitter(Qt::Vertical, content);
    m_table = new QTableWidget(splitter);
    m_table->setObjectName(QStringLiteral("exportResultsTable"));
    m_table->setColumnCount(6);
    m_table->setHorizontalHeaderLabels({QStringLiteral("源模型"), QStringLiteral("输出路径"), QStringLiteral("状态"),
                                      QStringLiteral("阶段"), QStringLiteral("耗时"), QStringLiteral("错误 / 说明")});
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setWordWrap(false);
    m_table->verticalHeader()->setVisible(false);
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_details = new QPlainTextEdit(splitter);
    m_details->setObjectName(QStringLiteral("exportResultDetails"));
    m_details->setReadOnly(true);
    m_details->setPlaceholderText(QStringLiteral("选择文件查看完整路径和错误原因"));
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 1);
    layout->addWidget(splitter, 1);
    setWidget(content);
    connect(m_table, &QTableWidget::itemSelectionChanged, this, &CExportResultsPanel::RefreshSelection);
    connect(m_open, &QPushButton::clicked, this, [this] {
        const int index = m_table->currentRow();
        if (!CanOpen(index)) { RefreshSelection(); return; }
        const QString output = m_results[index].outputPath;
        emit OpenOutputRequested(output);
    });
    connect(m_retry, &QPushButton::clicked, this, [this] {
        if (m_busy || !m_retryAvailable) return;
        const auto sources = RetrySources();
        if (!sources.isEmpty()) emit RetryRequested(sources);
    });
    connect(m_copy, &QPushButton::clicked, this, [this] { QApplication::clipboard()->setText(ReportText()); });
    RefreshSummary();
    RefreshSelection();
}

bool CExportResultsPanel::SetResults(const PsdBatchResults& results)
{
    if (results.size() > std::numeric_limits<int>::max()) return false;
    std::array<int, 5> counts{};
    int retryCount = 0;
    for (int index = 0; index < results.size(); ++index) {
        if (!IsPsdBatchResultValid(results[index]) || results[index].index != index) return false;
        ++counts[int(results[index].state)];
        retryCount += retryable(results[index]);
    }
    const QSignalBlocker blocker(m_table);
    m_results = results;
    m_counts = counts;
    m_retryable = retryCount;
    m_table->clearContents();
    m_table->setRowCount(m_results.size());
    for (int index = 0; index < m_results.size(); ++index) WriteRow(index);
    if (!m_results.isEmpty()) m_table->setCurrentCell(0, 0);
    RefreshSummary();
    RefreshSelection();
    return true;
}

bool CExportResultsPanel::UpdateResult(int index, const PsdBatchFileResult& result)
{
    if (index < 0 || index >= m_results.size() || result.index != index || !IsPsdBatchResultValid(result)) return false;
    const auto& before = m_results[index];
    if (before.sourcePath != result.sourcePath || before.outputPath != result.outputPath) return false;
    if (before == result) return true;
    if (IsPsdBatchTerminal(before.state) ||
        (before.state == PsdBatchFileState::Running && result.state == PsdBatchFileState::Pending) ||
        (before.state == PsdBatchFileState::Running && result.state == PsdBatchFileState::Running &&
         (result.stage < before.stage || result.elapsedMs < before.elapsedMs))) return false;
    --m_counts[int(before.state)];
    m_retryable -= retryable(before);
    m_results[index] = result;
    ++m_counts[int(result.state)];
    m_retryable += retryable(result);
    WriteRow(index);
    RefreshSummary();
    if (m_table->currentRow() == index) RefreshSelection();
    return true;
}

void CExportResultsPanel::WriteRow(int index)
{
    const auto& result = m_results[index];
    const QStringList values{result.sourcePath, result.outputPath, PsdBatchStateText(result.state),
                             PsdExportStageText(result.stage), elapsedText(result), result.error};
    for (int column = 0; column < values.size(); ++column) {
        auto* item = m_table->item(index, column);
        if (!item) { item = new QTableWidgetItem; m_table->setItem(index, column, item); }
        item->setText(values[column]);
    }
}

void CExportResultsPanel::SetBusy(bool busy)
{
    m_busy = busy;
    RefreshSummary();
    RefreshSelection();
}

void CExportResultsPanel::SetRetryAvailable(bool available, const QString& reason)
{
    m_retryAvailable = available;
    const QString explanation = reason.isEmpty() ? QStringLiteral("恢复预览后才能重试。") : reason;
    m_retry->setToolTip(available ? QString() : QStringLiteral("<qt>%1</qt>").arg(explanation.toHtmlEscaped()));
    RefreshSummary();
}

void CExportResultsPanel::RefreshSummary()
{
    m_summary->setText(m_results.isEmpty() ? QStringLiteral("尚无导出结果") :
        QStringLiteral("共 %1 项 · 成功 %2 · 失败 %3 · 未执行 %4 · 进行中 %5 · 等待 %6")
            .arg(m_results.size()).arg(m_counts[int(PsdBatchFileState::Succeeded)]).arg(m_counts[int(PsdBatchFileState::Failed)])
            .arg(m_counts[int(PsdBatchFileState::NotRun)]).arg(m_counts[int(PsdBatchFileState::Running)]).arg(m_counts[int(PsdBatchFileState::Pending)]));
    m_retry->setEnabled(!m_busy && m_retryAvailable && m_retryable > 0);
    m_copy->setEnabled(!m_results.isEmpty());
}

bool CExportResultsPanel::CanOpen(int index) const
{
    return !m_busy && index >= 0 && index < m_results.size() && m_results[index].outputCommitted &&
           QFileInfo(m_results[index].outputPath).isFile();
}

void CExportResultsPanel::RefreshSelection()
{
    const int index = m_table->currentRow();
    m_open->setEnabled(CanOpen(index));
    m_details->setPlainText(index >= 0 && index < m_results.size() ? detailText(m_results[index]) : QString());
}

QStringList CExportResultsPanel::RetrySources() const
{
    QStringList result;
    for (const auto& item : m_results) if (retryable(item)) result.append(item.sourcePath);
    return result;
}

QJsonObject CExportResultsPanel::ReportJson() const
{
    QJsonArray files;
    for (const auto& item : m_results)
        files.append(QJsonObject{{QStringLiteral("index"), item.index}, {QStringLiteral("source"), item.sourcePath},
            {QStringLiteral("output"), item.outputPath}, {QStringLiteral("state"), PsdBatchStateKey(item.state)},
            {QStringLiteral("stage"), PsdExportStageKey(item.stage)}, {QStringLiteral("error"), item.error},
            {QStringLiteral("elapsedMs"), item.elapsedMs}, {QStringLiteral("outputCommitted"), item.outputCommitted}});
    return QJsonObject{{QStringLiteral("version"), 1}, {QStringLiteral("busy"), m_busy}, {QStringLiteral("files"), files}};
}

QString CExportResultsPanel::ReportText() const
{
    return QString::fromUtf8(QJsonDocument(ReportJson()).toJson(QJsonDocument::Indented));
}
