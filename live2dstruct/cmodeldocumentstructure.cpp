#include "cmodeldocument.h"
#include "cmodelsession.h"
#include "ckeyformbindinggrid.h"
#include "cparentgraph.h"
#include "cmocpreviewbuilder.h"
#include "csectiontable.h"
#include "csectionvalidation.h"
#include <QCryptographicHash>
#include <QByteArrayView>
#include <QDataStream>
#include <QIODevice>
#include <algorithm>
#include <cmath>
#include <exception>

namespace live2dstruct {
namespace {
using Builder = CMocParameterStructureBuilder;

bool fail(QString* error, const QString& message)
{
    if (error) *error = message;
    return false;
}

template<class T> QMap<QString, T> ordered(const QHash<QString, T>& values)
{
    QMap<QString, T> result;
    for (auto it = values.cbegin(); it != values.cend(); ++it) result.insert(it.key(), it.value());
    return result;
}

template<class T> bool read(const QVariantMap& state, const QString& key, T& value)
{
    const auto found = state.constFind(key);
    if (found == state.cend() || found->metaType() != QMetaType::fromType<T>()) return false;
    value = found->value<T>();
    return true;
}

template<class T> void captureEntries(QVariantMap& state, const QString& name, const QHash<QString, T>& values,
                                     const QSet<QString>& ids)
{
    QVariantMap entries;
    for (const auto& id : ids) {
        const auto found = values.constFind(id);
        QVariantMap entry{{QStringLiteral("present"), found != values.cend()}};
        if (found != values.cend()) entry.insert(QStringLiteral("value"), QVariant::fromValue(found.value()));
        entries.insert(id, entry);
    }
    state.insert(name, entries);
}

template<class T> bool restoreEntries(const QVariantMap& state, const QString& name, QHash<QString, T>& values)
{
    if (!state.contains(name)) return true;
    if (state.value(name).metaType() != QMetaType::fromType<QVariantMap>()) return false;
    const auto entries = state.value(name).toMap();
    auto restored = values;
    for (auto it = entries.cbegin(); it != entries.cend(); ++it) {
        if (it.value().metaType() != QMetaType::fromType<QVariantMap>()) return false;
        const auto entry = it.value().toMap();
        if (entry.value("present").metaType() != QMetaType::fromType<bool>()) return false;
        if (!entry.value("present").toBool()) {
            if (entry.size() != 1) return false;
            restored.remove(it.key());
        } else {
            if (entry.size() != 2 || entry.value("value").metaType() != QMetaType::fromType<T>()) return false;
            restored.insert(it.key(), entry.value("value").value<T>());
        }
    }
    values.swap(restored);
    return true;
}

bool canBind(const CModelDocument& document, const QVector<Builder::BindingRequest>& bindings, QString* error)
{
    QVector<int> partParents;
    for (const auto& part : document.Parts()) partParents.append(part.parentIndex);
    if (!CParentGraph(partParents).IsValid()) return fail(error, QStringLiteral("模型 Part 父引用无效"));
    QHash<QString, int> meshes, deformers;
    for (int index = 0; index < document.Drawables().size(); ++index) meshes.insert(document.Drawables()[index].id, index);
    for (int index = 0; index < document.Deformers().size(); ++index) deformers.insert(document.Deformers()[index].id, index);
    QSet<QString> visited;
    for (const auto& binding : bindings) {
        const auto& owner = binding.owner;
        if (owner.type != Builder::Owner::ArtMesh && owner.type != Builder::Owner::Warp && owner.type != Builder::Owner::Rotation)
            return fail(error, QStringLiteral("对象类型不支持参数绑定"));
        const QString type = owner.type == Builder::Owner::ArtMesh ? QStringLiteral("artmesh")
            : owner.type == Builder::Owner::Warp ? QStringLiteral("warp") : QStringLiteral("rotation");
        const QString key = type + '|' + owner.id;
        if (visited.contains(key)) return fail(error, QStringLiteral("绑定对象重复：%1").arg(owner.id));
        visited.insert(key);
        int deformer = -1, part = -1;
        if (owner.type == Builder::Owner::ArtMesh) {
            const int index = meshes.value(owner.id, -1);
            if (index < 0) return fail(error, QStringLiteral("未知 ArtMesh：%1").arg(owner.id));
            part = document.Drawables()[index].parentPartIndex;
            deformer = document.Drawables()[index].parentDeformerIndex;
        } else {
            deformer = deformers.value(owner.id, -1);
            if (deformer < 0 || (owner.type == Builder::Owner::Warp) != (document.Deformers()[deformer].type == CModelDeformer::Warp))
                return fail(error, QStringLiteral("变形器不存在或类型不匹配：%1").arg(owner.id));
        }
        if (document.IsLocked(type, owner.id)) return fail(error, QStringLiteral("绑定对象已锁定：%1").arg(owner.id));
        QSet<int> seenParts, seenDeformers;
        const auto checkParts = [&](int start) {
            for (int index = start; index != -1;) {
                if (index < 0 || index >= document.Parts().size()) return false;
                if (seenParts.contains(index)) return true;
                seenParts.insert(index);
                const auto& value = document.Parts()[index];
                if (document.IsLocked(QStringLiteral("part"), value.id)) return false;
                index = value.parentIndex;
            }
            return true;
        };
        if (!checkParts(part)) return fail(error, QStringLiteral("绑定对象的 Part 已锁定或父引用无效：%1").arg(owner.id));
        while (deformer != -1) {
            if (deformer < 0 || deformer >= document.Deformers().size() || seenDeformers.contains(deformer))
                return fail(error, QStringLiteral("绑定对象的父变形器关系无效：%1").arg(owner.id));
            seenDeformers.insert(deformer);
            const auto& value = document.Deformers()[deformer];
            const QString ancestorType = value.type == CModelDeformer::Warp ? QStringLiteral("warp") : QStringLiteral("rotation");
            if (document.IsLocked(ancestorType, value.id) || !checkParts(value.parentPartIndex))
                return fail(error, QStringLiteral("绑定对象的父级已锁定：%1").arg(owner.id));
            deformer = document.DeformerParentIndex(value.id);
        }
    }
    return true;
}

bool validSlice(qint64 begin, qint64 count, qint64 size)
{
    return count >= 0 && (count == 0 ? begin >= -1 && begin <= size : begin >= 0 && begin <= size && count <= size - begin);
}

bool auxiliaryIdentity(const QByteArray& original, const CSectionTable& before,
                       const QByteArray& working, const CSectionTable& after, QString* error)
{
    const auto originalSpans = CSectionValidation::ArraySpans(*before.sectionOffsetTable, *before.count_info);
    const auto workingSpans = CSectionValidation::ArraySpans(*after.sectionOffsetTable, *after.count_info);
    if (originalSpans.size() != workingSpans.size()) return fail(error, QStringLiteral("参数结构改变了区段目录"));
    const QSet<QByteArray> fixed{
        "parts.ids", "parts.is_visible", "parts.is_enabled", "parts.parent_part_indices",
        "glue.ids", "glue.art_mesh_indices_a", "glue.art_mesh_indices_b", "glue.glue_info_sources_begin_indices",
        "glue.glue_info_sources_counts", "glue_info.weights", "glue_info.position_indices"};
    for (int index = 0; index < originalSpans.size(); ++index) {
        const auto& a = originalSpans[index]; const auto& b = workingSpans[index];
        if (!fixed.contains(a.name)) continue;
        if (a.slot != b.slot || a.count != b.count || a.width != b.width ||
            QByteArrayView(original).sliced(a.offset, qint64(a.count) * a.width) !=
                QByteArrayView(working).sliced(b.offset, qint64(b.count) * b.width))
            return fail(error, QStringLiteral("参数结构改变了 Part 或 Glue 身份与连接：%1").arg(QString::fromLatin1(a.name)));
    }
    for (const auto* part : after.parts)
        if (!validSlice(part->keyform_sources_begin_indices, part->keyform_sources_counts, after.part_keyforms.size()))
            return fail(error, QStringLiteral("Part 关键形范围无效：%1").arg(part->ids));
    for (const auto* form : after.part_keyforms)
        if (!std::isfinite(form->draw_orders)) return fail(error, QStringLiteral("Part 关键形数值无效"));
    for (const auto* glue : after.glue) {
        if (glue->art_mesh_indices_a < 0 || glue->art_mesh_indices_a >= after.art_meshes.size() ||
            glue->art_mesh_indices_b < 0 || glue->art_mesh_indices_b >= after.art_meshes.size() ||
            !validSlice(glue->glue_info_sources_begin_indices, glue->glue_info_sources_counts, after.glue_info.size()) ||
            !validSlice(glue->keyform_sources_begin_indices, glue->keyform_sources_counts, after.glue_keyforms.size()))
            return fail(error, QStringLiteral("Glue 连接或关键形范围无效：%1").arg(glue->ids));
    }
    for (const auto* form : after.glue_keyforms)
        if (!std::isfinite(form->intensities)) return fail(error, QStringLiteral("Glue 关键形数值无效"));
    return true;
}

bool canDelete(const CModelDocument& document, const CSectionTable& table, const Builder::DeleteImpact& impact, QString* error)
{
    QVector<int> parts, parents;
    for (const auto& part : document.Parts()) parts.append(part.parentIndex);
    for (const auto& deformer : document.Deformers())
        parents.append(document.DeformerParentOverrides().value(deformer.id, deformer.parentDeformerIndex));
    const CParentGraph partGraph(parts), deformerGraph(parents);
    if (!partGraph.IsValid() || !deformerGraph.IsValid()) return fail(error, QStringLiteral("对象父关系无效"));
    QVector<bool> partLocked(parts.size()), deformerLocked(parents.size());
    QHash<QString, bool> locked;
    const auto save = [&](const QString& type, const QString& id, bool value) {
        const QString key = type + '|' + id;
        locked.insert(key, value || locked.value(key));
    };
    const auto blockedPart = [&](int index) { return index < -1 || index >= partLocked.size() || (index >= 0 && partLocked[index]); };
    for (int index : partGraph.TopologicalOrder()) {
        const auto& part = document.Parts()[index];
        partLocked[index] = document.IsLocked("part", part.id) || (part.parentIndex >= 0 && partLocked[part.parentIndex]);
        save("part", part.id, partLocked[index]);
    }
    for (int index : deformerGraph.TopologicalOrder()) {
        const auto& deformer = document.Deformers()[index];
        const QString type = deformer.type == CModelDeformer::Warp ? "warp" : "rotation";
        deformerLocked[index] = document.IsLocked(type, deformer.id) || blockedPart(deformer.parentPartIndex) ||
            (parents[index] >= 0 && deformerLocked[parents[index]]);
        save(type, deformer.id, deformerLocked[index]);
    }
    QVector<bool> meshLocked;
    for (const auto& mesh : document.Drawables()) {
        const int parent = mesh.parentDeformerIndex;
        const bool value = document.IsLocked("artmesh", mesh.id) || blockedPart(mesh.parentPartIndex) ||
            parent < -1 || parent >= deformerLocked.size() || (parent >= 0 && deformerLocked[parent]);
        meshLocked.append(value);
        save("artmesh", mesh.id, value);
    }
    for (const auto* glue : table.glue) {
        const int a = glue->art_mesh_indices_a, b = glue->art_mesh_indices_b;
        const bool value = document.IsLocked("glue", glue->ids) || a < 0 || b < 0 || a >= meshLocked.size() || b >= meshLocked.size() ||
            meshLocked[a] || meshLocked[b];
        save("glue", glue->ids, value);
    }
    const auto check = [&](const QString& type, const QString& id) {
        if (id.isEmpty()) return true;
        const QString key = type + '|' + id;
        return locked.contains(key) && !locked.value(key);
    };
    for (const auto& target : impact.targets) {
        QString type;
        switch (target.type) {
        case Builder::DeleteTarget::ArtMesh: type = "artmesh"; break;
        case Builder::DeleteTarget::Warp: type = "warp"; break;
        case Builder::DeleteTarget::Rotation: type = "rotation"; break;
        case Builder::DeleteTarget::Part: type = "part"; break;
        case Builder::DeleteTarget::Glue: type = "glue"; break;
        default: return fail(error, QStringLiteral("删除影响包含未知对象类型"));
        }
        if (!check(type, target.id)) return fail(error, QStringLiteral("受影响对象或父级已锁定：%1").arg(target.id));
    }
    const QHash<QString, QString> types{{"ArtMesh", "artmesh"}, {"WarpDeformer", "warp"}, {"RotationDeformer", "rotation"}, {"Part", "part"}, {"Glue", "glue"}};
    for (const auto& reference : impact.references) {
        const auto type = types.constFind(reference.ownerType);
        if (type != types.cend() && !check(*type, reference.ownerId))
            return fail(error, QStringLiteral("引用对象或父级已锁定：%1").arg(reference.ownerId));
    }
    return true;
}

bool sameAxes(const QVector<CKeyformBindingGrid::Axis>& first, const QVector<CKeyformBindingGrid::Axis>& second)
{
    if (first.size() != second.size()) return false;
    for (int index = 0; index < first.size(); ++index)
        if (first[index].parameterId != second[index].parameterId || first[index].parameterType != second[index].parameterType ||
            first[index].keys != second[index].keys) return false;
    return true;
}
}

QVariantMap CModelDocument::EditStorage(const CModelEdit& edit) const
{
    QVariantMap state;
    QSet<QString> ids{edit.name};
    switch (edit.field) {
    case CModelEdit::ParameterValues:
        ids.clear();
        for (const auto& value : {edit.before, edit.after}) {
            const auto values = value.value<QMap<QString, float>>();
            for (auto it = values.cbegin(); it != values.cend(); ++it) ids.insert(it.key());
        }
        [[fallthrough]];
    case CModelEdit::ParameterValue:
        captureEntries(state, "parameters", m_parameterValues, ids); break;
    case CModelEdit::ParameterMetadata:
        captureEntries(state, "parameters", m_parameterValues, ids);
        captureEntries(state, "metadata", m_parameterMetadataOverrides, ids); break;
    case CModelEdit::Opacity: captureEntries(state, "opacity", m_drawableOpacityOverrides, ids); break;
    case CModelEdit::DrawOrder: captureEntries(state, "drawOrder", m_drawableDrawOrderOverrides, ids); break;
    case CModelEdit::KeyformPositions: captureEntries(state, "positions", m_drawableKeyformPositionOverrides, ids); break;
    case CModelEdit::MeshIndices: captureEntries(state, "indices", m_drawableIndexOverrides, ids); break;
    case CModelEdit::MaskIndices: captureEntries(state, "masks", m_drawableMaskIndexOverrides, ids); break;
    case CModelEdit::MaskInverted: captureEntries(state, "inverted", m_drawableMaskInvertedOverrides, ids); break;
    case CModelEdit::DeformerParent: captureEntries(state, "parents", m_deformerParentOverrides, ids); break;
    case CModelEdit::WarpKeyforms: captureEntries(state, "warp", m_deformerWarpKeyformOverrides, ids); break;
    case CModelEdit::RotationKeyforms: captureEntries(state, "rotation", m_deformerRotationKeyformOverrides, ids); break;
    default: break;
    }
    return state;
}

bool CModelDocument::RestoreEditStorage(const QVariantMap& state)
{
    return restoreEntries(state, "parameters", m_parameterValues) && restoreEntries(state, "metadata", m_parameterMetadataOverrides) &&
        restoreEntries(state, "opacity", m_drawableOpacityOverrides) && restoreEntries(state, "drawOrder", m_drawableDrawOrderOverrides) &&
        restoreEntries(state, "positions", m_drawableKeyformPositionOverrides) && restoreEntries(state, "indices", m_drawableIndexOverrides) &&
        restoreEntries(state, "masks", m_drawableMaskIndexOverrides) && restoreEntries(state, "inverted", m_drawableMaskInvertedOverrides) &&
        restoreEntries(state, "parents", m_deformerParentOverrides) && restoreEntries(state, "warp", m_deformerWarpKeyformOverrides) &&
        restoreEntries(state, "rotation", m_deformerRotationKeyformOverrides);
}

bool CModelDocument::ValidateEditStorage(const CModelEdit& edit) const
{
    CModelDocument check = *this;
    const QString& id = edit.name;
    switch (edit.field) {
    case CModelEdit::ParameterMetadata:
        if (m_parameterMetadataOverrides.contains(id) && !check.SetParameterMetadata(id, m_parameterMetadataOverrides.value(id))) return false;
        [[fallthrough]];
    case CModelEdit::ParameterValue:
        return !m_parameterValues.contains(id) || check.SetParameterValue(id, m_parameterValues.value(id));
    case CModelEdit::ParameterValues: {
        const auto entries = EditStorage(edit).value("parameters").toMap();
        for (auto it = entries.cbegin(); it != entries.cend(); ++it)
            if (m_parameterValues.contains(it.key()) && !check.SetParameterValue(it.key(), m_parameterValues.value(it.key()))) return false;
        return true;
    }
    case CModelEdit::Opacity:
        return !m_drawableOpacityOverrides.contains(id) || check.SetDrawableOpacity(id, m_drawableOpacityOverrides.value(id));
    case CModelEdit::DrawOrder:
        return !m_drawableDrawOrderOverrides.contains(id) || check.SetDrawableDrawOrder(id, m_drawableDrawOrderOverrides.value(id));
    case CModelEdit::KeyformPositions: {
        const auto forms = m_drawableKeyformPositionOverrides.value(id);
        for (int index = 0; index < forms.size(); ++index)
            if (!forms[index].isEmpty() && !check.SetDrawableKeyformPositions(id, index, forms[index])) return false;
        return true;
    }
    case CModelEdit::MeshIndices:
        return !m_drawableIndexOverrides.contains(id) || check.SetDrawableIndices(id, m_drawableIndexOverrides.value(id));
    case CModelEdit::MaskIndices:
        return !m_drawableMaskIndexOverrides.contains(id) || check.SetDrawableMaskIndices(id, m_drawableMaskIndexOverrides.value(id));
    case CModelEdit::MaskInverted:
        return !m_drawableMaskInvertedOverrides.contains(id) || check.SetDrawableMaskInverted(id, m_drawableMaskInvertedOverrides.value(id));
    case CModelEdit::DeformerParent: {
        QVector<int> parents;
        for (const auto& deformer : m_deformers) parents.append(m_deformerParentOverrides.value(deformer.id, deformer.parentDeformerIndex));
        return CParentGraph(parents).IsValid() && check.SetDeformerParentIndices(m_deformerParentOverrides);
    }
    case CModelEdit::WarpKeyforms:
        return !m_deformerWarpKeyformOverrides.contains(id) || check.SetDeformerWarpKeyforms(id, m_deformerWarpKeyformOverrides.value(id));
    case CModelEdit::RotationKeyforms:
        return !m_deformerRotationKeyformOverrides.contains(id) || check.SetDeformerRotationKeyforms(id, m_deformerRotationKeyformOverrides.value(id));
    default: return true;
    }
}

QVariantMap CModelDocument::ParameterStructureState() const
{
    QVariantMap state{
        {"sourceDigest", m_sourceDigest}, {"sourcePath", m_sourcePath}, {"columns", QVariant::fromValue(m_structure.columns)},
        {"counts", QVariant::fromValue(m_structure.counts)}, {"parameters", QVariant::fromValue(m_parameterValues)},
        {"parameterMetadata", QVariant::fromValue(m_parameterMetadataOverrides)}, {"visibility", QVariant::fromValue(VisibilityState())},
        {"locked", QVariant::fromValue(m_lockedKeys)}, {"opacity", QVariant::fromValue(m_drawableOpacityOverrides)},
        {"drawOrder", QVariant::fromValue(m_drawableDrawOrderOverrides)}, {"positions", QVariant::fromValue(m_drawableKeyformPositionOverrides)},
        {"indices", QVariant::fromValue(m_drawableIndexOverrides)}, {"masks", QVariant::fromValue(m_drawableMaskIndexOverrides)},
        {"parents", QVariant::fromValue(m_deformerParentOverrides)}, {"rotation", QVariant::fromValue(m_deformerRotationKeyformOverrides)},
        {"warp", QVariant::fromValue(m_deformerWarpKeyformOverrides)}, {"inverted", QVariant::fromValue(m_drawableMaskInvertedOverrides)}
    };
    QByteArray exact;
    QDataStream stream(&exact, QIODevice::WriteOnly);
    stream.setVersion(QDataStream::Qt_6_0);
    stream.setFloatingPointPrecision(QDataStream::SinglePrecision);
    stream << ordered(m_parameterValues) << ordered(m_drawableOpacityOverrides) << ordered(m_drawableKeyformPositionOverrides);
    const auto metadata = ordered(m_parameterMetadataOverrides);
    stream << qint64(metadata.size());
    for (auto it = metadata.cbegin(); it != metadata.cend(); ++it)
        stream << it.key() << it->minimum << it->maximum << it->defaultValue;
    const auto rotations = ordered(m_deformerRotationKeyformOverrides);
    stream << qint64(rotations.size());
    for (auto it = rotations.cbegin(); it != rotations.cend(); ++it) {
        stream << it.key() << qint64(it->size());
        for (const auto& form : *it) stream << form.opacity << form.angle << form.originX << form.originY << form.scale << form.reflectX << form.reflectY;
    }
    const auto warps = ordered(m_deformerWarpKeyformOverrides);
    stream << qint64(warps.size());
    for (auto it = warps.cbegin(); it != warps.cend(); ++it) {
        stream << it.key() << qint64(it->size());
        for (const auto& form : *it) stream << form.positions << form.opacity;
    }
    state.insert(QStringLiteral("fingerprint"), QCryptographicHash::hash(exact, QCryptographicHash::Sha256));
    return state;
}

bool CModelDocument::PrepareParameterStructure(const Builder::ColumnState& structure, CModelDocument& candidate, QString* error) const
{
    if (!m_loaded) return fail(error, QStringLiteral("尚未加载模型"));
    QByteArray working;
    if (!Builder::BuildColumnState(m_sourceBytes, structure, working, error)) return false;
    {
        auto sourceReader = CReader::FromBytes(m_sourceBytes), workingReader = CReader::FromBytes(working);
        CSectionTable sourceTable(sourceReader), workingTable(workingReader);
        if (!sourceTable.IsValid() || !workingTable.IsValid()) return fail(error, sourceReader.Error() + workingReader.Error());
        if (!auxiliaryIdentity(m_sourceBytes, sourceTable, working, workingTable, error)) return false;
    }
    CModelDocument original, next;
    if (!original.LoadBytes(m_sourceBytes, m_sourcePath, error) || !next.LoadBytes(working, m_sourcePath, error)) return false;
    if (!next.m_previewBuilder || next.m_parts.size() != original.m_parts.size() ||
        next.m_deformers.size() != original.m_deformers.size() || next.m_drawables.size() != original.m_drawables.size() ||
        next.m_canvasPixels != original.m_canvasPixels)
        return fail(error, QStringLiteral("参数结构不能改变模型对象目录或画布"));
    QVector<int> partParents;
    for (int index = 0; index < original.m_parts.size(); ++index) {
        const auto& before = original.m_parts[index]; const auto& after = next.m_parts[index];
        if (before.id != after.id || before.parentIndex != after.parentIndex || before.visible != after.visible || before.enabled != after.enabled)
            return fail(error, QStringLiteral("参数结构改变了 Part 定义：%1").arg(before.id));
        partParents.append(after.parentIndex);
    }
    if (!CParentGraph(partParents).IsValid()) return fail(error, QStringLiteral("参数结构包含无效 Part 父引用"));
    QVector<int> parents;
    for (int index = 0; index < original.m_deformers.size(); ++index) {
        const auto& before = original.m_deformers[index]; const auto& after = next.m_deformers[index];
        if (before.id != after.id || before.type != after.type || before.parentPartIndex != after.parentPartIndex ||
            before.rows != after.rows || before.columns != after.columns || before.baseAngle != after.baseAngle ||
            before.quadTransform != after.quadTransform || before.visible != after.visible || before.enabled != after.enabled)
            return fail(error, QStringLiteral("参数结构改变了变形器定义：%1").arg(before.id));
        if (after.parentDeformerIndex >= 0 && after.parentDeformerIndex != before.parentDeformerIndex &&
            (after.parentDeformerIndex >= next.m_deformers.size() ||
             next.m_deformers[after.parentDeformerIndex].parentPartIndex != after.parentPartIndex))
            return fail(error, QStringLiteral("参数结构新增了跨 Part 的父变形器关系：%1").arg(before.id));
        parents.append(after.parentDeformerIndex);
    }
    if (!CParentGraph(parents).IsValid()) return fail(error, QStringLiteral("参数结构包含无效父变形器关系"));
    for (int index = 0; index < original.m_drawables.size(); ++index) {
        const auto& before = original.m_drawables[index]; const auto& after = next.m_drawables[index];
        if (before.id != after.id || before.parentPartIndex != after.parentPartIndex || before.parentDeformerIndex != after.parentDeformerIndex ||
            before.vertexCount != after.vertexCount || before.textureIndex != after.textureIndex ||
            (before.flags & ~8) != (after.flags & ~8) || before.uvs != after.uvs || before.indices != after.indices ||
            before.visible != after.visible || before.enabled != after.enabled)
            return fail(error, QStringLiteral("参数结构改变了 ArtMesh 拓扑或定义：%1").arg(before.id));
    }
    if (next.m_drawOrderGroups.size() != original.m_drawOrderGroups.size()) return fail(error, QStringLiteral("参数结构改变了绘制顺序组"));
    for (int index = 0; index < original.m_drawOrderGroups.size(); ++index) {
        const auto& before = original.m_drawOrderGroups[index]; const auto& after = next.m_drawOrderGroups[index];
        if (before.minimumDrawOrder != after.minimumDrawOrder || before.maximumDrawOrder != after.maximumDrawOrder ||
            before.objectCount != after.objectCount || before.totalObjectCount != after.totalObjectCount ||
            before.objectsValid != after.objectsValid || before.objects.size() != after.objects.size())
            return fail(error, QStringLiteral("参数结构改变了绘制顺序组定义"));
        for (int object = 0; object < before.objects.size(); ++object)
            if (before.objects[object].type != after.objects[object].type || before.objects[object].index != after.objects[object].index ||
                before.objects[object].selfIndex != after.objects[object].selfIndex)
                return fail(error, QStringLiteral("参数结构改变了绘制顺序组成员"));
    }
    QSet<QString> ids;
    for (const auto& parameter : next.m_parameters) {
        if (parameter.id.isEmpty() || ids.contains(parameter.id) || !std::isfinite(parameter.minimum) || !std::isfinite(parameter.maximum) ||
            !std::isfinite(parameter.defaultValue) || parameter.minimum > parameter.defaultValue || parameter.defaultValue > parameter.maximum ||
            parameter.decimalPlaces < 0 || parameter.decimalPlaces > 9 || (parameter.type != 0 && parameter.type != 1))
            return fail(error, QStringLiteral("参数结构包含无效或重复的参数定义"));
        ids.insert(parameter.id);
    }
    const auto validateGrid = [&](CKeyformBindingGrid::ObjectType type, const QString& id) {
        return !original.m_keyformBindings->Diagnostic(type, id).isEmpty() || next.m_keyformBindings->Diagnostic(type, id).isEmpty();
    };
    for (const auto& mesh : next.m_drawables)
        if (!validateGrid(CKeyformBindingGrid::ArtMesh, mesh.id)) return fail(error, QStringLiteral("参数结构破坏了 ArtMesh 绑定：%1").arg(mesh.id));
    for (const auto& deformer : next.m_deformers)
        if (!validateGrid(deformer.type == CModelDeformer::Warp ? CKeyformBindingGrid::Warp : CKeyformBindingGrid::Rotation, deformer.id))
            return fail(error, QStringLiteral("参数结构破坏了变形器绑定：%1").arg(deformer.id));
    next.m_structure = structure;
    next.m_sourceDeformerParents = original.m_sourceDeformerParents;
    auto preview = std::make_shared<CMocPreviewBuilder>();
    if (!preview->Initialize(working, next.m_sourceDeformerParents, error)) return false;
    next.m_previewBuilder = std::move(preview);
    next.m_previewError.clear();
    next.m_sourceBytes = m_sourceBytes;
    next.m_sourcePath = m_sourcePath;
    next.m_sourceDigest = m_sourceDigest;
    next.m_workingBaseBytes = std::move(working);
    candidate = std::move(next);
    return true;
}

bool CModelDocument::SetParameterStructure(const Builder::ColumnState& structure, QString* error)
{
    try {
        CModelDocument candidate;
        if (!PrepareParameterStructure(structure, candidate, error)) return false;
        CommitCandidate(std::move(candidate));
        return true;
    } catch (const std::exception&) { return fail(error, QStringLiteral("无法恢复参数结构")); }
}

bool CModelDocument::ApplyParameterStructureState(const QVariantMap& state, QString* error)
{
    try {
        const auto current = ParameterStructureState();
        if (state.keys() != current.keys() || state.value("sourceDigest") != m_sourceDigest || state.value("sourcePath") != m_sourcePath)
            return fail(error, QStringLiteral("参数结构状态来源或字段不匹配"));
        CModelSession session;
        QHash<QString, float> parameters;
        QSet<QString> locks;
        if (!read(state, "columns", session.structure.columns) || !read(state, "counts", session.structure.counts) ||
            !read(state, "parameters", parameters) || !read(state, "parameterMetadata", session.parameterMetadata) ||
            !read(state, "visibility", session.visibility) || !read(state, "locked", locks) ||
            !read(state, "opacity", session.drawableOpacity) || !read(state, "drawOrder", session.drawableDrawOrder) ||
            !read(state, "positions", session.drawableKeyformPositions) || !read(state, "indices", session.drawableIndices) ||
            !read(state, "masks", session.drawableMaskIndices) || !read(state, "parents", session.deformerParentIndices) ||
            !read(state, "rotation", session.deformerRotationKeyforms) || !read(state, "warp", session.deformerWarpKeyforms) ||
            !read(state, "inverted", session.drawableMaskInverted))
            return fail(error, QStringLiteral("参数结构状态类型无效"));
        session.parameters = ordered(parameters);
        for (const auto& lock : locks) session.locked.insert(lock, true);
        session.sourceDigest = m_sourceDigest;
        session.version = session.structure.columns.isEmpty() && session.structure.counts.isEmpty() ? 1 : 2;
        CModelDocument candidate;
        if (!session.RestoreDocument(*this, candidate, error)) return false;
        if (candidate.ParameterStructureState() != state) return fail(error, QStringLiteral("参数结构状态内容无效或发生归一化"));
        QVector<Builder::BindingRequest> changed;
        const auto examine = [&](Builder::Owner::Type type, const QString& id) {
            const auto gridType = static_cast<CKeyformBindingGrid::ObjectType>(type);
            if (!sameAxes(m_keyformBindings->Axes(gridType, id), candidate.m_keyformBindings->Axes(gridType, id)))
                changed.append(Builder::BindingRequest{{type, id}, {}});
        };
        for (const auto& mesh : m_drawables) examine(Builder::Owner::ArtMesh, mesh.id);
        for (const auto& deformer : m_deformers)
            examine(deformer.type == CModelDeformer::Warp ? Builder::Owner::Warp : Builder::Owner::Rotation, deformer.id);
        if (!canBind(*this, changed, error)) return false;
        candidate.m_editHistory = m_editHistory;
        candidate.m_editIndex = m_editIndex;
        CommitCandidate(std::move(candidate));
        return true;
    } catch (const std::exception&) { return fail(error, QStringLiteral("无法应用参数结构状态")); }
}

bool CModelDocument::MakeParameterStructureEdit(const Builder::Result& result, const QString& id,
                                               const QString* displayName, bool created, CModelEdit& edit, QString* error) const
{
    Builder::ColumnState structure;
    if (!Builder::CaptureColumnState(m_sourceBytes, result.bytes, structure, error)) return false;
    CModelDocument candidate;
    if (!PrepareParameterStructure(structure, candidate, error)) return false;
    const auto visibility = VisibilityState();
    if (!visibility.isEmpty() && !candidate.SetVisibilityValues(visibility)) return fail(error, QStringLiteral("无法保留对象可见性"));
    candidate.m_drawableOpacityOverrides = m_drawableOpacityOverrides;
    candidate.m_drawableDrawOrderOverrides = m_drawableDrawOrderOverrides;
    candidate.m_drawableIndexOverrides = m_drawableIndexOverrides;
    candidate.m_lockedKeys = m_lockedKeys;
    for (const auto& parameter : candidate.m_parameters) {
        if (m_parameterValues.contains(parameter.id)) candidate.m_parameterValues.insert(parameter.id, m_parameterValues.value(parameter.id));
        const auto previous = m_parameterMetadataOverrides.constFind(parameter.id);
        if (previous != m_parameterMetadataOverrides.cend() && !previous->displayName.isEmpty() && previous->displayName != parameter.id) {
            auto metadata = candidate.ParameterMetadata(parameter.id);
            metadata.displayName = previous->displayName;
            candidate.m_parameterMetadataOverrides.insert(parameter.id, metadata);
        }
    }
    if (created) {
        if (candidate.ParameterMetadataState(id).isEmpty()) return fail(error, QStringLiteral("创建结果缺少唯一参数：%1").arg(id));
        if (displayName && displayName->contains(QChar::Null)) return fail(error, QStringLiteral("显示名包含空字符"));
        auto metadata = candidate.ParameterMetadata(id);
        if (displayName && !displayName->isEmpty() && *displayName != id) {
            metadata.displayName = *displayName;
            candidate.m_parameterMetadataOverrides.insert(id, metadata);
        }
        candidate.m_parameterValues.insert(id, metadata.defaultValue);
    }
    CModelEdit command{id, QStringLiteral("parameter"), CModelEdit::ParameterStructure,
                       ParameterStructureState(), candidate.ParameterStructureState(), -1, false};
    edit = std::move(command);
    if (error) error->clear();
    return true;
}

bool CModelDocument::MakeCreateParameterEdit(const Builder::CreateRequest& request, const QString& displayName,
                                           CModelEdit& edit, QString* error) const
{
    try {
        if (!canBind(*this, request.bindings, error)) return false;
        QByteArray bytes;
        Builder::Result result;
        if (!BuildPreviewMoc(bytes, error) || !Builder::Create(bytes, request, result, error) ||
            !MakeParameterStructureEdit(result, request.parameter.id, &displayName, true, edit, error)) return false;
        edit.parameterChange = CModelEdit::ParameterChange::Create;
        return true;
    } catch (const std::exception&) { return fail(error, QStringLiteral("无法创建参数")); }
}

bool CModelDocument::MakeBindParameterEdit(const QString& id, const QVector<Builder::BindingRequest>& bindings,
                                         CModelEdit& edit, QString* error) const
{
    try {
        if (bindings.isEmpty()) return fail(error, QStringLiteral("没有选定绑定对象"));
        if (!canBind(*this, bindings, error)) return false;
        QByteArray bytes;
        Builder::Result result;
        if (!BuildPreviewMoc(bytes, error) || !Builder::BindExistingParameter(bytes, id, bindings, result, error) ||
            !MakeParameterStructureEdit(result, id, nullptr, false, edit, error)) return false;
        edit.parameterChange = CModelEdit::ParameterChange::Bind;
        return true;
    } catch (const std::exception&) { return fail(error, QStringLiteral("无法绑定参数")); }
}

bool CModelDocument::MakeDeleteParameterEdit(const QString& id, CModelEdit& edit, QString* error) const
{
    try {
        QByteArray bytes;
        Builder::Result result;
        if (!BuildPreviewMoc(bytes, error) || !Builder::DeleteUnreferenced(bytes, id, result, error) ||
            !MakeParameterStructureEdit(result, id, nullptr, false, edit, error)) return false;
        edit.parameterChange = CModelEdit::ParameterChange::Delete;
        return true;
    } catch (const std::exception&) { return fail(error, QStringLiteral("无法删除参数")); }
}

bool CModelDocument::MakeDeleteParameterEdit(const Builder::DeleteRequest& request, CModelEdit& edit, QString* error) const
{
    try {
        QByteArray bytes;
        if (!BuildPreviewMoc(bytes, error)) return false;
        Builder::DeleteImpact impact;
        if (!Builder::AnalyzeDelete(bytes, request, impact, error)) return false;
        if (!impact.blockingReasons.isEmpty()) return fail(error, impact.blockingReasons.join(QLatin1Char('\n')));
        {
            auto reader = CReader::FromBytes(bytes);
            CSectionTable table(reader);
            if (!table.IsValid()) return fail(error, reader.Error());
            if (!canDelete(*this, table, impact, error)) return false;
        }
        Builder::Result result;
        if (!Builder::Delete(bytes, request, result, error) ||
            !MakeParameterStructureEdit(result, request.id, nullptr, false, edit, error)) return false;
        edit.parameterChange = CModelEdit::ParameterChange::Delete;
        edit.freezeValue = request.freezeValue;
        return true;
    } catch (const std::exception&) { return fail(error, QStringLiteral("无法冻结并删除参数")); }
}

}
