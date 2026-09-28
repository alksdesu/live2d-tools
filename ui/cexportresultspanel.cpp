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
#include <QScrollBar>
#include <QTextDocument>
#include <cmath>
#include <QSplitter>
#include <QTableWidget>
#include <QVBoxLayout>
#include <limits>

namespace {
class ResultTable final : public QTableWidget
{
public:
    using QTableWidget::QTableWidget;
    QSize minimumSizeHint() const override
    {
        QSize hint = QTableWidget::minimumSizeHint();
        const auto margins = contentsMargins();
        const int frame = qMax(2 * frameWidth(), margins.top() + margins.bottom());
        const int header = horizontalHeader()->isHidden() ? 0 :
            qMax(horizontalHeader()->height(), horizontalHeader()->sizeHint().height());
        const int row = rowCount() ? rowHeight(0) : verticalHeader()->defaultSectionSize();
        const bool scrolling = horizontalScrollBarPolicy() == Qt::ScrollBarAlwaysOn ||
            (horizontalScrollBarPolicy() == Qt::ScrollBarAsNeeded && horizontalScrollBar()->maximum() > 0);
        hint.setHeight(frame + header + row + (scrolling ? horizontalScrollBar()->sizeHint().height() : 0));
        return hint;
    }
protected:
    bool event(QEvent* event) override
    {
        const bool accepted = QTableWidget::event(event);
        if (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange ||
            event->type() == QEvent::Polish || event->type() == QEvent::Show ||
            event->type() == QEvent::Resize || event->type() == QEvent::DevicePixelRatioChange) {
            const int minimum = minimumSizeHint().height();
            if (minimumHeight() != minimum) {
                setMinimumHeight(minimum);
                updateGeometry();
            }
        }
        return accepted;
    }
};

class ResultDetails final : public QPlainTextEdit
{
public:
    using QPlainTextEdit::QPlainTextEdit;
    QSize minimumSizeHint() const override
    {
        QSize hint = QPlainTextEdit::minimumSizeHint();
        const auto margins = contentsMargins();
        const int frame = margins.top() + margins.bottom();
        const int text = fontMetrics().lineSpacing() * 2 + int(std::ceil(2 * document()->documentMargin()));
        hint.setHeight(frame + text);
        return hint;
    }
protected:
    bool event(QEvent* event) override
    {
        const bool accepted = QPlainTextEdit::event(event);
        if (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange ||
            event->type() == QEvent::Polish || event->type() == QEvent::Show ||
            event->type() == QEvent::Resize || event->type() == QEvent::DevicePixelRatioChange) {
            const int minimum = minimumSizeHint().height();
            if (minimumHeight() != minimum) {
                setMinimumHeight(minimum);
                updateGeometry();
            }
        }
        return accepted;
    }
};

bool retryable(const PsdBatchFileResult& result)
{
    return result.state == PsdBatchFileState::Failed && !result.outputCommitted && result.sourceInfo.IsEmpty();
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
        case PsdBatchFileState::Succeeded:
            summary = result.sourceInfo.IsEmpty() ? QStringLiteral("导出成功。") :
                result.sourceInfo.Get().method == CExportSourceInfo::Method::OriginalPsd
                    ? QStringLiteral("原始 PSD 已完整另存。") : QStringLiteral("来源图层已重建。");
            break;
        case PsdBatchFileState::Failed: summary = QStringLiteral("导出失败，未提供详细原因。"); break;
        case PsdBatchFileState::NotRun: summary = QStringLiteral("未执行导出。"); break;
        case PsdBatchFileState::Canceled: summary = QStringLiteral("当前文件导出已取消。"); break;
        }
    }
    const QString writeStatus = result.outputCommitted ? QStringLiteral("文件已确认写出。") :
        result.state == PsdBatchFileState::Canceled ? QStringLiteral("未提交新的输出文件。") :
        result.state == PsdBatchFileState::Pending || result.state == PsdBatchFileState::NotRun
            ? QStringLiteral("尚未开始写入文件。") : QStringLiteral("文件写入未确认。");
    QString details = QStringLiteral("%1\n%2").arg(summary, writeStatus);
    if (!result.coverage.IsEmpty()) details += QStringLiteral("\n\n当前姿态对象：\n") + result.coverage.Text();
    details += QStringLiteral("\n\n%1：%2\n输出文件：%3\n状态：%4\n阶段：%5\n耗时：%6")
        .arg(result.sourceInfo.IsEmpty() ? QStringLiteral("源模型") : QStringLiteral("来源文件"),
             result.sourcePath, result.outputPath, PsdBatchStateText(result.state),
             PsdExportStageText(result.stage), elapsedText(result));
    if (result.diagnostics.ErrorCount() || result.diagnostics.WarningCount())
        details += QStringLiteral("\n\n%1：\n%2").arg(result.sourceInfo.IsEmpty() ? QStringLiteral("模型检查") : QStringLiteral("来源检查"), result.diagnostics.Text());
    if (!result.sourceInfo.IsEmpty()) {
        const auto& source = result.sourceInfo.Get();
        details += QStringLiteral("\n\n恢复方式：%1\n来源文档：%2\n来源版本：%3\n来源摘要：%4")
            .arg(result.sourceInfo.MethodText(), source.documentName, source.revisionId,
                 QString::fromLatin1(source.inputDigest.toHex()));
        if (!source.outputDigest.isEmpty()) details += QStringLiteral("\n输出摘要：%1").arg(QString::fromLatin1(source.outputDigest.toHex()));
    }
    return details;
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
    m_table = new ResultTable(splitter);
    m_table->setObjectName(QStringLiteral("exportResultsTable"));
    m_table->setColumnCount(6);
    m_table->setHorizontalHeaderLabels({QStringLiteral("来源文件"), QStringLiteral("输出路径"), QStringLiteral("状态"),
                                      QStringLiteral("阶段"), QStringLiteral("耗时"), QStringLiteral("错误 / 说明")});
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setWordWrap(false);
    m_table->verticalHeader()->setVisible(false);
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_details = new ResultDetails(splitter);
    m_details->setObjectName(QStringLiteral("exportResultDetails"));
    m_details->setReadOnly(true);
    m_details->setPlaceholderText(QStringLiteral("选择文件查看完整路径、提示和错误原因"));
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
    decltype(m_counts) counts{};
    int retryCount = 0;
    int warningFiles = 0;
    for (int index = 0; index < results.size(); ++index) {
        if (!IsPsdBatchResultValid(results[index]) || results[index].index != index) return false;
        ++counts[int(results[index].state)];
        retryCount += retryable(results[index]);
        warningFiles += results[index].diagnostics.WarningCount() > 0;
    }
    const QSignalBlocker blocker(m_table);
    m_results = results;
    m_counts = counts;
    m_retryable = retryCount;
    m_warningFiles = warningFiles;
    m_table->clearContents();
    m_table->setRowCount(m_results.size());
    m_table->updateGeometry();
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
         (!IsPsdExportStageTransitionValid(before.stage, result.stage) || result.elapsedMs < before.elapsedMs))) return false;
    --m_counts[int(before.state)];
    m_retryable -= retryable(before);
    m_warningFiles -= before.diagnostics.WarningCount() > 0;
    m_results[index] = result;
    ++m_counts[int(result.state)];
    m_retryable += retryable(result);
    m_warningFiles += result.diagnostics.WarningCount() > 0;
    WriteRow(index);
    RefreshSummary();
    if (m_table->currentRow() == index) RefreshSelection();
    return true;
}

void CExportResultsPanel::WriteRow(int index)
{
    const auto& result = m_results[index];
    const QString state = result.state == PsdBatchFileState::Succeeded && result.diagnostics.WarningCount()
        ? QStringLiteral("成功（有提示）") : PsdBatchStateText(result.state);
    QString explanation = result.error.isEmpty() && result.diagnostics.WarningCount()
        ? QStringLiteral("%1 条检查提示，详情见下方").arg(result.diagnostics.WarningCount()) : result.error;
    if (!result.coverage.IsEmpty()) {
        if (!explanation.isEmpty()) explanation += QLatin1Char('\n');
        explanation += result.coverage.Summary();
    }
    const QStringList values{result.sourcePath, result.outputPath, state,
                             PsdExportStageText(result.stage), elapsedText(result), explanation};
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
        QStringLiteral("共 %1 项 · 成功 %2 · 失败 %3 · 已取消 %4 · 未执行 %5 · 进行中 %6 · 等待 %7")
            .arg(m_results.size()).arg(m_counts[int(PsdBatchFileState::Succeeded)]).arg(m_counts[int(PsdBatchFileState::Failed)])
            .arg(m_counts[int(PsdBatchFileState::Canceled)])
            .arg(m_counts[int(PsdBatchFileState::NotRun)]).arg(m_counts[int(PsdBatchFileState::Running)]).arg(m_counts[int(PsdBatchFileState::Pending)]));
    const int selected = m_table->currentRow();
    if (selected >= 0 && selected < m_results.size() && !m_results[selected].coverage.IsEmpty()) {
        const auto& result = m_results[selected];
        const QString status = result.outputCommitted
            ? result.coverage.IsCommitted() ? QStringLiteral("已写出") : QStringLiteral("对象记录未确认")
            : QStringLiteral("未提交");
        m_summary->setText(m_summary->text() + QStringLiteral("\n所选文件（%1）：%2").arg(status, result.coverage.Summary()));
    }
    if (m_warningFiles) m_summary->setText(m_summary->text() + QStringLiteral("\n%1 项包含检查提示").arg(m_warningFiles));
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
    RefreshSummary();
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
    bool hasSource = false;
    bool hasCoverage = false;
    for (const auto& item : m_results) {
        QJsonObject file{{QStringLiteral("index"), item.index}, {QStringLiteral("source"), item.sourcePath},
            {QStringLiteral("output"), item.outputPath}, {QStringLiteral("state"), PsdBatchStateKey(item.state)},
            {QStringLiteral("stage"), PsdExportStageKey(item.stage)}, {QStringLiteral("error"), item.error},
            {QStringLiteral("elapsedMs"), item.elapsedMs}, {QStringLiteral("outputCommitted"), item.outputCommitted},
            {QStringLiteral("diagnostics"), item.diagnostics.ToJson()}};
        if (!item.sourceInfo.IsEmpty()) {
            file.insert(QStringLiteral("sourceRecovery"), item.sourceInfo.ToJson());
            hasSource = true;
        }
        if (!item.coverage.IsEmpty()) {
            file.insert(QStringLiteral("objectCoverage"), item.coverage.ToJson());
            hasCoverage = true;
        }
        files.append(file);
    }
    return QJsonObject{{QStringLiteral("version"), hasCoverage ? 4 : hasSource ? 3 : 2}, {QStringLiteral("busy"), m_busy}, {QStringLiteral("files"), files}};
}

QString CExportResultsPanel::ReportText() const
{
    return QString::fromUtf8(QJsonDocument(ReportJson()).toJson(QJsonDocument::Indented));
}
