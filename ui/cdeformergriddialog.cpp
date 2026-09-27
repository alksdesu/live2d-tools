#include "cdeformergriddialog.h"
#include "theme.h"
#include <QAbstractListModel>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QResizeEvent>
#include <QScopeGuard>
#include <QScrollArea>
#include <QScrollBar>
#include <QShowEvent>
#include <QSpinBox>
#include <QStyle>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <exception>
#include <limits>

namespace {
constexpr qsizetype MaximumPoints = 2004002;
constexpr qsizetype MaximumContours = 2000000;
struct Geometry {
    QPainterPath before, after;
    QSizeF span;
    bool empty = true;
};
class Keyforms final : public QAbstractListModel
{
public:
    Keyforms(int count, QObject* parent) : QAbstractListModel(parent), m_count(std::max(0, count)) {}
    int rowCount(const QModelIndex& parent = {}) const override { return parent.isValid() ? 0 : std::max(1, m_count); }
    QVariant data(const QModelIndex& index, int role) const override {
        if (!index.isValid() || index.row() < 0 || index.row() >= rowCount()) return {};
        if (role == Qt::DisplayRole) return m_count ? QStringLiteral("关键形 %1").arg(index.row()) : QStringLiteral("基础网格");
        if (role == Qt::UserRole) return m_count ? index.row() : -1;
        return {};
    }
private:
    int m_count;
};
class ControlsScroll final : public QScrollArea
{
public:
    using QScrollArea::QScrollArea;
    QSize minimumSizeHint() const override {
        auto hint = QScrollArea::minimumSizeHint();
        if (widget()) hint.setWidth(widget()->minimumSizeHint().width() + verticalScrollBar()->sizeHint().width());
        return hint;
    }
};
class GridCanvas final : public QWidget
{
public:
    explicit GridCanvas(QWidget* parent) : QWidget(parent) {
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        setToolTip(QStringLiteral("滚轮缩放，拖动查看；不会改变模型"));
    }
    QSize minimumSizeHint() const override { return {fontMetrics().averageCharWidth() * 20, fontMetrics().height() * 9}; }
    void SetGeometry(Geometry geometry) { m_geometry = std::move(geometry); m_prepared = true; Fit(); update(); }
    void SetEmptyText(const QString& text) { m_emptyText = text; }
    void Fit() { m_fit = true; m_stable = {}; QueueFit(); }
protected:
    void showEvent(QShowEvent* event) override { QWidget::showEvent(event); QueueFit(); }
    void resizeEvent(QResizeEvent* event) override {
        QWidget::resizeEvent(event);
        if (!m_fit && event->oldSize().isValid()) {
            m_offset += QPointF(event->size().width() - event->oldSize().width(), event->size().height() - event->oldSize().height()) / 2;
            if (m_pan) { m_start = m_offset; m_press = mapFromGlobal(m_pointer); }
        }
        QueueFit();
    }
    void changeEvent(QEvent* event) override { QWidget::changeEvent(event); QueueFit(); update(); }
    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() != Qt::LeftButton && event->button() != Qt::MiddleButton) return;
        m_fit = false; m_pan = true; m_press = event->position(); m_start = m_offset;
        m_pointer = event->globalPosition().toPoint(); setCursor(Qt::ClosedHandCursor); event->accept();
    }
    void mouseMoveEvent(QMouseEvent* event) override {
        if (!m_pan) return;
        m_pointer = event->globalPosition().toPoint(); m_offset = m_start + event->position() - m_press; update(); event->accept();
    }
    void mouseReleaseEvent(QMouseEvent* event) override { if (m_pan) { m_pan = false; unsetCursor(); event->accept(); } }
    void hideEvent(QHideEvent* event) override { m_pan = false; unsetCursor(); QWidget::hideEvent(event); }
    void wheelEvent(QWheelEvent* event) override {
        if (!event->angleDelta().y() || m_geometry.empty) return;
        m_fit = false;
        const auto point = (event->position() - m_offset) / m_scale;
        const auto next = std::clamp(m_scale * std::pow(1.0015, event->angleDelta().y()), 1e-100, 1e100);
        m_offset = event->position() - point * next; m_scale = next; update(); event->accept();
    }
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        const auto colors = Theme::tokens(palette().color(QPalette::Window).lightness() < 128);
        painter.fillRect(rect(), QColor(colors.value("@surface@")));
        if (m_geometry.empty) {
            painter.setPen(QColor(colors.value("@textMuted@")));
            painter.drawText(rect().adjusted(12, 12, -12, -12), Qt::AlignCenter | Qt::TextWordWrap,
                m_prepared ? m_emptyText : QStringLiteral("暂无可显示的预览\n调整分段后，点击“更新预览”"));
            return;
        }
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setClipRect(rect());
        painter.setTransform(QTransform(m_scale, 0, 0, -m_scale, m_offset.x(), m_offset.y()));
        const qreal width = std::max(1.0, 2.0 / devicePixelRatioF());
        QPen before(QColor(colors.value("@textMuted@")), width / m_scale, Qt::DashLine);
        painter.setPen(before); painter.drawPath(m_geometry.before);
        QPen after(QColor(colors.value("@accentInk@")), width / m_scale);
        painter.setPen(after); painter.drawPath(m_geometry.after);
    }
private:
    void QueueFit() {
        if (!m_fit || m_queued || !isVisible()) return;
        m_queued = true;
        QTimer::singleShot(0, this, [this] {
            m_queued = false;
            if (!m_fit || !isVisible()) return;
            if (m_stable != size()) { m_stable = size(); QueueFit(); return; }
            const auto margin = fontMetrics().height() * 1.5;
            const double width = std::max(m_geometry.span.width(), m_geometry.span.height() > 0 ? m_geometry.span.height() * 1e-6 : 1.0);
            const double height = std::max(m_geometry.span.height(), m_geometry.span.width() > 0 ? m_geometry.span.width() * 1e-6 : 1.0);
            m_scale = std::clamp(std::min(std::max(1.0, this->width() - margin * 2) / width,
                                        std::max(1.0, this->height() - margin * 2) / height), 1e-100, 1e100);
            m_offset = QPointF(this->width(), this->height()) / 2; update();
        });
    }
    Geometry m_geometry;
    QString m_emptyText;
    bool m_prepared = false;
    QPointF m_offset, m_start, m_press;
    QPoint m_pointer;
    QSize m_stable;
    double m_scale = 1;
    bool m_fit = true, m_queued = false, m_pan = false;
};

bool buildGeometry(const CDeformerGridDialog::Context& context, int rows, int columns, int keyform,
              const CDeformerGridDialog::Preview& preview, Geometry& grid, Geometry& contours, QString* error)
{
    const auto fail = [&](const QString& text) { if (error) *error = text; return false; };
    if (!std::isfinite(preview.maximumDisplacementPixels) || preview.maximumDisplacementPixels < 0)
        return fail(QStringLiteral("预览位移结果无效，请重新计算"));
    if (preview.beforeGrid.size() > MaximumPoints || preview.afterGrid.size() > MaximumPoints ||
        preview.beforeContours.size() > MaximumContours || preview.afterContours.size() > MaximumContours)
        return fail(QStringLiteral("预览数据超过显示预算，请减少转换分段"));
    if (keyform >= 0 && (preview.beforeGrid.size() != (qint64(context.rows) + 1) * (qint64(context.columns) + 1) ||
        preview.afterGrid.size() != (qint64(rows) + 1) * (qint64(columns) + 1)))
        return fail(QStringLiteral("预览控制点数量与转换分段不匹配"));
    if (keyform < 0 && (!preview.beforeGrid.isEmpty() || !preview.afterGrid.isEmpty()))
        return fail(QStringLiteral("没有普通关键形时不能使用未经确认的基础网格"));
    const auto prepare = [&](const auto& visit, auto build, Geometry& result) {
        bool found = false; double left = 0, right = 0, top = 0, bottom = 0;
        const auto include = [&](QPointF point) {
            if (!std::isfinite(point.x()) || !std::isfinite(point.y()) || std::abs(point.x()) > 1e100 || std::abs(point.y()) > 1e100) return false;
            if (!found) { left = right = point.x(); top = bottom = point.y(); found = true; }
            else { left = std::min(left, point.x()); right = std::max(right, point.x()); top = std::min(top, point.y()); bottom = std::max(bottom, point.y()); }
            return true;
        };
        if (!visit(include)) return false;
        result.empty = !found;
        if (!found) return true;
        const double span = std::max(right - left, bottom - top);
        const double divisor = span > 0 ? span : 1;
        result.span = QSizeF((right - left) / divisor, (bottom - top) / divisor);
        build(QPointF(left + (right - left) / 2, top + (bottom - top) / 2), divisor, result);
        return true;
    };
    const auto gridVisit = [&](auto include) {
        for (const auto* points : {&preview.beforeGrid, &preview.afterGrid})
            for (const auto& point : *points) if (!include(point)) return false;
        return true;
    };
    const auto gridBuild = [&](QPointF center, double divisor, Geometry& result) {
        const auto local = [&](QPointF point) { return QPointF((point.x() - center.x()) / divisor, (point.y() - center.y()) / divisor); };
        const auto path = [&](const QVector<QPointF>& points, int r, int c, QPainterPath& out) {
            if (points.isEmpty()) return;
            for (int row = 0; row <= r; ++row) {
                out.moveTo(local(points[row * (c + 1)]));
                for (int column = 1; column <= c; ++column) out.lineTo(local(points[row * (c + 1) + column]));
            }
            for (int column = 0; column <= c; ++column) {
                out.moveTo(local(points[column]));
                for (int row = 1; row <= r; ++row) out.lineTo(local(points[row * (c + 1) + column]));
            }
        };
        path(preview.beforeGrid, context.rows, context.columns, result.before);
        path(preview.afterGrid, rows, columns, result.after);
    };
    if (!prepare(gridVisit, gridBuild, grid)) return fail(QStringLiteral("网格预览包含无效坐标"));
    const auto contourVisit = [&](auto include) {
        for (const auto* lines : {&preview.beforeContours, &preview.afterContours})
            for (const auto& line : *lines) if (!include(line.p1()) || !include(line.p2())) return false;
        return true;
    };
    const auto contourBuild = [&](QPointF center, double divisor, Geometry& result) {
        const auto path = [center, divisor](const QVector<QLineF>& lines, QPainterPath& out) {
            const auto local = [&](QPointF point) { return QPointF((point.x() - center.x()) / divisor, (point.y() - center.y()) / divisor); };
            for (const auto& line : lines) { out.moveTo(local(line.p1())); out.lineTo(local(line.p2())); }
        };
        path(preview.beforeContours, result.before); path(preview.afterContours, result.after);
    };
    if (!prepare(contourVisit, contourBuild, contours)) return fail(QStringLiteral("模型边界预览包含无效坐标"));
    return true;
}
}

class CDeformerGridDialog::State
{
public:
    Context context;
    Prepare prepare;
    Apply apply;
    QSpinBox *rows = nullptr, *columns = nullptr;
    QComboBox* keyform = nullptr;
    QPushButton *update = nullptr, *applyButton = nullptr;
    QLabel *status = nullptr, *counts = nullptr;
    QPlainTextEdit* details = nullptr;
    QTabWidget* tabs = nullptr;
    GridCanvas *grid = nullptr, *contours = nullptr;
    QString error, summary, limitations;
    quint64 revision = 0;
    bool preparing = false, submitting = false, ready = false, closed = false;
    int acceptedRows = -1, acceptedColumns = -1, acceptedKeyform = -2;
    double displacement = 0;
};

CDeformerGridDialog::CDeformerGridDialog(Context context, Prepare prepare, Apply apply, QWidget* parent)
    : QDialog(parent), m_state(std::make_unique<State>())
{
    auto& state = *m_state; state.context = std::move(context); state.prepare = std::move(prepare); state.apply = std::move(apply);
    setObjectName(QStringLiteral("deformerGridDialog")); setWindowTitle(QStringLiteral("调整转换网格")); setModal(true);
    auto* outer = new QVBoxLayout(this);
    auto* body = new QHBoxLayout;
    outer->addLayout(body, 1);
    auto* scroll = new ControlsScroll(this); scroll->setObjectName(QStringLiteral("deformerGridControls"));
    scroll->setWidgetResizable(true); scroll->setFrameShape(QFrame::NoFrame);
    scroll->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    auto* content = new QWidget(scroll); auto* controls = new QVBoxLayout(content);
    auto* form = new QFormLayout; form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow); form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    const auto identity = [content](const QString& value, const char* name) {
        auto* field = new QLineEdit(value, content); field->setObjectName(QString::fromLatin1(name)); field->setReadOnly(true);
        field->setMinimumWidth(0); field->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed); field->setCursorPosition(0); return field;
    };
    form->addRow(QStringLiteral("名称"), identity(state.context.name.isEmpty() ? state.context.id : state.context.name, "deformerGridName"));
    form->addRow(QStringLiteral("ID"), identity(state.context.id, "deformerGridId"));
    auto* dimensions = new QWidget(content); auto* dimensionsLayout = new QHBoxLayout(dimensions); dimensionsLayout->setContentsMargins(0, 0, 0, 0);
    const auto spin = [&](const QString& text, const char* name, int value) {
        auto* label = new QLabel(text, dimensions); dimensionsLayout->addWidget(label);
        auto* input = new QSpinBox(dimensions); input->setObjectName(QString::fromLatin1(name)); input->setRange(1, 1000);
        input->setKeyboardTracking(false); input->setValue(value); input->setAccessibleName(text + QStringLiteral("转换分段"));
        input->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed); dimensionsLayout->addWidget(input, 1); label->setBuddy(input); return input;
    };
    state.columns = spin(QStringLiteral("水平"), "deformerGridColumns", state.context.columns);
    state.rows = spin(QStringLiteral("垂直"), "deformerGridRows", state.context.rows);
    form->addRow(QStringLiteral("转换分段"), dimensions);
    state.keyform = new QComboBox(content); state.keyform->setObjectName(QStringLiteral("deformerGridKeyform"));
    state.keyform->setModel(new Keyforms(state.context.ordinaryKeyforms, state.keyform));
    state.keyform->setMinimumContentsLength(1); state.keyform->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    state.keyform->setCurrentIndex(std::clamp(state.context.initialKeyform, 0, state.keyform->count() - 1));
    form->addRow(QStringLiteral("预览关键形"), state.keyform); controls->addLayout(form);
    state.counts = new QLabel(content); state.counts->setObjectName(QStringLiteral("deformerGridCounts")); state.counts->setWordWrap(true); state.counts->setTextFormat(Qt::PlainText);
    state.counts->setMinimumWidth(0); state.counts->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred); controls->addWidget(state.counts);
    controls->addStretch();
    scroll->setWidget(content); body->addWidget(scroll);
    auto* preview = new QWidget(this);
    auto* layout = new QVBoxLayout(preview); layout->setContentsMargins(0, 0, 0, 0);
    body->addWidget(preview, 1);
    auto* tools = new QHBoxLayout;
    auto* legend = new QLabel(QStringLiteral("原：虚线　新：实线"), this); legend->setWordWrap(true); tools->addWidget(legend, 1);
    auto* fit = new QPushButton(QStringLiteral("适应视图"), this); fit->setObjectName(QStringLiteral("deformerGridFit")); fit->setAutoDefault(false); tools->addWidget(fit); layout->addLayout(tools);
    state.tabs = new QTabWidget(this); state.tabs->setObjectName(QStringLiteral("deformerGridPreviewTabs")); Theme::setDocumentMode(*state.tabs);
    state.grid = new GridCanvas(state.tabs); state.grid->setObjectName(QStringLiteral("deformerGridCanvas"));
    state.contours = new GridCanvas(state.tabs); state.contours->setObjectName(QStringLiteral("deformerGridContours"));
    state.grid->SetEmptyText(QStringLiteral("此预览没有可显示的普通关键形网格"));
    state.contours->SetEmptyText(QStringLiteral("当前冻结姿态没有可显示的模型边界"));
    state.tabs->addTab(state.grid, QStringLiteral("网格")); state.tabs->addTab(state.contours, QStringLiteral("模型边界"));
    state.tabs->setTabToolTip(0, QStringLiteral("所选普通关键形的父级坐标，Y 轴向上"));
    state.tabs->setTabToolTip(1, QStringLiteral("打开窗口时冻结参数姿态的模型坐标"));
    layout->addWidget(state.tabs, 1);
    connect(fit, &QPushButton::clicked, this, [this] { (m_state->tabs->currentIndex() ? m_state->contours : m_state->grid)->Fit(); });
    state.status = new QLabel(this); state.status->setObjectName(QStringLiteral("deformerGridStatus")); state.status->setTextFormat(Qt::PlainText); state.status->setWordWrap(true);
    state.status->setMinimumWidth(0); state.status->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred); layout->addWidget(state.status);
    state.details = new QPlainTextEdit(this); state.details->setObjectName(QStringLiteral("deformerGridDetails")); state.details->setReadOnly(true);
    state.details->setMaximumHeight(fontMetrics().height() * 5); state.details->setMinimumHeight(fontMetrics().height() * 3); layout->addWidget(state.details);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Apply | QDialogButtonBox::Cancel, this); buttons->setObjectName(QStringLiteral("deformerGridButtons"));
    state.applyButton = buttons->button(QDialogButtonBox::Apply); state.applyButton->setObjectName(QStringLiteral("primaryBtn")); state.applyButton->setText(QStringLiteral("应用到全部关键形")); state.applyButton->setAutoDefault(false);
    state.applyButton->style()->unpolish(state.applyButton); state.applyButton->style()->polish(state.applyButton);
    state.update = buttons->addButton(QStringLiteral("更新预览"), QDialogButtonBox::ActionRole); state.update->setObjectName(QStringLiteral("deformerGridUpdate")); state.update->setAutoDefault(false);
    auto* cancel = buttons->button(QDialogButtonBox::Cancel); cancel->setText(QStringLiteral("取消")); cancel->setDefault(true);
    connect(state.update, &QPushButton::clicked, this, &CDeformerGridDialog::UpdatePreview);
    connect(state.applyButton, &QPushButton::clicked, this, &QDialog::accept); connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject); outer->addWidget(buttons);
    for (auto* input : {state.rows, state.columns}) {
        connect(input, &QSpinBox::valueChanged, this, [this] { DraftChanged(); });
        connect(input->findChild<QLineEdit*>(), &QLineEdit::textChanged, this, [this] { DraftChanged(); });
    }
    connect(state.keyform, &QComboBox::currentIndexChanged, this, [this] { DraftChanged(); });
    resize(fontMetrics().averageCharWidth() * 90, fontMetrics().height() * 38);
    RefreshControls();
}

CDeformerGridDialog::~CDeformerGridDialog() = default;
bool CDeformerGridDialog::IsPreparing() const { return m_state->preparing; }
bool CDeformerGridDialog::IsSubmitting() const { return m_state->submitting; }
QString CDeformerGridDialog::Error() const { return m_state->error; }
bool CDeformerGridDialog::IsReady() const {
    const auto& state = *m_state;
    return state.ready && !state.closed && state.rows->hasAcceptableInput() && state.columns->hasAcceptableInput() &&
        state.rows->value() == state.acceptedRows && state.columns->value() == state.acceptedColumns && state.keyform->currentData().toInt() == state.acceptedKeyform;
}
void CDeformerGridDialog::DraftChanged() { ++m_state->revision; m_state->ready = false; m_state->error.clear(); RefreshControls(); }
void CDeformerGridDialog::ShowError(const QString& error) { m_state->error = error; RefreshControls(); }
void CDeformerGridDialog::RefreshControls()
{
    auto& state = *m_state;
    const bool idle = !state.preparing && !state.submitting && !state.closed;
    const bool valid = !state.context.id.isEmpty() && state.context.rows > 0 && state.context.columns > 0 &&
        state.context.ordinaryKeyforms >= 0 && state.context.blendKeyforms >= 0 && state.rows->hasAcceptableInput() && state.columns->hasAcceptableInput();
    state.rows->setEnabled(idle); state.columns->setEnabled(idle); state.keyform->setEnabled(idle);
    state.update->setEnabled(idle && valid && bool(state.prepare));
    const bool changed = state.rows->value() != state.context.rows || state.columns->value() != state.context.columns;
    state.applyButton->setEnabled(idle && IsReady() && changed && bool(state.apply));
    state.counts->setText(QStringLiteral("原网格 %1 × %2 → %3 个目标控制点 · %4 个普通关键形、%5 个融合关键形均会更新 · %6")
        .arg(state.context.columns).arg(state.context.rows).arg((qint64(state.rows->value()) + 1) * (state.columns->value() + 1))
        .arg(state.context.ordinaryKeyforms).arg(state.context.blendKeyforms).arg(state.context.quad ? QStringLiteral("四边形插值") : QStringLiteral("三角插值")));
    QString status;
    if (state.preparing) status = QStringLiteral("正在计算预览…");
    else if (state.submitting) status = QStringLiteral("正在应用转换网格…");
    else if (!state.error.isEmpty()) status = QStringLiteral("未能完成操作，请查看下方说明");
    else if (!valid) status = QStringLiteral("请输入有效的转换分段，或重新打开此对象");
    else if (IsReady()) status = QStringLiteral("当前冻结姿态最大位移：%1 像素%2").arg(QString::number(state.displacement, 'g', 7), changed ? QString() : QStringLiteral(" · 分段未改变，无需应用"));
    else status = state.acceptedRows < 0 ? QStringLiteral("点击“更新预览”查看结果；尚未修改模型") : QStringLiteral("输入已改变，当前图像为上次结果；请更新预览");
    state.status->setText(status);
    QStringList detail;
    if (!state.error.isEmpty()) detail.append(state.error);
    if (!state.summary.isEmpty()) detail.append(IsReady() ? state.summary : QStringLiteral("上次成功预览：") + state.summary);
    detail.append(QStringLiteral("网格页预览所选普通关键形的父级坐标。模型边界比较打开窗口时冻结的参数姿态；其它姿态仍可能出现差异。"));
    if (!state.limitations.isEmpty()) detail.append(state.limitations);
    const auto text = detail.join(QLatin1Char('\n'));
    if (state.details->toPlainText() != text) state.details->setPlainText(text);
}

void CDeformerGridDialog::UpdatePreview()
{
    auto& state = *m_state;
    if (state.preparing || state.submitting || state.closed) return;
    if (!state.rows->hasAcceptableInput() || !state.columns->hasAcceptableInput()) { ShowError(QStringLiteral("请输入 1 到 1000 之间的整数分段")); return; }
    state.rows->interpretText(); state.columns->interpretText();
    if (!state.prepare || state.context.id.isEmpty() || state.context.rows < 1 || state.context.columns < 1 || state.context.ordinaryKeyforms < 0 || state.context.blendKeyforms < 0) {
        ShowError(QStringLiteral("转换网格来源尚未就绪，请关闭后重新打开")); return;
    }
    const int rows = state.rows->value(), columns = state.columns->value(), keyform = state.keyform->currentData().toInt();
    const auto prepare = state.prepare; const auto revision = state.revision;
    const QPointer<CDeformerGridDialog> dialog(this);
    state.ready = false; state.preparing = true; state.error.clear(); RefreshControls();
    const auto finish = qScopeGuard([dialog] { if (dialog) { dialog->m_state->preparing = false; dialog->RefreshControls(); } });
    Preview preview; Geometry grid, contours; QString error; bool success = false;
    try {
        success = prepare(rows, columns, keyform, preview, &error);
        if (!dialog || state.closed || state.revision != revision) return;
        if (success) success = buildGeometry(state.context, rows, columns, keyform, preview, grid, contours, &error);
    } catch (const std::exception&) { success = false; error = QStringLiteral("预览计算未完成，请减少分段或重试"); }
    catch (...) { success = false; error = QStringLiteral("预览计算未完成，请重试"); }
    if (!dialog || state.closed || state.revision != revision) return;
    if (!success) { state.error = error.isEmpty() ? QStringLiteral("无法生成预览，请检查来源或重试") : error; return; }
    state.grid->SetGeometry(std::move(grid)); state.contours->SetGeometry(std::move(contours));
    state.summary = QString(preview.summary.constData(), preview.summary.size()); state.limitations = QString(preview.limitations.constData(), preview.limitations.size());
    state.displacement = preview.maximumDisplacementPixels;
    state.acceptedRows = rows; state.acceptedColumns = columns; state.acceptedKeyform = keyform; state.ready = true;
}

void CDeformerGridDialog::done(int result)
{
    auto& state = *m_state;
    if (state.submitting || state.closed) return;
    if (result != Accepted) { state.closed = true; state.ready = false; ++state.revision; QDialog::done(result); return; }
    if (state.preparing || !IsReady() || !state.apply) return;
    const int rows = state.rows->value(), columns = state.columns->value();
    if (rows == state.context.rows && columns == state.context.columns) return;
    const auto apply = state.apply;
    const QPointer<CDeformerGridDialog> dialog(this);
    state.submitting = true; state.error.clear(); RefreshControls();
    const auto finish = qScopeGuard([dialog] { if (dialog) { dialog->m_state->submitting = false; dialog->RefreshControls(); } });
    QString error; bool success = false;
    try { success = apply(rows, columns, &error); }
    catch (const std::exception&) { error = QStringLiteral("应用未完成，输入与预览已保留，请重试"); }
    catch (...) { error = QStringLiteral("应用未完成，请重试"); }
    if (!dialog) return;
    if (!success) { state.error = error.isEmpty() ? QStringLiteral("应用未完成，输入与预览已保留，请重试") : error; return; }
    state.closed = true; state.ready = false; QDialog::done(Accepted);
}
