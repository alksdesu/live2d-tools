#include "cmodelhierarchymodel.h"
#include "ui/para/ccidjson.h"
#include "live2dstruct/csectiontable.h"
#include "live2dstruct/cparentgraph.h"
#include "live2dstruct/cparameterselectionquery.h"
#include <QApplication>
#include <QFont>
#include <QIcon>
#include <QHash>
#include <QSet>
#include <QStyle>
#include <array>
#include <exception>
#include <numeric>

using namespace live2dstruct;

struct CModelHierarchyModel::Data
{
    struct Object {
        QString id, type, label, key;
        bool visible = true, locked = false;
        int binding = -1;
    };
    struct Node { int parent = -1, row = 0, begin = 0, count = 0; };
    struct Tree { QVector<Node> nodes; QVector<int> children, roots; };
    QVector<Object> objects;
    QHash<QString, int> byKey;
    std::array<Tree, 2> trees;
    CParentGraph deformers;
    CParameterSelectionQuery parameterQuery;
    std::array<QIcon, 4> icons;
    int parts = 0, deformerCount = 0;
};

namespace {
bool fail(QString* error, const QString& message)
{
    if (error) *error = message;
    return false;
}

bool validIndex(int index, qsizetype size) { return index >= -1 && index < size; }

int treeNumber(CModelHierarchyModel::Projection value)
{
    return value == CModelHierarchyModel::Projection::Parts ? 0 : 1;
}

QVector<int> subtree(const CModelHierarchyModel::Data::Tree& tree, int root)
{
    QVector<int> result;
    if (root < 0 || root >= tree.nodes.size()) return result;
    QVector<int> pending{root};
    while (!pending.isEmpty()) {
        const int current = pending.takeLast();
        result.append(current);
        const auto& node = tree.nodes[current];
        for (int child = node.count - 1; child >= 0; --child)
            pending.append(tree.children[node.begin + child]);
    }
    return result;
}

void buildProjection(CModelHierarchyModel::Data::Tree& tree, const QVector<int>& parents)
{
    tree.nodes.resize(parents.size());
    for (int object = 0; object < parents.size(); ++object) {
        tree.nodes[object].parent = parents[object];
        if (parents[object] >= 0) ++tree.nodes[parents[object]].count;
        else {
            tree.nodes[object].row = tree.roots.size();
            tree.roots.append(object);
        }
    }
    int count = 0;
    for (auto& node : tree.nodes) { node.begin = count; count += node.count; }
    tree.children.resize(count);
    QVector<int> written(parents.size(), 0);
    for (int object = 0; object < parents.size(); ++object) {
        const int parent = parents[object];
        if (parent < 0) continue;
        const int row = written[parent]++;
        tree.nodes[object].row = row;
        tree.children[tree.nodes[parent].begin + row] = object;
    }
}
}

CModelHierarchyModel::CModelHierarchyModel(Projection projection, QObject* parent)
    : QAbstractItemModel(parent), m_projection(projection) {}

bool CModelHierarchyModel::Prepare(const CSectionTable& section, CCidJson* cid,
                                    Snapshot& result, QString* error)
{
    try {
        auto candidate = std::make_shared<Data>();
        auto& data = *candidate;
        data.parts = section.parts.size();
        data.deformerCount = section.deformers.size();
        QVector<int> partParents, deformerParents;
        for (const auto* part : section.parts) {
            if (!part) return fail(error, QStringLiteral("部件数据无效"));
            partParents.append(part->parent_part_indices);
        }
        for (const auto* deformer : section.deformers) {
            if (!deformer || !validIndex(deformer->parent_part_indices, section.parts.size()))
                return fail(error, QStringLiteral("变形器所属部件索引无效"));
            deformerParents.append(deformer->parent_deformer_indices);
        }
        CParentGraph parts(partParents);
        if (!parts.IsValid()) return fail(error, QStringLiteral("部件父级索引无效或包含循环"));
        if (!data.deformers.Analyze(deformerParents))
            return fail(error, QStringLiteral("变形器父级索引无效或包含循环"));

        const auto add = [&](const QString& id, const QString& type, QString label, bool visible, int binding) {
            const QString key = type + QLatin1Char('|') + id;
            if (id.isEmpty() || data.byKey.contains(key)) return false;
            if (label.isEmpty()) label = id;
            data.byKey.insert(key, data.objects.size());
            data.objects.append({id, type, label, key, visible, false, binding});
            return true;
        };
        QVector<int> partProjection, deformerProjection;
        for (const auto* part : section.parts) {
            if (!add(part->ids, QStringLiteral("part"), cid ? cid->getPartName(part->ids) : QString(),
                     part->is_visible, -1)) return fail(error, QStringLiteral("部件 ID 为空或重复"));
            partProjection.append(part->parent_part_indices);
            deformerProjection.append(part->parent_part_indices);
        }
        for (const auto* deformer : section.deformers) {
            if (!validIndex(deformer->keyform_binding_sources_indices, section.keyform_bindings.size()))
                return fail(error, QStringLiteral("变形器关键形绑定引用无效"));
            const QString type = deformer->types == 0 ? QStringLiteral("warp") : QStringLiteral("rotation");
            if (!add(deformer->ids, type, cid ? cid->getDeformerName(deformer->ids) : QString(),
                     deformer->is_visible, deformer->keyform_binding_sources_indices))
                return fail(error, QStringLiteral("变形器 ID 为空或重复"));
            partProjection.append(deformer->parent_deformer_indices >= 0
                ? data.parts + deformer->parent_deformer_indices : deformer->parent_part_indices);
            deformerProjection.append(deformer->parent_part_indices);
        }
        for (const auto* mesh : section.art_meshes) {
            if (!mesh || !validIndex(mesh->parent_part_indices, section.parts.size()) ||
                !validIndex(mesh->parent_deformer_indices, section.deformers.size()))
                return fail(error, QStringLiteral("网格所属部件或父变形器索引无效"));
            if (!validIndex(mesh->keyform_binding_sources_indices, section.keyform_bindings.size()))
                return fail(error, QStringLiteral("网格关键形绑定引用无效"));
            if (!add(mesh->ids, QStringLiteral("artmesh"), {}, mesh->is_visible, mesh->keyform_binding_sources_indices))
                return fail(error, QStringLiteral("网格 ID 为空或重复"));
            partProjection.append(mesh->parent_deformer_indices >= 0
                ? data.parts + mesh->parent_deformer_indices : mesh->parent_part_indices);
        }

        if (!data.parameterQuery.Build(section, error)) return false;
        buildProjection(data.trees[0], partProjection);
        buildProjection(data.trees[1], deformerProjection);
        if (qApp) {
            auto* style = qApp->style();
            data.icons = {style->standardIcon(QStyle::SP_DirIcon), style->standardIcon(QStyle::SP_ArrowRight),
                          style->standardIcon(QStyle::SP_BrowserReload), style->standardIcon(QStyle::SP_FileIcon)};
        }
        result.swap(candidate);
        if (error) error->clear();
        return true;
    } catch (const std::exception&) {
        return fail(error, QStringLiteral("模型结构树分配失败"));
    }
}

void CModelHierarchyModel::SetSnapshot(const Snapshot& snapshot)
{
    beginResetModel();
    m_data = snapshot;
    endResetModel();
}

int CModelHierarchyModel::ObjectIndex(const QModelIndex& index) const
{
    if (!m_data || !index.isValid() || index.model() != this || index.column() != 0) return -1;
    const auto number = index.internalId();
    return number < quintptr(m_data->trees[treeNumber(m_projection)].nodes.size()) ? int(number) : -1;
}

QModelIndex CModelHierarchyModel::ObjectIndex(int object) const
{
    if (!m_data) return {};
    const auto& nodes = m_data->trees[treeNumber(m_projection)].nodes;
    return object >= 0 && object < nodes.size() ? createIndex(nodes[object].row, 0, quintptr(object)) : QModelIndex();
}

QModelIndex CModelHierarchyModel::index(int row, int column, const QModelIndex& parent) const
{
    if (!m_data || row < 0 || column != 0) return {};
    const auto& tree = m_data->trees[treeNumber(m_projection)];
    if (!parent.isValid()) return row < tree.roots.size() ? ObjectIndex(tree.roots[row]) : QModelIndex();
    const int object = ObjectIndex(parent);
    if (object < 0 || row >= tree.nodes[object].count) return {};
    return ObjectIndex(tree.children[tree.nodes[object].begin + row]);
}

QModelIndex CModelHierarchyModel::parent(const QModelIndex& child) const
{
    const int object = ObjectIndex(child);
    return object < 0 ? QModelIndex() : ObjectIndex(m_data->trees[treeNumber(m_projection)].nodes[object].parent);
}

int CModelHierarchyModel::rowCount(const QModelIndex& parent) const
{
    if (!m_data) return 0;
    const auto& tree = m_data->trees[treeNumber(m_projection)];
    if (!parent.isValid()) return tree.roots.size();
    const int object = ObjectIndex(parent);
    return object < 0 ? 0 : tree.nodes[object].count;
}

int CModelHierarchyModel::columnCount(const QModelIndex&) const { return 1; }

QVariant CModelHierarchyModel::data(const QModelIndex& index, int role) const
{
    const int object = ObjectIndex(index);
    if (object < 0) return {};
    const auto& value = m_data->objects[object];
    switch (role) {
    case Qt::DisplayRole: return value.label;
    case Qt::UserRole: return value.id;
    case Qt::UserRole + 1: return value.type;
    case Qt::UserRole + 2: return value.locked;
    case Qt::CheckStateRole: return value.visible ? Qt::Checked : Qt::Unchecked;
    case Qt::ToolTipRole: return value.locked ? QStringLiteral("已锁定 · ") + value.id : value.id;
    case Qt::FontRole: { QFont font; font.setStrikeOut(value.locked); return font; }
    case Qt::DecorationRole:
        return QVariant::fromValue(m_data->icons[value.type == QLatin1String("part") ? 0 : value.type == QLatin1String("warp") ? 1
                              : value.type == QLatin1String("rotation") ? 2 : 3]);
    default: return {};
    }
}

Qt::ItemFlags CModelHierarchyModel::flags(const QModelIndex& index) const
{
    const int object = ObjectIndex(index);
    if (object < 0) return Qt::NoItemFlags;
    Qt::ItemFlags result = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
    if (!m_data->objects[object].locked) result |= Qt::ItemIsUserCheckable;
    if (!rowCount(index)) result |= Qt::ItemNeverHasChildren;
    return result;
}

bool CModelHierarchyModel::setData(const QModelIndex& index, const QVariant& value, int role)
{
    if (role != Qt::CheckStateRole || !(flags(index) & Qt::ItemIsUserCheckable)) return false;
    const int state = value.toInt();
    if (state != Qt::Checked && state != Qt::Unchecked) return false;
    emit CheckRequested(index, state == Qt::Checked);
    return true;
}

QModelIndex CModelHierarchyModel::IndexForObject(const QString& id, const QString& type) const
{
    return IndexForKey(type + QLatin1Char('|') + id);
}

QModelIndex CModelHierarchyModel::IndexForKey(const QString& key) const
{
    return m_data ? ObjectIndex(m_data->byKey.value(key, -1)) : QModelIndex();
}

QModelIndexList CModelHierarchyModel::Indices() const
{
    QModelIndexList result;
    if (!m_data) return result;
    const auto& tree = m_data->trees[treeNumber(m_projection)];
    for (int root : tree.roots)
        for (int object : subtree(tree, root)) result.append(ObjectIndex(object));
    return result;
}

QString CModelHierarchyModel::Key(const QModelIndex& index)
{
    return index.isValid() ? index.data(Qt::UserRole + 1).toString() + QLatin1Char('|') + index.data(Qt::UserRole).toString() : QString();
}

QStringList CModelHierarchyModel::SubtreeKeys(const QModelIndex& root) const
{
    QStringList result;
    if (!m_data) return result;
    for (int object : subtree(m_data->trees[treeNumber(m_projection)], ObjectIndex(root))) result.append(m_data->objects[object].key);
    return result;
}

QStringList CModelHierarchyModel::MeshesForSelection(const QModelIndex& root) const
{
    QStringList result;
    if (!m_data) return result;
    for (int object : subtree(m_data->trees[0], ObjectIndex(root)))
        if (m_data->objects[object].type == QLatin1String("artmesh")) result.append(m_data->objects[object].id);
    return result;
}

CModelHierarchyModel::ParameterInfo CModelHierarchyModel::ParametersForSelection(const QModelIndex& root) const
{
    ParameterInfo result;
    if (!m_data) return result;
    const int object = ObjectIndex(root);
    if (object < m_data->parts || object >= m_data->parts + m_data->deformerCount) return result;
    CParameterSelectionQuery::Result related;
    const auto& selected = m_data->objects[object];
    if (!m_data->parameterQuery.Query({{selected.type, selected.id}}, related)) return result;
    const QSet<QString> ordinary(related.ordinary.cbegin(), related.ordinary.cend());
    for (const auto& parameter : m_data->parameterQuery.Parameters()) if (ordinary.contains(parameter.id)) {
        result.ids.append(parameter.id); result.minimums.append(parameter.minimum);
        result.maximums.append(parameter.maximum); result.defaults.append(parameter.defaultValue);
    }
    return result;
}

QMap<QString, bool> CModelHierarchyModel::VisibilityState() const
{
    QMap<QString, bool> result;
    if (m_data) for (const auto& object : m_data->objects) result.insert(object.key, object.visible);
    return result;
}

QMap<QString, bool> CModelHierarchyModel::LockedState() const
{
    QMap<QString, bool> result;
    if (m_data) for (const auto& object : m_data->objects) if (object.locked) result.insert(object.key, true);
    return result;
}

QMap<QString, bool> CModelHierarchyModel::SoloVisibility(const QStringList& selectedKeys) const
{
    QMap<QString, bool> result;
    if (!m_data) return result;
    const auto& tree = m_data->trees[0];
    QVector<quint8> selected(m_data->objects.size(), 0), visible(m_data->objects.size(), 0);
    for (const auto& key : selectedKeys) {
        const int object = m_data->byKey.value(key, -1);
        if (object >= 0) selected[object] = 1;
    }
    QVector<int> order;
    for (int root : tree.roots) order.append(subtree(tree, root));
    for (int object : order) {
        const int parent = tree.nodes[object].parent;
        visible[object] = selected[object] || (parent >= 0 && visible[parent]);
    }
    for (auto object = order.crbegin(); object != order.crend(); ++object) {
        const int parent = tree.nodes[*object].parent;
        if (visible[*object] && parent >= 0) visible[parent] = 1;
    }
    for (int object = 0; object < m_data->objects.size(); ++object) {
        const auto& value = m_data->objects[object];
        result.insert(value.key, value.locked ? value.visible : bool(visible[object]));
    }
    return result;
}

QMap<QString, bool> CModelHierarchyModel::UpdateVisibility(const QMap<QString, bool>& values)
{
    QMap<QString, bool> changed;
    if (!m_data) return changed;
    for (auto value = values.cbegin(); value != values.cend(); ++value) {
        const int object = m_data->byKey.value(value.key(), -1);
        if (object < 0 || m_data->objects[object].visible == value.value()) continue;
        m_data->objects[object].visible = value.value();
        changed.insert(value.key(), value.value());
    }
    return changed;
}

QStringList CModelHierarchyModel::UpdateLocked(const QMap<QString, bool>& values)
{
    QStringList changed;
    if (!m_data) return changed;
    for (auto& object : m_data->objects) {
        const bool locked = values.value(object.key, false);
        if (object.locked == locked) continue;
        object.locked = locked;
        changed.append(object.key);
    }
    return changed;
}

void CModelHierarchyModel::NotifyObjects(const QStringList& keys)
{
    for (const auto& key : keys) {
        const auto index = IndexForKey(key);
        if (index.isValid()) emit dataChanged(index, index,
            {Qt::CheckStateRole, Qt::UserRole + 2, Qt::FontRole, Qt::ToolTipRole});
    }
}
