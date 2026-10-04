#include "cphysicsbakedialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStyle>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <limits>

namespace {
QLabel* label(const QString& text, QWidget* parent)
{
    auto* result = new QLabel(text, parent);
    result->setTextFormat(Qt::PlainText);
    result->setWordWrap(true);
    result->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    return result;
}

bool validId(const QString& id) { return !id.isEmpty() && !id.contains(QChar::Null); }
QString number(double value) { return QString::number(value, 'g', 12); }
}

CPhysicsBakeDialog::CPhysicsBakeDialog(QMap<QString, QString> outputNames, const CMotionTimeline& timeline, QWidget* parent,
                                       double physicsFps)
    : QDialog(parent), m_originalPoints(timeline.TotalPointCount()), m_sceneDuration(timeline.Timing().duration),
      m_hasAnimation(timeline.HasAnimation())
{
    setObjectName(QStringLiteral("physicsBakeDialog"));
    setWindowTitle(QStringLiteral("将物理烘焙为关键帧"));
    setModal(true);
    m_sourceValid = !timeline.IsRecording() && std::isfinite(m_sceneDuration) && m_sceneDuration >= 0;
    for (auto it = timeline.Tracks().cbegin(); it != timeline.Tracks().cend(); ++it)
        if (it.key().target == CMotionTarget::Parameter) m_replacedPointCounts.insert(it.key().id, it.value().size());
    const double sourceFps = timeline.Timing().fps;
    if (std::isfinite(sourceFps) && sourceFps >= .2 && sourceFps <= std::numeric_limits<float>::max()) m_initialFps = sourceFps;
    if (std::isfinite(physicsFps) && physicsFps >= .2 && physicsFps <= std::numeric_limits<float>::max()) m_physicsFps = physicsFps;
    if (std::isfinite(m_sceneDuration) && m_sceneDuration > 0 && m_sceneDuration <= std::numeric_limits<float>::max())
        m_initialDuration = m_sceneDuration;

    auto* outer = new QVBoxLayout(this);
    m_scroll = new QScrollArea(this);
    m_scroll->setObjectName(QStringLiteral("physicsBakeScroll"));
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setFocusPolicy(Qt::NoFocus);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* content = new QWidget(m_scroll);
    auto* body = new QVBoxLayout(content);
    body->addWidget(label(QStringLiteral("把独立物理计算的结果写入当前时间轴，预览模型保持原状。"), content));
    auto* outputTitle = label(QStringLiteral("要烘焙的输出参数"), content);
    outputTitle->setObjectName(QStringLiteral("titleLabel"));
    body->addWidget(outputTitle);
    m_outputs = new QListWidget(content);
    m_outputs->setObjectName(QStringLiteral("physicsBakeOutputs"));
    m_outputs->setAccessibleName(QStringLiteral("选择要烘焙的物理输出参数"));
    m_outputs->setFocusPolicy(Qt::StrongFocus);
    m_outputs->setSelectionMode(QAbstractItemView::SingleSelection);
    m_outputs->setTextElideMode(Qt::ElideMiddle);
    m_outputs->setMinimumWidth(0);
    m_outputs->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    for (auto it = outputNames.cbegin(); it != outputNames.cend(); ++it) {
        if (!validId(it.key())) m_sourceValid = false;
        const auto name = it.value().trimmed();
        const auto text = name.isEmpty() || name == it.key() ? it.key() : QStringLiteral("%1（%2）").arg(name, it.key());
        auto* item = new QListWidgetItem(text, m_outputs);
        item->setData(Qt::UserRole, it.key());
        item->setData(Qt::AccessibleTextRole, text);
        item->setToolTip(text);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(Qt::Checked);
    }
    const int rows = std::clamp(m_outputs->count(), 2, 6);
    m_outputs->ensurePolished();
    m_outputs->doItemsLayout();
    const int rowHeight = std::max(m_outputs->fontMetrics().height(), m_outputs->sizeHintForRow(0));
    m_outputs->setFixedHeight(rowHeight * rows + 2 * m_outputs->frameWidth());
    if (m_outputs->count()) m_outputs->setCurrentRow(0);
    outputTitle->setBuddy(m_outputs);
    body->addWidget(m_outputs);
    auto* selection = new QHBoxLayout;
    auto* selectAll = new QPushButton(QStringLiteral("全选"), content);
    selectAll->setObjectName(QStringLiteral("physicsBakeSelectAll"));
    auto* clear = new QPushButton(QStringLiteral("清空选择"), content);
    clear->setObjectName(QStringLiteral("physicsBakeClear"));
    for (auto* button : {selectAll, clear}) {
        button->setAutoDefault(false);
        button->setFocusPolicy(Qt::StrongFocus);
        selection->addWidget(button);
    }
    selection->addStretch();
    body->addLayout(selection);
    const auto checkAll = [this](Qt::CheckState state) {
        const QSignalBlocker blocker(m_outputs);
        for (int i = 0; i < m_outputs->count(); ++i) m_outputs->item(i)->setCheckState(state);
        RefreshValidation();
    };
    connect(selectAll, &QPushButton::clicked, this, [checkAll]() { checkAll(Qt::Checked); });
    connect(clear, &QPushButton::clicked, this, [checkAll]() { checkAll(Qt::Unchecked); });

    m_newAnimation = new QCheckBox(QStringLiteral("新建独立动画"), content);
    m_newAnimation->setObjectName(QStringLiteral("physicsBakeNewAnimation"));
    m_newAnimation->setFocusPolicy(Qt::StrongFocus);
    body->addWidget(m_newAnimation);
    auto* form = new QFormLayout;
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    m_fps = new QDoubleSpinBox(content);
    m_fps->setObjectName(QStringLiteral("physicsBakeFps"));
    m_fps->setAccessibleName(QStringLiteral("采样帧率"));
    m_fps->setDecimals(6);
    m_fps->setRange(.2, std::numeric_limits<float>::max());
    m_fps->setValue(m_initialFps);
    m_fps->setSuffix(QStringLiteral(" 帧/秒"));
    m_fpsSource = new QComboBox(content);
    m_fpsSource->setObjectName(QStringLiteral("physicsBakeFpsSource"));
    m_fpsSource->addItem(QStringLiteral("场景帧率"), int(Options::FpsSource::Scene));
    if (m_physicsFps > 0) m_fpsSource->addItem(QStringLiteral("物理计算帧率"), int(Options::FpsSource::Physics));
    m_fpsSource->addItem(QStringLiteral("自定义"), int(Options::FpsSource::Custom));
    m_duration = new QDoubleSpinBox(content);
    m_duration->setObjectName(QStringLiteral("physicsBakeDuration"));
    m_duration->setAccessibleName(QStringLiteral("烘焙时长"));
    m_duration->setDecimals(6);
    m_duration->setRange(0, std::numeric_limits<float>::max());
    m_duration->setValue(m_initialDuration);
    m_duration->setSuffix(QStringLiteral(" 秒"));
    m_duration->setSingleStep(.1);
    m_warmup = new QSpinBox(content);
    m_warmup->setObjectName(QStringLiteral("physicsBakeWarmup"));
    m_warmup->setAccessibleName(QStringLiteral("完整动画预热圈数"));
    m_warmup->setRange(0, 3);
    m_warmup->setValue(1);
    m_warmup->setSuffix(QStringLiteral(" 圈"));
    for (QAbstractSpinBox* spin : QList<QAbstractSpinBox*>{m_fps, m_duration, m_warmup}) {
        spin->installEventFilter(this);
        spin->findChild<QLineEdit*>()->installEventFilter(this);
    }
    form->addRow(QStringLiteral("采样率来源"), m_fpsSource);
    form->addRow(QStringLiteral("采样帧率"), m_fps);
    form->addRow(QStringLiteral("烘焙时长"), m_duration);
    form->addRow(QStringLiteral("预热"), m_warmup);
    body->addLayout(form);
    body->addWidget(label(QStringLiteral("从 0 秒开始，末帧向上对齐采样网格。采样帧率不会改变场景帧率（%1 帧/秒）。").arg(number(sourceFps)), content));
    body->addWidget(label(QStringLiteral("预热会先循环播放完整动画，再记录结果；摆锤状态会影响首帧。空动画按烘焙时长预热，预热帧不写入曲线。"), content));
    m_destinationHint = label({}, content);
    m_destinationHint->setObjectName(QStringLiteral("physicsBakeDestinationHint"));
    body->addWidget(m_destinationHint);
    body->addStretch();
    m_scroll->setWidget(content);
    outer->addWidget(m_scroll, 1);

    m_summary = label({}, this);
    m_summary->setObjectName(QStringLiteral("physicsBakeSummary"));
    m_summary->setAccessibleName(QStringLiteral("烘焙范围与关键点预算"));
    outer->addWidget(m_summary);
    m_error = label({}, this);
    m_error->setObjectName(QStringLiteral("physicsBakeError"));
    m_error->setAccessibleName(QStringLiteral("烘焙输入提示"));
    outer->addWidget(m_error);
    m_buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    m_buttons->setObjectName(QStringLiteral("physicsBakeButtons"));
    auto* start = m_buttons->button(QDialogButtonBox::Ok);
    start->setObjectName(QStringLiteral("primaryBtn"));
    start->setText(QStringLiteral("开始烘焙"));
    start->setDefault(true);
    start->setFocusPolicy(Qt::StrongFocus);
    start->style()->unpolish(start);
    start->style()->polish(start);
    auto* cancel = m_buttons->button(QDialogButtonBox::Cancel);
    cancel->setText(QStringLiteral("取消"));
    cancel->setFocusPolicy(Qt::StrongFocus);
    outer->addWidget(m_buttons);
    connect(m_buttons, &QDialogButtonBox::accepted, this, &CPhysicsBakeDialog::accept);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &CPhysicsBakeDialog::reject);
    connect(m_outputs, &QListWidget::itemChanged, this, [this]() { RefreshValidation(); });
    connect(m_newAnimation, &QCheckBox::toggled, this, [this]() { RefreshValidation(); });
    connect(m_fps, &QDoubleSpinBox::valueChanged, this, [this]() {
        if (m_fpsSource && m_fpsSource->currentData().toInt() != int(Options::FpsSource::Custom)) {
            const QSignalBlocker blocker(m_fpsSource);
            const auto custom = m_fpsSource->findData(int(Options::FpsSource::Custom));
            if (custom >= 0) m_fpsSource->setCurrentIndex(custom);
        }
        m_fpsChanged = true; RefreshValidation();
    });
    connect(m_duration, &QDoubleSpinBox::valueChanged, this, [this]() { m_durationChanged = true; RefreshValidation(); });
    connect(m_warmup, &QSpinBox::valueChanged, this, [this]() { RefreshValidation(); });
    connect(m_fpsSource, &QComboBox::currentIndexChanged, this, [this](int) {
        const auto source = m_fpsSource->currentData().toInt();
        const double value = source == int(Options::FpsSource::Physics) ? m_physicsFps : m_initialFps;
        const QSignalBlocker blocker(m_fps);
        if (source != int(Options::FpsSource::Custom)) m_fps->setValue(value);
        RefreshValidation();
    });
    connect(m_fps->findChild<QLineEdit*>(), &QLineEdit::textEdited, this, [this]() { m_fpsChanged = true; RefreshValidation(); });
    connect(m_duration->findChild<QLineEdit*>(), &QLineEdit::textEdited, this, [this]() { m_durationChanged = true; RefreshValidation(); });
    connect(m_warmup->findChild<QLineEdit*>(), &QLineEdit::textEdited, this, [this]() { RefreshValidation(); });
    setTabOrder(m_outputs, selectAll);
    setTabOrder(selectAll, clear);
    setTabOrder(clear, m_newAnimation);
    setTabOrder(m_newAnimation, m_fpsSource);
    setTabOrder(m_fpsSource, m_fps);
    setTabOrder(m_fps, m_duration);
    setTabOrder(m_duration, m_warmup);
    setTabOrder(m_warmup, start);
    setTabOrder(start, cancel);
    RefreshValidation();
    resize(fontMetrics().averageCharWidth() * 82, fontMetrics().height() * 35);
    m_outputs->setFocus();
}

CPhysicsBakeDialog::Options CPhysicsBakeDialog::Selection() const
{
    Options options;
    for (int i = 0; i < m_outputs->count(); ++i)
        if (m_outputs->item(i)->checkState() == Qt::Checked) options.outputIds.insert(m_outputs->item(i)->data(Qt::UserRole).toString());
    const auto source = static_cast<Options::FpsSource>(m_fpsSource->currentData().toInt());
    options.fpsSource = source;
    options.fps = source == Options::FpsSource::Physics ? m_physicsFps
        : source == Options::FpsSource::Custom ? m_fps->value() : m_initialFps;
    options.duration = m_durationChanged ? m_duration->value() : m_initialDuration;
    options.warmupCycles = m_warmup->value();
    options.newAnimation = m_newAnimation->isChecked();
    return options;
}

QString CPhysicsBakeDialog::Validate(Options& options, qint64& pointsPerOutput, qint64& retainedPoints, double& endTime) const
{
    options = Selection();
    if (!m_sourceValid || m_originalPoints < 0 || m_originalPoints > CMotionTimeline::MaximumPointCount)
        return QStringLiteral("当前时间轴或输出参数无效；请先停止录制并检查输入。");
    if (options.outputIds.isEmpty()) return QStringLiteral("请至少选择一个物理输出参数。");
    if (!m_fps->hasAcceptableInput() || !m_duration->hasAcceptableInput() || !m_warmup->hasAcceptableInput())
        return QStringLiteral("请填写有效的帧率、时长和预热圈数。");
    if (!std::isfinite(options.fps) || options.fps < .2 || !std::isfinite(options.duration) || options.duration < 0)
        return QStringLiteral("采样帧率不能低于 0.2，时长不能为负数。");
    const double intervals = std::ceil(options.duration * options.fps);
    if (!std::isfinite(intervals) || intervals >= CMotionTimeline::MaximumPointCount)
        return QStringLiteral("单条曲线超过 100000 个关键点，请降低帧率或缩短时长。");
    pointsPerOutput = qint64(intervals) + 1;
    retainedPoints = options.newAnimation ? 0 : m_originalPoints;
    if (!options.newAnimation)
        for (const auto& id : options.outputIds) retainedPoints -= m_replacedPointCounts.value(id);
    endTime = intervals / options.fps;
    if (options.outputIds.size() > (CMotionTimeline::MaximumPointCount - retainedPoints) / pointsPerOutput)
        return QStringLiteral("烘焙结果与保留轨道合计超过 100000 个关键点，请减少输出、降低帧率或缩短时长。");
    auto conflicts = options.outputIds;
    conflicts.intersect(m_conflictingOutputs);
    if (!options.newAnimation && !conflicts.isEmpty()) {
        auto names = conflicts.values();
        names.sort();
        return QStringLiteral("%1 受全局眨眼或口型轨道作用，直接合并会重复应用效果。请选择“新建独立动画”，或取消选择这些输出。").arg(names.join(QStringLiteral("、")));
    }
    const double period = m_hasAnimation && m_sceneDuration > 0 ? m_sceneDuration : options.duration;
    if (options.warmupCycles && std::ceil(period * options.fps) > CMotionTimeline::MaximumPointCount)
        return QStringLiteral("完整动画的预热超过 100000 帧，请降低采样帧率或将预热设为 0 圈。");
    return {};
}

void CPhysicsBakeDialog::RefreshValidation()
{
    Options options;
    qint64 points = 0, retained = 0;
    double end = 0;
    const QString error = Validate(options, points, retained, end);
    m_destinationHint->setText(options.newAnimation
        ? QStringLiteral("只将所选输出写入新动画，不复制原动画的其它轨道与事件。原动画仍作为本次计算输入，完成后可撤销恢复。")
        : QStringLiteral("将替换所选输出参数的整条曲线，其它轨道和事件保留。完成后可以撤销。"));
    if (points) {
        const qint64 generated = points * options.outputIds.size();
        m_summary->setText(QStringLiteral("实际范围 0–%1 秒 · 每条 %2 点\n新增 %3 + 保留 %4 = %5 / %6 点")
            .arg(number(end)).arg(points).arg(generated).arg(retained).arg(generated + retained).arg(CMotionTimeline::MaximumPointCount));
    } else {
        m_summary->setText(QStringLiteral("所选 %1 个输出 · 最多 %2 个关键点（含保留轨道）")
            .arg(options.outputIds.size()).arg(CMotionTimeline::MaximumPointCount));
    }
    m_error->setText(error);
    m_error->setVisible(!error.isEmpty());
    m_buttons->button(QDialogButtonBox::Ok)->setEnabled(error.isEmpty());
}

void CPhysicsBakeDialog::accept() { done(QDialog::Accepted); }

void CPhysicsBakeDialog::done(int result)
{
    if (result == QDialog::Accepted) {
        RefreshValidation();
        Options options;
        qint64 points = 0, retained = 0;
        double end = 0;
        if (!Validate(options, points, retained, end).isEmpty()) {
            QWidget* target = m_outputs;
            if (!m_fps->hasAcceptableInput()) target = m_fps;
            else if (!m_duration->hasAcceptableInput()) target = m_duration;
            else if (!m_warmup->hasAcceptableInput()) target = m_warmup;
            m_scroll->ensureWidgetVisible(target);
            target->setFocus();
            return;
        }
    }
    QDialog::done(result);
}

bool CPhysicsBakeDialog::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() == QEvent::KeyPress) {
        const auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) {
            auto* spin = qobject_cast<QAbstractSpinBox*>(watched);
            if (!spin) spin = qobject_cast<QAbstractSpinBox*>(watched->parent());
            if (spin && !spin->hasAcceptableInput()) {
                RefreshValidation();
                return true;
            }
        }
    }
    return QDialog::eventFilter(watched, event);
}

void CPhysicsBakeDialog::SetConflictingOutputs(const QSet<QString>& ids)
{
    m_conflictingOutputs = ids;
    RefreshValidation();
}
