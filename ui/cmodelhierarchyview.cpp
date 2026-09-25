#include "cmodelhierarchyview.h"

#include <QAccessible>
#include <QAccessibleWidget>
#include <QApplication>
#include <QHelpEvent>
#include <QIcon>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QPersistentModelIndex>
#include <QScopedValueRollback>
#include <QScrollBar>
#include <QStyledItemDelegate>
#include <QToolTip>
#include <algorithm>
#include <limits>

namespace {
int bounded(qint64 value)
{
    return int(std::clamp(value, qint64(std::numeric_limits<int>::min()), qint64(std::numeric_limits<int>::max())));
}
QAccessibleInterface* hierarchyFactory(const QString&, QObject* object);
}

CModelHierarchyView::CModelHierarchyView(QWidget* parent) : QAbstractItemView(parent)
{
    static const bool installed = [] { QAccessible::installFactory(hierarchyFactory); return true; }();
    Q_UNUSED(installed);
    setSelectionMode(ExtendedSelection);
    setSelectionBehavior(SelectRows);
    setEditTriggers(NoEditTriggers);
    setVerticalScrollMode(ScrollPerPixel);
    setHorizontalScrollMode(ScrollPerPixel);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setItemDelegate(new QStyledItemDelegate(this));
}

CModelHierarchyView::~CModelHierarchyView()
{
    InvalidateAccessibleItems();
}

void CModelHierarchyView::setModel(QAbstractItemModel* source)
{
    for (const auto& connection : m_connections) disconnect(connection);
    m_connections.clear();
    InvalidateAccessibleItems();
    m_nodes.clear();
    m_lookup.clear();
    m_visible.clear();
    m_visualRows.clear();
    QAbstractItemView::setModel(source);
    if (source) {
        m_connections.append(connect(source, &QAbstractItemModel::layoutChanged, this, [this] { Rebuild(); }));
        m_connections.append(connect(source, &QAbstractItemModel::rowsInserted, this, [this] { Rebuild(); }));
        m_connections.append(connect(source, &QAbstractItemModel::rowsRemoved, this, [this] { Rebuild(); }));
        m_connections.append(connect(source, &QAbstractItemModel::rowsMoved, this, [this] { Rebuild(); }));
        m_connections.append(connect(source, &QObject::destroyed, this, [this] {
            InvalidateAccessibleItems();
            m_nodes.clear();
            m_lookup.clear();
            LayoutVisible();
        }));
    }
    Rebuild();
}

int CModelHierarchyView::NodeFor(const QModelIndex& index) const
{
    return index.isValid() && index.model() == model() ? m_lookup.value(index, -1) : -1;
}

QStyleOptionViewItem CModelHierarchyView::Option(const QModelIndex& index, const QRect& rect) const
{
    QStyleOptionViewItem option;
    initViewItemOption(&option);
    option.rect = rect;
    option.index = index;
    option.text = index.data(Qt::DisplayRole).toString();
    option.features |= QStyleOptionViewItem::HasDisplay;
    const auto font = index.data(Qt::FontRole);
    if (font.canConvert<QFont>()) option.font = font.value<QFont>().resolve(option.font);
    option.fontMetrics = QFontMetrics(option.font);
    const auto icon = index.data(Qt::DecorationRole);
    if (icon.canConvert<QIcon>()) {
        option.icon = icon.value<QIcon>();
        option.features |= QStyleOptionViewItem::HasDecoration;
    }
    const auto check = index.data(Qt::CheckStateRole);
    if (check.isValid()) {
        option.features |= QStyleOptionViewItem::HasCheckIndicator;
        option.checkState = Qt::CheckState(check.toInt());
    }
    option.state.setFlag(QStyle::State_Enabled, isEnabled() && index.flags().testFlag(Qt::ItemIsEnabled));
    option.state.setFlag(QStyle::State_Selected, selectionModel() && selectionModel()->isSelected(index));
    option.state.setFlag(QStyle::State_HasFocus, hasFocus() && currentIndex() == index);
    option.state.setFlag(QStyle::State_Active, window()->isActiveWindow());
    return option;
}

void CModelHierarchyView::Rebuild()
{
    if (m_rebuilding) return;
    const QScopedValueRollback<bool> rebuilding(m_rebuilding, true);
    InvalidateAccessibleItems();
    m_nodes.clear();
    m_lookup.clear();
    m_pressed = {};
    m_indentation = std::max(1, style()->pixelMetric(QStyle::PM_TreeViewIndentation, nullptr, this));
    const int verticalMargin = style()->pixelMetric(QStyle::PM_FocusFrameVMargin, nullptr, this);
    m_rowHeight = std::max({fontMetrics().height(), style()->pixelMetric(QStyle::PM_IndicatorHeight, nullptr, this),
                           style()->pixelMetric(QStyle::PM_SmallIconSize, nullptr, this)}) + verticalMargin * 2;
    m_rowHeight = std::max(1, m_rowHeight);
    m_contentWidth = 0;
    struct Pending { QModelIndex index; int parent; int depth; };
    QVector<Pending> pending;
    if (model()) for (int row = model()->rowCount(rootIndex()) - 1; row >= 0; --row)
        pending.append({model()->index(row, 0, rootIndex()), -1, 0});
    QSet<QString> validExpanded;
    while (!pending.isEmpty()) {
        const auto next = pending.takeLast();
        if (!next.index.isValid() || m_lookup.contains(next.index)) continue;
        const int number = m_nodes.size();
        const int children = model()->rowCount(next.index);
        const QString key = next.index.data(Qt::UserRole + 1).toString() + QLatin1Char('|') + next.index.data(Qt::UserRole).toString();
        const auto option = Option(next.index, {});
        const QSize hint = itemDelegateForIndex(next.index)->sizeHint(option, next.index);
        m_rowHeight = std::max(m_rowHeight, hint.height());
        m_nodes.append({next.index, next.parent, next.depth, std::max(1, hint.width()), children > 0, key});
        m_lookup.insert(next.index, number);
        if (children && m_expanded.contains(key)) validExpanded.insert(key);
        for (int row = children - 1; row >= 0; --row)
            pending.append({model()->index(row, 0, next.index), number, next.depth + 1});
    }
    m_expanded.swap(validExpanded);
    if (!m_lookup.contains(m_anchor)) m_anchor = {};
    LayoutVisible();
    if (QAccessible::isActive()) {
        QAccessibleEvent event(this, QAccessible::ObjectReorder);
        QAccessible::updateAccessibility(&event);
    }
}

void CModelHierarchyView::LayoutVisible()
{
    m_matches.fill(m_filter.isEmpty(), m_nodes.size());
    if (!m_filter.isEmpty()) {
        for (int index = m_nodes.size() - 1; index >= 0; --index) {
            const auto& node = m_nodes[index];
            m_matches[index] = m_matches[index] || node.index.data(Qt::DisplayRole).toString().contains(m_filter, Qt::CaseInsensitive) ||
                node.index.data(Qt::UserRole).toString().contains(m_filter, Qt::CaseInsensitive);
            if (m_matches[index] && node.parent >= 0) m_matches[node.parent] = true;
        }
    }
    m_visible.clear();
    m_contentWidth = 0;
    m_visualRows.fill(-1, m_nodes.size());
    for (int index = 0; index < m_nodes.size(); ++index) {
        const auto& node = m_nodes[index];
        if (!m_matches[index] || (node.parent >= 0 &&
            (m_visualRows[node.parent] < 0 || !isExpanded(m_nodes[node.parent].index)))) continue;
        m_visualRows[index] = m_visible.size();
        m_visible.append(index);
        m_contentWidth = std::max(m_contentWidth, bounded((qint64(node.depth) + 1) * m_indentation + node.width));
    }
    updateGeometries();
    viewport()->update();
}

void CModelHierarchyView::reset()
{
    m_anchor = {};
    InvalidateAccessibleItems();
    m_nodes.clear();
    m_lookup.clear();
    m_visible.clear();
    m_visualRows.clear();
    QAbstractItemView::reset();
    Rebuild();
}

void CModelHierarchyView::doItemsLayout() { Rebuild(); }
int CModelHierarchyView::horizontalOffset() const { return horizontalScrollBar()->value(); }
int CModelHierarchyView::verticalOffset() const { return verticalScrollBar()->value(); }

QModelIndex CModelHierarchyView::indexAt(const QPoint& point) const
{
    if (!viewport()->rect().contains(point)) return {};
    const qint64 row = (qint64(point.y()) + verticalOffset()) / m_rowHeight;
    return row >= 0 && row < m_visible.size() ? m_nodes[m_visible[int(row)]].index : QModelIndex();
}

QRect CModelHierarchyView::visualRect(const QModelIndex& index) const
{
    const int node = NodeFor(index);
    if (node < 0 || node >= m_visualRows.size() || m_visualRows[node] < 0) return {};
    const int x = bounded(qint64(m_nodes[node].depth + 1) * m_indentation - horizontalOffset());
    const int y = bounded(qint64(m_visualRows[node]) * m_rowHeight - verticalOffset());
    const QRect logical(x, y, std::max(m_nodes[node].width, viewport()->width() - x), m_rowHeight);
    return QStyle::visualRect(layoutDirection(), viewport()->rect(), logical);
}

QRect CModelHierarchyView::BranchRect(int node) const
{
    const QRect item = visualRect(m_nodes[node].index);
    if (item.isEmpty()) return {};
    const int x = isRightToLeft() ? item.right() + 1 : item.left() - m_indentation;
    return QRect(x, item.y(), m_indentation, item.height());
}

QRect CModelHierarchyView::CheckRect(const QModelIndex& index) const
{
    const auto option = Option(index, visualRect(index));
    return style()->subElementRect(QStyle::SE_ItemViewItemCheckIndicator, &option, this);
}

bool CModelHierarchyView::isIndexHidden(const QModelIndex& index) const
{
    const int node = NodeFor(index);
    return node < 0 || node >= m_visualRows.size() || m_visualRows[node] < 0;
}

bool CModelHierarchyView::isExpanded(const QModelIndex& index) const
{
    const int node = NodeFor(index);
    return node >= 0 && m_nodes[node].children && (m_expanded.contains(m_nodes[node].key) ||
        (!m_filter.isEmpty() && node < m_matches.size() && m_matches[node]));
}

void CModelHierarchyView::setExpanded(const QModelIndex& index, bool expanded)
{
    const int node = NodeFor(index);
    if (node < 0 || !m_nodes[node].children || m_expanded.contains(m_nodes[node].key) == expanded) return;
    if (expanded) m_expanded.insert(m_nodes[node].key);
    else m_expanded.remove(m_nodes[node].key);
    LayoutVisible();
    if (QAccessible::isActive()) {
        QAccessible::State state;
        state.expanded = state.collapsed = true;
        QAccessibleStateChangeEvent event(AccessibleItem(node), state);
        QAccessible::updateAccessibility(&event);
    }
}

void CModelHierarchyView::expandToDepth(int depth)
{
    m_expanded.clear();
    for (const auto& node : m_nodes)
        if (node.children && (depth < 0 || node.depth <= depth)) m_expanded.insert(node.key);
    LayoutVisible();
}

void CModelHierarchyView::setFilterText(const QString& text)
{
    m_filter = text.trimmed();
    LayoutVisible();
    if (!isIndexHidden(currentIndex())) scrollTo(currentIndex());
    else verticalScrollBar()->setValue(0);
}

QSet<QString> CModelHierarchyView::expandedKeys() const { return m_expanded; }
void CModelHierarchyView::restoreExpandedKeys(const QSet<QString>& keys)
{
    m_expanded.clear();
    for (const auto& node : m_nodes) if (node.children && keys.contains(node.key)) m_expanded.insert(node.key);
    LayoutVisible();
}

void CModelHierarchyView::scrollTo(const QModelIndex& index, ScrollHint hint)
{
    const int node = NodeFor(index);
    if (node < 0) return;
    bool changed = false;
    for (int parent = m_nodes[node].parent; parent >= 0; parent = m_nodes[parent].parent) {
        if (!isExpanded(m_nodes[parent].index)) { m_expanded.insert(m_nodes[parent].key); changed = true; }
    }
    if (changed) LayoutVisible();
    if (isIndexHidden(index)) return;
    const qint64 top = qint64(m_visualRows[node]) * m_rowHeight;
    qint64 vertical = verticalOffset();
    if (hint == PositionAtTop) vertical = top;
    else if (hint == PositionAtBottom) vertical = top + m_rowHeight - viewport()->height();
    else if (hint == PositionAtCenter) vertical = top + (m_rowHeight - viewport()->height()) / 2;
    else if (top < vertical) vertical = top;
    else if (top + m_rowHeight > vertical + viewport()->height()) vertical = top + m_rowHeight - viewport()->height();
    verticalScrollBar()->setValue(bounded(vertical));
    const qint64 left = qint64(m_nodes[node].depth) * m_indentation;
    const qint64 right = left + m_indentation + m_nodes[node].width;
    if (left < horizontalOffset() || right - left >= viewport()->width()) horizontalScrollBar()->setValue(bounded(left));
    else if (right > qint64(horizontalOffset()) + viewport()->width()) horizontalScrollBar()->setValue(bounded(right - viewport()->width()));
}

void CModelHierarchyView::paintEvent(QPaintEvent* event)
{
    QPainter painter(viewport());
    painter.setClipRegion(event->region());
    painter.fillRect(event->rect(), palette().brush(QPalette::Base));
    const int first = std::max(0, bounded((qint64(event->rect().top()) + verticalOffset()) / m_rowHeight));
    const int last = std::min(int(m_visible.size()) - 1, bounded((qint64(event->rect().bottom()) + verticalOffset()) / m_rowHeight));
    for (int row = first; row <= last; ++row) {
        const int node = m_visible[row];
        const auto index = m_nodes[node].index;
        const QRect rect = visualRect(index);
        auto option = Option(index, rect);
        if (alternatingRowColors() && row % 2) option.features |= QStyleOptionViewItem::Alternate;
        itemDelegateForIndex(index)->paint(&painter, option, index);
        if (m_nodes[node].children) {
            QStyleOption branch;
            branch.initFrom(this);
            const QRect hitbox = BranchRect(node);
            const int extent = std::min({hitbox.width(), hitbox.height(),
                style()->pixelMetric(QStyle::PM_SmallIconSize, &branch, this)});
            branch.rect = QStyle::alignedRect(layoutDirection(), Qt::AlignCenter, QSize(extent, extent), hitbox);
            branch.state.setFlag(QStyle::State_Enabled, option.state.testFlag(QStyle::State_Enabled));
            branch.palette = option.palette;
            for (const auto group : {QPalette::Active, QPalette::Inactive, QPalette::Disabled})
                branch.palette.setColor(group, QPalette::ButtonText, option.palette.color(group, QPalette::Text));
            const auto arrow = isExpanded(index) ? QStyle::PE_IndicatorArrowDown :
                (isRightToLeft() ? QStyle::PE_IndicatorArrowLeft : QStyle::PE_IndicatorArrowRight);
            style()->drawPrimitive(arrow, &branch, &painter, this);
        }
    }
}

void CModelHierarchyView::Choose(const QModelIndex& index, Qt::KeyboardModifiers modifiers)
{
    if (!selectionModel() || !index.isValid() || !index.flags().testFlag(Qt::ItemIsSelectable) ||
        !index.flags().testFlag(Qt::ItemIsEnabled)) return;
    const bool control = modifiers.testFlag(Qt::ControlModifier) || modifiers.testFlag(Qt::MetaModifier);
    if (selectionMode() == NoSelection) { selectionModel()->setCurrentIndex(index, QItemSelectionModel::NoUpdate); return; }
    const int anchorNode = NodeFor(m_anchor);
    const int node = NodeFor(index);
    QItemSelection selection;
    auto command = QItemSelectionModel::ClearAndSelect;
    if (modifiers.testFlag(Qt::ShiftModifier) && selectionMode() == ExtendedSelection && anchorNode >= 0 &&
        m_visualRows[anchorNode] >= 0 && node >= 0 && m_visualRows[node] >= 0) {
        const int first = std::min(m_visualRows[anchorNode], m_visualRows[node]);
        const int last = std::max(m_visualRows[anchorNode], m_visualRows[node]);
        for (int row = first; row <= last; ++row) {
            const auto item = m_nodes[m_visible[row]].index;
            if (item.flags().testFlag(Qt::ItemIsSelectable)) selection.select(item, item);
        }
        if (control) command = QItemSelectionModel::Select;
    } else {
        selection.select(index, index);
        if (control && selectionMode() != SingleSelection) command = QItemSelectionModel::Toggle;
        m_anchor = index;
    }
    const QPersistentModelIndex requested(index);
    const QPointer<QItemSelectionModel> target(selectionModel());
    target->setCurrentIndex(requested, QItemSelectionModel::NoUpdate);
    if (!requested.isValid() || !target || target != selectionModel()) return;
    target->select(selection, command);
    if (currentIndex() != requested) m_anchor = currentIndex();
}

void CModelHierarchyView::ToggleCheck(const QModelIndex& index)
{
    if (!model() || !index.isValid() || !index.flags().testFlag(Qt::ItemIsEnabled) ||
        !index.flags().testFlag(Qt::ItemIsUserCheckable) || index.data(Qt::UserRole + 2).toBool()) return;
    model()->setData(index, index.data(Qt::CheckStateRole).toInt() == Qt::Checked ? Qt::Unchecked : Qt::Checked, Qt::CheckStateRole);
}

void CModelHierarchyView::mousePressEvent(QMouseEvent* event)
{
    m_pressed = indexAt(event->position().toPoint());
    m_pressedCheck = m_pressedBranch = false;
    if (event->button() == Qt::LeftButton) {
        setFocus(Qt::MouseFocusReason);
        const int node = NodeFor(m_pressed);
        if (node >= 0) {
            m_pressedBranch = m_nodes[node].children && BranchRect(node).contains(event->position().toPoint());
            m_pressedCheck = m_pressed.data(Qt::CheckStateRole).isValid() && CheckRect(m_pressed).contains(event->position().toPoint());
            if (m_pressedBranch) setExpanded(m_pressed, !isExpanded(m_pressed));
            else Choose(m_pressed, event->modifiers());
            emit pressed(m_pressed);
        } else if (!event->modifiers().testFlag(Qt::ControlModifier)) clearSelection();
    } else if (event->button() == Qt::RightButton && m_pressed.isValid() && !selectionModel()->isSelected(m_pressed)) {
        Choose(m_pressed, {});
    }
    event->accept();
}

void CModelHierarchyView::mouseMoveEvent(QMouseEvent* event)
{
    const auto index = indexAt(event->position().toPoint());
    if (index.isValid()) emit entered(index);
    else emit viewportEntered();
    event->accept();
}

void CModelHierarchyView::mouseReleaseEvent(QMouseEvent* event)
{
    const auto index = indexAt(event->position().toPoint());
    if (event->button() == Qt::LeftButton && index.isValid() && index == m_pressed && !m_pressedBranch) {
        if (m_pressedCheck && selectionModel() && selectionModel()->isSelected(index) &&
            CheckRect(index).contains(event->position().toPoint())) ToggleCheck(index);
        emit clicked(index);
    }
    m_pressed = {};
    event->accept();
}

void CModelHierarchyView::mouseDoubleClickEvent(QMouseEvent* event)
{
    const auto index = indexAt(event->position().toPoint());
    if (event->button() == Qt::LeftButton && index.isValid() && !CheckRect(index).contains(event->position().toPoint())) {
        setExpanded(index, !isExpanded(index));
        emit doubleClicked(index);
        emit activated(index);
    }
    event->accept();
}

QModelIndex CModelHierarchyView::moveCursor(CursorAction action, Qt::KeyboardModifiers)
{
    if (m_visible.isEmpty()) return {};
    const int node = NodeFor(currentIndex());
    if (node < 0 || m_visualRows[node] < 0)
        return m_nodes[action == MoveEnd ? m_visible.last() : m_visible.first()].index;
    int row = m_visualRows[node];
    const int page = std::max(1, viewport()->height() / m_rowHeight);
    if (action == MoveUp || action == MovePrevious) --row;
    else if (action == MoveDown || action == MoveNext) ++row;
    else if (action == MoveHome) row = 0;
    else if (action == MoveEnd) row = m_visible.size() - 1;
    else if (action == MovePageUp) row -= page;
    else if (action == MovePageDown) row += page;
    else if (action == MoveLeft && node >= 0) {
        if (isExpanded(currentIndex())) setExpanded(currentIndex(), false);
        else if (m_nodes[node].parent >= 0) return m_nodes[m_nodes[node].parent].index;
        return currentIndex();
    } else if (action == MoveRight && node >= 0) {
        if (!isExpanded(currentIndex())) { setExpanded(currentIndex(), true); return currentIndex(); }
        if (row + 1 < m_visible.size() && m_nodes[m_visible[row + 1]].parent == node) ++row;
    }
    row = std::clamp(row, 0, int(m_visible.size()) - 1);
    return m_nodes[m_visible[row]].index;
}

void CModelHierarchyView::keyPressEvent(QKeyEvent* event)
{
    if (event->matches(QKeySequence::SelectAll)) { selectAll(); event->accept(); return; }
    if (event->key() == Qt::Key_Space) { ToggleCheck(currentIndex()); event->accept(); return; }
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        if (currentIndex().isValid()) emit activated(currentIndex());
        event->accept();
        return;
    }
    CursorAction action;
    switch (event->key()) {
    case Qt::Key_Up: action = MoveUp; break;
    case Qt::Key_Down: action = MoveDown; break;
    case Qt::Key_Left: action = isRightToLeft() ? MoveRight : MoveLeft; break;
    case Qt::Key_Right: action = isRightToLeft() ? MoveLeft : MoveRight; break;
    case Qt::Key_Home: action = MoveHome; break;
    case Qt::Key_End: action = MoveEnd; break;
    case Qt::Key_PageUp: action = MovePageUp; break;
    case Qt::Key_PageDown: action = MovePageDown; break;
    default: QAbstractItemView::keyPressEvent(event); return;
    }
    const auto next = moveCursor(action, event->modifiers());
    if (selectionModel() && event->modifiers().testFlag(Qt::ControlModifier) && !event->modifiers().testFlag(Qt::ShiftModifier))
        selectionModel()->setCurrentIndex(next, QItemSelectionModel::NoUpdate);
    else Choose(next, event->modifiers());
    scrollTo(currentIndex());
    event->accept();
}

void CModelHierarchyView::keyboardSearch(const QString& text)
{
    if (text.isEmpty() || m_visible.isEmpty()) return;
    const int node = NodeFor(currentIndex());
    const int start = node >= 0 ? m_visualRows.value(node, -1) : -1;
    for (int offset = 1; offset <= m_visible.size(); ++offset) {
        const auto index = m_nodes[m_visible[(start + offset) % m_visible.size()]].index;
        if (!index.data().toString().startsWith(text, Qt::CaseInsensitive)) continue;
        Choose(index, {});
        scrollTo(index);
        return;
    }
}

void CModelHierarchyView::selectAll()
{
    if (!selectionModel() || selectionMode() == NoSelection) return;
    QItemSelection selection;
    for (int node : m_visible) {
        const auto index = m_nodes[node].index;
        if (index.flags().testFlag(Qt::ItemIsSelectable)) selection.select(index, index);
    }
    selectionModel()->select(selection, QItemSelectionModel::ClearAndSelect);
}

void CModelHierarchyView::setSelection(const QRect& rect, QItemSelectionModel::SelectionFlags command)
{
    if (!selectionModel()) return;
    QItemSelection selection;
    const QRect area = rect.normalized();
    for (int node : m_visible) {
        const auto index = m_nodes[node].index;
        if (visualRect(index).intersects(area) && index.flags().testFlag(Qt::ItemIsSelectable)) selection.select(index, index);
    }
    selectionModel()->select(selection, command);
}

QRegion CModelHierarchyView::visualRegionForSelection(const QItemSelection& selection) const
{
    QRegion region;
    for (const auto& range : selection) for (int row = range.top(); row <= range.bottom(); ++row) {
        const QRect rect = visualRect(model()->index(row, 0, range.parent())).intersected(viewport()->rect());
        if (!rect.isEmpty()) region += rect;
    }
    return region;
}

void CModelHierarchyView::updateGeometries()
{
    if (m_geometryUpdating) return;
    const QScopedValueRollback<bool> guard(m_geometryUpdating, true);
    verticalScrollBar()->setSingleStep(m_rowHeight);
    verticalScrollBar()->setPageStep(viewport()->height());
    verticalScrollBar()->setRange(0, std::max(0, bounded(qint64(m_visible.size()) * m_rowHeight - viewport()->height())));
    horizontalScrollBar()->setSingleStep(m_indentation);
    horizontalScrollBar()->setPageStep(viewport()->width());
    horizontalScrollBar()->setRange(0, std::max(0, m_contentWidth - viewport()->width()));
}

bool CModelHierarchyView::viewportEvent(QEvent* event)
{
    if (event->type() == QEvent::ToolTip) {
        auto* help = static_cast<QHelpEvent*>(event);
        const auto index = indexAt(help->pos());
        const QString text = index.data(Qt::ToolTipRole).toString();
        if (!text.isEmpty()) QToolTip::showText(help->globalPos(), QStringLiteral("<qt>") + text.toHtmlEscaped() + QStringLiteral("</qt>"), viewport(), visualRect(index));
        else QToolTip::hideText();
        event->accept();
        return true;
    }
    return QAbstractItemView::viewportEvent(event);
}

void CModelHierarchyView::resizeEvent(QResizeEvent* event) { QAbstractItemView::resizeEvent(event); updateGeometries(); }
void CModelHierarchyView::scrollContentsBy(int, int) { viewport()->update(); }
void CModelHierarchyView::changeEvent(QEvent* event)
{
    QAbstractItemView::changeEvent(event);
    if (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange ||
        event->type() == QEvent::LayoutDirectionChange) Rebuild();
}

void CModelHierarchyView::dataChanged(const QModelIndex& first, const QModelIndex&, const QList<int>& roles)
{
    const bool presentationOnly = !roles.isEmpty() && std::all_of(roles.cbegin(), roles.cend(), [&](int role) {
        return role == Qt::CheckStateRole || role == Qt::ToolTipRole || role == Qt::UserRole + 2 ||
            (role == Qt::FontRole && roles.contains(Qt::UserRole + 2));
    });
    if (!presentationOnly) Rebuild();
    else {
        viewport()->update();
        const int node = NodeFor(first);
        if (QAccessible::isActive() && node >= 0) {
            QAccessible::State state;
            state.checked = state.checkable = state.readOnly = true;
            QAccessibleStateChangeEvent event(AccessibleItem(node), state);
            QAccessible::updateAccessibility(&event);
        }
    }
}

void CModelHierarchyView::selectionChanged(const QItemSelection&, const QItemSelection&)
{
    viewport()->update();
    if (QAccessible::isActive()) {
        QAccessibleEvent event(this, QAccessible::SelectionWithin);
        QAccessible::updateAccessibility(&event);
    }
}

void CModelHierarchyView::currentChanged(const QModelIndex& current, const QModelIndex& previous)
{
    QAbstractItemView::currentChanged(current, previous);
    viewport()->update();
    const int node = NodeFor(current);
    if (QAccessible::isActive() && node >= 0) {
        QAccessibleEvent event(AccessibleItem(node), QAccessible::Focus);
        QAccessible::updateAccessibility(&event);
    }
}

class CHierarchyAccessibleItem final : public QAccessibleInterface, public QAccessibleActionInterface,
                                       public QAccessibleTableCellInterface
{
public:
    CHierarchyAccessibleItem(CModelHierarchyView* view, int node)
        : m_view(view), m_node(node), m_generation(view->m_generation) {}
    bool isValid() const override
    {
        return m_view && m_generation == m_view->m_generation && m_node >= 0 && m_node < m_view->m_nodes.size();
    }
    QObject* object() const override { return nullptr; }
    QWindow* window() const override { return isValid() ? m_view->window()->windowHandle() : nullptr; }
    QModelIndex index() const { return isValid() ? m_view->m_nodes[m_node].index : QModelIndex(); }
    QAccessibleInterface* parent() const override
    {
        if (!isValid()) return nullptr;
        const int parent = m_view->m_nodes[m_node].parent;
        return parent >= 0 ? m_view->AccessibleItem(parent) : QAccessible::queryAccessibleInterface(m_view);
    }
    int childCount() const override { return isValid() ? m_view->model()->rowCount(index()) : 0; }
    QAccessibleInterface* child(int row) const override
    {
        if (!isValid() || row < 0 || row >= childCount()) return nullptr;
        return m_view->AccessibleItem(m_view->NodeFor(m_view->model()->index(row, 0, index())));
    }
    int indexOfChild(const QAccessibleInterface* child) const override
    {
        const auto* item = dynamic_cast<const CHierarchyAccessibleItem*>(child);
        return isValid() && item && item->isValid() && item->m_view == m_view &&
            item->index().parent() == index() ? item->index().row() : -1;
    }
    QAccessibleInterface* childAt(int x, int y) const override
    {
        if (!isValid()) return nullptr;
        const int node = m_view->NodeFor(m_view->indexAt(m_view->viewport()->mapFromGlobal(QPoint(x, y))));
        if (node < 0 || node == m_node) return nullptr;
        for (int parent = m_view->m_nodes[node].parent; parent >= 0; parent = m_view->m_nodes[parent].parent)
            if (parent == m_node) return m_view->AccessibleItem(node);
        return nullptr;
    }
    QString text(QAccessible::Text type) const override
    {
        if (!isValid()) return {};
        if (type == QAccessible::Name) return index().data(Qt::DisplayRole).toString();
        if (type == QAccessible::Description) return index().data(Qt::ToolTipRole).toString();
        if (type == QAccessible::Value && index().data(Qt::CheckStateRole).isValid())
            return index().data(Qt::CheckStateRole).toInt() == Qt::Checked ? QStringLiteral("已显示") : QStringLiteral("已隐藏");
        return {};
    }
    void setText(QAccessible::Text, const QString&) override {}
    QRect rect() const override
    {
        if (!isValid() || !m_view->isVisible()) return {};
        const auto rect = m_view->visualRect(index()).intersected(m_view->viewport()->rect());
        return rect.isEmpty() ? QRect() : QRect(m_view->viewport()->mapToGlobal(rect.topLeft()), rect.size());
    }
    QAccessible::Role role() const override { return QAccessible::TreeItem; }
    QAccessible::State state() const override
    {
        QAccessible::State state;
        if (!isValid()) { state.invalid = true; return state; }
        const auto item = index();
        state.disabled = !m_view->isEnabled() || !item.flags().testFlag(Qt::ItemIsEnabled);
        state.selectable = item.flags().testFlag(Qt::ItemIsSelectable);
        state.selected = isSelected();
        state.focusable = true;
        state.focused = m_view->hasFocus() && m_view->currentIndex() == item;
        state.checkable = item.data(Qt::CheckStateRole).isValid();
        state.checked = item.data(Qt::CheckStateRole).toInt() == Qt::Checked;
        state.checkStateMixed = item.data(Qt::CheckStateRole).toInt() == Qt::PartiallyChecked;
        state.readOnly = item.data(Qt::UserRole + 2).toBool();
        state.expandable = m_view->m_nodes[m_node].children;
        state.expanded = state.expandable && m_view->isExpanded(item);
        state.collapsed = state.expandable && !state.expanded;
        state.invisible = !m_view->isVisible() || m_view->isIndexHidden(item);
        state.offscreen = rect().isEmpty();
        return state;
    }
    void* interface_cast(QAccessible::InterfaceType type) override
    {
        if (type == QAccessible::ActionInterface) return static_cast<QAccessibleActionInterface*>(this);
        if (type == QAccessible::TableCellInterface) return static_cast<QAccessibleTableCellInterface*>(this);
        return nullptr;
    }
    QStringList actionNames() const override
    {
        if (!isValid()) return {};
        QStringList actions{setFocusAction(), pressAction()};
        if (index().flags().testFlag(Qt::ItemIsUserCheckable) && !index().data(Qt::UserRole + 2).toBool()) actions.append(toggleAction());
        if (childCount()) actions.append(m_view->isExpanded(index()) ? QStringLiteral("collapse") : QStringLiteral("expand"));
        return actions;
    }
    QString localizedActionName(const QString& action) const override
    {
        if (action == QLatin1String("expand")) return QStringLiteral("展开");
        if (action == QLatin1String("collapse")) return QStringLiteral("折叠");
        return QAccessibleActionInterface::localizedActionName(action);
    }
    void doAction(const QString& action) override
    {
        if (!isValid() || !m_view->isEnabled() || !index().flags().testFlag(Qt::ItemIsEnabled)) return;
        const auto item = index();
        if (action == setFocusAction() || action == pressAction()) {
            m_view->setFocus(Qt::OtherFocusReason);
            m_view->scrollTo(item);
            m_view->Choose(item, {});
            if (action == pressAction()) emit m_view->clicked(item);
        } else if (action == toggleAction()) m_view->ToggleCheck(item);
        else if (action == QLatin1String("expand")) m_view->setExpanded(item, true);
        else if (action == QLatin1String("collapse")) m_view->setExpanded(item, false);
    }
    QStringList keyBindingsForAction(const QString& action) const override
    {
        if (action == toggleAction()) return {QStringLiteral("Space")};
        if (action == QLatin1String("expand")) return {m_view && m_view->isRightToLeft() ? QStringLiteral("Left") : QStringLiteral("Right")};
        if (action == QLatin1String("collapse")) return {m_view && m_view->isRightToLeft() ? QStringLiteral("Right") : QStringLiteral("Left")};
        return {};
    }
    bool isSelected() const override { return isValid() && m_view->selectionModel()->isSelected(index()); }
    QList<QAccessibleInterface*> columnHeaderCells() const override { return {}; }
    QList<QAccessibleInterface*> rowHeaderCells() const override { return {}; }
    int columnIndex() const override { return 0; }
    int rowIndex() const override { return isValid() ? m_node : -1; }
    int columnExtent() const override { return 1; }
    int rowExtent() const override { return 1; }
    QAccessibleInterface* table() const override { return isValid() ? QAccessible::queryAccessibleInterface(m_view) : nullptr; }

private:
    QPointer<CModelHierarchyView> m_view;
    int m_node;
    quint64 m_generation;
};

class CHierarchyAccessible final : public QAccessibleWidget, public QAccessibleTableInterface,
                                   public QAccessibleSelectionInterface
{
public:
    explicit CHierarchyAccessible(CModelHierarchyView* view) : QAccessibleWidget(view, QAccessible::Tree), m_view(view) {}
    int childCount() const override { return m_view && m_view->model() ? m_view->model()->rowCount(m_view->rootIndex()) : 0; }
    QAccessibleInterface* child(int row) const override
    {
        return row >= 0 && row < childCount() ? m_view->AccessibleItem(m_view->NodeFor(m_view->model()->index(row, 0, m_view->rootIndex()))) : nullptr;
    }
    int indexOfChild(const QAccessibleInterface* child) const override
    {
        const auto* item = dynamic_cast<const CHierarchyAccessibleItem*>(child);
        return item && item->isValid() && item->table() == this && item->index().parent() == m_view->rootIndex() ? item->index().row() : -1;
    }
    QAccessibleInterface* childAt(int x, int y) const override
    {
        return m_view ? m_view->AccessibleItem(m_view->NodeFor(m_view->indexAt(m_view->viewport()->mapFromGlobal(QPoint(x, y))))) : nullptr;
    }
    QAccessibleInterface* focusChild() const override { return m_view ? m_view->AccessibleItem(m_view->NodeFor(m_view->currentIndex())) : nullptr; }
    QAccessible::State state() const override
    {
        auto state = QAccessibleWidget::state();
        state.multiSelectable = state.extSelectable = m_view && m_view->selectionMode() == QAbstractItemView::ExtendedSelection;
        return state;
    }
    void* interface_cast(QAccessible::InterfaceType type) override
    {
        if (type == QAccessible::TableInterface) return static_cast<QAccessibleTableInterface*>(this);
        if (type == QAccessible::SelectionInterface) return static_cast<QAccessibleSelectionInterface*>(this);
        return QAccessibleWidget::interface_cast(type);
    }
    QAccessibleInterface* caption() const override { return nullptr; }
    QAccessibleInterface* summary() const override { return nullptr; }
    QAccessibleInterface* cellAt(int row, int column) const override { return column == 0 && m_view ? m_view->AccessibleItem(row) : nullptr; }
    int rowCount() const override { return m_view ? m_view->m_nodes.size() : 0; }
    int columnCount() const override { return 1; }
    QString columnDescription(int column) const override { return column == 0 ? QStringLiteral("模型对象") : QString(); }
    QString rowDescription(int row) const override
    {
        const auto* item = cellAt(row, 0);
        return item ? item->text(QAccessible::Name) : QString();
    }
    QList<int> selectedRows() const override
    {
        QList<int> result;
        if (!m_view || !m_view->selectionModel()) return result;
        for (const auto& index : m_view->selectionModel()->selectedRows()) {
            const int node = m_view->NodeFor(index);
            if (node >= 0) result.append(node);
        }
        std::sort(result.begin(), result.end());
        return result;
    }
    bool isRowSelected(int row) const override
    {
        return m_view && row >= 0 && row < rowCount() && m_view->selectionModel()->isSelected(m_view->m_nodes[row].index);
    }
    bool isColumnSelected(int column) const override { return column == 0 && rowCount() > 0 && selectedRowCount() == rowCount(); }
    QList<int> selectedColumns() const override { return isColumnSelected(0) ? QList<int>{0} : QList<int>{}; }
    int selectedColumnCount() const override { return isColumnSelected(0) ? 1 : 0; }
    int selectedRowCount() const override { return selectedRows().size(); }
    int selectedCellCount() const override { return selectedRowCount(); }
    QList<QAccessibleInterface*> selectedCells() const override
    {
        QList<QAccessibleInterface*> result;
        for (int row : selectedRows()) result.append(cellAt(row, 0));
        return result;
    }
    bool selectRow(int row) override
    {
        if (!m_view || row < 0 || row >= rowCount()) return false;
        const auto index = m_view->m_nodes[row].index;
        if (!index.flags().testFlag(Qt::ItemIsSelectable)) return false;
        m_view->selectionModel()->select(index, QItemSelectionModel::Select);
        return true;
    }
    bool unselectRow(int row) override
    {
        if (!m_view || row < 0 || row >= rowCount()) return false;
        m_view->selectionModel()->select(m_view->m_nodes[row].index, QItemSelectionModel::Deselect);
        return true;
    }
    bool selectColumn(int column) override
    {
        if (!m_view || column != 0) return false;
        QItemSelection selection;
        for (const auto& node : m_view->m_nodes) if (node.index.flags().testFlag(Qt::ItemIsSelectable)) selection.select(node.index, node.index);
        m_view->selectionModel()->select(selection, QItemSelectionModel::Select);
        return true;
    }
    bool unselectColumn(int column) override { return column == 0 && clear(); }
    void modelChange(QAccessibleTableModelChangeEvent*) override {}
    int selectedItemCount() const override { return selectedRowCount(); }
    QList<QAccessibleInterface*> selectedItems() const override { return selectedCells(); }
    bool select(QAccessibleInterface* child) override
    {
        auto* item = dynamic_cast<CHierarchyAccessibleItem*>(child);
        return item && item->table() == this && selectRow(item->rowIndex());
    }
    bool unselect(QAccessibleInterface* child) override
    {
        auto* item = dynamic_cast<CHierarchyAccessibleItem*>(child);
        return item && item->table() == this && unselectRow(item->rowIndex());
    }
    bool selectAll() override { return selectColumn(0); }
    bool clear() override
    {
        if (!m_view || !m_view->selectionModel()) return false;
        m_view->selectionModel()->clearSelection();
        return true;
    }

private:
    QPointer<CModelHierarchyView> m_view;
};

namespace {
QAccessibleInterface* hierarchyFactory(const QString&, QObject* object)
{
    if (auto* view = qobject_cast<CModelHierarchyView*>(object)) return new CHierarchyAccessible(view);
    return nullptr;
}
}

void CModelHierarchyView::InvalidateAccessibleItems()
{
    ++m_generation;
    const auto items = m_accessibleItems;
    m_accessibleItems.clear();
    for (auto id : items) QAccessible::deleteAccessibleInterface(id);
}

QAccessibleInterface* CModelHierarchyView::AccessibleItem(int node) const
{
    if (node < 0 || node >= m_nodes.size()) return nullptr;
    const auto found = m_accessibleItems.constFind(node);
    if (found != m_accessibleItems.cend()) return QAccessible::accessibleInterface(found.value());
    auto* item = new CHierarchyAccessibleItem(const_cast<CModelHierarchyView*>(this), node);
    const auto id = QAccessible::registerAccessibleInterface(item);
    m_accessibleItems.insert(node, id);
    return item;
}
