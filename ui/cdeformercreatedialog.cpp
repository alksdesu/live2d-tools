#include "cdeformercreatedialog.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QScopeGuard>
#include <QScrollArea>
#include <QShowEvent>
#include <QSpinBox>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <array>
#include <cmath>
#include <exception>

namespace {
constexpr qint64 MaximumCoordinates = 1000000;

bool fail(QString* error, const QString& message)
{
    if (error) *error = message;
    return false;
}

QLabel* label(QWidget* parent, const QString& text, const char* name)
{
    auto* result = new QLabel(text, parent);
    result->setObjectName(QString::fromLatin1(name));
    result->setTextFormat(Qt::PlainText);
    result->setWordWrap(true);
    result->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    result->setTextInteractionFlags(Qt::TextSelectableByMouse);
    return result;
}

bool validId(const QString& id)
{
    const auto encoded = id.toUtf8();
    return !id.trimmed().isEmpty() && encoded.size() < 64 && !encoded.contains('\0') && QString::fromUtf8(encoded) == id;
}

QString selectionText(const CDeformerCreateDialog::Context& context)
{
    if (!context.selectionSummary.isEmpty()) return context.selectionSummary;
    return context.children.isEmpty() ? QStringLiteral("未选择对象，将创建空变形器。")
        : QStringLiteral("将 %1 个选中对象放入新的变形器。后续仍可单独编辑。").arg(context.children.size());
}
}

class CDeformerCreateDialog::State
{
public:
    Context context;
    Prepare prepare;
    Submit submit;
    QLineEdit* id = nullptr;
    QComboBox* type = nullptr;
    QSpinBox *rows = nullptr, *columns = nullptr;
    QCheckBox* quad = nullptr;
    QWidget* warpFields = nullptr;
    QLabel *summary = nullptr, *error = nullptr, *gridHint = nullptr;
    QPushButton *create = nullptr, *retry = nullptr;
    quint64 revision = 0;
    bool busy = false, pending = false, queued = false, ready = false, closed = false;
    QString initialGridError;
};

CDeformerCreateDialog::CDeformerCreateDialog(Context context, Prepare prepare, Submit submit, QWidget* parent)
    : QDialog(parent), m_state(std::make_unique<State>())
{
    auto& state = *m_state;
    state.context = std::move(context); state.prepare = std::move(prepare); state.submit = std::move(submit);
    setObjectName(QStringLiteral("deformerCreateDialog"));
    setWindowTitle(QStringLiteral("创建变形器")); setModal(true);
    auto* outer = new QVBoxLayout(this);
    const int gap = std::max(fontMetrics().height() / 2, style()->pixelMetric(QStyle::PM_LayoutVerticalSpacing));
    outer->setSpacing(gap);
    auto* scroll = new QScrollArea(this); scroll->setObjectName(QStringLiteral("deformerCreateScroll"));
    scroll->setWidgetResizable(true); scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* body = new QWidget(scroll); auto* layout = new QVBoxLayout(body); layout->setSpacing(gap);
    auto* title = label(body, QStringLiteral("为选区建立新的控制层"), "deformerCreateTitle");
    auto titleFont = title->font(); titleFont.setBold(true); title->setFont(titleFont); layout->addWidget(title);
    layout->addWidget(label(body, selectionText(state.context), "deformerCreateSelection"));
    const auto part = state.context.partLabel.isEmpty() ? (state.context.partIndex < 0 ? QStringLiteral("无所属部件") : QString::number(state.context.partIndex)) : state.context.partLabel;
    const auto parentText = state.context.parentLabel.isEmpty() ? (state.context.parentId.isEmpty() ? QStringLiteral("根级") : state.context.parentId) : state.context.parentLabel;
    layout->addWidget(label(body, QStringLiteral("所属部件：%1\n父变形器：%2").arg(part, parentText), "deformerCreateContext"));
    auto* form = new QFormLayout; form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow); form->setSpacing(gap);
    state.id = new QLineEdit(state.context.suggestedId, body); state.id->setObjectName(QStringLiteral("deformerCreateId"));
    state.id->setAccessibleName(QStringLiteral("新变形器 ID")); state.id->setPlaceholderText(QStringLiteral("输入唯一 ID"));
    form->addRow(QStringLiteral("ID"), state.id);
    state.type = new QComboBox(body); state.type->setObjectName(QStringLiteral("deformerCreateType"));
    state.type->setMinimumContentsLength(1); state.type->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    state.type->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed); state.type->setMinimumWidth(0);
    state.type->addItem(QStringLiteral("Warp（变形网格）"), false); state.type->addItem(QStringLiteral("Rotation（旋转）"), true);
    state.type->setCurrentIndex(state.context.startWithRotation ? 1 : 0);
    state.type->setAccessibleName(QStringLiteral("变形器类型"));
    form->addRow(QStringLiteral("类型"), state.type); layout->addLayout(form);
    state.warpFields = new QWidget(body); auto* warpLayout = new QVBoxLayout(state.warpFields);
    warpLayout->setContentsMargins(0, 0, 0, 0); warpLayout->setSpacing(gap);
    auto* grid = new QFormLayout; grid->setRowWrapPolicy(QFormLayout::WrapLongRows);
    grid->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow); grid->setSpacing(gap);
    const auto spin = [&](const char* name, const QString& accessible, int value) {
        auto* box = new QSpinBox(state.warpFields); box->setObjectName(QString::fromLatin1(name));
        box->setAccessibleName(accessible); box->setRange(1, int(MaximumCoordinates - 1)); box->setKeyboardTracking(true);
        box->setValue(value); box->setEnabled(!state.context.lockWarpGrid);
        box->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed); box->setMinimumWidth(0);
        return box;
    };
    state.rows = spin("deformerCreateRows", QStringLiteral("网格行数"), state.context.warp.rows);
    state.columns = spin("deformerCreateColumns", QStringLiteral("网格列数"), state.context.warp.columns);
    if (state.rows->value() != state.context.warp.rows || state.columns->value() != state.context.warp.columns)
        state.initialGridError = QStringLiteral("提供的网格行列数无效，请重新分析选区。");
    grid->addRow(QStringLiteral("网格行数"), state.rows); grid->addRow(QStringLiteral("网格列数"), state.columns);
    warpLayout->addLayout(grid);
    state.quad = new QCheckBox(QStringLiteral("四边形插值"), state.warpFields);
    state.quad->setObjectName(QStringLiteral("deformerCreateQuad")); state.quad->setChecked(state.context.warp.quad);
    state.quad->setEnabled(!state.context.lockWarpGrid && state.context.mocVersion != 1);
    warpLayout->addWidget(state.quad);
    QString hint = state.context.lockWarpGrid
        ? (state.context.warpGridHint.isEmpty() ? QStringLiteral("网格与父变形器保持一致，以保留现有动画。") : state.context.warpGridHint)
        : QStringLiteral("行列决定控制点的密度。变形范围由当前选区确定。");
    if (state.context.mocVersion == 1) hint += QStringLiteral("\n此模型版本仅支持三角插值。");
    state.gridHint = label(state.warpFields, hint, "deformerCreateGridHint"); warpLayout->addWidget(state.gridHint);
    layout->addWidget(state.warpFields);
    state.summary = label(body, QStringLiteral("正在检查当前选区…"), "deformerCreateSummary"); layout->addWidget(state.summary);
    layout->addStretch(); scroll->setWidget(body); outer->addWidget(scroll, 1);
    state.error = label(this, {}, "deformerCreateError"); state.error->setAccessibleName(QStringLiteral("创建检查结果"));
    state.error->hide(); outer->addWidget(state.error);
    state.retry = new QPushButton(QStringLiteral("重新检查选区"), this);
    state.retry->setObjectName(QStringLiteral("deformerCreateRetry")); state.retry->setAutoDefault(false); state.retry->hide();
    outer->addWidget(state.retry, 0, Qt::AlignLeft);
    connect(state.retry, &QPushButton::clicked, this, &CDeformerCreateDialog::RefreshAvailability);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel | QDialogButtonBox::Ok, this);
    buttons->setObjectName(QStringLiteral("deformerCreateButtons"));
    state.create = buttons->button(QDialogButtonBox::Ok); state.create->setText(QStringLiteral("创建变形器"));
    state.create->setObjectName(QStringLiteral("primaryBtn"));
    state.create->style()->unpolish(state.create); state.create->style()->polish(state.create);
    state.create->setEnabled(false);
    state.create->setDefault(true); buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    buttons->button(QDialogButtonBox::Cancel)->setAutoDefault(false); outer->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &CDeformerCreateDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &CDeformerCreateDialog::reject);
    connect(state.id, &QLineEdit::textChanged, this, &CDeformerCreateDialog::DraftChanged);
    connect(state.type, &QComboBox::currentIndexChanged, this, &CDeformerCreateDialog::DraftChanged);
    connect(state.rows, &QSpinBox::valueChanged, this, &CDeformerCreateDialog::DraftChanged);
    connect(state.columns, &QSpinBox::valueChanged, this, &CDeformerCreateDialog::DraftChanged);
    for (auto* spinBox : {state.rows, state.columns})
        connect(spinBox->findChild<QLineEdit*>(), &QLineEdit::textChanged, this, &CDeformerCreateDialog::DraftChanged);
    connect(state.quad, &QCheckBox::toggled, this, &CDeformerCreateDialog::DraftChanged);
    state.warpFields->setVisible(!state.context.startWithRotation);
    const int em = fontMetrics().horizontalAdvance(QLatin1Char('M'));
    resize(em * 48, fontMetrics().height() * 32);
    setMinimumSize(em * 24, fontMetrics().height() * 16);
    state.id->selectAll(); state.id->setFocus(); QueueRefresh();
}

CDeformerCreateDialog::~CDeformerCreateDialog() = default;

bool CDeformerCreateDialog::BuildRequest(Builder::InsertRequest& request, QString* error) const
{
    if (error) error->clear();
    const auto& state = *m_state; const auto& context = state.context;
    const QString id = state.id->text();
    if (!validId(id)) return fail(error, QStringLiteral("ID 不能为空，且 UTF-8 长度须少于 64 字节。"));
    if (context.existingIds.contains(id)) return fail(error, QStringLiteral("此 ID 已存在，请使用其他名称。"));
    if (context.partIndex < -1 || context.mocVersion < 1 || context.mocVersion > 5 || context.parentId.contains(QChar::Null))
        return fail(error, QStringLiteral("选区所属部件、父级或模型版本无效，请重新选择。"));
    QSet<QPair<int, QString>> selected;
    for (const auto& child : context.children) {
        const auto key = qMakePair(int(child.type), child.id);
        if (child.type < Builder::Owner::ArtMesh || child.type > Builder::Owner::Rotation || !validId(child.id) || selected.contains(key))
            return fail(error, QStringLiteral("选区包含无效或重复对象，请重新选择。"));
        selected.insert(key);
    }
    Builder::InsertRequest candidate;
    candidate.id = id; candidate.partIndex = context.partIndex; candidate.parentId = context.parentId; candidate.children = context.children;
    if (state.type->currentData().toBool()) {
        for (float value : {context.rotation.originX, context.rotation.originY, context.rotation.angle, context.rotation.baseAngle, context.rotation.scale})
            if (!std::isfinite(value)) return fail(error, QStringLiteral("旋转中心或初始姿态无效，请重新分析选区。"));
        candidate.shape = context.rotation;
    } else {
        if (!state.initialGridError.isEmpty()) return fail(error, state.initialGridError);
        if (!state.rows->hasAcceptableInput() || !state.columns->hasAcceptableInput())
            return fail(error, QStringLiteral("请输入有效的网格行数和列数。"));
        Builder::Warp warp;
        warp.rows = state.rows->value(); warp.columns = state.columns->value(); warp.quad = state.quad->isChecked();
        if (context.lockWarpGrid && (warp.rows != context.warp.rows || warp.columns != context.warp.columns || warp.quad != context.warp.quad))
            return fail(error, QStringLiteral("当前网格已锁定，请保留与父变形器一致的设置。"));
        if (context.mocVersion == 1 && warp.quad)
            return fail(error, QStringLiteral("此模型版本不支持四边形插值，请由选区分析重新提供三角网格。"));
        const qint64 count = (qint64(warp.rows) + 1) * (qint64(warp.columns) + 1);
        if (count * 2 > MaximumCoordinates)
            return fail(error, QStringLiteral("网格控制点过多，请减少行数或列数。"));
        if (!context.warpBounds || !context.warpBounds->isValid())
            return fail(error, QStringLiteral("尚未确定变形范围，请重新分析选区。"));
        const auto& bounds = *context.warpBounds;
        for (double value : {bounds.left(), bounds.top(), bounds.right(), bounds.bottom()})
            if (!std::isfinite(value) || !std::isfinite(float(value)))
                return fail(error, QStringLiteral("变形范围超出可用坐标，请重新分析选区。"));
        warp.positions.reserve(count * 2);
        for (int row = 0; row <= warp.rows; ++row) for (int column = 0; column <= warp.columns; ++column) {
            warp.positions.append(float(bounds.left() + bounds.width() * (double(column) / warp.columns)));
            warp.positions.append(float(bounds.top() + bounds.height() * (double(row) / warp.rows)));
        }
        candidate.shape = std::move(warp);
    }
    request = std::move(candidate);
    return true;
}

void CDeformerCreateDialog::ShowError(const QString& error)
{
    m_state->error->setText(error); m_state->error->setVisible(!error.isEmpty());
}

void CDeformerCreateDialog::SetError(const QString& error) { ShowError(error); }

void CDeformerCreateDialog::DraftChanged()
{
    ++m_state->revision; m_state->ready = false; m_state->create->setEnabled(false);
    m_state->warpFields->setVisible(!m_state->type->currentData().toBool());
    ShowError({}); m_state->summary->setText(QStringLiteral("正在检查当前选区…")); m_state->retry->hide(); QueueRefresh();
}

void CDeformerCreateDialog::QueueRefresh()
{
    if (m_state->closed) return;
    if (m_state->busy) { m_state->pending = true; return; }
    if (m_state->queued) return;
    m_state->queued = true;
    QTimer::singleShot(0, this, [this] { m_state->queued = false; RefreshAvailability(); });
}

void CDeformerCreateDialog::RefreshAvailability() { Evaluate(false); }

void CDeformerCreateDialog::Evaluate(bool submit)
{
    if (m_state->busy || m_state->closed) return;
    m_state->ready = false; m_state->create->setEnabled(false); m_state->retry->hide(); m_state->retry->setEnabled(false);
    Builder::InsertRequest request; QString error;
    if (!BuildRequest(request, &error)) { m_state->summary->clear(); ShowError(error); return; }
    if (!m_state->prepare || !m_state->submit) {
        m_state->summary->clear(); ShowError(QStringLiteral("创建功能尚未就绪，请关闭窗口后重试。")); return;
    }
    const QPointer<CDeformerCreateDialog> alive(this);
    const auto prepare = m_state->prepare; const auto commit = m_state->submit;
    const auto revision = m_state->revision;
    m_state->busy = true;
    const auto finish = qScopeGuard([alive] {
        if (!alive) return;
        auto& state = *alive->m_state;
        state.busy = false;
        if (state.pending) { state.pending = false; alive->QueueRefresh(); }
        else state.create->setEnabled(state.ready);
        state.retry->setEnabled(!state.closed);
    });
    QString summary;
    bool allowed = false;
    try { allowed = prepare(request, &summary, &error); }
    catch (const std::exception&) { error = QStringLiteral("检查选区时发生错误，请重试。"); }
    catch (...) { error = QStringLiteral("检查选区时发生错误，请重试。"); }
    if (!alive) return;
    if (revision != m_state->revision) { m_state->pending = true; return; }
    m_state->summary->setText(summary);
    if (!allowed) { ShowError(error.isEmpty() ? QStringLiteral("当前选区暂不支持创建此变形器。") : error); m_state->retry->show(); return; }
    m_state->ready = true; ShowError({});
    if (!submit) return;
    QVector<QPair<QPointer<QWidget>, bool>> controls;
    const std::array<QWidget*, 5> inputs{m_state->id, m_state->type, m_state->rows, m_state->columns, m_state->quad};
    for (QWidget* widget : inputs) {
        controls.append({widget, widget->isEnabledTo(this)}); widget->setEnabled(false);
    }
    const auto restoreControls = qScopeGuard([controls] {
        for (const auto& control : controls) if (control.first) control.first->setEnabled(control.second);
    });
    bool accepted = false; error.clear();
    try { accepted = commit(request, &error); }
    catch (const std::exception&) { error = QStringLiteral("创建未完成，输入已保留，请重试。"); }
    catch (...) { error = QStringLiteral("创建未完成，输入已保留，请重试。"); }
    if (!alive) return;
    if (!accepted) { ShowError(error.isEmpty() ? QStringLiteral("创建未完成，输入已保留，请重试。") : error); return; }
    m_state->ready = false; m_state->closed = true; m_state->pending = false;
    QDialog::accept();
}

void CDeformerCreateDialog::accept() { Evaluate(true); }
void CDeformerCreateDialog::reject()
{
    if (m_state->busy || m_state->closed) return;
    m_state->closed = true; m_state->ready = false; m_state->create->setEnabled(false);
    QDialog::reject();
}

void CDeformerCreateDialog::showEvent(QShowEvent* event)
{
    QDialog::showEvent(event);
    m_state->closed = false;
    QueueRefresh();
}
