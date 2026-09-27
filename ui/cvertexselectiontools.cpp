#include "cvertexselectiontools.h"
#include <QActionGroup>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleValidator>
#include <QEvent>
#include <QFormLayout>
#include <QFrame>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPainter>
#include <QPointer>
#include <QScreen>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QStyle>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

namespace {
using Selection = live2dstruct::CVertexSelection;
class StatusLabel : public QLabel
{
public:
    using QLabel::QLabel;
    QSize minimumSizeHint() const override { return {0, fontMetrics().height()}; }
    QSize sizeHint() const override { return {0, fontMetrics().height()}; }
protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this); painter.setPen(palette().color(foregroundRole()));
        painter.drawText(contentsRect(), Qt::AlignVCenter | Qt::AlignLeft,
                         fontMetrics().elidedText(text(), Qt::ElideRight, contentsRect().width()));
    }
};
QString number(double value, const QLocale& locale) { return locale.toString(value, 'g', 17); }
QString escaped(const QString& value) { return QStringLiteral("<qt>%1</qt>").arg(value.toHtmlEscaped()); }
QString combineText(Selection::Combine combine, bool brush)
{
    if (combine == Selection::Combine::Add) return brush ? QStringLiteral("增加影响") : QStringLiteral("追加选择");
    if (combine == Selection::Combine::Subtract) return brush ? QStringLiteral("擦除影响") : QStringLiteral("减少选择");
    return brush ? QStringLiteral("重画影响") : QStringLiteral("替换选择");
}
}

CVertexSelectionTools::CVertexSelectionTools(QWidget* parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("vertexSelectionTools"));
    auto* layout = new QVBoxLayout(this); layout->setContentsMargins(0, 0, 0, 0);
    m_toolbar = new QToolBar(QStringLiteral("点编辑"), this); m_toolbar->setObjectName(QStringLiteral("vertexSelectionToolbar"));
    m_toolbar->setMovable(false); m_toolbar->setFloatable(false); m_toolbar->setToolButtonStyle(Qt::ToolButtonTextOnly); layout->addWidget(m_toolbar);
    m_edit = m_toolbar->addAction(QStringLiteral("编辑点")); m_edit->setObjectName(QStringLiteral("vertexEditing")); m_edit->setCheckable(true);
    connect(m_edit, &QAction::triggered, this, [this](bool enabled) {
        const QPointer<CVertexSelectionTools> alive(this); emit EditingRequested(enabled); if (alive) Refresh();
    });
    m_tools = new QActionGroup(this); m_tools->setExclusive(true);
    const QStringList names{QStringLiteral("选择/移动"), QStringLiteral("框选"), QStringLiteral("套索"), QStringLiteral("影响笔刷")};
    const QStringList tips{QStringLiteral("点选或拖动影响点；空白处拖动可框选"), QStringLiteral("拖出矩形选择点，不移动模型"),
        QStringLiteral("围出任意形状选择点，不移动模型"), QStringLiteral("绘制临时影响权重；再用选择/移动工具拖动")};
    for (int index = 0; index < names.size(); ++index) {
        auto* action = m_toolbar->addAction(names[index]); action->setObjectName(QStringLiteral("vertexTool%1").arg(index));
        action->setCheckable(true); action->setData(index); action->setToolTip(tips[index]); m_tools->addAction(action);
    }
    connect(m_tools, &QActionGroup::triggered, this, [this](QAction* action) {
        auto settings = m_settings; settings.tool = Selection::Tool(action->data().toInt()); RequestSettings(settings);
    });
    m_combineMenu = new QMenu(this); m_combineMenu->setObjectName(QStringLiteral("vertexCombineMenu"));
    m_combinations = new QActionGroup(this); m_combinations->setExclusive(true);
    for (int index = 0; index < 3; ++index) {
        auto* action = m_combineMenu->addAction(QString()); action->setObjectName(QStringLiteral("vertexCombine%1").arg(index));
        action->setCheckable(true); action->setData(index); m_combinations->addAction(action);
    }
    connect(m_combinations, &QActionGroup::triggered, this, [this](QAction* action) {
        auto settings = m_settings; settings.combine = Selection::Combine(action->data().toInt()); RequestSettings(settings);
    });
    auto* combine = m_toolbar->addAction(QString()); combine->setObjectName(QStringLiteral("vertexCombine")); combine->setMenu(m_combineMenu);
    m_combineButton = qobject_cast<QToolButton*>(m_toolbar->widgetForAction(combine)); m_combineButton->setPopupMode(QToolButton::InstantPopup);
    m_soft = m_toolbar->addAction(QStringLiteral("距离柔化")); m_soft->setObjectName(QStringLiteral("vertexSoftEnabled")); m_soft->setCheckable(true);
    connect(m_soft, &QAction::triggered, this, [this](bool enabled) { auto settings = m_settings; settings.soft.enabled = enabled; RequestSettings(settings); });
    m_options = m_toolbar->addAction(QStringLiteral("设置…")); m_options->setObjectName(QStringLiteral("vertexOptions"));
    connect(m_options, &QAction::triggered, this, &CVertexSelectionTools::OpenSettings);
    m_undo = m_toolbar->addAction(QStringLiteral("撤回笔触")); m_undo->setObjectName(QStringLiteral("vertexUndoStroke"));
    m_undo->setToolTip(QStringLiteral("撤回笔刷影响范围，不撤销模型形变")); connect(m_undo, &QAction::triggered, this, &CVertexSelectionTools::UndoStrokeRequested);
    m_redo = m_toolbar->addAction(QStringLiteral("重做笔触")); m_redo->setObjectName(QStringLiteral("vertexRedoStroke"));
    connect(m_redo, &QAction::triggered, this, &CVertexSelectionTools::RedoStrokeRequested);
    m_status = new StatusLabel(this); m_status->setObjectName(QStringLiteral("vertexSelectionStatus")); m_status->setTextFormat(Qt::PlainText);
    m_status->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed); layout->addWidget(m_status);
    for (auto* button : m_toolbar->findChildren<QToolButton*>()) button->setFocusPolicy(Qt::StrongFocus);

    m_popup = new QFrame(this, Qt::Popup); m_popup->setObjectName(QStringLiteral("vertexSelectionPopup")); m_popup->setWindowTitle(QStringLiteral("点编辑设置"));
    m_popup->setFrameShape(QFrame::StyledPanel); m_popup->installEventFilter(this);
    auto* popupLayout = new QVBoxLayout(m_popup);
    auto* title = new QLabel(QStringLiteral("影响范围"), m_popup); popupLayout->addWidget(title);
    m_scroll = new QScrollArea(m_popup); m_scroll->setWidgetResizable(true); m_scroll->setFrameShape(QFrame::NoFrame); popupLayout->addWidget(m_scroll, 1);
    auto* content = new QWidget(m_scroll); auto* fields = new QVBoxLayout(content); fields->setContentsMargins(0, 0, 0, 0); m_scroll->setWidget(content);
    const auto form = [](QWidget* widget) {
        auto* result = new QFormLayout(widget); result->setContentsMargins(0, 0, 0, 0);
        result->setRowWrapPolicy(QFormLayout::WrapLongRows); result->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow); return result;
    };
    const auto numeric = [this](QWidget* owner, const QString& name, const QString& label, double maximum) {
        auto* edit = new QLineEdit(owner); edit->setObjectName(name); edit->setAccessibleName(label); edit->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        auto* validator = new QDoubleValidator(0, maximum, 323, edit); validator->setNotation(QDoubleValidator::ScientificNotation); validator->setLocale(edit->locale()); edit->setValidator(validator);
        connect(edit, &QLineEdit::editingFinished, this, &CVertexSelectionTools::SubmitInputs);
        connect(edit, &QLineEdit::textEdited, this, [this] {
            if (m_refreshing) return; Selection::Settings candidate; QString error;
            SetError(ReadInputs(candidate, &error) ? QString() : error);
        });
        return edit;
    };
    m_softFields = new QWidget(content); auto* soft = form(m_softFields);
    m_softEnabled = new QCheckBox(QStringLiteral("启用距离柔化"), m_softFields); m_softEnabled->setObjectName(QStringLiteral("vertexSoftCheck")); soft->addRow(m_softEnabled);
    connect(m_softEnabled, &QCheckBox::clicked, this, [this](bool enabled) { auto settings = m_settings; settings.soft.enabled = enabled; RequestSettings(settings); });
    m_softRadius = numeric(m_softFields, QStringLiteral("vertexSoftRadius"), QStringLiteral("影响半径（逻辑像素）"), 1000000); soft->addRow(QStringLiteral("半径（逻辑 px）"), m_softRadius);
    m_distance = new QComboBox(m_softFields); m_distance->setObjectName(QStringLiteral("vertexDistance"));
    m_distance->addItems({QStringLiteral("沿网格边，不跨岛"), QStringLiteral("同一连通区域，直线距离"), QStringLiteral("全部顶点，允许跨岛")});
    m_distance->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon); m_distance->setMinimumContentsLength(1); soft->addRow(QStringLiteral("范围"), m_distance);
    m_falloff = new QComboBox(m_softFields); m_falloff->setObjectName(QStringLiteral("vertexFalloff")); m_falloff->addItems({QStringLiteral("线性"), QStringLiteral("平滑")}); soft->addRow(QStringLiteral("衰减"), m_falloff);
    for (auto* combo : {m_distance, m_falloff}) connect(combo, &QComboBox::activated, this, &CVertexSelectionTools::SubmitInputs);
    fields->addWidget(m_softFields);
    m_brushFields = new QWidget(content); auto* brush = form(m_brushFields);
    m_brushRadius = numeric(m_brushFields, QStringLiteral("vertexBrushRadius"), QStringLiteral("笔刷半径（逻辑像素）"), 1000000); brush->addRow(QStringLiteral("半径（逻辑 px）"), m_brushRadius);
    m_strength = numeric(m_brushFields, QStringLiteral("vertexBrushStrength"), QStringLiteral("每笔强度百分比"), 100); brush->addRow(QStringLiteral("每笔强度（%）"), m_strength);
    m_hardness = numeric(m_brushFields, QStringLiteral("vertexBrushHardness"), QStringLiteral("笔刷硬度百分比"), 100); brush->addRow(QStringLiteral("硬度（%）"), m_hardness);
    fields->addWidget(m_brushFields);
    m_radiusSlider = new QSlider(Qt::Horizontal, content); m_radiusSlider->setObjectName(QStringLiteral("vertexRadiusSlider")); m_radiusSlider->setAccessibleName(QStringLiteral("调整当前工具半径")); m_radiusSlider->setRange(0, 700); fields->addWidget(m_radiusSlider);
    connect(m_radiusSlider, &QSlider::valueChanged, this, [this](int value) {
        if (m_refreshing) return;
        auto* field = m_settings.tool == Selection::Tool::Brush ? m_brushRadius : m_softRadius;
        field->setText(number(std::pow(10.0, value / 100.0 - 1), field->locale()));
        if (!m_radiusSlider->isSliderDown()) SubmitInputs();
    });
    connect(m_radiusSlider, &QSlider::sliderReleased, this, &CVertexSelectionTools::SubmitInputs);
    m_popupHint = new QLabel(content); m_popupHint->setObjectName(QStringLiteral("vertexSelectionHint")); m_popupHint->setTextFormat(Qt::PlainText); m_popupHint->setWordWrap(true); m_popupHint->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred); fields->addWidget(m_popupHint);
    m_popupError = new QLabel(content); m_popupError->setObjectName(QStringLiteral("vertexSelectionError")); m_popupError->setTextFormat(Qt::PlainText); m_popupError->setWordWrap(true); m_popupError->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred); fields->addWidget(m_popupError);
    fields->addStretch();
    RefreshInputs(); Refresh();
}

bool CVertexSelectionTools::CanEdit() const
{
    return m_context.available && m_context.editing && !m_context.busy && (m_context.type == QLatin1String("artmesh") || m_context.type == QLatin1String("warp"));
}
CVertexSelectionTools::~CVertexSelectionTools() { m_popup->removeEventFilter(this); }
void CVertexSelectionTools::SetContext(const Context& context)
{
    const bool changedTarget = context.target != m_context.target || context.type != m_context.type;
    m_context = context;
    if (changedTarget) { m_error.clear(); m_popup->hide(); }
    Refresh();
}
bool CVertexSelectionTools::SetSettings(const Selection::Settings& settings, QString* error)
{
    if (!Selection::ValidateSettings(settings, error)) return false;
    if (settings == m_settings) { Refresh(); return true; }
    m_settings = settings; m_error.clear(); RefreshInputs(); Refresh(); return true;
}
void CVertexSelectionTools::SetError(const QString& error) { m_error = error; Refresh(); }
void CVertexSelectionTools::RefreshInputs()
{
    m_refreshing = true;
    m_softEnabled->setChecked(m_settings.soft.enabled);
    m_softRadius->setText(number(m_settings.soft.radius, m_softRadius->locale()));
    m_brushRadius->setText(number(m_settings.brush.radius, m_brushRadius->locale()));
    m_strength->setText(number(m_settings.brush.strength * 100, m_strength->locale()));
    m_hardness->setText(number(m_settings.brush.hardness * 100, m_hardness->locale()));
    m_distance->setCurrentIndex(int(m_settings.soft.distance)); m_falloff->setCurrentIndex(int(m_settings.soft.falloff));
    const double radius = m_settings.tool == Selection::Tool::Brush ? m_settings.brush.radius : m_settings.soft.radius;
    m_radiusSlider->setValue(radius <= 0 ? 0 : int(std::clamp((std::log10(radius) + 1) * 100, 0.0, 700.0)));
    m_refreshing = false;
}
void CVertexSelectionTools::Refresh()
{
    const bool supported = m_context.type == QLatin1String("artmesh") || m_context.type == QLatin1String("warp");
    const bool editable = CanEdit(); const bool brush = m_settings.tool == Selection::Tool::Brush;
    const bool painted = m_context.source == Selection::Source::Painted;
    { const QSignalBlocker blocker(m_edit); m_edit->setChecked(m_context.editing); }
    m_edit->setEnabled(supported && m_context.available && !m_context.busy);
    m_tools->setEnabled(editable);
    for (auto* action : m_tools->actions()) { const QSignalBlocker block(action); action->setChecked(action->data().toInt() == int(m_settings.tool)); }
    for (auto* action : m_combinations->actions()) {
        const QSignalBlocker block(action); action->setChecked(action->data().toInt() == int(m_settings.combine));
        action->setText(combineText(Selection::Combine(action->data().toInt()), brush));
    }
    m_combineButton->defaultAction()->setText(combineText(m_settings.combine, brush)); m_combineButton->defaultAction()->setEnabled(editable);
    { const QSignalBlocker block(m_soft); m_soft->setChecked(!painted && m_settings.soft.enabled); }
    m_soft->setEnabled(editable && !painted && !brush);
    { const QSignalBlocker block(m_softEnabled); m_softEnabled->setChecked(!painted && m_settings.soft.enabled); }
    m_soft->setToolTip(painted ? QStringLiteral("当前使用笔刷权重；重新选点后可使用距离柔化") : QStringLiteral("从硬选点按距离渐弱，只影响拖动，不扩大删除或连边范围"));
    m_undo->setEnabled(editable && m_context.canUndoStroke); m_redo->setEnabled(editable && m_context.canRedoStroke);
    m_options->setEnabled(supported && m_context.available);
    m_softFields->setVisible(!brush); m_brushFields->setVisible(brush);
    m_softFields->setEnabled(editable && !painted); m_brushFields->setEnabled(editable); m_radiusSlider->setEnabled(editable && (brush || !painted));
    const QString source = painted ? QStringLiteral("笔刷权重") : m_context.source == Selection::Source::Distance ? QStringLiteral("距离柔化") : QStringLiteral("仅选点");
    const int hard = std::max(0, m_context.hardCount), affected = std::max(0, m_context.affectedCount);
    QString state = QStringLiteral("%1 · 硬选 %2 · %3 %4").arg(source).arg(hard)
        .arg(painted ? QStringLiteral("影响") : QStringLiteral("另影响")).arg(painted ? affected : std::max(0, affected - hard));
    if (m_context.busy) state += QStringLiteral(" · 正在操作，Esc 取消");
    else if (!m_context.influenceReady && m_context.editing) state += QStringLiteral(" · 影响预览尚未就绪");
    state += QStringLiteral(" · ") + m_context.target;
    if (!supported) state = !m_context.reason.isEmpty() ? m_context.reason : m_context.type == QLatin1String("rotation") ? QStringLiteral("旋转变形器使用中心与角度手柄，不使用点选择") : QStringLiteral("选择一个 ArtMesh 或 Warp 以编辑点");
    else if (!m_context.available || !m_context.editing) state = (m_context.reason.isEmpty() ? QStringLiteral("点击“编辑点”开始") : m_context.reason) + QStringLiteral(" · ") + m_context.target;
    if (!m_error.isEmpty()) state = m_error + QStringLiteral(" · ") + state;
    m_status->setText(state); m_status->setToolTip(escaped(state)); m_status->setAccessibleDescription(state);
    m_popupHint->setText(brush ? QStringLiteral("每笔从落笔前的权重增加或擦除；同一笔反复经过不叠加，抬笔后再画才叠加。权重是临时编辑状态，涂抹不改变模型；切回选择/移动可拖动影响点。") :
        painted ? QStringLiteral("当前是笔刷权重。切换到选择/移动可拖动；接受新的点选、框选或套索会清除笔刷权重，再按选点计算距离影响。") :
        QStringLiteral("半径按画布显示的逻辑像素计算，缩放视图会改变覆盖范围。只有硬选点参与删除和连边；其余权重只影响拖动。"));
    m_popupError->setText(m_error); m_popupError->setVisible(!m_error.isEmpty());
}
void CVertexSelectionTools::RequestSettings(const Selection::Settings& candidate)
{
    if (m_refreshing || !CanEdit()) { Refresh(); return; }
    QString error; if (!Selection::ValidateSettings(candidate, &error)) { SetError(error); return; }
    const QPointer<CVertexSelectionTools> alive(this); emit SettingsRequested(candidate);
    if (alive) Refresh();
}
bool CVertexSelectionTools::ReadInputs(Selection::Settings& result, QString* error) const
{
    result = m_settings;
    const auto read = [error](const QLineEdit* field, double& value) {
        bool valid = false; value = field->locale().toDouble(field->text().trimmed(), &valid);
        if (!valid || !std::isfinite(value)) { if (error) *error = QStringLiteral("请完成有效的数字输入"); return false; } return true;
    };
    if (m_settings.tool == Selection::Tool::Brush) {
        double strength = 0, hardness = 0;
        if (!read(m_brushRadius, result.brush.radius) || !read(m_strength, strength) || !read(m_hardness, hardness)) return false;
        if (strength != m_settings.brush.strength * 100) result.brush.strength = strength / 100;
        if (hardness != m_settings.brush.hardness * 100) result.brush.hardness = hardness / 100;
    } else {
        if (!read(m_softRadius, result.soft.radius)) return false;
        result.soft.enabled = m_softEnabled->isChecked(); result.soft.distance = Selection::Distance(m_distance->currentIndex()); result.soft.falloff = Selection::Falloff(m_falloff->currentIndex());
    }
    return Selection::ValidateSettings(result, error);
}
void CVertexSelectionTools::SubmitInputs()
{
    if (m_refreshing || !CanEdit() || (m_context.source == Selection::Source::Painted && m_settings.tool != Selection::Tool::Brush)) return;
    Selection::Settings candidate; QString error;
    if (!ReadInputs(candidate, &error)) { SetError(error); return; }
    RequestSettings(candidate);
}
void CVertexSelectionTools::OpenSettings()
{
    if (m_popup->isVisible()) { m_popup->hide(); return; }
    RefreshInputs(); Refresh(); PositionPopup(); m_popup->show(); m_popup->raise();
}
void CVertexSelectionTools::PositionPopup()
{
    auto* anchor = m_toolbar->widgetForAction(m_options); if (!anchor) anchor = m_toolbar;
    const QRect available = screen()->availableGeometry();
    const int margin = style()->pixelMetric(QStyle::PM_LayoutLeftMargin, nullptr, this);
    const int width = std::min(std::max(fontMetrics().horizontalAdvance(QStringLiteral("同一连通区域，直线距离")) + margin * 4, fontMetrics().averageCharWidth() * 42), available.width());
    const int height = std::min(fontMetrics().height() * 24, available.height());
    m_popup->resize(width, height);
    QPoint origin = anchor->mapToGlobal(QPoint(0, anchor->height()));
    origin.setX(std::clamp(origin.x(), available.left(), std::max(available.left(), available.right() - m_popup->width() + 1)));
    if (origin.y() + m_popup->height() > available.bottom() + 1) origin.setY(anchor->mapToGlobal(QPoint()).y() - m_popup->height());
    origin.setY(std::clamp(origin.y(), available.top(), std::max(available.top(), available.bottom() - m_popup->height() + 1)));
    m_popup->move(origin);
}
bool CVertexSelectionTools::eventFilter(QObject* object, QEvent* event)
{
    if (object == m_popup && event->type() == QEvent::Hide) { m_error.clear(); RefreshInputs(); Refresh(); }
    return QWidget::eventFilter(object, event);
}
void CVertexSelectionTools::changeEvent(QEvent* event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange || event->type() == QEvent::PaletteChange) {
        if (m_status) m_status->update();
        if (m_popup && m_popup->isVisible()) PositionPopup();
    }
}
