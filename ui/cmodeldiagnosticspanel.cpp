#include "cmodeldiagnosticspanel.h"
#include "live2dstruct/cmodeldocument.h"
#include <QBrush>
#include <QColor>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>
#include <QWidget>

CModelDiagnosticsPanel::CModelDiagnosticsPanel(QWidget* parent)
    : QDockWidget(QStringLiteral("模型检查"), parent)
{
    auto* content = new QWidget(this);
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(8);
    auto* header = new QHBoxLayout();
    m_summary = new QLabel(content);
    auto* refresh = new QPushButton(QStringLiteral("重新检查"), content);
    header->addWidget(m_summary, 1);
    header->addWidget(refresh);
    layout->addLayout(header);
    m_list = new QListWidget(content);
    m_list->setSelectionMode(QAbstractItemView::NoSelection);
    layout->addWidget(m_list);
    setWidget(content);
    connect(refresh, &QPushButton::clicked, this, &CModelDiagnosticsPanel::Refresh);
    Refresh();
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
