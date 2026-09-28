#ifndef CMODELHIERARCHYVIEW_H
#define CMODELHIERARCHYVIEW_H

#include <QAbstractItemView>
#include <QHash>
#include <QSet>
#include <QStyleOptionViewItem>

class CHierarchyAccessible;
class CHierarchyAccessibleItem;
class QAccessibleInterface;

class CModelHierarchyView : public QAbstractItemView
{
    Q_OBJECT
public:
    explicit CModelHierarchyView(QWidget* parent = nullptr);
    ~CModelHierarchyView() override;
    void setModel(QAbstractItemModel* model) override;
    QModelIndex indexAt(const QPoint& point) const override;
    QRect visualRect(const QModelIndex& index) const override;
    void scrollTo(const QModelIndex& index, ScrollHint hint = EnsureVisible) override;
    bool isExpanded(const QModelIndex& index) const;
    void setExpanded(const QModelIndex& index, bool expanded);
    void expandToDepth(int depth);
    void setFilterText(const QString& text);
    QSet<QString> expandedKeys() const;
    void restoreExpandedKeys(const QSet<QString>& keys);
    void reset() override;
    void doItemsLayout() override;
    void selectAll() override;
    void keyboardSearch(const QString& text) override;
    void BeginDisplayNameUpdate();
    void EndDisplayNameUpdate();

protected:
    QModelIndex moveCursor(CursorAction action, Qt::KeyboardModifiers modifiers) override;
    int horizontalOffset() const override;
    int verticalOffset() const override;
    bool isIndexHidden(const QModelIndex& index) const override;
    void setSelection(const QRect& rect, QItemSelectionModel::SelectionFlags command) override;
    QRegion visualRegionForSelection(const QItemSelection& selection) const override;
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    bool viewportEvent(QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void changeEvent(QEvent* event) override;
    void scrollContentsBy(int dx, int dy) override;
    void updateGeometries() override;
    void dataChanged(const QModelIndex& first, const QModelIndex& last, const QList<int>& roles = {}) override;
    void selectionChanged(const QItemSelection& selected, const QItemSelection& deselected) override;
    void currentChanged(const QModelIndex& current, const QModelIndex& previous) override;

private:
    friend class CHierarchyAccessible;
    friend class CHierarchyAccessibleItem;
    struct Node {
        QModelIndex index;
        int parent = -1;
        int depth = 0;
        int width = 0;
        bool children = false;
        QString key;
    };
    void Rebuild();
    void LayoutVisible();
    void InvalidateAccessibleItems();
    QAccessibleInterface* AccessibleItem(int node) const;
    QStyleOptionViewItem Option(const QModelIndex& index, const QRect& rect) const;
    QRect BranchRect(int node) const;
    QRect CheckRect(const QModelIndex& index) const;
    void Choose(const QModelIndex& index, Qt::KeyboardModifiers modifiers);
    void ToggleCheck(const QModelIndex& index);
    int NodeFor(const QModelIndex& index) const;
    QVector<Node> m_nodes;
    QHash<QModelIndex, int> m_lookup;
    QVector<int> m_visible, m_visualRows;
    QVector<quint8> m_matches;
    QSet<QString> m_expanded;
    QString m_filter;
    int m_rowHeight = 1, m_indentation = 1, m_contentWidth = 0;
    QModelIndex m_anchor, m_pressed;
    bool m_pressedCheck = false, m_pressedBranch = false;
    bool m_rebuilding = false, m_geometryUpdating = false;
    int m_nameUpdateDepth = 0;
    bool m_nameUpdatePending = false;
    quint64 m_generation = 0;
    mutable QHash<int, quint32> m_accessibleItems;
    QVector<QMetaObject::Connection> m_connections;
};

#endif
