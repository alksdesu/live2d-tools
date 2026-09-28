#include "cpsdbatchdialog.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QDialogButtonBox>
#include <QLabel>

CPsdBatchDialog::CPsdBatchDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle("批量导出 PSD");

    auto* layout = new QVBoxLayout(this);

    // Options
    m_checkAll = new QCheckBox("导出所有图层（包括隐藏）", this);
    m_checkFenzu = new QCheckBox("按分组导出", this);
    m_checkTexExtract = new QCheckBox("纹理采样导出（保留当前网格形状）", this);
    m_checkTexExtract->setToolTip(QStringLiteral("按网格形状采样图集，保留图层透明度与混合模式，不烘焙裁剪蒙版、乘色或屏幕色。此模式不恢复原始 PSD。"));
    m_checkCubism = new QCheckBox("Cubism 可导入模式", this);
    auto* optRow = new QHBoxLayout();
    optRow->addWidget(m_checkAll);
    optRow->addWidget(m_checkFenzu);
    optRow->addWidget(m_checkTexExtract);
    optRow->addWidget(m_checkCubism);
    optRow->addStretch(1);
    layout->addLayout(optRow);
    auto* coverageNotice = new QLabel(QStringLiteral("包含隐藏时也导出编辑器中隐藏的对象。当前姿态没有像素的对象会保留为空图层，不代表原始素材已恢复；具体对象与原因见导出结果。"), this);
    coverageNotice->setObjectName(QStringLiteral("psdCoverageNotice"));
    coverageNotice->setTextFormat(Qt::PlainText);
    coverageNotice->setWordWrap(true);
    layout->addWidget(coverageNotice);

    // List
    m_list = new QListWidget(this);
    m_list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    layout->addWidget(new QLabel("选择需要导出的 model3.json：", this));
    layout->addWidget(m_list, 1);

    // Selection helpers
    m_btnAll = new QPushButton("全选", this);
    m_btnNone = new QPushButton("全不选", this);
    m_btnInvert = new QPushButton("反选", this);
    auto* selectRow = new QHBoxLayout();
    selectRow->addWidget(m_btnAll);
    selectRow->addWidget(m_btnNone);
    selectRow->addWidget(m_btnInvert);
    selectRow->addStretch(1);
    layout->addLayout(selectRow);

    connect(m_btnAll, &QPushButton::clicked, this, &CPsdBatchDialog::onSelectAll);
    connect(m_btnNone, &QPushButton::clicked, this, &CPsdBatchDialog::onDeselectAll);
    connect(m_btnInvert, &QPushButton::clicked, this, &CPsdBatchDialog::onInvertSelect);

    auto* btns = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(btns, &QDialogButtonBox::accepted, this, &CPsdBatchDialog::onAccept);
    connect(btns, &QDialogButtonBox::rejected, this, &CPsdBatchDialog::onReject);
    layout->addWidget(btns);
}

void CPsdBatchDialog::setFileList(const QStringList &files)
{
    m_list->clear();
    for (const QString& f : files) {
        auto* item = new QListWidgetItem(f, m_list);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(Qt::Checked);
    }
}

QStringList CPsdBatchDialog::getSelectedFiles() const
{
    QStringList selected;
    for (int i = 0; i < m_list->count(); ++i) {
        auto* item = m_list->item(i);
        if (item->checkState() == Qt::Checked) {
            selected << item->text();
        }
    }
    return selected;
}

bool CPsdBatchDialog::isAllChecked() const
{
    return m_checkAll && m_checkAll->isChecked();
}

bool CPsdBatchDialog::isFenzuChecked() const
{
    return m_checkFenzu && m_checkFenzu->isChecked();
}

bool CPsdBatchDialog::isTexExtractChecked() const
{
    return m_checkTexExtract && m_checkTexExtract->isChecked();
}

bool CPsdBatchDialog::isCubismChecked() const
{
    return m_checkCubism && m_checkCubism->isChecked();
}

void CPsdBatchDialog::onSelectAll()
{
    for (int i = 0; i < m_list->count(); ++i) m_list->item(i)->setCheckState(Qt::Checked);
}

void CPsdBatchDialog::onDeselectAll()
{
    for (int i = 0; i < m_list->count(); ++i) m_list->item(i)->setCheckState(Qt::Unchecked);
}

void CPsdBatchDialog::onInvertSelect()
{
    for (int i = 0; i < m_list->count(); ++i) {
        auto* it = m_list->item(i);
        it->setCheckState(it->checkState() == Qt::Checked ? Qt::Unchecked : Qt::Checked);
    }
}

void CPsdBatchDialog::onAccept()
{
    accept();
}

void CPsdBatchDialog::onReject()
{
    reject();
}
