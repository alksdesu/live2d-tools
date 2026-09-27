#include "cparameterorganizationdialog.h"
#include "ctableeditdelegate.h"
#include "live2dstruct/cmodeldocument.h"
#include "ui/para/ccidjson.h"
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QCompleter>
#include <QDialogButtonBox>
#include <QDropEvent>
#include <QFormLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QPointer>
#include <QScopeGuard>
#include <QScrollArea>
#include <QStyledItemDelegate>
#include <exception>
#include <QMenu>
#include <QPushButton>
#include <QPainter>
#include <QSignalBlocker>
#include <QStyle>
#include <QTabWidget>
#include <QTableWidget>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QUuid>
#include <QVBoxLayout>
#include <algorithm>

namespace {
using Organization = live2dstruct::CParameterOrganization;
constexpr int kindRole = Qt::UserRole, idRole = Qt::UserRole + 1, colorRole = Qt::UserRole + 2;
class COrganizationTree final : public QTreeWidget
{
public:
    using QTreeWidget::QTreeWidget;
protected:
    bool edit(const QModelIndex& index, EditTrigger trigger, QEvent* event) override
    {
        if (index.column() != 0 || index.data(kindRole).toInt() != Organization::Node::Folder) return false;
        return QTreeWidget::edit(index, trigger, event);
    }
    void dropEvent(QDropEvent* event) override
    {
        auto* target = itemAt(event->position().toPoint());
        const auto position = dropIndicatorPosition();
        const bool inside = target && position == OnItem;
        if (inside && target->data(0, kindRole).toInt() != Organization::Node::Folder) { event->ignore(); return; }
        for (auto* item : selectedItems()) if (item->data(0, kindRole).toInt() == Organization::Node::Folder &&
                (inside || (target && target->parent() && position != OnViewport))) { event->ignore(); return; }
        QTreeWidget::dropEvent(event);
    }
};
void compactCombo(QComboBox* combo)
{
    combo->setMinimumContentsLength(1);
    combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    combo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    combo->setMinimumWidth(0);
    auto* view = new QListView(combo);
    view->setTextElideMode(Qt::ElideNone);
    view->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    combo->setView(view);
    combo->setItemDelegate(new QStyledItemDelegate(view));
}
QString selectedParameter(QComboBox* combo)
{
    return combo->currentIndex() >= 0 && combo->currentText() == combo->itemText(combo->currentIndex())
        ? combo->currentData().toString() : QString();
}
QList<QTreeWidgetItem*> selectedInTreeOrder(QTreeWidget* tree)
{
    QList<QTreeWidgetItem*> result;
    for (QTreeWidgetItemIterator item(tree); *item; ++item)
        if ((*item)->isSelected()) result.append(*item);
    return result;
}
QString parameterName(const live2dstruct::CModelDocument& document, const CCidJson* displayInfo, const QString& id)
{
    const auto metadata = document.ParameterMetadataOverrides().value(id);
    if (!metadata.displayName.isEmpty() && metadata.displayName != id) return metadata.displayName;
    const auto name = displayInfo ? displayInfo->getParaName(id) : QString();
    return name.isEmpty() ? id : name;
}
}

Organization::State CParameterOrganizationDialog::InitialState(
    const live2dstruct::CModelDocument& document, const CCidJson* displayInfo)
{
    if (document.ParameterOrganization().configured) return document.ParameterOrganization();
    Organization::State result; result.configured = true;
    QSet<QString> available, placed, folders;
    for (const auto& parameter : document.Parameters()) available.insert(parameter.id);
    if (displayInfo) for (const auto& group : displayInfo->getGroups()) {
        const QString folder = group.strGroup;
        if (!folder.isEmpty() && !folders.contains(folder)) {
            const auto name = displayInfo->getGroupIdToName().value(folder, folder);
            result.nodes.append({Organization::Node::Folder, folder, {}, name.isEmpty() ? folder : name, {}});
            folders.insert(folder);
        }
        for (const auto& id : group.vetParam) if (available.contains(id) && !placed.contains(id)) {
            result.nodes.append({Organization::Node::Parameter, id, folder, {}, {}});
            placed.insert(id);
        }
    }
    for (const auto& parameter : document.Parameters()) if (!placed.contains(parameter.id))
        result.nodes.append({Organization::Node::Parameter, parameter.id, {}, {}, {}});
    return result;
}

CParameterOrganizationDialog::CParameterOrganizationDialog(const live2dstruct::CModelDocument& document,
                                                           const CCidJson* displayInfo, QWidget* parent)
    : QDialog(parent), m_result(InitialState(document, displayInfo))
{
    setWindowTitle(QStringLiteral("参数组织与配对"));
    setObjectName(QStringLiteral("parameterOrganizationDialog"));
    const auto metrics = fontMetrics();
    resize(metrics.horizontalAdvance(QString(74, QLatin1Char('0'))), metrics.lineSpacing() * 30);
    auto* layout = new QVBoxLayout(this);
    auto* scroll = new QScrollArea(this);
    scroll->setObjectName(QStringLiteral("parameterOrganizationScroll"));
    scroll->setWidgetResizable(true); scroll->setFrameShape(QFrame::NoFrame);
    auto* content = new QWidget(scroll); m_content = content;
    auto* contentLayout = new QVBoxLayout(content); contentLayout->setContentsMargins(0, 0, 0, 0);
    auto* tabs = new QTabWidget(content); tabs->setObjectName(QStringLiteral("parameterOrganizationTabs"));
    contentLayout->addWidget(tabs); scroll->setWidget(content); layout->addWidget(scroll, 1);
    auto* groups = new QWidget(tabs);
    auto* groupLayout = new QVBoxLayout(groups);
    auto* tools = new QToolBar(groups);
    tools->setMovable(false); tools->setFloatable(false);
    const auto addAction = [tools](const QString& label, const QString& name) {
        auto* action = tools->addAction(label); action->setObjectName(name); return action;
    };
    connect(addAction(QStringLiteral("新建文件夹"), QStringLiteral("addParameterFolder")), &QAction::triggered, this, &CParameterOrganizationDialog::AddFolder);
    connect(addAction(QStringLiteral("解散文件夹"), QStringLiteral("dissolveParameterFolder")), &QAction::triggered, this, &CParameterOrganizationDialog::DissolveFolders);
    connect(addAction(QStringLiteral("上移"), QStringLiteral("moveParameterUp")), &QAction::triggered, this, [this] { MoveSelected(-1); });
    connect(addAction(QStringLiteral("下移"), QStringLiteral("moveParameterDown")), &QAction::triggered, this, [this] { MoveSelected(1); });
    connect(addAction(QStringLiteral("标签颜色"), QStringLiteral("parameterLabelColor")), &QAction::triggered, this, [this] { SetLabelColor(false); });
    connect(addAction(QStringLiteral("清除颜色"), QStringLiteral("clearParameterLabelColor")), &QAction::triggered, this, [this] { SetLabelColor(true); });
    for (auto* button : tools->findChildren<QToolButton*>()) button->setFocusPolicy(Qt::StrongFocus);
    groupLayout->addWidget(tools);
    m_tree = new COrganizationTree(groups);
    m_tree->setObjectName(QStringLiteral("parameterOrganizationTree"));
    m_tree->setHeaderLabels({QStringLiteral("名称"), QStringLiteral("ID")});
    m_tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_tree->header()->setSectionResizeMode(1, QHeaderView::Interactive);
    m_tree->header()->resizeSection(1, metrics.horizontalAdvance(QString(20, QLatin1Char('0'))));
    m_tree->setTextElideMode(Qt::ElideMiddle);
    m_tree->setMinimumWidth(0);
    m_tree->setMinimumHeight(metrics.lineSpacing() * 7);
    m_treeDelegate = new CTableEditDelegate(m_tree); m_tree->setItemDelegate(m_treeDelegate);
    m_tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_tree->setDragDropMode(QAbstractItemView::InternalMove);
    m_tree->setDefaultDropAction(Qt::MoveAction);
    m_tree->setAccessibleName(QStringLiteral("参数及文件夹顺序，可拖动排序"));
    QHash<QString, QTreeWidgetItem*> folders;
    for (const auto& node : m_result.nodes) if (node.kind == Organization::Node::Folder) {
        auto* item = new QTreeWidgetItem({node.name, node.id});
        item->setData(0, kindRole, node.kind); item->setData(0, idRole, node.id); item->setData(0, colorRole, node.color);
        item->setFlags(item->flags() | Qt::ItemIsEditable | Qt::ItemIsDropEnabled);
        folders.insert(node.id, item); SetNodeDecoration(item);
    }
    for (const auto& parameter : document.Parameters()) {
        m_parameterIds.insert(parameter.id);
        m_names.insert(parameter.id, parameterName(document, displayInfo, parameter.id));
    }
    for (const auto& node : m_result.nodes) {
        if (node.kind == Organization::Node::Folder) { m_tree->addTopLevelItem(folders.value(node.id)); continue; }
        auto* item = new QTreeWidgetItem({m_names.value(node.id, node.id), node.id});
        item->setData(0, kindRole, node.kind); item->setData(0, idRole, node.id); item->setData(0, colorRole, node.color);
        item->setFlags(item->flags() & ~Qt::ItemIsDropEnabled);
        if (auto* folder = folders.value(node.parentFolder, nullptr)) folder->addChild(item);
        else m_tree->addTopLevelItem(item);
        SetNodeDecoration(item);
    }
    m_tree->expandAll(); groupLayout->addWidget(m_tree, 1);
    auto* destinationRow = new QHBoxLayout;
    m_destination = new QComboBox(groups);
    m_destination->setObjectName(QStringLiteral("parameterFolderDestination"));
    compactCombo(m_destination);
    auto* move = new QPushButton(QStringLiteral("移动选中参数"), groups);
    move->setObjectName(QStringLiteral("moveParametersToFolder"));
    destinationRow->addWidget(m_destination, 1); destinationRow->addWidget(move);
    groupLayout->addLayout(destinationRow);
    connect(move, &QPushButton::clicked, this, &CParameterOrganizationDialog::MoveParameters);
    connect(m_tree, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* item) {
        const QSignalBlocker blocked(m_tree);
        SetNodeDecoration(item); RefreshFolders();
    });
    auto* hint = new QLabel(QStringLiteral("双击文件夹改名；拖动名称可排序。解散文件夹会保留其中的参数。"), groups);
    hint->setWordWrap(true); hint->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred); groupLayout->addWidget(hint);
    tabs->addTab(groups, QStringLiteral("文件夹与顺序"));

    auto* pairs = new QWidget(tabs);
    auto* pairsLayout = new QVBoxLayout(pairs);
    auto* pairHint = new QLabel(QStringLiteral("同向按最小值到最大值的比例联动；反向按相反比例联动。动画和物理预览保持各自的参数值。"), pairs);
    pairHint->setWordWrap(true); pairHint->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred); pairsLayout->addWidget(pairHint);
    m_pairs = new QTableWidget(0, 4, pairs);
    m_pairs->setObjectName(QStringLiteral("parameterPairTable"));
    m_pairs->setHorizontalHeaderLabels({QStringLiteral("参数 A"), QStringLiteral("参数 B"), QStringLiteral("方向"), QStringLiteral("启用")});
    m_pairs->setSelectionBehavior(QAbstractItemView::SelectRows); m_pairs->setSelectionMode(QAbstractItemView::SingleSelection);
    m_pairs->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_pairs->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_pairs->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    for (int column : {2, 3}) m_pairs->horizontalHeader()->setSectionResizeMode(column, QHeaderView::ResizeToContents);
    m_pairs->verticalHeader()->hide();
    m_pairs->setTextElideMode(Qt::ElideMiddle); m_pairs->setMinimumWidth(0);
    m_pairs->setMinimumHeight(metrics.lineSpacing() * 5); pairsLayout->addWidget(m_pairs, 1);
    auto* form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    m_first = new QComboBox(pairs); m_second = new QComboBox(pairs);
    m_first->setObjectName(QStringLiteral("pairedParameterA")); m_second->setObjectName(QStringLiteral("pairedParameterB"));
    for (auto* combo : {m_first, m_second}) {
        compactCombo(combo);
        combo->setEditable(true); combo->setInsertPolicy(QComboBox::NoInsert);
        for (const auto& parameter : document.Parameters()) combo->addItem(m_names.value(parameter.id) + QStringLiteral(" · ") + parameter.id, parameter.id);
        combo->completer()->setFilterMode(Qt::MatchContains);
    }
    if (m_second->count() > 1) m_second->setCurrentIndex(1);
    m_reversed = new QCheckBox(QStringLiteral("反向联动"), pairs);
    m_enabled = new QCheckBox(QStringLiteral("启用此配对"), pairs); m_enabled->setChecked(true);
    m_reversed->setObjectName(QStringLiteral("reverseParameterPair")); m_enabled->setObjectName(QStringLiteral("enableParameterPair"));
    form->addRow(QStringLiteral("参数 A"), m_first); form->addRow(QStringLiteral("参数 B"), m_second);
    form->addRow(m_reversed); form->addRow(m_enabled); pairsLayout->addLayout(form);
    auto* pairButtons = new QHBoxLayout;
    auto* newPair = new QPushButton(QStringLiteral("新增配对"), pairs); m_newPair = newPair;
    auto* updatePair = new QPushButton(QStringLiteral("应用到列表"), pairs); m_storePair = updatePair;
    auto* removePair = new QPushButton(QStringLiteral("移除配对"), pairs); m_removePair = removePair;
    newPair->setObjectName(QStringLiteral("newParameterPair")); updatePair->setObjectName(QStringLiteral("storeParameterPair")); removePair->setObjectName(QStringLiteral("removeParameterPair"));
    pairButtons->addWidget(newPair); pairButtons->addWidget(updatePair); pairButtons->addWidget(removePair); pairsLayout->addLayout(pairButtons);
    connect(newPair, &QPushButton::clicked, this, &CParameterOrganizationDialog::StartNewPair);
    connect(updatePair, &QPushButton::clicked, this, &CParameterOrganizationDialog::StorePair);
    connect(removePair, &QPushButton::clicked, this, &CParameterOrganizationDialog::RemovePair);
    for (const auto& pair : m_result.pairs) {
        const int row = m_pairs->rowCount(); m_pairs->insertRow(row);
        for (int column = 0; column < 4; ++column) m_pairs->setItem(row, column, new QTableWidgetItem);
        m_pairs->item(row, 0)->setText(pair.first); m_pairs->item(row, 1)->setText(pair.second);
        m_pairs->item(row, 0)->setToolTip(m_names.value(pair.first, pair.first) + QStringLiteral(" · ") + pair.first);
        m_pairs->item(row, 1)->setToolTip(m_names.value(pair.second, pair.second) + QStringLiteral(" · ") + pair.second);
        m_pairs->item(row, 2)->setText(pair.reversed ? QStringLiteral("反向") : QStringLiteral("同向"));
        m_pairs->item(row, 2)->setData(Qt::UserRole, pair.reversed);
        m_pairs->item(row, 3)->setCheckState(pair.enabled ? Qt::Checked : Qt::Unchecked);
        for (int column = 0; column < 4; ++column) m_pairs->item(row, column)->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    }
    auto* draftRow = new QHBoxLayout;
    m_pairStatus = new QLabel(pairs); m_pairStatus->setObjectName(QStringLiteral("parameterPairDraftStatus"));
    m_pairStatus->setWordWrap(true); m_pairStatus->setTextFormat(Qt::PlainText);
    m_pairStatus->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_discardPair = new QPushButton(QStringLiteral("放弃本次修改"), pairs);
    m_discardPair->setObjectName(QStringLiteral("discardParameterPairDraft"));
    draftRow->addWidget(m_pairStatus, 1); draftRow->addWidget(m_discardPair); pairsLayout->addLayout(draftRow);
    connect(m_discardPair, &QPushButton::clicked, this, [this] { LoadPair(m_pairRow); m_error->clear(); });
    for (auto* combo : {m_first, m_second}) {
        connect(combo, &QComboBox::currentTextChanged, this, &CParameterOrganizationDialog::RefreshPairDraft);
        connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &CParameterOrganizationDialog::RefreshPairDraft);
    }
    connect(m_reversed, &QCheckBox::toggled, this, &CParameterOrganizationDialog::RefreshPairDraft);
    connect(m_enabled, &QCheckBox::toggled, this, &CParameterOrganizationDialog::RefreshPairDraft);
    connect(m_pairs, &QTableWidget::currentCellChanged, this, [this] { SelectPair(); });
    tabs->addTab(pairs, QStringLiteral("参数配对"));
    m_error = new QLabel(this); m_error->setObjectName(QStringLiteral("parameterOrganizationError"));
    m_error->setTextFormat(Qt::PlainText); m_error->setWordWrap(true); m_error->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred); layout->addWidget(m_error);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this); m_buttons = buttons;
    buttons->setObjectName(QStringLiteral("parameterOrganizationButtons"));
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("应用"));
    buttons->button(QDialogButtonBox::Ok)->setObjectName(QStringLiteral("primaryBtn"));
    buttons->button(QDialogButtonBox::Ok)->style()->unpolish(buttons->button(QDialogButtonBox::Ok));
    buttons->button(QDialogButtonBox::Ok)->style()->polish(buttons->button(QDialogButtonBox::Ok));
    connect(buttons, &QDialogButtonBox::accepted, this, &CParameterOrganizationDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject); layout->addWidget(buttons);
    RefreshFolders();
    LoadPair(m_pairs->rowCount() > 0 ? 0 : -1);
}

void CParameterOrganizationDialog::SetNodeDecoration(QTreeWidgetItem* item)
{
    item->setToolTip(0, item->text(0)); item->setToolTip(1, item->data(0, idRole).toString());
    const QColor color(item->data(0, colorRole).toString());
    if (color.isValid()) {
        QPixmap swatch(fontMetrics().height(), fontMetrics().height()); swatch.fill(color);
        m_tree->ensurePolished();
        QPainter painter(&swatch); painter.setPen(m_tree->palette().color(QPalette::Text)); painter.drawRect(swatch.rect().adjusted(0, 0, -1, -1));
        painter.end(); item->setIcon(0, QIcon(swatch));
    }
    else item->setIcon(0, item->data(0, kindRole).toInt() == Organization::Node::Folder ? style()->standardIcon(QStyle::SP_DirIcon) : QIcon());
}

void CParameterOrganizationDialog::RefreshFolders()
{
    const auto selected = m_destination->currentData(); const QSignalBlocker blocker(m_destination);
    m_destination->clear(); m_destination->addItem(QStringLiteral("根级（不放入文件夹）"), QString());
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        auto* item = m_tree->topLevelItem(i);
        if (item->data(0, kindRole).toInt() == Organization::Node::Folder) m_destination->addItem(item->text(0), item->data(0, idRole));
    }
    const int index = m_destination->findData(selected); m_destination->setCurrentIndex(qMax(0, index));
}

void CParameterOrganizationDialog::AddFolder()
{
    if (m_submitting) return;
    const QPointer<CParameterOrganizationDialog> alive(this);
    QPointer<QInputDialog> dialog = new QInputDialog(this);
    dialog->setObjectName(QStringLiteral("parameterFolderNameDialog"));
    dialog->setWindowTitle(QStringLiteral("新建参数文件夹")); dialog->setLabelText(QStringLiteral("名称"));
    dialog->setTextValue(QStringLiteral("新建文件夹"));
    const auto cleanup = qScopeGuard([dialog] { if (dialog) delete dialog.data(); });
    if (dialog->exec() != QDialog::Accepted || !alive || !dialog) return;
    const auto name = dialog->textValue().trimmed();
    if (name.isEmpty()) { m_error->setText(QStringLiteral("请输入文件夹名称。")); return; }
    auto* folder = new QTreeWidgetItem({name, QString()});
    const QString id = QStringLiteral("folder_") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    folder->setText(1, id); folder->setData(0, kindRole, Organization::Node::Folder); folder->setData(0, idRole, id);
    folder->setFlags(folder->flags() | Qt::ItemIsEditable | Qt::ItemIsDropEnabled);
    const auto selected = selectedInTreeOrder(m_tree);
    m_tree->addTopLevelItem(folder); SetNodeDecoration(folder);
    for (auto* item : selected) if (item->data(0, kindRole).toInt() == Organization::Node::Parameter) {
        if (auto* parent = item->parent()) parent->takeChild(parent->indexOfChild(item));
        else m_tree->takeTopLevelItem(m_tree->indexOfTopLevelItem(item));
        folder->addChild(item);
    }
    m_tree->setCurrentItem(folder); folder->setExpanded(true); RefreshFolders();
}

void CParameterOrganizationDialog::DissolveFolders()
{
    if (m_submitting) return;
    for (auto* folder : m_tree->selectedItems()) if (folder->data(0, kindRole).toInt() == Organization::Node::Folder) {
        const int index = m_tree->indexOfTopLevelItem(folder);
        auto children = folder->takeChildren();
        m_tree->takeTopLevelItem(index); delete folder;
        for (int child = 0; child < children.size(); ++child) m_tree->insertTopLevelItem(index + child, children[child]);
    }
    RefreshFolders();
}

void CParameterOrganizationDialog::MoveSelected(int direction)
{
    if (m_submitting) return;
    QSet<QTreeWidgetItem*> selected;
    for (auto* item : m_tree->selectedItems()) selected.insert(item);
    QVector<QTreeWidgetItem*> parents{nullptr};
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) parents.append(m_tree->topLevelItem(i));
    for (auto* parent : parents) {
        const int count = parent ? parent->childCount() : m_tree->topLevelItemCount();
        for (int step = 0; step < count; ++step) {
            const int index = direction < 0 ? step : count - 1 - step, adjacent = index + direction;
            if (adjacent < 0 || adjacent >= count) continue;
            auto* item = parent ? parent->child(index) : m_tree->topLevelItem(index);
            auto* neighbor = parent ? parent->child(adjacent) : m_tree->topLevelItem(adjacent);
            if (!selected.contains(item) || selected.contains(neighbor)) continue;
            if (parent) { parent->takeChild(index); parent->insertChild(adjacent, item); }
            else { m_tree->takeTopLevelItem(index); m_tree->insertTopLevelItem(adjacent, item); }
        }
    }
    for (auto* item : selected) item->setSelected(true);
}

void CParameterOrganizationDialog::MoveParameters()
{
    if (m_submitting) return;
    const auto id = m_destination->currentData().toString(); QTreeWidgetItem* folder = nullptr;
    if (!id.isEmpty()) for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        auto* item = m_tree->topLevelItem(i);
        if (item->data(0, kindRole).toInt() == Organization::Node::Folder && item->data(0, idRole).toString() == id) folder = item;
    }
    for (auto* item : selectedInTreeOrder(m_tree)) if (item->data(0, kindRole).toInt() == Organization::Node::Parameter && item->parent() != folder) {
        if (auto* parent = item->parent()) parent->takeChild(parent->indexOfChild(item));
        else m_tree->takeTopLevelItem(m_tree->indexOfTopLevelItem(item));
        if (folder) folder->addChild(item); else m_tree->addTopLevelItem(item);
    }
    if (folder) folder->setExpanded(true);
}

void CParameterOrganizationDialog::SetLabelColor(bool reset)
{
    if (m_submitting) return;
    struct Selected { int kind; QString id; };
    QVector<Selected> selected;
    for (const auto* item : m_tree->selectedItems()) selected.append({item->data(0, kindRole).toInt(), item->data(0, idRole).toString()});
    if (selected.isEmpty()) return;
    const QPointer<CParameterOrganizationDialog> alive(this);
    QColor color;
    if (!reset) {
        QPointer<QColorDialog> dialog = new QColorDialog(this);
        dialog->setObjectName(QStringLiteral("parameterLabelColorDialog")); dialog->setWindowTitle(QStringLiteral("参数标签颜色"));
        const QColor current(m_tree->selectedItems().front()->data(0, colorRole).toString());
        if (current.isValid()) dialog->setCurrentColor(current);
        const auto cleanup = qScopeGuard([dialog] { if (dialog) delete dialog.data(); });
        if (dialog->exec() != QDialog::Accepted || !alive || !dialog) return;
        color = dialog->selectedColor();
        if (!color.isValid()) return;
    }
    QVector<QTreeWidgetItem*> items;
    for (int root = 0; root < m_tree->topLevelItemCount(); ++root) {
        auto* item = m_tree->topLevelItem(root); items.append(item);
        for (int child = 0; child < item->childCount(); ++child) items.append(item->child(child));
    }
    for (const auto& key : selected) for (auto* item : items)
        if (item->data(0, kindRole).toInt() == key.kind && item->data(0, idRole).toString() == key.id) {
            item->setData(0, colorRole, reset ? QString() : color.name(QColor::HexRgb).toUpper()); SetNodeDecoration(item);
        }

}

bool CParameterOrganizationDialog::ReadState(Organization::State& state, QString* error) const
{
    Organization::State candidate; candidate.configured = true;
    const auto add = [&candidate](QTreeWidgetItem* item, const QString& parent) {
        const auto kind = static_cast<Organization::Node::Kind>(item->data(0, kindRole).toInt());
        candidate.nodes.append({kind, item->data(0, idRole).toString(), parent,
            kind == Organization::Node::Folder ? item->text(0).trimmed() : QString(), item->data(0, colorRole).toString()});
    };
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        auto* item = m_tree->topLevelItem(i); add(item, {});
        for (int child = 0; child < item->childCount(); ++child) {
            auto* parameter = item->child(child);
            if (parameter->childCount() || item->data(0, kindRole).toInt() != Organization::Node::Folder) {
                if (error) *error = QStringLiteral("参数文件夹只支持一层，请将嵌套项目移至根级"); return false;
            }
            add(parameter, item->data(0, idRole).toString());
        }
    }
    for (int row = 0; row < m_pairs->rowCount(); ++row)
        candidate.pairs.append({m_pairs->item(row, 0)->text(), m_pairs->item(row, 1)->text(),
                               m_pairs->item(row, 2)->data(Qt::UserRole).toBool(), m_pairs->item(row, 3)->checkState() == Qt::Checked});
    if (!Organization::Normalize(candidate, state, error)) return false;
    return Organization::Validate(state, m_parameterIds, error);
}

bool CParameterOrganizationDialog::PairDraftChanged() const
{
    return m_first->currentText() != m_pairFirstText || m_second->currentText() != m_pairSecondText ||
        m_reversed->isChecked() != m_pairReversed || m_enabled->isChecked() != m_pairEnabled;
}

void CParameterOrganizationDialog::RefreshPairDraft()
{
    if (m_loadingPair || !m_pairStatus || !m_discardPair) return;
    m_first->setToolTip(m_first->currentText()); m_second->setToolTip(m_second->currentText());
    const bool changed = PairDraftChanged();
    m_pairStatus->setText(changed ? QStringLiteral("配对有未保存的修改；应用时会一并保存。")
                                : m_pairRow < 0 ? QStringLiteral("选择两个参数后保存配对。") : QString());
    m_discardPair->setEnabled(changed && !m_submitting);
    m_removePair->setEnabled(m_pairRow >= 0 && !m_submitting);
    m_storePair->setEnabled(m_parameterIds.size() >= 2 && !m_submitting);
    m_newPair->setEnabled(m_parameterIds.size() - m_pairs->rowCount() * 2 >= 2 && !m_submitting);
}

void CParameterOrganizationDialog::LoadPair(int row)
{
    if (row < 0 || row >= m_pairs->rowCount()) row = -1;
    m_loadingPair = true;
    const QSignalBlocker first(m_first), second(m_second), reversed(m_reversed), enabled(m_enabled), table(m_pairs);
    m_pairRow = row;
    if (row >= 0) {
        m_first->setCurrentIndex(m_first->findData(m_pairs->item(row, 0)->text()));
        m_second->setCurrentIndex(m_second->findData(m_pairs->item(row, 1)->text()));
        m_reversed->setChecked(m_pairs->item(row, 2)->data(Qt::UserRole).toBool());
        m_enabled->setChecked(m_pairs->item(row, 3)->checkState() == Qt::Checked);
        m_pairs->setCurrentCell(row, 0); m_pairs->selectRow(row);
    } else {
        QSet<QString> used;
        for (int index = 0; index < m_pairs->rowCount(); ++index) {
            used.insert(m_pairs->item(index, 0)->text()); used.insert(m_pairs->item(index, 1)->text());
        }
        QVector<int> available;
        for (int index = 0; index < m_first->count(); ++index)
            if (!used.contains(m_first->itemData(index).toString())) available.append(index);
        m_first->setCurrentIndex(available.isEmpty() ? -1 : available[0]);
        m_second->setCurrentIndex(available.size() < 2 ? -1 : available[1]);
        if (available.isEmpty()) m_first->setEditText(QString());
        if (available.size() < 2) m_second->setEditText(QString());
        m_reversed->setChecked(false); m_enabled->setChecked(true);
        m_pairs->clearSelection(); m_pairs->setCurrentItem(nullptr);
    }
    m_pairFirstText = m_first->currentText(); m_pairSecondText = m_second->currentText();
    m_pairReversed = m_reversed->isChecked(); m_pairEnabled = m_enabled->isChecked();
    m_loadingPair = false;
    RefreshPairDraft();
}

bool CParameterOrganizationDialog::CommitPairDraft(QString* error, bool selectStored)
{
    const auto first = selectedParameter(m_first), second = selectedParameter(m_second);
    if (first.isEmpty() || second.isEmpty() || first == second) {
        if (error) *error = QStringLiteral("请选择两个不同的有效参数，或放弃本次配对修改。");
        return false;
    }
    for (int row = 0; row < m_pairs->rowCount(); ++row) if (row != m_pairRow) {
        if (m_pairs->item(row, 0)->text() == first || m_pairs->item(row, 1)->text() == first ||
            m_pairs->item(row, 0)->text() == second || m_pairs->item(row, 1)->text() == second) {
            if (error) *error = QStringLiteral("参数已属于另一组配对；请修正选择或放弃本次修改。");
            return false;
        }
    }
    const int row = m_pairRow >= 0 ? m_pairRow : m_pairs->rowCount();
    const QSignalBlocker blocked(m_pairs);
    if (m_pairRow < 0) {
        m_pairs->insertRow(row);
        for (int column = 0; column < 4; ++column) {
            auto* item = new QTableWidgetItem;
            item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
            m_pairs->setItem(row, column, item);
        }
    }
    m_pairs->item(row, 0)->setText(first); m_pairs->item(row, 1)->setText(second);
    m_pairs->item(row, 0)->setToolTip(m_names.value(first, first) + QStringLiteral(" · ") + first);
    m_pairs->item(row, 1)->setToolTip(m_names.value(second, second) + QStringLiteral(" · ") + second);
    m_pairs->item(row, 2)->setText(m_reversed->isChecked() ? QStringLiteral("反向") : QStringLiteral("同向"));
    m_pairs->item(row, 2)->setData(Qt::UserRole, m_reversed->isChecked());
    m_pairs->item(row, 3)->setCheckState(m_enabled->isChecked() ? Qt::Checked : Qt::Unchecked);
    m_pairRow = row;
    m_pairFirstText = m_first->currentText(); m_pairSecondText = m_second->currentText();
    m_pairReversed = m_reversed->isChecked(); m_pairEnabled = m_enabled->isChecked();
    if (selectStored) { m_pairs->setCurrentCell(row, 0); m_pairs->selectRow(row); }
    RefreshPairDraft();
    return true;
}

void CParameterOrganizationDialog::SelectPair()
{
    if (m_loadingPair || m_submitting) return;
    const int row = m_pairs->currentRow();
    if (row == m_pairRow) return;
    QString error;
    if (PairDraftChanged() && !CommitPairDraft(&error, false)) {
        const QSignalBlocker blocked(m_pairs);
        m_pairs->clearSelection();
        if (m_pairRow >= 0) { m_pairs->setCurrentCell(m_pairRow, 0); m_pairs->selectRow(m_pairRow); }
        else m_pairs->setCurrentItem(nullptr);
        m_error->setText(error);
        return;
    }
    LoadPair(row); m_error->clear();
}

void CParameterOrganizationDialog::StartNewPair()
{
    if (m_submitting) return;
    QString error;
    if (PairDraftChanged() && !CommitPairDraft(&error, false)) { m_error->setText(error); return; }
    LoadPair(-1); m_error->clear();
}

void CParameterOrganizationDialog::RemovePair()
{
    if (m_submitting || m_pairRow < 0) return;
    const int row = m_pairRow;
    { const QSignalBlocker blocked(m_pairs); m_pairs->removeRow(row); }
    LoadPair(m_pairs->rowCount() ? qMin(row, m_pairs->rowCount() - 1) : -1);
    m_error->clear();
}

void CParameterOrganizationDialog::StorePair()
{
    if (m_submitting) return;
    QString error;
    if (!CommitPairDraft(&error, true)) m_error->setText(error);
    else m_error->clear();
}

void CParameterOrganizationDialog::SetSubmissionHandler(SubmissionHandler handler)
{
    if (!m_submitting) m_submission = std::move(handler);
}

void CParameterOrganizationDialog::accept()
{
    if (m_submitting) return;
    const QPointer<CParameterOrganizationDialog> alive(this);
    m_submitting = true;
    const bool contentEnabled = !m_content->testAttribute(Qt::WA_ForceDisabled);
    const bool buttonsEnabled = !m_buttons->testAttribute(Qt::WA_ForceDisabled);
    const auto submitting = qScopeGuard([alive, contentEnabled, buttonsEnabled] {
        if (!alive) return;
        alive->m_content->setEnabled(contentEnabled);
        if (!alive) return;
        alive->m_buttons->setEnabled(buttonsEnabled);
        if (!alive) return;
        alive->m_submitting = false; alive->RefreshPairDraft();
    });
    m_treeDelegate->CommitEditor();
    if (!alive) return;
    QString error;
    if (PairDraftChanged() && !CommitPairDraft(&error, true)) { m_error->setText(error); return; }
    Organization::State candidate;
    if (!ReadState(candidate, &error)) { m_error->setText(error); return; }
    const auto handler = m_submission;
    m_content->setEnabled(false);
    if (!alive) return;
    m_buttons->setEnabled(false);
    if (!alive) return;
    bool accepted = true;
    try { if (handler) accepted = handler(candidate, &error); }
    catch (...) { accepted = false; error = QStringLiteral("参数组织未能保存，修改已保留，请重试。"); }
    if (!alive) return;
    if (!accepted) {
        m_error->setText(error.isEmpty() ? QStringLiteral("参数组织未能保存，修改已保留，请重试。") : error);
        return;
    }
    m_result = std::move(candidate);
    QDialog::accept();
}

void CParameterOrganizationDialog::reject()
{
    if (!m_submitting) QDialog::reject();
}
