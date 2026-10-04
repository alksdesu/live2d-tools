#include "cbindingkeysdialog.h"
#include <QAbstractTableModel>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSet>
#include <QSignalBlocker>
#include <QStyledItemDelegate>
#include <QTableView>
#include <QVBoxLayout>
#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <limits>
#include <locale>
#include <sstream>

using live2dstruct::CKeyformGridSnapshot;
using Builder = live2dstruct::CMocParameterStructureBuilder;

namespace {
QString text(float value)
{
    if (value == 0 && std::signbit(value)) return QStringLiteral("-0");
    for (int precision = 6; precision < std::numeric_limits<float>::max_digits10; ++precision) {
        const auto result = QString::number(value, 'g', precision);
        bool valid = false;
        if (result.toFloat(&valid) == value && valid) return result;
    }
    return QString::number(value, 'g', std::numeric_limits<float>::max_digits10);
}
bool parse(const QString& text, float& result)
{
    static const QRegularExpression number(QStringLiteral("^[+-]?(?:[0-9]+(?:\\.[0-9]*)?|\\.[0-9]+)(?:[eE][+-]?[0-9]+)?$"));
    const auto input = text.trimmed();
    if (!number.match(input).hasMatch()) return false;
    std::istringstream stream(input.toStdString()); stream.imbue(std::locale::classic());
    float value = 0; stream >> std::noskipws >> value;
    if (!stream.eof() || stream.bad() || (stream.fail() && std::fpclassify(value) != FP_SUBNORMAL) || !std::isfinite(value)) return false;
    if (value == 0) {
        const auto mantissa = input.section(QRegularExpression(QStringLiteral("[eE]")), 0, 0);
        if (std::any_of(mantissa.cbegin(), mantissa.cend(), [](QChar digit) { return digit >= QLatin1Char('1') && digit <= QLatin1Char('9'); })) return false;
    }
    result = value;
    return true;
}
QString tooltip(const QString& value)
{
    return QStringLiteral("<qt>%1</qt>").arg(value.toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>")));
}
QString name(const CKeyformGridSnapshot::Axis& axis)
{
    return axis.displayName.isEmpty() || axis.displayName == axis.parameterId ? axis.parameterId
        : QStringLiteral("%1（%2）").arg(axis.displayName, axis.parameterId);
}
QLabel* label(QWidget* parent, const QString& value = {})
{
    auto* result = new QLabel(value, parent);
    result->setTextFormat(Qt::PlainText); result->setWordWrap(true);
    result->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    return result;
}
QComboBox* combo(QWidget* parent, const char* objectName)
{
    auto* result = new QComboBox(parent); result->setObjectName(QString::fromLatin1(objectName));
    result->setMinimumContentsLength(1); result->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    result->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed); result->setMinimumWidth(0);
    auto* choices = new QListView(result); choices->setTextElideMode(Qt::ElideNone);
    choices->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded); result->setView(choices);
    result->setItemDelegate(new QStyledItemDelegate(choices));
    return result;
}

class AxisModel : public QAbstractListModel
{
public:
    AxisModel(const QVector<CKeyformGridSnapshot::Axis>& source, QObject* parent) : QAbstractListModel(parent), axes(source) {}
    int rowCount(const QModelIndex& parent = {}) const override { return parent.isValid() ? 0 : int(axes.size()); }
    QVariant data(const QModelIndex& index, int role) const override
    {
        if (!index.isValid() || index.row() >= axes.size()) return {};
        const QString value = QStringLiteral("轴 %1：%2").arg(index.row()).arg(name(axes[index.row()]));
        if (role == Qt::DisplayRole || role == Qt::AccessibleTextRole) return value;
        if (role == Qt::ToolTipRole) return tooltip(value);
        if (role == Qt::UserRole) return index.row();
        return {};
    }
private:
    QVector<CKeyformGridSnapshot::Axis> axes;
};

class CopyModel : public QAbstractListModel
{
public:
    using QAbstractListModel::QAbstractListModel;
    QVector<float> keys;
    void setKeys(const QVector<float>& value) { beginResetModel(); keys = value; endResetModel(); }
    int rowCount(const QModelIndex& parent = {}) const override { return parent.isValid() ? 0 : int(keys.size()); }
    QVariant data(const QModelIndex& index, int role) const override
    {
        if (!index.isValid() || index.row() >= keys.size()) return {};
        const auto value = QStringLiteral("旧关键值 #%1：%2").arg(index.row()).arg(text(keys[index.row()]));
        if (role == Qt::DisplayRole || role == Qt::AccessibleTextRole) return value;
        if (role == Qt::ToolTipRole) return tooltip(value);
        if (role == Qt::UserRole) return index.row();
        return {};
    }
};

struct Entry { Builder::BindingKey key; QString edited; };
struct Draft {
    QVector<Entry> rows;
    QString addText;
    int mode = 0;
    int copy = 0;
    bool initialized = false;
};

class DraftModel : public QAbstractTableModel
{
public:
    DraftModel(const CKeyformGridSnapshot& value, QObject* parent) : QAbstractTableModel(parent), snapshot(value), drafts(value.axes.size()) {}
    CKeyformGridSnapshot snapshot;
    QVector<Draft> drafts;
    std::function<void()> changed;
    int axis = -1;
    bool editable = false;

    Draft& current() { return drafts[axis]; }
    const Draft& current() const { return drafts[axis]; }
    void ensure(int index, int activeKeyform)
    {
        if (index < 0 || index >= drafts.size() || drafts[index].initialized) return;
        auto& draft = drafts[index];
        const auto& keys = snapshot.axes[index].keys;
        draft.rows.reserve(keys.size());
        for (int key = 0; key < keys.size(); ++key)
            draft.rows.append(Entry{{keys[key], key, Builder::BindingKey::InterpolateOldAxis, -1}, {}});
        int remainder = activeKeyform >= 0 && activeKeyform < snapshot.keyformCount ? activeKeyform : 0;
        for (int previous = 0; previous < index; ++previous) {
            if (snapshot.axes[previous].keys.isEmpty()) { remainder = 0; break; }
            remainder /= snapshot.axes[previous].keys.size();
        }
        draft.copy = keys.isEmpty() ? 0 : remainder % keys.size();
        draft.mode = keys.size() == 1 ? 1 : 0;
        draft.initialized = true;
    }
    int rowCount(const QModelIndex& parent = {}) const override { return parent.isValid() || axis < 0 ? 0 : int(current().rows.size()); }
    int columnCount(const QModelIndex& parent = {}) const override { return parent.isValid() ? 0 : 2; }
    void select(int index, int activeKeyform)
    {
        beginResetModel(); axis = index;
        ensure(axis, activeKeyform);
        endResetModel();
    }
    QVariant data(const QModelIndex& index, int role) const override
    {
        if (!index.isValid() || index.row() >= rowCount()) return {};
        const auto& entry = current().rows[index.row()];
        const QString value = entry.edited.isNull() ? text(entry.key.value) : entry.edited;
        QString source;
        if (entry.key.oldKeyIndex >= 0)
            source = QStringLiteral("保留 #%1（%2）").arg(entry.key.oldKeyIndex).arg(text(snapshot.axes[axis].keys[entry.key.oldKeyIndex]));
        else if (entry.key.source == Builder::BindingKey::CopyOldSlice)
            source = QStringLiteral("复制 #%1（%2）").arg(entry.key.copyOldKeyIndex).arg(text(snapshot.axes[axis].keys[entry.key.copyOldKeyIndex]));
        else source = QStringLiteral("旧轴插值");
        if (role == Qt::DisplayRole || role == Qt::EditRole || role == Qt::AccessibleTextRole) return index.column() ? source : value;
        if (role == Qt::ToolTipRole) return tooltip(index.column() ? source : QStringLiteral("%1\n双击或按 F2 编辑；支持科学计数法").arg(value));
        if (role == Qt::UserRole) return entry.key.oldKeyIndex;
        return {};
    }
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override
    {
        if (role != Qt::DisplayRole) return {};
        if (orientation == Qt::Vertical) return section + 1;
        return section == 0 ? QStringLiteral("关键值") : QStringLiteral("形状来源");
    }
    Qt::ItemFlags flags(const QModelIndex& index) const override
    {
        if (!index.isValid()) return Qt::NoItemFlags;
        return Qt::ItemIsEnabled | Qt::ItemIsSelectable | (editable && index.column() == 0 ? Qt::ItemIsEditable : Qt::NoItemFlags);
    }
    void liveText(const QModelIndex& index, const QString& value)
    {
        if (!editable || !index.isValid() || index.column() || index.row() >= rowCount()) return;
        auto& row = current().rows[index.row()];
        if ((row.edited.isNull() ? text(row.key.value) : row.edited) == value) return;
        row.edited = value;
        if (changed) changed();
    }
    bool setData(const QModelIndex& index, const QVariant& value, int role) override
    {
        if (role != Qt::EditRole || !editable || !index.isValid() || index.column() || index.row() >= rowCount()) return false;
        liveText(index, value.toString()); emit dataChanged(index, index); return true;
    }
    void append(const Builder::BindingKey& key)
    {
        const int row = rowCount(); beginInsertRows({}, row, row); current().rows.append(Entry{key, {}}); endInsertRows();
    }
    void erase(QVector<int> rows)
    {
        std::sort(rows.begin(), rows.end(), std::greater<int>());
        for (int row : rows) { beginRemoveRows({}, row, row); current().rows.removeAt(row); endRemoveRows(); }
    }
};

class ValueDelegate : public QStyledItemDelegate
{
public:
    ValueDelegate(DraftModel* value, QObject* parent) : QStyledItemDelegate(parent), model(value) {}
    QWidget* createEditor(QWidget* parent, const QStyleOptionViewItem&, const QModelIndex& index) const override
    {
        auto* editor = new QLineEdit(parent); editor->setObjectName(QStringLiteral("bindingKeysValue"));
        editor->setProperty("originalValue", index.data(Qt::EditRole));
        editor->setProperty("editedIndex", QVariant::fromValue(QPersistentModelIndex(index)));
        const QPersistentModelIndex persistent(index);
        connect(editor, &QLineEdit::textEdited, model.data(), [model = model, persistent](const QString& value) {
            if (model && persistent.isValid()) model->liveText(persistent, value);
        });
        return editor;
    }
    void setEditorData(QWidget* widget, const QModelIndex& index) const override
    {
        auto* editor = qobject_cast<QLineEdit*>(widget);
        const auto value = index.data(Qt::EditRole).toString();
        if (editor && editor->text() != value) editor->setText(value);
    }
    void setModelData(QWidget* widget, QAbstractItemModel* target, const QModelIndex& index) const override
    {
        if (const auto* editor = qobject_cast<QLineEdit*>(widget)) target->setData(index, editor->text(), Qt::EditRole);
    }
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (event->type() == QEvent::KeyPress && static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape && model) {
            const auto index = watched->property("editedIndex").value<QPersistentModelIndex>();
            if (index.isValid()) model->liveText(index, watched->property("originalValue").toString());
        }
        return QStyledItemDelegate::eventFilter(watched, event);
    }
private:
    QPointer<DraftModel> model;
};

QString sourceError(const CKeyformGridSnapshot& snapshot)
{
    if (snapshot.objectId.isEmpty() || snapshot.objectId.contains(QChar::Null)) return QStringLiteral("未选择有效对象");
    if (snapshot.objectType != "artmesh" && snapshot.objectType != "warp" && snapshot.objectType != "rotation")
        return QStringLiteral("此对象类型不支持编辑绑定关键值");
    if (!snapshot.axesValid) return snapshot.diagnostic.isEmpty() ? QStringLiteral("无法读取参数轴结构") : snapshot.diagnostic;
    if (snapshot.axes.isEmpty()) return QStringLiteral("对象尚未绑定参数轴");
    qint64 product = 1;
    for (const auto& axis : snapshot.axes) {
        if (axis.parameterId.isEmpty() || axis.parameterId.contains(QChar::Null) || axis.keys.isEmpty()) return QStringLiteral("参数轴名称或关键值列表无效");
        for (int index = 0; index < axis.keys.size(); ++index)
            if (!std::isfinite(axis.keys[index]) || (index && axis.keys[index] <= axis.keys[index - 1]))
                return QStringLiteral("旧轴关键值不是有限且严格递增的数值");
        if (product > std::numeric_limits<qint64>::max() / axis.keys.size()) return QStringLiteral("关键形组合数量溢出");
        product *= axis.keys.size();
    }
    if (product != snapshot.expectedKeyformCount || product != snapshot.keyformCount || snapshot.keyformCount <= 0)
        return QStringLiteral("关键形矩阵不完整，不能编辑绑定关键值");
    return {};
}

QString axisError(const DraftModel& model, int axisIndex)
{
    if (axisIndex < 0 || axisIndex >= model.snapshot.axes.size()) return QStringLiteral("请选择参数轴");
    const auto& axis = model.snapshot.axes[axisIndex];
    if (axis.parameterType != 0) return QStringLiteral("当前轴不是普通参数，暂不支持编辑融合参数轴");
    if (!std::isfinite(axis.minimum) || !std::isfinite(axis.maximum) || axis.minimum > axis.maximum)
        return QStringLiteral("参数有效范围无效");
    return {};
}

QString axisError(const DraftModel& model)
{
    return axisError(model, model.axis);
}

QString collectAxis(const DraftModel& model, int axisIndex,
                    Builder::EditBindingKeysBatchAxis& request, bool allowPending = false)
{
    const auto problem = axisError(model, axisIndex);
    if (!problem.isEmpty()) return problem;
    if (axisIndex >= model.drafts.size() || !model.drafts[axisIndex].initialized)
        return QStringLiteral("参数轴草稿尚未初始化");
    const auto& draft = model.drafts[axisIndex]; const auto& axis = model.snapshot.axes[axisIndex];
    if (!allowPending && !draft.addText.trimmed().isEmpty()) return QStringLiteral("添加区域有未加入的值，请先点击添加或清空输入");
    if (draft.rows.isEmpty()) return QStringLiteral("至少保留一个关键值");
    if (qint64(model.snapshot.keyformCount / axis.keys.size()) * draft.rows.size() > std::numeric_limits<int>::max())
        return QStringLiteral("关键形总数超出支持的范围");
    request.axisIndex = axisIndex; request.expectedKeys = axis.keys;
    request.keys.reserve(draft.rows.size());
    QSet<int> retained;
    for (int index = 0; index < draft.rows.size(); ++index) {
        const auto& row = draft.rows[index]; auto key = row.key;
        if (!row.edited.isNull() && !parse(row.edited, key.value)) return QStringLiteral("第 %1 行不是可表示的有限数值；可输入科学计数法").arg(index + 1);
        if (!std::isfinite(key.value)) return QStringLiteral("关键值必须是有限数值");
        if (key.oldKeyIndex >= 0) {
            if (key.oldKeyIndex >= axis.keys.size() || retained.contains(key.oldKeyIndex)) return QStringLiteral("旧关键值身份重复或无效");
            retained.insert(key.oldKeyIndex);
            if (key.value == axis.keys[key.oldKeyIndex]) key.value = axis.keys[key.oldKeyIndex];
        }
        const bool unchanged = key.oldKeyIndex >= 0 && key.value == axis.keys[key.oldKeyIndex];
        if (!unchanged && (key.value < axis.minimum || key.value > axis.maximum))
            return QStringLiteral("移动或新增的值必须位于参数范围 [%1, %2] 内").arg(text(axis.minimum), text(axis.maximum));
        if (key.oldKeyIndex < 0 && key.source == Builder::BindingKey::InterpolateOldAxis
            && (key.value < axis.keys.first() || key.value > axis.keys.last()))
            return QStringLiteral("旧轴区间 [%1, %2] 外不能插值，请明确选择复制旧形").arg(text(axis.keys.first()), text(axis.keys.last()));
        if (key.oldKeyIndex < 0 && key.source == Builder::BindingKey::CopyOldSlice
            && (key.copyOldKeyIndex < 0 || key.copyOldKeyIndex >= axis.keys.size())) return QStringLiteral("请选择有效的旧形复制来源");
        request.keys.append(key);
    }
    std::stable_sort(request.keys.begin(), request.keys.end(), [](const auto& a, const auto& b) { return a.value < b.value; });
    for (int index = 1; index < request.keys.size(); ++index)
        if (request.keys[index].value <= request.keys[index - 1].value) return QStringLiteral("关键值重复；每个数值只能保留一项");
    return {};
}

QString collect(const DraftModel& model, Builder::EditBindingKeysRequest& request, bool allowPending = false)
{
    Builder::EditBindingKeysBatchAxis candidate;
    const auto problem = collectAxis(model, model.axis, candidate, allowPending);
    if (!problem.isEmpty()) return problem;
    request.owner = {model.snapshot.objectType == "artmesh" ? Builder::Owner::ArtMesh
        : model.snapshot.objectType == "warp" ? Builder::Owner::Warp : Builder::Owner::Rotation, model.snapshot.objectId};
    request.axisIndex = candidate.axisIndex;
    request.parameterId = model.snapshot.axes[candidate.axisIndex].parameterId;
    request.expectedKeys = std::move(candidate.expectedKeys);
    request.keys = std::move(candidate.keys);
    return {};
}

qint64 draftKeyformCount(const DraftModel& model)
{
    qint64 total = 1;
    for (int index = 0; index < model.snapshot.axes.size(); ++index) {
        const auto& axis = model.snapshot.axes[index];
        const auto& draft = model.drafts[index];
        const qsizetype count = axis.parameterType == 0 && draft.initialized ? draft.rows.size() : axis.keys.size();
        if (count <= 0) return 0;
        if (total > std::numeric_limits<int>::max() / count) return qint64(std::numeric_limits<int>::max()) + 1;
        total *= count;
    }
    return total;
}

QString collectBatch(const DraftModel& model, int activeAxis,
                     Builder::EditBindingKeysBatchRequest& request)
{
    if (model.snapshot.axes.isEmpty()) return QStringLiteral("对象尚未绑定参数轴");
    request.owner = {model.snapshot.objectType == "artmesh" ? Builder::Owner::ArtMesh
        : model.snapshot.objectType == "warp" ? Builder::Owner::Warp : Builder::Owner::Rotation, model.snapshot.objectId};
    request.axes.clear(); request.axes.reserve(model.snapshot.axes.size());
    request.parameterId.clear();
    for (int axis = 0; axis < model.snapshot.axes.size(); ++axis) {
        if (!model.drafts[axis].initialized) return QStringLiteral("参数轴草稿尚未初始化");
        if (model.snapshot.axes[axis].parameterType != 0) {
            if (axis == activeAxis) return axisError(model, axis);
            continue;
        }
        Builder::EditBindingKeysBatchAxis candidate;
        const auto problem = collectAxis(model, axis, candidate);
        if (!problem.isEmpty()) return QStringLiteral("轴 %1：%2").arg(axis).arg(problem);
        candidate.parameterId = model.snapshot.axes[axis].parameterId;
        if (request.parameterId.isEmpty() || axis == activeAxis) request.parameterId = candidate.parameterId;
        request.axes.append(std::move(candidate));
    }
    if (request.parameterId.isEmpty()) request.parameterId = model.snapshot.axes.first().parameterId;
    if (draftKeyformCount(model) > std::numeric_limits<int>::max()) return QStringLiteral("关键形总数超出支持的范围");
    return {};
}

bool unchanged(const Builder::EditBindingKeysBatchAxis& request)
{
    if (request.keys.size() != request.expectedKeys.size()) return false;
    for (int index = 0; index < request.keys.size(); ++index) {
        const auto& key = request.keys[index];
        if (key.value != request.expectedKeys[index]) return false;
        if (key.oldKeyIndex >= 0) { if (key.oldKeyIndex != index) return false; }
        else if (key.source == Builder::BindingKey::CopyOldSlice && key.copyOldKeyIndex != index) return false;
    }
    return true;
}

bool unchanged(const Builder::EditBindingKeysBatchRequest& request)
{
    if (request.axes.isEmpty()) return true;
    return std::all_of(request.axes.cbegin(), request.axes.cend(), [](const auto& axis) { return unchanged(axis); });
}
}

class CBindingKeysDialog::State
{
public:
    DraftModel* model = nullptr;
    CopyModel* copies = nullptr;
    QComboBox *axis = nullptr, *source = nullptr, *copy = nullptr;
    QLineEdit* addValue = nullptr;
    QTableView* table = nullptr;
    QScrollArea* scroll = nullptr;
    QLabel *range = nullptr, *summary = nullptr, *error = nullptr;
    QPushButton *add = nullptr, *remove = nullptr, *apply = nullptr;
    QFormLayout* addForm = nullptr;
    QString structuralError, externalError;
    int active = -1;
    bool syncing = false, applying = false;
};

CBindingKeysDialog::CBindingKeysDialog(const CKeyformGridSnapshot& snapshot, int activeKeyform, QWidget* parent)
    : QDialog(parent), m_state(std::make_unique<State>())
{
    setObjectName(QStringLiteral("bindingKeysDialog")); setWindowTitle(QStringLiteral("编辑绑定关键值")); setModal(true);
    m_state->active = activeKeyform; m_state->structuralError = sourceError(snapshot);
    auto* outer = new QVBoxLayout(this);
    m_state->scroll = new QScrollArea(this); m_state->scroll->setObjectName(QStringLiteral("bindingKeysScroll"));
    m_state->scroll->setWidgetResizable(true); m_state->scroll->setFrameShape(QFrame::NoFrame);
    m_state->scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* content = new QWidget(m_state->scroll); auto* body = new QVBoxLayout(content);
    body->addWidget(label(content, QStringLiteral("对象：%1").arg(snapshot.objectId)));
    body->addWidget(label(content, QStringLiteral("可编辑多个参数轴；形状随原关键值移动，修改可撤销。")));
    auto* axes = new QFormLayout; axes->setRowWrapPolicy(QFormLayout::WrapLongRows);
    axes->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    m_state->axis = combo(content, "bindingKeysAxis");
    m_state->axis->setModel(new AxisModel(snapshot.axes, m_state->axis));
    axes->addRow(QStringLiteral("参数轴"), m_state->axis); body->addLayout(axes);
    m_state->range = label(content); m_state->range->setObjectName(QStringLiteral("bindingKeysRange")); body->addWidget(m_state->range);
    m_state->table = new QTableView(content); m_state->table->setObjectName(QStringLiteral("bindingKeysTable"));
    m_state->table->setAccessibleName(QStringLiteral("关键值草稿，应用时自动按数值排序"));
    m_state->table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_state->table->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_state->table->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
    m_state->table->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_state->table->setMinimumWidth(0); m_state->table->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    m_state->table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_state->table->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    m_state->model = new DraftModel(snapshot, m_state->table); m_state->table->setModel(m_state->model);
    for (int axis = 0; axis < snapshot.axes.size(); ++axis) m_state->model->ensure(axis, activeKeyform);
    m_state->table->setItemDelegate(new ValueDelegate(m_state->model, m_state->table));
    m_state->table->installEventFilter(this); body->addWidget(m_state->table);
    body->addWidget(label(content, QStringLiteral("双击数值或按 F2 编辑；支持 1e-8 等写法。应用时自动排序。")));
    m_state->remove = new QPushButton(QStringLiteral("删除选中值"), content);
    m_state->remove->setObjectName(QStringLiteral("bindingKeysDelete")); m_state->remove->setAutoDefault(false);
    m_state->remove->setToolTip(QStringLiteral("Delete 删除选中关键值，至少保留一个"));
    body->addWidget(m_state->remove, 0, Qt::AlignLeft);
    m_state->addForm = new QFormLayout; m_state->addForm->setRowWrapPolicy(QFormLayout::WrapLongRows);
    m_state->addForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    m_state->addValue = new QLineEdit(content); m_state->addValue->setObjectName(QStringLiteral("bindingKeysAddValue"));
    m_state->addValue->installEventFilter(this);
    m_state->addValue->setPlaceholderText(QStringLiteral("例如 0.5 或 1e-8"));
    m_state->addValue->setAccessibleName(QStringLiteral("新增关键值"));
    m_state->source = combo(content, "bindingKeysSource");
    m_state->source->addItem(QStringLiteral("按旧轴插值"), int(Builder::BindingKey::InterpolateOldAxis));
    m_state->source->addItem(QStringLiteral("复制旧形"), int(Builder::BindingKey::CopyOldSlice));
    m_state->copy = combo(content, "bindingKeysCopyFrom");
    m_state->copies = new CopyModel(m_state->copy); m_state->copy->setModel(m_state->copies);
    m_state->addForm->addRow(QStringLiteral("新增值"), m_state->addValue);
    m_state->addForm->addRow(QStringLiteral("形状来源"), m_state->source);
    m_state->addForm->addRow(QStringLiteral("复制来源"), m_state->copy); body->addLayout(m_state->addForm);
    body->addWidget(label(content, QStringLiteral("来源取打开窗口时的旧轴，包含其它轴的全部组合；区间外必须明确复制旧形。")));
    m_state->add = new QPushButton(QStringLiteral("添加关键值"), content); m_state->add->setObjectName(QStringLiteral("bindingKeysAdd"));
    m_state->add->setAutoDefault(false); body->addWidget(m_state->add, 0, Qt::AlignLeft);
    body->addStretch(); m_state->scroll->setWidget(content); outer->addWidget(m_state->scroll, 1);
    m_state->summary = label(this); m_state->summary->setObjectName(QStringLiteral("bindingKeysSummary")); outer->addWidget(m_state->summary);
    m_state->error = label(this); m_state->error->setObjectName(QStringLiteral("bindingKeysError")); outer->addWidget(m_state->error);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Apply | QDialogButtonBox::Cancel, this);
    buttons->setObjectName(QStringLiteral("bindingKeysButtons"));
    m_state->apply = buttons->button(QDialogButtonBox::Apply); m_state->apply->setObjectName(QStringLiteral("bindingKeysApply"));
    m_state->apply->setText(QStringLiteral("应用修改")); m_state->apply->setDefault(true);
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    outer->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_state->apply, &QPushButton::clicked, this, &CBindingKeysDialog::Apply);
    connect(m_state->axis, &QComboBox::currentIndexChanged, this, &CBindingKeysDialog::SelectAxis);
    connect(m_state->add, &QPushButton::clicked, this, &CBindingKeysDialog::AddKey);
    connect(m_state->remove, &QPushButton::clicked, this, &CBindingKeysDialog::DeleteKeys);
    connect(m_state->table->selectionModel(), &QItemSelectionModel::selectionChanged, this, [this] { Refresh(); });
    connect(m_state->addValue, &QLineEdit::textChanged, this, [this](const QString& value) {
        if (m_state->syncing || m_state->model->axis < 0) return;
        m_state->model->current().addText = value; m_state->externalError.clear(); Refresh();
    });
    connect(m_state->addValue, &QLineEdit::returnPressed, this, &CBindingKeysDialog::AddKey);
    connect(m_state->source, &QComboBox::currentIndexChanged, this, [this](int mode) {
        if (m_state->syncing || m_state->model->axis < 0) return;
        m_state->model->current().mode = mode; m_state->externalError.clear(); Refresh();
    });
    connect(m_state->copy, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (m_state->syncing || m_state->model->axis < 0) return;
        m_state->model->current().copy = index; m_state->externalError.clear(); Refresh();
    });
    m_state->model->changed = [this] { m_state->externalError.clear(); Refresh(); };
    int first = snapshot.axes.isEmpty() ? -1 : 0;
    for (int index = 0; index < snapshot.axes.size(); ++index) if (snapshot.axes[index].parameterType == 0) { first = index; break; }
    { const QSignalBlocker blocker(m_state->axis); m_state->axis->setCurrentIndex(first); }
    SelectAxis(first);
    resize(fontMetrics().horizontalAdvance(QLatin1Char('M')) * 42, fontMetrics().height() * 34);
}

CBindingKeysDialog::~CBindingKeysDialog()
{
    m_state->model->changed = {};
    for (auto* child : findChildren<QObject*>()) QObject::disconnect(child, nullptr, this, nullptr);
}

void CBindingKeysDialog::SelectAxis(int index)
{
    m_state->syncing = true; m_state->externalError.clear();
    m_state->model->select(m_state->structuralError.isEmpty() ? index : -1, m_state->active);
    const QSignalBlocker a(m_state->addValue), b(m_state->source), c(m_state->copy);
    if (m_state->model->axis >= 0) {
        auto& draft = m_state->model->current();
        m_state->copies->setKeys(m_state->model->snapshot.axes[index].keys);
        m_state->addValue->setText(draft.addText); m_state->source->setCurrentIndex(draft.mode); m_state->copy->setCurrentIndex(draft.copy);
        if (!draft.rows.isEmpty()) m_state->table->setCurrentIndex(m_state->model->index(qMin(draft.copy, int(draft.rows.size()) - 1), 0));
    } else { m_state->copies->setKeys({}); m_state->addValue->clear(); }
    m_state->syncing = false; Refresh();
}

bool CBindingKeysDialog::BuildRequest(Builder::EditBindingKeysRequest& request, QString* error) const
{
    const auto problem = m_state->structuralError.isEmpty() ? collect(*m_state->model, request) : m_state->structuralError;
    if (error) *error = problem;
    return problem.isEmpty();
}

bool CBindingKeysDialog::BuildBatchRequest(Builder::EditBindingKeysBatchRequest& request, QString* error) const
{
    Builder::EditBindingKeysBatchRequest candidate;
    const auto problem = m_state->structuralError.isEmpty()
        ? collectBatch(*m_state->model, m_state->model->axis, candidate) : m_state->structuralError;
    if (error) *error = problem;
    if (problem.isEmpty()) request = std::move(candidate);
    return problem.isEmpty();
}

int CBindingKeysDialog::SelectedAxis() const { return m_state->axis->currentIndex(); }
void CBindingKeysDialog::SetError(const QString& error) { m_state->externalError = error; Refresh(); }

void CBindingKeysDialog::AddKey()
{
    auto* model = m_state->model;
    if (!m_state->structuralError.isEmpty() || !axisError(*model).isEmpty()) return;
    auto& draft = model->current(); float value = 0;
    if (!parse(draft.addText, value)) { SetError(QStringLiteral("请输入可表示的有限数值，可使用科学计数法")); return; }
    const Entry entry{{value, -1, draft.mode == 1 ? Builder::BindingKey::CopyOldSlice : Builder::BindingKey::InterpolateOldAxis,
                       draft.mode == 1 ? draft.copy : -1}, {}};
    draft.rows.append(entry);
    Builder::EditBindingKeysRequest request; const QString problem = collect(*model, request, true);
    draft.rows.removeLast();
    if (!problem.isEmpty()) { SetError(problem); return; }
    model->append(entry.key); draft.addText.clear();
    { const QSignalBlocker blocker(m_state->addValue); m_state->addValue->clear(); }
    m_state->externalError.clear();
    const auto index = model->index(model->rowCount() - 1, 0);
    m_state->table->setCurrentIndex(index); m_state->table->scrollTo(index); Refresh();
}

void CBindingKeysDialog::DeleteKeys()
{
    auto* model = m_state->model;
    if (!m_state->structuralError.isEmpty() || !axisError(*model).isEmpty()) return;
    QVector<int> rows;
    for (const auto& index : m_state->table->selectionModel()->selectedRows()) rows.append(index.row());
    if (rows.isEmpty()) return;
    if (rows.size() >= model->rowCount()) { SetError(QStringLiteral("至少保留一个关键值")); return; }
    const int next = qMin(*std::min_element(rows.cbegin(), rows.cend()), model->rowCount() - rows.size() - 1);
    model->erase(rows); m_state->externalError.clear();
    m_state->table->setCurrentIndex(model->index(next, 0)); Refresh();
}

void CBindingKeysDialog::Refresh()
{
    if (m_state->syncing) return;
    auto* model = m_state->model;
    const auto problem = m_state->structuralError.isEmpty() ? axisError(*model) : m_state->structuralError;
    const bool enabled = problem.isEmpty(); model->editable = enabled;
    m_state->axis->setEnabled(m_state->structuralError.isEmpty());
    for (auto* control : std::array<QWidget*, 4>{m_state->addValue, m_state->source, m_state->copy, m_state->add}) control->setEnabled(enabled);
    m_state->remove->setEnabled(enabled && model->rowCount() > 1 && !m_state->table->selectionModel()->selectedRows().isEmpty());
    m_state->addForm->setRowVisible(m_state->copy, m_state->source->currentIndex() == 1);
    for (auto* combo : {m_state->axis, m_state->source, m_state->copy}) combo->setToolTip(tooltip(combo->currentText()));
    Builder::EditBindingKeysBatchRequest request; QString error;
    const bool valid = BuildBatchRequest(request, &error);
    const bool changed = valid && !unchanged(request);
    m_state->apply->setEnabled(changed && !m_state->applying);
    if (model->axis >= 0) {
        const auto& axis = model->snapshot.axes[model->axis];
        m_state->range->setText(QStringLiteral("可移动/新增范围：[%1, %2]\n旧轴区间：[%3, %4]；超范围旧值可原样保留。")
            .arg(text(axis.minimum), text(axis.maximum), text(axis.keys.first()), text(axis.keys.last())));
        const qint64 total = draftKeyformCount(*model);
        const QString totalText = total > std::numeric_limits<int>::max() ? QStringLiteral("超出支持范围") : QString::number(total);
        m_state->summary->setText(QStringLiteral("关键值：%1 → %2　总关键形：%3 → %4%5")
            .arg(axis.keys.size()).arg(model->rowCount()).arg(model->snapshot.keyformCount).arg(totalText)
            .arg(valid && !changed ? QStringLiteral("\n关键值未改变") : QString()));
    } else { m_state->range->clear(); m_state->summary->clear(); }
    m_state->error->setText(!m_state->externalError.isEmpty() ? m_state->externalError : error);
    const int margin = qMax(1, style()->pixelMetric(QStyle::PM_FocusFrameHMargin, nullptr, this));
    const int rowHeight = fontMetrics().height() + margin * 4;
    m_state->table->verticalHeader()->setDefaultSectionSize(rowHeight);
    m_state->table->horizontalHeader()->setMinimumSectionSize(fontMetrics().horizontalAdvance(QStringLiteral("-1.23456e-20")) + margin * 4);
    m_state->table->setFixedHeight(qBound(2, model->rowCount(), 6) * rowHeight + m_state->table->horizontalHeader()->sizeHint().height()
        + style()->pixelMetric(QStyle::PM_ScrollBarExtent, nullptr, m_state->table) + m_state->table->frameWidth() * 2);
}

void CBindingKeysDialog::Apply()
{
    if (m_state->applying) return;
    Builder::EditBindingKeysBatchRequest request; QString error;
    if (!BuildBatchRequest(request, &error)) { SetError(error); return; }
    if (unchanged(request)) { SetError(QStringLiteral("关键值未改变")); return; }
    m_state->applying = true; Refresh();
    const QPointer<CBindingKeysDialog> guard(this);
    emit ApplyRequested();
    if (guard) { m_state->applying = false; Refresh(); }
}

bool CBindingKeysDialog::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_state->addValue && event->type() == QEvent::KeyPress) {
        const auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) {
            if (!key->isAutoRepeat()) AddKey();
            return true;
        }
    }
    if (watched == m_state->table && event->type() == QEvent::KeyPress) {
        const int key = static_cast<QKeyEvent*>(event)->key();
        if (key == Qt::Key_Delete || key == Qt::Key_Backspace) { DeleteKeys(); return true; }
        if (key == Qt::Key_Return || key == Qt::Key_Enter) { Apply(); return true; }
        if (key == Qt::Key_F2) {
            const auto current = m_state->table->currentIndex();
            if (current.isValid() && m_state->model->editable) m_state->table->edit(current.siblingAtColumn(0));
            return true;
        }
    }
    return QDialog::eventFilter(watched, event);
}
