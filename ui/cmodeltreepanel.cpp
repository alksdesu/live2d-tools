#include "cmodeltreepanel.h"
#include "ctreenodedelegate.h"
#include "live2dstruct/ctreemanage.h"
#include "live2dstruct/tree/c2dtree.h"
#include "live2dstruct/tree/c2dtreenode.h"
#include "live2dstruct/tree/cartmeshnode.h"
#include "live2dstruct/tree/cpartsnode.h"
#include "live2dstruct/tree/cwarpnode.h"
#include "live2dstruct/tree/crotationnode.h"
#include "live2dstruct/tree/cparameternode.h"
#include <QVBoxLayout>
#include <QHeaderView>
#include <QDebug>

using namespace live2dstruct;

CModelTreePanel::CModelTreePanel(QWidget *parent)
    : QDockWidget("模型结构", parent)
{
    m_tree = new QTreeWidget(this);
    m_tree->setHeaderLabel("模型结构树");
    m_tree->header()->setVisible(false);
    m_tree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_tree->setIndentation(16);
    m_tree->setAnimated(true);
    m_tree->setRootIsDecorated(true);
    m_tree->setItemDelegate(new CTreeNodeDelegate(m_tree));
    setWidget(m_tree);

    connect(m_tree, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* item, int){
        QString name = item->data(0, Qt::UserRole).toString();
        QString type = item->data(0, Qt::UserRole + 1).toString();
        if(type == "artmesh"){
            emit MeshSelected(name);
        } else if(type == "warp" || type == "rotation"){
            emit WarpSelected(name);
        }
    });
}

void CModelTreePanel::BuildTree()
{
    m_tree->clear();
    m_nameToItem.clear();

    C2DTree* tree = CTreeManage::GetInstance();
    if(!tree){
        qDebug() << "[TreePanel] CTreeManage::GetInstance() returned null";
        return;
    }

    QList<C2DTreeNode*>& partRoot = tree->getPartRoot();
    qDebug() << "[TreePanel] partRoot size:" << partRoot.size();

    for(auto* node : partRoot){
        // 跳过非 Part 节点（ArtMesh 直接挂在 root 的情况）
        bool isPart = dynamic_cast<CPartsNode*>(node) != nullptr;
        QTreeWidgetItem* item = new QTreeWidgetItem(m_tree);
        item->setText(0, node->getName());
        item->setData(0, Qt::UserRole, node->getName());
        if(isPart){
            item->setData(0, Qt::UserRole + 1, "part");
            item->setIcon(0, style()->standardIcon(QStyle::SP_DirIcon));
        } else {
            item->setData(0, Qt::UserRole + 1, "artmesh");
            item->setIcon(0, style()->standardIcon(QStyle::SP_FileIcon));
        }
        m_nameToItem[node->getName()] = item;
        if(isPart){
            BuildPartTree(item, node);
        }
    }

    m_tree->expandAll();
}

void CModelTreePanel::BuildPartTree(QTreeWidgetItem* parentItem, void* nodePtr)
{
    C2DTreeNode* node = static_cast<C2DTreeNode*>(nodePtr);
    for(auto* child : node->getChilds()){
        // 跳过 ParameterNode（参数节点不应出现在结构树中）
        if(dynamic_cast<CParameterNode*>(child)) continue;

        QTreeWidgetItem* item = new QTreeWidgetItem(parentItem);
        item->setText(0, child->getName());
        item->setData(0, Qt::UserRole, child->getName());

        if(dynamic_cast<CArtMeshNode*>(child)){
            item->setData(0, Qt::UserRole + 1, "artmesh");
            item->setIcon(0, style()->standardIcon(QStyle::SP_FileIcon));
        } else if(dynamic_cast<CWarpNode*>(child)){
            item->setData(0, Qt::UserRole + 1, "warp");
            item->setIcon(0, style()->standardIcon(QStyle::SP_ArrowRight));
            BuildPartTree(item, child);
        } else if(dynamic_cast<CRotationNode*>(child)){
            item->setData(0, Qt::UserRole + 1, "rotation");
            item->setIcon(0, style()->standardIcon(QStyle::SP_BrowserReload));
            BuildPartTree(item, child);
        } else if(dynamic_cast<CPartsNode*>(child)){
            item->setData(0, Qt::UserRole + 1, "part");
            item->setIcon(0, style()->standardIcon(QStyle::SP_DirIcon));
            BuildPartTree(item, child);
        } else {
            item->setData(0, Qt::UserRole + 1, "other");
        }

        m_nameToItem[child->getName()] = item;
    }
}

void CModelTreePanel::HighlightMesh(const QString& artName)
{
    if(m_nameToItem.contains(artName)){
        m_tree->clearSelection();
        QTreeWidgetItem* item = m_nameToItem[artName];
        item->setSelected(true);
        m_tree->scrollToItem(item);
    }
}
