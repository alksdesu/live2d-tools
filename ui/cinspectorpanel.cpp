#include "cinspectorpanel.h"
#include "ckeyformmatrix.h"
#include "cbindingkeysdialog.h"
#include "live2dstruct/cmodeldocument.h"
#include "live2dstruct/cmeshtopology.h"
#include "live2dstruct/cmesheditplanner.h"
#include <QFormLayout>
#include <QScrollArea>
#include <QScrollBar>
#include <QScopeGuard>
#include <QPointer>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QCheckBox>
#include <QComboBox>
#include <QListView>
#include <QStyledItemDelegate>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QPushButton>
#include <QPlainTextEdit>
#include <QSignalBlocker>
#include <QScopedValueRollback>
#include <QTimer>
#include <QSpinBox>
#include <QSet>
#include <QTableWidget>
#include <QPersistentModelIndex>
#include <QHash>
#include <QCache>
#include <QEvent>
#include <QListWidget>
#include <QLineEdit>
#include <QDialog>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QVBoxLayout>
#include <QWidget>
#include <QApplication>
#include <QClipboard>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QMessageBox>
#include <cmath>
#include <algorithm>
#include <exception>
#include <limits>
#include <utility>

namespace {
class VisibilityCheckBox : public QCheckBox
{
public:
    using QCheckBox::QCheckBox;
protected:
    void nextCheckState() override
    {
        setCheckState(checkState() == Qt::Checked ? Qt::Unchecked : Qt::Checked);
    }
};

bool stableMetricRole(const QVariant& value, int depth, int& budget, QVector<int>& types)
{
    if (--budget < 0 || depth > 8) return false;
    types.append(value.metaType().id());
    if (!value.isValid()) return true;
    switch (value.metaType().id()) {
    case QMetaType::QVariantList: {
        const auto values = value.toList();
        if (values.size() > budget) return false;
        for (const auto& item : values) if (!stableMetricRole(item, depth + 1, budget, types)) return false;
        return true;
    }
    case QMetaType::QVariantMap: {
        const auto values = value.toMap();
        if (values.size() > budget) return false;
        for (auto item = values.cbegin(); item != values.cend(); ++item)
            if (!stableMetricRole(item.value(), depth + 1, budget, types)) return false;
        return true;
    }
    case QMetaType::QVariantHash: {
        QMap<QString, QVariant> values;
        const auto hash = value.toHash();
        if (hash.size() > budget) return false;
        for (auto item = hash.cbegin(); item != hash.cend(); ++item) values.insert(item.key(), item.value());
        for (auto item = values.cbegin(); item != values.cend(); ++item)
            if (!stableMetricRole(item.value(), depth + 1, budget, types)) return false;
        return true;
    }
    case QMetaType::Bool:
    case QMetaType::Int:
    case QMetaType::UInt:
    case QMetaType::LongLong:
    case QMetaType::ULongLong:
    case QMetaType::Long:
    case QMetaType::ULong:
    case QMetaType::Short:
    case QMetaType::UShort:
    case QMetaType::Char:
    case QMetaType::SChar:
    case QMetaType::UChar:
    case QMetaType::Float:
    case QMetaType::Double:
    case QMetaType::QChar:
    case QMetaType::QString:
    case QMetaType::QStringList:
    case QMetaType::QByteArray:
    case QMetaType::QSize:
    case QMetaType::QPoint:
    case QMetaType::QRect:
    case QMetaType::QLine:
    case QMetaType::QFont:
    case QMetaType::QColor:
    case QMetaType::QBrush:
    case QMetaType::QIcon:
    case QMetaType::QImage:
    case QMetaType::QPixmap:
    case QMetaType::QBitmap:
    case QMetaType::QTransform:
    case QMetaType::QPolygon:
    case QMetaType::QRegion:
        return true;
    default:
        return false;
    }
}

class ItemMetricCache : public QObject
{
public:
    explicit ItemMetricCache(QTableWidget* table, int capacity = 1024)
        : m_table(table), m_entries(capacity)
    {
        WatchParents();
        qApp->installEventFilter(this);
        auto* model = table->model();
        const auto clear = [this] { m_entries.clear(); };
        connect(model, &QAbstractItemModel::modelAboutToBeReset, this, clear);
        connect(model, &QAbstractItemModel::layoutAboutToBeChanged, this, clear);
        connect(model, &QAbstractItemModel::rowsAboutToBeInserted, this, clear);
        connect(model, &QAbstractItemModel::rowsAboutToBeRemoved, this, clear);
        connect(model, &QAbstractItemModel::columnsAboutToBeInserted, this, clear);
        connect(model, &QAbstractItemModel::columnsAboutToBeRemoved, this, clear);
        connect(model, &QObject::destroyed, this, clear);
    }

    ~ItemMetricCache() override
    {
        if (qApp) qApp->removeEventFilter(this);
    }

    QSize Get(const QStyleOptionViewItem& option, int digits, const std::function<QSize()>& measure) const
    {
        if (!option.index.isValid() || option.type != QStyleOptionViewItem::Type ||
            option.version != QStyleOptionViewItem::Version) return measure();
        const auto* widget = option.widget;
        Entry current;
        current.option = option;
        current.roles = option.index.model()->itemData(option.index);
        current.explicitSize = option.index.data(Qt::SizeHintRole);
        int roleBudget = 256;
        for (auto role = current.roles.cbegin(); role != current.roles.cend(); ++role) {
            if (!stableMetricRole(role.value(), 0, roleBudget, current.roleTypes)) {
                m_entries.remove(option.index);
                return measure();
            }
        }
        if (!stableMetricRole(current.explicitSize, 0, roleBudget, current.roleTypes)) return measure();
        current.flags = option.index.flags();
        current.digits = digits;
        current.rows = option.index.model()->rowCount(option.index.parent());
        current.columns = option.index.model()->columnCount(option.index.parent());
        current.epoch = m_epoch;
        current.style = widget ? widget->style() : QApplication::style();
        current.applicationStyle = qApp->styleSheet();
        if (widget) {
            current.widgetStyle = widget->styleSheet();
            current.widgetSize = widget->size();
            current.dpr = widget->devicePixelRatioF();
            current.dpi = {widget->logicalDpiX(), widget->logicalDpiY()};
            current.macSize = int(widget->testAttribute(Qt::WA_MacSmallSize)) |
                (int(widget->testAttribute(Qt::WA_MacMiniSize)) << 1);
        }
        if (const auto* saved = m_entries.object(option.index); saved && saved->Matches(current)) return saved->size;
        current.size = measure();
        m_entries.insert(option.index, new Entry(std::move(current)));
        return m_entries.object(option.index)->size;
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (watched != qApp && !m_watched.contains(watched)) return false;
        switch (event->type()) {
        case QEvent::StyleChange:
        case QEvent::FontChange:
        case QEvent::ApplicationFontChange:
        case QEvent::PaletteChange:
        case QEvent::ApplicationPaletteChange:
        case QEvent::ThemeChange:
        case QEvent::DevicePixelRatioChange:
        case QEvent::LayoutDirectionChange:
        case QEvent::LocaleChange:
        case QEvent::EnabledChange:
        case QEvent::ContentsRectChange:
        case QEvent::DynamicPropertyChange:
        case QEvent::ParentAboutToChange:
        case QEvent::ParentChange:
            ++m_epoch;
            m_entries.clear();
            if (event->type() == QEvent::ParentChange) WatchParents();
            break;
        default:
            break;
        }
        return false;
    }

private:
    struct Entry {
        QStyleOptionViewItem option;
        QMap<int, QVariant> roles;
        QVector<int> roleTypes;
        QVariant explicitSize;
        Qt::ItemFlags flags;
        const QStyle* style = nullptr;
        QString applicationStyle, widgetStyle;
        QSize widgetSize, dpi, size;
        qreal dpr = 1;
        quint64 epoch = 0;
        int digits = 0, rows = 0, columns = 0, macSize = 0;
        bool Matches(const Entry& other) const
        {
            const auto& a = option;
            const auto& b = other.option;
            return epoch == other.epoch && style == other.style && roles == other.roles && roleTypes == other.roleTypes &&
                explicitSize == other.explicitSize && flags == other.flags &&
                digits == other.digits && rows == other.rows && columns == other.columns &&
                applicationStyle == other.applicationStyle && widgetStyle == other.widgetStyle &&
                widgetSize == other.widgetSize && dpi == other.dpi && dpr == other.dpr && macSize == other.macSize &&
                a.version == b.version && a.type == b.type && a.state == b.state && a.direction == b.direction &&
                a.rect == b.rect && a.fontMetrics == b.fontMetrics && a.palette == b.palette &&
                a.palette.currentColorGroup() == b.palette.currentColorGroup() && a.styleObject == b.styleObject &&
                a.displayAlignment == b.displayAlignment && a.decorationAlignment == b.decorationAlignment &&
                a.textElideMode == b.textElideMode && a.decorationPosition == b.decorationPosition &&
                a.decorationSize == b.decorationSize && a.font == b.font &&
                a.showDecorationSelected == b.showDecorationSelected && a.features == b.features &&
                a.locale == b.locale && a.widget == b.widget && a.index == b.index && a.checkState == b.checkState &&
                a.icon.cacheKey() == b.icon.cacheKey() && a.text == b.text && a.viewItemPosition == b.viewItemPosition &&
                a.backgroundBrush == b.backgroundBrush;
        }
    };
    void WatchParents()
    {
        for (const auto& connection : m_nameConnections) QObject::disconnect(connection);
        m_nameConnections.clear();
        m_watched.clear();
        if (!m_table) return;
        for (auto* widget = static_cast<QWidget*>(m_table.data()); widget; widget = widget->parentWidget())
            m_watched.insert(widget);
        m_watched.insert(m_table->viewport());
        m_watched.insert(m_table->horizontalHeader());
        m_watched.insert(m_table->verticalHeader());
        for (auto* object : m_watched)
            m_nameConnections.append(connect(object, &QObject::objectNameChanged, this, [this] {
                ++m_epoch;
                m_entries.clear();
            }));
    }
    QVector<QMetaObject::Connection> m_nameConnections;
    QPointer<QTableWidget> m_table;
    QSet<QObject*> m_watched;
    quint64 m_epoch = 0;
    mutable QCache<QModelIndex, Entry> m_entries;
};

class GeometryNotifications
{
public:
    explicit GeometryNotifications(QVector<QPoint> protectedCells) : m_protected(std::move(protectedCells))
    {
        std::sort(m_protected.begin(), m_protected.end(), [](QPoint a, QPoint b) {
            return a.y() == b.y() ? a.x() < b.x() : a.y() < b.y();
        });
    }
    void Add(int row, int column)
    {
        const QRect cell(column, row, 1, 1);
        if (!m_ranges.isEmpty()) {
            auto& previous = m_ranges.last();
            const auto joined = previous.area.united(cell);
            if (qint64(joined.width()) * joined.height() <= qint64(previous.changed + 1) * 4 && !ContainsEditor(joined)) {
                previous.area = joined;
                ++previous.changed;
                return;
            }
        }
        m_ranges.append({cell, 1});
    }
    void Publish(QAbstractItemModel* model, bool allRoles) const
    {
        const QList<int> roles = allRoles ? QList<int>() : QList<int>{Qt::DisplayRole, Qt::EditRole};
        for (const auto& range : m_ranges)
            emit model->dataChanged(model->index(range.area.top(), range.area.left()),
                                   model->index(range.area.bottom(), range.area.right()), roles);
    }
private:
    bool ContainsEditor(const QRect& area) const
    {
        auto cell = std::lower_bound(m_protected.cbegin(), m_protected.cend(), area.top(),
            [](QPoint point, int row) { return point.y() < row; });
        for (; cell != m_protected.cend() && cell->y() <= area.bottom(); ++cell)
            if (cell->x() >= area.left() && cell->x() <= area.right()) return true;
        return false;
    }
    struct Range { QRect area; int changed = 0; };
    QVector<Range> m_ranges;
    QVector<QPoint> m_protected;
};

class NumericIndexDelegate : public QStyledItemDelegate
{
public:
    explicit NumericIndexDelegate(QTableWidget* table) : QStyledItemDelegate(table), m_metrics(table) {}
    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override
    {
        QSize result = QStyledItemDelegate::sizeHint(option, index);
        if (!index.isValid()) return result;
        QStyleOptionViewItem value(option); initStyleOption(&value, index);
        const int digits = QString::number(std::max(0, index.model()->rowCount(index.parent()) - 1)).size();
        const auto* style = value.widget ? value.widget->style() : QApplication::style();
        const QSize lowerBound = m_metrics.Get(value, digits, [&] {
            QSize measured;
            for (char digit = '0'; digit <= '9'; ++digit) {
                value.text = QString(digits, QChar(digit));
                measured = measured.expandedTo(style->sizeFromContents(QStyle::CT_ItemViewItem, &value, {}, value.widget));
            }
            return measured;
        });
        return result.expandedTo(lowerBound);
    }
private:
    ItemMetricCache m_metrics;
};

class GeometryEditorDelegate : public QStyledItemDelegate
{
public:
    explicit GeometryEditorDelegate(QTableWidget* table) : QStyledItemDelegate(table), m_metrics(table, 8192) {}
    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override
    {
        QStyleOptionViewItem value(option);
        initStyleOption(&value, index);
        return m_metrics.Get(value, 0, [&] { return QStyledItemDelegate::sizeHint(option, index); });
    }
    std::function<void(QWidget*, const QModelIndex&, bool)> editorChanged;
    std::function<bool(QWidget*)> canCommit;
    QWidget* createEditor(QWidget* parent, const QStyleOptionViewItem& option, const QModelIndex& index) const override
    {
        auto* editor = QStyledItemDelegate::createEditor(parent, option, index);
        if (editor && editorChanged) editorChanged(editor, index, true);
        return editor;
    }
    void setModelData(QWidget* editor, QAbstractItemModel* model, const QModelIndex& index) const override
    {
        if (canCommit && canCommit(editor)) QStyledItemDelegate::setModelData(editor, model, index);
    }
    void destroyEditor(QWidget* editor, const QModelIndex& index) const override
    {
        if (editorChanged) editorChanged(editor, index, false);
        QStyledItemDelegate::destroyEditor(editor, index);
    }
private:
    ItemMetricCache m_metrics;
};

class InspectorComboBox : public QComboBox
{
public:
    explicit InspectorComboBox(QWidget* parent) : QComboBox(parent)
    {
        setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        setMinimumContentsLength(12);
        setMinimumWidth(0);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        auto* choices = new QListView(this);
        setView(choices);
        setItemDelegate(new QStyledItemDelegate(choices));
        choices->setTextElideMode(Qt::ElideNone);
        choices->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        connect(this, &QComboBox::currentTextChanged, this, [this](const QString& text) {
            setToolTip(QStringLiteral("<qt>%1</qt>").arg(text.toHtmlEscaped()));
        });
    }
};

void setRowVisible(QWidget* field, bool visible)
{
    auto* form = field->parentWidget()->findChild<QFormLayout*>(QStringLiteral("inspectorForm"));
    int row = -1;
    QFormLayout::ItemRole role;
    if (form) form->getWidgetPosition(field, &row, &role);
    if (row >= 0) form->setRowVisible(row, visible);
    else field->setVisible(visible);
}

void updateObjectRows(CInspectorPanel* panel, const QString& type)
{
    auto* form = panel->findChild<QFormLayout*>(QStringLiteral("inspectorForm"));
    if (!form) return;
    for (int row = 0; row < form->rowCount(); ++row) {
        auto* item = form->itemAt(row, QFormLayout::FieldRole);
        if (!item) item = form->itemAt(row, QFormLayout::SpanningRole);
        auto* field = item ? item->widget() : nullptr;
        if (!field) continue;
        const auto types = field->property("inspectorTypes").toStringList();
        bool visible = types.isEmpty() ? !type.isEmpty() && type != QLatin1String("multiple") : types.contains(type);
        if (auto* table = qobject_cast<QTableWidget*>(field)) visible = visible && table->rowCount() > 0;
        if (auto* combo = qobject_cast<QComboBox*>(field)) visible = visible && combo->count() > 0;
        form->setRowVisible(row, visible);
    }
}

QString indexValue(int index, const QString& fallback = QStringLiteral("根"))
{
    return index < 0 ? fallback : QString::number(index);
}
}

struct CInspectorPanel::ProjectionState
{
    struct Context {
        const live2dstruct::CModelDocument* document = nullptr;
        QByteArray source;
        QString path, name, type;
        int keyform = -1;
        bool locked = false;
        live2dstruct::CMocParameterStructureBuilder::ColumnState structure;
        QVector<quint16> indices;
        bool operator==(const Context& other) const {
            return document == other.document && source == other.source && path == other.path && name == other.name &&
                type == other.type && keyform == other.keyform && locked == other.locked && structure == other.structure && indices == other.indices;
        }
    };
    struct Table { Context context; QVector<float> values; bool valid = false; };
    struct Editor { QPersistentModelIndex index; quint64 generation; };
    Context context, metadata;
    QHash<QTableWidget*, Table> tables;
    QHash<QWidget*, Editor> editors;
    QList<QPointer<GeometryEditorDelegate>> delegates;
    QSet<QString> locks;
    quint64 generation = 0, revision = 0;
    bool contextValid = false, metadataValid = false, pending = false, queued = false, force = false;
    bool visibilityKnown = false, visible = true;
};

void CInspectorPanel::InvalidateProjection()
{
    auto& state = *m_projection;
    ++state.generation; state.contextValid = state.metadataValid = false;
    state.pending = state.queued = false; state.tables.clear(); state.editors.clear();
}

void CInspectorPanel::PrepareProjectionContext(int keyform)
{
    ProjectionState::Context next;
    next.document = m_document; next.name = m_selectionName; next.type = m_selectionType; next.keyform = keyform;
    if (m_document) {
        next.source = m_document->SourceDigest(); next.path = m_document->SourcePath();
        next.structure = m_document->ParameterStructure(); next.locked = IsLocked(next.type, next.name);
        if (next.type == QLatin1String("artmesh")) next.indices = m_document->DrawableIndices(next.name);
    }
    auto& state = *m_projection;
    if (!state.contextValid || !(state.context == next)) {
        ++state.generation; state.editors.clear(); state.pending = state.queued = false;
        state.context = std::move(next); state.contextValid = true;
    }
}

void CInspectorPanel::QueueProjection()
{
    auto& state = *m_projection;
    if (state.queued || !state.pending) return;
    state.queued = true; const auto generation = state.generation;
    QTimer::singleShot(0, this, [this, generation] {
        auto& current = *m_projection;
        if (generation != current.generation) return;
        current.queued = false;
        if (current.visibilityKnown && !current.visible) return;
        RefreshSelection();
    });
}

void CInspectorPanel::ClearGeometryTable(QTableWidget* table)
{
    m_projection->tables.remove(table);
    if (table->rowCount()) table->setRowCount(0);
}

void CInspectorPanel::UpdateGeometryTable(QTableWidget* table, const QVector<float>& values, int columns, bool rotation)
{
    PrepareProjectionContext(GetActiveGeometryKeyformIndex());
    auto& state = *m_projection; auto& cached = state.tables[table];
    const int rows = values.size() / columns;
    const bool reset = !cached.valid || !(cached.context == state.context) || cached.values.size() != values.size() || table->rowCount() != rows;
    if (!reset && state.visibilityKnown && !state.visible && !state.force) { state.pending = true; return; }
    const QSignalBlocker blocker(table);
    if (reset) {
        table->setRowCount(0); table->setRowCount(rows);
        cached = {state.context, values, true};
    }
    QVector<QPoint> protectedCells;
    if (!state.force) for (auto editor = state.editors.cbegin(); editor != state.editors.cend(); ++editor)
        if (editor->generation == state.generation && editor->index.isValid() && editor->index.model() == table->model())
            protectedCells.append({editor->index.column(), editor->index.row()});
    GeometryNotifications notifications(std::move(protectedCells));
    QSignalBlocker modelBlocker(table->model());
    bool allRoles = reset;
    if (reset) {
        for (int row = 0; row < rows; ++row) {
            auto* item = new QTableWidgetItem(QString::number(row)); item->setFlags(item->flags() & ~Qt::ItemIsEditable); table->setItem(row, 0, item);
            notifications.Add(row, 0);
        }
    }
    for (int row = 0; row < rows; ++row) for (int column = 0; column < columns; ++column) {
        const int at = row * columns + column;
        if (!reset && !state.force && cached.values[at] == values[at]) continue;
        bool editing = false;
        if (!state.force) for (auto editor = state.editors.cbegin(); editor != state.editors.cend(); ++editor)
            if (editor->generation == state.generation && editor->index.isValid() && editor->index.model() == table->model() &&
                editor->index.row() == row && editor->index.column() == column + 1) { editing = true; break; }
        if (editing) { state.pending = true; continue; }
        const QString text = rotation && column >= 4 ? (values[at] != 0 ? QStringLiteral("是") : QStringLiteral("否")) : QString::number(values[at], 'g', 9);
        if (auto* item = table->item(row, column + 1)) {
            if (item->text() != text) { item->setText(text); notifications.Add(row, column + 1); }
        } else {
            table->setItem(row, column + 1, new QTableWidgetItem(text));
            notifications.Add(row, column + 1);
            allRoles = true;
        }
        cached.values[at] = values[at];
    }
    modelBlocker.unblock();
    notifications.Publish(table->model(), allRoles);
}

CInspectorPanel::CInspectorPanel(QWidget* parent)
    : QDockWidget(QStringLiteral("属性"), parent), m_projection(std::make_unique<ProjectionState>())
{
    setObjectName(QStringLiteral("inspectorPanel"));
    auto* content = new QWidget(this);
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(10);

    m_emptyLabel = new QLabel(QStringLiteral("从模型结构中选择一个对象"), content);
    m_emptyLabel->setObjectName(QStringLiteral("inspectorEmpty"));
    m_emptyLabel->setWordWrap(true);
    layout->addWidget(m_emptyLabel);

    auto* form = new QFormLayout();
    form->setObjectName(QStringLiteral("inspectorForm"));
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    form->setVerticalSpacing(9);
    form->setHorizontalSpacing(12);

    m_typeLabel = new QLabel(content);
    m_idLabel = new QLabel(content);
    m_idLabel->setObjectName(QStringLiteral("inspectorObjectId"));
    m_nameLabel = new QLabel(content);
    m_nameLabel->setObjectName(QStringLiteral("inspectorDeformerName"));
    m_renameDeformer = new QPushButton(QStringLiteral("重命名…"), content);
    m_renameDeformer->setObjectName(QStringLiteral("inspectorRenameDeformer"));
    m_renameDeformer->setEnabled(false);
    connect(m_renameDeformer, &QPushButton::clicked, this, [this] {
        if (!m_renameEnabled || m_submitting || m_selectionNames.size() != 1 ||
            (m_selectionType != QLatin1String("warp") && m_selectionType != QLatin1String("rotation")) ||
            IsLocked(m_selectionType, m_selectionName)) return;
        const QString id = m_selectionName, type = m_selectionType;
        emit RenameDeformerRequested(id, type);
    });
    m_parentLabel = new QLabel(content);
    m_deformerLabel = new QLabel(content);
    m_resizeWarpGrid = new QPushButton(QStringLiteral("调整转换网格…"), content);
    m_resizeWarpGrid->setObjectName(QStringLiteral("inspectorResizeWarpGrid"));
    m_resizeWarpGrid->setToolTip(QStringLiteral("调整水平与垂直转换分段，统一更新全部普通和融合关键形"));
    m_resizeWarpGrid->setEnabled(false);
    connect(m_resizeWarpGrid, &QPushButton::clicked, this, [this] {
        if (!m_warpGridResizeEnabled || m_submitting || m_selectionNames.size() != 1 || m_selectionType != QLatin1String("warp")) return;
        const QString id = m_selectionName;
        emit ResizeWarpGridRequested(id);
    });
    m_deformerParentCombo = new InspectorComboBox(content);
    m_deformerParentCombo->setObjectName(QStringLiteral("deformerParentSelector"));
    m_deformerParentCombo->setEnabled(false);
    m_visibilityLabel = new QLabel(content);
    m_visibilityCheck = new VisibilityCheckBox(QStringLiteral("显示"), content);
    m_opacitySpin = new QDoubleSpinBox(content);
    m_opacitySpin->setRange(0.0, 1.0);
    m_opacitySpin->setDecimals(3);
    m_opacitySpin->setSingleStep(0.05);
    m_drawOrderSpin = new QSpinBox(content);
    m_drawOrderSpin->setRange(0, 1000);
    m_geometryLabel = new QLabel(content);
    m_textureLabel = new QLabel(content);
    m_drawOrderLabel = new QLabel(content);
    m_blendLabel = new QLabel(content);
    m_maskLabel = new QLabel(content);
    m_maskLabel->setObjectName(QStringLiteral("maskCountLabel"));
    m_editMasks = new QPushButton(QStringLiteral("编辑蒙版"), content);
    m_editMasks->setObjectName(QStringLiteral("editMasks"));
    m_editMasks->setEnabled(false);
    m_maskInvertedCheck = new QCheckBox(QStringLiteral("反向蒙版"), content);
    m_maskInvertedCheck->setObjectName(QStringLiteral("maskInverted"));
    m_maskInvertedCheck->setEnabled(false);
    m_keyformLabel = new QLabel(content);
    m_keyformCombo = new InspectorComboBox(content);
    m_keyformCombo->setObjectName(QStringLiteral("keyformSelector"));
    m_keyformCombo->setEnabled(false);
    m_keyformMatrix = new CKeyformMatrix(content);
    m_keyformMatrix->setObjectName(QStringLiteral("keyformMatrix"));
    m_keyformMatrix->setAccessibleName(QStringLiteral("关键形矩阵"));
    m_editBindingKeys = new QPushButton(QStringLiteral("编辑关键值…"), content);
    m_editBindingKeys->setObjectName(QStringLiteral("editBindingKeys"));
    m_editBindingKeys->setToolTip(QStringLiteral("增删或移动当前对象参数轴的关键值，复制或插值生成对应关键形"));
    m_vertexTable = new QTableWidget(content);
    m_vertexTable->setObjectName(QStringLiteral("keyformVertexTable"));
    m_vertexTable->setColumnCount(3);
    m_vertexTable->setHorizontalHeaderLabels({QStringLiteral("顶点"), QStringLiteral("X"), QStringLiteral("Y")});
    m_vertexTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_vertexTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_vertexTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_vertexTable->verticalHeader()->setVisible(false);
    m_vertexTable->setSelectionBehavior(QAbstractItemView::SelectItems);
    m_vertexTable->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed |
                                   QAbstractItemView::SelectedClicked);
    m_vertexTable->setMinimumHeight(120);
    m_vertexTable->setMaximumHeight(280);
    auto* keyformTools = new QWidget(content);
    auto* keyformToolsLayout = new QGridLayout(keyformTools);
    keyformToolsLayout->setContentsMargins(0, 0, 0, 0);
    keyformToolsLayout->setHorizontalSpacing(6);
    keyformToolsLayout->setVerticalSpacing(6);
    keyformToolsLayout->setContentsMargins(0, 0, 0, 0);
    m_mirrorHorizontal = new QPushButton(QStringLiteral("水平镜像"), keyformTools);
    m_mirrorVertical = new QPushButton(QStringLiteral("垂直镜像"), keyformTools);
    m_interpolatePrevious = new QPushButton(QStringLiteral("与前一形插值"), keyformTools);
    m_copyToNext = new QPushButton(QStringLiteral("复制到后一形"), keyformTools);
    m_resetKeyform = new QPushButton(QStringLiteral("重置当前形"), keyformTools);
    m_copyKeyform = new QPushButton(QStringLiteral("复制关键形"), keyformTools);
    m_pasteKeyform = new QPushButton(QStringLiteral("粘贴关键形"), keyformTools);
    m_optimizeTopology = new QPushButton(QStringLiteral("优化网格"), keyformTools);
    m_optimizeTopology->setToolTip(QStringLiteral("按当前关键形优化内部连线，保留外轮廓、孔洞和分离区域"));
    m_autoTriangulate = new QPushButton(QStringLiteral("重建全部连线…"), keyformTools);
    m_autoTriangulate->setToolTip(QStringLiteral("按当前关键形的全部顶点重新连线，可能填补孔洞、扩大轮廓或连接分离区域"));
    m_copyTopology = new QPushButton(QStringLiteral("复制拓扑"), keyformTools);
    m_pasteTopology = new QPushButton(QStringLiteral("粘贴拓扑"), keyformTools);
    m_mirrorHorizontal->setObjectName(QStringLiteral("mirrorHorizontal"));
    m_mirrorVertical->setObjectName(QStringLiteral("mirrorVertical"));
    m_interpolatePrevious->setObjectName(QStringLiteral("interpolatePreviousKeyform"));
    m_copyToNext->setObjectName(QStringLiteral("copyKeyformToNext"));
    m_resetKeyform->setObjectName(QStringLiteral("resetKeyform"));
    m_copyKeyform->setObjectName(QStringLiteral("copyKeyform"));
    m_pasteKeyform->setObjectName(QStringLiteral("pasteKeyform"));
    m_optimizeTopology->setObjectName(QStringLiteral("optimizeTopology"));
    m_autoTriangulate->setObjectName(QStringLiteral("autoTriangulate"));
    m_copyTopology->setObjectName(QStringLiteral("copyTopology"));
    m_pasteTopology->setObjectName(QStringLiteral("pasteTopology"));
    m_mirrorHorizontal->setEnabled(false);
    m_mirrorVertical->setEnabled(false);
    m_interpolatePrevious->setEnabled(false);
    m_copyToNext->setEnabled(false);
    m_resetKeyform->setEnabled(false);
    m_copyKeyform->setEnabled(false);
    m_pasteKeyform->setEnabled(false);
    m_optimizeTopology->setEnabled(false);
    m_autoTriangulate->setEnabled(false);
    m_copyTopology->setEnabled(false);
    m_pasteTopology->setEnabled(false);
    ResetMaskControls();
    const QList<QPushButton*> keyformButtons = {
        m_mirrorHorizontal, m_mirrorVertical, m_interpolatePrevious, m_copyToNext,
        m_resetKeyform, m_copyKeyform, m_pasteKeyform, m_optimizeTopology, m_autoTriangulate,
        m_copyTopology, m_pasteTopology
    };
    for (int i = 0; i < keyformButtons.size(); ++i)
        keyformToolsLayout->addWidget(keyformButtons[i], i / 2, i % 2);

    for (QLabel* value : {m_typeLabel, m_idLabel, m_nameLabel, m_parentLabel, m_deformerLabel,
                          m_visibilityLabel, m_geometryLabel, m_textureLabel,
                          m_drawOrderLabel, m_blendLabel, m_maskLabel, m_keyformLabel}) {
        value->setTextFormat(Qt::PlainText);
        value->setTextInteractionFlags(Qt::TextSelectableByMouse);
        value->setWordWrap(true);
        value->setMinimumWidth(0);
        value->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    }
    form->addRow(QStringLiteral("类型"), m_typeLabel);
    auto* nameRow = new QWidget(content);
    nameRow->setProperty("inspectorTypes", QStringList{QStringLiteral("warp"), QStringLiteral("rotation")});
    auto* nameLayout = new QHBoxLayout(nameRow);
    nameLayout->setContentsMargins(0, 0, 0, 0);
    nameLayout->addWidget(m_nameLabel, 1);
    nameLayout->addWidget(m_renameDeformer);
    form->addRow(QStringLiteral("名称"), nameRow);
    form->addRow(QStringLiteral("ID"), m_idLabel);
    form->addRow(QStringLiteral("所属 Part"), m_parentLabel);
    form->addRow(QStringLiteral("父变形器"), m_deformerLabel);
    form->addRow(QStringLiteral("编辑父变形器"), m_deformerParentCombo);
    form->addRow(m_resizeWarpGrid);
    form->addRow(QStringLiteral("可见 / 启用"), m_visibilityLabel);
    form->addRow(QStringLiteral("编辑可见性"), m_visibilityCheck);
    form->addRow(QStringLiteral("编辑透明度"), m_opacitySpin);
    form->addRow(QStringLiteral("编辑绘制顺序"), m_drawOrderSpin);
    form->addRow(QStringLiteral("几何"), m_geometryLabel);
    form->addRow(QStringLiteral("纹理"), m_textureLabel);
    form->addRow(QStringLiteral("绘制顺序"), m_drawOrderLabel);
    form->addRow(QStringLiteral("混合 / 剔除"), m_blendLabel);
    form->addRow(QStringLiteral("Clipping"), m_maskLabel);
    form->addRow(QStringLiteral("蒙版关系"), m_editMasks);
    form->addRow(QStringLiteral("蒙版模式"), m_maskInvertedCheck);
    form->addRow(QStringLiteral("Keyform"), m_keyformLabel);
    form->addRow(m_keyformMatrix);
    form->addRow(QStringLiteral("关键值"), m_editBindingKeys);
    form->addRow(QStringLiteral("关键形选择"), m_keyformCombo);
    form->addRow(QStringLiteral("关键形工具"), keyformTools);
    auto* vertexTransformTools = new QWidget(content);
    auto* vertexTransformLayout = new QFormLayout(vertexTransformTools);
    vertexTransformLayout->setRowWrapPolicy(QFormLayout::WrapLongRows);
    vertexTransformLayout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    vertexTransformLayout->setContentsMargins(0, 0, 0, 0);
    m_vertexDeltaX = new QDoubleSpinBox(vertexTransformTools);
    m_vertexDeltaX->setObjectName(QStringLiteral("vertexDeltaX"));
    m_vertexDeltaX->setRange(-10000.0, 10000.0);
    m_vertexDeltaX->setDecimals(4);
    m_vertexDeltaX->setSingleStep(0.01);
    vertexTransformLayout->addRow(QStringLiteral("ΔX"), m_vertexDeltaX);
    m_vertexDeltaY = new QDoubleSpinBox(vertexTransformTools);
    m_vertexDeltaY->setObjectName(QStringLiteral("vertexDeltaY"));
    m_vertexDeltaY->setRange(-10000.0, 10000.0);
    m_vertexDeltaY->setDecimals(4);
    m_vertexDeltaY->setSingleStep(0.01);
    vertexTransformLayout->addRow(QStringLiteral("ΔY"), m_vertexDeltaY);
    m_moveVertices = new QPushButton(QStringLiteral("移动选中顶点"), vertexTransformTools);
    m_moveVertices->setObjectName(QStringLiteral("moveSelectedVertices"));
    m_moveVertices->setEnabled(false);
    vertexTransformLayout->addRow(m_moveVertices);
    m_mergeVertices = new QPushButton(QStringLiteral("合并拓扑顶点"), vertexTransformTools);
    m_mergeVertices->setObjectName(QStringLiteral("mergeSelectedVertices"));
    m_mergeVertices->setEnabled(false);
    vertexTransformLayout->addRow(m_mergeVertices);
    m_vertexScale = new QDoubleSpinBox(vertexTransformTools);
    m_vertexScale->setObjectName(QStringLiteral("vertexScale"));
    m_vertexScale->setRange(0.01, 100.0);
    m_vertexScale->setDecimals(3);
    m_vertexScale->setValue(1.0);
    vertexTransformLayout->addRow(QStringLiteral("缩放"), m_vertexScale);
    m_vertexRotation = new QDoubleSpinBox(vertexTransformTools);
    m_vertexRotation->setObjectName(QStringLiteral("vertexRotation"));
    m_vertexRotation->setRange(-3600.0, 3600.0);
    m_vertexRotation->setDecimals(2);
    vertexTransformLayout->addRow(QStringLiteral("旋转°"), m_vertexRotation);
    m_transformVertices = new QPushButton(QStringLiteral("应用变换"), vertexTransformTools);
    m_transformVertices->setObjectName(QStringLiteral("transformSelectedVertices"));
    m_transformVertices->setEnabled(false);
    vertexTransformLayout->addRow(m_transformVertices);
    form->addRow(QStringLiteral("顶点变换"), vertexTransformTools);
    form->addRow(QStringLiteral("顶点位置"), m_vertexTable);
    m_rotationKeyformTable = new QTableWidget(content);
    m_rotationKeyformTable->setObjectName(QStringLiteral("rotationKeyformTable"));
    m_rotationKeyformTable->setColumnCount(7);
    m_rotationKeyformTable->setHorizontalHeaderLabels({QStringLiteral("形"), QStringLiteral("角度"),
                                                        QStringLiteral("原点 X"), QStringLiteral("原点 Y"),
                                                        QStringLiteral("缩放"), QStringLiteral("反射 X"),
                                                        QStringLiteral("反射 Y")});
    m_rotationKeyformTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    for (int column = 1; column < 7; ++column)
        m_rotationKeyformTable->horizontalHeader()->setSectionResizeMode(column, QHeaderView::ResizeToContents);
    m_rotationKeyformTable->verticalHeader()->setVisible(false);
    m_rotationKeyformTable->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed |
                                             QAbstractItemView::SelectedClicked);
    setRowVisible(m_rotationKeyformTable, false);
    form->addRow(QStringLiteral("旋转关键形"), m_rotationKeyformTable);
    m_warpKeyformCombo = new InspectorComboBox(content);
    m_warpKeyformCombo->setObjectName(QStringLiteral("warpKeyformSelector"));
    m_warpKeyformCombo->setEnabled(false);
    m_warpKeyformTable = new QTableWidget(content);
    m_warpKeyformTable->setObjectName(QStringLiteral("warpKeyformTable"));
    m_warpKeyformTable->setColumnCount(3);
    m_warpKeyformTable->setHorizontalHeaderLabels({QStringLiteral("网格点"), QStringLiteral("X"), QStringLiteral("Y")});
    m_warpKeyformTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_warpKeyformTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_warpKeyformTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_warpKeyformTable->verticalHeader()->setVisible(false);
    m_warpKeyformTable->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed |
                                        QAbstractItemView::SelectedClicked);
    setRowVisible(m_warpKeyformTable, false);
    form->addRow(QStringLiteral("Warp 关键形"), m_warpKeyformCombo);
    form->addRow(QStringLiteral("Warp 网格坐标"), m_warpKeyformTable);
    for (QWidget* field : QList<QWidget*>{m_keyformCombo, keyformTools, vertexTransformTools, m_vertexTable,
                                         m_textureLabel, m_drawOrderLabel, m_blendLabel, m_maskLabel, m_editMasks, m_maskInvertedCheck})
        field->setProperty("inspectorTypes", QStringList{QStringLiteral("artmesh")});
    for (QWidget* field : QList<QWidget*>{m_opacitySpin, m_drawOrderSpin})
        field->setProperty("inspectorTypes", QStringList{QStringLiteral("artmesh"), QStringLiteral("multiple")});
    m_visibilityCheck->setProperty("inspectorTypes", QStringList{QStringLiteral("part"), QStringLiteral("artmesh"),
        QStringLiteral("warp"), QStringLiteral("rotation"), QStringLiteral("multiple")});
    m_deformerParentCombo->setProperty("inspectorTypes", QStringList{QStringLiteral("warp"), QStringLiteral("rotation")});
    m_resizeWarpGrid->setProperty("inspectorTypes", QStringList{QStringLiteral("warp")});
    m_geometryLabel->setProperty("inspectorTypes", QStringList{QStringLiteral("artmesh"), QStringLiteral("warp"), QStringLiteral("rotation")});
    m_keyformLabel->setProperty("inspectorTypes", QStringList{QStringLiteral("artmesh")});
    m_keyformMatrix->setProperty("inspectorTypes", QStringList{QStringLiteral("artmesh"), QStringLiteral("warp"), QStringLiteral("rotation")});
    m_editBindingKeys->setProperty("inspectorTypes", QStringList{QStringLiteral("artmesh"), QStringLiteral("warp"), QStringLiteral("rotation")});
    m_rotationKeyformTable->setProperty("inspectorTypes", QStringList{QStringLiteral("rotation")});
    for (QWidget* field : QList<QWidget*>{m_warpKeyformCombo, m_warpKeyformTable})
        field->setProperty("inspectorTypes", QStringList{QStringLiteral("warp")});
    for (QTableWidget* table : {m_vertexTable, m_rotationKeyformTable, m_warpKeyformTable}) {
        auto* delegate = new GeometryEditorDelegate(table); m_projection->delegates.append(delegate);
        const QPointer<CInspectorPanel> owner(this);
        delegate->canCommit = [owner](QWidget* editor) {
            if (!owner) return false;
            const auto& state = *owner->m_projection;
            const auto found = state.editors.constFind(editor);
            return found != state.editors.cend() && found->generation == state.generation && found->index.isValid();
        };
        delegate->editorChanged = [owner](QWidget* editor, const QModelIndex& index, bool opened) {
            if (!owner) return;
            auto& state = *owner->m_projection;
            if (opened) {
                state.editors.insert(editor, {QPersistentModelIndex(index), state.generation});
                const auto generation = state.generation;
                QObject::connect(editor, &QObject::destroyed, owner, [owner, editor, generation] {
                    if (!owner || generation != owner->m_projection->generation) return;
                    owner->m_projection->editors.remove(editor); owner->QueueProjection();
                });
            } else { state.editors.remove(editor); owner->QueueProjection(); }
        };
        table->setItemDelegate(delegate);
        table->setItemDelegateForColumn(0, new NumericIndexDelegate(table));
        table->setMinimumHeight(120);
        table->setMaximumHeight(280);
        table->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    }
    const int headerMargin = m_rotationKeyformTable->style()->pixelMetric(QStyle::PM_HeaderMargin);
    m_rotationKeyformTable->horizontalHeader()->setMinimumSectionSize(
        m_rotationKeyformTable->fontMetrics().horizontalAdvance(QStringLiteral("原点 X")) + 2 * headerMargin);
    layout->addLayout(form);
    layout->addStretch(1);
    auto* scroll = new QScrollArea(this);
    scroll->setObjectName(QStringLiteral("inspectorScrollArea"));
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setWidget(content);
    setWidget(scroll);
    connect(m_keyformMatrix, &CKeyformMatrix::KeyformRequested, this, &CInspectorPanel::RequestMatrixKeyform);
    connect(m_editBindingKeys, &QPushButton::clicked, this, &CInspectorPanel::EditBindingKeys);
    connect(m_keyformCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index){
        if (m_updatingKeyform) return;
        if (m_keyformHandler) {
            // activated 在 currentIndexChanged 之后发出，不能提前回滚用户选择。
            QTimer::singleShot(0, this, [this] {
                if (!m_keyformHandler || m_updatingKeyform) return;
                const QSignalBlocker blocker(m_keyformCombo);
                m_keyformCombo->setCurrentIndex(m_activeKeyformIndex);
            });
            return;
        }
        m_activeKeyformIndex = index;
        m_keyformMatrix->SetActiveKeyform(index);
        RebuildKeyformTable();
        if (index >= 0 && !m_selectionName.isEmpty()) {
            emit DrawableKeyformActivated(m_selectionName, index);
            emit DrawableKeyformSelected(m_selectionName, index);
        }
    });
    connect(m_keyformCombo, QOverload<int>::of(&QComboBox::activated), this, [this](int index){
        if (!m_keyformHandler || m_updatingKeyform || index == m_activeKeyformIndex ||
            index < 0 || index >= m_keyformCombo->count() || m_selectionType != QLatin1String("artmesh")) return;
        if (!RequestKeyform(m_selectionName, index)) return;
        m_activeKeyformIndex = index;
        m_keyformMatrix->SetActiveKeyform(index);
        {
            const QSignalBlocker blocker(m_keyformCombo);
            m_keyformCombo->setCurrentIndex(index);
        }
        RebuildKeyformTable();
        emit DrawableKeyformActivated(m_selectionName, index);
        emit DrawableKeyformSelected(m_selectionName, index);
    });
    connect(m_vertexTable, &QTableWidget::cellChanged, this, [this](int row, int column){
        if (!m_updatingKeyform) ApplyKeyformCellEdit(row, column);
    });
    connect(m_rotationKeyformTable, &QTableWidget::cellChanged, this, [this](int row, int column){
        if (!m_updatingRotationKeyform) ApplyRotationKeyformCellEdit(row, column);
    });
    connect(m_rotationKeyformTable, &QTableWidget::currentCellChanged, this,
            [this](int row, int, int, int) {
        if (m_updatingRotationKeyform || row < 0 || m_selectionType != QLatin1String("rotation")) return;
        if (!RequestDeformerKeyform(row)) return;
        m_activeDeformerKeyform = row;
        m_keyformMatrix->SetActiveKeyform(row);
    });
    connect(m_warpKeyformCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index){
        if (m_updatingWarpKeyform) return;
        if (m_deformerKeyformHandler) {
            QTimer::singleShot(0, this, [this] {
                if (!m_deformerKeyformHandler || m_updatingWarpKeyform) return;
                const QSignalBlocker blocker(m_warpKeyformCombo);
                m_warpKeyformCombo->setCurrentIndex(m_activeDeformerKeyform);
            });
            return;
        }
        m_activeDeformerKeyform = index;
        m_keyformMatrix->SetActiveKeyform(index);
        RebuildWarpKeyformTable();
    });
    connect(m_warpKeyformCombo, QOverload<int>::of(&QComboBox::activated), this, [this](int index){
        if (!m_deformerKeyformHandler || m_updatingWarpKeyform || index < 0 || index >= m_warpKeyformCombo->count() ||
            m_selectionType != QLatin1String("warp")) return;
        if (!RequestDeformerKeyform(index)) return;
        m_activeDeformerKeyform = index;
        m_keyformMatrix->SetActiveKeyform(index);
        {
            const QSignalBlocker blocker(m_warpKeyformCombo);
            m_warpKeyformCombo->setCurrentIndex(index);
        }
        RebuildWarpKeyformTable();
    });
    connect(m_warpKeyformTable, &QTableWidget::cellChanged, this, [this](int row, int column){
        if (!m_updatingWarpKeyform) ApplyWarpKeyformCellEdit(row, column);
    });
    connect(m_mirrorHorizontal, &QPushButton::clicked, this, [this](){ MirrorKeyform(true); });
    connect(m_mirrorVertical, &QPushButton::clicked, this, [this](){ MirrorKeyform(false); });
    connect(m_interpolatePrevious, &QPushButton::clicked, this, [this](){ InterpolatePreviousKeyform(); });
    connect(m_copyToNext, &QPushButton::clicked, this, [this](){ CopyKeyformToNext(); });
    connect(m_resetKeyform, &QPushButton::clicked, this, [this](){ ResetCurrentKeyform(); });
    connect(m_copyKeyform, &QPushButton::clicked, this, [this](){ CopyCurrentKeyform(); });
    connect(m_pasteKeyform, &QPushButton::clicked, this, [this](){ PasteCurrentKeyform(); });
    connect(m_optimizeTopology, &QPushButton::clicked, this, &CInspectorPanel::OptimizeTopology);
    connect(m_autoTriangulate, &QPushButton::clicked, this, [this](){ AutoTriangulate(); });
    connect(m_copyTopology, &QPushButton::clicked, this, [this](){ CopyTopology(); });
    connect(m_pasteTopology, &QPushButton::clicked, this, [this](){ PasteTopology(); });
    connect(m_editMasks, &QPushButton::clicked, this, [this](){ EditMasks(); });
    connect(m_moveVertices, &QPushButton::clicked, this, [this](){ TranslateSelectedVertices(); });
    connect(m_mergeVertices, &QPushButton::clicked, this, [this](){ MergeSelectedVertices(); });
    connect(m_transformVertices, &QPushButton::clicked, this, [this](){ TransformSelectedVertices(); });
    connect(m_vertexTable, &QTableWidget::itemSelectionChanged, this, [this](){
        const bool hasSelection = m_vertexTable->selectedItems().size() > 0 && m_vertexTable->isEnabled();
        QSet<int> selectedRows;
        for (const QTableWidgetSelectionRange& range : m_vertexTable->selectedRanges())
            for (int row = range.topRow(); row <= range.bottomRow(); ++row) selectedRows.insert(row);
        m_moveVertices->setEnabled(hasSelection &&
                                   m_vertexTable->isEnabled() && m_vertexDeltaX && m_vertexDeltaY);
        m_mergeVertices->setEnabled(selectedRows.size() >= 2);
        m_transformVertices->setEnabled(!selectedRows.isEmpty());
    });
    connect(m_maskInvertedCheck, &QCheckBox::toggled, this, [this](bool inverted){
        if (m_updatingVisibility || !m_document || m_selectionType != QLatin1String("artmesh") ||
            m_selectionName.isEmpty() || IsLocked(m_selectionType, m_selectionName)) return;
        const bool before = m_document->DrawableMaskInverted(m_selectionName);
        if (before == inverted) return;
        const live2dstruct::CModelEdit command{m_selectionName, QStringLiteral("artmesh"),
                                               live2dstruct::CModelEdit::MaskInverted, before, inverted};
        if (!SubmitEdit(command)) return;
        emit HistoryChanged(m_document->CanUndoEdit(), m_document->CanRedoEdit());
        showModelObject(m_selectionName, m_selectionType);
    });
    connect(m_deformerParentCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index){
        if (m_updatingDeformerParent || !m_document || m_selectionName.isEmpty() ||
            (m_selectionType != QLatin1String("warp") && m_selectionType != QLatin1String("rotation")) ||
            IsLocked(m_selectionType, m_selectionName) || index < 0) return;
        const int before = m_document->DeformerParentIndex(m_selectionName);
        const int after = m_deformerParentCombo->itemData(index).toInt();
        if (before == after) return;
        const live2dstruct::CModelEdit command{m_selectionName, m_selectionType,
                                               live2dstruct::CModelEdit::DeformerParent,
                                               before, after};
        if (!SubmitEdit(command)) return;
        emit HistoryChanged(m_document->CanUndoEdit(), m_document->CanRedoEdit());
        showModelObject(m_selectionName, m_selectionType);
    });
    connect(m_visibilityCheck, &QCheckBox::checkStateChanged, this, [this](Qt::CheckState state){
        if (state == Qt::PartiallyChecked) return;
        const bool visible = state == Qt::Checked;
        if (m_updatingVisibility || !m_document) return;
        if (m_selectionNames.size() > 1) {
            const auto current = m_document->VisibilityState();
            QMap<QString, bool> before, after;
            for (int i = 0; i < m_selectionNames.size(); ++i) {
                const QString name = m_selectionNames[i];
                const QString type = m_selectionTypes.value(i);
                if (IsLocked(type, name)) continue;
                const QString key = type + QLatin1Char('|') + name;
                const auto value = current.constFind(key);
                if (value == current.cend() || value.value() == visible) continue;
                before.insert(key, value.value());
                after.insert(key, visible);
            }
            if (after.isEmpty()) { RefreshSelection(); return; }
            const live2dstruct::CModelEdit command{QStringLiteral("selection"), QStringLiteral("objects"),
                live2dstruct::CModelEdit::VisibilityValues, QVariant::fromValue(before), QVariant::fromValue(after), -1, false};
            if (!SubmitEdit(command)) return;
            emit VisibilityBatchChanged(after);
            emit HistoryChanged(m_document->CanUndoEdit(), m_document->CanRedoEdit());
            RefreshSelection();
            return;
        }
        if (m_selectionName.isEmpty()) return;
        if (IsLocked(m_selectionType, m_selectionName)) return;
        bool before = visible;
        if (m_selectionType == QLatin1String("part")) {
            for (const auto& part : m_document->Parts()) if (part.id == m_selectionName) { before = part.visible; break; }
        } else if (m_selectionType == QLatin1String("warp") || m_selectionType == QLatin1String("rotation")) {
            for (const auto& deformer : m_document->Deformers()) if (deformer.id == m_selectionName) { before = deformer.visible; break; }
        } else if (m_selectionType == QLatin1String("artmesh")) {
            for (const auto& drawable : m_document->Drawables()) if (drawable.id == m_selectionName) { before = drawable.visible; break; }
        }
        if (before == visible) return;
        const live2dstruct::CModelEdit command{m_selectionName, m_selectionType,
                                               live2dstruct::CModelEdit::Visibility, before, visible};
        if (!SubmitEdit(command)) return;
        emit VisibilityChanged(m_selectionName, m_selectionType, visible);
        emit HistoryChanged(m_document->CanUndoEdit(), m_document->CanRedoEdit());
    });
    connect(m_opacitySpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double value){
        if (m_updatingVisibility || !m_document) return;
        if (m_selectionNames.size() > 1) {
            for (int i = 0; i < m_selectionNames.size(); ++i) {
                if (m_selectionTypes.value(i) != QLatin1String("artmesh")) continue;
                const QString name = m_selectionNames[i];
                if (IsLocked(QStringLiteral("artmesh"), name)) continue;
                float before = static_cast<float>(value);
                for (const auto& drawable : m_document->Drawables()) if (drawable.id == name) { before = m_document->DrawableOpacity(drawable); break; }
                const float after = static_cast<float>(value);
                if (qFuzzyCompare(before + 1.0f, after + 1.0f)) continue;
                const live2dstruct::CModelEdit command{name, QLatin1String("artmesh"),
                                                       live2dstruct::CModelEdit::Opacity, before, after};
                if (!SubmitEdit(command)) return;
                emit DrawableOpacityChanged(name, after);
                emit HistoryChanged(m_document->CanUndoEdit(), m_document->CanRedoEdit());
            }
            return;
        }
        if (m_selectionType != QLatin1String("artmesh") || m_selectionName.isEmpty()) return;
        if (IsLocked(m_selectionType, m_selectionName)) return;
        float before = static_cast<float>(value);
        for (const auto& drawable : m_document->Drawables()) if (drawable.id == m_selectionName) { before = m_document->DrawableOpacity(drawable); break; }
        if (qFuzzyCompare(before + 1.0f, static_cast<float>(value) + 1.0f)) return;
        const float after = static_cast<float>(value);
        const live2dstruct::CModelEdit command{m_selectionName, m_selectionType,
                                               live2dstruct::CModelEdit::Opacity, before, after};
        if (!SubmitEdit(command)) return;
        emit DrawableOpacityChanged(m_selectionName, after);
        emit HistoryChanged(m_document->CanUndoEdit(), m_document->CanRedoEdit());
    });
    connect(m_drawOrderSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int value){
        if (m_updatingVisibility || !m_document) return;
        if (m_selectionNames.size() > 1) {
            for (int i = 0; i < m_selectionNames.size(); ++i) {
                if (m_selectionTypes.value(i) != QLatin1String("artmesh")) continue;
                const QString name = m_selectionNames[i];
                if (IsLocked(QStringLiteral("artmesh"), name)) continue;
                int before = value;
                for (const auto& drawable : m_document->Drawables()) if (drawable.id == name) { before = qRound(m_document->DrawableDrawOrder(drawable)); break; }
                if (before == value) continue;
                const live2dstruct::CModelEdit command{name, QLatin1String("artmesh"),
                                                       live2dstruct::CModelEdit::DrawOrder, before, value};
                if (!SubmitEdit(command)) return;
                emit DrawableDrawOrderChanged(name, value);
                emit HistoryChanged(m_document->CanUndoEdit(), m_document->CanRedoEdit());
            }
            return;
        }
        if (m_selectionType != QLatin1String("artmesh") || m_selectionName.isEmpty()) return;
        if (IsLocked(m_selectionType, m_selectionName)) return;
        int before = value;
        for (const auto& drawable : m_document->Drawables()) if (drawable.id == m_selectionName) { before = qRound(m_document->DrawableDrawOrder(drawable)); break; }
        if (before == value) return;
        const live2dstruct::CModelEdit command{m_selectionName, m_selectionType,
                                               live2dstruct::CModelEdit::DrawOrder, before, value};
        if (!SubmitEdit(command)) return;
        emit DrawableDrawOrderChanged(m_selectionName, value);
        emit HistoryChanged(m_document->CanUndoEdit(), m_document->CanRedoEdit());
    });
    ClearSelection();
    connect(this, &QDockWidget::visibilityChanged, this, [this](bool visible) {
        m_projection->visibilityKnown = true; m_projection->visible = visible;
        if (visible) QueueProjection();
    });
}

CInspectorPanel::~CInspectorPanel()
{
    for (auto delegate : m_projection->delegates) if (delegate) { delegate->editorChanged = {}; delegate->canCommit = {}; }
    // 浮动窗口销毁时会提交编辑，须在派生成员释放前断开。
    for (QObject* child : findChildren<QObject*>()) QObject::disconnect(child, nullptr, this, nullptr);
}

void CInspectorPanel::SetDocument(live2dstruct::CModelDocument* document)
{
    const bool sameDocument = m_document == document;
    InvalidateProjection();
    m_document = document;
    if (m_selectionName.isEmpty()) ClearSelection();
    else showModelObject(m_selectionName, m_selectionType, sameDocument);
}

void CInspectorPanel::SetEditHandler(std::function<bool(const live2dstruct::CModelEdit&, QString*)> handler)
{
    m_editHandler = std::move(handler);
}

void CInspectorPanel::SetHistoryHandler(std::function<bool(bool, live2dstruct::CModelEdit&, QString*)> handler)
{
    m_historyHandler = std::move(handler);
}

void CInspectorPanel::SetKeyformHandler(std::function<bool(const QString&, int, QString*)> handler)
{
    m_keyformHandler = std::move(handler);
}

void CInspectorPanel::SetDeformerKeyformHandler(std::function<bool(const QString&, const QString&, int, QString*)> handler)
{
    m_deformerKeyformHandler = std::move(handler);
}

bool CInspectorPanel::RequestDeformerKeyform(int index)
{
    if (!m_deformerKeyformHandler) return true;
    if (m_submitting) return false;
    const QPointer<CInspectorPanel> alive(this);
    m_submitting = true;
    const auto guard = qScopeGuard([alive] { if (alive) alive->m_submitting = false; });
    const QString id = m_selectionName, type = m_selectionType;
    QString error;
    try {
        const auto handler = m_deformerKeyformHandler;
        const bool accepted = handler(id, type, index, &error);
        if (!alive) return false;
        if (accepted) return true;
    } catch (const std::exception& exception) {
        error = QStringLiteral("变形器关键形选择失败：%1").arg(QString::fromUtf8(exception.what()));
    }
    if (alive) RejectEdit(error);
    return false;
}

void CInspectorPanel::RejectEdit(const QString& error)
{
    {
        const QSignalBlocker blocker(this);
        m_projection->force = true;
        RefreshSelection();
        m_projection->force = false;
    }
    emit EditRejected(error.isEmpty() ? QStringLiteral("无法提交当前编辑，已保留原状态") : error);
}

bool CInspectorPanel::SubmitEdit(const live2dstruct::CModelEdit& edit)
{
    if (!m_document || m_submitting) return false;
    const QPointer<CInspectorPanel> alive(this);
    m_submitting = true;
    const auto guard = qScopeGuard([alive] { if (alive) alive->m_submitting = false; });
    QString error;
    try {
        const auto handler = m_editHandler;
        const bool accepted = handler ? handler(edit, &error) : m_document->ExecuteEdit(edit);
        if (!alive) return false;
        if (accepted) return true;
    } catch (const std::exception& exception) {
        error = QStringLiteral("编辑提交失败：%1").arg(QString::fromUtf8(exception.what()));
    }
    if (alive) RejectEdit(error);
    return false;
}

bool CInspectorPanel::SubmitHistory(bool undo, live2dstruct::CModelEdit& edit)
{
    if (!m_document || m_submitting) return false;
    const QPointer<CInspectorPanel> alive(this);
    m_submitting = true;
    const auto guard = qScopeGuard([alive] { if (alive) alive->m_submitting = false; });
    QString error;
    try {
        const auto handler = m_historyHandler;
        const bool accepted = handler ? handler(undo, edit, &error)
                                      : (undo ? m_document->UndoEdit(&edit) : m_document->RedoEdit(&edit));
        if (!alive) return false;
        if (accepted) return true;
    } catch (const std::exception& exception) {
        error = QStringLiteral("历史操作失败：%1").arg(QString::fromUtf8(exception.what()));
    }
    if (alive) RejectEdit(error);
    return false;
}

bool CInspectorPanel::RequestKeyform(const QString& name, int index)
{
    if (!m_keyformHandler) return true;
    if (m_submitting) return false;
    const QPointer<CInspectorPanel> alive(this);
    m_submitting = true;
    const auto guard = qScopeGuard([alive] { if (alive) alive->m_submitting = false; });
    const QString target = name;
    QString error;
    try {
        const auto handler = m_keyformHandler;
        const bool accepted = handler(target, index, &error);
        if (!alive) return false;
        if (accepted) return true;
    } catch (const std::exception& exception) {
        error = QStringLiteral("关键形选择失败：%1").arg(QString::fromUtf8(exception.what()));
    }
    if (alive) RejectEdit(error);
    return false;
}

void CInspectorPanel::SetSelection(const QString& name, const QString& type)
{
    const bool sameObject = m_selectionName == name && m_selectionType == type;
    m_selectionName = name;
    m_selectionType = type;
    m_selectionNames = {name};
    m_selectionTypes = {type};
    m_visibilityCheck->setTristate(false);
    showModelObject(name, type, sameObject);
}

void CInspectorPanel::SetSelections(const QStringList& names, const QStringList& types)
{
    if (names.size() <= 1) {
        if (names.isEmpty()) ClearSelection();
        else SetSelection(names.first(), types.value(0));
        return;
    }
    InvalidateProjection();
    m_renameDeformer->setEnabled(false);
    m_resizeWarpGrid->setEnabled(false);
    m_selectionName.clear();
    m_matrixSnapshot.reset();
    m_keyformMatrix->Clear();
    m_selectionType.clear();
    m_activeKeyformIndex = -1;
    m_activeDeformerKeyform = -1;
    m_selectionNames = names;
    m_selectionTypes = types;
    m_emptyLabel->setVisible(true);
    m_updatingKeyform = true;
    m_keyformCombo->clear();
    m_keyformCombo->setEnabled(false);
    m_vertexTable->setRowCount(0);
    m_rotationKeyformTable->setRowCount(0);
    setRowVisible(m_rotationKeyformTable, false);
    m_rotationKeyformTable->setEnabled(false);
    m_warpKeyformCombo->clear();
    m_warpKeyformCombo->setEnabled(false);
    m_warpKeyformTable->setRowCount(0);
    setRowVisible(m_warpKeyformTable, false);
    m_warpKeyformTable->setEnabled(false);
    m_mirrorHorizontal->setEnabled(false);
    m_mirrorVertical->setEnabled(false);
    m_interpolatePrevious->setEnabled(false);
    m_copyToNext->setEnabled(false);
    m_resetKeyform->setEnabled(false);
    m_copyKeyform->setEnabled(false);
    m_pasteKeyform->setEnabled(false);
    m_optimizeTopology->setEnabled(false);
    m_autoTriangulate->setEnabled(false);
    m_copyTopology->setEnabled(false);
    m_pasteTopology->setEnabled(false);
    ResetMaskControls();
    m_deformerParentCombo->setEnabled(false);
    m_moveVertices->setEnabled(false);
    m_mergeVertices->setEnabled(false);
    m_transformVertices->setEnabled(false);
    m_updatingKeyform = false;
    m_updatingVisibility = true;
    m_visibilityCheck->setTristate(true);
    bool anyVisible = false;
    bool anyHidden = false;
    bool allVisibleEditable = true;
    bool allArtMeshes = true;
    bool allUnlocked = true;
    bool anyUnlocked = false;
    const auto visibility = m_document ? m_document->VisibilityState() : QMap<QString, bool>();
    if (!m_document) {
        allVisibleEditable = false;
        allArtMeshes = false;
    }
    for (int i = 0; i < names.size(); ++i) {
        if (!m_document) break;
        if (i >= types.size()) {
            allVisibleEditable = false;
            allArtMeshes = false;
            continue;
        }
        const QString type = types.value(i);
        const QString name = names[i];
        const bool unlocked = !IsLocked(type, name);
        allUnlocked = allUnlocked && unlocked;
        anyUnlocked = anyUnlocked || unlocked;
        const auto value = visibility.constFind(type + QLatin1Char('|') + name);
        const bool found = value != visibility.cend();
        if (found) {
            anyVisible = anyVisible || value.value();
            anyHidden = anyHidden || !value.value();
        }
        if (!found) allVisibleEditable = false;
        allArtMeshes = allArtMeshes && type == QLatin1String("artmesh");
        if (!found) allArtMeshes = false;
    }
    if (anyVisible && anyHidden) m_visibilityCheck->setCheckState(Qt::PartiallyChecked);
    else m_visibilityCheck->setChecked(anyVisible);
    m_visibilityCheck->setEnabled(allVisibleEditable && anyUnlocked);
    m_opacitySpin->setEnabled(allArtMeshes && allUnlocked);
    m_drawOrderSpin->setEnabled(allArtMeshes && allUnlocked);
    if (allArtMeshes && !names.isEmpty()) {
        for (const auto& drawable : m_document->Drawables()) {
            if (drawable.id == names.first()) {
                m_opacitySpin->setValue(m_document->DrawableOpacity(drawable));
                m_drawOrderSpin->setValue(qRound(m_document->DrawableDrawOrder(drawable)));
                break;
            }
        }
    } else {
        m_opacitySpin->setValue(1.0);
        m_drawOrderSpin->setValue(0);
    }
    m_updatingVisibility = false;
    m_emptyLabel->setText((allUnlocked ? QStringLiteral("已选择 %1 个对象，可在结构树中继续调整范围") :
        QStringLiteral("已选择 %1 个对象，锁定对象不受批量修改影响")).arg(names.size()));
    for (QLabel* label : {m_typeLabel, m_idLabel, m_nameLabel, m_parentLabel, m_deformerLabel,
                          m_visibilityLabel, m_geometryLabel, m_textureLabel,
                          m_drawOrderLabel, m_blendLabel, m_maskLabel, m_keyformLabel}) {
        label->clear();
    }
    QSet<QString> uniqueTypes;
    for (const QString& type : types) uniqueTypes.insert(type);
    m_typeLabel->setText(uniqueTypes.values().join(QStringLiteral("、")));
    m_idLabel->setText(QStringLiteral("%1 个对象").arg(names.size()));
    updateObjectRows(this, QStringLiteral("multiple"));
}

void CInspectorPanel::RefreshSelection()
{
    if (m_selectionNames.size() > 1) SetSelections(m_selectionNames, m_selectionTypes);
    else if (!m_selectionName.isEmpty()) showModelObject(m_selectionName, m_selectionType);
}

bool CInspectorPanel::SyncActiveKeyform(const QString& id, const QString& type, int keyform, bool refresh)
{
    if (!m_document || keyform < 0) return false;
    int count = 0;
    if (type == QLatin1String("artmesh")) {
        for (const auto& mesh : m_document->Drawables()) if (mesh.id == id) { count = mesh.keyforms.size(); break; }
    } else if (type == QLatin1String("warp")) count = m_document->DeformerWarpKeyforms(id).size();
    else if (type == QLatin1String("rotation")) count = m_document->DeformerRotationKeyforms(id).size();
    if (keyform >= count) return false;
    const QSignalBlocker blocker(this), visibilityBlocker(m_visibilityCheck);
    m_selectionName = id;
    m_selectionType = type;
    m_selectionNames = {id};
    m_selectionTypes = {type};
    m_visibilityCheck->setTristate(false);
    m_activeKeyformIndex = type == QLatin1String("artmesh") ? keyform : -1;
    m_activeDeformerKeyform = type == QLatin1String("artmesh") ? -1 : keyform;
    if (refresh) showModelObject(id, type, true);
    return true;
}

void CInspectorPanel::SetLockedState(const QSet<QString>& lockedKeys)
{
    m_lockedKeys = lockedKeys;
    RefreshSelection();
}

bool CInspectorPanel::IsLocked(const QString& type, const QString& name) const
{
    return m_lockedKeys.contains(type + QLatin1Char('|') + name);
}

void CInspectorPanel::RefreshKeyformMatrix()
{
    m_editBindingKeys->setEnabled(false);
    live2dstruct::CKeyformGridSnapshot snapshot;
    if (!m_document || m_selectionName.isEmpty() ||
        !m_document->DescribeKeyformGrid(m_selectionName, m_selectionType, snapshot)) {
        m_matrixSnapshot.reset();
        m_keyformMatrix->Clear();
        return;
    }
    const int active = m_selectionType == QLatin1String("artmesh") ? m_activeKeyformIndex : m_activeDeformerKeyform;
    m_keyformMatrix->SetGrid(snapshot, active, IsLocked(m_selectionType, m_selectionName));
    m_editBindingKeys->setEnabled(snapshot.axesValid && snapshot.expectedKeyformCount == snapshot.keyformCount &&
        !IsLocked(m_selectionType, m_selectionName) && std::any_of(snapshot.axes.cbegin(), snapshot.axes.cend(),
            [](const auto& axis) { return axis.parameterType == 0; }));
    m_matrixSnapshot = std::make_shared<const live2dstruct::CKeyformGridSnapshot>(std::move(snapshot));
}

void CInspectorPanel::EditBindingKeys()
{
    if (!m_document || !m_editBindingKeys->isEnabled() || m_submitting || m_rebuildingTopology || m_editingBindingKeys) return;
    live2dstruct::CKeyformGridSnapshot snapshot;
    QString error;
    if (!m_document->DescribeKeyformGrid(m_selectionName, m_selectionType, snapshot, &error)) { RejectEdit(error); return; }
    const auto* document = m_document;
    const QString path = document->SourcePath();
    const int active = m_selectionType == QLatin1String("artmesh") ? m_activeKeyformIndex : m_activeDeformerKeyform;
    const QPointer<CInspectorPanel> alive(this);
    const QPointer<CBindingKeysDialog> pending(new CBindingKeysDialog(snapshot, active, this));
    connect(this, &CInspectorPanel::EditRejected, pending, &CBindingKeysDialog::SetError);
    connect(pending, &CBindingKeysDialog::ApplyRequested, this, [this, alive, pending, document, path, snapshot, active] {
        if (!alive || !pending || m_submitting) return;
        const int current = m_selectionType == QLatin1String("artmesh") ? m_activeKeyformIndex : m_activeDeformerKeyform;
        if (m_document != document || m_document->Revision() != snapshot.revision ||
            m_document->SourceDigest() != snapshot.sourceDigest || m_document->SourcePath() != path ||
            m_selectionName != snapshot.objectId || m_selectionType != snapshot.objectType || current != active ||
            IsLocked(m_selectionType, m_selectionName)) {
            pending->SetError(QStringLiteral("编辑期间模型、关键形或选区已改变，请关闭后重新打开"));
            return;
        }
        live2dstruct::CMocParameterStructureBuilder::EditBindingKeysRequest request;
        live2dstruct::CModelEdit edit;
        QString error;
        if (!pending->BuildRequest(request, &error) || !m_document->MakeBindingKeysEdit(request, edit, &error)) {
            pending->SetError(error);
            emit EditRejected(error);
            return;
        }
        if (!SubmitEdit(edit) || !alive || !pending) return;
        if (m_document != document || m_document->SourceDigest() != snapshot.sourceDigest || m_document->SourcePath() != path) {
            pending->SetError(QStringLiteral("提交期间模型已改变，请关闭后重新打开"));
            return;
        }
        pending->accept();
        RefreshSelection();
        if (!alive) return;
        emit StructureChanged();
        if (alive && m_document) emit HistoryChanged(m_document->CanUndoEdit(), m_document->CanRedoEdit());
    });
    m_editingBindingKeys = true;
    pending->exec();
    if (pending) delete pending.data();
    if (alive) m_editingBindingKeys = false;
}

void CInspectorPanel::RequestMatrixKeyform(int keyform)
{
    if (!m_document || !m_matrixSnapshot || m_submitting || m_rebuildingTopology ||
        IsLocked(m_selectionType, m_selectionName)) return;
    const auto previous = m_matrixSnapshot;
    live2dstruct::CKeyformGridSnapshot current;
    QString error;
    if (!m_document->DescribeKeyformGrid(m_selectionName, m_selectionType, current, &error)) {
        RejectEdit(error);
        return;
    }
    bool same = previous->objectId == current.objectId && previous->objectType == current.objectType &&
        previous->sourceDigest == current.sourceDigest && previous->keyformCount == current.keyformCount &&
        previous->axes.size() == current.axes.size();
    for (int axis = 0; same && axis < current.axes.size(); ++axis)
        same = previous->axes[axis].parameterId == current.axes[axis].parameterId &&
               previous->axes[axis].parameterType == current.axes[axis].parameterType &&
               previous->axes[axis].keys == current.axes[axis].keys;
    if (!same) {
        RejectEdit(QStringLiteral("关键形绑定已改变，请在更新后的矩阵中重新选择"));
        return;
    }
    QMap<QString, float> pose;
    if (!current.Pose(keyform, pose, &error)) { RejectEdit(error); return; }
    const QString id = m_selectionName, type = m_selectionType;
    const auto* document = m_document;
    const QString sourcePath = document->SourcePath();
    const QPointer<CInspectorPanel> alive(this);
    const bool accepted = type == QLatin1String("artmesh") ? RequestKeyform(id, keyform) : RequestDeformerKeyform(keyform);
    if (!alive) return;
    if (!accepted || m_document != document || m_selectionName != id || m_selectionType != type ||
        m_document->SourceDigest() != current.sourceDigest || m_document->SourcePath() != sourcePath) return;
    if (!SyncActiveKeyform(id, type, keyform)) {
        RejectEdit(QStringLiteral("关键形在预览提交期间已改变，请重新选择"));
        return;
    }
    if (type == QLatin1String("artmesh")) {
        emit DrawableKeyformActivated(id, keyform);
        emit DrawableKeyformSelected(id, keyform);
    }
}

void CInspectorPanel::RebuildKeyformTable()
{
    m_updatingKeyform = true;
    const auto reset = qScopeGuard([this] { m_updatingKeyform = false; });
    const auto positions = m_document && m_selectionType == QLatin1String("artmesh") &&
        !IsLocked(m_selectionType, m_selectionName)
        ? m_document->DrawableKeyformPositions(m_selectionName, m_activeKeyformIndex) : QVector<float>();
    if (positions.isEmpty() || positions.size() % 2) ClearGeometryTable(m_vertexTable);
    else UpdateGeometryTable(m_vertexTable, positions, 2);
    m_vertexTable->setEnabled(!positions.isEmpty());
}

void CInspectorPanel::RebuildRotationKeyformTable()
{
    m_updatingRotationKeyform = true;
    const auto reset = qScopeGuard([this] { m_updatingRotationKeyform = false; });
    if (!m_document || m_selectionType != QLatin1String("rotation") || m_selectionName.isEmpty()) {
        ClearGeometryTable(m_rotationKeyformTable);
        m_rotationKeyformTable->setEnabled(false);
        return;
    }
    const auto keyforms = m_document->DeformerRotationKeyforms(m_selectionName);
    QVector<float> values;
    values.reserve(keyforms.size() * 6);
    for (const auto& form : keyforms)
        values << form.angle << form.originX << form.originY << form.scale << float(form.reflectX) << float(form.reflectY);
    PrepareProjectionContext(m_activeDeformerKeyform);
    const auto previous = m_projection->tables.constFind(m_rotationKeyformTable);
    const bool changedContext = previous == m_projection->tables.cend() || !(previous->context == m_projection->context);
    UpdateGeometryTable(m_rotationKeyformTable, values, 6, true);
    m_rotationKeyformTable->setEnabled(!IsLocked(m_selectionType, m_selectionName));
    const bool restoreRejectedSelection = m_projection->force && m_rotationKeyformTable->currentRow() != m_activeDeformerKeyform;
    if ((changedContext || restoreRejectedSelection) && m_activeDeformerKeyform >= 0 && m_activeDeformerKeyform < keyforms.size())
        m_rotationKeyformTable->setCurrentCell(m_activeDeformerKeyform, 0);
}

void CInspectorPanel::ApplyRotationKeyformCellEdit(int row, int column)
{
    if (!m_document || m_selectionType != QLatin1String("rotation") || m_selectionName.isEmpty() ||
        row < 0 || column < 1 || column > 6 || IsLocked(m_selectionType, m_selectionName)) return;
    QVector<live2dstruct::CModelRotationKeyform> before =
        m_document->DeformerRotationKeyforms(m_selectionName);
    if (row >= before.size() || !m_rotationKeyformTable->item(row, column)) return;
    QVector<live2dstruct::CModelRotationKeyform> after = before;
    const QString text = m_rotationKeyformTable->item(row, column)->text().trimmed().toLower();
    if (column == 5 || column == 6) {
        bool value = false;
        if (text == QStringLiteral("是") || text == QStringLiteral("true") || text == QStringLiteral("1")) value = true;
        else if (text != QStringLiteral("否") && text != QStringLiteral("false") && text != QStringLiteral("0")) {
            m_projection->force = true;
            RebuildRotationKeyformTable();
            m_projection->force = false;
            return;
        }
        if (column == 5) after[row].reflectX = value;
        else after[row].reflectY = value;
    } else {
        bool ok = false;
        const double number = m_rotationKeyformTable->item(row, column)->text().toDouble(&ok);
        if (!ok || !std::isfinite(number) || std::abs(number) > std::numeric_limits<float>::max()) {
            m_projection->force = true;
            RebuildRotationKeyformTable();
            m_projection->force = false;
            return;
        }
        const float value = static_cast<float>(number);
        if (column == 1) after[row].angle = value;
        else if (column == 2) after[row].originX = value;
        else if (column == 3) after[row].originY = value;
        else if (column == 4) after[row].scale = value;
    }
    if (!after[row].IsValid()) {
        m_projection->force = true;
        RebuildRotationKeyformTable();
        m_projection->force = false;
        return;
    }
    if (after == before) return;
    const live2dstruct::CModelEdit command{m_selectionName, QStringLiteral("rotation"),
                                           live2dstruct::CModelEdit::RotationKeyforms,
                                           QVariant::fromValue(before), QVariant::fromValue(after), row};
    if (!SubmitEdit(command)) return;
    m_activeDeformerKeyform = row;
    m_keyformMatrix->SetActiveKeyform(row);
    emit HistoryChanged(m_document->CanUndoEdit(), m_document->CanRedoEdit());
    RebuildRotationKeyformTable();
}

void CInspectorPanel::RebuildWarpKeyformTable()
{
    m_updatingWarpKeyform = true;
    const auto reset = qScopeGuard([this] { m_updatingWarpKeyform = false; });
    const auto keyforms = m_document && m_selectionType == QLatin1String("warp")
        ? m_document->DeformerWarpKeyforms(m_selectionName) : QVector<live2dstruct::CModelWarpKeyform>();
    if (m_activeDeformerKeyform < 0 || m_activeDeformerKeyform >= keyforms.size()) {
        ClearGeometryTable(m_warpKeyformTable);
        m_warpKeyformTable->setEnabled(false);
        return;
    }
    UpdateGeometryTable(m_warpKeyformTable, keyforms[m_activeDeformerKeyform].positions, 2);
    m_warpKeyformTable->setEnabled(!IsLocked(m_selectionType, m_selectionName));
}

void CInspectorPanel::ApplyWarpKeyformCellEdit(int row, int column)
{
    if (!m_document || m_selectionType != QLatin1String("warp") || m_selectionName.isEmpty() ||
        row < 0 || (column != 1 && column != 2) || IsLocked(m_selectionType, m_selectionName)) return;
    const int keyformIndex = m_activeDeformerKeyform;
    QVector<live2dstruct::CModelWarpKeyform> before = m_document->DeformerWarpKeyforms(m_selectionName);
    if (keyformIndex < 0 || keyformIndex >= before.size() || !m_warpKeyformTable->item(row, column)) return;
    QVector<live2dstruct::CModelWarpKeyform> after = before;
    bool ok = false;
    const double number = m_warpKeyformTable->item(row, column)->text().toDouble(&ok);
    if (!ok || !std::isfinite(number) || std::abs(number) > std::numeric_limits<float>::max()) {
        m_projection->force = true;
        RebuildWarpKeyformTable();
        m_projection->force = false;
        return;
    }
    after[keyformIndex].positions[row * 2 + (column - 1)] = float(number);
    if (after == before) return;
    const live2dstruct::CModelEdit command{m_selectionName, QStringLiteral("warp"),
                                           live2dstruct::CModelEdit::WarpKeyforms,
                                           QVariant::fromValue(before), QVariant::fromValue(after), keyformIndex};
    if (!SubmitEdit(command)) return;
    emit HistoryChanged(m_document->CanUndoEdit(), m_document->CanRedoEdit());
    RebuildWarpKeyformTable();
}

void CInspectorPanel::ApplyKeyformCellEdit(int row, int column)
{
    if (!m_document || m_selectionType != QLatin1String("artmesh") || m_selectionName.isEmpty() ||
        IsLocked(m_selectionType, m_selectionName) ||
        (column != 1 && column != 2) || row < 0 || row >= m_vertexTable->rowCount()) return;
    const int keyformIndex = m_activeKeyformIndex;
    QVector<float> before = m_document->DrawableKeyformPositions(m_selectionName, keyformIndex);
    if (keyformIndex < 0 || before.size() != m_vertexTable->rowCount() * 2) return;
    bool ok = false;
    const double parsed = m_vertexTable->item(row, column)->text().toDouble(&ok);
    if (!ok || !std::isfinite(parsed) || std::abs(parsed) > std::numeric_limits<float>::max()) {
        m_updatingKeyform = true;
        m_vertexTable->item(row, column)->setText(QString::number(before[row * 2 + column - 1], 'g', 9));
        m_updatingKeyform = false;
        return;
    }
    QVector<float> after = before;
    after[row * 2 + column - 1] = static_cast<float>(parsed);
    if (before[row * 2 + column - 1] == after[row * 2 + column - 1]) return;
    const live2dstruct::CModelEdit command{m_selectionName, QStringLiteral("artmesh"),
                                           live2dstruct::CModelEdit::KeyformPositions,
                                           QVariant::fromValue(before), QVariant::fromValue(after), keyformIndex};
    if (!SubmitEdit(command)) return;
    emit DrawableKeyformPositionsChanged(m_selectionName, keyformIndex, after);
    emit HistoryChanged(m_document->CanUndoEdit(), m_document->CanRedoEdit());
}

void CInspectorPanel::MirrorKeyform(bool horizontal)
{
    if (!m_document || m_selectionType != QLatin1String("artmesh") || m_selectionName.isEmpty() ||
        IsLocked(m_selectionType, m_selectionName)) return;
    const int keyformIndex = m_activeKeyformIndex;
    const QVector<float> before = m_document->DrawableKeyformPositions(m_selectionName, keyformIndex);
    if (keyformIndex < 0 || before.isEmpty() || before.size() % 2 != 0) return;
    float minimum = before[horizontal ? 0 : 1];
    float maximum = minimum;
    for (int i = horizontal ? 0 : 1; i < before.size(); i += 2) {
        minimum = qMin(minimum, before[i]);
        maximum = qMax(maximum, before[i]);
    }
    const double sum = double(minimum) + maximum;
    QVector<float> after = before;
    for (int i = horizontal ? 0 : 1; i < after.size(); i += 2) after[i] = float(sum - after[i]);
    if (after == before) return;
    const live2dstruct::CModelEdit command{m_selectionName, QStringLiteral("artmesh"),
                                           live2dstruct::CModelEdit::KeyformPositions,
                                           QVariant::fromValue(before), QVariant::fromValue(after), keyformIndex, false};
    if (!SubmitEdit(command)) return;
    RebuildKeyformTable();
    emit DrawableKeyformPositionsChanged(m_selectionName, keyformIndex, after);
    emit HistoryChanged(m_document->CanUndoEdit(), m_document->CanRedoEdit());
}

void CInspectorPanel::InterpolatePreviousKeyform()
{
    if (!m_document || m_selectionType != QLatin1String("artmesh") || m_selectionName.isEmpty() ||
        IsLocked(m_selectionType, m_selectionName)) return;
    const int keyformIndex = m_activeKeyformIndex;
    if (keyformIndex <= 0) return;
    const QVector<float> before = m_document->DrawableKeyformPositions(m_selectionName, keyformIndex);
    const QVector<float> previous = m_document->DrawableKeyformPositions(m_selectionName, keyformIndex - 1);
    if (before.isEmpty() || before.size() != previous.size()) return;
    QVector<float> after;
    after.resize(before.size());
    for (int i = 0; i < before.size(); ++i) after[i] = float((double(before[i]) + previous[i]) * 0.5);
    if (after == before) return;
    const live2dstruct::CModelEdit command{m_selectionName, QStringLiteral("artmesh"),
                                           live2dstruct::CModelEdit::KeyformPositions,
                                           QVariant::fromValue(before), QVariant::fromValue(after), keyformIndex, false};
    if (!SubmitEdit(command)) return;
    RebuildKeyformTable();
    emit DrawableKeyformPositionsChanged(m_selectionName, keyformIndex, after);
    emit HistoryChanged(m_document->CanUndoEdit(), m_document->CanRedoEdit());
}

void CInspectorPanel::CopyKeyformToNext()
{
    if (!m_document || m_selectionType != QLatin1String("artmesh") || m_selectionName.isEmpty() ||
        IsLocked(m_selectionType, m_selectionName)) return;
    const int keyformIndex = m_activeKeyformIndex;
    const QVector<float> source = m_document->DrawableKeyformPositions(m_selectionName, keyformIndex);
    const QVector<float> before = m_document->DrawableKeyformPositions(m_selectionName, keyformIndex + 1);
    if (keyformIndex < 0 || source.isEmpty() || source.size() != before.size()) return;
    const live2dstruct::CModelEdit command{m_selectionName, QStringLiteral("artmesh"),
                                           live2dstruct::CModelEdit::KeyformPositions,
                                           QVariant::fromValue(before), QVariant::fromValue(source), keyformIndex + 1, false};
    if (!SubmitEdit(command)) return;
    emit DrawableKeyformPositionsChanged(m_selectionName, keyformIndex + 1, source);
    emit DrawableKeyformActivated(m_selectionName, keyformIndex);
    emit HistoryChanged(m_document->CanUndoEdit(), m_document->CanRedoEdit());
}

void CInspectorPanel::ResetCurrentKeyform()
{
    if (!m_document || m_selectionType != QLatin1String("artmesh") || m_selectionName.isEmpty() ||
        IsLocked(m_selectionType, m_selectionName)) return;
    const int keyformIndex = m_activeKeyformIndex;
    const QVector<float> before = m_document->DrawableKeyformPositions(m_selectionName, keyformIndex);
    QVector<float> original;
    for (const auto& drawable : m_document->Drawables()) {
        if (drawable.id == m_selectionName && keyformIndex >= 0 && keyformIndex < drawable.keyforms.size()) {
            original = drawable.keyforms.at(keyformIndex).positions;
            break;
        }
    }
    if (before.isEmpty() || original.isEmpty() || before.size() != original.size() || before == original) return;
    const live2dstruct::CModelEdit command{m_selectionName, QStringLiteral("artmesh"),
                                           live2dstruct::CModelEdit::KeyformPositions,
                                           QVariant::fromValue(before), QVariant::fromValue(original), keyformIndex, false};
    if (!SubmitEdit(command)) return;
    RebuildKeyformTable();
    emit DrawableKeyformPositionsChanged(m_selectionName, keyformIndex, original);
    emit HistoryChanged(m_document->CanUndoEdit(), m_document->CanRedoEdit());
}

void CInspectorPanel::CopyCurrentKeyform()
{
    if (!m_document || m_selectionType != QLatin1String("artmesh") || m_selectionName.isEmpty()) return;
    const int keyformIndex = m_activeKeyformIndex;
    const QVector<float> positions = m_document->DrawableKeyformPositions(m_selectionName, keyformIndex);
    if (keyformIndex < 0 || positions.isEmpty() || positions.size() % 2 != 0) return;
    QJsonArray values;
    for (float value : positions) values.append(static_cast<double>(value));
    QJsonObject object;
    object.insert(QStringLiteral("format"), QStringLiteral("live2d2psd-keyform-v1"));
    object.insert(QStringLiteral("vertexCount"), positions.size() / 2);
    object.insert(QStringLiteral("positions"), values);
    QApplication::clipboard()->setText(QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact)));
}

void CInspectorPanel::OptimizeTopology()
{
    if (!m_document || m_rebuildingTopology || m_submitting || m_selectionType != QLatin1String("artmesh") ||
        m_selectionName.isEmpty() || m_activeKeyformIndex < 0 || IsLocked(m_selectionType, m_selectionName)) return;
    using Planner = live2dstruct::CMeshEditPlanner;
    const Planner::Input input{m_selectionName, m_document->DrawableKeyformPositions(m_selectionName, m_activeKeyformIndex),
                               m_document->DrawableIndices(m_selectionName), {}};
    QVector<int> triangles;
    triangles.reserve(input.indices.size() / 3);
    for (int index = 0; index < input.indices.size() / 3; ++index) triangles.append(index);
    Planner::Result planned;
    Planner::Diagnostic diagnostic;
    if (!Planner::Retriangulate(input, triangles, planned, &diagnostic)) {
        RejectEdit(diagnostic.message);
        return;
    }
    ApplyTopology(input.indices, planned.edit.indices);
}

void CInspectorPanel::AutoTriangulate()
{
    if (!m_document || m_rebuildingTopology || m_submitting || m_selectionType != QLatin1String("artmesh") ||
        m_selectionName.isEmpty() || m_activeKeyformIndex < 0 || IsLocked(m_selectionType, m_selectionName)) return;
    using Planner = live2dstruct::CMeshEditPlanner;
    const auto* document = m_document;
    const quint64 revision = document->Revision();
    const QByteArray sourceDigest = document->SourceDigest();
    const QString sourcePath = document->SourcePath();
    const QString name = m_selectionName;
    const int keyform = m_activeKeyformIndex;
    const Planner::Input input{name, document->DrawableKeyformPositions(name, keyform), document->DrawableIndices(name), {}};
    Planner::Result planned;
    Planner::Diagnostic diagnostic;
    if (!Planner::TriangulateVertices(input, planned, &diagnostic)) {
        RejectEdit(diagnostic.message);
        return;
    }
    if (planned.edit.indices == input.indices) return;
    auto* dialog = new QMessageBox(QMessageBox::Question, QStringLiteral("重建全部连线"),
        QStringLiteral("将用 %1 个顶点重新生成 %2 个三角形，替换现有的 %3 个三角形。")
            .arg(input.positions.size() / 2).arg(planned.edit.indices.size() / 3).arg(input.indices.size() / 3),
        QMessageBox::Yes | QMessageBox::Cancel, this);
    dialog->setObjectName(QStringLiteral("rebuildTopologyConfirmation"));
    dialog->setTextFormat(Qt::PlainText);
    dialog->setInformativeText(QStringLiteral("这会重新连接全部顶点，可能填补孔洞、扩大外轮廓或连接分离区域。"
                                              "顶点位置和 UV 保持不变，结果应用于所有关键形，可撤销。"));
    dialog->button(QMessageBox::Yes)->setText(QStringLiteral("重建连线"));
    dialog->button(QMessageBox::Cancel)->setText(QStringLiteral("保留当前网格"));
    dialog->setDefaultButton(QMessageBox::Cancel);
    dialog->setEscapeButton(QMessageBox::Cancel);
    const QPointer<CInspectorPanel> alive(this);
    const QPointer<QMessageBox> pending(dialog);
    m_rebuildingTopology = true;
    const int choice = dialog->exec();
    if (pending) delete pending.data();
    if (!alive) return;
    m_rebuildingTopology = false;
    if (choice != QMessageBox::Yes) return;
    if (m_document != document || m_document->Revision() != revision || m_document->SourceDigest() != sourceDigest ||
        m_document->SourcePath() != sourcePath || m_selectionName != name ||
        m_selectionType != QLatin1String("artmesh") || m_activeKeyformIndex != keyform || IsLocked(m_selectionType, name) ||
        m_document->DrawableIndices(name) != input.indices || m_document->DrawableKeyformPositions(name, keyform) != input.positions) {
        RejectEdit(QStringLiteral("确认期间模型、关键形或选区已改变，请重新操作"));
        return;
    }
    ApplyTopology(input.indices, planned.edit.indices);
}

void CInspectorPanel::ApplyTopology(const QVector<quint16>& before, const QVector<quint16>& after)
{
    if (before == after) return;
    const live2dstruct::CModelEdit command{m_selectionName, QStringLiteral("artmesh"),
                                           live2dstruct::CModelEdit::MeshIndices,
                                           QVariant::fromValue(before), QVariant::fromValue(after), -1, false};
    if (!SubmitEdit(command)) return;
    emit DrawableIndicesChanged(m_selectionName, after);
    emit HistoryChanged(m_document->CanUndoEdit(), m_document->CanRedoEdit());
}

void CInspectorPanel::CopyTopology()
{
    if (!m_document || m_selectionType != QLatin1String("artmesh") || m_selectionName.isEmpty()) return;
    const QVector<quint16> indices = m_document->DrawableIndices(m_selectionName);
    if (indices.isEmpty() || indices.size() % 3 != 0) return;
    QJsonArray values;
    for (quint16 index : indices) values.append(static_cast<int>(index));
    int vertexCount = 0;
    for (const auto& drawable : m_document->Drawables()) {
        if (drawable.id == m_selectionName) { vertexCount = drawable.vertexCount; break; }
    }
    QJsonObject object;
    object.insert(QStringLiteral("format"), QStringLiteral("live2d2psd-topology-v1"));
    object.insert(QStringLiteral("vertexCount"), vertexCount);
    object.insert(QStringLiteral("indices"), values);
    QApplication::clipboard()->setText(QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact)));
}

void CInspectorPanel::PasteTopology()
{
    if (!m_document || m_selectionType != QLatin1String("artmesh") || m_selectionName.isEmpty() ||
        IsLocked(m_selectionType, m_selectionName)) return;
    const QVector<quint16> before = m_document->DrawableIndices(m_selectionName);
    int vertexCount = 0;
    for (const auto& drawable : m_document->Drawables()) {
        if (drawable.id == m_selectionName) { vertexCount = drawable.vertexCount; break; }
    }
    if (vertexCount <= 0) return;
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(QApplication::clipboard()->text().toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        QMessageBox::warning(this, QStringLiteral("粘贴拓扑失败"), QStringLiteral("剪贴板不是有效的拓扑数据"));
        return;
    }
    const QJsonObject object = document.object();
    if (object.value(QStringLiteral("format")).toString() != QStringLiteral("live2d2psd-topology-v1")) {
        QMessageBox::warning(this, QStringLiteral("粘贴拓扑失败"), QStringLiteral("剪贴板格式不受支持"));
        return;
    }
    if (object.value(QStringLiteral("vertexCount")).toInt() != vertexCount) {
        QMessageBox::warning(this, QStringLiteral("粘贴拓扑失败"), QStringLiteral("目标网格的顶点数量不同"));
        return;
    }
    const QJsonArray values = object.value(QStringLiteral("indices")).toArray();
    if (values.isEmpty() || values.size() % 3 != 0 || values.size() > 3000000) {
        QMessageBox::warning(this, QStringLiteral("粘贴拓扑失败"), QStringLiteral("三角形索引数量无效"));
        return;
    }
    QVector<quint16> after;
    after.reserve(values.size());
    for (const QJsonValue& value : values) {
        const double number = value.toDouble(-1.0);
        if (!value.isDouble() || std::floor(number) != number || number < 0.0 || number >= vertexCount || number > live2dstruct::CMeshTopology::MaximumIndex) {
            QMessageBox::warning(this, QStringLiteral("粘贴拓扑失败"), QStringLiteral("索引超出目标网格范围"));
            return;
        }
        after.append(static_cast<quint16>(number));
    }
    if (!live2dstruct::CMeshTopology::IsValid(after, vertexCount)) {
        QMessageBox::warning(this, QStringLiteral("粘贴拓扑失败"), QStringLiteral("索引超出目标网格范围"));
        return;
    }
    if (after == before) return;
    const live2dstruct::CModelEdit command{m_selectionName, QStringLiteral("artmesh"),
                                           live2dstruct::CModelEdit::MeshIndices,
                                           QVariant::fromValue(before), QVariant::fromValue(after), -1, false};
    if (!SubmitEdit(command)) return;
    emit DrawableIndicesChanged(m_selectionName, after);
    emit HistoryChanged(m_document->CanUndoEdit(), m_document->CanRedoEdit());
}

void CInspectorPanel::PasteCurrentKeyform()
{
    if (!m_document || m_selectionType != QLatin1String("artmesh") || m_selectionName.isEmpty() ||
        IsLocked(m_selectionType, m_selectionName)) return;
    const int keyformIndex = m_activeKeyformIndex;
    const QVector<float> before = m_document->DrawableKeyformPositions(m_selectionName, keyformIndex);
    if (keyformIndex < 0 || before.isEmpty()) return;
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(QApplication::clipboard()->text().toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        QMessageBox::warning(this, QStringLiteral("粘贴关键形失败"), QStringLiteral("剪贴板不是有效的关键形数据"));
        return;
    }
    const QJsonObject object = document.object();
    if (object.value(QStringLiteral("format")).toString() != QStringLiteral("live2d2psd-keyform-v1")) {
        QMessageBox::warning(this, QStringLiteral("粘贴关键形失败"), QStringLiteral("剪贴板格式不受支持"));
        return;
    }
    const QJsonArray values = object.value(QStringLiteral("positions")).toArray();
    if (values.size() != before.size() || object.value(QStringLiteral("vertexCount")).toInt() != before.size() / 2) {
        QMessageBox::warning(this, QStringLiteral("粘贴关键形失败"), QStringLiteral("目标网格的顶点数量不同"));
        return;
    }
    QVector<float> after;
    after.reserve(values.size());
    for (const QJsonValue& value : values) {
        if (!value.isDouble() || !std::isfinite(value.toDouble())) {
            QMessageBox::warning(this, QStringLiteral("粘贴关键形失败"), QStringLiteral("关键形包含无效顶点坐标"));
            return;
        }
        after.append(static_cast<float>(value.toDouble()));
    }
    if (after == before) return;
    const live2dstruct::CModelEdit command{m_selectionName, QStringLiteral("artmesh"),
                                           live2dstruct::CModelEdit::KeyformPositions,
                                           QVariant::fromValue(before), QVariant::fromValue(after), keyformIndex, false};
    if (!SubmitEdit(command)) return;
    RebuildKeyformTable();
    emit DrawableKeyformPositionsChanged(m_selectionName, keyformIndex, after);
    emit HistoryChanged(m_document->CanUndoEdit(), m_document->CanRedoEdit());
}

void CInspectorPanel::ClearSelection()
{
    m_renameDeformer->setEnabled(false);
    m_resizeWarpGrid->setEnabled(false);
    InvalidateProjection();
    m_matrixSnapshot.reset();
    m_keyformMatrix->Clear();
    m_editBindingKeys->setEnabled(false);
    m_activeKeyformIndex = -1;
    m_activeDeformerKeyform = -1;
    m_selectionName.clear();
    m_selectionType.clear();
    m_selectionNames.clear();
    m_selectionTypes.clear();
    m_emptyLabel->setText(QStringLiteral("从模型结构中选择一个对象"));
    m_emptyLabel->setVisible(true);
    m_updatingVisibility = true;
    m_visibilityCheck->setTristate(false);
    m_visibilityCheck->setChecked(false);
    m_opacitySpin->setValue(1.0);
    m_visibilityCheck->setEnabled(false);
    m_opacitySpin->setEnabled(false);
    m_drawOrderSpin->setValue(0);
    m_drawOrderSpin->setEnabled(false);
    m_updatingVisibility = false;
    m_updatingKeyform = true;
    m_keyformCombo->clear();
    m_keyformCombo->setEnabled(false);
    m_vertexTable->setRowCount(0);
    m_rotationKeyformTable->setRowCount(0);
    setRowVisible(m_rotationKeyformTable, false);
    m_rotationKeyformTable->setEnabled(false);
    m_warpKeyformCombo->clear();
    m_warpKeyformCombo->setEnabled(false);
    m_warpKeyformTable->setRowCount(0);
    setRowVisible(m_warpKeyformTable, false);
    m_warpKeyformTable->setEnabled(false);
    m_mirrorHorizontal->setEnabled(false);
    m_mirrorVertical->setEnabled(false);
    m_interpolatePrevious->setEnabled(false);
    m_copyToNext->setEnabled(false);
    m_resetKeyform->setEnabled(false);
    m_copyKeyform->setEnabled(false);
    m_pasteKeyform->setEnabled(false);
    m_optimizeTopology->setEnabled(false);
    m_autoTriangulate->setEnabled(false);
    m_copyTopology->setEnabled(false);
    m_pasteTopology->setEnabled(false);
    ResetMaskControls();
    m_deformerParentCombo->setEnabled(false);
    m_moveVertices->setEnabled(false);
    m_mergeVertices->setEnabled(false);
    m_transformVertices->setEnabled(false);
    m_updatingKeyform = false;
    for (QLabel* label : {m_typeLabel, m_idLabel, m_nameLabel, m_parentLabel, m_deformerLabel,
                          m_visibilityLabel, m_geometryLabel, m_textureLabel,
                          m_drawOrderLabel, m_blendLabel, m_maskLabel, m_keyformLabel}) {
        label->clear();
    }
    updateObjectRows(this, QString());
}

void CInspectorPanel::EmitEdit(const live2dstruct::CModelEdit& edit, bool forward)
{
    const QVariant& value = forward ? edit.after : edit.before;
    if (edit.field == live2dstruct::CModelEdit::Visibility) {
        emit VisibilityChanged(edit.name, edit.type, value.toBool());
    } else if (edit.field == live2dstruct::CModelEdit::VisibilityValues) {
        emit VisibilityBatchChanged(value.value<QMap<QString, bool>>());
    } else if (edit.field == live2dstruct::CModelEdit::Opacity) {
        emit DrawableOpacityChanged(edit.name, value.toFloat());
    } else if (edit.field == live2dstruct::CModelEdit::DrawOrder) {
        emit DrawableDrawOrderChanged(edit.name, value.toInt());
    } else if (edit.field == live2dstruct::CModelEdit::KeyformPositions) {
        emit DrawableKeyformPositionsChanged(edit.name, edit.index, value.value<QVector<float>>());
    } else if (edit.field == live2dstruct::CModelEdit::MeshIndices) {
        emit DrawableIndicesChanged(edit.name, value.value<QVector<quint16>>());
    } else if (edit.field == live2dstruct::CModelEdit::MaskIndices) {
        emit DrawableMaskIndicesChanged(edit.name, value.value<QVector<int>>());
    }
    RefreshSelection();
}

void CInspectorPanel::ClearHistory()
{
    if (m_document) m_document->ClearEditHistory();
    emit HistoryChanged(false, false);
}

void CInspectorPanel::SyncHistoryState()
{
    if (!m_document) {
        emit HistoryChanged(false, false);
        return;
    }
    emit HistoryChanged(m_document->CanUndoEdit(), m_document->CanRedoEdit());
}

void CInspectorPanel::ResetMaskControls()
{
    const QSignalBlocker blocker(m_maskInvertedCheck);
    m_editMasks->setEnabled(false);
    m_maskInvertedCheck->setEnabled(false);
    m_maskInvertedCheck->setChecked(false);
}

void CInspectorPanel::EditMasks()
{
    if (!m_document || m_selectionType != QLatin1String("artmesh") || m_selectionName.isEmpty() ||
        IsLocked(m_selectionType, m_selectionName)) return;
    const auto* document = m_document;
    const auto source = document->SourceDigest();
    const auto sourcePath = document->SourcePath();
    const auto revision = document->Revision();
    const QPointer<CInspectorPanel> alive(this);
    const QString target = m_selectionName;
    const QVector<int> current = document->DrawableMaskIndices(target);
    auto* dialog = new QDialog(this);
    const QPointer<QDialog> pending(dialog);
    const auto cleanup = qScopeGuard([pending] { if (pending) delete pending.data(); });
    dialog->setObjectName(QStringLiteral("maskEditorDialog"));
    dialog->setWindowTitle(QStringLiteral("编辑蒙版 · %1").arg(target));
    dialog->resize(400, 480);
    auto* layout = new QVBoxLayout(dialog);
    auto* hint = new QLabel(QStringLiteral("选择裁剪此网格的蒙版。多个蒙版会合并可见区域。"), dialog);
    hint->setTextFormat(Qt::PlainText);
    hint->setWordWrap(true);
    layout->addWidget(hint);
    auto* search = new QLineEdit(dialog);
    search->setObjectName(QStringLiteral("maskSearch"));
    search->setPlaceholderText(QStringLiteral("搜索网格 ID"));
    search->setClearButtonEnabled(true);
    layout->addWidget(search);
    auto* selectedOnly = new QCheckBox(QStringLiteral("仅显示已选"), dialog);
    selectedOnly->setObjectName(QStringLiteral("selectedMasksOnly"));
    layout->addWidget(selectedOnly);
    auto* list = new QListWidget(dialog);
    list->setObjectName(QStringLiteral("maskList"));
    list->setAccessibleName(QStringLiteral("可用蒙版"));
    const QSet<int> selected(current.cbegin(), current.cend());
    QHash<int, int> occurrences;
    for (int index : current) ++occurrences[index];
    QStringList duplicates;
    const auto& drawables = document->Drawables();
    for (int index = 0; index < drawables.size(); ++index) {
        const bool self = drawables[index].id == target;
        if (self && !selected.contains(index)) continue;
        QString label = self ? QStringLiteral("%1（自身 · 原始引用）").arg(drawables[index].id) : drawables[index].id;
        if (occurrences.value(index) > 1) {
            duplicates.append(QStringLiteral("%1：%2 次").arg(drawables[index].id).arg(occurrences.value(index)));
            label += QStringLiteral("（原有 %1 次）").arg(occurrences.value(index));
        }
        auto* item = new QListWidgetItem(label, list);
        item->setToolTip(QStringLiteral("<qt>") + label.toHtmlEscaped() + QStringLiteral("</qt>"));
        item->setData(Qt::UserRole, index);
        item->setCheckState(selected.contains(index) ? Qt::Checked : Qt::Unchecked);
    }
    QSet<int> added;
    for (int index : current) {
        if ((index >= 0 && index < drawables.size()) || added.contains(index)) continue;
        added.insert(index);
        QString label = QStringLiteral("无效索引 %1（原始引用）").arg(index);
        if (occurrences.value(index) > 1) {
            duplicates.append(QStringLiteral("无效索引 %1：%2 次").arg(index).arg(occurrences.value(index)));
            label += QStringLiteral("（原有 %1 次）").arg(occurrences.value(index));
        }
        auto* item = new QListWidgetItem(label, list);
        item->setData(Qt::UserRole, index);
        item->setCheckState(Qt::Checked);
    }
    layout->addWidget(list, 1);
    auto* deduplicate = new QCheckBox(QStringLiteral("去除重复引用"), dialog);
    deduplicate->setObjectName(QStringLiteral("deduplicateMasks"));
    deduplicate->setEnabled(!duplicates.isEmpty());
    deduplicate->setToolTip(QStringLiteral("仅保留每个已选蒙版的第一次引用，可能改变合成结果；应用后可撤销。"));
    layout->addWidget(deduplicate);
    if (!duplicates.isEmpty()) {
        auto* detail = new QPlainTextEdit(dialog);
        detail->setObjectName(QStringLiteral("maskDuplicateDetails"));
        detail->setReadOnly(true);
        detail->setLineWrapMode(QPlainTextEdit::WidgetWidth);
        detail->setPlainText(QStringLiteral("当前按原序列保留重复引用，可能影响合成结果。取消勾选某个蒙版会移除它的全部引用。\n%1").arg(duplicates.join(QLatin1Char('\n'))));
        detail->setMaximumHeight(detail->fontMetrics().lineSpacing() * 5);
        layout->addWidget(detail);
    }
    auto* footer = new QHBoxLayout();
    auto* count = new QLabel(dialog);
    count->setObjectName(QStringLiteral("maskSelectionCount"));
    auto* clear = new QPushButton(QStringLiteral("清空选择"), dialog);
    clear->setObjectName(QStringLiteral("clearMasks"));
    footer->addWidget(count, 1);
    footer->addWidget(clear);
    layout->addLayout(footer);
    const auto refresh = [list, search, selectedOnly, count, clear, deduplicate, occurrences] {
        const QString query = search->text().trimmed();
        int selected = 0, visible = 0, references = 0;
        for (int row = 0; row < list->count(); ++row) {
            auto* item = list->item(row);
            const bool checked = item->checkState() == Qt::Checked;
            if (checked) {
                ++selected;
                references += deduplicate->isChecked() ? 1 : std::max(1, occurrences.value(item->data(Qt::UserRole).toInt()));
            }
            const bool shown = item->text().contains(query, Qt::CaseInsensitive) && (!selectedOnly->isChecked() || checked);
            item->setHidden(!shown);
            if (shown) ++visible;
        }
        count->setText(QStringLiteral("已选 %1 个蒙版，共 %2 次引用 · 显示 %3 / %4").arg(selected).arg(references).arg(visible).arg(list->count()));
        count->setWordWrap(true);
        clear->setEnabled(selected > 0);
    };
    connect(search, &QLineEdit::textChanged, dialog, refresh);
    connect(selectedOnly, &QCheckBox::toggled, dialog, refresh);
    connect(deduplicate, &QCheckBox::toggled, dialog, refresh);
    connect(list, &QListWidget::itemChanged, dialog, refresh);
    connect(clear, &QPushButton::clicked, dialog, [list, refresh] {
        const QSignalBlocker blocker(list);
        for (int row = 0; row < list->count(); ++row) list->item(row)->setCheckState(Qt::Unchecked);
        refresh();
    });
    refresh();
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("应用"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    search->setFocus();
    if (dialog->exec() != QDialog::Accepted || !alive || !pending) return;
    if (m_document != document || m_document->SourceDigest() != source || m_document->SourcePath() != sourcePath ||
        m_document->Revision() != revision || m_selectionName != target || m_selectionType != QLatin1String("artmesh") ||
        IsLocked(m_selectionType, target) ||
        m_document->DrawableMaskIndices(target) != current) {
        RejectEdit(QStringLiteral("模型或蒙版已改变，请重新打开蒙版编辑器"));
        return;
    }
    QSet<int> checked;
    for (int row = 0; row < list->count(); ++row)
        if (list->item(row)->checkState() == Qt::Checked) checked.insert(list->item(row)->data(Qt::UserRole).toInt());
    if (checked == selected && !deduplicate->isChecked()) return;
    QVector<int> after;
    QSet<int> retained;
    for (int index : current) if (checked.contains(index)) {
        if (!deduplicate->isChecked() || !retained.contains(index)) after.append(index);
        retained.insert(index);
    }
    checked.subtract(retained);
    for (int row = 0; row < list->count(); ++row) {
        const int index = list->item(row)->data(Qt::UserRole).toInt();
        if (checked.remove(index)) after.append(index);
    }
    if (after == current) return;
    const live2dstruct::CModelEdit command{target, QStringLiteral("artmesh"),
                                           live2dstruct::CModelEdit::MaskIndices,
                                           QVariant::fromValue(current), QVariant::fromValue(after)};
    if (!SubmitEdit(command)) return;
    emit DrawableMaskIndicesChanged(target, after);
    if (!alive) return;
    emit HistoryChanged(m_document->CanUndoEdit(), m_document->CanRedoEdit());
    if (alive) showModelObject(m_selectionName, m_selectionType);
}

void CInspectorPanel::TranslateSelectedVertices()
{
    if (!m_document || m_selectionType != QLatin1String("artmesh") || m_selectionName.isEmpty() ||
        IsLocked(m_selectionType, m_selectionName) || !m_vertexDeltaX || !m_vertexDeltaY) return;
    const float deltaX = static_cast<float>(m_vertexDeltaX->value());
    const float deltaY = static_cast<float>(m_vertexDeltaY->value());
    if (!std::isfinite(deltaX) || !std::isfinite(deltaY) || (deltaX == 0.0f && deltaY == 0.0f)) return;
    const int keyformIndex = m_activeKeyformIndex;
    const QVector<float> before = m_document->DrawableKeyformPositions(m_selectionName, keyformIndex);
    if (keyformIndex < 0 || before.isEmpty() || before.size() % 2 != 0) return;
    QSet<int> rows;
    for (const QTableWidgetSelectionRange& range : m_vertexTable->selectedRanges()) {
        for (int row = range.topRow(); row <= range.bottomRow(); ++row) rows.insert(row);
    }
    if (rows.isEmpty()) return;
    QVector<float> after = before;
    for (int row : rows) {
        if (row < 0 || row * 2 + 1 >= after.size()) continue;
        after[row * 2] += deltaX;
        after[row * 2 + 1] += deltaY;
    }
    const live2dstruct::CModelEdit command{m_selectionName, QStringLiteral("artmesh"),
                                           live2dstruct::CModelEdit::KeyformPositions,
                                           QVariant::fromValue(before), QVariant::fromValue(after), keyformIndex, false};
    if (!SubmitEdit(command)) return;
    RebuildKeyformTable();
    emit DrawableKeyformPositionsChanged(m_selectionName, keyformIndex, after);
    emit HistoryChanged(m_document->CanUndoEdit(), m_document->CanRedoEdit());
}

void CInspectorPanel::MergeSelectedVertices()
{
    if (!m_document || m_selectionType != QLatin1String("artmesh") || m_selectionName.isEmpty() ||
        IsLocked(m_selectionType, m_selectionName)) return;
    QSet<int> selectedRows;
    for (const QTableWidgetSelectionRange& range : m_vertexTable->selectedRanges())
        for (int row = range.topRow(); row <= range.bottomRow(); ++row) selectedRows.insert(row);
    if (selectedRows.size() < 2) return;
    const QVector<quint16> before = m_document->DrawableIndices(m_selectionName);
    int vertexCount = 0;
    for (const auto& drawable : m_document->Drawables())
        if (drawable.id == m_selectionName) { vertexCount = drawable.vertexCount; break; }
    QVector<quint16> after;
    if (!live2dstruct::CMeshTopology::MergeVertices(before, selectedRows, vertexCount, &after) || after == before) return;
    const live2dstruct::CModelEdit command{m_selectionName, QStringLiteral("artmesh"),
                                           live2dstruct::CModelEdit::MeshIndices,
                                           QVariant::fromValue(before), QVariant::fromValue(after), -1, false};
    if (!SubmitEdit(command)) return;
    emit DrawableIndicesChanged(m_selectionName, after);
    emit HistoryChanged(m_document->CanUndoEdit(), m_document->CanRedoEdit());
}

void CInspectorPanel::TransformSelectedVertices()
{
    if (!m_document || m_selectionType != QLatin1String("artmesh") || m_selectionName.isEmpty() ||
        IsLocked(m_selectionType, m_selectionName) || !m_vertexScale || !m_vertexRotation) return;
    const float scale = static_cast<float>(m_vertexScale->value());
    const float degrees = static_cast<float>(m_vertexRotation->value());
    if (!std::isfinite(scale) || !std::isfinite(degrees) || scale <= 0.0f ||
        (qFuzzyCompare(scale, 1.0f) && qFuzzyIsNull(degrees))) return;
    const int keyformIndex = m_activeKeyformIndex;
    const QVector<float> before = m_document->DrawableKeyformPositions(m_selectionName, keyformIndex);
    if (keyformIndex < 0 || before.isEmpty() || before.size() % 2 != 0) return;
    QSet<int> selectedRows;
    for (const QTableWidgetSelectionRange& range : m_vertexTable->selectedRanges())
        for (int row = range.topRow(); row <= range.bottomRow(); ++row) selectedRows.insert(row);
    if (selectedRows.isEmpty()) return;
    double centerX = 0.0;
    double centerY = 0.0;
    int count = 0;
    for (int row : selectedRows) {
        if (row < 0 || row * 2 + 1 >= before.size()) continue;
        centerX += before[row * 2];
        centerY += before[row * 2 + 1];
        ++count;
    }
    if (count == 0) return;
    centerX /= count;
    centerY /= count;
    const double radians = double(degrees) * 0.017453292519943295;
    const double cosine = std::cos(radians);
    const double sine = std::sin(radians);
    QVector<float> after = before;
    for (int row : selectedRows) {
        if (row < 0 || row * 2 + 1 >= after.size()) continue;
        const double x = double(before[row * 2]) - centerX;
        const double y = double(before[row * 2 + 1]) - centerY;
        const double changedX = centerX + (x * cosine - y * sine) * scale;
        const double changedY = centerY + (x * sine + y * cosine) * scale;
        if (!std::isfinite(changedX) || !std::isfinite(changedY) ||
            std::abs(changedX) > std::numeric_limits<float>::max() || std::abs(changedY) > std::numeric_limits<float>::max()) {
            RejectEdit(QStringLiteral("变换结果超出坐标范围"));
            return;
        }
        after[row * 2] = float(changedX);
        after[row * 2 + 1] = float(changedY);
    }
    if (after == before) return;
    const live2dstruct::CModelEdit command{m_selectionName, QStringLiteral("artmesh"),
                                           live2dstruct::CModelEdit::KeyformPositions,
                                           QVariant::fromValue(before), QVariant::fromValue(after), keyformIndex, false};
    if (!SubmitEdit(command)) return;
    RebuildKeyformTable();
    emit DrawableKeyformPositionsChanged(m_selectionName, keyformIndex, after);
    emit HistoryChanged(m_document->CanUndoEdit(), m_document->CanRedoEdit());
}

void CInspectorPanel::Undo()
{
    if (!m_document) return;
    live2dstruct::CModelEdit pending;
    if (!m_document->PeekUndoEdit(&pending) || pending.field == live2dstruct::CModelEdit::ParameterValue) return;
    live2dstruct::CModelEdit edit;
    if (!SubmitHistory(true, edit)) return;
    EmitEdit(edit, false);
    emit HistoryChanged(m_document->CanUndoEdit(), m_document->CanRedoEdit());
}

void CInspectorPanel::Redo()
{
    if (!m_document) return;
    live2dstruct::CModelEdit pending;
    if (!m_document->PeekRedoEdit(&pending) || pending.field == live2dstruct::CModelEdit::ParameterValue) return;
    live2dstruct::CModelEdit edit;
    if (!SubmitHistory(false, edit)) return;
    EmitEdit(edit, true);
    emit HistoryChanged(m_document->CanUndoEdit(), m_document->CanRedoEdit());
}

void CInspectorPanel::showModelObject(const QString& name, const QString& type, bool preserveKeyforms)
{
    if (!m_document || name.isEmpty()) { ClearSelection(); return; }
    const int selectedKeyform = preserveKeyforms ? m_activeKeyformIndex : 0;
    const int selectedDeformerKeyform = preserveKeyforms ? m_activeDeformerKeyform : 0;
    PrepareProjectionContext(type == QLatin1String("artmesh") ? selectedKeyform : selectedDeformerKeyform);
    auto& projection = *m_projection;
    const bool sameContext = projection.metadataValid && projection.metadata == projection.context;
    if (sameContext && projection.revision == m_document->Revision() && projection.locks == m_lockedKeys &&
        !projection.force && (!projection.pending || (projection.visibilityKnown && !projection.visible))) return;
    projection.pending = false;
    const auto revision = m_document->Revision();
    const auto context = projection.context;
    const QPointer<CInspectorPanel> owner(this);
    const auto rows = qScopeGuard([owner, revision, context] {
        if (!owner || !owner->m_document || owner->m_document != context.document ||
            owner->m_document->SourceDigest() != context.source || owner->m_document->SourcePath() != context.path ||
            owner->m_document->Revision() != revision || owner->m_selectionName != context.name ||
            owner->m_selectionType != context.type) return;
        owner->RefreshKeyformMatrix();
        if (!owner) return;
        updateObjectRows(owner, owner->GetSelectionType());
        auto& state = *owner->m_projection;
        state.metadata = state.context; state.metadataValid = true;
        state.revision = revision; state.locks = owner->m_lockedKeys;
    });
    const bool unlocked = !IsLocked(type, name);
    const bool deformerSelected = type == QLatin1String("warp") || type == QLatin1String("rotation");
    m_nameLabel->setText(deformerSelected ? m_document->DeformerDisplayName(name) : QString());
    m_renameDeformer->setEnabled(m_renameEnabled && deformerSelected && unlocked && !m_submitting);
    m_resizeWarpGrid->setEnabled(m_warpGridResizeEnabled && type == QLatin1String("warp") && !m_submitting);
    if (!sameContext) {
        m_activeKeyformIndex = -1;
        m_activeDeformerKeyform = -1;
        m_emptyLabel->setVisible(false);
        m_typeLabel->setText(type == QLatin1String("artmesh") ? QStringLiteral("ArtMesh")
                           : type == QLatin1String("part") ? QStringLiteral("Part")
                           : type == QLatin1String("warp") ? QStringLiteral("Warp Deformer")
                           : type == QLatin1String("rotation") ? QStringLiteral("Rotation Deformer")
                           : type);
        m_idLabel->setText(name);
        m_parentLabel->setText(QStringLiteral("—"));
        m_deformerLabel->setText(QStringLiteral("—"));
        m_visibilityLabel->setText(QStringLiteral("—"));
        m_geometryLabel->setText(QStringLiteral("—"));
        m_textureLabel->setText(QStringLiteral("—"));
        m_drawOrderLabel->setText(QStringLiteral("—"));
        m_blendLabel->setText(QStringLiteral("—"));
        m_maskLabel->setText(QStringLiteral("—"));
        m_keyformLabel->setText(QStringLiteral("—"));
        m_updatingDeformerParent = true;
        m_deformerParentCombo->clear();
        m_deformerParentCombo->setEnabled(false);
        m_updatingDeformerParent = false;
        ResetMaskControls();
        m_visibilityCheck->setEnabled(unlocked);
        m_opacitySpin->setEnabled(false);
        m_drawOrderSpin->setEnabled(false);
        m_updatingKeyform = true;
        m_keyformCombo->clear();
        m_keyformCombo->setEnabled(false);
        if (type != QLatin1String("artmesh")) ClearGeometryTable(m_vertexTable);
        m_vertexTable->setEnabled(false);
        if (type != QLatin1String("rotation")) ClearGeometryTable(m_rotationKeyformTable);
        setRowVisible(m_rotationKeyformTable, false);
        m_rotationKeyformTable->setEnabled(false);
        m_warpKeyformCombo->clear();
        m_warpKeyformCombo->setEnabled(false);
        if (type != QLatin1String("warp")) ClearGeometryTable(m_warpKeyformTable);
        setRowVisible(m_warpKeyformTable, false);
        m_warpKeyformTable->setEnabled(false);
        m_mirrorHorizontal->setEnabled(false);
        m_mirrorVertical->setEnabled(false);
        m_interpolatePrevious->setEnabled(false);
        m_updatingKeyform = false;
    }

    if (type == QLatin1String("artmesh")) {
        for (const auto& drawable : m_document->Drawables()) {
            if (drawable.id != name) continue;
            m_parentLabel->setText(indexValue(drawable.parentPartIndex));
            m_deformerLabel->setText(indexValue(drawable.parentDeformerIndex, QStringLiteral("无")));
            m_visibilityLabel->setText(QStringLiteral("%1 / %2").arg(drawable.visible ? QStringLiteral("是") : QStringLiteral("否"),
                                                                    drawable.enabled ? QStringLiteral("是") : QStringLiteral("否")));
            m_updatingVisibility = true;
            m_visibilityCheck->setChecked(drawable.visible);
            m_updatingVisibility = false;
            m_updatingVisibility = true;
            if (m_opacitySpin->value() != m_document->DrawableOpacity(drawable) || projection.force)
                m_opacitySpin->setValue(m_document->DrawableOpacity(drawable));
            m_opacitySpin->setEnabled(unlocked);
            if (m_drawOrderSpin->value() != qRound(m_document->DrawableDrawOrder(drawable)) || projection.force)
                m_drawOrderSpin->setValue(qRound(m_document->DrawableDrawOrder(drawable)));
            m_drawOrderSpin->setEnabled(unlocked);
            m_updatingVisibility = false;
            m_geometryLabel->setText(QStringLiteral("%1 顶点，%2 索引").arg(drawable.vertexCount).arg(drawable.indices.size()));
            live2dstruct::CModelDrawableMaterial material;
            QString materialError;
            const bool materialValid = m_document->CaptureDrawableMaterial(drawable.id, material, &materialError);
            m_textureLabel->setText(!materialValid ? materialError : material.textureIndex < 0
                ? QStringLiteral("无") : QString::number(material.textureIndex));
            const QString blend = drawable.blendMode == 1 ? QStringLiteral("ADD")
                               : drawable.blendMode == 2 ? QStringLiteral("MULTIPLY")
                                                          : QStringLiteral("NORMAL");
            m_drawOrderLabel->setText(QStringLiteral("%1（默认透明度 %2）")
                                      .arg(QString::number(drawable.drawOrder, 'f', 0))
                                      .arg(QString::number(drawable.opacity, 'f', 3)));
            m_blendLabel->setText(QStringLiteral("%1 / %2").arg(blend, drawable.culling ? QStringLiteral("启用") : QStringLiteral("关闭")));
            const auto masks = m_document->DrawableMaskIndices(name);
            m_maskLabel->setText(masks.isEmpty() ? QStringLiteral("无") : QString::number(masks.size()));
            m_keyformLabel->setText(QString::number(drawable.keyforms.size()));
            m_updatingKeyform = true;
            if (m_keyformCombo->count() != drawable.keyforms.size()) {
                m_keyformCombo->clear();
                for (int keyformIndex = 0; keyformIndex < drawable.keyforms.size(); ++keyformIndex)
                    m_keyformCombo->addItem(QStringLiteral("Keyform %1").arg(keyformIndex), keyformIndex);
            }
            m_keyformCombo->setEnabled(unlocked && !drawable.keyforms.isEmpty());
            m_mirrorHorizontal->setEnabled(unlocked && !drawable.keyforms.isEmpty());
            m_mirrorVertical->setEnabled(unlocked && !drawable.keyforms.isEmpty());
            m_interpolatePrevious->setEnabled(unlocked && drawable.keyforms.size() > 1);
            m_copyToNext->setEnabled(unlocked && drawable.keyforms.size() > 1);
            m_resetKeyform->setEnabled(unlocked && !drawable.keyforms.isEmpty());
            m_copyKeyform->setEnabled(unlocked && !drawable.keyforms.isEmpty());
            m_pasteKeyform->setEnabled(unlocked && !drawable.keyforms.isEmpty());
            m_optimizeTopology->setEnabled(unlocked && drawable.vertexCount >= 3 && !drawable.keyforms.isEmpty() &&
                                            !m_document->DrawableIndices(name).isEmpty());
            m_autoTriangulate->setEnabled(unlocked && drawable.vertexCount >= 3 && !drawable.keyforms.isEmpty());
            m_copyTopology->setEnabled(unlocked && !drawable.indices.isEmpty());
            m_pasteTopology->setEnabled(unlocked && drawable.vertexCount > 0);
            m_editMasks->setEnabled(unlocked);
            m_updatingVisibility = true;
            m_maskInvertedCheck->setChecked(m_document->DrawableMaskInverted(name));
            m_maskInvertedCheck->setEnabled(unlocked && !masks.isEmpty());
            m_updatingVisibility = false;
            if (!drawable.keyforms.isEmpty())
                m_keyformCombo->setCurrentIndex(selectedKeyform >= 0 && selectedKeyform < drawable.keyforms.size()
                                                   ? selectedKeyform : 0);
            m_activeKeyformIndex = m_keyformCombo->currentIndex();
            m_updatingKeyform = false;
            RebuildKeyformTable();
            if (m_keyformCombo->currentIndex() >= 0)
                emit DrawableKeyformActivated(name, m_keyformCombo->currentIndex());
            return;
        }
    } else if (type == QLatin1String("part")) {
        for (const auto& part : m_document->Parts()) {
            if (part.id != name) continue;
            m_parentLabel->setText(indexValue(part.parentIndex));
            m_visibilityLabel->setText(QStringLiteral("%1 / %2").arg(part.visible ? QStringLiteral("是") : QStringLiteral("否"),
                                                                    part.enabled ? QStringLiteral("是") : QStringLiteral("否")));
            m_updatingVisibility = true;
            m_visibilityCheck->setChecked(part.visible);
            m_updatingVisibility = false;
            return;
        }
    } else {
        for (const auto& deformer : m_document->Deformers()) {
            if (deformer.id != name) continue;
            m_parentLabel->setText(indexValue(deformer.parentPartIndex));
            m_deformerLabel->setText(indexValue(m_document->DeformerParentIndex(name), QStringLiteral("无")));
            m_updatingDeformerParent = true;
            m_deformerParentCombo->clear();
            m_deformerParentCombo->addItem(QStringLiteral("无（根级）"), -1);
            for (int parentIndex = 0; parentIndex < m_document->Deformers().size(); ++parentIndex) {
                const auto& candidate = m_document->Deformers()[parentIndex];
                if (candidate.id == name || (candidate.parentPartIndex != deformer.parentPartIndex &&
                    parentIndex != deformer.parentDeformerIndex &&
                    parentIndex != m_document->SourceDeformerParentIndex(deformer.id))) continue;
                const auto displayName = m_document->DeformerDisplayName(candidate.id);
                m_deformerParentCombo->addItem(displayName == candidate.id ? candidate.id :
                    displayName + QStringLiteral(" · ") + candidate.id, parentIndex);
            }
            const int parentIndex = m_document->DeformerParentIndex(name);
            for (int comboIndex = 0; comboIndex < m_deformerParentCombo->count(); ++comboIndex) {
                if (m_deformerParentCombo->itemData(comboIndex).toInt() == parentIndex) {
                    m_deformerParentCombo->setCurrentIndex(comboIndex);
                    break;
                }
            }
            m_deformerParentCombo->setEnabled(unlocked);
            m_updatingDeformerParent = false;
            m_visibilityLabel->setText(QStringLiteral("%1 / %2").arg(deformer.visible ? QStringLiteral("是") : QStringLiteral("否"),
                                                                    deformer.enabled ? QStringLiteral("是") : QStringLiteral("否")));
            m_updatingVisibility = true;
            m_visibilityCheck->setChecked(deformer.visible);
            m_updatingVisibility = false;
            if (deformer.type == live2dstruct::CModelDeformer::Warp) {
                m_geometryLabel->setText(QStringLiteral("Warp %1 × %2").arg(deformer.columns).arg(deformer.rows));
                const auto keyforms = m_document->DeformerWarpKeyforms(name);
                m_updatingWarpKeyform = true;
                if (m_warpKeyformCombo->count() != keyforms.size()) {
                    m_warpKeyformCombo->clear();
                    for (int keyformIndex = 0; keyformIndex < keyforms.size(); ++keyformIndex)
                        m_warpKeyformCombo->addItem(QStringLiteral("Keyform %1").arg(keyformIndex), keyformIndex);
                }
                m_warpKeyformCombo->setEnabled(unlocked && !keyforms.isEmpty());
                setRowVisible(m_warpKeyformTable, !keyforms.isEmpty());
                m_warpKeyformTable->setEnabled(unlocked && !keyforms.isEmpty());
                if (!keyforms.isEmpty()) {
                    m_activeDeformerKeyform = selectedDeformerKeyform >= 0 && selectedDeformerKeyform < keyforms.size()
                        ? selectedDeformerKeyform : 0;
                    m_warpKeyformCombo->setCurrentIndex(m_activeDeformerKeyform);
                }
                m_updatingWarpKeyform = false;
                RebuildWarpKeyformTable();
            } else {
                m_geometryLabel->setText(QStringLiteral("Rotation Deformer"));
                setRowVisible(m_rotationKeyformTable, true);
                m_rotationKeyformTable->setEnabled(unlocked);
                m_activeDeformerKeyform = selectedDeformerKeyform >= 0 && selectedDeformerKeyform < deformer.rotationKeyforms.size()
                    ? selectedDeformerKeyform : 0;
                RebuildRotationKeyformTable();
            }
            return;
        }
    }
    ClearSelection();
}

void CInspectorPanel::SetRenameDeformerEnabled(bool enabled)
{
    m_renameEnabled = enabled;
    m_renameDeformer->setEnabled(enabled && !m_submitting && m_selectionNames.size() == 1 &&
        (m_selectionType == QLatin1String("warp") || m_selectionType == QLatin1String("rotation")) &&
        !IsLocked(m_selectionType, m_selectionName));
}

void CInspectorPanel::SetWarpGridResizeEnabled(bool enabled)
{
    m_warpGridResizeEnabled = enabled;
    m_resizeWarpGrid->setEnabled(enabled && !m_submitting && m_selectionNames.size() == 1 && m_selectionType == QLatin1String("warp"));
}
