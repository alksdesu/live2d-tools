#include "cmeshtoolbar.h"
#include <QActionGroup>
#include <QLabel>
#include <QSignalBlocker>
#include <QToolButton>

namespace {
class ContextLabel : public QLabel
{
public:
    using QLabel::QLabel;
    QSize sizeHint() const override { return {0, QLabel::sizeHint().height()}; }
    QSize minimumSizeHint() const override { return {0, QLabel::minimumSizeHint().height()}; }
};
}

CMeshToolBar::CMeshToolBar(QWidget* parent) : QToolBar(QStringLiteral("网格工具"), parent)
{
    setObjectName(QStringLiteral("meshTools"));
    setMovable(false);
    setFloatable(false);
    setToolButtonStyle(Qt::ToolButtonTextOnly);
    m_autoGenerate = addAction(QStringLiteral("自动网格…"));
    m_autoGenerate->setObjectName(QStringLiteral("meshAutoGenerate"));
    m_autoGenerate->setToolTip(QStringLiteral("按素材透明轮廓预览并生成网格，同步保留全部关键形和 UV"));
    connect(m_autoGenerate, &QAction::triggered, this, &CMeshToolBar::AutoGenerateRequested);
    m_clone = addAction(QStringLiteral("克隆 ArtMesh…"));
    m_clone->setObjectName(QStringLiteral("meshClone"));
    m_clone->setToolTip(QStringLiteral("复制当前 ArtMesh 的几何、关键形、蒙版和绘制组，生成独立可编辑对象"));
    m_clone->setStatusTip(m_clone->toolTip());
    connect(m_clone, &QAction::triggered, this, &CMeshToolBar::CloneRequested);
    addSeparator();
    m_edit = addAction(QStringLiteral("网格工具"));
    m_edit->setObjectName(QStringLiteral("meshToolsEnabled"));
    m_edit->setCheckable(true);
    connect(m_edit, &QAction::toggled, this, &CMeshToolBar::EditingRequested);
    addSeparator();
    m_tools = new QActionGroup(this);
    m_tools->setExclusive(true);
    const struct { Tool tool; const char* name; const char* title; const char* help; } tools[] = {
        {Tool::Select, "meshSelect", "选择", "单击选择顶点，Shift 多选，拖动空白处框选"},
        {Tool::Insert, "meshInsert", "插点", "点击三角形内部或边上插点，同步更新全部关键形与 UV"},
        {Tool::Connect, "meshConnect", "连边", "依次点击两个顶点连接，Esc 取消待连接的起点"},
        {Tool::Delete, "meshDelete", "删点", "点击顶点删除，或按 Delete 删除选区，并重建周围三角形"}
    };
    for (const auto& item : tools) {
        auto* action = addAction(QString::fromUtf8(item.title));
        action->setObjectName(QString::fromLatin1(item.name));
        action->setData(static_cast<int>(item.tool));
        action->setToolTip(QString::fromUtf8(item.help));
        action->setStatusTip(action->toolTip());
        action->setCheckable(true);
        m_tools->addAction(action);
        if (item.tool == Tool::Select) action->setChecked(true);
    }
    connect(m_tools, &QActionGroup::triggered, this, [this](QAction* action) {
        emit ToolRequested(static_cast<Tool>(action->data().toInt()));
    });
    addSeparator();
    m_preserveBoundary = addAction(QStringLiteral("锁定轮廓"));
    m_preserveBoundary->setObjectName(QStringLiteral("meshPreserveBoundary"));
    m_preserveBoundary->setCheckable(true);
    m_preserveBoundary->setChecked(true);
    m_preserveBoundary->setToolTip(QStringLiteral("删点时保留当前姿态的外轮廓与孔洞；仍可删除内部点或共线边界点。取消后允许用直线连接相邻边界点"));
    m_preserveBoundary->setStatusTip(m_preserveBoundary->toolTip());
    m_optimize = addAction(QStringLiteral("优化网格"));
    m_optimize->setObjectName(QStringLiteral("meshRetriangulate"));
    m_optimize->setToolTip(QStringLiteral("优化选区周围的三角连接；没有选区时处理整个网格，保留轮廓与孔洞"));
    m_optimize->setStatusTip(m_optimize->toolTip());
    connect(m_optimize, &QAction::triggered, this, &CMeshToolBar::RetriangulateRequested);
    m_context = new ContextLabel(this);
    m_context->setObjectName(QStringLiteral("meshToolContext"));
    m_context->setTextFormat(Qt::PlainText);
    m_context->setMinimumWidth(0);
    m_context->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    addWidget(m_context);
    for (auto* button : findChildren<QToolButton*>()) button->setFocusPolicy(Qt::StrongFocus);
    SetContext({}, 0, 0, false, QStringLiteral("选择一个网格以开始编辑"));
}

void CMeshToolBar::SetContext(const QString& name, int vertices, int selected, bool available, const QString& reason)
{
    m_available = available;
    m_reason = reason;
    const QString text = name.isEmpty() ? QStringLiteral("尚未选择网格")
        : QStringLiteral("%1   %2 个顶点   已选 %3").arg(name).arg(vertices).arg(selected);
    m_context->setText(text);
    m_context->setToolTip(QStringLiteral("<qt>%1</qt>").arg((reason.isEmpty() ? text : reason).toHtmlEscaped()));
    RefreshActions();
}

void CMeshToolBar::SetEditing(bool editing)
{
    const QSignalBlocker blocker(m_edit);
    m_edit->setChecked(editing && m_available);
    RefreshActions();
}

void CMeshToolBar::SetTool(Tool tool)
{
    for (auto* action : m_tools->actions())
        if (action->data().toInt() == static_cast<int>(tool)) {
            action->setChecked(true);
            break;
        }
}

bool CMeshToolBar::IsEditing() const { return m_edit->isChecked(); }

bool CMeshToolBar::PreservesBoundary() const { return m_preserveBoundary->isChecked(); }

CMeshToolBar::Tool CMeshToolBar::CurrentTool() const
{
    const auto* action = m_tools->checkedAction();
    return action ? static_cast<Tool>(action->data().toInt()) : Tool::Select;
}

void CMeshToolBar::RefreshActions()
{
    m_autoGenerate->setEnabled(m_available);
    m_clone->setEnabled(m_available);
    const QString automaticHelp = m_available ? QStringLiteral("按素材透明轮廓预览并生成网格，同步保留全部关键形和 UV") : m_reason;
    m_autoGenerate->setToolTip(QStringLiteral("<qt>%1</qt>").arg(automaticHelp.toHtmlEscaped()));
    m_edit->setEnabled(m_available);
    const QString help = m_available ? QStringLiteral("在当前关键形姿态下编辑网格，修改可撤销") : m_reason;
    m_edit->setToolTip(QStringLiteral("<qt>%1</qt>").arg(help.toHtmlEscaped()));
    m_tools->setEnabled(m_available && IsEditing());
    m_preserveBoundary->setEnabled(m_available && IsEditing());
    m_optimize->setEnabled(m_available && IsEditing());
}
