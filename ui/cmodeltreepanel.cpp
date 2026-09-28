#include "cmodeltreepanel.h"
#include <QScopeGuard>
#include <QLabel>
#include "cmodelhierarchymodel.h"
#include "cmodelhierarchyview.h"
#include "ctreenodedelegate.h"
#include "live2dstruct/csectiontable.h"
#include <QFile>
#include <QItemSelectionModel>
#include <QLineEdit>
#include <QMenu>
#include <QPersistentModelIndex>
#include <QPointer>
#include <QPushButton>
#include <QHBoxLayout>
#include <QSet>
#include <QShortcut>
#include <QSignalBlocker>
#include <QTabWidget>
#include <QVBoxLayout>

using namespace live2dstruct;

namespace {
QStringList selectionKeys(CModelHierarchyView* view)
{
    QStringList result;
    for (const auto& index : view->selectionModel()->selectedRows()) result.append(CModelHierarchyModel::Key(index));
    return result;
}

void restoreSelection(CModelHierarchyView* view, CModelHierarchyModel* model, const QStringList& keys)
{
    QItemSelection selection;
    for (const auto& key : keys) {
        const auto index = model->IndexForKey(key);
        if (index.isValid()) selection.select(index, index);
    }
    view->selectionModel()->select(selection, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
}
}

CModelTreePanel::CModelTreePanel(QWidget* parent) : QDockWidget(QStringLiteral("模型结构"), parent)
{
    setObjectName(QStringLiteral("modelTreePanel"));
    m_tabWidget = new QTabWidget(this);
    m_partModel = new CModelHierarchyModel(CModelHierarchyModel::Projection::Parts, this);
    m_deformerModel = new CModelHierarchyModel(CModelHierarchyModel::Projection::Deformers, this);
    m_partTree = new CModelHierarchyView(m_tabWidget);
    m_deformerTree = new CModelHierarchyView(m_tabWidget);
    m_partTree->setObjectName(QStringLiteral("partHierarchy"));
    m_deformerTree->setObjectName(QStringLiteral("deformerHierarchy"));
    m_partTree->setModel(m_partModel);
    m_deformerTree->setModel(m_deformerModel);
    for (auto* view : {m_partTree, m_deformerTree}) {
        view->setSelectionMode(QAbstractItemView::ExtendedSelection);
        view->setItemDelegate(new CTreeNodeDelegate(view));
    }
    m_tabWidget->addTab(m_partTree, QStringLiteral("部件"));
    m_tabWidget->addTab(m_deformerTree, QStringLiteral("变形器"));
    auto* wrapper = new QWidget(this);
    auto* layout = new QVBoxLayout(wrapper);
    m_search = new QLineEdit(wrapper);
    m_search->setPlaceholderText(QStringLiteral("搜索名称或 ID"));
    m_search->setClearButtonEnabled(true);
    layout->addWidget(m_search);
    auto* actions = new QHBoxLayout;
    m_createDeformer = new QPushButton(QStringLiteral("新建变形器"), wrapper);
    m_removeDeformer = new QPushButton(QStringLiteral("删除"), wrapper);
    m_createDeformer->setObjectName(QStringLiteral("createDeformerButton"));
    m_removeDeformer->setObjectName(QStringLiteral("removeDeformerButton"));
    m_createDeformer->setToolTip(QStringLiteral("为选中对象创建弯曲或旋转变形器"));
    m_removeDeformer->setToolTip(QStringLiteral("删除选中的变形器，并保留子对象形状"));
    actions->addWidget(m_createDeformer, 1);
    actions->addWidget(m_removeDeformer);
    layout->addLayout(actions);
    SetDeformerActionsEnabled(false, false);
    connect(m_createDeformer, &QPushButton::clicked, this, &CModelTreePanel::CreateDeformerRequested);
    connect(m_removeDeformer, &QPushButton::clicked, this, &CModelTreePanel::RemoveDeformerRequested);
    auto* pairActions = new QHBoxLayout;
    m_geometryPairs = new QPushButton(QStringLiteral("对象配对…"), wrapper);
    m_mirrorGeometryPair = new QPushButton(QStringLiteral("镜像当前形"), wrapper);
    m_geometryPairs->setObjectName(QStringLiteral("editGeometryPairs"));
    m_mirrorGeometryPair->setObjectName(QStringLiteral("mirrorGeometryPair"));
    m_geometryPairs->setToolTip(QStringLiteral("设置两个对象的顶点、参数关键值对应和同步编辑"));
    m_mirrorGeometryPair->setToolTip(QStringLiteral("将所选对象的当前关键形镜像复制到已确认的配对对象"));
    pairActions->addWidget(m_geometryPairs);
    pairActions->addWidget(m_mirrorGeometryPair);
    layout->addLayout(pairActions);
    m_geometryPairHint = new QLabel(wrapper);
    m_geometryPairHint->setObjectName(QStringLiteral("geometryPairHint"));
    m_geometryPairHint->setTextFormat(Qt::PlainText);
    m_geometryPairHint->setWordWrap(true);
    layout->addWidget(m_geometryPairHint);
    SetGeometryPairActionsEnabled(false, false);
    connect(m_geometryPairs, &QPushButton::clicked, this, &CModelTreePanel::EditGeometryPairsRequested);
    connect(m_mirrorGeometryPair, &QPushButton::clicked, this, &CModelTreePanel::MirrorGeometryPairRequested);
    layout->addWidget(m_tabWidget, 1);
    setWidget(wrapper);
    connect(m_search, &QLineEdit::textChanged, this, [this](const QString& text) {
        m_partTree->setFilterText(text);
        m_deformerTree->setFilterText(text);
    });
    InstallInteractions(m_partTree, m_partModel, false);
    InstallInteractions(m_deformerTree, m_deformerModel, true);
}

void CModelTreePanel::InstallInteractions(CModelHierarchyView* view, CModelHierarchyModel* model, bool deformerTab)
{
    connect(view->selectionModel(), &QItemSelectionModel::selectionChanged, this,
            [this, view] { EmitSelection(view); });
    connect(view, &QAbstractItemView::clicked, this, [this, view, model, deformerTab](const QModelIndex& index) {
        if (!index.isValid()) return;
        const QPersistentModelIndex clicked(index);
        const auto selected = view->selectionModel()->selectedRows();
        const bool clickedSelected = selected.contains(index);
        if (!clickedSelected && view->currentIndex() != index) return;
        if (selected.size() != 1 || !clickedSelected) {
            QSet<QString> selectedKeys, included;
            for (const auto& item : selected) selectedKeys.insert(CModelHierarchyModel::Key(item));
            QStringList meshes;
            for (const auto& item : m_partModel->Indices()) {
                const auto key = CModelHierarchyModel::Key(item);
                if (!selectedKeys.contains(key) && !included.contains(CModelHierarchyModel::Key(item.parent()))) continue;
                included.insert(key);
                if (item.data(Qt::UserRole + 1) == QLatin1String("artmesh")) meshes.append(item.data(Qt::UserRole).toString());
            }
            emit ShowDeformerMeshes(meshes);
            if (deformerTab) emit DeformerDragParamsChanged({}, {}, {}, {});
            return;
        }
        const QString id = index.data(Qt::UserRole).toString();
        const QString type = index.data(Qt::UserRole + 1).toString();
        emit ObjectSelected(id, type);
        if (!clicked.isValid() || view->selectionModel()->selectedRows() != QModelIndexList{clicked}) return;
        if (type == QLatin1String("artmesh")) emit MeshSelected(id);
        else emit ShowDeformerMeshes(model->MeshesForSelection(clicked));
        if (deformerTab && clicked.isValid() && view->selectionModel()->selectedRows() == QModelIndexList{clicked}) {
            const auto parameters = model->ParametersForSelection(clicked);
            emit DeformerDragParamsChanged(parameters.ids, parameters.minimums, parameters.maximums, parameters.defaults);
        }
    });
    connect(model, &CModelHierarchyModel::CheckRequested, this,
            [this, model](const QModelIndex& index, bool visible) {
        QMap<QString, bool> changes;
        const auto locked = model->LockedState();
        for (const auto& key : model->SubtreeKeys(index))
            if (!locked.value(key, false)) changes.insert(key, visible);
        PublishVisibility(changes);
        const auto type = index.data(Qt::UserRole + 1).toString();
        if (!visible && (type == QLatin1String("warp") || type == QLatin1String("rotation"))) emit ShowDeformers({});
    });
    auto* rename = new QShortcut(QKeySequence(Qt::Key_F2), view);
    rename->setContext(Qt::WidgetWithChildrenShortcut);
    connect(rename, &QShortcut::activated, this, [this, view] { RequestRename(view); });
    view->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(view, &QWidget::customContextMenuRequested, this,
            [this, view](const QPoint& position) { ShowContextMenu(view, position); });
}

void CModelTreePanel::BuildTree(const QString& mocPath, CCidJson* cid)
{
    QFile file(mocPath);
    if (file.open(QIODevice::ReadOnly)) BuildTreeBytes(file.readAll(), cid);
}

bool CModelTreePanel::BuildTreeBytes(const QByteArray& bytes, CCidJson* cid, QString* error)
{
    auto reader = CReader::FromBytes(bytes);
    CSectionTable section(reader);
    if (!section.IsValid()) {
        if (error) *error = reader.Error();
        return false;
    }
    CModelHierarchyModel::Snapshot candidate;
    if (!CModelHierarchyModel::Prepare(section, cid, candidate, error)) return false;
    const auto partExpanded = m_partTree->expandedKeys(), deformerExpanded = m_deformerTree->expandedKeys();
    const auto partSelection = selectionKeys(m_partTree), deformerSelection = selectionKeys(m_deformerTree);
    const auto partCurrent = CModelHierarchyModel::Key(m_partTree->currentIndex());
    const auto deformerCurrent = CModelHierarchyModel::Key(m_deformerTree->currentIndex());
    const bool first = m_partModel->rowCount() == 0 && m_deformerModel->rowCount() == 0;
    const QSignalBlocker blocker(this);
    const QSignalBlocker partBlocker(m_partTree->selectionModel());
    const QSignalBlocker deformerBlocker(m_deformerTree->selectionModel());
    m_partModel->SetSnapshot(candidate);
    m_deformerModel->SetSnapshot(candidate);
    if (first) { m_partTree->expandToDepth(1); m_deformerTree->expandToDepth(1); }
    else { m_partTree->restoreExpandedKeys(partExpanded); m_deformerTree->restoreExpandedKeys(deformerExpanded); }
    restoreSelection(m_partTree, m_partModel, partSelection);
    restoreSelection(m_deformerTree, m_deformerModel, deformerSelection);
    m_partTree->selectionModel()->setCurrentIndex(m_partModel->IndexForKey(partCurrent), QItemSelectionModel::NoUpdate);
    m_deformerTree->selectionModel()->setCurrentIndex(m_deformerModel->IndexForKey(deformerCurrent), QItemSelectionModel::NoUpdate);
    m_partTree->setFilterText(m_search->text());
    m_deformerTree->setFilterText(m_search->text());
    if (error) error->clear();
    return true;
}

void CModelTreePanel::SetDeformerActionsEnabled(bool create, bool remove)
{
    m_createDeformer->setEnabled(create);
    m_removeDeformer->setEnabled(remove);
}

void CModelTreePanel::SetGeometryPairActionsEnabled(bool edit, bool mirror, const QString& detail)
{
    m_geometryPairs->setEnabled(edit);
    m_mirrorGeometryPair->setEnabled(mirror);
    m_geometryPairHint->setText(detail);
    m_geometryPairHint->setVisible(!detail.isEmpty());
}

void CModelTreePanel::HighlightObjects(const QStringList& ids, const QStringList& types)
{
    QStringList keys;
    for (int i = 0; i < ids.size() && i < types.size(); ++i)
        keys.append(types[i] + QLatin1Char('|') + ids[i]);
    const QSignalBlocker partBlocker(m_partTree->selectionModel());
    const QSignalBlocker deformerBlocker(m_deformerTree->selectionModel());
    restoreSelection(m_partTree, m_partModel, keys);
    restoreSelection(m_deformerTree, m_deformerModel, keys);
    for (const auto& entry : {qMakePair(m_partTree, m_partModel), qMakePair(m_deformerTree, m_deformerModel)}) {
        const auto index = keys.isEmpty() ? QModelIndex() : entry.second->IndexForKey(keys.first());
        entry.first->selectionModel()->setCurrentIndex(index, QItemSelectionModel::NoUpdate);
        if (index.isValid()) entry.first->scrollTo(index);
    }
}

QMap<QString, bool> CModelTreePanel::VisibilityState() const { return m_partModel->VisibilityState(); }
QMap<QString, bool> CModelTreePanel::LockedState() const { return m_partModel->LockedState(); }

void CModelTreePanel::PublishVisibility(const QMap<QString, bool>& requested)
{
    const auto changes = m_partModel->UpdateVisibility(requested);
    if (changes.isEmpty()) return;
    const auto keys = changes.keys();
    m_partModel->NotifyObjects(keys);
    m_deformerModel->NotifyObjects(keys);
    emit VisibilityBatchChanged(changes);
    for (auto change = changes.cbegin(); change != changes.cend(); ++change) {
        const auto index = m_partModel->IndexForKey(change.key());
        if (!index.isValid() || (index.data(Qt::CheckStateRole).toInt() == Qt::Checked) != change.value()) continue;
        const int separator = change.key().indexOf(QLatin1Char('|'));
        emit VisibilityChanged(change.key().mid(separator + 1), change.key().left(separator), change.value());
    }
}

void CModelTreePanel::ApplyVisibilityState(const QMap<QString, bool>& state) { PublishVisibility(state); }

void CModelTreePanel::ApplyLockedState(const QMap<QString, bool>& state)
{
    const auto changed = m_partModel->UpdateLocked(state);
    m_partModel->NotifyObjects(changed);
    m_deformerModel->NotifyObjects(changed);
}

void CModelTreePanel::PublishLocks(const QMap<QString, bool>& requested)
{
    auto state = LockedState();
    QMap<QString, bool> changed;
    for (auto value = requested.cbegin(); value != requested.cend(); ++value) {
        if (!m_partModel->IndexForKey(value.key()).isValid() || state.value(value.key(), false) == value.value()) continue;
        changed.insert(value.key(), value.value());
        state.insert(value.key(), value.value());
    }
    if (changed.isEmpty()) return;
    ApplyLockedState(state);
    emit LocksBatchChanged(changed);
    for (auto value = changed.cbegin(); value != changed.cend(); ++value) {
        const auto index = m_partModel->IndexForKey(value.key());
        if (!index.isValid() || index.data(Qt::UserRole + 2).toBool() != value.value()) continue;
        emit ObjectLockChanged(index.data(Qt::UserRole).toString(), index.data(Qt::UserRole + 1).toString(), value.value());
    }
}

void CModelTreePanel::SetVisibility(const QString& name, const QString& type, bool visible)
{
    if (m_partModel->IndexForObject(name, type).isValid()) PublishVisibility({{type + QLatin1Char('|') + name, visible}});
}

void CModelTreePanel::EmitSelection(CModelHierarchyView* view)
{
    QStringList ids, types;
    for (const auto& index : view->selectionModel()->selectedRows()) {
        ids.append(index.data(Qt::UserRole).toString());
        types.append(index.data(Qt::UserRole + 1).toString());
    }
    emit ObjectsSelected(ids, types);
}

void CModelTreePanel::SelectObjects(const QModelIndex& part, const QModelIndex& deformer,
                                    const QString& id, const QString& type)
{
    const QModelIndexList partExpected = part.isValid() ? QModelIndexList{part} : QModelIndexList{};
    const QModelIndexList deformerExpected = deformer.isValid() ? QModelIndexList{deformer} : QModelIndexList{};
    if (m_partTree->selectionModel()->selectedRows() == partExpected &&
        m_deformerTree->selectionModel()->selectedRows() == deformerExpected) return;
    {
        const QSignalBlocker partBlocker(m_partTree->selectionModel());
        const QSignalBlocker deformerBlocker(m_deformerTree->selectionModel());
        for (const auto& entry : {qMakePair(m_partTree, part), qMakePair(m_deformerTree, deformer)}) {
            auto* selection = entry.first->selectionModel();
            selection->clearSelection();
            selection->setCurrentIndex(entry.second, QItemSelectionModel::NoUpdate);
            if (entry.second.isValid()) {
                selection->select(entry.second, QItemSelectionModel::Select | QItemSelectionModel::Rows);
                entry.first->scrollTo(entry.second);
            }
        }
    }
    if (id.isEmpty()) emit ObjectsSelected({}, {});
    else emit ObjectsSelected({id}, {type});
}

void CModelTreePanel::HighlightMesh(const QString& artName)
{
    if (artName.isEmpty()) { SelectObjects({}, {}, {}, {}); return; }
    const auto mesh = m_partModel->IndexForObject(artName, QStringLiteral("artmesh"));
    if (!mesh.isValid()) return;
    const auto linked = m_deformerModel->IndexForKey(CModelHierarchyModel::Key(mesh.parent()));
    SelectObjects(mesh, linked, artName, QStringLiteral("artmesh"));
}

void CModelTreePanel::HighlightObject(const QString& id, const QString& type)
{
    if (type == QLatin1String("artmesh")) { HighlightMesh(id); return; }
    const auto part = m_partModel->IndexForObject(id, type), deformer = m_deformerModel->IndexForObject(id, type);
    if (part.isValid() || deformer.isValid()) SelectObjects(part, deformer, id, type);
}

void CModelTreePanel::ShowContextMenu(CModelHierarchyView* view, const QPoint& position)
{
    const auto index = view->indexAt(position);
    if (!index.isValid()) return;
    if (!view->selectionModel()->isSelected(index))
        view->selectionModel()->select(index, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    const auto selected = selectionKeys(view);
    const QPersistentModelIndex anchor(index);
    const QPointer<CModelTreePanel> guard(this);
    auto locks = LockedState();
    bool allLocked = !selected.isEmpty();
    for (const auto& key : selected) allLocked = allLocked && locks.value(key, false);
    QMenu menu;
    menu.setPalette(view->palette());
    menu.setFont(view->font());
    connect(view, &QObject::destroyed, &menu, &QMenu::close);
    auto* lock = menu.addAction(allLocked ? QStringLiteral("解锁选中对象") : QStringLiteral("锁定选中对象"));
    auto* solo = menu.addAction(QStringLiteral("独显选中对象"));
    auto* show = menu.addAction(QStringLiteral("显示全部对象"));
    menu.addSeparator();
    auto* rename = menu.addAction(QStringLiteral("重命名…"));
    rename->setEnabled(CanRename(view));
    auto* create = menu.addAction(QStringLiteral("新建变形器"));
    auto* remove = menu.addAction(QStringLiteral("删除变形器"));
    create->setEnabled(m_createDeformer->isEnabled());
    remove->setEnabled(m_removeDeformer->isEnabled());
    menu.addSeparator();
    auto* pairSettings = menu.addAction(QStringLiteral("对象配对…"));
    auto* mirror = menu.addAction(QStringLiteral("镜像当前形到对侧"));
    auto* counterpart = menu.addAction(QStringLiteral("查看对侧关键形"));
    pairSettings->setEnabled(m_geometryPairs->isEnabled());
    mirror->setEnabled(m_mirrorGeometryPair->isEnabled());
    counterpart->setEnabled(m_mirrorGeometryPair->isEnabled());
    auto* chosen = menu.exec(view->viewport()->mapToGlobal(position));
    if (!guard || !anchor.isValid()) return;
    if (chosen == pairSettings || chosen == mirror || chosen == counterpart) {
        if (selectionKeys(view) != selected) return;
        if (chosen == pairSettings) emit EditGeometryPairsRequested();
        else if (chosen == mirror) emit MirrorGeometryPairRequested();
        else emit ViewGeometryPairRequested();
        return;
    }
    if (chosen == rename) {
        if (selectionKeys(view) == selected) RequestRename(view);
        return;
    }
    if (chosen == create) { emit CreateDeformerRequested(); return; }
    if (chosen == remove) { emit RemoveDeformerRequested(); return; }
    locks = LockedState();
    if (chosen == lock) {
        QMap<QString, bool> requested;
        for (const auto& key : selected) requested.insert(key, !allLocked);
        PublishLocks(requested);
    } else if (chosen == solo || chosen == show) {
        auto states = chosen == solo ? m_partModel->SoloVisibility(selected) : VisibilityState();
        if (chosen == show)
            for (auto state = states.begin(); state != states.end(); ++state)
                if (!locks.value(state.key(), false)) state.value() = true;
        PublishVisibility(states);
    }
}

void CModelTreePanel::SetRenameDeformerEnabled(bool enabled) { m_renameEnabled = enabled; }

bool CModelTreePanel::CanRename(CModelHierarchyView* view) const
{
    if (!m_renameEnabled) return false;
    const auto selected = view->selectionModel()->selectedRows();
    if (selected.size() != 1) return false;
    const auto type = selected.first().data(Qt::UserRole + 1).toString();
    return (type == QLatin1String("warp") || type == QLatin1String("rotation") ||
        type == QLatin1String("part") || type == QLatin1String("artmesh")) &&
        !selected.first().data(Qt::UserRole + 2).toBool();
}

void CModelTreePanel::RequestRename(CModelHierarchyView* view)
{
    if (!CanRename(view)) return;
    const auto index = view->selectionModel()->selectedRows().first();
    const QString id = index.data(Qt::UserRole).toString(), type = index.data(Qt::UserRole + 1).toString();
    const QPointer<CModelTreePanel> alive(this);
    emit RenameObjectRequested(id, type);
    if (alive && (type == QLatin1String("warp") || type == QLatin1String("rotation"))) emit RenameDeformerRequested(id, type);
}

void CModelTreePanel::ApplyDeformerDisplayNames(const QMap<QString, QString>& names)
{
    const auto changed = m_partModel->UpdateDeformerDisplayNames(names);
    const QPointer<CModelTreePanel> panel(this);
    m_partModel->NotifyDisplayNames(changed);
    if (panel) m_deformerModel->NotifyDisplayNames(changed);
}

void CModelTreePanel::ApplyDisplayNames(const live2dstruct::CModelDisplayNames::State& names)
{
    const auto changed = m_partModel->UpdateDisplayNames(names);
    if (changed.isEmpty()) return;
    const QPointer<CModelTreePanel> panel(this);
    const QPointer<CModelHierarchyView> first(m_partTree), second(m_deformerTree);
    first->BeginDisplayNameUpdate(); second->BeginDisplayNameUpdate();
    const auto finish = qScopeGuard([first, second] {
        if (first) first->EndDisplayNameUpdate();
        if (second) second->EndDisplayNameUpdate();
    });
    m_partModel->NotifyDisplayNames(changed);
    if (panel) m_deformerModel->NotifyDisplayNames(changed);
}
