#include "cpsdprintdialog.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QDialogButtonBox>

CPsdPrintDialog::CPsdPrintDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle("导出 PSD 选项");

    m_IsPrint = false;
    m_IsFenZu = false;
    m_IsAll = false;
    m_IsTexExtract = false;
    m_IsSnapshot = false;
    m_IsCubismCompat = false;

    auto* layout = new QVBoxLayout(this);

    auto* pathRow = new QHBoxLayout();
    auto* pathTitle = new QLabel("保存路径:", this);
    m_pathLabel = new QLabel("", this);
    m_pathLabel->setTextFormat(Qt::PlainText);
    m_pathLabel->setWordWrap(true);
    m_pathLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    pathRow->addWidget(pathTitle);
    pathRow->addWidget(m_pathLabel, 1);
    layout->addLayout(pathRow);

    m_checkAll = new QCheckBox("导出所有图层（包括隐藏）", this);
    m_checkFenzu = new QCheckBox("按分组导出", this);
    m_checkTexExtract = new QCheckBox("纹理采样导出（保留当前网格形状）", this);
    m_checkTexExtract->setToolTip(QStringLiteral("按网格形状采样图集，保留图层透明度与混合模式，不烘焙裁剪蒙版、乘色或屏幕色。此模式不恢复原始 PSD。"));
    m_checkSnapshot = new QCheckBox("使用当前参数（禁用自动效果）", this);
    m_checkCubism = new QCheckBox("Cubism 可导入模式（sRGB / 图层名去重）", this);
    layout->addWidget(m_checkAll);
    layout->addWidget(m_checkFenzu);
    layout->addWidget(m_checkTexExtract);
    layout->addWidget(m_checkSnapshot);
    layout->addWidget(m_checkCubism);
    auto* coverageNotice = new QLabel(QStringLiteral("包含隐藏时也导出编辑器中隐藏的对象。当前姿态没有像素的对象会保留为空图层，不代表原始素材已恢复；具体对象与原因见导出结果。"), this);
    coverageNotice->setObjectName(QStringLiteral("psdCoverageNotice"));
    coverageNotice->setTextFormat(Qt::PlainText);
    coverageNotice->setWordWrap(true);
    layout->addWidget(coverageNotice);

    auto* btns = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(btns, &QDialogButtonBox::accepted, this, &CPsdPrintDialog::on_pushButton_clicked);
    connect(btns, &QDialogButtonBox::rejected, this, &CPsdPrintDialog::on_pushButton_2_clicked);
    layout->addWidget(btns);
}

CPsdPrintDialog::~CPsdPrintDialog() {}

void CPsdPrintDialog::SetName(const QString &name)
{
    if (m_pathLabel) m_pathLabel->setText(name);
}

void CPsdPrintDialog::on_pushButton_clicked()
{
    m_IsPrint = true;
    m_IsFenZu = (m_checkFenzu ? m_checkFenzu->isChecked() : false);
    m_IsAll = (m_checkAll ? m_checkAll->isChecked() : false);
    m_IsTexExtract = (m_checkTexExtract ? m_checkTexExtract->isChecked() : false);
    m_IsSnapshot = (m_checkSnapshot ? m_checkSnapshot->isChecked() : false);
    m_IsCubismCompat = (m_checkCubism ? m_checkCubism->isChecked() : false);
    accept();
}

void CPsdPrintDialog::on_pushButton_2_clicked()
{
    m_IsPrint = false;
    reject();
}
