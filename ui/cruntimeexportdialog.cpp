#include "cruntimeexportdialog.h"
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QSignalBlocker>
#include <QPushButton>
#include <QScrollArea>
#include <QStandardPaths>
#include <QStyle>
#include <QVBoxLayout>

namespace {
QLabel* textLabel(const QString& text, QWidget* parent)
{
    auto* label = new QLabel(text, parent);
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    return label;
}

QString existingDirectory(QString path)
{
    while (!path.isEmpty()) {
        const QFileInfo info(path);
        if (info.isDir()) return info.absoluteFilePath();
        const QString parent = info.dir().absolutePath();
        if (parent == path) break;
        path = parent;
    }
    return QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
}
}

CRuntimeExportDialog::CRuntimeExportDialog(const QString& currentModelPath, bool hasTimeline, QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("导出运行时模型包"));
    setObjectName(QStringLiteral("runtimeExportDialog"));
    setModal(true);
    auto* layout = new QVBoxLayout(this);
    m_scroll = new QScrollArea(this);
    m_scroll->setObjectName(QStringLiteral("runtimeExportScroll"));
    m_scroll->setWidgetResizable(true);
    m_scroll->setFocusPolicy(Qt::NoFocus);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* content = new QWidget(m_scroll);
    auto* body = new QVBoxLayout(content);
    auto* introduction = textLabel(QStringLiteral("生成一个可独立加载的新模型目录，原文件保持不变。"), content);
    body->addWidget(introduction);

    auto* destinationLabel = new QLabel(QStringLiteral("目标文件夹"), content);
    body->addWidget(destinationLabel);
    auto* targetRow = new QHBoxLayout;
    m_target = new QLineEdit(content);
    m_target->setObjectName(QStringLiteral("runtimeExportTarget"));
    m_target->setPlaceholderText(QStringLiteral("输入新文件夹的完整路径"));
    m_target->setAccessibleName(QStringLiteral("目标文件夹完整路径"));
    m_target->setClearButtonEnabled(true);
    destinationLabel->setBuddy(m_target);
    auto* browse = new QPushButton(QStringLiteral("选择父目录…"), content);
    browse->setObjectName(QStringLiteral("runtimeExportBrowse"));
    browse->setFocusPolicy(Qt::StrongFocus);
    browse->setAutoDefault(false);
    targetRow->addWidget(m_target, 1);
    targetRow->addWidget(browse);
    body->addLayout(targetRow);
    body->addWidget(textLabel(QStringLiteral("目标文件夹必须尚未存在。选择父目录后，可以在上方修改文件夹名称。"), content));

    auto* optionsLabel = new QLabel(QStringLiteral("内容选项"), content);
    QFont sectionFont = optionsLabel->font();
    sectionFont.setBold(true);
    optionsLabel->setFont(sectionFont);
    body->addWidget(optionsLabel);
    m_currentValues = new QCheckBox(QStringLiteral("将当前参数值设为默认值"), content);
    m_currentValues->setObjectName(QStringLiteral("runtimeExportCurrentValues"));
    m_currentValues->setFocusPolicy(Qt::StrongFocus);
    body->addWidget(m_currentValues);
    body->addWidget(textLabel(QStringLiteral("不勾选时保留模型定义的默认值。"), content));
    m_timeline = new QCheckBox(QStringLiteral("将当前时间轴另存为动作"), content);
    m_timeline->setObjectName(QStringLiteral("runtimeExportTimeline"));
    m_timeline->setFocusPolicy(Qt::StrongFocus);
    m_timeline->setEnabled(hasTimeline);
    body->addWidget(m_timeline);
    body->addWidget(textLabel(hasTimeline ? QStringLiteral("在模型包中加入一份独立动作，保留原动作文件。")
                                         : QStringLiteral("当前时间轴没有可导出的轨道。"), content));
    body->addWidget(textLabel(QStringLiteral("包含当前已确认的物理、Pose 和网格标签草稿、原动作与表情，以及当前有效图集。"), content));
    body->addWidget(textLabel(QStringLiteral("播放状态、对象锁定和界面布局属于编辑器或宿主应用设置。"), content));
    m_artwork = new QCheckBox(QStringLiteral("包含可继续编辑的来源素材"), content);
    m_artwork->setObjectName(QStringLiteral("runtimeExportArtwork")); m_artwork->setFocusPolicy(Qt::StrongFocus); m_artwork->setEnabled(false); m_artwork->hide(); body->addWidget(m_artwork);
    m_artworkSummary = textLabel({}, content); m_artworkSummary->setObjectName(QStringLiteral("runtimeExportArtworkSummary")); m_artworkSummary->hide(); body->addWidget(m_artworkSummary);
    m_artworkNotice = textLabel({}, content);
    m_artworkNotice->setObjectName(QStringLiteral("runtimeExportArtworkNotice")); m_artworkNotice->hide(); body->addWidget(m_artworkNotice);
    connect(m_artwork, &QCheckBox::toggled, this, &CRuntimeExportDialog::RefreshArtworkNotice);
    body->addStretch();
    m_scroll->setWidget(content);
    layout->addWidget(m_scroll, 1);

    m_error = textLabel(QString(), this);
    m_error->setObjectName(QStringLiteral("runtimeExportError"));
    m_error->setAccessibleName(QStringLiteral("导出目录提示"));
    m_error->hide();
    layout->addWidget(m_error);
    m_buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    m_buttons->setObjectName(QStringLiteral("runtimeExportButtons"));
    auto* submit = m_buttons->button(QDialogButtonBox::Ok);
    submit->setObjectName(QStringLiteral("primaryBtn"));
    submit->style()->unpolish(submit);
    submit->style()->polish(submit);
    submit->setText(QStringLiteral("导出模型包"));
    submit->setDefault(true);
    submit->setFocusPolicy(Qt::StrongFocus);
    m_buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    m_buttons->button(QDialogButtonBox::Cancel)->setFocusPolicy(Qt::StrongFocus);
    layout->addWidget(m_buttons);
    connect(m_target, &QLineEdit::textChanged, this, &CRuntimeExportDialog::RefreshValidation);
    connect(browse, &QPushButton::clicked, this, &CRuntimeExportDialog::ChooseParentDirectory);
    connect(m_buttons, &QDialogButtonBox::accepted, this, &CRuntimeExportDialog::accept);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &CRuntimeExportDialog::reject);
    setTabOrder(m_target, browse);
    setTabOrder(browse, m_currentValues);
    setTabOrder(m_currentValues, m_timeline);
    setTabOrder(m_timeline, m_artwork);
    setTabOrder(m_artwork, submit);
    setTabOrder(submit, m_buttons->button(QDialogButtonBox::Cancel));

    const QFileInfo model(currentModelPath);
    m_defaultName = model.fileName();
    const QString modelSuffix = QStringLiteral(".model3.json");
    if (m_defaultName.endsWith(modelSuffix, Qt::CaseInsensitive)) m_defaultName.chop(modelSuffix.size());
    else m_defaultName = model.completeBaseName();
    if (m_defaultName.isEmpty()) m_defaultName = QStringLiteral("model");
    m_defaultName += QStringLiteral("-runtime");
    auto directory = model.absoluteDir();
    if (currentModelPath.isEmpty() || !directory.cdUp())
        directory = QDir(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation));
    SetInitialDirectory(directory.filePath(m_defaultName));
    resize(fontMetrics().averageCharWidth() * 78, fontMetrics().height() * 25);
    m_target->setFocus();
}

QString CRuntimeExportDialog::TargetDirectory() const
{
    const auto path = QDir::fromNativeSeparators(m_target->text());
    return path.isEmpty() ? QString() : QDir::cleanPath(path);
}

bool CRuntimeExportDialog::UseCurrentValues() const { return m_currentValues->isChecked(); }
bool CRuntimeExportDialog::IncludeTimeline() const { return m_timeline->isEnabled() && m_timeline->isChecked(); }
bool CRuntimeExportDialog::IncludeArtwork() const { return m_artworkRevisions > 0 && m_artwork->isChecked(); }

void CRuntimeExportDialog::SetInitialDirectory(const QString& targetDirectory)
{
    m_target->setText(QDir::toNativeSeparators(targetDirectory));
    m_target->setCursorPosition(m_target->text().size());
    RefreshValidation();
}

void CRuntimeExportDialog::SetArtworkNotice(bool visible)
{
    m_legacyArtworkNotice = visible; RefreshArtworkNotice();
}

void CRuntimeExportDialog::SetArtworkSummary(int revisionCount, qint64 materialBytes)
{
    const bool available = revisionCount > 0 && materialBytes >= 0;
    const bool wasAvailable = m_artworkRevisions > 0;
    m_artworkRevisions = available ? revisionCount : 0; m_artworkBytes = available ? materialBytes : 0; m_legacyArtworkNotice = false;
    const QSignalBlocker blocker(m_artwork);
    if (!available || !wasAvailable) m_artwork->setChecked(false);
    m_artwork->setEnabled(available); m_artwork->setVisible(available); m_artworkSummary->setVisible(available);
    const QLocale locale;
    m_artworkSummary->setText(available ? QStringLiteral("%1 个来源版本，素材大小约 %2。包含旧版本与未关联来源。")
        .arg(locale.toString(m_artworkRevisions), locale.formattedDataSize(m_artworkBytes, 1, QLocale::DataSizeTraditionalFormat)) : QString());
    RefreshArtworkNotice();
}

void CRuntimeExportDialog::RefreshArtworkNotice()
{
    m_artworkNotice->setText(IncludeArtwork()
        ? QStringLiteral("模型包将保存来源版本与关联，可继续编辑来源素材。对象锁定与视图等编辑状态仍请保存工作会话。")
        : QStringLiteral("本次只导出可运行模型；需要继续编辑来源素材时，请另存工作会话。"));
    m_artworkNotice->setVisible(m_artworkRevisions > 0 || m_legacyArtworkNotice);
}

QString CRuntimeExportDialog::ValidateTarget() const
{
    const auto entered = m_target->text();
    if (entered.trimmed().isEmpty()) return QStringLiteral("请输入新文件夹的完整路径。");
    if (entered.contains(QChar::Null) || !QDir::isAbsolutePath(QDir::fromNativeSeparators(entered)))
        return QStringLiteral("请输入有效的绝对路径，或使用“选择父目录”。");
    const QFileInfo destination(TargetDirectory());
    if (destination.exists() || destination.isSymLink()) return QStringLiteral("目标已存在，请使用一个新的文件夹名称。");
    if (!destination.dir().exists()) return QStringLiteral("父目录不存在，请先选择一个已有的父目录。");
    return {};
}

void CRuntimeExportDialog::RefreshValidation()
{
    const auto error = ValidateTarget();
    m_error->setText(error);
    m_error->setVisible(!error.isEmpty());
    m_buttons->button(QDialogButtonBox::Ok)->setEnabled(error.isEmpty());
}

void CRuntimeExportDialog::ChooseParentDirectory()
{
    const QFileInfo destination(TargetDirectory());
    const QString parent = QFileDialog::getExistingDirectory(this, QStringLiteral("选择模型包的父目录"),
                                                            existingDirectory(destination.dir().absolutePath()));
    if (parent.isEmpty()) return;
    const QString name = destination.fileName().isEmpty() ? m_defaultName : destination.fileName();
    SetInitialDirectory(QDir(parent).filePath(name));
    m_target->setFocus();
    m_target->selectAll();
}

void CRuntimeExportDialog::accept() { done(QDialog::Accepted); }

void CRuntimeExportDialog::done(int result)
{
    if (result == QDialog::Accepted) {
        RefreshValidation();
        if (!ValidateTarget().isEmpty()) {
            m_scroll->ensureWidgetVisible(m_target);
            m_target->setFocus();
            m_target->selectAll();
            return;
        }
    }
    QDialog::done(result);
}
