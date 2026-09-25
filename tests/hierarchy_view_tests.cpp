#include <QtTest>
#include <QAccessible>
#include <QAbstractItemModel>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QStyle>
#include <QStyleFactory>
#include <QStyleOptionViewItem>
#include <memory>
#include "ui/cmodelhierarchyview.h"
#include "ui/theme.h"

namespace {
class HierarchyModel final : public QAbstractItemModel
{
public:
    struct Node {
        int parent = -1;
        QString name, id, type = QStringLiteral("part");
        bool checked = true, locked = false;
        int row = 0;
        QVector<int> children;
    };
    explicit HierarchyModel(QObject* parent = nullptr) : QAbstractItemModel(parent) {}
    void Set(QVector<Node> nodes)
    {
        beginResetModel();
        m_nodes = std::move(nodes);
        m_roots.clear();
        for (int i = 0; i < m_nodes.size(); ++i) {
            auto& node = m_nodes[i];
            auto& siblings = node.parent < 0 ? m_roots : m_nodes[node.parent].children;
            node.row = siblings.size();
            siblings.append(i);
        }
        endResetModel();
    }
    static QVector<Node> Chain(int count)
    {
        QVector<Node> nodes;
        nodes.reserve(count);
        for (int i = 0; i < count; ++i)
            nodes.append({i - 1, QStringLiteral("Node %1").arg(i), QStringLiteral("id%1").arg(i)});
        return nodes;
    }
    static QVector<Node> Wide(int count)
    {
        auto nodes = Chain(count);
        for (auto& node : nodes) node.parent = -1;
        return nodes;
    }
    QModelIndex At(int number) const
    {
        return number >= 0 && number < m_nodes.size() ? createIndex(m_nodes[number].row, 0, quintptr(number + 1)) : QModelIndex();
    }
    QModelIndex index(int row, int column, const QModelIndex& parent = {}) const override
    {
        if (column != 0 || row < 0 || (parent.isValid() && parent.model() != this)) return {};
        const auto& siblings = parent.isValid() ? m_nodes[Number(parent)].children : m_roots;
        return row < siblings.size() ? At(siblings[row]) : QModelIndex();
    }
    QModelIndex parent(const QModelIndex& index) const override
    {
        return index.isValid() && index.model() == this ? At(m_nodes[Number(index)].parent) : QModelIndex();
    }
    int rowCount(const QModelIndex& parent = {}) const override
    {
        return parent.isValid() ? (parent.model() == this && parent.column() == 0 ? m_nodes[Number(parent)].children.size() : 0) : m_roots.size();
    }
    int columnCount(const QModelIndex& = {}) const override { return 1; }
    QVariant data(const QModelIndex& index, int role) const override
    {
        if (!index.isValid() || index.model() != this) return {};
        const auto& node = m_nodes[Number(index)];
        switch (role) {
        case Qt::DisplayRole: return node.name;
        case Qt::UserRole: return node.id;
        case Qt::UserRole + 1: return node.type;
        case Qt::UserRole + 2: return node.locked;
        case Qt::ToolTipRole: return node.id + QStringLiteral(" <model object>");
        case Qt::CheckStateRole: return node.checked ? Qt::Checked : Qt::Unchecked;
        default: return {};
        }
    }
    Qt::ItemFlags flags(const QModelIndex& index) const override
    {
        if (!index.isValid()) return {};
        auto flags = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
        if (!m_nodes[Number(index)].locked) flags |= Qt::ItemIsUserCheckable;
        return flags;
    }
    bool setData(const QModelIndex& index, const QVariant& value, int role) override
    {
        if (!index.isValid() || role != Qt::CheckStateRole || m_nodes[Number(index)].locked) return false;
        m_nodes[Number(index)].checked = value.toInt() == Qt::Checked;
        emit dataChanged(index, index, {Qt::CheckStateRole});
        return true;
    }
    void Lock(int number, bool locked)
    {
        m_nodes[number].locked = locked;
        emit dataChanged(At(number), At(number), {Qt::UserRole + 2, Qt::FontRole, Qt::ToolTipRole});
    }
private:
    int Number(const QModelIndex& index) const { return int(index.internalId()) - 1; }
    QVector<Node> m_nodes;
    QVector<int> m_roots;
};

void show(CModelHierarchyView& view)
{
    view.resize(520, 300);
    view.show();
    view.setFocus();
    QCoreApplication::processEvents();
}

bool hasPaintedContent(const QImage& image)
{
    if (image.isNull()) return false;
    const auto first = image.pixel(0, 0);
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x)
            if (image.pixel(x, y) != first) return true;
    return false;
}

QPoint textPoint(CModelHierarchyView& view, const QModelIndex& index)
{
    view.scrollTo(index);
    QCoreApplication::processEvents();
    const QRect rect = view.visualRect(index).intersected(view.viewport()->rect());
    return QPoint(rect.right() - 3, rect.center().y());
}

QPoint checkPoint(CModelHierarchyView& view, const QModelIndex& index)
{
    QStyleOptionViewItem option;
    option.initFrom(&view);
    option.rect = view.visualRect(index);
    option.index = index;
    option.features = QStyleOptionViewItem::HasDisplay | QStyleOptionViewItem::HasCheckIndicator;
    option.text = index.data().toString();
    option.checkState = Qt::CheckState(index.data(Qt::CheckStateRole).toInt());
    return view.style()->subElementRect(QStyle::SE_ItemViewItemCheckIndicator, &option, &view).center();
}
}

class HierarchyViewTests : public QObject
{
    Q_OBJECT
private slots:
    void disclosureIsVisibleAndClickableAcrossThemesAndStyles_data()
    {
        QTest::addColumn<QString>("styleName");
        QTest::addColumn<bool>("dark");
        QTest::addColumn<bool>("rtl");
        QStringList styles{QStringLiteral("Fusion"), QApplication::style()->objectName()};
        styles.removeDuplicates();
        for (const auto& style : styles) for (bool dark : {false, true}) for (bool rtl : {false, true}) {
            const auto name = QStringLiteral("%1-%2-%3").arg(style, dark ? "dark" : "light", rtl ? "rtl" : "ltr").toLatin1();
            QTest::newRow(name.constData()) << style << dark << rtl;
        }
    }

    void disclosureIsVisibleAndClickableAcrossThemesAndStyles()
    {
        QFETCH(QString, styleName);
        QFETCH(bool, dark);
        QFETCH(bool, rtl);
        std::unique_ptr<QStyle> style(QStyleFactory::create(styleName));
        QVERIFY2(style, qPrintable(styleName));
        HierarchyModel model;
        model.Set({{-1, "Root", "root"}, {0, "Child", "child"}, {-1, "Leaf root", "leaf"}});
        CModelHierarchyView view;
        view.setStyle(style.get());
        view.setStyleSheet(Theme::globalStyleSheet(dark));
        view.setLayoutDirection(rtl ? Qt::RightToLeft : Qt::LeftToRight);
        view.setModel(&model);
        show(view);
        const int indent = view.style()->pixelMetric(QStyle::PM_TreeViewIndentation, nullptr, &view);
        auto branchRect = [&](const QModelIndex& index) {
            const auto rect = view.visualRect(index);
            return QRect(rtl ? rect.right() + 1 : rect.left() - indent, rect.y(), indent, rect.height());
        };
        auto capture = [&](const QModelIndex& index) {
            QCoreApplication::processEvents();
            return view.viewport()->grab(branchRect(index)).toImage();
        };
        const auto collapsed = capture(model.At(0));
        QVERIFY2(hasPaintedContent(collapsed), "Collapsed parent must have a visible disclosure glyph");
        QVERIFY(!hasPaintedContent(capture(model.At(2))));
        const auto center = branchRect(model.At(0)).center();
        QCOMPARE(view.indexAt(center), model.At(0));
        QSignalSpy clicks(&view, &QAbstractItemView::clicked);
        QTest::mouseClick(view.viewport(), Qt::LeftButton, {}, center);
        QVERIFY(view.isExpanded(model.At(0)));
        QVERIFY(!view.visualRect(model.At(1)).isEmpty());
        QCOMPARE(clicks.size(), 0);
        const auto expanded = capture(model.At(0));
        QVERIFY2(hasPaintedContent(expanded), "Expanded parent must have a visible disclosure glyph");
        QVERIFY2(expanded != collapsed, "Expanded and collapsed glyphs must differ");
        QVERIFY(!hasPaintedContent(capture(model.At(1))));
        QTest::mouseClick(view.viewport(), Qt::LeftButton, {}, center);
        QVERIFY(!view.isExpanded(model.At(0)));
        QVERIFY(view.visualRect(model.At(1)).isEmpty());
        QVERIFY(hasPaintedContent(capture(model.At(0))));
        view.setCurrentIndex(model.At(0));
        QTest::keyClick(&view, rtl ? Qt::Key_Left : Qt::Key_Right);
        QVERIFY(view.isExpanded(model.At(0)));
        QTest::keyClick(&view, rtl ? Qt::Key_Right : Qt::Key_Left);
        QVERIFY(!view.isExpanded(model.At(0)));
    }

    void emptyModelAndModelDestructionLeaveNoStaleRows()
    {
        CModelHierarchyView view;
        show(view);
        QVERIFY(!view.indexAt(QPoint(10, 10)).isValid());
        QTest::keyClick(&view, Qt::Key_Down, Qt::ControlModifier);
        auto model = std::make_unique<HierarchyModel>();
        view.setModel(model.get());
        model->Set(HierarchyModel::Wide(2));
        QCOMPARE(view.indexAt(QPoint(10, 10)), model->At(0));
        model.reset();
        QCoreApplication::processEvents();
        QVERIFY(!view.indexAt(QPoint(10, 10)).isValid());
        QCOMPARE(view.verticalScrollBar()->maximum(), 0);
        QCOMPARE(view.horizontalScrollBar()->maximum(), 0);
        view.setModel(nullptr);
        QVERIFY(!view.grab().isNull());
    }

    void deepHierarchyPaintSearchScrollAndResetAreIterative()
    {
        constexpr int depth = 12000;
        HierarchyModel model;
        model.Set(HierarchyModel::Chain(depth));
        CModelHierarchyView view;
        view.setModel(&model);
        show(view);
        QVERIFY(view.visualRect(model.At(depth - 1)).isEmpty());
        QCOMPARE(view.horizontalScrollBar()->maximum(), 0);
        view.expandToDepth(depth);
        const auto saved = view.expandedKeys();
        QCOMPARE(saved.size(), depth - 1);
        view.scrollTo(model.At(depth - 1), QAbstractItemView::PositionAtCenter);
        QCoreApplication::processEvents();
        const QRect leaf = view.visualRect(model.At(depth - 1)).intersected(view.viewport()->rect());
        QVERIFY(!leaf.isEmpty());
        QCOMPARE(view.indexAt(leaf.center()), model.At(depth - 1));
        QVERIFY(view.horizontalScrollBar()->value() > view.width());
        QVERIFY(hasPaintedContent(view.viewport()->grab().toImage()));
        int ancestorCount = 0;
        for (auto index = model.At(depth - 1); index.isValid(); index = index.parent()) ++ancestorCount;
        QCOMPARE(ancestorCount, depth);
        view.restoreExpandedKeys({});
        QCOMPARE(view.horizontalScrollBar()->maximum(), 0);
        view.setFilterText(QStringLiteral("id11999"));
        QCOMPARE(view.expandedKeys().size(), 0);
        QVERIFY(view.isExpanded(model.At(0)));
        view.scrollTo(model.At(depth - 1));
        QVERIFY(!view.visualRect(model.At(depth - 1)).intersected(view.viewport()->rect()).isEmpty());
        QCOMPARE(view.expandedKeys().size(), 0);
        view.restoreExpandedKeys({});
        view.setFilterText({});
        QVERIFY(!view.isExpanded(model.At(0)));
        QVERIFY(view.visualRect(model.At(depth - 1)).isEmpty());
        view.restoreExpandedKeys(saved);
        view.selectionModel()->setCurrentIndex(model.At(depth - 1), QItemSelectionModel::ClearAndSelect);
        model.Set(HierarchyModel::Chain(depth));
        view.restoreExpandedKeys(saved);
        view.scrollTo(model.At(depth - 1));
        QCoreApplication::processEvents();
        QCOMPARE(view.expandedKeys(), saved);
        QVERIFY(!view.grab().isNull());
        view.close();
    }

    void wideHierarchyKeyboardAndHitTestingUseVisibleRows()
    {
        constexpr int count = 10000;
        HierarchyModel model;
        model.Set(HierarchyModel::Wide(count));
        CModelHierarchyView view;
        view.setModel(&model);
        show(view);
        view.selectionModel()->clear();
        QTest::keyClick(&view, Qt::Key_Down);
        QCOMPARE(view.currentIndex(), model.At(0));
        QTest::keyClick(&view, Qt::Key_End);
        QCOMPARE(view.currentIndex(), model.At(count - 1));
        const QRect last = view.visualRect(view.currentIndex()).intersected(view.viewport()->rect());
        QVERIFY(!last.isEmpty());
        QCOMPARE(view.indexAt(last.center()), model.At(count - 1));
        QVERIFY(view.verticalScrollBar()->value() > 0);
        QTest::keyClick(&view, Qt::Key_Home);
        QCOMPARE(view.currentIndex(), model.At(0));
        QCOMPARE(view.verticalScrollBar()->value(), 0);
        QTest::keyClick(&view, Qt::Key_PageDown);
        QVERIFY(view.currentIndex().row() > 1);
        QTest::keyClick(&view, Qt::Key_PageUp);
        QCOMPARE(view.currentIndex(), model.At(0));
        view.keyboardSearch(QStringLiteral("Node 9876"));
        QCOMPARE(view.currentIndex(), model.At(9876));
        QVERIFY(!view.grab().isNull());
    }

    void keyboardTreeNavigationAndCheckRespectLocks()
    {
        HierarchyModel model;
        model.Set({{-1, "Root", "root"}, {0, "Branch", "branch"}, {1, "Leaf", "leaf"}, {0, "Sibling", "sibling"}});
        CModelHierarchyView view;
        view.setModel(&model);
        show(view);
        view.setCurrentIndex(model.At(0));
        QTest::keyClick(&view, Qt::Key_Right);
        QVERIFY(view.isExpanded(model.At(0)));
        QCOMPARE(view.currentIndex(), model.At(0));
        QTest::keyClick(&view, Qt::Key_Right);
        QCOMPARE(view.currentIndex(), model.At(1));
        QTest::keyClick(&view, Qt::Key_Right);
        QTest::keyClick(&view, Qt::Key_Right);
        QCOMPARE(view.currentIndex(), model.At(2));
        QTest::keyClick(&view, Qt::Key_Left);
        QCOMPARE(view.currentIndex(), model.At(1));
        QTest::keyClick(&view, Qt::Key_Left);
        QVERIFY(!view.isExpanded(model.At(1)));
        QTest::keyClick(&view, Qt::Key_Down);
        QCOMPARE(view.currentIndex(), model.At(3));
        QSignalSpy changes(&model, &QAbstractItemModel::dataChanged);
        QTest::keyClick(&view, Qt::Key_Space);
        QCOMPARE(model.At(3).data(Qt::CheckStateRole).toInt(), int(Qt::Unchecked));
        QCOMPARE(changes.size(), 1);
        model.Lock(3, true);
        changes.clear();
        QTest::keyClick(&view, Qt::Key_Space);
        QCOMPARE(changes.size(), 0);
        QCOMPARE(model.At(3).data(Qt::CheckStateRole).toInt(), int(Qt::Unchecked));
        QSignalSpy activated(&view, &QAbstractItemView::activated);
        QTest::keyClick(&view, Qt::Key_Return);
        QCOMPARE(activated.size(), 1);
        QCOMPARE(qvariant_cast<QModelIndex>(activated.first().first()), model.At(3));
    }

    void mouseBranchCheckboxAndExtendedSelection()
    {
        HierarchyModel model;
        model.Set({{-1, "Root", "root"}, {0, "First", "first"}, {0, "Second", "second"}, {0, "Third", "third"}});
        CModelHierarchyView view;
        view.setModel(&model);
        show(view);
        const QRect root = view.visualRect(model.At(0));
        QTest::mouseClick(view.viewport(), Qt::LeftButton, {}, QPoint(root.left() / 2, root.center().y()));
        QVERIFY(view.isExpanded(model.At(0)));
        QSignalSpy clicks(&view, &QAbstractItemView::clicked);
        QTest::mouseClick(view.viewport(), Qt::LeftButton, {}, textPoint(view, model.At(1)));
        QTest::mouseClick(view.viewport(), Qt::LeftButton, Qt::ControlModifier, textPoint(view, model.At(3)));
        QCOMPARE(view.selectionModel()->selectedRows().size(), 2);
        QVERIFY(view.selectionModel()->isSelected(model.At(1)));
        QVERIFY(view.selectionModel()->isSelected(model.At(3)));
        QTest::mouseClick(view.viewport(), Qt::LeftButton, Qt::ShiftModifier, textPoint(view, model.At(1)));
        QCOMPARE(view.selectionModel()->selectedRows().size(), 3);
        QCOMPARE(clicks.size(), 3);
        QTest::mouseClick(view.viewport(), Qt::LeftButton, {}, checkPoint(view, model.At(2)));
        QCOMPARE(model.At(2).data(Qt::CheckStateRole).toInt(), int(Qt::Unchecked));
        QCOMPARE(clicks.size(), 4);
        model.Lock(2, true);
        QTest::mouseClick(view.viewport(), Qt::LeftButton, {}, checkPoint(view, model.At(2)));
        QCOMPARE(model.At(2).data(Qt::CheckStateRole).toInt(), int(Qt::Unchecked));
        QTest::keyClick(&view, Qt::Key_A, Qt::ControlModifier);
        QCOMPARE(view.selectionModel()->selectedRows().size(), 4);
        QTest::keyClick(&view, Qt::Key_Home, Qt::ControlModifier);
        QCOMPARE(view.currentIndex(), model.At(0));
        QCOMPARE(view.selectionModel()->selectedRows().size(), 4);
    }

    void synchronousSelectionRollbackKeepsCurrentAndFocus_data()
    {
        QTest::addColumn<int>("input");
        QTest::newRow("mouse") << 0;
        QTest::newRow("keyboard") << 1;
        QTest::newRow("accessible") << 2;
        QTest::newRow("checkbox") << 3;
    }

    void synchronousSelectionRollbackKeepsCurrentAndFocus()
    {
        QFETCH(int, input);
        HierarchyModel model;
        model.Set(HierarchyModel::Wide(3));
        CModelHierarchyView view;
        view.setModel(&model);
        show(view);
        view.setCurrentIndex(model.At(0));
        auto* tree = QAccessible::queryAccessibleInterface(&view);
        QVERIFY(tree);
        int rejected = 0;
        bool requestedCurrentWasSet = false;
        const auto connection = connect(view.selectionModel(), &QItemSelectionModel::selectionChanged, &view, [&] {
            if (view.selectionModel()->selectedRows() == QModelIndexList{model.At(0)}) return;
            ++rejected;
            requestedCurrentWasSet = view.currentIndex() == model.At(1);
            const QSignalBlocker blocker(view.selectionModel());
            view.selectionModel()->setCurrentIndex(model.At(0), QItemSelectionModel::ClearAndSelect);
        });
        QSignalSpy changes(&model, &QAbstractItemModel::dataChanged);
        if (input == 0) QTest::mouseClick(view.viewport(), Qt::LeftButton, {}, textPoint(view, model.At(1)));
        else if (input == 1) QTest::keyClick(&view, Qt::Key_Down);
        else if (input == 2) tree->child(1)->actionInterface()->doAction(QAccessibleActionInterface::pressAction());
        else QTest::mouseClick(view.viewport(), Qt::LeftButton, {}, checkPoint(view, model.At(1)));
        QCOMPARE(rejected, 1);
        QVERIFY(requestedCurrentWasSet);
        QCOMPARE(view.currentIndex(), model.At(0));
        QCOMPARE(view.selectionModel()->selectedRows(), QModelIndexList{model.At(0)});
        QCOMPARE(tree->focusChild(), tree->child(0));
        QVERIFY(tree->child(0)->state().selected);
        QVERIFY(!tree->child(1)->state().selected);
        QCOMPARE(tree->selectionInterface()->selectedItems(), (QList<QAccessibleInterface*>{tree->child(0)}));
        QCOMPARE(changes.size(), 0);
        disconnect(connection);
        QTest::mouseClick(view.viewport(), Qt::LeftButton, Qt::ShiftModifier, textPoint(view, model.At(2)));
        QCOMPARE(view.selectionModel()->selectedRows().size(), 3);
    }

    void expansionKeysAreTypedAndFilterIsRecomputedOnReset()
    {
        HierarchyModel model;
        model.Set({{-1, "Part", "same", "part"}, {0, "Needle", "a"},
                   {-1, "Warp", "same", "warp"}, {2, "Other", "b"}});
        CModelHierarchyView view;
        view.setModel(&model);
        show(view);
        view.restoreExpandedKeys({"part|same", "missing|id"});
        QVERIFY(view.isExpanded(model.At(0)));
        QVERIFY(!view.isExpanded(model.At(2)));
        QCOMPARE(view.expandedKeys(), QSet<QString>{"part|same"});
        view.setFilterText("Needle");
        QVERIFY(!view.visualRect(model.At(1)).isEmpty());
        QVERIFY(view.visualRect(model.At(2)).isEmpty());
        model.Set({{-1, "Part", "same", "part"}, {0, "Replaced", "a"},
                   {-1, "Warp", "same", "warp"}, {2, "Needle", "b"}});
        view.setFilterText("Needle");
        QVERIFY(view.visualRect(model.At(0)).isEmpty());
        QVERIFY(!view.visualRect(model.At(3)).isEmpty());
        QCOMPARE(view.expandedKeys(), QSet<QString>{"part|same"});
        view.setFilterText({});
        QVERIFY(view.isExpanded(model.At(0)));
        QVERIFY(!view.isExpanded(model.At(2)));
        QVERIFY(view.visualRect(model.At(3)).isEmpty());
        view.setExpanded(model.At(2), true);
        QCOMPARE(view.expandedKeys().size(), 2);
    }

    void accessibilityExposesCompleteHierarchyAndActions()
    {
        HierarchyModel model;
        model.Set({{-1, "Root", "root"}, {0, "Branch", "branch"}, {1, "Leaf", "leaf"}, {-1, "Other root", "other"}});
        CModelHierarchyView view;
        view.setAccessibleName("Model objects");
        view.setModel(&model);
        show(view);
        auto* tree = QAccessible::queryAccessibleInterface(&view);
        QVERIFY(tree);
        QCOMPARE(tree->role(), QAccessible::Tree);
        QCOMPARE(tree->text(QAccessible::Name), QString("Model objects"));
        QCOMPARE(tree->childCount(), 2);
        auto* root = tree->child(0);
        QVERIFY(root);
        QCOMPARE(root->parent(), tree);
        QCOMPARE(root->childCount(), 1);
        QVERIFY(root->state().collapsed);
        auto* branch = root->child(0);
        auto* leaf = branch->child(0);
        QVERIFY(leaf);
        QCOMPARE(leaf->parent(), branch);
        QCOMPARE(branch->indexOfChild(leaf), 0);
        QCOMPARE(tree->indexOfChild(branch), -1);
        QVERIFY(leaf->state().invisible);
        auto* table = tree->tableInterface();
        QVERIFY(table);
        QCOMPARE(table->rowCount(), 4);
        QCOMPARE(table->cellAt(2, 0), leaf);
        QCOMPARE(leaf->tableCellInterface()->rowIndex(), 2);
        auto* actions = root->actionInterface();
        QVERIFY(actions);
        QVERIFY(actions->actionNames().contains("expand"));
        actions->doAction("expand");
        QVERIFY(view.isExpanded(model.At(0)));
        QSignalSpy clicks(&view, &QAbstractItemView::clicked);
        leaf->actionInterface()->doAction(QAccessibleActionInterface::pressAction());
        QCOMPARE(clicks.size(), 1);
        QCOMPARE(view.currentIndex(), model.At(2));
        QVERIFY(view.isExpanded(model.At(1)));
        QVERIFY(leaf->state().selected);
        QVERIFY(!leaf->state().invisible);
        QVERIFY(!leaf->rect().isEmpty());
        QCOMPARE(tree->childAt(leaf->rect().center().x(), leaf->rect().center().y()), leaf);
        leaf->actionInterface()->doAction(QAccessibleActionInterface::toggleAction());
        QCOMPARE(model.At(2).data(Qt::CheckStateRole).toInt(), int(Qt::Unchecked));
        model.Lock(2, true);
        QVERIFY(leaf->state().readOnly);
        QVERIFY(!leaf->actionInterface()->actionNames().contains(QAccessibleActionInterface::toggleAction()));
        leaf->actionInterface()->doAction(QAccessibleActionInterface::toggleAction());
        QCOMPARE(model.At(2).data(Qt::CheckStateRole).toInt(), int(Qt::Unchecked));
        auto* selection = tree->selectionInterface();
        QVERIFY(selection);
        QCOMPARE(selection->selectedItemCount(), 1);
        QCOMPARE(selection->selectedItem(0), leaf);
        QVERIFY(selection->select(tree->child(1)));
        QCOMPARE(table->selectedRows(), QList<int>({2, 3}));
        QVERIFY(selection->clear());
        QCOMPARE(table->selectedCellCount(), 0);
        root->actionInterface()->doAction("collapse");
        QVERIFY(!view.isExpanded(model.At(0)));
        QCOMPARE(root->childCount(), 1);
        QCOMPARE(branch->child(0), leaf);
        QVERIFY(leaf->state().invisible);
    }

    void accessibilityDeepTraversalAndCacheLifetimeAreIterative()
    {
        constexpr int depth = 12000;
        HierarchyModel model;
        model.Set(HierarchyModel::Chain(depth));
        auto view = std::make_unique<CModelHierarchyView>();
        view->setModel(&model);
        show(*view);
        auto* tree = QAccessible::queryAccessibleInterface(view.get());
        QVERIFY(tree);
        auto* parent = tree;
        QAccessible::Id leafId = 0;
        for (int i = 0; i < depth; ++i) {
            QCOMPARE(parent->childCount(), 1);
            auto* child = parent->child(0);
            QVERIFY(child);
            QCOMPARE(child->role(), QAccessible::TreeItem);
            QCOMPARE(child->parent(), parent);
            QCOMPARE(parent->indexOfChild(child), 0);
            QCOMPARE(child->text(QAccessible::Name), QStringLiteral("Node %1").arg(i));
            parent = child;
        }
        QCOMPARE(parent->childCount(), 0);
        leafId = QAccessible::uniqueId(parent);
        QVERIFY(QAccessible::accessibleInterface(leafId));
        model.Set(HierarchyModel::Wide(2));
        QVERIFY(!QAccessible::accessibleInterface(leafId));
        QCOMPARE(tree->childCount(), 2);
        auto* item = tree->child(1);
        const auto itemId = QAccessible::uniqueId(item);
        const auto treeId = QAccessible::uniqueId(tree);
        QVERIFY(itemId);
        view.reset();
        QVERIFY(!QAccessible::accessibleInterface(itemId));
        QVERIFY(!QAccessible::accessibleInterface(treeId));
    }
};

QTEST_MAIN(HierarchyViewTests)
#include "hierarchy_view_tests.moc"
