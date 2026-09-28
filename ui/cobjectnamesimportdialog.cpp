#include "cobjectnamesimportdialog.h"
#include "live2dstruct/cmodeldisplaynames.h"
#include <QAbstractTableModel>
#include <QAbstractItemDelegate>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QSet>
#include <QSortFilterProxyModel>
#include <QSplitter>
#include <QStyle>
#include <QStyleOptionViewItem>
#include <QStandardItemModel>
#include <QScopedValueRollback>
#include <QTableView>
#include <QVBoxLayout>
#include <exception>

namespace {
using Dialog = CObjectNamesImportDialog;
class ErrorLabel final : public QLabel
{
public:
    using QLabel::QLabel;
    QSize minimumSizeHint() const override { return {0, fontMetrics().height()}; }
    QSize sizeHint() const override { return {0, fontMetrics().height()}; }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setPen(palette().color(foregroundRole()));
        painter.drawText(contentsRect(), Qt::AlignVCenter | Qt::AlignLeft,
                         fontMetrics().elidedText(text(), Qt::ElideRight, contentsRect().width()));
    }
};
constexpr qsizetype MaximumRows = 100000;
constexpr qint64 MaximumTextBytes = 32LL * 1024 * 1024;
QString typeLabel(const QString& type)
{
    if (type == QLatin1String("part")) return QStringLiteral("部件");
    if (type == QLatin1String("artmesh")) return QStringLiteral("网格");
    if (type == QLatin1String("warp")) return QStringLiteral("曲面变形器");
    if (type == QLatin1String("rotation")) return QStringLiteral("旋转变形器");
    return type;
}
QString identity(const Dialog::Row& row) { return row.type + QChar(':') + row.id; }
bool freeze(const Dialog::Context& input, Dialog::Context& output, QString& error)
{
    if (input.rows.size() > MaximumRows || input.diagnostics.size() > 4096 || input.sourceDigest.size() != 32) {
        error = QStringLiteral("名称预览目录或来源指纹无效，请重新读取工程"); return false;
    }
    qint64 bytes = 0;
    const auto check = [&](const QString& text) {
        if (text.size() > (MaximumTextBytes - bytes) / 2 || !text.isValidUtf16()) return false;
        bytes += text.size() * 2;
        return true;
    };
    if (!check(input.sourcePath)) { error = QStringLiteral("来源路径超出预览限制"); return false; }
    for (const auto& text : input.diagnostics)
        if (!check(text)) { error = QStringLiteral("来源说明超出预览限制"); return false; }
    for (const auto& row : input.rows) {
        if (row.evidence.size() > 4096 || !check(row.type) || !check(row.id) || !check(row.currentName) ||
            !check(row.sourceName) || !check(row.status) || !check(row.reason)) {
            error = QStringLiteral("名称预览内容超出限制"); return false;
        }
        for (const auto& text : row.evidence)
            if (!check(text)) { error = QStringLiteral("名称匹配依据超出预览限制"); return false; }
    }
    const auto own = [](const QString& text) { return QString(text.constData(), text.size()); };
    Dialog::Context candidate;
    candidate.sourcePath = own(input.sourcePath);
    candidate.sourceDigest = QByteArray(input.sourceDigest.constData(), input.sourceDigest.size());
    for (const auto& text : input.diagnostics) candidate.diagnostics.append(own(text));
    QHash<QString, int> counts;
    for (const auto& row : input.rows) ++counts[identity(row)];
    for (const auto& row : input.rows) {
        Dialog::Row item{own(row.type), own(row.id), own(row.currentName), own(row.sourceName), own(row.status), own(row.reason), {}, row.eligible};
        for (const auto& text : row.evidence) item.evidence.append(own(text));
        QString invalid;
        if (counts.value(identity(row)) != 1) invalid = QStringLiteral("同类型对象 ID 重复，不能确定采用目标");
        else if (row.type != QLatin1String("part") && row.type != QLatin1String("artmesh") &&
                 row.type != QLatin1String("warp") && row.type != QLatin1String("rotation")) invalid = QStringLiteral("不支持的对象类型");
        else if (row.id.isEmpty() || row.id.toUtf8().size() > 64 || row.id.contains(QChar::Null)) invalid = QStringLiteral("对象 ID 无效");
        else if (!live2dstruct::CModelDisplayNames::ValidateName(row.sourceName, &invalid)) {}
        else if (row.currentName == row.sourceName) invalid = QStringLiteral("当前名称与工程名称相同，无需修改");
        if (!invalid.isEmpty()) {
            item.eligible = false;
            if (!item.reason.isEmpty()) item.reason += QChar('\n');
            item.reason += invalid;
        }
        candidate.rows.append(std::move(item));
    }
    output = std::move(candidate);
    return true;
}
class NamesModel final : public QAbstractTableModel
{
public:
    explicit NamesModel(const QVector<Dialog::Row>& rows, QObject* parent) : QAbstractTableModel(parent), m_rows(rows), m_checked(rows.size(), false) {
        for (int i = 0; i < m_rows.size(); ++i) m_checked[i] = m_rows[i].eligible && m_rows[i].currentName == m_rows[i].id;
    }
    int rowCount(const QModelIndex& parent = {}) const override { return parent.isValid() ? 0 : m_rows.size(); }
    int columnCount(const QModelIndex& parent = {}) const override { return parent.isValid() ? 0 : 6; }
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override {
        if (orientation != Qt::Horizontal || role != Qt::DisplayRole || section < 0 || section >= 6) return {};
        const QStringList labels{QStringLiteral("采用"), QStringLiteral("对象类型"), QStringLiteral("对象 ID"),
            QStringLiteral("当前名称"), QStringLiteral("工程名称"), QStringLiteral("匹配状态")};
        return labels[section];
    }
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override {
        if (!valid(index)) return {};
        const auto& row = m_rows[index.row()];
        if (role == Qt::UserRole) return index.row();
        if (role == Qt::CheckStateRole && index.column() == 0) return m_checked[index.row()] ? Qt::Checked : Qt::Unchecked;
        if (role == Qt::UserRole + 1) return row.reason;
        if (role == Qt::ToolTipRole && !row.reason.isEmpty())
            return QStringLiteral("<qt>%1</qt>").arg(row.reason.toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br/>")));
        if (role != Qt::DisplayRole && role != Qt::AccessibleTextRole) return {};
        switch (index.column()) {
        case 0: return row.eligible ? (m_checked[index.row()] ? QStringLiteral("采用") : QStringLiteral("保留")) : QStringLiteral("不可采用");
        case 1: return typeLabel(row.type);
        case 2: return row.id;
        case 3: return row.currentName;
        case 4: return row.sourceName;
        case 5: return row.status;
        }
        return {};
    }
    Qt::ItemFlags flags(const QModelIndex& index) const override {
        if (!valid(index)) return Qt::NoItemFlags;
        auto flags = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
        if (index.column() == 0 && m_rows[index.row()].eligible && !m_locked) flags |= Qt::ItemIsUserCheckable;
        return flags;
    }
    bool setData(const QModelIndex& index, const QVariant& value, int role) override {
        if (!valid(index) || index.column() != 0 || role != Qt::CheckStateRole || m_locked || !m_rows[index.row()].eligible ||
            (value.toInt() != Qt::Checked && value.toInt() != Qt::Unchecked)) return false;
        const bool checked = value.toInt() == Qt::Checked;
        if (m_checked[index.row()] == checked) return true;
        m_checked[index.row()] = checked;
        emit dataChanged(index, index, {Qt::DisplayRole, Qt::CheckStateRole, Qt::AccessibleTextRole});
        return true;
    }
    void SetLocked(bool locked) { m_locked = locked; }
    void SelectMissing(bool select) {
        if (m_locked) return;
        bool changed = false;
        for (int i = 0; i < m_rows.size(); ++i) {
            const bool checked = select && m_rows[i].eligible && m_rows[i].currentName == m_rows[i].id;
            changed = changed || checked != m_checked[i];
            m_checked[i] = checked;
        }
        if (changed && !m_rows.isEmpty()) emit dataChanged(index(0, 0), index(m_rows.size() - 1, 0), {Qt::DisplayRole, Qt::CheckStateRole, Qt::AccessibleTextRole});
    }
    QVector<Dialog::Selection> Selection() const {
        QVector<Dialog::Selection> result;
        for (int i = 0; i < m_rows.size(); ++i) if (m_checked[i] && m_rows[i].eligible)
            result.append({m_rows[i].type, m_rows[i].id, m_rows[i].sourceName});
        return result;
    }
    QPair<int, int> Counts() const {
        int fill = 0, replace = 0;
        for (int i = 0; i < m_rows.size(); ++i) if (m_checked[i]) {
            if (m_rows[i].currentName == m_rows[i].id) ++fill;
            else ++replace;
        }
        return {fill, replace};
    }
private:
    bool valid(const QModelIndex& index) const { return index.isValid() && index.model() == this && index.row() >= 0 && index.row() < m_rows.size() && index.column() >= 0 && index.column() < 6; }
    QVector<Dialog::Row> m_rows;
    QVector<bool> m_checked;
    bool m_locked = false;
};
class NamesTable final : public QTableView
{
public:
    using QTableView::QTableView;
protected:
    bool event(QEvent* event) override {
        const bool accepted = QTableView::event(event);
        if (event->type() == QEvent::Polish || event->type() == QEvent::Show ||
            event->type() == QEvent::StyleChange || event->type() == QEvent::FontChange ||
            event->type() == QEvent::ApplicationFontChange || event->type() == QEvent::DevicePixelRatioChange)
            UpdateShortColumns();
        return accepted;
    }
private:
    void UpdateShortColumns() {
        if (m_measuring || !model() || model()->columnCount() != 6) return;
        const QScopedValueRollback<bool> measuring(m_measuring, true);
        ensurePolished();
        QStyleOptionViewItem option;
        initViewItemOption(&option);
        option.widget = this;
        QStandardItemModel samples(1, model()->columnCount());
        samples.setData(samples.index(0, 0), QStringLiteral("不可采用"));
        samples.setData(samples.index(0, 0), Qt::Unchecked, Qt::CheckStateRole);
        samples.setData(samples.index(0, 1), QStringLiteral("曲面变形器"));
        samples.setData(samples.index(0, 2), QStringLiteral("ArtMesh1234"));
        samples.setData(samples.index(0, 5), QStringLiteral("结构匹配"));
        const int grid = showGrid() ? style()->pixelMetric(QStyle::PM_DefaultFrameWidth, nullptr, this) : 0;
        for (int column : {0, 1, 2, 5}) {
            const auto* delegate = itemDelegateForColumn(column);
            if (!delegate) delegate = itemDelegate();
            const int width = delegate->sizeHint(option, samples.index(0, column)).width() + grid;
            setColumnWidth(column, qMax(width, horizontalHeader()->sectionSizeHint(column)));
        }
    }
    bool m_measuring = false;
};
class NamesFilter final : public QSortFilterProxyModel
{
public:
    using QSortFilterProxyModel::QSortFilterProxyModel;
protected:
    bool filterAcceptsRow(int row, const QModelIndex& parent) const override {
        if (filterRegularExpression().pattern().isEmpty()) return true;
        if (sourceModel()->index(row, 0, parent).data(Qt::UserRole + 1).toString().contains(filterRegularExpression())) return true;
        for (int column = 1; column < sourceModel()->columnCount(); ++column)
            if (sourceModel()->index(row, column, parent).data().toString().contains(filterRegularExpression())) return true;
        return false;
    }
};
}

CObjectNamesImportDialog::CObjectNamesImportDialog(const Context& context, QWidget* parent) : QDialog(parent)
{
    setObjectName(QStringLiteral("objectNamesImportDialog"));
    setWindowTitle(QStringLiteral("采用 CMO3 工程名称"));
    try { m_valid = freeze(context, m_context, m_error); }
    catch (const std::exception&) { m_error = QStringLiteral("名称预览超出可用内存，请重新读取工程"); }
    auto* layout = new QVBoxLayout(this);
    auto* introduction = new QLabel(QStringLiteral("核对工程中的对象名称。已有名称默认保留；勾选后才会覆盖。"), this);
    introduction->setTextFormat(Qt::PlainText); introduction->setWordWrap(true);
    introduction->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred); layout->addWidget(introduction);
    m_search = new QLineEdit(this); m_search->setObjectName(QStringLiteral("objectNamesFilter"));
    m_search->setPlaceholderText(QStringLiteral("搜索对象 ID、名称或状态")); m_search->setClearButtonEnabled(true);
    m_search->setAccessibleName(QStringLiteral("筛选名称候选")); layout->addWidget(m_search);
    auto* actions = new QHBoxLayout;
    m_fill = new QPushButton(QStringLiteral("仅选择可补全项"), this); m_fill->setObjectName(QStringLiteral("objectNamesFillMissing"));
    m_keep = new QPushButton(QStringLiteral("全部保留"), this); m_keep->setObjectName(QStringLiteral("objectNamesKeepAll"));
    m_fill->setAutoDefault(false); m_keep->setAutoDefault(false);
    actions->addWidget(m_fill); actions->addWidget(m_keep); actions->addStretch(); layout->addLayout(actions);
    auto* splitter = new QSplitter(Qt::Vertical, this); splitter->setChildrenCollapsible(false);
    m_table = new NamesTable(splitter); m_table->setObjectName(QStringLiteral("objectNamesTable"));
    m_model = new NamesModel(m_context.rows, m_table);
    m_filter = new NamesFilter(m_table); m_filter->setSourceModel(m_model); m_filter->setFilterCaseSensitivity(Qt::CaseInsensitive);
    m_table->setModel(m_filter); m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection); m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setWordWrap(false); m_table->setAlternatingRowColors(true);
    m_table->verticalHeader()->hide(); m_table->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    auto* header = m_table->horizontalHeader();
    header->setSectionResizeMode(QHeaderView::Interactive);
    header->setStretchLastSection(false);
    header->setSectionResizeMode(3, QHeaderView::Stretch);
    header->setSectionResizeMode(4, QHeaderView::Stretch);
    m_table->setMinimumWidth(0); m_table->setMinimumHeight(fontMetrics().height() * 5);
    m_details = new QPlainTextEdit(splitter); m_details->setObjectName(QStringLiteral("objectNamesDetails"));
    m_details->setReadOnly(true); m_details->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    m_details->setMinimumWidth(0); m_details->setMinimumHeight(fontMetrics().height() * 4);
    splitter->setStretchFactor(0, 3); splitter->setStretchFactor(1, 2); layout->addWidget(splitter, 1);
    m_summary = new QLabel(this); m_summary->setObjectName(QStringLiteral("objectNamesSummary"));
    m_summary->setTextFormat(Qt::PlainText); m_summary->setWordWrap(true); m_summary->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    layout->addWidget(m_summary);
    m_status = new ErrorLabel(this); m_status->setObjectName(QStringLiteral("objectNamesStatus"));
    m_status->setTextFormat(Qt::PlainText); m_status->setWordWrap(false); m_status->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    layout->addWidget(m_status);
    auto* buttons = new QDialogButtonBox(this);
    m_apply = buttons->addButton(QStringLiteral("采用所选名称"), QDialogButtonBox::ActionRole); m_apply->setObjectName(QStringLiteral("objectNamesApply"));
    m_cancel = buttons->addButton(QStringLiteral("取消"), QDialogButtonBox::RejectRole); m_cancel->setObjectName(QStringLiteral("objectNamesCancel"));
    m_cancel->setDefault(true); layout->addWidget(buttons);
    connect(m_model, &QAbstractItemModel::dataChanged, this, [this] { RefreshControls(); });
    connect(m_search, &QLineEdit::textChanged, this, [this](const QString& text) {
        const QPointer<CObjectNamesImportDialog> alive(this);
        m_filter->setFilterFixedString(text);
        if (!alive) return;
        RefreshControls(); RefreshDetails();
    });
    connect(m_table->selectionModel(), &QItemSelectionModel::currentChanged, this, [this] { RefreshDetails(); });
    connect(m_fill, &QPushButton::clicked, this, [this] { static_cast<NamesModel*>(m_model)->SelectMissing(true); });
    connect(m_keep, &QPushButton::clicked, this, [this] { static_cast<NamesModel*>(m_model)->SelectMissing(false); });
    connect(m_apply, &QPushButton::clicked, this, &CObjectNamesImportDialog::Submit);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    resize(fontMetrics().averageCharWidth() * 92, fontMetrics().height() * 35);
    if (m_filter->rowCount()) m_table->setCurrentIndex(m_filter->index(0, 0));
    RefreshControls(); RefreshDetails();
}

void CObjectNamesImportDialog::SetSubmissionHandler(SubmissionHandler handler)
{
    if (m_submitting || m_completed) return;
    m_handler = std::move(handler); RefreshControls();
}

void CObjectNamesImportDialog::RefreshControls()
{
    const auto counts = static_cast<NamesModel*>(m_model)->Counts();
    m_summary->setText(QStringLiteral("已选择 %1 项补全、%2 项覆盖；当前显示 %3 / %4 项。筛选不会清除勾选。")
        .arg(counts.first).arg(counts.second).arg(m_filter->rowCount()).arg(m_model->rowCount()));
    const bool enabled = m_valid && !m_submitting && !m_completed;
    m_apply->setEnabled(enabled && bool(m_handler) && counts.first + counts.second > 0);
    m_cancel->setEnabled(!m_submitting && !m_completed);
    m_fill->setEnabled(enabled); m_keep->setEnabled(enabled); m_search->setEnabled(enabled); m_table->setEnabled(enabled);
    m_status->setText(m_error.section(QLatin1Char('\n'), 0, 0));
    m_status->setVisible(!m_error.isEmpty());
}

void CObjectNamesImportDialog::RefreshDetails()
{
    QStringList details;
    if (!m_error.isEmpty()) details.append(QStringLiteral("无法采用名称：\n%1").arg(m_error));
    const auto current = m_filter->mapToSource(m_table->currentIndex());
    if (current.isValid() && current.row() >= 0 && current.row() < m_context.rows.size()) {
        const auto& row = m_context.rows[current.row()];
        details.append(QStringLiteral("%1 · %2\n当前名称：%3\n工程名称：%4\n状态：%5")
            .arg(typeLabel(row.type), row.id, row.currentName, row.sourceName, row.status));
        if (!row.reason.isEmpty()) details.append(row.reason);
        if (!row.evidence.isEmpty()) details.append(QStringLiteral("匹配依据：\n%1").arg(row.evidence.join(QLatin1Char('\n'))));
    }
    details.append(QStringLiteral("来源工程：%1").arg(m_context.sourcePath));
    details.append(QStringLiteral("来源 SHA-256：%1").arg(QString::fromLatin1(m_context.sourceDigest.toHex())));
    details.append(QStringLiteral("结构相符只能作为匹配依据，不能证明两个文件同源。请确认对象与工程对应后采用。"));
    details.append(m_context.diagnostics);
    m_details->setPlainText(details.join(QStringLiteral("\n\n")));
}

void CObjectNamesImportDialog::SetError(const QString& error)
{
    m_error = error; RefreshControls(); RefreshDetails();
}

bool CObjectNamesImportDialog::Submit()
{
    if (m_submitting || m_completed || !m_valid || !m_handler) return false;
    const auto selection = static_cast<NamesModel*>(m_model)->Selection();
    if (selection.isEmpty()) return false;
    const auto handler = m_handler;
    const QPointer<CObjectNamesImportDialog> alive(this);
    m_submitting = true;
    static_cast<NamesModel*>(m_model)->SetLocked(true);
    RefreshControls();
    if (!alive) return false;
    QString error; bool accepted = false;
    try { accepted = handler(selection, &error); }
    catch (const std::exception&) { error = QStringLiteral("名称提交失败，请重试"); }
    catch (...) { error = QStringLiteral("名称提交失败，请重试"); }
    if (!alive) return accepted;
    m_submitting = false;
    static_cast<NamesModel*>(m_model)->SetLocked(false);
    if (!accepted) {
        SetError(error.isEmpty() ? QStringLiteral("未能采用名称，请检查选择后重试") : error);
        return false;
    }
    m_completed = true;
    m_error.clear();
    QDialog::done(QDialog::Accepted);
    return true;
}

void CObjectNamesImportDialog::done(int result)
{
    if (m_submitting || m_completed) return;
    if (result == QDialog::Accepted) { Submit(); return; }
    m_completed = true;
    static_cast<NamesModel*>(m_model)->SetLocked(true);
    QDialog::done(result);
}
