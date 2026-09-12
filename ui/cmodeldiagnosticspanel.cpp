#include "cmodeldiagnosticspanel.h"
#include "live2dstruct/cmodeldocument.h"
#include <QBrush>
#include <QColor>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QFileDialog>
#include <QSaveFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QVBoxLayout>
#include <QWidget>

CModelDiagnosticsPanel::CModelDiagnosticsPanel(QWidget* parent)
    : QDockWidget(QStringLiteral("模型检查"), parent)
{
    setObjectName(QStringLiteral("diagnosticsPanel"));
    auto* content = new QWidget(this);
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(8);
    auto* header = new QHBoxLayout();
    m_summary = new QLabel(content);
    auto* refresh = new QPushButton(QStringLiteral("重新检查"), content);
    auto* exportButton = new QPushButton(QStringLiteral("导出报告"), content);
    header->addWidget(m_summary, 1);
    header->addWidget(refresh);
    header->addWidget(exportButton);
    layout->addLayout(header);
    m_list = new QListWidget(content);
    m_list->setSelectionMode(QAbstractItemView::NoSelection);
    layout->addWidget(m_list);
    setWidget(content);
    connect(refresh, &QPushButton::clicked, this, &CModelDiagnosticsPanel::Refresh);
    connect(exportButton, &QPushButton::clicked, this, &CModelDiagnosticsPanel::ExportReport);
    Refresh();
}

void CModelDiagnosticsPanel::ExportReport()
{
    if (!m_document || !m_document->IsLoaded()) {
        QMessageBox::information(this, QStringLiteral("导出诊断报告"), QStringLiteral("没有已加载的模型"));
        return;
    }
    const QString defaultPath = m_document->SourcePath() + QStringLiteral(".diagnostics.json");
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("导出诊断报告"), defaultPath,
                                                       QStringLiteral("JSON (*.json)"));
    if (path.isEmpty()) return;
    QJsonObject root;
    root.insert(QStringLiteral("format"), QStringLiteral("live2d2psd-diagnostics-v1"));
    root.insert(QStringLiteral("sourceMoc"), m_document->SourcePath());
    const QVector<live2dstruct::CModelDiagnostic> diagnostics = m_document->Validate();
    QJsonArray entries;
    for (const auto& diagnostic : diagnostics) {
        QJsonObject entry;
        entry.insert(QStringLiteral("severity"), diagnostic.severity == live2dstruct::CModelDiagnostic::Error
                     ? QStringLiteral("error") : QStringLiteral("warning"));
        entry.insert(QStringLiteral("code"), diagnostic.code);
        entry.insert(QStringLiteral("message"), diagnostic.message);
        entry.insert(QStringLiteral("index"), diagnostic.index);
        entries.append(entry);
    }
    root.insert(QStringLiteral("diagnostics"), entries);
    const QByteArray data = QJsonDocument(root).toJson(QJsonDocument::Indented);
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) {
        QMessageBox::warning(this, QStringLiteral("导出诊断报告失败"), path);
        return;
    }
    m_summary->setToolTip(QStringLiteral("报告已保存：%1").arg(path));
}

void CModelDiagnosticsPanel::SetDocument(const live2dstruct::CModelDocument* document)
{
    m_document = document;
    Refresh();
}

void CModelDiagnosticsPanel::Refresh()
{
    m_list->clear();
    if (!m_document || !m_document->IsLoaded()) {
        m_summary->setText(QStringLiteral("没有已加载的模型"));
        return;
    }
    const QVector<live2dstruct::CModelDiagnostic> diagnostics = m_document->Validate();
    int errors = 0;
    int warnings = 0;
    for (const auto& diagnostic : diagnostics) {
        if (diagnostic.severity == live2dstruct::CModelDiagnostic::Error) ++errors;
        else ++warnings;
        auto* item = new QListWidgetItem(diagnostic.message, m_list);
        item->setToolTip(diagnostic.code);
        item->setForeground(diagnostic.severity == live2dstruct::CModelDiagnostic::Error
                                ? QBrush(QColor(QStringLiteral("#D92D20")))
                                : QBrush(QColor(QStringLiteral("#B54708"))));
    }
    if (diagnostics.isEmpty()) {
        m_summary->setText(QStringLiteral("检查通过"));
    } else {
        m_summary->setText(QStringLiteral("%1 个错误，%2 个警告").arg(errors).arg(warnings));
    }
}
