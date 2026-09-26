#include "ckeyformmatrix.h"
#include <QAbstractTableModel>
#include <QComboBox>
#include <QFormLayout>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStyledItemDelegate>
#include <QTableView>
#include <QVBoxLayout>
#include <array>
#include <limits>
#include <utility>

using live2dstruct::CKeyformGridSnapshot;

namespace {
constexpr int ActiveRole = Qt::UserRole + 1;

QString axisName(const CKeyformGridSnapshot::Axis& axis)
{
    return axis.displayName.isEmpty() || axis.displayName == axis.parameterId ? axis.parameterId
        : QStringLiteral("%1（%2）").arg(axis.displayName, axis.parameterId);
}

QString keyText(float value)
{
    for (int precision = 6; precision < std::numeric_limits<float>::max_digits10; ++precision) {
        const auto text = QString::number(value, 'g', precision);
        bool valid = false;
        if (text.toFloat(&valid) == value && valid) return text;
    }
    return QString::number(value, 'g', std::numeric_limits<float>::max_digits10);
}

QString tooltip(const QString& text)
{
    return QStringLiteral("<qt>%1</qt>").arg(text.toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>")));
}

bool sameLayout(const CKeyformGridSnapshot& a, const CKeyformGridSnapshot& b)
{
    if (a.objectId != b.objectId || a.objectType != b.objectType || a.sourceDigest != b.sourceDigest
        || a.axesValid != b.axesValid || a.keyformCount != b.keyformCount
        || a.expectedKeyformCount != b.expectedKeyformCount || a.axes.size() != b.axes.size()) return false;
    for (int index = 0; index < a.axes.size(); ++index) {
        const auto& x = a.axes[index]; const auto& y = b.axes[index];
        if (x.parameterId != y.parameterId || x.parameterType != y.parameterType || x.keys != y.keys) return false;
    }
    return true;
}

class MatrixModel : public QAbstractTableModel
{
public:
    explicit MatrixModel(QObject* parent) : QAbstractTableModel(parent) {}
    CKeyformGridSnapshot grid;
    QVector<int> fixed;
    int x = -1, y = -1, active = -1;
    bool locked = false;

    int rowCount(const QModelIndex& parent = {}) const override
    {
        if (parent.isValid() || !grid.axesValid) return 0;
        return y < 0 ? 1 : int(grid.axes[y].keys.size());
    }
    int columnCount(const QModelIndex& parent = {}) const override
    {
        if (parent.isValid() || !grid.axesValid) return 0;
        return x < 0 ? 1 : int(grid.axes[x].keys.size());
    }
    qint64 localKeyform(const QModelIndex& index) const
    {
        if (!index.isValid() || index.row() >= rowCount() || index.column() >= columnCount()) return -1;
        qint64 local = 0, stride = 1;
        for (int axis = 0; axis < grid.axes.size(); ++axis) {
            const qint64 count = grid.axes[axis].keys.size();
            const int key = axis == x ? index.column() : axis == y ? index.row() : fixed.value(axis);
            if (count <= 0 || key < 0 || key >= count || stride > std::numeric_limits<qint64>::max() / count) return -1;
            local += stride * key;
            stride *= count;
        }
        return local;
    }
    QString rejection(qint64 local) const
    {
        if (grid.expectedKeyformCount < 0) return grid.diagnostic.isEmpty() ? QStringLiteral("关键形组合数量超出范围") : grid.diagnostic;
        if (local < 0 || local > std::numeric_limits<int>::max()) return QStringLiteral("关键形索引超出范围");
        if (local >= grid.keyformCount) return QStringLiteral("缺失角点：没有对应的关键形");
        if (locked) return QStringLiteral("对象已锁定，解锁后可切换关键形");
        QMap<QString, float> pose; QString error;
        if (!grid.Pose(int(local), pose, &error)) return error;
        return {};
    }
    QVariant data(const QModelIndex& index, int role) const override
    {
        if (!index.isValid()) return {};
        const qint64 local = localKeyform(index);
        if (role == Qt::UserRole) return local;
        if (role == ActiveRole) return local >= 0 && local == active;
        if (role == Qt::TextAlignmentRole) return int(Qt::AlignCenter);
        if (role == Qt::DisplayRole) {
            if (local < 0 || local >= grid.keyformCount) return QStringLiteral("缺失");
            return QStringLiteral("#%1").arg(local);
        }
        if (role == Qt::ToolTipRole || role == Qt::AccessibleDescriptionRole) {
            QStringList lines;
            for (int axis = 0; axis < grid.axes.size(); ++axis) {
                const int key = axis == x ? index.column() : axis == y ? index.row() : fixed.value(axis);
                if (key >= 0 && key < grid.axes[axis].keys.size())
                    lines.append(QStringLiteral("%1 = %2").arg(axisName(grid.axes[axis]), keyText(grid.axes[axis].keys[key])));
            }
            const QString error = rejection(local);
            lines.prepend(local < 0 ? QStringLiteral("无效角点") : QStringLiteral("关键形 #%1").arg(local));
            if (!error.isEmpty()) lines.append(error);
            const auto text = lines.join(QLatin1Char('\n'));
            return role == Qt::ToolTipRole ? tooltip(text) : text;
        }
        return {};
    }
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override
    {
        const int axis = orientation == Qt::Horizontal ? x : y;
        if (role == Qt::ToolTipRole) return tooltip(axis < 0 ? QStringLiteral("未绑定参数")
            : QStringLiteral("%1 = %2").arg(axisName(grid.axes[axis]), keyText(grid.axes[axis].keys.value(section))));
        if (role != Qt::DisplayRole) return {};
        if (axis < 0) return QStringLiteral("—");
        return section >= 0 && section < grid.axes[axis].keys.size() ? keyText(grid.axes[axis].keys[section]) : QString();
    }
    Qt::ItemFlags flags(const QModelIndex& index) const override
    {
        return index.isValid() && rejection(localKeyform(index)).isEmpty()
            ? Qt::ItemIsEnabled | Qt::ItemIsSelectable : Qt::NoItemFlags;
    }
    void updateGrid(const CKeyformGridSnapshot& value, bool reset)
    {
        if (reset) beginResetModel();
        grid = value;
        if (reset) {
            x = grid.axes.isEmpty() ? -1 : 0;
            y = grid.axes.size() < 2 ? -1 : 1;
            fixed.fill(0, grid.axes.size());
            endResetModel();
        } else refresh();
    }
    void project(int horizontal, int vertical, const QVector<int>& keys)
    {
        beginResetModel(); x = horizontal; y = vertical; fixed = keys; endResetModel();
    }
    void refresh()
    {
        if (rowCount() && columnCount()) emit dataChanged(index(0, 0), index(rowCount() - 1, columnCount() - 1));
    }
    QModelIndex activeIndex() const
    {
        if (active < 0) return {};
        int remaining = active, row = 0, column = 0;
        for (int axis = 0; axis < grid.axes.size(); ++axis) {
            const int count = int(grid.axes[axis].keys.size());
            if (!count) return {};
            const int key = remaining % count; remaining /= count;
            if (axis == x) column = key;
            else if (axis == y) row = key;
            else if (fixed.value(axis) != key) return {};
        }
        return remaining ? QModelIndex() : index(row, column);
    }
};

class MatrixDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override
    {
        QStyleOptionViewItem view(option);
        initStyleOption(&view, index);
        const bool active = index.data(ActiveRole).toBool();
        const auto group = view.state & QStyle::State_Enabled ? QPalette::Active : QPalette::Disabled;
        painter->save();
        painter->fillRect(view.rect, view.palette.brush(group, active ? QPalette::Highlight : QPalette::Base));
        view.font.setBold(active);
        painter->setFont(view.font);
        painter->setPen(view.palette.color(group, active ? QPalette::HighlightedText : QPalette::Text));
        painter->drawText(view.rect, Qt::AlignCenter, view.text);
        if (view.state & QStyle::State_HasFocus) {
            QPen pen(view.palette.color(group, active ? QPalette::HighlightedText : QPalette::Highlight));
            pen.setStyle(Qt::DotLine);
            painter->setPen(pen);
            painter->setBrush(Qt::NoBrush);
            painter->drawRect(view.rect.adjusted(1, 1, -2, -2));
        }
        painter->restore();
    }
};
}

class CKeyformMatrix::State
{
public:
    MatrixModel* model = nullptr;
    QTableView* table = nullptr;
    QComboBox *horizontal = nullptr, *vertical = nullptr, *sliceAxis = nullptr, *sliceKey = nullptr;
    QLabel *horizontalLabel = nullptr, *verticalLabel = nullptr, *sliceAxisLabel = nullptr, *sliceKeyLabel = nullptr;
    QLabel* status = nullptr;
    QPushButton* locate = nullptr;
    bool requesting = false;
    bool initialized = false;
};

CKeyformMatrix::CKeyformMatrix(QWidget* parent) : QWidget(parent), m_state(std::make_unique<State>())
{
    setObjectName(QStringLiteral("keyformMatrix"));
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* axes = new QFormLayout;
    axes->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    axes->setRowWrapPolicy(QFormLayout::WrapLongRows);
    const auto addAxis = [&](QComboBox*& combo, QLabel*& label, const char* name, const QString& title) {
        combo = new QComboBox(this);
        combo->setObjectName(QString::fromLatin1(name));
        combo->setMinimumContentsLength(1);
        combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        combo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        combo->setMinimumWidth(0);
        combo->setAccessibleName(title);
        label = new QLabel(title, this); label->setBuddy(combo);
        axes->addRow(label, combo);
    };
    addAxis(m_state->horizontal, m_state->horizontalLabel, "keyformMatrixXAxis", QStringLiteral("横轴"));
    addAxis(m_state->vertical, m_state->verticalLabel, "keyformMatrixYAxis", QStringLiteral("纵轴 ↓"));
    addAxis(m_state->sliceAxis, m_state->sliceAxisLabel, "keyformMatrixSliceAxis", QStringLiteral("固定轴"));
    addAxis(m_state->sliceKey, m_state->sliceKeyLabel, "keyformMatrixSliceKey", QStringLiteral("固定值"));
    layout->addLayout(axes);
    m_state->table = new QTableView(this);
    m_state->table->setObjectName(QStringLiteral("keyformMatrixTable"));
    m_state->table->setAccessibleName(QStringLiteral("关键形角点矩阵"));
    m_state->table->setToolTip(QStringLiteral("单击或按 Enter 请求预览；方向键只移动光标。纵轴关键值从上到下递增。"));
    m_state->table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_state->table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_state->table->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_state->table->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_state->table->horizontalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    m_state->table->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    m_state->table->setItemDelegate(new MatrixDelegate(m_state->table));
    m_state->model = new MatrixModel(m_state->table);
    m_state->table->setModel(m_state->model);
    m_state->table->installEventFilter(this);
    layout->addWidget(m_state->table);
    m_state->status = new QLabel(this);
    m_state->status->setObjectName(QStringLiteral("keyformMatrixStatus"));
    m_state->status->setTextFormat(Qt::PlainText);
    m_state->status->setWordWrap(true);
    m_state->status->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_state->locate = new QPushButton(QStringLiteral("定位当前形"), this);
    m_state->locate->setObjectName(QStringLiteral("keyformMatrixLocate"));
    m_state->locate->setToolTip(QStringLiteral("显示已选择的关键形所在切片，不改变预览姿态"));
    auto* footer = new QHBoxLayout;
    footer->addWidget(m_state->status, 1);
    footer->addWidget(m_state->locate);
    layout->addLayout(footer);
    connect(m_state->locate, &QPushButton::clicked, this, [this] {
        auto* model = m_state->model;
        if (model->active < 0 || !model->grid.axesValid || model->grid.expectedKeyformCount < 0) return;
        int remaining = model->active;
        auto fixed = model->fixed;
        for (int axis = 0; axis < model->grid.axes.size(); ++axis) {
            const int count = int(model->grid.axes[axis].keys.size());
            if (!count) return;
            fixed[axis] = remaining % count; remaining /= count;
        }
        if (remaining) return;
        model->fixed = std::move(fixed);
        RefreshProjection();
        const auto index = model->activeIndex();
        m_state->table->setCurrentIndex(index);
        m_state->table->scrollTo(index);
        m_state->table->setFocus();
    });
    connect(m_state->table, &QTableView::clicked, this, [this](const QModelIndex& index) {
        if (index == m_state->table->currentIndex()) RequestCurrent();
    });
    const auto selectAxis = [this](bool horizontal, int index) {
        if (index < 0) return;
        auto* model = m_state->model;
        const int axis = (horizontal ? m_state->horizontal : m_state->vertical)->itemData(index).toInt();
        const int old = horizontal ? model->x : model->y;
        if (horizontal) { if (axis == model->y) model->y = old; model->x = axis; }
        else { if (axis == model->x) model->x = old; model->y = axis; }
        const QSignalBlocker a(m_state->horizontal), b(m_state->vertical);
        m_state->horizontal->setCurrentIndex(m_state->horizontal->findData(model->x));
        m_state->vertical->setCurrentIndex(m_state->vertical->findData(model->y));
        RefreshProjection();
    };
    connect(m_state->horizontal, &QComboBox::currentIndexChanged, this, [selectAxis](int index) { selectAxis(true, index); });
    connect(m_state->vertical, &QComboBox::currentIndexChanged, this, [selectAxis](int index) { selectAxis(false, index); });
    connect(m_state->sliceAxis, &QComboBox::currentIndexChanged, this, [this] { RefreshSliceKeys(); });
    connect(m_state->sliceKey, &QComboBox::currentIndexChanged, this, [this](int key) {
        const int axis = m_state->sliceAxis->currentData().toInt();
        if (key < 0 || axis < 0 || axis >= m_state->model->fixed.size()) return;
        m_state->model->fixed[axis] = key;
        RefreshProjection();
    });
    Clear();
    UpdateMetrics();
}

CKeyformMatrix::~CKeyformMatrix() = default;

void CKeyformMatrix::SetGrid(const CKeyformGridSnapshot& grid, int activeKeyform, bool locked)
{
    auto* model = m_state->model;
    const bool rebuild = !m_state->initialized || !sameLayout(model->grid, grid);
    m_state->initialized = true;
    model->locked = locked;
    model->updateGrid(grid, rebuild);
    model->active = activeKeyform >= 0 && activeKeyform < grid.keyformCount ? activeKeyform : -1;
    if (rebuild) {
        int remaining = model->active;
        if (remaining >= 0) for (int axis = 0; axis < grid.axes.size(); ++axis) {
            const int count = int(grid.axes[axis].keys.size());
            if (!count) break;
            model->fixed[axis] = remaining % count; remaining /= count;
        }
        RebuildAxes();
    } else {
        for (auto* combo : {m_state->horizontal, m_state->vertical, m_state->sliceAxis})
            for (int index = 0; index < combo->count(); ++index) {
                const int axis = combo->itemData(index).toInt();
                combo->setItemText(index, axisName(grid.axes[axis]));
                combo->setItemData(index, tooltip(axisName(grid.axes[axis])), Qt::ToolTipRole);
            }
        model->refresh();
        RefreshStatus();
    }
}

void CKeyformMatrix::SetActiveKeyform(int keyform)
{
    auto* model = m_state->model;
    model->active = keyform >= 0 && keyform < model->grid.keyformCount ? keyform : -1;
    model->refresh();
    RefreshStatus();
}

void CKeyformMatrix::Clear() { SetGrid({}, -1, false); }
int CKeyformMatrix::ActiveKeyform() const { return m_state->model->active; }

void CKeyformMatrix::RebuildAxes()
{
    auto* model = m_state->model;
    const QSignalBlocker a(m_state->horizontal), b(m_state->vertical);
    for (auto* combo : {m_state->horizontal, m_state->vertical}) {
        combo->clear();
        for (int axis = 0; axis < model->grid.axes.size(); ++axis) {
            combo->addItem(axisName(model->grid.axes[axis]), axis);
            combo->setItemData(axis, tooltip(axisName(model->grid.axes[axis])), Qt::ToolTipRole);
        }
    }
    m_state->horizontal->setCurrentIndex(model->x);
    m_state->vertical->setCurrentIndex(model->y);
    m_state->horizontal->setVisible(!model->grid.axes.isEmpty());
    m_state->horizontalLabel->setVisible(!model->grid.axes.isEmpty());
    m_state->vertical->setVisible(model->grid.axes.size() > 1);
    m_state->verticalLabel->setVisible(model->grid.axes.size() > 1);
    RefreshProjection();
}

void CKeyformMatrix::RefreshProjection()
{
    auto* model = m_state->model;
    model->project(model->x, model->y, model->fixed);
    const int previous = m_state->sliceAxis->currentData().isValid() ? m_state->sliceAxis->currentData().toInt() : -1;
    const QSignalBlocker blocker(m_state->sliceAxis);
    m_state->sliceAxis->clear();
    for (int axis = 0; axis < model->grid.axes.size(); ++axis)
        if (axis != model->x && axis != model->y) {
            m_state->sliceAxis->addItem(axisName(model->grid.axes[axis]), axis);
            m_state->sliceAxis->setItemData(m_state->sliceAxis->count() - 1, tooltip(axisName(model->grid.axes[axis])), Qt::ToolTipRole);
        }
    const int found = m_state->sliceAxis->findData(previous);
    if (found >= 0) m_state->sliceAxis->setCurrentIndex(found);
    const bool slices = m_state->sliceAxis->count() > 0;
    for (auto* widget : std::array<QWidget*, 4>{m_state->sliceAxis, m_state->sliceKey,
                                              m_state->sliceAxisLabel, m_state->sliceKeyLabel}) widget->setVisible(slices);
    RefreshSliceKeys();
    UpdateMetrics();
    RefreshStatus();
}

void CKeyformMatrix::RefreshSliceKeys()
{
    const QSignalBlocker blocker(m_state->sliceKey);
    m_state->sliceKey->clear();
    m_state->sliceAxis->setToolTip(tooltip(m_state->sliceAxis->currentText()));
    m_state->sliceKey->setToolTip({});
    if (!m_state->sliceAxis->currentData().isValid()) return;
    const int axis = m_state->sliceAxis->currentData().toInt();
    const auto& keys = m_state->model->grid.axes[axis].keys;
    for (float key : keys) m_state->sliceKey->addItem(keyText(key));
    m_state->sliceKey->setCurrentIndex(m_state->model->fixed.value(axis));
    m_state->sliceKey->setToolTip(tooltip(m_state->sliceKey->currentText()));
}

void CKeyformMatrix::RefreshStatus()
{
    const auto* model = m_state->model;
    QStringList text;
    if (model->grid.objectId.isEmpty()) text.append(QStringLiteral("选择对象以浏览关键形"));
    else {
        if (!model->grid.diagnostic.isEmpty()) text.append(model->grid.diagnostic);
        if (model->locked) text.append(QStringLiteral("对象已锁定"));
        if (model->active >= 0) text.append(model->activeIndex().isValid()
            ? QStringLiteral("当前形 #%1；方向键只移动光标").arg(model->active)
            : QStringLiteral("当前形 #%1 位于其它切片").arg(model->active));
        else if (model->grid.axesValid) text.append(QStringLiteral("单击或按 Enter 预览角点"));
    }
    m_state->status->setText(text.join(QLatin1Char('\n')));
    m_state->locate->setEnabled(model->active >= 0 && model->grid.axesValid && model->grid.expectedKeyformCount >= 0);
    for (auto* combo : {m_state->horizontal, m_state->vertical, m_state->sliceAxis, m_state->sliceKey})
        combo->setToolTip(tooltip(combo->currentText()));
}

void CKeyformMatrix::UpdateMetrics()
{
    if (!m_state || !m_state->table) return;
    const QFontMetrics metrics(m_state->table->font());
    const int margin = qMax(1, style()->pixelMetric(QStyle::PM_FocusFrameHMargin, nullptr, this));
    const int height = metrics.height() + margin * 4;
    int width = metrics.horizontalAdvance(QStringLiteral("#%1").arg(qMax(0, m_state->model->grid.keyformCount - 1)));
    if (m_state->model->x >= 0)
        for (float value : m_state->model->grid.axes[m_state->model->x].keys)
            width = qMax(width, metrics.horizontalAdvance(keyText(value)));
    width = qMax(height, width + margin * 4);
    m_state->table->horizontalHeader()->setMinimumSectionSize(width);
    m_state->table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    const int rowHeight = qMax(height, m_state->table->verticalHeader()->minimumSectionSize());
    m_state->table->verticalHeader()->setDefaultSectionSize(rowHeight);
    const int rows = qBound(1, m_state->model->rowCount(), 6);
    const int scrollbar = qMax(0, style()->pixelMetric(QStyle::PM_ScrollBarExtent, nullptr, this));
    m_state->table->setMinimumWidth(0);
    m_state->table->setFixedHeight(rowHeight * rows + m_state->table->horizontalHeader()->sizeHint().height() +
                                  scrollbar + m_state->table->frameWidth() * 2);
}

void CKeyformMatrix::RequestCurrent()
{
    auto* model = m_state->model;
    const QModelIndex index = m_state->table->currentIndex();
    const qint64 local = model->localKeyform(index);
    if (m_state->requesting || !index.isValid() || !model->rejection(local).isEmpty()) return;
    m_state->requesting = true;
    const QPointer<CKeyformMatrix> guard(this);
    emit KeyformRequested(int(local));
    if (guard) m_state->requesting = false;
}

bool CKeyformMatrix::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_state->table && event->type() == QEvent::KeyPress) {
        const auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) {
            if (!key->isAutoRepeat()) RequestCurrent();
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void CKeyformMatrix::changeEvent(QEvent* event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange) UpdateMetrics();
}
