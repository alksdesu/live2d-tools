#include "cmodeltreepanel.h"
#include "ctreenodedelegate.h"
#include "ui/para/ccidjson.h"
#include "live2dstruct/util/cfilereader.h"
#include "live2dstruct/csectiontable.h"
#include <QVBoxLayout>
#include <QHeaderView>
#include <QDebug>
#include <QFile>
#include <QTextStream>
#include <QCoreApplication>
#include <QMenu>
#include <QFont>

using namespace live2dstruct;

static void initTree(QTreeWidget* tree, QObject* parent)
{
    tree->header()->setVisible(false);
    tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    tree->setIndentation(16);
    tree->setAnimated(true);
    tree->setRootIsDecorated(true);
    tree->setItemDelegate(new CTreeNodeDelegate(tree));
}

CModelTreePanel::CModelTreePanel(QWidget *parent)
    : QDockWidget("模型结构", parent)
{
    setObjectName(QStringLiteral("modelTreePanel"));
    m_tabWidget = new QTabWidget(this);

    m_partTree = new QTreeWidget(m_tabWidget);
    initTree(m_partTree, this);
    m_tabWidget->addTab(m_partTree, "部件");

    m_deformerTree = new QTreeWidget(m_tabWidget);
    initTree(m_deformerTree, this);
    m_tabWidget->addTab(m_deformerTree, "变形器");

    auto* wrapper = new QWidget(this);
    auto* layout = new QVBoxLayout(wrapper);
    m_search = new QLineEdit(wrapper);
    m_search->setPlaceholderText(QStringLiteral("搜索名称或 ID"));
    m_search->setClearButtonEnabled(true);
    layout->addWidget(m_search);
    layout->addWidget(m_tabWidget, 1);
    setWidget(wrapper);

    connect(m_search, &QLineEdit::textChanged, this, [this](const QString& text){
        const QString query = text.trimmed().toLower();
        const auto filter = [&query](QTreeWidgetItem* item, auto&& filterRef) -> bool {
            bool match = item->text(0).toLower().contains(query) ||
                         item->data(0, Qt::UserRole).toString().toLower().contains(query);
            for (int i = 0; i < item->childCount(); ++i) match = filterRef(item->child(i), filterRef) || match;
            item->setHidden(!query.isEmpty() && !match);
            if (match && !query.isEmpty()) item->setExpanded(true);
            return match;
        };
        for (QTreeWidget* tree : {m_partTree, m_deformerTree}) {
            for (int i = 0; i < tree->topLevelItemCount(); ++i) filter(tree->topLevelItem(i), filter);
        }
    });

    // 部件树点击
    connect(m_partTree, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* item, int){
        QString name = item->data(0, Qt::UserRole).toString();
        QString type = item->data(0, Qt::UserRole + 1).toString();
        emit ObjectSelected(name, type);
        if(type == "artmesh"){
            emit MeshSelected(name);
        } else if(type == "warp" || type == "rotation"){
            // 点击变形器 → 高亮该变形器下所有 ArtMesh
            QStringList artMeshes;
            if(m_deformerToArtMeshes.contains(name)){
                artMeshes = m_deformerToArtMeshes[name];
            }
            emit ShowDeformerMeshes(artMeshes);
        } else if(type == "part"){
            // 点击 Part → 高亮该 Part 下所有变形器的 ArtMesh
            QStringList allArtMeshes;
            QStringList deformers = collectDeformerNames(item);
            for(const QString& defName : deformers){
                if(m_deformerToArtMeshes.contains(defName)){
                    allArtMeshes << m_deformerToArtMeshes[defName];
                }
            }
            allArtMeshes.removeDuplicates();
            emit ShowDeformerMeshes(allArtMeshes);
        }
    });

    // 变形器树点击 → 高亮 ArtMesh + 设置拖拽参数
    connect(m_deformerTree, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* item, int){
        QString name = item->data(0, Qt::UserRole).toString();
        QString type = item->data(0, Qt::UserRole + 1).toString();
        emit ObjectSelected(name, type);
        if(type == "warp" || type == "rotation"){
            QStringList artMeshes;
            if(m_deformerToArtMeshes.contains(name))
                artMeshes = m_deformerToArtMeshes[name];
            emit ShowDeformerMeshes(artMeshes);
            // 发射拖拽参数
            if(m_deformerToParams.contains(name)){
                const auto& info = m_deformerToParams[name];
                emit DeformerDragParamsChanged(info.paramIds, info.minVals, info.maxVals, info.defaultVals);
            }
        } else if(type == "part"){
            QStringList allArtMeshes;
            QStringList deformers = collectDeformerNames(item);
            for(const QString& defName : deformers){
                if(m_deformerToArtMeshes.contains(defName))
                    allArtMeshes << m_deformerToArtMeshes[defName];
            }
            allArtMeshes.removeDuplicates();
            emit ShowDeformerMeshes(allArtMeshes);
            // Part 选中时清空拖拽参数
            emit DeformerDragParamsChanged(QStringList(), QList<float>(), QList<float>(), QList<float>());
        }
    });

    const auto emitSelection = [this](QTreeWidget* tree) {
        QStringList names;
        QStringList types;
        for (QTreeWidgetItem* item : tree->selectedItems()) {
            names.append(item->data(0, Qt::UserRole).toString());
            types.append(item->data(0, Qt::UserRole + 1).toString());
        }
        emit ObjectsSelected(names, types);
    };
    connect(m_partTree, &QTreeWidget::itemSelectionChanged, this, [this, emitSelection](){ emitSelection(m_partTree); });
    connect(m_deformerTree, &QTreeWidget::itemSelectionChanged, this, [this, emitSelection](){ emitSelection(m_deformerTree); });

    connect(m_partTree, &QTreeWidget::itemChanged, this, &CModelTreePanel::onPartItemChanged);
    connect(m_deformerTree, &QTreeWidget::itemChanged, this, &CModelTreePanel::onDeformerItemChanged);

    const auto installLockMenu = [this](QTreeWidget* tree) {
        tree->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(tree, &QTreeWidget::customContextMenuRequested, this, [this, tree](const QPoint& position) {
            QTreeWidgetItem* item = tree->itemAt(position);
            if (!item) return;
            if (!item->isSelected()) {
                tree->clearSelection();
                item->setSelected(true);
            }
            const QList<QTreeWidgetItem*> selected = tree->selectedItems();
            bool allLocked = !selected.isEmpty();
            for (QTreeWidgetItem* selectedItem : selected) allLocked = allLocked && selectedItem->data(0, Qt::UserRole + 2).toBool();
            QMenu menu(tree);
            QAction* lockAction = menu.addAction(allLocked ? QStringLiteral("解锁选中对象") : QStringLiteral("锁定选中对象"));
            QAction* soloAction = menu.addAction(QStringLiteral("独显选中对象"));
            QAction* showAllAction = menu.addAction(QStringLiteral("显示全部对象"));
            QAction* chosenAction = menu.exec(tree->viewport()->mapToGlobal(position));
            if (chosenAction == soloAction || chosenAction == showAllAction) {
                QSet<QString> selectedKeys;
                for (QTreeWidgetItem* selectedItem : selected) {
                    selectedKeys.insert(selectedItem->data(0, Qt::UserRole + 1).toString() + QLatin1Char('|') +
                                        selectedItem->data(0, Qt::UserRole).toString());
                }
                QSet<QString> changedKeys;
                const bool showAll = chosenAction == showAllAction;
                const auto updateTree = [&](QTreeWidget* targetTree, auto&& updateRef) -> void {
                    const auto updateItem = [&](QTreeWidgetItem* item, auto&& itemRef) -> void {
                        const QString type = item->data(0, Qt::UserRole + 1).toString();
                        const QString name = item->data(0, Qt::UserRole).toString();
                        const QString key = type + QLatin1Char('|') + name;
                        bool keepVisible = showAll || selectedKeys.contains(key);
                        for (QTreeWidgetItem* parent = item->parent(); parent && !keepVisible; parent = parent->parent()) {
                            const QString parentKey = parent->data(0, Qt::UserRole + 1).toString() + QLatin1Char('|') +
                                                      parent->data(0, Qt::UserRole).toString();
                            keepVisible = selectedKeys.contains(parentKey);
                        }
                        if (!type.isEmpty() && !name.isEmpty()) {
                            const bool visible = keepVisible;
                            if (item->checkState(0) != (visible ? Qt::Checked : Qt::Unchecked)) changedKeys.insert(key);
                            item->setCheckState(0, visible ? Qt::Checked : Qt::Unchecked);
                        }
                        for (int i = 0; i < item->childCount(); ++i) itemRef(item->child(i), itemRef);
                    };
                    for (int i = 0; i < targetTree->topLevelItemCount(); ++i) updateItem(targetTree->topLevelItem(i), updateItem);
                };
                m_updatingChecks = true;
                m_partTree->blockSignals(true);
                m_deformerTree->blockSignals(true);
                updateTree(m_partTree, updateTree);
                updateTree(m_deformerTree, updateTree);
                m_partTree->blockSignals(false);
                m_deformerTree->blockSignals(false);
                m_updatingChecks = false;
                for (const QString& key : changedKeys) {
                    const int separator = key.indexOf(QLatin1Char('|'));
                    if (separator <= 0) continue;
                    const QString type = key.left(separator);
                    const QString name = key.mid(separator + 1);
                    emit VisibilityChanged(name, type, showAll || selectedKeys.contains(key));
                }
                return;
            }
            if (chosenAction != lockAction) return;
            for (QTreeWidgetItem* selectedItem : selected) {
                const QString name = selectedItem->data(0, Qt::UserRole).toString();
                const QString type = selectedItem->data(0, Qt::UserRole + 1).toString();
                setItemLocked(selectedItem, !allLocked);
                emit ObjectLockChanged(name, type, !allLocked);
            }
        });
    };
    installLockMenu(m_partTree);
    installLockMenu(m_deformerTree);
}

void CModelTreePanel::BuildTree(const QString& mocPath, CCidJson* cid)
{
    m_partTree->clear();
    m_deformerTree->clear();
    m_nameToPartItem.clear();
    m_nameToDeformerItem.clear();

    CReader reader(mocPath);
    CSectionTable sec(reader);
    if (!sec.IsValid()) return;

    int partCount = sec.parts.size();
    int deformerCount = sec.deformers.size();
    int artMeshCount = sec.art_meshes.size();
    const auto setVisible = [](QTreeWidgetItem* item, bool visible) {
        item->setCheckState(0, visible ? Qt::Checked : Qt::Unchecked);
    };

    // Part 层级
    QMap<int, QVector<int>> partChildren;
    QVector<int> rootParts;
    for(int i = 0; i < partCount; i++){
        int parentIdx = sec.parts[i]->parent_part_indices;
        if(parentIdx < 0) rootParts.append(i);
        else partChildren[parentIdx].append(i);
    }

    // Deformer 归属（用于部件树中变形器嵌套）
    QMap<int, QVector<int>> partDeformers;
    QMap<int, QVector<int>> deformerChildren;
    for(int i = 0; i < deformerCount; i++){
        int parentDeformer = sec.deformers[i]->parent_deformer_indices;
        if(parentDeformer < 0){
            int partIdx = sec.deformers[i]->parent_part_indices;
            partDeformers[partIdx].append(i);
        } else {
            deformerChildren[parentDeformer].append(i);
        }
    }

    // ArtMesh 归属
    QMap<int, QVector<int>> deformerArtMeshes;
    QMap<int, QVector<int>> partDirectArtMeshes;
    for(int i = 0; i < artMeshCount; i++){
        int parentDeformer = sec.art_meshes[i]->parent_deformer_indices;
        if(parentDeformer >= 0) deformerArtMeshes[parentDeformer].append(i);
        else partDirectArtMeshes[sec.art_meshes[i]->parent_part_indices].append(i);
    }

    // 构建变形器 → ArtMesh 映射（递归含子变形器）
    m_deformerToArtMeshes.clear();
    std::function<QStringList(int, QSet<int>&)> collectArtMeshesUnderDeformer;
    collectArtMeshesUnderDeformer = [&](int defIdx, QSet<int>& visited) -> QStringList {
        if (defIdx < 0 || defIdx >= deformerCount || visited.contains(defIdx)) return {};
        visited.insert(defIdx);
        QStringList result;
        for(int amIdx : deformerArtMeshes[defIdx]){
            result << sec.art_meshes[amIdx]->ids;
        }
        for(int childIdx : deformerChildren[defIdx]){
            result << collectArtMeshesUnderDeformer(childIdx, visited);
        }
        return result;
    };
    for(int i = 0; i < deformerCount; i++){
        QString defName = sec.deformers[i]->ids;
        QSet<int> visited;
        m_deformerToArtMeshes[defName] = collectArtMeshesUnderDeformer(i, visited);
    }

    // 构建变形器 → 绑定参数映射
    m_deformerToParams.clear();
    {
        // parameter_bindings[k] 属于哪个参数？反向映射
        QMap<int, int> pbToParamIdx;
        for(int i = 0; i < sec.parameters.size(); i++){
            int begin = sec.parameters[i]->parameter_binding_sources_begin_indices;
            int count = sec.parameters[i]->parameter_binding_sources_counts;
            for(int j = 0; j < count; j++){
                pbToParamIdx[begin + j] = i;
            }
        }

        // 对每个 deformer，收集其下 ArtMesh 绑定的参数
        // 同时也收集变形器自身、父链、以及同链所有变形器的绑定
        for(int i = 0; i < deformerCount; i++){
            DeformerParamInfo info;
            QSet<int> addedParams;

            auto tryAddFromKb = [&](int kbIdx){
                if(kbIdx < 0 || kbIdx >= sec.keyform_bindings.size()) return;
                auto* kb = sec.keyform_bindings[kbIdx];
                int beginIdx = kb->parameter_binding_index_sources_begin_indices;
                int count = kb->parameter_binding_index_sources_counts;
                for(int j = 0; j < count; j++){
                    int pbiIdx = beginIdx + j;
                    if(pbiIdx < 0 || pbiIdx >= sec.parameter_binding_indices.size()) continue;
                    int pbIdx = sec.parameter_binding_indices[pbiIdx]->binding_sources_indices;
                    if(!pbToParamIdx.contains(pbIdx)) continue;
                    int paramIdx = pbToParamIdx[pbIdx];
                    if(addedParams.contains(paramIdx)) continue;
                    addedParams.insert(paramIdx);
                    auto* param = sec.parameters[paramIdx];
                    info.paramIds << param->ids;
                    info.minVals << param->min_values;
                    info.maxVals << param->max_values;
                    info.defaultVals << param->default_values;
                }
            };

            // 1) 变形器自身
            tryAddFromKb(sec.deformers[i]->keyform_binding_sources_indices);

            // 2) 沿父变形器链向上
            int parentDef = sec.deformers[i]->parent_deformer_indices;
            while(parentDef >= 0 && parentDef < deformerCount){
                tryAddFromKb(sec.deformers[parentDef]->keyform_binding_sources_indices);
                parentDef = sec.deformers[parentDef]->parent_deformer_indices;
            }

            // 3) 子 ArtMesh 的绑定
            QVector<int> amIndices;
            std::function<void(int, QSet<int>&)> collectAM;
            collectAM = [&](int defIdx, QSet<int>& visited){
                if (defIdx < 0 || defIdx >= deformerCount || visited.contains(defIdx)) return;
                visited.insert(defIdx);
                for(int amIdx : deformerArtMeshes[defIdx])
                    amIndices.append(amIdx);
                for(int childIdx : deformerChildren[defIdx])
                    collectAM(childIdx, visited);
            };
            QSet<int> visited;
            collectAM(i, visited);
            for(int amIdx : amIndices){
                tryAddFromKb(sec.art_meshes[amIdx]->keyform_binding_sources_indices);
            }

            m_deformerToParams[sec.deformers[i]->ids] = info;
        }
    }

    // 获取变形器显示名的 lambda
    auto getDeformerDisplayName = [&](const QString& defId) -> QString {
        if(cid){
            QString mapped = cid->getDeformerName(defId);
            if(!mapped.isEmpty()) return mapped;
        }
        return defId;
    };

    auto getPartDisplayName = [&](const QString& partId) -> QString {
        if(cid){
            QString mapped = cid->getPartName(partId);
            if(!mapped.isEmpty()) return mapped;
        }
        return partId;
    };

    // ========= 构建部件树 (Part Tree) =========

    // 递归构建 Deformer 子树 (部件树用)
    std::function<void(QTreeWidgetItem*, int, QSet<int>&)> buildDeformerSubtree;
    buildDeformerSubtree = [&](QTreeWidgetItem* parentItem, int defIdx, QSet<int>& visited){
        if (defIdx < 0 || defIdx >= deformerCount || visited.contains(defIdx)) return;
        visited.insert(defIdx);
        for(int childIdx : deformerChildren[defIdx]){
            CDeformers* def = sec.deformers[childIdx];
            QString defName = def->ids;
            QTreeWidgetItem* item = new QTreeWidgetItem(parentItem);
            item->setText(0, getDeformerDisplayName(defName));
            item->setData(0, Qt::UserRole, defName);
            QString typeStr = (def->types == 0) ? "warp" : "rotation";
            item->setData(0, Qt::UserRole + 1, typeStr);
            item->setIcon(0, (def->types == 0)
                ? style()->standardIcon(QStyle::SP_ArrowRight)
                : style()->standardIcon(QStyle::SP_BrowserReload));
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            setVisible(item, def->is_visible);
            m_nameToPartItem[defName] = item;
            buildDeformerSubtree(item, childIdx, visited);
        }
        for(int amIdx : deformerArtMeshes[defIdx]){
            QString amName = sec.art_meshes[amIdx]->ids;
            QTreeWidgetItem* item = new QTreeWidgetItem(parentItem);
            item->setText(0, amName);
            item->setData(0, Qt::UserRole, amName);
            item->setData(0, Qt::UserRole + 1, "artmesh");
            item->setIcon(0, style()->standardIcon(QStyle::SP_FileIcon));
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            setVisible(item, sec.art_meshes[amIdx]->is_visible);
            m_nameToPartItem[amName] = item;
        }
    };

    std::function<void(QTreeWidgetItem*, int, QSet<int>&, QSet<int>&)> buildPartSubtree;
    buildPartSubtree = [&](QTreeWidgetItem* parentItem, int partIdx, QSet<int>& partPath, QSet<int>& deformerPath){
        if (partIdx < 0 || partIdx >= partCount || partPath.contains(partIdx)) return;
        partPath.insert(partIdx);
        for(int childPartIdx : partChildren[partIdx]){
            CParts* part = sec.parts[childPartIdx];
            QString partId = part->ids;
            QTreeWidgetItem* item = new QTreeWidgetItem(parentItem);
            item->setText(0, getPartDisplayName(partId));
            item->setData(0, Qt::UserRole, partId);
            item->setData(0, Qt::UserRole + 1, "part");
            item->setIcon(0, style()->standardIcon(QStyle::SP_DirIcon));
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            setVisible(item, part->is_visible);
            m_nameToPartItem[partId] = item;
            buildPartSubtree(item, childPartIdx, partPath, deformerPath);
        }
        for(int defIdx : partDeformers[partIdx]){
            CDeformers* def = sec.deformers[defIdx];
            QString defName = def->ids;
            QTreeWidgetItem* item = new QTreeWidgetItem(parentItem);
            item->setText(0, getDeformerDisplayName(defName));
            item->setData(0, Qt::UserRole, defName);
            QString typeStr = (def->types == 0) ? "warp" : "rotation";
            item->setData(0, Qt::UserRole + 1, typeStr);
            item->setIcon(0, (def->types == 0)
                ? style()->standardIcon(QStyle::SP_ArrowRight)
                : style()->standardIcon(QStyle::SP_BrowserReload));
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            setVisible(item, def->is_visible);
            m_nameToPartItem[defName] = item;
            QSet<int> branchVisited = deformerPath;
            buildDeformerSubtree(item, defIdx, branchVisited);
        }
        for(int amIdx : partDirectArtMeshes[partIdx]){
            QString amName = sec.art_meshes[amIdx]->ids;
            QTreeWidgetItem* item = new QTreeWidgetItem(parentItem);
            item->setText(0, amName);
            item->setData(0, Qt::UserRole, amName);
            item->setData(0, Qt::UserRole + 1, "artmesh");
            item->setIcon(0, style()->standardIcon(QStyle::SP_FileIcon));
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            setVisible(item, sec.art_meshes[amIdx]->is_visible);
            m_nameToPartItem[amName] = item;
        }
        partPath.remove(partIdx);
    };

    m_partTree->blockSignals(true);
    for(int rootIdx : rootParts){
        CParts* part = sec.parts[rootIdx];
        QString partId = part->ids;
        QTreeWidgetItem* item = new QTreeWidgetItem(m_partTree);
        item->setText(0, getPartDisplayName(partId));
        item->setData(0, Qt::UserRole, partId);
        item->setData(0, Qt::UserRole + 1, "part");
        item->setIcon(0, style()->standardIcon(QStyle::SP_DirIcon));
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        setVisible(item, part->is_visible);
        m_nameToPartItem[partId] = item;
        QSet<int> partPath;
        QSet<int> deformerPath;
        buildPartSubtree(item, rootIdx, partPath, deformerPath);
    }
    m_partTree->blockSignals(false);
    m_partTree->expandToDepth(1);

    // ========= 构建变形器树 (Deformer Tree) =========
    // 完整 Part 层级 + 每个 Part 下列出属于它的变形器（按 parentPart 归属）

    // 按 parentPart 收集每个 Part 拥有的变形器
    QMap<int, QVector<int>> partOwnedDeformers;
    for(int i = 0; i < deformerCount; i++){
        int pIdx = sec.deformers[i]->parent_part_indices;
        partOwnedDeformers[pIdx].append(i);
    }

    // 递归构建变形器树的 Part 层级
    std::function<void(QTreeWidgetItem*, int, QSet<int>&)> buildDeformerTreePart;
    buildDeformerTreePart = [&](QTreeWidgetItem* parentItem, int partIdx, QSet<int>& visited){
        if (partIdx < 0 || partIdx >= partCount || visited.contains(partIdx)) return;
        visited.insert(partIdx);
        // 子 Part
        for(int childPartIdx : partChildren[partIdx]){
            CParts* childPart = sec.parts[childPartIdx];
            QString childPartId = childPart->ids;
            QTreeWidgetItem* childItem = new QTreeWidgetItem(parentItem);
            childItem->setText(0, getPartDisplayName(childPartId));
            childItem->setData(0, Qt::UserRole, childPartId);
            childItem->setData(0, Qt::UserRole + 1, "part");
            childItem->setIcon(0, style()->standardIcon(QStyle::SP_DirIcon));
            childItem->setFlags(childItem->flags() | Qt::ItemIsUserCheckable);
            setVisible(childItem, childPart->is_visible);
            m_nameToDeformerItem[childPartId] = childItem;
            buildDeformerTreePart(childItem, childPartIdx, visited);
        }
        visited.remove(partIdx);
        // 该 Part 拥有的变形器（平铺列出，不嵌套）
        for(int defIdx : partOwnedDeformers[partIdx]){
            CDeformers* def = sec.deformers[defIdx];
            QString defName = def->ids;
            QTreeWidgetItem* item = new QTreeWidgetItem(parentItem);
            item->setText(0, getDeformerDisplayName(defName));
            item->setData(0, Qt::UserRole, defName);
            QString typeStr = (def->types == 0) ? "warp" : "rotation";
            item->setData(0, Qt::UserRole + 1, typeStr);
            item->setIcon(0, (def->types == 0)
                ? style()->standardIcon(QStyle::SP_ArrowRight)
                : style()->standardIcon(QStyle::SP_BrowserReload));
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            setVisible(item, def->is_visible);
            m_nameToDeformerItem[defName] = item;
        }
    };

    m_deformerTree->blockSignals(true);
    for(int rootIdx : rootParts){
        CParts* part = sec.parts[rootIdx];
        QString partId = part->ids;
        QTreeWidgetItem* partItem = new QTreeWidgetItem(m_deformerTree);
        partItem->setText(0, getPartDisplayName(partId));
        partItem->setData(0, Qt::UserRole, partId);
        partItem->setData(0, Qt::UserRole + 1, "part");
        partItem->setIcon(0, style()->standardIcon(QStyle::SP_DirIcon));
        partItem->setFlags(partItem->flags() | Qt::ItemIsUserCheckable);
        setVisible(partItem, part->is_visible);
        m_nameToDeformerItem[partId] = partItem;
        QSet<int> visited;
        buildDeformerTreePart(partItem, rootIdx, visited);
    }
    m_deformerTree->blockSignals(false);
    m_deformerTree->expandToDepth(1);

}

void CModelTreePanel::setItemLocked(QTreeWidgetItem* item, bool locked)
{
    if (!item) return;
    Qt::ItemFlags flags = item->flags();
    if (locked) flags &= ~Qt::ItemIsUserCheckable;
    else flags |= Qt::ItemIsUserCheckable;
    item->setFlags(flags);
    item->setData(0, Qt::UserRole + 2, locked);
    QFont font = item->font(0);
    font.setStrikeOut(locked);
    item->setFont(0, font);
    item->setToolTip(0, locked ? QStringLiteral("已锁定") : QString());
}

QMap<QString, bool> CModelTreePanel::LockedState() const
{
    QMap<QString, bool> state;
    const auto collect = [&state](QTreeWidgetItem* item, auto&& collectRef) -> void {
        const QString type = item->data(0, Qt::UserRole + 1).toString();
        const QString id = item->data(0, Qt::UserRole).toString();
        if (item->data(0, Qt::UserRole + 2).toBool() && !type.isEmpty() && !id.isEmpty()) {
            state.insert(type + QLatin1Char('|') + id, true);
        }
        for (int i = 0; i < item->childCount(); ++i) collectRef(item->child(i), collectRef);
    };
    for (QTreeWidget* tree : {m_partTree, m_deformerTree}) {
        for (int i = 0; i < tree->topLevelItemCount(); ++i) collect(tree->topLevelItem(i), collect);
    }
    return state;
}

void CModelTreePanel::ApplyLockedState(const QMap<QString, bool>& state)
{
    const auto apply = [this, &state](QTreeWidgetItem* item, auto&& applyRef) -> void {
        const QString type = item->data(0, Qt::UserRole + 1).toString();
        const QString id = item->data(0, Qt::UserRole).toString();
        const QString key = type + QLatin1Char('|') + id;
        const bool locked = state.value(key, false);
        setItemLocked(item, locked);
        for (int i = 0; i < item->childCount(); ++i) applyRef(item->child(i), applyRef);
    };
    for (QTreeWidget* tree : {m_partTree, m_deformerTree}) {
        for (int i = 0; i < tree->topLevelItemCount(); ++i) apply(tree->topLevelItem(i), apply);
    }
}

QMap<QString, bool> CModelTreePanel::VisibilityState() const
{
    QMap<QString, bool> state;
    const auto collect = [&state](QTreeWidgetItem* item, auto&& collectRef) -> void {
        const QString type = item->data(0, Qt::UserRole + 1).toString();
        const QString id = item->data(0, Qt::UserRole).toString();
        if (!type.isEmpty() && !id.isEmpty()) state.insert(type + QLatin1Char('|') + id, item->checkState(0) == Qt::Checked);
        for (int i = 0; i < item->childCount(); ++i) collectRef(item->child(i), collectRef);
    };
    for (QTreeWidget* tree : {m_partTree, m_deformerTree}) {
        for (int i = 0; i < tree->topLevelItemCount(); ++i) collect(tree->topLevelItem(i), collect);
    }
    return state;
}

void CModelTreePanel::ApplyVisibilityState(const QMap<QString, bool>& state)
{
    m_updatingChecks = true;
    const auto apply = [this, &state](QTreeWidgetItem* item, auto&& applyRef) -> void {
        const QString type = item->data(0, Qt::UserRole + 1).toString();
        const QString id = item->data(0, Qt::UserRole).toString();
        const QString key = type + QLatin1Char('|') + id;
        if (state.contains(key)) item->setCheckState(0, state.value(key) ? Qt::Checked : Qt::Unchecked);
        for (int i = 0; i < item->childCount(); ++i) applyRef(item->child(i), applyRef);
    };
    for (QTreeWidget* tree : {m_partTree, m_deformerTree}) {
        tree->blockSignals(true);
        for (int i = 0; i < tree->topLevelItemCount(); ++i) apply(tree->topLevelItem(i), apply);
        tree->blockSignals(false);
    }
    m_updatingChecks = false;

    const auto emitState = [this](QTreeWidgetItem* item, auto&& emitRef) -> void {
        const QString type = item->data(0, Qt::UserRole + 1).toString();
        const QString id = item->data(0, Qt::UserRole).toString();
        if ((type == QLatin1String("part") || type == QLatin1String("artmesh") ||
             type == QLatin1String("warp") || type == QLatin1String("rotation")) && !id.isEmpty())
            emit VisibilityChanged(id, type, item->checkState(0) == Qt::Checked);
        for (int i = 0; i < item->childCount(); ++i) emitRef(item->child(i), emitRef);
    };
    for (QTreeWidget* tree : {m_partTree, m_deformerTree}) {
        for (int i = 0; i < tree->topLevelItemCount(); ++i) emitState(tree->topLevelItem(i), emitState);
    }
}

void CModelTreePanel::SetVisibility(const QString& name, const QString& type, bool visible)
{
    const auto updateItem = [visible](QTreeWidgetItem* item, auto&& updateRef) -> void {
        if (!item) return;
        item->setCheckState(0, visible ? Qt::Checked : Qt::Unchecked);
        for (int i = 0; i < item->childCount(); ++i) updateRef(item->child(i), updateRef);
    };
    m_updatingChecks = true;
    for (QTreeWidget* tree : {m_partTree, m_deformerTree}) tree->blockSignals(true);
    if (type == QLatin1String("part")) {
        updateItem(m_nameToPartItem.value(name, nullptr), updateItem);
        updateItem(m_nameToDeformerItem.value(name, nullptr), updateItem);
    } else if (type == QLatin1String("artmesh")) {
        if (m_nameToPartItem.contains(name)) m_nameToPartItem.value(name)->setCheckState(0, visible ? Qt::Checked : Qt::Unchecked);
        if (m_nameToDeformerItem.contains(name)) m_nameToDeformerItem.value(name)->setCheckState(0, visible ? Qt::Checked : Qt::Unchecked);
    } else {
        updateItem(m_nameToPartItem.value(name, nullptr), updateItem);
        updateItem(m_nameToDeformerItem.value(name, nullptr), updateItem);
    }
    for (QTreeWidget* tree : {m_partTree, m_deformerTree}) tree->blockSignals(false);
    m_updatingChecks = false;

    const auto emitItem = [this](QTreeWidgetItem* item, auto&& emitRef) -> void {
        if (!item) return;
        const QString itemType = item->data(0, Qt::UserRole + 1).toString();
        const QString itemName = item->data(0, Qt::UserRole).toString();
        if ((itemType == QLatin1String("part") || itemType == QLatin1String("artmesh") ||
             itemType == QLatin1String("warp") || itemType == QLatin1String("rotation")) && !itemName.isEmpty())
            emit VisibilityChanged(itemName, itemType, item->checkState(0) == Qt::Checked);
        for (int i = 0; i < item->childCount(); ++i) emitRef(item->child(i), emitRef);
    };
    if (type == QLatin1String("artmesh")) {
        if (m_nameToPartItem.contains(name)) emitItem(m_nameToPartItem.value(name), emitItem);
        if (m_nameToDeformerItem.contains(name)) emitItem(m_nameToDeformerItem.value(name), emitItem);
    } else {
        if (m_nameToPartItem.contains(name)) emitItem(m_nameToPartItem.value(name), emitItem);
        if (m_nameToDeformerItem.contains(name)) emitItem(m_nameToDeformerItem.value(name), emitItem);
    }
}

QStringList CModelTreePanel::collectDeformerNames(QTreeWidgetItem* item)
{
    QStringList result;
    QString type = item->data(0, Qt::UserRole + 1).toString();
    if(type == "warp" || type == "rotation"){
        result << item->data(0, Qt::UserRole).toString();
    }
    for(int i = 0; i < item->childCount(); i++){
        result << collectDeformerNames(item->child(i));
    }
    return result;
}

void CModelTreePanel::onPartItemChanged(QTreeWidgetItem* item, int column)
{
    if(column != 0 || m_updatingChecks) return;
    m_updatingChecks = true;

    Qt::CheckState state = item->checkState(0);
    setChildrenChecked(item, state);

    bool visible = (state == Qt::Checked);
    const auto emitSubtree = [this, visible](QTreeWidgetItem* node, auto&& emitRef) -> void {
        const QString type = node->data(0, Qt::UserRole + 1).toString();
        const QString name = node->data(0, Qt::UserRole).toString();
        if ((type == QLatin1String("part") || type == QLatin1String("artmesh") ||
             type == QLatin1String("warp") || type == QLatin1String("rotation")) && !name.isEmpty()) {
            emit VisibilityChanged(name, type, visible);
        }
        for (int i = 0; i < node->childCount(); ++i) emitRef(node->child(i), emitRef);
    };
    emitSubtree(item, emitSubtree);

    m_updatingChecks = false;
}

void CModelTreePanel::onDeformerItemChanged(QTreeWidgetItem* item, int column)
{
    if(column != 0 || m_updatingChecks) return;
    m_updatingChecks = true;

    Qt::CheckState state = item->checkState(0);
    setChildrenChecked(item, state);

    const bool visible = state == Qt::Checked;
    const auto emitSubtree = [this, visible](QTreeWidgetItem* node, auto&& emitRef) -> void {
        const QString type = node->data(0, Qt::UserRole + 1).toString();
        const QString name = node->data(0, Qt::UserRole).toString();
        if ((type == QLatin1String("part") || type == QLatin1String("artmesh") ||
             type == QLatin1String("warp") || type == QLatin1String("rotation")) && !name.isEmpty()) {
            emit VisibilityChanged(name, type, visible);
        }
        for (int i = 0; i < node->childCount(); ++i) emitRef(node->child(i), emitRef);
    };
    emitSubtree(item, emitSubtree);

    // 取消勾选时隐藏对应变形器的网格叠加
    if(state == Qt::Unchecked){
        QString type = item->data(0, Qt::UserRole + 1).toString();
        if(type == "warp" || type == "rotation"){
            // 发射空列表来隐藏（ShowDeformers 会先 HideAll 再显示指定的）
            // 这里只需要告知刷新当前显示：隐藏被取消勾选的
            emit ShowDeformers(QStringList());
        }
    }

    m_updatingChecks = false;
}

void CModelTreePanel::setChildrenChecked(QTreeWidgetItem* item, Qt::CheckState state)
{
    for(int i = 0; i < item->childCount(); i++){
        QTreeWidgetItem* child = item->child(i);
        child->setCheckState(0, state);
        setChildrenChecked(child, state);
    }
}

void CModelTreePanel::HighlightMesh(const QString& artName)
{
    if(artName.isEmpty()){
        m_partTree->clearSelection();
        m_deformerTree->clearSelection();
        return;
    }
    // 部件树中高亮
    if(m_nameToPartItem.contains(artName)){
        m_partTree->clearSelection();
        QTreeWidgetItem* item = m_nameToPartItem[artName];
        item->setSelected(true);
        m_partTree->scrollToItem(item);

        // 在变形器树中定位父变形器
        QTreeWidgetItem* parent = item->parent();
        if(parent){
            QString parentName = parent->data(0, Qt::UserRole).toString();
            if(m_nameToDeformerItem.contains(parentName)){
                m_deformerTree->clearSelection();
                QTreeWidgetItem* defItem = m_nameToDeformerItem[parentName];
                defItem->setSelected(true);
                m_deformerTree->scrollToItem(defItem);
            }
        }
    }
}
