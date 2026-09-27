#include "cmodeldiagnosticspanel.h"
#include "live2dstruct/cmodeldocument.h"
#include "theme.h"
#include <QEvent>
#include <QColor>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QSaveFile>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStyle>
#include <QVBoxLayout>
#include <algorithm>

namespace {
using Diagnostic = live2dstruct::CModelDiagnostic;
QString severity(const Diagnostic& diagnostic)
{
    return diagnostic.severity == Diagnostic::Error ? QStringLiteral("错误") : QStringLiteral("警告");
}
QString identity(const Diagnostic& diagnostic)
{
    return diagnostic.code + QChar::Null + diagnostic.objectType + QChar::Null + diagnostic.objectId +
        QChar::Null + QString::number(diagnostic.index);
}
}

CModelDiagnosticsPanel::CModelDiagnosticsPanel(QWidget* parent)
    : QDockWidget(QStringLiteral("模型检查"), parent)
{
    setObjectName(QStringLiteral("diagnosticsPanel"));
    auto* content = new QWidget(this);
    auto* layout = new QVBoxLayout(content);
    m_summary = new QLabel(content);
    m_summary->setObjectName(QStringLiteral("diagnosticsSummary"));
    m_summary->setTextFormat(Qt::PlainText);
    m_summary->setWordWrap(true);
    layout->addWidget(m_summary);
    auto* actions = new QHBoxLayout;
    m_locate = new QPushButton(QStringLiteral("定位对象"), content);
    m_locate->setObjectName(QStringLiteral("diagnosticsLocate"));
    auto* refresh = new QPushButton(QStringLiteral("重新检查"), content);
    refresh->setObjectName(QStringLiteral("diagnosticsRefresh"));
    auto* exportButton = new QPushButton(QStringLiteral("导出报告"), content);
    exportButton->setObjectName(QStringLiteral("diagnosticsExport"));
    actions->addWidget(m_locate);
    actions->addWidget(refresh);
    actions->addWidget(exportButton);
    actions->addStretch();
    layout->addLayout(actions);
    auto* splitter = new QSplitter(Qt::Vertical, content);
    m_list = new QListWidget(splitter);
    m_list->setObjectName(QStringLiteral("diagnosticsList"));
    m_list->setAccessibleName(QStringLiteral("模型诊断"));
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    m_details = new QPlainTextEdit(splitter);
    m_details->setObjectName(QStringLiteral("diagnosticsDetails"));
    m_details->setReadOnly(true);
    m_details->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    m_details->setPlaceholderText(QStringLiteral("选择一项查看完整说明，可选择文字复制。"));
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 1);
    layout->addWidget(splitter, 1);
    setWidget(content);
    connect(refresh, &QPushButton::clicked, this, &CModelDiagnosticsPanel::Refresh);
    connect(exportButton, &QPushButton::clicked, this, &CModelDiagnosticsPanel::ExportReport);
    connect(m_locate, &QPushButton::clicked, this, &CModelDiagnosticsPanel::LocateSelection);
    connect(m_list, &QListWidget::currentRowChanged, this, &CModelDiagnosticsPanel::RefreshSelection);
    connect(m_list, &QListWidget::itemActivated, this, [this] { LocateSelection(); });
    Refresh();
}

QJsonObject CModelDiagnosticsPanel::ReportJson() const
{
    if (!m_document || !m_document->IsLoaded()) return {};
    QJsonArray entries;
    for (const auto& diagnostic : m_document->Validate()) {
        entries.append(QJsonObject{
            {QStringLiteral("severity"), diagnostic.severity == Diagnostic::Error ? QStringLiteral("error") : QStringLiteral("warning")},
            {QStringLiteral("code"), diagnostic.code}, {QStringLiteral("message"), diagnostic.message},
            {QStringLiteral("index"), diagnostic.index}, {QStringLiteral("objectType"), diagnostic.objectType},
            {QStringLiteral("objectId"), diagnostic.objectId}});
    }
    return {{QStringLiteral("format"), QStringLiteral("live2d2psd-diagnostics-v2")},
            {QStringLiteral("sourceMoc"), m_document->SourcePath()},
            {QStringLiteral("sourceDigest"), QString::fromLatin1(m_document->SourceDigest().toHex())},
            {QStringLiteral("documentRevision"), QString::number(m_document->Revision())},
            {QStringLiteral("diagnostics"), entries}};
}

void CModelDiagnosticsPanel::ExportReport()
{
    const auto report = ReportJson();
    if (report.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("导出诊断报告"), QStringLiteral("没有已加载的模型"));
        return;
    }
    const QByteArray data = QJsonDocument(report).toJson(QJsonDocument::Indented);
    const QPointer<CModelDiagnosticsPanel> alive(this);
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("导出诊断报告"),
        report.value(QStringLiteral("sourceMoc")).toString() + QStringLiteral(".diagnostics.json"), QStringLiteral("JSON (*.json)"));
    if (!alive || path.isEmpty()) return;
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) {
        auto* message = new QMessageBox(QMessageBox::Warning, QStringLiteral("导出诊断报告失败"), path, QMessageBox::Ok, this);
        message->setTextFormat(Qt::PlainText);
        message->setAttribute(Qt::WA_DeleteOnClose);
        message->open();
        return;
    }
    m_summary->setToolTip(QStringLiteral("<qt>报告已保存：%1</qt>").arg(path.toHtmlEscaped()));
}

void CModelDiagnosticsPanel::SetDocument(const live2dstruct::CModelDocument* document)
{
    m_document = document;
    Refresh();
}

void CModelDiagnosticsPanel::Refresh()
{
    const int previous = m_list->currentRow();
    const auto selected = previous >= 0 && previous < m_diagnostics.size() ? identity(m_diagnostics[previous]) : QString();
    const QSignalBlocker blocker(m_list);
    m_list->clear();
    m_diagnostics.clear();
    m_summary->setToolTip({});
    m_source.clear(); m_sourcePath.clear(); m_revision = 0;
    if (!m_document || !m_document->IsLoaded()) {
        m_summary->setText(QStringLiteral("没有已加载的模型"));
        RefreshSelection();
        return;
    }
    m_diagnostics = m_document->Validate();
    m_source = m_document->SourceDigest(); m_sourcePath = m_document->SourcePath(); m_revision = m_document->Revision();
    int errors = 0, warnings = 0, current = -1;
    for (int row = 0; row < m_diagnostics.size(); ++row) {
        const auto& diagnostic = m_diagnostics[row];
        if (diagnostic.severity == Diagnostic::Error) ++errors; else ++warnings;
        const auto title = diagnostic.objectId.isEmpty() ? severity(diagnostic) :
            QStringLiteral("%1 · %2").arg(severity(diagnostic), diagnostic.objectId);
        auto* item = new QListWidgetItem(QStringLiteral("%1：%2").arg(title, diagnostic.message), m_list);
        item->setToolTip(QStringLiteral("<qt>%1</qt>").arg(item->text().toHtmlEscaped()));
        if (current < 0 && identity(diagnostic) == selected) current = row;
    }
    m_summary->setText(m_diagnostics.isEmpty() ? QStringLiteral("检查通过") :
        QStringLiteral("%1 个错误，%2 个警告").arg(errors).arg(warnings));
    RefreshAppearance();
    if (!m_diagnostics.isEmpty()) m_list->setCurrentRow(current < 0 ? 0 : current);
    RefreshSelection();
}

bool CModelDiagnosticsPanel::CanLocate(int row) const
{
    if (!m_document || !m_document->IsLoaded() || m_document->SourceDigest() != m_source ||
        m_document->SourcePath() != m_sourcePath || m_document->Revision() != m_revision ||
        row < 0 || row >= m_diagnostics.size()) return false;
    const auto& diagnostic = m_diagnostics[row];
    if (diagnostic.objectId.isEmpty()) return false;
    const auto unique = [&](const auto& objects) {
        return std::count_if(objects.cbegin(), objects.cend(), [&](const auto& object) { return object.id == diagnostic.objectId; }) == 1;
    };
    if (diagnostic.objectType == QLatin1String("artmesh")) return unique(m_document->Drawables());
    if (diagnostic.objectType == QLatin1String("part")) return unique(m_document->Parts());
    if (diagnostic.objectType == QLatin1String("parameter")) return unique(m_document->Parameters());
    if (diagnostic.objectType == QLatin1String("warp") || diagnostic.objectType == QLatin1String("rotation"))
        return unique(m_document->Deformers()) && std::any_of(m_document->Deformers().cbegin(), m_document->Deformers().cend(), [&](const auto& object) {
            return object.id == diagnostic.objectId &&
                ((object.type == live2dstruct::CModelDeformer::Warp) == (diagnostic.objectType == QLatin1String("warp")));
        });
    return false;
}

void CModelDiagnosticsPanel::RefreshSelection()
{
    const int row = m_list->currentRow();
    m_locate->setEnabled(CanLocate(row));
    if (row < 0 || row >= m_diagnostics.size()) { m_details->clear(); return; }
    const auto& diagnostic = m_diagnostics[row];
    QStringList detail{QStringLiteral("%1：%2").arg(severity(diagnostic), diagnostic.message)};
    if (!diagnostic.objectId.isEmpty()) detail.append(QStringLiteral("对象：%1（%2）").arg(diagnostic.objectId, diagnostic.objectType));
    detail.append(QStringLiteral("诊断代码：%1").arg(diagnostic.code));
    const auto text = detail.join(QStringLiteral("\n\n"));
    if (m_details->toPlainText() != text) m_details->setPlainText(text);
}

void CModelDiagnosticsPanel::LocateSelection()
{
    const int row = m_list->currentRow();
    if (!CanLocate(row)) { Refresh(); return; }
    const auto diagnostic = m_diagnostics[row];
    emit ObjectRequested(diagnostic.objectId, diagnostic.objectType);
}

void CModelDiagnosticsPanel::RefreshAppearance()
{
    if (!m_list) return;
    const auto colors = Theme::tokens(palette().color(QPalette::Window).lightness() < 128);
    for (int row = 0; row < m_diagnostics.size(); ++row) {
        auto* item = m_list->item(row);
        const bool error = m_diagnostics[row].severity == Diagnostic::Error;
        item->setForeground(QColor(colors.value(error ? QStringLiteral("@danger@") : QStringLiteral("@text@"))));
        item->setIcon(style()->standardIcon(error ? QStyle::SP_MessageBoxCritical : QStyle::SP_MessageBoxWarning, nullptr, this));
    }
}

void CModelDiagnosticsPanel::changeEvent(QEvent* event)
{
    QDockWidget::changeEvent(event);
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::StyleChange || event->type() == QEvent::ApplicationPaletteChange)
        RefreshAppearance();
}
