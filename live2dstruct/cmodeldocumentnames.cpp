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
}

QString CModelDocument::DeformerDisplayName(const QString& id) const
{
    return findDeformer(m_deformers, id) ? m_deformerDisplayNames.value(id, id) : QString();
}

bool CModelDocument::SetDeformerDisplayNames(const CDeformerDisplayNames::State& names, QString* error) try
{
    if (error) error->clear();
    if (!m_loaded) return fail(error, QStringLiteral("尚未载入模型"));
    QHash<QString, int> directory;
    for (const auto& item : m_deformers) ++directory[item.id];
    for (auto item = names.cbegin(); item != names.cend(); ++item)
        if (directory.value(item.key()) != 1) return fail(error, QStringLiteral("变形器不存在或 ID 重复：%1").arg(item.key()));
    CDeformerDisplayNames::State normalized;
    if (!CDeformerDisplayNames::Normalize(names, normalized, error)) return false;
    if (normalized == m_deformerDisplayNames) return true;
    m_deformerDisplayNames.swap(normalized);
    ++m_revision;
    return true;
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
    auto names = m_deformerDisplayNames;
    if (after == edit.name) names.remove(edit.name);
    else names.insert(edit.name, after);
    return SetDeformerDisplayNames(names);
}
}
