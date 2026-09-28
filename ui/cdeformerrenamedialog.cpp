#include "cdeformerrenamedialog.h"
#include "live2dstruct/cmodeldisplaynames.h"
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QScopeGuard>
#include <QScrollArea>
#include <QStyle>
#include <QVBoxLayout>
#include <exception>

CDeformerRenameDialog::CDeformerRenameDialog(const QString& id, const QString& type,
                                             const QString& currentName, QWidget* parent)
    : QDialog(parent)
{
    setObjectName(QStringLiteral("deformerRenameDialog"));
    setWindowTitle(QStringLiteral("重命名对象"));
    setModal(true);
    auto* layout = new QVBoxLayout(this);
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* content = new QWidget(scroll);
    auto* body = new QVBoxLayout(content);
    auto* form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    const auto label = [content](const QString& text, const char* name) {
        auto* result = new QLabel(text, content);
        result->setObjectName(QString::fromLatin1(name));
        result->setTextFormat(Qt::PlainText);
        result->setWordWrap(true);
        result->setTextInteractionFlags(Qt::TextSelectableByMouse);
        result->setMinimumWidth(0);
        result->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        return result;
    };
    m_name = new QLineEdit(currentName, content);
    m_name->setObjectName(QStringLiteral("deformerRenameName"));
    m_name->setAccessibleName(QStringLiteral("对象显示名称"));
    m_name->setMaxLength(live2dstruct::CModelDisplayNames::MaximumNameLength);
    form->addRow(QStringLiteral("名称"), m_name);
    auto* identifier = new QLineEdit(id, content);
    identifier->setObjectName(QStringLiteral("deformerRenameId"));
    identifier->setAccessibleName(QStringLiteral("稳定 ID"));
    identifier->setReadOnly(true);
    identifier->setCursorPosition(0);
    form->addRow(QStringLiteral("ID"), identifier);
    form->addRow(QStringLiteral("类型"), label(type == QLatin1String("warp") ?
        QStringLiteral("弯曲变形器（Warp）") : type == QLatin1String("rotation") ? QStringLiteral("旋转变形器（Rotation）") :
        type == QLatin1String("artmesh") ? QStringLiteral("网格（ArtMesh）") :
        type == QLatin1String("part") ? QStringLiteral("部件（Part）") : type, "deformerRenameType"));
    body->addLayout(form);
    body->addWidget(label(QStringLiteral("只修改显示名称。ID、层级、参数绑定和素材关联保持不变。"), "deformerRenameHint"));
    m_error = label({}, "deformerRenameError");
    body->addWidget(m_error);
    body->addStretch();
    scroll->setWidget(content);
    layout->addWidget(scroll, 1);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
    buttons->setObjectName(QStringLiteral("deformerRenameButtons"));
    m_save = buttons->button(QDialogButtonBox::Save);
    m_save->setText(QStringLiteral("保存"));
    m_save->setObjectName(QStringLiteral("primaryBtn"));
    m_save->style()->unpolish(m_save); m_save->style()->polish(m_save);
    m_save->setDefault(true);
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
    connect(m_name, &QLineEdit::textChanged, this, [this] {
        QString error;
        live2dstruct::CModelDisplayNames::ValidateName(Name(), &error);
        m_error->setText(error);
        Refresh();
    });
    resize(fontMetrics().horizontalAdvance(QString(45, QLatin1Char('0'))), fontMetrics().lineSpacing() * 17);
    m_name->selectAll();
    m_name->setFocus();
    Refresh();
}

void CDeformerRenameDialog::SetSubmissionHandler(Submit handler) { m_submit = std::move(handler); Refresh(); }
QString CDeformerRenameDialog::Name() const { return m_name->text(); }
QString CDeformerRenameDialog::Error() const { return m_error->text(); }

void CDeformerRenameDialog::Refresh()
{
    m_save->setEnabled(!m_submitting && bool(m_submit) &&
        live2dstruct::CModelDisplayNames::ValidateName(m_name->text()));
    m_error->setToolTip(QStringLiteral("<qt>%1</qt>").arg(m_error->text().toHtmlEscaped()));
    m_error->setVisible(!m_error->text().isEmpty());
}

void CDeformerRenameDialog::done(int result)
{
    if (m_submitting) return;
    if (result != Accepted) { QDialog::done(result); return; }
    const QPointer<CDeformerRenameDialog> dialog(this);
    const auto submit = m_submit;
    if (!submit) return;
    m_submitting = true;
    const auto finish = qScopeGuard([dialog] { if (dialog) { dialog->m_submitting = false; dialog->Refresh(); } });
    Refresh();
    QString error;
    if (!live2dstruct::CModelDisplayNames::ValidateName(Name(), &error)) { m_error->setText(error); return; }
    bool success = false;
    try { success = submit(Name(), &error); }
    catch (const std::exception&) { error = QStringLiteral("未能保存名称，原名称保持不变"); }
    if (!dialog) return;
    if (!success) {
        m_error->setText(error.isEmpty() ? QStringLiteral("未能保存名称，请检查输入后重试") : error);
        return;
    }
    m_error->clear();
    QDialog::done(Accepted);
}
