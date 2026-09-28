#include "cmodeldocument.h"
#include <exception>

namespace live2dstruct {
namespace {
bool fail(QString* error, const QString& message) { if (error) *error = message; return false; }
const CModelDeformer* findDeformer(const QVector<CModelDeformer>& deformers, const QString& id)
{
    const CModelDeformer* result = nullptr;
    for (const auto& item : deformers) if (item.id == id) {
        if (result) return nullptr;
        result = &item;
    }
    return result;
}
QString owned(const QString& text) { return QString(text.constData(), text.size()); }
QString objectType(const CModelDeformer& object)
{
    return object.type == CModelDeformer::Warp ? QStringLiteral("warp") : QStringLiteral("rotation");
}
template <class Objects>
int uniqueIndex(const Objects& objects, const QString& id)
{
    int result = -1;
    for (int index = 0; index < objects.size(); ++index) if (objects[index].id == id) {
        if (result >= 0) return -1;
        result = index;
    }
    return result;
}
}

QString CModelDocument::DisplayName(const QString& type, const QString& id) const
{
    const auto* deformer = findDeformer(m_deformers, id);
    const bool exists = type == QLatin1String("part") ? uniqueIndex(m_parts, id) >= 0 :
        type == QLatin1String("artmesh") ? uniqueIndex(m_drawables, id) >= 0 :
        deformer && objectType(*deformer) == type;
    return exists ? CModelDisplayNames::EffectiveName(m_displayNames, type, id) : QString();
}

bool CModelDocument::SetDisplayNames(const CModelDisplayNames::State& names, QString* error) try
{
    if (error) error->clear();
    if (!m_loaded) return fail(error, QStringLiteral("尚未载入模型"));
    if (!CModelDisplayNames::Validate(names, error)) return false;
    const auto validate = [&](const auto& objects, const auto& entries) {
        QHash<QString, int> directory;
        for (const auto& object : objects) ++directory[object.id];
        for (auto entry = entries.cbegin(); entry != entries.cend(); ++entry)
            if (directory.value(entry.key()) != 1)
                return fail(error, QStringLiteral("名称所指对象不存在或 ID 重复：%1").arg(entry.key()));
        return true;
    };
    if (!validate(m_parts, names.parts) || !validate(m_drawables, names.artMeshes) || !validate(m_deformers, names.deformers)) return false;
    CModelDisplayNames::State normalized;
    if (!CModelDisplayNames::Normalize(names, normalized, error)) return false;
    if (normalized == m_displayNames) return true;
    m_displayNames = std::move(normalized);
    ++m_revision;
    return true;
}
catch (...) { return fail(error, QStringLiteral("无法设置对象名称，原状态保持不变")); }

bool CModelDocument::CanEditDisplayName(const QString& type, const QString& id, QString* error) const
{
    if (error) error->clear();
    if (!m_loaded) return fail(error, QStringLiteral("尚未载入模型"));
    if (type == QLatin1String("warp") || type == QLatin1String("rotation")) {
        const auto* object = findDeformer(m_deformers, id);
        return object && objectType(*object) == type ? CanEditDeformerDisplayName(id, error) :
            fail(error, QStringLiteral("对象类型与变形器不匹配"));
    }
    int partIndex = -1;
    if (type == QLatin1String("part")) {
        partIndex = uniqueIndex(m_parts, id);
        if (partIndex < 0) return fail(error, QStringLiteral("部件不存在或 ID 重复"));
    } else if (type == QLatin1String("artmesh")) {
        const int index = uniqueIndex(m_drawables, id);
        if (index < 0) return fail(error, QStringLiteral("网格不存在或 ID 重复"));
        if (IsLocked(type, id)) return fail(error, QStringLiteral("网格已锁定"));
        const auto& mesh = m_drawables[index];
        partIndex = mesh.parentPartIndex;
        const int parent = mesh.parentDeformerIndex;
        if (parent != -1 && (parent < 0 || parent >= m_deformers.size() ||
            !CanEditDeformerDisplayName(m_deformers[parent].id, error)))
            return fail(error, error && !error->isEmpty() ? *error : QStringLiteral("网格变形器父级无效"));
    } else return fail(error, QStringLiteral("不支持这个对象类型的名称编辑"));
    QSet<int> visited;
    while (partIndex != -1) {
        if (partIndex < 0 || partIndex >= m_parts.size() || visited.contains(partIndex))
            return fail(error, QStringLiteral("对象所属部件层级无效"));
        visited.insert(partIndex);
        const auto& part = m_parts[partIndex];
        if (uniqueIndex(m_parts, part.id) != partIndex) return fail(error, QStringLiteral("对象所属部件 ID 重复"));
        if (IsLocked(QStringLiteral("part"), part.id)) return fail(error, QStringLiteral("对象或所属部件已锁定"));
        partIndex = part.parentIndex;
    }
    return true;
}

bool CModelDocument::MakeDisplayNamesEdit(const CModelDisplayNames::State& names, CModelEdit& edit, QString* error) const try
{
    CModelDocument candidate = *this;
    if (!candidate.SetDisplayNames(names, error)) return false;
    const auto changed = [&](const auto& before, const auto& after, const QString& type) {
        QSet<QString> ids;
        for (auto item = before.cbegin(); item != before.cend(); ++item) ids.insert(item.key());
        for (auto item = after.cbegin(); item != after.cend(); ++item) ids.insert(item.key());
        for (const auto& id : ids) {
            if (before.value(id, id) == after.value(id, id)) continue;
            const auto* deformer = type.isEmpty() ? findDeformer(m_deformers, id) : nullptr;
            if (!CanEditDisplayName(type.isEmpty() && deformer ? objectType(*deformer) : type, id, error)) return false;
        }
        return true;
    };
    if (!changed(m_displayNames.parts, candidate.m_displayNames.parts, QStringLiteral("part")) ||
        !changed(m_displayNames.artMeshes, candidate.m_displayNames.artMeshes, QStringLiteral("artmesh")) ||
        !changed(m_displayNames.deformers, candidate.m_displayNames.deformers, {})) return false;
    CModelDisplayNames::State before;
    if (!CModelDisplayNames::Normalize(m_displayNames, before, error)) return false;
    edit = {QStringLiteral("objects"), QStringLiteral("names"), CModelEdit::DisplayNames,
        QVariant::fromValue(before), QVariant::fromValue(candidate.m_displayNames), -1, false};
    return true;
}
catch (...) { return fail(error, QStringLiteral("无法准备对象名称修改")); }

bool CModelDocument::MakeDisplayNameEdit(const QString& type, const QString& id, const QString& name,
                                        CModelEdit& edit, QString* error) const try
{
    if (!CanEditDisplayName(type, id, error) || !CModelDisplayNames::ValidateName(name, error)) return false;
    auto names = m_displayNames;
    auto& entries = type == QLatin1String("part") ? names.parts : type == QLatin1String("artmesh") ? names.artMeshes : names.deformers;
    entries.insert(id, name);
    return MakeDisplayNamesEdit(names, edit, error);
}
catch (...) { return fail(error, QStringLiteral("无法准备对象名称修改")); }

bool CModelDocument::ApplyDisplayNamesEdit(const CModelEdit& edit, bool forward)
{
    if (edit.name != QLatin1String("objects") || edit.type != QLatin1String("names") || edit.index != -1 || edit.coalesce ||
        edit.before.metaType() != QMetaType::fromType<CModelDisplayNames::State>() ||
        edit.after.metaType() != QMetaType::fromType<CModelDisplayNames::State>()) return false;
    const auto before = (forward ? edit.before : edit.after).value<CModelDisplayNames::State>();
    const auto after = (forward ? edit.after : edit.before).value<CModelDisplayNames::State>();
    return before == m_displayNames && SetDisplayNames(after);
}

QString CModelDocument::DeformerDisplayName(const QString& id) const
{
    return findDeformer(m_deformers, id) ? m_displayNames.deformers.value(id, id) : QString();
}

bool CModelDocument::SetDeformerDisplayNames(const CDeformerDisplayNames::State& names, QString* error) try
{
    auto combined = m_displayNames;
    combined.deformers = names;
    return SetDisplayNames(combined, error);
}
catch (const std::exception&) { return fail(error, QStringLiteral("无法设置变形器名称，原状态保持不变")); }

bool CModelDocument::CanEditDeformerDisplayName(const QString& id, QString* error) const
{
    if (error) error->clear();
    const auto* object = findDeformer(m_deformers, id);
    if (!m_loaded || !object) return fail(error, QStringLiteral("变形器不存在或 ID 重复：%1").arg(id));
    QSet<int> checkedParts;
    const auto checkParts = [&](int index) {
        QSet<int> visited;
        while (index != -1) {
            if (index < 0 || index >= m_parts.size() || visited.contains(index))
                return fail(error, QStringLiteral("变形器所属部件层级无效"));
            if (checkedParts.contains(index)) return true;
            visited.insert(index);
            const auto& part = m_parts[index];
            if (IsLocked(QStringLiteral("part"), part.id)) return fail(error, QStringLiteral("变形器所属部件已锁定"));
            index = part.parentIndex;
        }
        checkedParts.unite(visited);
        return true;
    };
    QHash<QString, int> ids;
    for (const auto& item : m_deformers) ++ids[item.id];
    QSet<int> visited;
    int index = int(object - m_deformers.constData());
    while (index != -1) {
        if (index < 0 || index >= m_deformers.size() || visited.contains(index))
            return fail(error, QStringLiteral("变形器父级层级无效"));
        visited.insert(index);
        const auto& node = m_deformers[index];
        if (ids.value(node.id) != 1 || (node.type != CModelDeformer::Warp && node.type != CModelDeformer::Rotation))
            return fail(error, QStringLiteral("变形器父级身份无效"));
        if (IsLocked(objectType(node), node.id)) return fail(error, QStringLiteral("变形器或其父级已锁定"));
        if (!checkParts(node.parentPartIndex)) return false;
        index = DeformerParentIndex(node.id);
    }
    return true;
}

bool CModelDocument::MakeDeformerDisplayNameEdit(const QString& id, const QString& name,
                                               CModelEdit& edit, QString* error) const try
{
    if (error) error->clear();
    const auto* object = findDeformer(m_deformers, id);
    if (!CanEditDeformerDisplayName(id, error)) return false;
    if (!CDeformerDisplayNames::ValidateName(name, error)) return false;
    CModelEdit command{owned(id), objectType(*object), CModelEdit::DeformerDisplayName,
                       owned(DeformerDisplayName(id)), owned(name), -1, false};
    edit = std::move(command);
    return true;
}
catch (const std::exception&) { return fail(error, QStringLiteral("无法准备变形器名称修改")); }

bool CModelDocument::ApplyDeformerDisplayNameEdit(const CModelEdit& edit, bool forward)
{
    const auto* object = findDeformer(m_deformers, edit.name);
    if (!object || edit.type != objectType(*object) || edit.index != -1 || edit.coalesce ||
        edit.before.metaType() != QMetaType::fromType<QString>() || edit.after.metaType() != QMetaType::fromType<QString>()) return false;
    const auto before = (forward ? edit.before : edit.after).toString();
    const auto after = (forward ? edit.after : edit.before).toString();
    if (before != DeformerDisplayName(edit.name) ||
        (before != edit.name && !CDeformerDisplayNames::ValidateName(before)) ||
        (after != edit.name && !CDeformerDisplayNames::ValidateName(after))) return false;
    auto names = m_displayNames.deformers;
    if (after == edit.name) names.remove(edit.name);
    else names.insert(edit.name, after);
    return SetDeformerDisplayNames(names);
}
}
