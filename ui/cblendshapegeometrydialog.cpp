#include "cblendshapegeometrydialog.h"
#include <QAbstractTableModel>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMap>
#include <QPointer>
#include <QPushButton>
#include <QScopeGuard>
#include <QScrollArea>
#include <QStyledItemDelegate>
#include <QTableView>
#include <QVBoxLayout>
#include <cmath>
#include <cstring>
#include <limits>

namespace {
QString coordinateText(float value)
{
    if (value == 0 && std::signbit(value)) return QStringLiteral("-0");
    return QString::number(double(value), 'g', std::numeric_limits<float>::max_digits10);
}

bool sameDeltas(const QVector<float>& a, const QVector<float>& b)
{
    return a.size() == b.size() && (a.isEmpty() ||
        std::memcmp(a.constData(), b.constData(), size_t(a.size()) * sizeof(float)) == 0);
}

bool parseCoordinate(const QString& text, float& value)
{
    bool ok = false;
    const auto input = text.trimmed();
    const double precise = input.toDouble(&ok);
    if (!ok || !std::isfinite(precise)) return false;
    const float rounded = input.toFloat(&ok);
    if (!ok || !std::isfinite(rounded) || (precise != 0 && rounded == 0)) return false;
    value = precise == 0 && input.startsWith(QLatin1Char('-')) ? -0.0f : rounded;
    return true;
}
}

class CBlendShapeGeometryTableModel : public QAbstractTableModel
{
public:
    explicit CBlendShapeGeometryTableModel(const live2dstruct::CModelBlendShapeGeometryEntry& before,
                                          QObject* parent) : QAbstractTableModel(parent)
    {
        m_before.reserve(before.deltas.size());
        for (const float value : before.deltas) m_before.append(value);
        m_deltas = m_before;
        m_valid = !before.meshId.isEmpty() && before.shapeIndex >= 0 && before.bindingOffset >= 0 &&
            before.keyformOffset >= 0 && before.sourceKeyformIndex >= 0 && before.sourceDigest.size() == 32 &&
            !m_before.isEmpty() && m_before.size() % 2 == 0 && m_before.size() <= 131072;
        for (const float value : m_before) m_valid = m_valid && std::isfinite(value);
    }

    int rowCount(const QModelIndex& parent = {}) const override { return parent.isValid() ? 0 : int(m_deltas.size() / 2); }
    int columnCount(const QModelIndex& parent = {}) const override { return parent.isValid() ? 0 : 3; }
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override
    {
        if (!ValidIndex(index)) return {};
        if (role == Qt::TextAlignmentRole) return int(Qt::AlignRight | Qt::AlignVCenter);
        if (role != Qt::DisplayRole && role != Qt::EditRole && role != Qt::ToolTipRole &&
            role != Qt::AccessibleTextRole && role != Qt::AccessibleDescriptionRole) return {};
        if (index.column() == 0) return index.row();
        const int offset = Offset(index);
        const QString text = m_invalid.contains(offset) ? m_invalid.value(offset) : coordinateText(m_deltas[offset]);
        if (role == Qt::AccessibleDescriptionRole)
            return QStringLiteral("顶点 %1 的局部叠加位移 %2：%3").arg(index.row()).arg(index.column() == 1 ? QStringLiteral("X") : QStringLiteral("Y"), text);
        return text;
    }
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override
    {
        if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};
        if (section == 0) return QStringLiteral("顶点索引");
        if (section == 1) return QStringLiteral("位移 X");
        if (section == 2) return QStringLiteral("位移 Y");
        return {};
    }
    Qt::ItemFlags flags(const QModelIndex& index) const override
    {
        if (!ValidIndex(index)) return Qt::NoItemFlags;
        auto result = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
        if (index.column() && m_valid && !m_locked) result |= Qt::ItemIsEditable;
        return result;
    }
    bool setData(const QModelIndex& index, const QVariant& value, int role = Qt::EditRole) override
    {
        return role == Qt::EditRole && SetText(index, value.toString(), true);
    }
    bool SetText(const QModelIndex& index, const QString& text, bool notify)
    {
        if (!m_valid || m_locked || !ValidIndex(index) || index.column() == 0) return false;
        const int offset = Offset(index);
        float value = 0;
        const bool valid = parseCoordinate(text, value);
        if (valid) { m_deltas[offset] = value; m_invalid.remove(offset); }
        else m_invalid[offset] = text;
        if (notify) emit dataChanged(index, index);
        if (changed) changed();
        return valid;
    }
    bool Valid() const { return m_valid && m_invalid.isEmpty(); }
    bool Changed() const { return !sameDeltas(m_before, m_deltas); }
    const QVector<float>& Deltas() const { return m_deltas; }
    QString Error() const
    {
        if (!m_valid) return QStringLiteral("融合形快照无效：需要完整的来源、端点和有限的 XY 位移对。");
        if (m_invalid.isEmpty()) return {};
        const int offset = m_invalid.firstKey();
        return QStringLiteral("顶点 %1 的位移 %2 无效。请输入有限的单精度数值（小数点用 .，可用科学计数法）；不接受溢出或下溢为零。").arg(offset / 2).arg(offset % 2 ? QStringLiteral("Y") : QStringLiteral("X"));
    }
    void SetLocked(bool locked) { m_locked = locked; }
    void Reset()
    {
        if (m_locked) return;
        beginResetModel(); m_deltas = m_before; m_invalid.clear(); endResetModel();
        if (changed) changed();
    }
    std::function<void()> changed;

private:
    bool ValidIndex(const QModelIndex& index) const
    {
        return index.isValid() && index.model() == this && index.row() >= 0 && index.row() < rowCount() &&
            index.column() >= 0 && index.column() < 3;
    }
    static int Offset(const QModelIndex& index) { return index.row() * 2 + index.column() - 1; }
    QVector<float> m_before, m_deltas;
    QMap<int, QString> m_invalid;
    bool m_valid = false;
    bool m_locked = false;
};

namespace {
class CoordinateDelegate : public QStyledItemDelegate
{
public:
    explicit CoordinateDelegate(CBlendShapeGeometryTableModel* model, QObject* parent)
        : QStyledItemDelegate(parent), m_model(model) {}
    QWidget* createEditor(QWidget* parent, const QStyleOptionViewItem&, const QModelIndex& index) const override
    {
        auto* input = new QLineEdit(parent);
        input->setAccessibleName(QStringLiteral("顶点 %1 位移 %2").arg(index.row()).arg(index.column() == 1 ? QStringLiteral("X") : QStringLiteral("Y")));
        input->setProperty("initialCoordinate", index.data(Qt::EditRole));
        input->setProperty("coordinateIndex", QVariant::fromValue(QPersistentModelIndex(index)));
        connect(input, &QLineEdit::textEdited, m_model, [model = m_model, index = QPersistentModelIndex(index)](const QString& text) {
            if (index.isValid()) model->SetText(index, text, false);
        });
        return input;
    }
    void setEditorData(QWidget* editor, const QModelIndex& index) const override
    {
        auto* input = qobject_cast<QLineEdit*>(editor);
        if (input) { input->setText(index.data(Qt::EditRole).toString()); input->selectAll(); }
    }
    void setModelData(QWidget* editor, QAbstractItemModel* model, const QModelIndex& index) const override
    {
        auto* input = qobject_cast<QLineEdit*>(editor);
        if (input) model->setData(index, input->text());
    }
    bool eventFilter(QObject* object, QEvent* event) override
    {
        if (event->type() == QEvent::KeyPress && static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape) {
            const auto index = object->property("coordinateIndex").value<QPersistentModelIndex>();
            if (index.isValid()) m_model->setData(index, object->property("initialCoordinate"));
        }
        return QStyledItemDelegate::eventFilter(object, event);
    }
private:
    CBlendShapeGeometryTableModel* m_model;
};
}

CBlendShapeGeometryDialog::CBlendShapeGeometryDialog(const live2dstruct::CModelBlendShapeGeometryEntry& before,
                                                     QWidget* parent) : QDialog(parent)
{
    setObjectName(QStringLiteral("blendShapeGeometryDialog"));
    setWindowTitle(QStringLiteral("编辑融合形几何")); setModal(true);
    auto* layout = new QVBoxLayout(this);
    auto* scroll = new QScrollArea(this);
    scroll->setObjectName(QStringLiteral("blendShapeGeometryScroll"));
    scroll->setWidgetResizable(true); scroll->setFrameShape(QFrame::NoFrame);
    auto* content = new QWidget(scroll);
    auto* body = new QVBoxLayout(content);
    const auto label = [content](const QString& text, const char* name) {
        auto* result = new QLabel(text, content);
        result->setObjectName(QString::fromLatin1(name)); result->setTextFormat(Qt::PlainText);
        result->setWordWrap(true); result->setMinimumWidth(0);
        result->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        result->setTextInteractionFlags(Qt::TextSelectableByMouse);
        return result;
    };
    body->addWidget(label(QStringLiteral("%1\n融合形 %2 · 绑定 %3 · 关键形 %4 · 来源行 %5 · %6 个顶点")
        .arg(before.meshId).arg(before.shapeIndex).arg(before.bindingOffset).arg(before.keyformOffset)
        .arg(before.sourceKeyformIndex).arg(before.deltas.size() / 2), "blendShapeGeometryTarget"));
    body->addWidget(label(QStringLiteral("编辑的是模型局部坐标中的叠加位移，不是绝对坐标，也不会修改 PSD 原稿。双击位移单元格或按 F2 编辑；保存后可通过主窗口撤销。"), "blendShapeGeometryHint"));
    m_model = new CBlendShapeGeometryTableModel(before, this);
    m_table = new QTableView(content);
    m_table->setObjectName(QStringLiteral("blendShapeGeometryTable"));
    m_table->setAccessibleName(QStringLiteral("融合形逐顶点局部位移"));
    m_table->setAccessibleDescription(QStringLiteral("顶点索引只读，位移 X 和位移 Y 可编辑。数值小数点使用英文句点。"));
    m_table->setModel(m_model); m_table->setItemDelegate(new CoordinateDelegate(m_model, m_table));
    m_table->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed | QAbstractItemView::AnyKeyPressed);
    m_table->setSelectionBehavior(QAbstractItemView::SelectItems);
    m_table->setAlternatingRowColors(true); m_table->verticalHeader()->hide();
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_table->horizontalHeader()->setMinimumSectionSize(fontMetrics().horizontalAdvance(QString(8, QLatin1Char('0'))));
    m_table->setMinimumHeight(fontMetrics().lineSpacing() * 7);
    body->addWidget(m_table, 1);
    m_error = label({}, "blendShapeGeometryError"); body->addWidget(m_error);
    scroll->setWidget(content); layout->addWidget(scroll, 1);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel | QDialogButtonBox::Reset, this);
    buttons->setObjectName(QStringLiteral("blendShapeGeometryButtons"));
    m_save = buttons->button(QDialogButtonBox::Save); m_save->setText(QStringLiteral("保存融合形"));
    m_save->setObjectName(QStringLiteral("primaryBtn")); m_save->setDefault(true);
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    buttons->button(QDialogButtonBox::Cancel)->setAutoDefault(false);
    m_reset = buttons->button(QDialogButtonBox::Reset); m_reset->setText(QStringLiteral("恢复打开时状态"));
    m_reset->setAutoDefault(false);
    connect(m_reset, &QPushButton::clicked, this, &CBlendShapeGeometryDialog::Reset);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
    m_model->changed = [this] { m_error->setText(m_model->Error()); Refresh(); };
    m_error->setText(m_model->Error());
    resize(fontMetrics().horizontalAdvance(QString(64, QLatin1Char('0'))), fontMetrics().lineSpacing() * 30);
    Refresh();
}

void CBlendShapeGeometryDialog::SetSubmissionHandler(Submit handler)
{
    if (m_submitting || m_accepted) return;
    m_submit = std::move(handler); Refresh();
}
const QVector<float>& CBlendShapeGeometryDialog::Deltas() const { return m_model->Deltas(); }
QString CBlendShapeGeometryDialog::Error() const { return m_error->text(); }

void CBlendShapeGeometryDialog::Refresh()
{
    m_model->SetLocked(m_submitting || m_accepted);
    m_table->setEnabled(!m_submitting && !m_accepted);
    m_save->setEnabled(!m_submitting && !m_accepted && bool(m_submit) && m_model->Valid() && m_model->Changed());
    m_reset->setEnabled(!m_submitting && !m_accepted && (m_model->Changed() || !m_model->Error().isEmpty()));
    m_error->setVisible(!m_error->text().isEmpty());
    m_error->setToolTip(QStringLiteral("<qt>%1</qt>").arg(m_error->text().toHtmlEscaped()));
}

void CBlendShapeGeometryDialog::Reset()
{
    if (m_submitting || m_accepted) return;
    m_table->setFocus(); m_model->Reset();
}

void CBlendShapeGeometryDialog::done(int result)
{
    if (m_submitting || m_accepted) return;
    if (result != Accepted) { QDialog::done(result); return; }
    if (!m_submit || !m_model->Valid() || !m_model->Changed()) { Refresh(); return; }
    const QPointer<CBlendShapeGeometryDialog> dialog(this);
    const auto submit = m_submit;
    const auto deltas = Deltas();
    m_submitting = true;
    const auto finish = qScopeGuard([dialog] { if (dialog) { dialog->m_submitting = false; dialog->Refresh(); } });
    Refresh();
    QString error;
    bool success = false;
    try { success = submit(deltas, &error); }
    catch (...) { error = QStringLiteral("未能保存融合形，请检查模型状态后重试。当前草稿已保留。"); }
    if (!dialog) return;
    if (!success) {
        m_error->setText(error.isEmpty() ? QStringLiteral("未能保存融合形，请检查输入后重试。") : error);
        return;
    }
    m_accepted = true; m_error->clear(); QDialog::done(Accepted);
}
