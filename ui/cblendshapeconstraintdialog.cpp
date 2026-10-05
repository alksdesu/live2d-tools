#include "cblendshapeconstraintdialog.h"
#include <QAbstractTableModel>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMap>
#include <QPointer>
#include <QPushButton>
#include <QScopeGuard>
#include <QStyledItemDelegate>
#include <QTableView>
#include <QVBoxLayout>
#include <cmath>
#include <limits>

using Value = live2dstruct::CMocBlendShapeConstraintBuilder::Value;

namespace {
QString valueText(float value) { return QString::number(double(value), 'g', std::numeric_limits<float>::max_digits10); }
bool parseValue(const QString& text, float& value)
{
    bool ok = false;
    const double precise = text.trimmed().toDouble(&ok);
    if (!ok || !std::isfinite(precise)) return false;
    const float rounded = float(precise);
    if (!std::isfinite(rounded) || (precise != 0.0 && rounded == 0.0f)) return false;
    value = rounded;
    return true;
}
}

class CBlendShapeConstraintTableModel : public QAbstractTableModel
{
public:
    CBlendShapeConstraintTableModel(const live2dstruct::CModelBlendShapeConstraintEntry& before, QObject* parent)
        : QAbstractTableModel(parent), m_minimum(before.parameterMinimum), m_maximum(before.parameterMaximum),
          m_before(before.values), m_values(before.values) {}
    int rowCount(const QModelIndex& parent = {}) const override { return parent.isValid() ? 0 : m_values.size(); }
    int columnCount(const QModelIndex& parent = {}) const override { return parent.isValid() ? 0 : 3; }
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override
    {
        if (!Valid(index)) return {};
        if (role == Qt::TextAlignmentRole) return int(Qt::AlignRight | Qt::AlignVCenter);
        if (role != Qt::DisplayRole && role != Qt::EditRole && role != Qt::ToolTipRole && role != Qt::AccessibleTextRole) return {};
        if (index.column() == 0) return index.row();
        const int offset = index.column() - 1;
        const QString value = m_invalid.contains({index.row(), offset}) ? m_invalid.value({index.row(), offset}) :
            valueText(offset == 0 ? m_values[index.row()].key : m_values[index.row()].weight);
        if (role == Qt::AccessibleTextRole) return QStringLiteral("曲线点 %1 的 %2：%3").arg(index.row()).arg(offset ? QStringLiteral("权重") : QStringLiteral("参数关键值"), value);
        return value;
    }
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override
    {
        if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};
        return section == 0 ? QStringLiteral("点") : section == 1 ? QStringLiteral("参数关键值") : QStringLiteral("权重");
    }
    Qt::ItemFlags flags(const QModelIndex& index) const override
    {
        if (!Valid(index)) return Qt::NoItemFlags;
        auto flags = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
        if (index.column() && !m_locked) flags |= Qt::ItemIsEditable;
        return flags;
    }
    bool setData(const QModelIndex& index, const QVariant& value, int role = Qt::EditRole) override
    {
        return role == Qt::EditRole && SetText(index, value.toString());
    }
    bool SetText(const QModelIndex& index, const QString& text)
    {
        if (m_locked || !Valid(index) || index.column() == 0) return false;
        const auto key = qMakePair(index.row(), index.column() - 1);
        float parsed = 0.0f;
        if (parseValue(text, parsed)) {
            if (index.column() == 1) m_values[index.row()].key = parsed;
            else m_values[index.row()].weight = parsed;
            m_invalid.remove(key);
        } else m_invalid.insert(key, text);
        emit dataChanged(index, index);
        if (changed) changed();
        return parsed == parsed && m_invalid.value(key).isEmpty();
    }
    bool Valid() const
    {
        if (m_values.isEmpty() || !m_invalid.isEmpty() || !std::isfinite(m_minimum) || !std::isfinite(m_maximum) || m_minimum > m_maximum) return false;
        float previous = 0.0f;
        for (int i = 0; i < m_values.size(); ++i) {
            const auto& value = m_values[i];
            if (!std::isfinite(value.key) || !std::isfinite(value.weight) || value.key < m_minimum || value.key > m_maximum || value.weight < 0.0f || value.weight > 1.0f || (i && value.key <= previous)) return false;
            previous = value.key;
        }
        return true;
    }
    bool Changed() const { return m_values != m_before; }
    const QVector<Value>& Values() const { return m_values; }
    QString Error() const
    {
        if (!m_invalid.isEmpty()) return QStringLiteral("曲线包含无效数字，请输入有限的单精度数值。");
        if (m_values.isEmpty()) return QStringLiteral("融合限制曲线不能为空。");
        if (!std::isfinite(m_minimum) || !std::isfinite(m_maximum) || m_minimum > m_maximum) return QStringLiteral("参数范围无效。");
        float previous = 0.0f;
        for (int i = 0; i < m_values.size(); ++i) {
            const auto& value = m_values[i];
            if (!std::isfinite(value.key) || !std::isfinite(value.weight)) return QStringLiteral("第 %1 个曲线点不是有限数值。").arg(i);
            if (value.key < m_minimum || value.key > m_maximum) return QStringLiteral("第 %1 个参数关键值超出范围 [%2, %3]。").arg(i).arg(valueText(m_minimum), valueText(m_maximum));
            if (value.weight < 0.0f || value.weight > 1.0f) return QStringLiteral("第 %1 个权重必须位于 [0, 1]。").arg(i);
            if (i && value.key <= previous) return QStringLiteral("参数关键值必须严格递增。");
            previous = value.key;
        }
        return {};
    }
    void SetLocked(bool locked) { m_locked = locked; }
    void Reset()
    {
        if (m_locked) return;
        beginResetModel(); m_values = m_before; m_invalid.clear(); endResetModel(); if (changed) changed();
    }
    std::function<void()> changed;
private:
    bool Valid(const QModelIndex& index) const { return index.isValid() && index.model() == this && index.row() >= 0 && index.row() < m_values.size() && index.column() >= 0 && index.column() < 3; }
    float m_minimum = 0, m_maximum = 1;
    QVector<Value> m_before, m_values;
    QMap<QPair<int, int>, QString> m_invalid;
    bool m_locked = false;
};

class ConstraintDelegate : public QStyledItemDelegate
{
public:
    explicit ConstraintDelegate(CBlendShapeConstraintTableModel* model, QObject* parent) : QStyledItemDelegate(parent), m_model(model) {}
    QWidget* createEditor(QWidget* parent, const QStyleOptionViewItem&, const QModelIndex& index) const override
    {
        auto* edit = new QLineEdit(parent); edit->setAccessibleName(QStringLiteral("曲线点 %1 %2").arg(index.row()).arg(index.column() == 1 ? QStringLiteral("参数关键值") : QStringLiteral("权重")));
        edit->setProperty("initialValue", index.data(Qt::EditRole)); edit->setProperty("constraintIndex", QVariant::fromValue(QPersistentModelIndex(index)));
        return edit;
    }
    void setEditorData(QWidget* editor, const QModelIndex& index) const override { if (auto* edit = qobject_cast<QLineEdit*>(editor)) { edit->setText(index.data(Qt::EditRole).toString()); edit->selectAll(); } }
    void setModelData(QWidget* editor, QAbstractItemModel* model, const QModelIndex& index) const override { if (auto* edit = qobject_cast<QLineEdit*>(editor)) model->setData(index, edit->text()); }
private:
    CBlendShapeConstraintTableModel* m_model;
};

CBlendShapeConstraintDialog::CBlendShapeConstraintDialog(const live2dstruct::CModelBlendShapeConstraintEntry& before, QWidget* parent) : QDialog(parent)
{
    setObjectName(QStringLiteral("blendShapeConstraintDialog")); setWindowTitle(QStringLiteral("编辑融合限制曲线")); setModal(true);
    auto* layout = new QVBoxLayout(this);
    auto* target = new QLabel(QStringLiteral("%1\n参数范围 [%2, %3] · %4 个曲线点").arg(before.parameterId, valueText(before.parameterMinimum), valueText(before.parameterMaximum)).arg(before.values.size()), this);
    target->setObjectName(QStringLiteral("blendShapeConstraintTarget")); target->setWordWrap(true); layout->addWidget(target);
    auto* hint = new QLabel(QStringLiteral("双击参数关键值或权重编辑。关键值必须严格递增，权重范围为 [0, 1]；取消不会提交模型。"), this); hint->setObjectName(QStringLiteral("blendShapeConstraintHint")); hint->setWordWrap(true); layout->addWidget(hint);
    m_model = new CBlendShapeConstraintTableModel(before, this); m_table = new QTableView(this); m_table->setObjectName(QStringLiteral("blendShapeConstraintTable")); m_table->setModel(m_model); m_table->setItemDelegate(new ConstraintDelegate(m_model, m_table)); m_table->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed | QAbstractItemView::AnyKeyPressed); m_table->setSelectionBehavior(QAbstractItemView::SelectItems); m_table->setAlternatingRowColors(true); m_table->verticalHeader()->hide(); m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch); layout->addWidget(m_table, 1);
    m_error = new QLabel(this); m_error->setObjectName(QStringLiteral("blendShapeConstraintError")); m_error->setWordWrap(true); layout->addWidget(m_error);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel | QDialogButtonBox::Reset, this); m_save = buttons->button(QDialogButtonBox::Save); m_save->setText(QStringLiteral("保存曲线")); m_save->setObjectName(QStringLiteral("primaryBtn")); buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消")); m_reset = buttons->button(QDialogButtonBox::Reset); m_reset->setText(QStringLiteral("恢复打开时状态")); connect(m_reset, &QPushButton::clicked, this, &CBlendShapeConstraintDialog::Reset); connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept); connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject); layout->addWidget(buttons);
    m_model->changed = [this] { m_error->setText(m_model->Error()); Refresh(); }; m_error->setText(m_model->Error()); resize(520, 480); Refresh();
}
void CBlendShapeConstraintDialog::SetSubmissionHandler(Submit handler) { if (!m_submitting && !m_accepted) m_submit = std::move(handler); Refresh(); }
const QVector<Value>& CBlendShapeConstraintDialog::Values() const { return m_model->Values(); }
QString CBlendShapeConstraintDialog::Error() const { return m_error->text(); }
void CBlendShapeConstraintDialog::Refresh() { m_model->SetLocked(m_submitting || m_accepted); m_table->setEnabled(!m_submitting && !m_accepted); m_save->setEnabled(!m_submitting && !m_accepted && bool(m_submit) && m_model->Valid() && m_model->Changed()); m_reset->setEnabled(!m_submitting && !m_accepted && (m_model->Changed() || !m_model->Error().isEmpty())); m_error->setVisible(!m_error->text().isEmpty()); }
void CBlendShapeConstraintDialog::Reset() { if (!m_submitting && !m_accepted) { m_table->setFocus(); m_model->Reset(); } }
void CBlendShapeConstraintDialog::done(int result)
{
    if (m_submitting || m_accepted) return;
    if (result != Accepted) { QDialog::done(result); return; }
    if (!m_submit || !m_model->Valid() || !m_model->Changed()) { Refresh(); return; }
    const QPointer<CBlendShapeConstraintDialog> dialog(this); const auto submit = m_submit; const auto values = Values(); m_submitting = true; Refresh(); QString error; bool success = false;
    try { success = submit(values, &error); } catch (...) { error = QStringLiteral("未能保存融合限制曲线，请检查模型状态后重试。当前草稿已保留。"); }
    if (!dialog) return; m_submitting = false; if (!success) { m_error->setText(error.isEmpty() ? QStringLiteral("未能保存融合限制曲线，请检查输入后重试。") : error); Refresh(); return; }
    m_accepted = true; m_error->clear(); QDialog::done(Accepted);
}
