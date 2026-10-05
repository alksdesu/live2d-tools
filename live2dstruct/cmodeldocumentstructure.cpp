#include "cmodeldocument.h"
#include "cmodelsession.h"
#include "ckeyformbindinggrid.h"
#include "cparentgraph.h"
#include "cmocpreviewbuilder.h"
#include "cmocruntimebuilder.h"
#include "cdeformerstructurereplay.h"
#include "csectiontable.h"
#include "csectionvalidation.h"
#include "cartworkreconciler.h"
#include <QCryptographicHash>
#include <QByteArrayView>
#include <QDataStream>
#include <QIODevice>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <exception>
#include <stdexcept>

namespace live2dstruct {
namespace {
using Builder = CMocParameterStructureBuilder;

bool fail(QString* error, const QString& message)
{
    if (error) *error = message;
    return false;
}

bool validWarpStructure(const CSectionTable& table, QString* error)
{
    constexpr qint64 maximumReferences = 1000000;
    qint64 references = 0;
    QVector<QPair<int, int>> positions;
    const auto consume = [&](qint64 count) {
        if (count < 0 || count > maximumReferences - references)
            return fail(error, QStringLiteral("Warp 结构引用总量超过限制"));
        references += count;
        return true;
    };
    const auto range = [](qint64 first, qint64 count, qint64 size) {
        return count == 0 ? first >= -1 && first <= size
                          : first >= 0 && count > 0 && first <= size && count <= size - first;
    };
    const auto forms = [&](int first, int count, int vertices) {
        if (!consume(count))
            return false;
        if (!range(first, count, table.warp_deformer_keyforms.size()))
            return fail(error, QStringLiteral("Warp 关键形引用越界"));
        for (int row = first; row < first + count; ++row) {
            const auto* form = table.warp_deformer_keyforms[row];
            const int begin = form->keyform_position_sources_begin_indices;
            const qint64 coordinates = qint64(vertices) * 2;
            if (!range(begin, coordinates, table.keyform_positions->xys.size()) || !std::isfinite(form->opacities))
                return fail(error, QStringLiteral("Warp 关键形位置引用或透明度无效"));
            positions.append({ begin, int(begin + coordinates) });
        }
        return true;
    };
    for (const auto* warp : table.warp_deformers) {
        if (!consume(1))
            return false;
        const qint64 vertices
            = warp->rows >= 0 && warp->columns >= 0 ? (qint64(warp->rows) + 1) * (qint64(warp->columns) + 1) : 0;
        if (vertices < 1 || vertices != warp->vertex_counts)
            return fail(error, QStringLiteral("Warp 行列数与控制点数量不一致"));
        if (!forms(warp->keyform_sources_begin_indices, warp->keyform_sources_counts, warp->vertex_counts))
            return false;
    }
    for (const auto* shape : table.blend_shapes_warp_deformers) {
        if (!consume(1))
            return false;
        const int target = shape->target_indices;
        const int first = shape->blend_shape_keyform_binding_sources_begin_indices;
        const int count = shape->blend_shape_keyform_binding_sources_counts;
        if (target < 0 || target >= table.warp_deformers.size()
            || !range(first, count, table.blend_shape_keyform_bindings.size()))
            return fail(error, QStringLiteral("Warp BlendShape 目标或绑定引用越界"));
        if (!consume(count))
            return false;
        for (int row = first; row < first + count; ++row) {
            const auto* binding = table.blend_shape_keyform_bindings[row];
            if (!forms(binding->keyform_sources_blend_shape_indices, binding->keyform_sources_blend_shape_counts,
                    table.warp_deformers[target]->vertex_counts))
                return false;
        }
    }
    std::sort(positions.begin(), positions.end());
    int checked = 0;
    for (const auto& interval : positions) {
        for (int index = std::max(checked, interval.first); index < interval.second; ++index)
            if (!std::isfinite(table.keyform_positions->xys[index]))
                return fail(error, QStringLiteral("Warp 关键形位置包含非有限值"));
        checked = std::max(checked, interval.second);
    }
    return true;
}

CDeformerStructureRequest ownedDeformerRequest(const CDeformerStructureRequest& source)
{
    const auto text = [](const QString& value) {
        if (value.size() > 64) throw std::length_error("Deformer identifier exceeds MOC capacity");
        return QString(value.constData(), value.size());
    };
    CDeformerStructureRequest result = source;
    result.sourceDigest = QByteArray(source.sourceDigest.constData(), source.sourceDigest.size());
    result.expectedAuthoringDigest = QByteArray(source.expectedAuthoringDigest.constData(), source.expectedAuthoringDigest.size());
    std::visit([&](auto& operation) { operation.id = text(operation.id); }, result.operation);
    if (auto* insertion = std::get_if<CMocDeformerStructureBuilder::InsertRequest>(&result.operation)) {
        insertion->parentId = text(insertion->parentId);
        if (insertion->children.size() > 1000000) throw std::length_error("Too many deformer children");
        QVector<CMocDeformerStructureBuilder::Owner> children;
        children.reserve(insertion->children.size());
        for (const auto& child : insertion->children) children.append({child.type, text(child.id)});
        insertion->children.swap(children);
        if (auto* warp = std::get_if<CMocDeformerStructureBuilder::Warp>(&insertion->shape)) {
            if (warp->positions.size() > 1000000) throw std::length_error("Too many warp coordinates");
            warp->positions = QVector<float>(warp->positions.cbegin(), warp->positions.cend());
        }
    }
    return result;
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

template<class T, template<class, class> class Map> void captureEntries(QVariantMap& state, const QString& name, const Map<QString, T>& values,
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

template<class T, template<class, class> class Map> bool restoreEntries(const QVariantMap& state, const QString& name, Map<QString, T>& values)
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
                       const QByteArray& working, const CSectionTable& after, bool meshStructure, QString* error)
{
    const auto originalSpans = CSectionValidation::ArraySpans(*before.sectionOffsetTable, *before.count_info);
    const auto workingSpans = CSectionValidation::ArraySpans(*after.sectionOffsetTable, *after.count_info);
    if (originalSpans.size() != workingSpans.size()) return fail(error, QStringLiteral("参数结构改变了区段目录"));
    QSet<QByteArray> fixed{
        "parts.ids", "parts.is_visible", "parts.is_enabled", "parts.parent_part_indices",
        "glue.ids", "glue.art_mesh_indices_a", "glue.art_mesh_indices_b", "glue.glue_info_sources_begin_indices",
        "glue.glue_info_sources_counts", "glue_info.weights", "glue_info.position_indices"};
    if (meshStructure)
        for (const auto* column : {"glue.glue_info_sources_begin_indices", "glue.glue_info_sources_counts",
                                  "glue_info.weights", "glue_info.position_indices"}) fixed.remove(column);
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
    qint64 glueReferences = 0;
    for (const auto* glue : after.glue) {
        if (glue->art_mesh_indices_a < 0 || glue->art_mesh_indices_a >= after.art_meshes.size() ||
            glue->art_mesh_indices_b < 0 || glue->art_mesh_indices_b >= after.art_meshes.size() ||
            !validSlice(glue->glue_info_sources_begin_indices, glue->glue_info_sources_counts, after.glue_info.size()) ||
            !validSlice(glue->keyform_sources_begin_indices, glue->keyform_sources_counts, after.glue_keyforms.size()))
            return fail(error, QStringLiteral("Glue 连接或关键形范围无效：%1").arg(glue->ids));
        if (meshStructure) {
            if (glue->glue_info_sources_counts % 2 || glue->glue_info_sources_counts > 1000000 - glueReferences)
                return fail(error, QStringLiteral("Glue 配对数量无效或超过检查总量"));
            glueReferences += glue->glue_info_sources_counts;
            for (int at = 0; at < glue->glue_info_sources_counts; ++at) {
                const auto* endpoint = after.glue_info[glue->glue_info_sources_begin_indices + at];
                const int mesh = at % 2 ? glue->art_mesh_indices_b : glue->art_mesh_indices_a;
                if (endpoint->position_indices < 0 || endpoint->position_indices >= after.art_meshes[mesh]->vertex_counts ||
                    !std::isfinite(endpoint->weights)) return fail(error, QStringLiteral("Glue 顶点或权重无效：%1").arg(glue->ids));
            }
        }
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
    case CModelEdit::AtlasLayout:
        state.insert("atlasWorkspace", QVariant::fromValue(m_textureWorkspace));
        state.insert("atlasMaterials", QVariant::fromValue(m_drawableMaterialOverrides));
        state.insert("artwork", QVariant::fromValue(m_artwork));
        state.insert("artworkFingerprint", m_artworkFingerprint);
        state.insert("materialFingerprint", CModelMaterialEdit::Fingerprint(m_drawableMaterialOverrides));
        break;
    case CModelEdit::DrawableMaterials: {
        state.insert("artwork", QVariant::fromValue(m_artwork));
        state.insert("artworkFingerprint", m_artworkFingerprint);
        CModelMaterialOverrides selected;
        ids.clear();
        for (const auto& value : {edit.before, edit.after})
            for (const auto& material : value.value<CModelMaterialState>()) ids.insert(material.id);
        captureEntries(state, "materials", m_drawableMaterialOverrides, ids);
        for (const auto& id : ids) if (m_drawableMaterialOverrides.contains(id)) selected.insert(id, m_drawableMaterialOverrides.value(id));
        state.insert("materialFingerprint", CModelMaterialEdit::Fingerprint(selected));
        break;
    }
    case CModelEdit::GeometryKeyforms: {
        QSet<QString> meshes, warps, rotations;
        for (const auto& value : {edit.before, edit.after}) {
            const auto forms = value.value<CModelGeometryState>();
            for (const auto& form : forms) {
                if (form.type == CModelGeometryKeyform::ArtMesh) meshes.insert(form.id);
                else if (form.type == CModelGeometryKeyform::Warp) warps.insert(form.id);
                else if (form.type == CModelGeometryKeyform::Rotation) rotations.insert(form.id);
            }
        }
        captureEntries(state, "positions", m_drawableKeyformPositionOverrides, meshes);
        captureEntries(state, "warp", m_deformerWarpKeyformOverrides, warps);
        captureEntries(state, "rotation", m_deformerRotationKeyformOverrides, rotations);
        state.insert("geometryFingerprint", GeometryEditStorageFingerprint(edit));
        break;
    }
    case CModelEdit::BlendShapeGeometry:
        state.insert("blendShapeGeometry", QVariant::fromValue(m_blendShapeGeometry));
        break;
    case CModelEdit::BlendShapeConstraints:
        state.insert("blendShapeConstraints", QVariant::fromValue(m_blendShapeConstraints));
        break;
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
    case CModelEdit::DeformerDisplayName:
        captureEntries(state, "deformerNames", m_displayNames.deformers, ids); break;
    case CModelEdit::DisplayNames:
        state.insert("displayNames", QVariant::fromValue(m_displayNames)); break;
    case CModelEdit::Opacity: captureEntries(state, "opacity", m_drawableOpacityOverrides, ids); break;
    case CModelEdit::DrawOrder: captureEntries(state, "drawOrder", m_drawableDrawOrderOverrides, ids); break;
    case CModelEdit::KeyformPositions: captureEntries(state, "positions", m_drawableKeyformPositionOverrides, ids); break;
    case CModelEdit::MeshIndices:
        captureEntries(state, "indices", m_drawableIndexOverrides, ids);
        state.insert("geometryObjectPairs", QVariant::fromValue(m_geometryObjectPairs));
        state.insert("artwork", QVariant::fromValue(m_artwork));
        state.insert("artworkFingerprint", m_artworkFingerprint);
        break;
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
    if (state.contains("displayNames")) {
        const auto names = state.value("displayNames");
        if (names.metaType() != QMetaType::fromType<CModelDisplayNames::State>() ||
            !SetDisplayNames(names.value<CModelDisplayNames::State>())) return false;
    }
    if (state.contains("atlasWorkspace") || state.contains("atlasMaterials")) {
        const auto workspace = state.value("atlasWorkspace"), materials = state.value("atlasMaterials");
        if (!HasTextureWorkspace() || workspace.metaType() != QMetaType::fromType<CTextureWorkspace::State>() ||
            materials.metaType() != QMetaType::fromType<CModelMaterialOverrides>()) return false;
        const auto restored = workspace.value<CTextureWorkspace::State>();
        QByteArray fingerprint;
        if (!CTextureWorkspace::Fingerprint(m_textureSources, restored, fingerprint)) return false;
        m_textureWorkspace = restored; m_drawableMaterialOverrides = materials.value<CModelMaterialOverrides>();
        m_textureWorkspaceFingerprint = std::move(fingerprint);
    }
    if (!(restoreEntries(state, "parameters", m_parameterValues) && restoreEntries(state, "metadata", m_parameterMetadataOverrides) &&
        restoreEntries(state, "opacity", m_drawableOpacityOverrides) && restoreEntries(state, "drawOrder", m_drawableDrawOrderOverrides) &&
        restoreEntries(state, "positions", m_drawableKeyformPositionOverrides) && restoreEntries(state, "indices", m_drawableIndexOverrides) &&
        restoreEntries(state, "masks", m_drawableMaskIndexOverrides) && restoreEntries(state, "inverted", m_drawableMaskInvertedOverrides) &&
        restoreEntries(state, "parents", m_deformerParentOverrides) && restoreEntries(state, "warp", m_deformerWarpKeyformOverrides) &&
        restoreEntries(state, "rotation", m_deformerRotationKeyformOverrides) &&
        restoreEntries(state, "deformerNames", m_displayNames.deformers) &&
        restoreEntries(state, "materials", m_drawableMaterialOverrides))) return false;
    if (state.contains("blendShapeGeometry")) {
        const auto value = state.value("blendShapeGeometry");
        if (value.metaType() != QMetaType::fromType<CModelBlendShapeGeometryState>() ||
            !SetBlendShapeGeometry(value.value<CModelBlendShapeGeometryState>())) return false;
    }
    if (state.contains("blendShapeConstraints")) {
        const auto value = state.value("blendShapeConstraints");
        if (value.metaType() != QMetaType::fromType<CModelBlendShapeConstraintState>() ||
            !SetBlendShapeConstraints(value.value<CModelBlendShapeConstraintState>())) return false;
    }
    if (state.contains("geometryObjectPairs")) {
        const auto value = state.value("geometryObjectPairs");
        if (value.metaType() != QMetaType::fromType<CGeometryObjectPairs::State>() ||
            !SetGeometryObjectPairs(value.value<CGeometryObjectPairs::State>())) return false;
    }
    if (state.contains("artwork")) {
        const auto artwork = state.value("artwork");
        if (artwork.metaType() != QMetaType::fromType<CModelArtwork::State>() ||
            !SetArtwork(artwork.value<CModelArtwork::State>()) || state.value("artworkFingerprint").toByteArray() != m_artworkFingerprint) return false;
    }
    return true;
}

bool CModelDocument::ValidateEditStorage(const CModelEdit& edit) const
{
    if (!edit.meshVerification.IsEmpty()) return false;
    CModelDocument check = *this;
    const QString& id = edit.name;
    switch (edit.field) {
    case CModelEdit::DisplayNames:
        return check.SetDisplayNames(m_displayNames) && check.DisplayNames() == m_displayNames;
    case CModelEdit::DeformerDisplayName:
        return check.SetDeformerDisplayNames(m_displayNames.deformers) && check.DeformerDisplayName(id) == DeformerDisplayName(id);
    case CModelEdit::AtlasLayout:
        return ValidateAtlasEditStorage(edit);
    case CModelEdit::DrawableMaterials:
        return ValidateMaterialEditStorage(edit) && ValidateArtworkReferences(m_artwork);
    case CModelEdit::GeometryKeyforms:
        return ValidateGeometryEditStorage(edit);
    case CModelEdit::BlendShapeGeometry:
        return EditStorage(edit).value("blendShapeGeometry").value<CModelBlendShapeGeometryState>() == m_blendShapeGeometry;
    case CModelEdit::BlendShapeConstraints:
        return check.SetBlendShapeConstraints(m_blendShapeConstraints);
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

QVariantMap CModelDocument::AuthoringStructureState() const
{
    QVariantMap state{
        {"sourceDigest", m_sourceDigest}, {"sourcePath", m_sourcePath}, {"meshStructure", m_hasMeshStructure},
        {"deformerStructure", m_hasDeformerStructure},
        {"materialStructure", m_hasMaterialStructure}, {"materials", QVariant::fromValue(m_drawableMaterialOverrides)},
        {"sourceDeformerIds", QVariant::fromValue(m_hasDeformerStructure ? m_sourceDeformerIds : QSet<QString>{})},
        {"columns", QVariant::fromValue(m_structure.columns)},
        {"counts", QVariant::fromValue(m_structure.counts)}, {"parameters", QVariant::fromValue(m_parameterValues)},
        {"parameterMetadata", QVariant::fromValue(m_parameterMetadataOverrides)}, {"visibility", QVariant::fromValue(VisibilityState())},
        {"deformerDisplayNames", QVariant::fromValue(m_displayNames.deformers)},
        {"partDisplayNames", QVariant::fromValue(m_displayNames.parts)},
        {"artMeshDisplayNames", QVariant::fromValue(m_displayNames.artMeshes)},
        {"parameterOrganization", QVariant::fromValue(m_parameterOrganization)},
        {"geometryObjectPairs", QVariant::fromValue(m_geometryObjectPairs)},
        {"textureWorkspace", QVariant::fromValue(m_textureWorkspace)},
        {"artwork", QVariant::fromValue(m_artwork)}, {"artworkFingerprint", m_artworkFingerprint},
        {"locked", QVariant::fromValue(m_lockedKeys)}, {"opacity", QVariant::fromValue(m_drawableOpacityOverrides)},
        {"drawOrder", QVariant::fromValue(m_drawableDrawOrderOverrides)}, {"positions", QVariant::fromValue(m_drawableKeyformPositionOverrides)},
        {"indices", QVariant::fromValue(m_drawableIndexOverrides)}, {"masks", QVariant::fromValue(m_drawableMaskIndexOverrides)},
        {"parents", QVariant::fromValue(m_deformerParentOverrides)}, {"rotation", QVariant::fromValue(m_deformerRotationKeyformOverrides)},
        {"warp", QVariant::fromValue(m_deformerWarpKeyformOverrides)}, {"inverted", QVariant::fromValue(m_drawableMaskInvertedOverrides)},
        {"blendShapeGeometry", QVariant::fromValue(m_blendShapeGeometry)},
        {"blendShapeConstraints", QVariant::fromValue(m_blendShapeConstraints)}
    };
    QByteArray exact;
    QDataStream stream(&exact, QIODevice::WriteOnly);
    stream.setVersion(QDataStream::Qt_6_0);
    stream.setFloatingPointPrecision(QDataStream::SinglePrecision);
    stream << CModelMaterialEdit::Fingerprint(m_drawableMaterialOverrides);
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
    for (const auto& entry : m_blendShapeGeometry) {
        stream << entry.meshId << entry.shapeIndex << entry.bindingOffset << entry.keyformOffset << entry.sourceKeyformIndex
               << entry.sourceDigest << qint64(entry.deltas.size());
        for (float value : entry.deltas) {
            quint32 bits = 0; std::memcpy(&bits, &value, sizeof(bits)); stream << bits;
        }
    }
    for (const auto& entry : m_blendShapeConstraints) {
        stream << entry.constraintIndex << entry.sourceDigest << entry.parameterIndex << entry.parameterId
               << entry.parameterMinimum << entry.parameterMaximum << qint64(entry.values.size());
        for (const auto& value : entry.values) stream << value.key << value.weight;
    }
    state.insert(QStringLiteral("fingerprint"), QCryptographicHash::hash(exact, QCryptographicHash::Sha256));
    return state;
}

QVariantMap CModelDocument::DeformerStructureState() const
{
    return {{"enabled", m_hasDeformerStructure},
            {"sourceIds", QVariant::fromValue(m_hasDeformerStructure ? m_sourceDeformerIds : QSet<QString>{})}};
}

bool CModelDocument::PrepareParameterStructure(const Builder::ColumnState& structure, CModelDocument& candidate, QString* error) const
{
    return PrepareStructure(structure, false, candidate, error);
}

bool CModelDocument::PrepareStructure(const Builder::ColumnState& structure, bool meshStructure,
                                     CModelDocument& candidate, QString* error) const
{
    return PrepareStructure(structure, meshStructure, m_hasDeformerStructure,
                            m_sourceDeformerIds, false, m_hasMaterialStructure || !m_drawableMaterialOverrides.isEmpty(), candidate, error);
}

bool CModelDocument::PrepareStructure(const Builder::ColumnState& structure, bool meshStructure,
                                     bool deformerStructure, const QSet<QString>& sourceIds, bool changeDirectory, bool materialStructure,
                                     CModelDocument& candidate, QString* error) const
{
    if (!m_loaded) return fail(error, QStringLiteral("尚未加载模型"));
    const QByteArray& authoringBase = m_artMeshClones.isEmpty() ? m_sourceBytes : m_workingBaseBytes;
    QByteArray working;
    if (!Builder::BuildColumnState(authoringBase, structure, working, error)) return false;
    if (!m_blendShapeConstraints.isEmpty() && working != m_workingBaseBytes)
        return fail(error, QStringLiteral("存在融合限制曲线编辑，结构变化前必须先提交或清除曲线状态"));
    if (!m_blendShapeGeometry.isEmpty() && working != m_workingBaseBytes)
        return fail(error, QStringLiteral("存在融合几何编辑，结构变化前必须先提交或清除融合几何状态"));
    {
        auto sourceReader = CReader::FromBytes(authoringBase), workingReader = CReader::FromBytes(working);
        CSectionTable sourceTable(sourceReader), workingTable(workingReader);
        if (!sourceTable.IsValid() || !workingTable.IsValid()) return fail(error, sourceReader.Error() + workingReader.Error());
        if (!validWarpStructure(workingTable, error)) return false;
        if (!auxiliaryIdentity(authoringBase, sourceTable, working, workingTable, meshStructure, error)) return false;
    }
    CModelDocument original, next;
    if (!original.LoadBytes(authoringBase, m_sourcePath, error) || !next.LoadBytes(working, m_sourcePath, error)) return false;
    next.m_blendShapeGeometry = m_blendShapeGeometry;
    next.m_blendShapeConstraints = m_blendShapeConstraints;
    next.m_maskSource = m_maskSource;
    if (!next.ValidateStructureMasks(error)) return false;
    if (!next.m_previewBuilder || next.m_parts.size() != original.m_parts.size() ||
        (!deformerStructure && next.m_deformers.size() != original.m_deformers.size()) || next.m_drawables.size() != original.m_drawables.size() ||
        next.m_canvasPixels != original.m_canvasPixels)
        return fail(error, QStringLiteral("参数结构不能改变模型对象目录或画布"));
    const auto& directory = m_hasDeformerStructure ? *this : original;
    if (!changeDirectory && next.m_deformers.size() != directory.m_deformers.size())
        return fail(error, QStringLiteral("参数或网格操作不能增删变形器"));
    QVector<int> partParents;
    for (int index = 0; index < original.m_parts.size(); ++index) {
        const auto& before = original.m_parts[index]; const auto& after = next.m_parts[index];
        if (before.id != after.id || before.parentIndex != after.parentIndex || before.visible != after.visible || before.enabled != after.enabled)
            return fail(error, QStringLiteral("参数结构改变了 Part 定义：%1").arg(before.id));
        partParents.append(after.parentIndex);
    }
    if (!CParentGraph(partParents).IsValid()) return fail(error, QStringLiteral("参数结构包含无效 Part 父引用"));
    QVector<int> parents;
    QHash<QString, int> sourceIndices, nextIndices;
    for (int index = 0; index < original.m_deformers.size(); ++index) sourceIndices.insert(original.m_deformers[index].id, index);
    for (int index = 0; index < next.m_deformers.size(); ++index) {
        const auto& after = next.m_deformers[index];
        if (after.id.isEmpty() || nextIndices.contains(after.id) || after.parentPartIndex < -1 || after.parentPartIndex >= next.m_parts.size())
            return fail(error, QStringLiteral("变形器标识或所属部件无效：%1").arg(after.id));
        nextIndices.insert(after.id, index);
        if (!changeDirectory) {
            const auto& before = directory.m_deformers[index];
            if (before.id != after.id || before.type != after.type || before.parentPartIndex != after.parentPartIndex ||
            before.rows != after.rows || before.columns != after.columns || before.baseAngle != after.baseAngle ||
            before.quadTransform != after.quadTransform || before.visible != after.visible || before.enabled != after.enabled)
                return fail(error, QStringLiteral("参数结构改变了变形器定义：%1").arg(before.id));
            if (after.parentDeformerIndex >= 0 && after.parentDeformerIndex != before.parentDeformerIndex &&
                after.parentDeformerIndex != SourceDeformerParentIndex(after.id) &&
            (after.parentDeformerIndex >= next.m_deformers.size() ||
             next.m_deformers[after.parentDeformerIndex].parentPartIndex != after.parentPartIndex))
                return fail(error, QStringLiteral("参数结构新增了跨 Part 的父变形器关系：%1").arg(before.id));
        }
        if (deformerStructure) {
            if (!std::isfinite(after.baseAngle)) return fail(error, QStringLiteral("变形器基础角度无效：%1").arg(after.id));
            if (after.type == CModelDeformer::Warp) {
                if (after.rows < 1 || after.columns < 1 || after.rows >= 1000000 || after.columns >= 1000000)
                    return fail(error, QStringLiteral("Warp 网格行列数无效：%1").arg(after.id));
                const qint64 points = (qint64(after.rows) + 1) * (qint64(after.columns) + 1) * 2;
                if (points > 1000000 || after.warpKeyforms.isEmpty())
                    return fail(error, QStringLiteral("Warp 网格或关键形无效：%1").arg(after.id));
                for (const auto& form : after.warpKeyforms)
                    if (form.positions.size() != points || !std::isfinite(form.opacity) || form.opacity < 0 || form.opacity > 1 ||
                        !std::all_of(form.positions.cbegin(), form.positions.cend(), [](float value) { return std::isfinite(value); }))
                        return fail(error, QStringLiteral("Warp 关键形数据无效：%1").arg(after.id));
            } else {
                if (after.rotationKeyforms.isEmpty() || !std::all_of(after.rotationKeyforms.cbegin(), after.rotationKeyforms.cend(),
                    [](const auto& form) { return form.IsValid(); }))
                    return fail(error, QStringLiteral("Rotation 关键形数据无效：%1").arg(after.id));
            }
        }
        parents.append(after.parentDeformerIndex);
    }
    if (deformerStructure) for (const auto& id : sourceIds) {
        if (!sourceIndices.contains(id) || !nextIndices.contains(id))
            return fail(error, QStringLiteral("变形器源身份不存在：%1").arg(id));
        const auto& before = original.m_deformers[sourceIndices.value(id)];
        const auto& after = next.m_deformers[nextIndices.value(id)];
        if (before.type != after.type || before.parentPartIndex != after.parentPartIndex)
            return fail(error, QStringLiteral("变形器源身份的类型或部件已改变：%1").arg(id));
    }
    if (!CParentGraph(parents).IsValid()) return fail(error, QStringLiteral("参数结构包含无效父变形器关系"));
    CMocMeshStructureBuilder::Request geometryValidation;
    qint64 geometryVertices = 0;
    for (int index = 0; index < original.m_drawables.size(); ++index) {
        const auto& before = original.m_drawables[index]; const auto& after = next.m_drawables[index];
        if (before.id != after.id || before.parentPartIndex != after.parentPartIndex ||
            (!changeDirectory && directory.m_drawables[index].parentDeformerIndex != after.parentDeformerIndex) ||
            (!materialStructure && before.textureIndex != after.textureIndex) || (materialStructure && after.textureIndex < 0) || (before.flags & ~8) != (after.flags & ~8) ||
            before.visible != after.visible || before.enabled != after.enabled)
            return fail(error, QStringLiteral("参数结构改变了 ArtMesh 拓扑或定义：%1").arg(before.id));
        if (after.parentDeformerIndex < -1 || after.parentDeformerIndex >= next.m_deformers.size())
            return fail(error, QStringLiteral("ArtMesh 父变形器引用无效：%1").arg(after.id));
        if (before.vertexCount != after.vertexCount || before.uvs != after.uvs || before.indices != after.indices) {
            if (!meshStructure && (!materialStructure || before.vertexCount != after.vertexCount || before.indices != after.indices))
                return fail(error, QStringLiteral("参数结构改变了 ArtMesh 拓扑或定义：%1").arg(before.id));
            if (after.vertexCount <= 0 || after.vertexCount > CMocMeshStructureBuilder::MaximumVertices)
                return fail(error, QStringLiteral("网格结构顶点数量无效：%1").arg(before.id));
            if (after.vertexCount > 1000000 - geometryVertices || after.uvs.size() != after.vertexCount * 2 ||
                !std::all_of(after.uvs.cbegin(), after.uvs.cend(), [](float value) { return std::isfinite(value); }))
                return fail(error, QStringLiteral("网格 UV 无效或检查总量超过限制：%1").arg(before.id));
            geometryVertices += after.vertexCount;
            CMocMeshStructureBuilder::MeshEdit identity;
            identity.artMeshId = after.id;
            identity.indices = after.indices;
            for (int vertex = 0; vertex < after.vertexCount; ++vertex) {
                identity.vertices.append({{vertex}, {1.0}});
                identity.oldToNew.append(vertex);
            }
            geometryValidation.meshes.append(std::move(identity));
        }
    }
    if (!geometryValidation.meshes.isEmpty()) {
        CMocMeshStructureBuilder::Impact impact;
        if (!CMocMeshStructureBuilder::Analyze(working, geometryValidation, impact, error)) return false;
        if (!impact.blockingReasons.isEmpty()) return fail(error, impact.blockingReasons.join(QLatin1Char('\n')));
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
        const bool existed = type == CKeyformBindingGrid::ArtMesh ||
            (deformerStructure ? sourceIds.contains(id) : sourceIndices.contains(id));
        return (existed && !original.m_keyformBindings->Diagnostic(type, id).isEmpty()) || next.m_keyformBindings->Diagnostic(type, id).isEmpty();
    };
    for (const auto& mesh : next.m_drawables)
        if (!validateGrid(CKeyformBindingGrid::ArtMesh, mesh.id)) return fail(error, QStringLiteral("参数结构破坏了 ArtMesh 绑定：%1").arg(mesh.id));
    for (const auto& deformer : next.m_deformers)
        if (!validateGrid(deformer.type == CModelDeformer::Warp ? CKeyformBindingGrid::Warp : CKeyformBindingGrid::Rotation, deformer.id))
            return fail(error, QStringLiteral("参数结构破坏了变形器绑定：%1").arg(deformer.id));
    next.m_structure = structure;
    next.m_hasMeshStructure = meshStructure;
    next.m_hasMaterialStructure = materialStructure;
    next.m_hasDeformerStructure = deformerStructure;
    next.m_sourceDeformerIds = deformerStructure ? sourceIds : original.m_sourceDeformerIds;
    next.m_sourceDeformerParents.clear();
    for (const auto& node : next.m_deformers) {
        int parent = -1;
        if (next.m_sourceDeformerIds.contains(node.id) && sourceIndices.contains(node.id)) {
            const int oldParent = original.m_deformers[sourceIndices.value(node.id)].parentDeformerIndex;
            if (oldParent >= 0 && oldParent < original.m_deformers.size() &&
                next.m_sourceDeformerIds.contains(original.m_deformers[oldParent].id))
                parent = nextIndices.value(original.m_deformers[oldParent].id, -1);
        }
        next.m_sourceDeformerParents.insert(node.id, parent);
    }
    auto preview = std::make_shared<CMocPreviewBuilder>();
    if (!preview->Initialize(working, next.m_sourceDeformerParents, error)) return false;
    next.m_previewBuilder = std::move(preview);
    next.m_previewError.clear();
    next.m_sourceBytes = m_sourceBytes;
    next.m_sourcePath = m_sourcePath;
    next.m_sourceDigest = m_sourceDigest;
    next.m_workingBaseBytes = std::move(working);
    next.m_textureSources = m_textureSources;
    next.m_textureWorkspace = m_textureWorkspace;
    next.m_textureWorkspaceFingerprint = m_textureWorkspaceFingerprint;
    next.m_artwork = m_artwork;
    next.m_artworkFingerprint = m_artworkFingerprint;
    CModelDisplayNames::State names;
    const auto retain = [](const auto& objects, const auto& before, auto& after) {
        for (const auto& object : objects) if (before.contains(object.id)) after.insert(object.id, before.value(object.id));
    };
    retain(next.m_parts, m_displayNames.parts, names.parts);
    retain(next.m_drawables, m_displayNames.artMeshes, names.artMeshes);
    retain(next.m_deformers, m_displayNames.deformers, names.deformers);
    if (!next.SetDisplayNames(names, error)) return false;
    QStringList beforeIds, afterIds;
    for (const auto& parameter : m_parameters) beforeIds.append(parameter.id);
    for (const auto& parameter : next.m_parameters) afterIds.append(parameter.id);
    if (!CParameterOrganization::Reconcile(m_parameterOrganization, beforeIds, afterIds, next.m_parameterOrganization, error)) return false;
    if (!ReconcileGeometryObjectPairs(next, {}, nullptr, nullptr, error)) return false;
    candidate = std::move(next);
    return true;
}

bool CModelDocument::PrepareCloneStructure(const Builder::ColumnState& structure,
                                           const CMocMeshStructureBuilder::CloneRequest& request,
                                           CModelDocument& candidate, QString* error) const
{
    if (!m_loaded) return fail(error, QStringLiteral("尚未加载模型"));
    QByteArray working;
    if (!Builder::BuildColumnState(m_sourceBytes, structure, working, error)) return false;
    CModelDocument next;
    if (!next.LoadBytes(working, m_sourcePath, error)) return false;
    if (next.m_parts.size() != m_parts.size() || next.m_deformers.size() != m_deformers.size() ||
        next.m_drawables.size() != m_drawables.size() + 1 || next.m_drawOrderGroups.size() != m_drawOrderGroups.size() ||
        next.m_drawables.last().id != request.newArtMeshId)
        return fail(error, QStringLiteral("ArtMesh 克隆改变了非目标对象目录"));
    for (int index = 0; index < m_parts.size(); ++index) {
        const auto& before = m_parts[index];
        const auto& after = next.m_parts[index];
        if (before.id != after.id || before.parentIndex != after.parentIndex || before.visible != after.visible || before.enabled != after.enabled)
            return fail(error, QStringLiteral("ArtMesh 克隆改变了 Part 目录"));
    }
    for (int index = 0; index < m_deformers.size(); ++index) {
        const auto& before = m_deformers[index];
        const auto& after = next.m_deformers[index];
        if (before.id != after.id || before.type != after.type || before.parentPartIndex != after.parentPartIndex ||
            before.parentDeformerIndex != after.parentDeformerIndex || before.rows != after.rows || before.columns != after.columns)
            return fail(error, QStringLiteral("ArtMesh 克隆改变了变形器目录"));
    }
    for (int index = 0; index < m_drawables.size(); ++index) {
        const auto& before = m_drawables[index];
        const auto& after = next.m_drawables[index];
        if (before.id != after.id || before.parentPartIndex != after.parentPartIndex ||
            before.parentDeformerIndex != after.parentDeformerIndex || before.textureIndex != after.textureIndex ||
            before.flags != after.flags || before.vertexCount != after.vertexCount || before.uvs != after.uvs ||
            before.indices != after.indices || before.maskIndices != after.maskIndices)
            return fail(error, QStringLiteral("ArtMesh 克隆改变了源网格：%1").arg(before.id));
    }
    int sourceGroup = -1;
    int sourceIndex = -1;
    for (int index = 0; index < m_drawables.size(); ++index)
        if (m_drawables[index].id == request.sourceArtMeshId) sourceIndex = index;
    if (sourceIndex < 0) return fail(error, QStringLiteral("源 ArtMesh 不存在：%1").arg(request.sourceArtMeshId));
    for (int group = 0; group < m_drawOrderGroups.size(); ++group)
        for (const auto& object : m_drawOrderGroups[group].objects)
            if (object.type == 0 && object.index == sourceIndex)
                sourceGroup = sourceGroup < 0 ? group : -2;
    if (sourceGroup < 0) return fail(error, QStringLiteral("源 ArtMesh 绘制组不存在：%1").arg(request.sourceArtMeshId));
    QSet<int> changedGroups{sourceGroup};
    for (int child = sourceGroup; child >= 0;) {
        int parent = -1;
        for (int group = 0; group < m_drawOrderGroups.size(); ++group)
            for (const auto& object : m_drawOrderGroups[group].objects)
                if (object.type == 1 && object.selfIndex == child)
                    parent = parent < 0 ? group : -2;
        if (parent < 0) break;
        changedGroups.insert(parent);
        child = parent;
    }
    for (int index = 0; index < m_drawOrderGroups.size(); ++index) {
        const auto& before = m_drawOrderGroups[index];
        const auto& after = next.m_drawOrderGroups[index];
        const bool source = index == sourceGroup;
        const bool totalChanged = changedGroups.contains(index);
        if (before.minimumDrawOrder != after.minimumDrawOrder || before.maximumDrawOrder != after.maximumDrawOrder ||
            after.objectCount != before.objectCount + (source ? 1 : 0) ||
            after.totalObjectCount != before.totalObjectCount + (totalChanged ? 1 : 0) ||
            before.objectsValid != after.objectsValid || after.objects.size() != before.objects.size() + (source ? 1 : 0))
            return fail(error, QStringLiteral("ArtMesh 克隆改变了已有绘制组"));
        for (int object = 0; object < before.objects.size(); ++object)
            if (before.objects[object].type != after.objects[object].type || before.objects[object].index != after.objects[object].index ||
                before.objects[object].selfIndex != after.objects[object].selfIndex)
                return fail(error, QStringLiteral("ArtMesh 克隆改变了已有绘制组成员"));
        if (source && (after.objects.last().type != 0 || after.objects.last().index != next.m_drawables.size() - 1))
            return fail(error, QStringLiteral("ArtMesh 克隆未追加到源绘制组"));
    }
    next.m_maskSource = m_maskSource;
    if (!next.ValidateStructureMasks(error)) return false;
    next.m_sourceBytes = m_sourceBytes;
    next.m_sourcePath = m_sourcePath;
    next.m_sourceDigest = m_sourceDigest;
    next.m_workingBaseBytes = working;
    next.m_textureSources = m_textureSources;
    next.m_textureWorkspace = m_textureWorkspace;
    next.m_textureWorkspaceFingerprint = m_textureWorkspaceFingerprint;
    next.m_artwork = m_artwork;
    next.m_artworkFingerprint = m_artworkFingerprint;
    next.m_structure = structure;
    next.m_hasMeshStructure = true;
    next.m_hasMaterialStructure = m_hasMaterialStructure;
    next.m_hasDeformerStructure = m_hasDeformerStructure;
    next.m_sourceDeformerIds = m_sourceDeformerIds;
    next.m_sourceDeformerParents = m_sourceDeformerParents;
    next.m_parameterOrganization = m_parameterOrganization;
    next.m_artMeshClones = m_artMeshClones;
    next.m_artMeshClones.append(request);
    auto preview = std::make_shared<CMocPreviewBuilder>();
    if (!preview->Initialize(working, next.m_sourceDeformerParents, error)) return false;
    next.m_previewBuilder = std::move(preview);
    next.m_previewError.clear();
    if (!next.SetDisplayNames(m_displayNames, error) || !PreserveStructureProperties(next, error)) return false;
    candidate = std::move(next);
    return true;
}

bool CModelDocument::SetSessionAuthoringStructure(const Builder::ColumnState& structure,
                                                  const QVector<CMocMeshStructureBuilder::CloneRequest>& clones,
                                                  bool materialStructure, bool deformerStructure,
                                                  const QSet<QString>& sourceDeformerIds,
                                                  QString* error)
{
    if (clones.isEmpty()) return SetAuthoringStructure(structure, error);
    try {
        QByteArray replay = m_sourceBytes;
        for (const auto& request : clones) {
            CMocMeshStructureBuilder::Result result;
            if (!CMocMeshStructureBuilder::Clone(replay, request, result, error)) return false;
            replay = std::move(result.bytes);
        }
        QByteArray working;
        if (!Builder::BuildColumnState(replay, structure, working, error)) return false;
        CModelDocument replayed, next;
        if (!replayed.LoadBytes(replay, m_sourcePath, error) || !next.LoadBytes(working, m_sourcePath, error)) return false;
        if (next.m_parts.size() != replayed.m_parts.size() ||
            (!deformerStructure && next.m_deformers.size() != replayed.m_deformers.size()) ||
            next.m_drawables.size() != replayed.m_drawables.size() || next.m_drawOrderGroups.size() != replayed.m_drawOrderGroups.size()) {
            return fail(error, QStringLiteral("会话克隆目录与结构状态不匹配"));
        }
        for (int index = 0; index < replayed.m_drawables.size(); ++index)
            if (next.m_drawables[index].id != replayed.m_drawables[index].id)
                return fail(error, QStringLiteral("会话克隆对象顺序不匹配"));
        next.m_maskSource = m_maskSource;
        if (!next.ValidateStructureMasks(error)) return false;
        next.m_sourceBytes = m_sourceBytes;
        next.m_sourcePath = m_sourcePath;
        next.m_sourceDigest = m_sourceDigest;
        next.m_workingBaseBytes = working;
        next.m_textureSources = m_textureSources;
        next.m_textureWorkspace = m_textureWorkspace;
        next.m_textureWorkspaceFingerprint = m_textureWorkspaceFingerprint;
        next.m_artwork = m_artwork;
        next.m_artworkFingerprint = m_artworkFingerprint;
        next.m_structure = structure;
        next.m_hasMeshStructure = true;
        next.m_hasMaterialStructure = materialStructure;
        next.m_hasDeformerStructure = deformerStructure;
        next.m_sourceDeformerIds = deformerStructure ? sourceDeformerIds : m_sourceDeformerIds;
        next.m_sourceDeformerParents = m_sourceDeformerParents;
        next.m_parameterOrganization = m_parameterOrganization;
        next.m_artMeshClones = clones;
        auto preview = std::make_shared<CMocPreviewBuilder>();
        if (!preview->Initialize(working, next.m_sourceDeformerParents, error)) return false;
        next.m_previewBuilder = std::move(preview);
        next.m_previewError.clear();
        if (!next.SetDisplayNames(m_displayNames, error) || !PreserveStructureProperties(next, error)) return false;
        CommitCandidate(std::move(next));
        return true;
    } catch (const std::exception&) {
        return fail(error, QStringLiteral("无法恢复会话网格克隆结构"));
    }
}

bool CModelDocument::SetMaterialStructure(const Builder::ColumnState& structure, bool meshStructure,
                                         bool deformerStructure, const QSet<QString>& sourceIds, QString* error)
{
    try {
        if (!deformerStructure && !sourceIds.isEmpty()) return fail(error, QStringLiteral("变形器源身份需要变形器作者结构"));
        CModelDocument candidate;
        if (!PrepareStructure(structure, meshStructure, deformerStructure, sourceIds, deformerStructure, true, candidate, error)) return false;
        if (!candidate.ValidateArtworkReferences(candidate.m_artwork, error)) return false;
        CommitCandidate(std::move(candidate));
        return true;
    } catch (const std::exception&) { return fail(error, QStringLiteral("无法恢复材质作者结构")); }
}

bool CModelDocument::SetParameterStructure(const Builder::ColumnState& structure, QString* error)
{
    try {
        CModelDocument candidate;
        if (!PrepareParameterStructure(structure, candidate, error)) return false;
        if (!candidate.ValidateArtworkReferences(candidate.m_artwork, error)) return false;
        CommitCandidate(std::move(candidate));
        return true;
    } catch (const std::exception&) { return fail(error, QStringLiteral("无法恢复参数结构")); }
}

bool CModelDocument::SetAuthoringStructure(const Builder::ColumnState& structure, QString* error)
{
    try {
        CModelDocument candidate;
        if (!PrepareStructure(structure, true, candidate, error)) return false;
        if (!candidate.ValidateArtworkReferences(candidate.m_artwork, error)) return false;
        CommitCandidate(std::move(candidate));
        return true;
    } catch (const std::exception&) { return fail(error, QStringLiteral("无法应用网格编辑结构")); }
}

bool CModelDocument::SetDeformerStructure(const Builder::ColumnState& structure, bool meshStructure,
                                         const QSet<QString>& sourceIds, QString* error)
{
    try {
        CModelDocument candidate;
        if (!PrepareStructure(structure, meshStructure, true, sourceIds, true, m_hasMaterialStructure || !m_drawableMaterialOverrides.isEmpty(), candidate, error)) return false;
        if (!candidate.ValidateArtworkReferences(candidate.m_artwork, error)) return false;
        CommitCandidate(std::move(candidate));
        return true;
    } catch (const std::exception&) { return fail(error, QStringLiteral("无法恢复变形器结构")); }
}

bool CModelDocument::SameDeformerDirectory(const CModelDocument& candidate, QString* error) const
{
    if (DeformerStructureState() != candidate.DeformerStructureState() || m_deformers.size() != candidate.m_deformers.size() ||
        m_drawables.size() != candidate.m_drawables.size())
        return fail(error, QStringLiteral("当前操作不能改变变形器目录或源身份"));
    for (int index = 0; index < m_deformers.size(); ++index) {
        const auto& before = m_deformers[index]; const auto& after = candidate.m_deformers[index];
        if (before.id != after.id || before.type != after.type || before.parentPartIndex != after.parentPartIndex ||
            before.rows != after.rows || before.columns != after.columns || before.baseAngle != after.baseAngle ||
            before.quadTransform != after.quadTransform || before.visible != after.visible || before.enabled != after.enabled ||
            DeformerParentIndex(before.id) != candidate.DeformerParentIndex(after.id))
            return fail(error, QStringLiteral("当前操作不能改变变形器定义或父级：%1").arg(before.id));
    }
    for (int index = 0; index < m_drawables.size(); ++index)
        if (m_drawables[index].id != candidate.m_drawables[index].id ||
            m_drawables[index].parentDeformerIndex != candidate.m_drawables[index].parentDeformerIndex)
            return fail(error, QStringLiteral("当前操作不能改变网格父级：%1").arg(m_drawables[index].id));
    return true;
}

bool CModelDocument::DecodeStructureState(const QVariantMap& state, const CModelEdit& edit, bool forward,
                                           CModelSession& session, QString* error) const
{
    if (!HasValidStructureRequests(edit)) return fail(error, QStringLiteral("绑定关键值请求与结构命令不匹配"));
    const auto current = ParameterStructureState();
    if (state.keys() != current.keys() || state.value("sourceDigest") != m_sourceDigest || state.value("sourcePath") != m_sourcePath)
        return fail(error, QStringLiteral("参数结构状态来源或字段不匹配"));
    QHash<QString, float> parameters;
    QSet<QString> locks;
    CModelDisplayNames::State names;
    if (!read(state, "meshStructure", session.meshStructure) ||
        !read(state, "deformerStructure", session.deformerStructure) ||
        !read(state, "materialStructure", session.materialStructure) || !read(state, "materials", session.drawableMaterials) ||
        !read(state, "sourceDeformerIds", session.sourceDeformerIds) ||
        !read(state, "columns", session.structure.columns) || !read(state, "counts", session.structure.counts) ||
        !read(state, "parameters", parameters) || !read(state, "parameterMetadata", session.parameterMetadata) ||
        !read(state, "deformerDisplayNames", names.deformers) ||
        !read(state, "partDisplayNames", names.parts) || !read(state, "artMeshDisplayNames", names.artMeshes) ||
        !read(state, "parameterOrganization", session.parameterOrganization) ||
        !read(state, "geometryObjectPairs", session.geometryObjectPairs) ||
        !read(state, "visibility", session.visibility) || !read(state, "locked", locks) ||
        !read(state, "opacity", session.drawableOpacity) || !read(state, "drawOrder", session.drawableDrawOrder) ||
        !read(state, "positions", session.drawableKeyformPositions) || !read(state, "indices", session.drawableIndices) ||
        !read(state, "masks", session.drawableMaskIndices) || !read(state, "parents", session.deformerParentIndices) ||
        !read(state, "rotation", session.deformerRotationKeyforms) || !read(state, "warp", session.deformerWarpKeyforms) ||
        !read(state, "inverted", session.drawableMaskInverted) ||
        !read(state, "blendShapeGeometry", session.blendShapeGeometry) ||
        !read(state, "blendShapeConstraints", session.blendShapeConstraints))
        return fail(error, QStringLiteral("参数结构状态类型无效"));
    session.parameters = ordered(parameters);
    session.objectDisplayNames = std::move(names);
    for (const auto& lock : locks) session.locked.insert(lock, true);
    session.sourceDigest = m_sourceDigest;
    session.sourceMoc = m_sourcePath;
    CTextureWorkspace::State workspace;
    if (!read(state, "textureWorkspace", workspace)) return fail(error, QStringLiteral("图集结构状态类型无效"));
    if (workspace != m_textureWorkspace) return fail(error, QStringLiteral("模型结构操作不能改变图集工作区"));
    if (!read(state, "artwork", session.artwork)) return fail(error, QStringLiteral("模型素材关联状态类型无效"));
    if (edit.field != CModelEdit::MeshStructure && session.artwork != m_artwork)
        return fail(error, QStringLiteral("当前结构操作不能改变素材关联"));
    if (HasTextureWorkspace()) {
        CTextureWorkspace::Draft draft;
        if (!CTextureWorkspace::CaptureDraft(m_textureSources, workspace, draft, error)) return false;
        session.textureWorkspace = std::move(draft);
    }
    session.version = session.materialStructure || !session.drawableMaterials.isEmpty() ? 10 : !session.geometryObjectPairs.sourceDigest.isEmpty() ? 9 : session.parameterOrganization.configured ? 8 : session.deformerStructure ? 6 : session.meshStructure ? 5 : session.structure.columns.isEmpty() && session.structure.counts.isEmpty() ? 1 : 2;
    if (session.textureWorkspace) session.version = 11;
    if (!session.artwork.IsEmpty()) session.version = 12;
    session.version = session.blendShapeConstraints.isEmpty() ? 16 : 19;
    if (!m_artMeshClones.isEmpty() && !edit.cloneRequest) {
        session.artMeshClones = m_artMeshClones;
        session.version = std::max(session.version, 18);
    }
    if (edit.cloneRequest) {
        auto clones = m_artMeshClones;
        if (forward) {
            clones.append(*edit.cloneRequest);
        } else if (!clones.isEmpty() && clones.last().newArtMeshId == edit.cloneRequest->newArtMeshId) {
            clones.removeLast();
        } else {
            return fail(error, QStringLiteral("ArtMesh 克隆历史顺序无效"));
        }
        session.artMeshClones = std::move(clones);
        session.version = std::max(session.version, 18);
    }
    return true;
}

bool CModelDocument::ApplyStructureState(const QVariantMap& state, const CModelEdit& edit, bool forward, QString* error)
{
    try {
        CModelSession session;
        if (!DecodeStructureState(state, edit, forward, session, error)) return false;
        CModelDocument candidate;
        if (!session.RestoreDocument(*this, candidate, error)) return false;
        if (candidate.ParameterStructureState() != state) return fail(error, QStringLiteral("参数结构状态内容无效或发生归一化"));
        if (!edit.cloneRequest && edit.field != CModelEdit::DeformerStructure && candidate.m_displayNames != m_displayNames)
            return fail(error, QStringLiteral("参数或网格结构编辑不能附带对象名称修改"));
        if (edit.field == CModelEdit::DeformerStructure) {
            if (!ValidateDeformerTransition(candidate, edit, forward, error)) return false;
        } else if (edit.cloneRequest) {
            if (!ValidateCloneTransition(candidate, edit, forward, error)) return false;
        } else if (!SameDeformerDirectory(candidate, error)) return false;
        else if (edit.field == CModelEdit::MeshStructure) {
            if (!ValidateMeshTransition(candidate, edit, forward, error)) return false;
        } else {
            if (candidate.m_hasMeshStructure != m_hasMeshStructure || !edit.meshRemaps.isEmpty())
                return fail(error, QStringLiteral("参数编辑不能改变网格结构模式"));
            CModelMaterialState beforeMaterials, afterMaterials;
            if (!CaptureDrawableMaterials(beforeMaterials, error) || !candidate.CaptureDrawableMaterials(afterMaterials, error) || beforeMaterials != afterMaterials)
                return fail(error, QStringLiteral("参数编辑不能改变网格材质"));
            for (int index = 0; index < m_drawables.size(); ++index) {
                const auto& before = m_drawables[index]; const auto& after = candidate.m_drawables[index];
                if (before.vertexCount != after.vertexCount ||
                    DrawableIndices(before.id) != candidate.DrawableIndices(after.id))
                    return fail(error, QStringLiteral("参数编辑不能改变网格几何：%1").arg(before.id));
            }
            auto beforeReader = CReader::FromBytes(m_workingBaseBytes), afterReader = CReader::FromBytes(candidate.m_workingBaseBytes);
            CSectionTable beforeTable(beforeReader), afterTable(afterReader);
            if (!beforeTable.IsValid() || !afterTable.IsValid() ||
                !auxiliaryIdentity(m_workingBaseBytes, beforeTable, candidate.m_workingBaseBytes, afterTable, false, error)) return false;
            if (edit.parameterChange == CModelEdit::ParameterChange::BindingKeys &&
                !ValidateBindingKeysTransition(candidate, edit, forward, error)) return false;
        }
        QVector<Builder::BindingRequest> changed;
        const auto examine = [&](Builder::Owner::Type type, const QString& id) {
            const auto gridType = static_cast<CKeyformBindingGrid::ObjectType>(type);
            if (!sameAxes(m_keyformBindings->Axes(gridType, id), candidate.m_keyformBindings->Axes(gridType, id)))
                changed.append(Builder::BindingRequest{{type, id}, {}});
        };
        for (const auto& mesh : m_drawables) examine(Builder::Owner::ArtMesh, mesh.id);
        if (edit.field != CModelEdit::DeformerStructure) for (const auto& deformer : m_deformers)
            examine(deformer.type == CModelDeformer::Warp ? Builder::Owner::Warp : Builder::Owner::Rotation, deformer.id);
        if (!canBind(*this, changed, error)) return false;
        if (edit.field == CModelEdit::ParameterStructure && !edit.bindingKeysRequest && !edit.bindingKeysBatchRequest) {
            const auto& origin = forward ? *this : candidate;
            const auto& destination = forward ? candidate : *this;
            auto paired = destination;
            const Builder::Result identities;
            if (!origin.ReconcileGeometryObjectPairs(paired, {}, &identities, nullptr, error) ||
                paired.m_geometryObjectPairs != destination.m_geometryObjectPairs)
                return fail(error, QStringLiteral("参数历史的几何配对身份与绑定变化不一致"));
        }
        candidate.m_editHistory = m_editHistory;
        candidate.m_editIndex = m_editIndex;
        CommitCandidate(std::move(candidate));
        return true;
    } catch (const std::exception&) { return fail(error, QStringLiteral("无法应用参数结构状态")); }
}

bool CModelDocument::MakeParameterStructureEdit(const Builder::Result& result, const QString& id,
                                               const QString* displayName, bool created, CModelEdit& edit, QString* error,
                                               const Builder::EditBindingKeysRequest* keys,
                                               const Builder::EditBindingKeysBatchRequest* batchKeys) const
{
    Builder::ColumnState structure;
    if (!Builder::CaptureColumnState(m_sourceBytes, result.bytes, structure, error)) return false;
    CModelDocument candidate;
    if (!PrepareStructure(structure, m_hasMeshStructure, candidate, error) || !PreserveStructureProperties(candidate, error)) return false;
    if (!ReconcileGeometryObjectPairs(candidate, {}, &result, keys, error, batchKeys)) return false;
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

bool CModelDocument::PreserveStructureProperties(CModelDocument& candidate, QString* error) const
{
    auto visibility = VisibilityState();
    const auto available = candidate.VisibilityState();
    for (auto it = visibility.begin(); it != visibility.end();)
        if (!available.contains(it.key())) it = visibility.erase(it); else ++it;
    if (!visibility.isEmpty() && !candidate.SetVisibilityValues(visibility)) return fail(error, QStringLiteral("无法保留对象可见性"));
    candidate.m_drawableOpacityOverrides = m_drawableOpacityOverrides;
    candidate.m_drawableDrawOrderOverrides = m_drawableDrawOrderOverrides;
    candidate.m_drawableIndexOverrides = m_drawableIndexOverrides;
    candidate.m_lockedKeys.clear();
    for (const auto& key : m_lockedKeys) if (available.contains(key)) candidate.m_lockedKeys.insert(key);
    for (const auto& parameter : candidate.m_parameters) {
        if (m_parameterValues.contains(parameter.id)) candidate.m_parameterValues.insert(parameter.id, m_parameterValues.value(parameter.id));
        const auto previous = m_parameterMetadataOverrides.constFind(parameter.id);
        if (previous != m_parameterMetadataOverrides.cend() && !previous->displayName.isEmpty() && previous->displayName != parameter.id) {
            auto metadata = candidate.ParameterMetadata(parameter.id);
            metadata.displayName = previous->displayName;
            candidate.m_parameterMetadataOverrides.insert(parameter.id, metadata);
        }
    }
    return true;
}

bool CModelDocument::MakeDeformerStructureEdit(const CDeformerPreserveRequest& request,
                                               CModelEdit& edit, QString* error) const
{
    if (error) error->clear();
    try {
        if (!m_loaded || request.sourceDigest != m_sourceDigest || request.expectedAuthoringDigest.size() != 32 || request.operation.valueless_by_exception())
            return fail(error, QStringLiteral("变形器编辑请求与源模型不匹配"));
        const auto frozen = ownedDeformerRequest(request);
        QByteArray base;
        if (!BuildPreviewMoc(base, error)) return false;
        CDeformerStructureReplay::Impact impact;
        if (!CDeformerStructureReplay::Analyze(base, frozen, impact, error)) return false;
        if (!impact.blockingReasons.isEmpty()) return fail(error, impact.blockingReasons.join(QLatin1Char('\n')));
        QVector<Builder::BindingRequest> affected;
        QSet<QPair<int, QString>> visited;
        const auto add = [&](const Builder::Owner& owner) {
            const QPair<int, QString> key{int(owner.type), owner.id};
            if (!visited.contains(key)) { visited.insert(key); affected.append({owner, {}}); }
        };
        for (const auto& owner : impact.structure.reparented) add(owner);
        for (const auto& owner : impact.coordinateOwners) add(owner);
        for (const auto& owner : impact.scalarOwners) add(owner);
        const auto addDeformer = [&](const QString& id) {
            if (id.isEmpty()) return true;
            for (const auto& node : m_deformers) if (node.id == id) {
                add({node.type == CModelDeformer::Warp ? Builder::Owner::Warp : Builder::Owner::Rotation, id});
                return true;
            }
            return false;
        };
        auto sourceIds = m_sourceDeformerIds;
        if (const auto* insertion = std::get_if<CMocDeformerStructureBuilder::InsertRequest>(&frozen.operation)) {
            if (!addDeformer(insertion->parentId)) return fail(error, QStringLiteral("新变形器的父级不存在"));
            QSet<int> parts;
            for (int part = insertion->partIndex; part != -1; part = m_parts[part].parentIndex) {
                if (part < 0 || part >= m_parts.size() || parts.contains(part) || IsLocked(QStringLiteral("part"), m_parts[part].id))
                    return fail(error, QStringLiteral("新变形器的所属部件已锁定或父引用无效"));
                parts.insert(part);
            }
            sourceIds.remove(insertion->id);
        } else if (const auto* removal = std::get_if<CMocDeformerStructureBuilder::RemoveRequest>(&frozen.operation)) {
            if (!addDeformer(removal->id)) return fail(error, QStringLiteral("要删除的变形器不存在"));
            sourceIds.remove(removal->id);
        } else {
            const auto& resize = std::get<CMocDeformerStructureBuilder::ResizeWarpRequest>(frozen.operation);
            if (!addDeformer(resize.id)) return fail(error, QStringLiteral("要调整网格的变形器不存在"));
        }
        if (!canBind(*this, affected, error)) return false;
        CMocDeformerStructureBuilder::Result result;
        if (!CDeformerStructureReplay::Build(base, frozen, result, error)) return false;
        if (impact.structure.warpGrid && impact.structure.warpGrid->oldRows == impact.structure.warpGrid->newRows &&
            impact.structure.warpGrid->oldColumns == impact.structure.warpGrid->newColumns) {
            CModelEdit command{impact.structure.id, QStringLiteral("deformer"), CModelEdit::DeformerStructure,
                               AuthoringStructureState(), AuthoringStructureState(), -1, false};
            command.deformerRequest = std::make_shared<const CDeformerStructureRequest>(frozen);
            edit = std::move(command);
            return true;
        }
        Builder::ColumnState structure;
        const QByteArray& authoringBase = m_artMeshClones.isEmpty() ? m_sourceBytes : m_workingBaseBytes;
        if (!Builder::CaptureColumnState(authoringBase, result.bytes, structure, error)) return false;
        CModelDocument candidate;
        if (!PrepareStructure(structure, m_hasMeshStructure, true, sourceIds, true,
                              m_hasMaterialStructure || !m_drawableMaterialOverrides.isEmpty(), candidate, error) ||
            !PreserveStructureProperties(candidate, error)) return false;
        if (!ReconcileGeometryObjectPairs(candidate, {}, nullptr, nullptr, error)) return false;
        CModelEdit command{impact.structure.id, QStringLiteral("deformer"), CModelEdit::DeformerStructure,
                           AuthoringStructureState(), candidate.AuthoringStructureState(), -1, false};
        command.deformerRequest = std::make_shared<const CDeformerStructureRequest>(frozen);
        command.runtime = std::make_shared<CModelEditRuntimeState>();
        edit = std::move(command);
        return true;
    } catch (const std::exception&) { return fail(error, QStringLiteral("无法准备变形器编辑，原模型保持不变")); }
}

bool CModelDocument::ValidateDeformerTransition(const CModelDocument& candidate, const CModelEdit& edit,
                                                bool forward, QString* error) const
{
    if (!HasValidStructureRequests(edit) || !edit.deformerRequest ||
        !(forward ? candidate.m_hasDeformerStructure : m_hasDeformerStructure))
        return fail(error, QStringLiteral("变形器结构命令缺少有效请求"));
    const auto& origin = forward ? *this : candidate;
    const auto& destination = forward ? candidate : *this;
    CModelEdit expected;
    if (!origin.MakeDeformerStructureEdit(*edit.deformerRequest, expected, error)) return false;
    if (expected.after.toMap() != destination.AuthoringStructureState())
        return fail(error, QStringLiteral("变形器编辑结果超出请求范围或已发生改变"));
    return true;
}

bool CModelDocument::ValidateCloneTransition(const CModelDocument& candidate, const CModelEdit& edit,
                                             bool forward, QString* error) const
{
    if (!edit.cloneRequest || edit.field != CModelEdit::MeshStructure || edit.meshRequest || !edit.meshRemaps.isEmpty())
        return fail(error, QStringLiteral("ArtMesh 克隆命令缺少有效请求"));
    const auto& request = *edit.cloneRequest;
    if (!forward) {
        if (m_drawables.size() != candidate.m_drawables.size() + 1 || m_drawables.last().id != request.newArtMeshId ||
            m_drawOrderGroups.size() != candidate.m_drawOrderGroups.size())
            return fail(error, QStringLiteral("ArtMesh 克隆撤销目录不匹配"));
        return true;
    }
    if (candidate.m_drawables.size() != m_drawables.size() + 1 || candidate.m_drawables.last().id != request.newArtMeshId ||
        candidate.m_drawOrderGroups.size() != m_drawOrderGroups.size())
        return fail(error, QStringLiteral("ArtMesh 克隆结果目录不匹配"));
    CModelDocument expected;
    if (!BuildCloneArtMeshCandidate(request, expected, error) ||
        expected.AuthoringStructureState() != candidate.AuthoringStructureState())
        return fail(error, QStringLiteral("ArtMesh 克隆历史包含请求之外的结构或覆盖修改"));
    return true;
}

bool CModelDocument::ValidateMeshTransition(const CModelDocument& candidate, const CModelEdit& edit,
                                           bool forward, QString* error) const
{
    if (candidate.m_displayNames != m_displayNames)
        return fail(error, QStringLiteral("网格结构编辑不能附带对象名称修改"));
    if (!edit.meshRequest || (edit.meshRemaps.isEmpty() && edit.meshRequest->glueWeights.isEmpty()) ||
        !(forward ? candidate.m_hasMeshStructure : m_hasMeshStructure) ||
        m_parameters.size() != candidate.m_parameters.size() || m_parameterValues != candidate.m_parameterValues ||
        VisibilityState() != candidate.VisibilityState() || m_lockedKeys != candidate.m_lockedKeys ||
        m_drawableOpacityOverrides != candidate.m_drawableOpacityOverrides ||
        m_drawableDrawOrderOverrides != candidate.m_drawableDrawOrderOverrides)
        return fail(error, QStringLiteral("网格结构不能改变参数、可见性、锁定或材质覆盖"));
    for (int index = 0; index < m_parameters.size(); ++index) {
        const auto& before = m_parameters[index]; const auto& after = candidate.m_parameters[index];
        if (before.id != after.id || before.type != after.type || before.keyValues != after.keyValues ||
            !(ParameterMetadata(before.id) == candidate.ParameterMetadata(after.id)))
            return fail(error, QStringLiteral("网格结构不能改变参数定义：%1").arg(before.id));
    }
    QSet<QString> affected;
    QHash<QString, int> meshIndices;
    for (int index = 0; index < m_drawables.size(); ++index) {
        const auto& id = m_drawables[index].id;
        if (id.isEmpty() || meshIndices.contains(id)) return fail(error, QStringLiteral("网格结构需要唯一的对象 ID"));
        meshIndices.insert(id, index);
    }
    QVector<Builder::BindingRequest> owners;
    const auto addOwner = [&](const QString& id) {
        if (affected.contains(id)) return;
        affected.insert(id);
        owners.append(Builder::BindingRequest{{Builder::Owner::ArtMesh, id}, {}});
    };
    for (const auto& remap : edit.meshRemaps) {
        if (affected.contains(remap.artMeshId)) return fail(error, QStringLiteral("顶点映射对象重复"));
        const int index = meshIndices.value(remap.artMeshId, -1);
        if (index < 0 || candidate.m_drawables[index].id != remap.artMeshId ||
            m_drawables[index].vertexCount != (forward ? remap.oldVertexCount : remap.newVertexCount) ||
            candidate.m_drawables[index].vertexCount != (forward ? remap.newVertexCount : remap.oldVertexCount) ||
            remap.oldToNew.size() != remap.oldVertexCount || remap.newToOld.size() != remap.newVertexCount)
            return fail(error, QStringLiteral("顶点映射与编辑两侧数量不匹配：%1").arg(remap.artMeshId));
        const auto valid = [](const QVector<int>& mapping, const QVector<int>& inverse) {
            for (int index = 0; index < mapping.size(); ++index)
                if (mapping[index] < -1 || mapping[index] >= inverse.size() ||
                    (mapping[index] >= 0 && inverse[mapping[index]] != index)) return false;
            return true;
        };
        if (!valid(remap.oldToNew, remap.newToOld) || !valid(remap.newToOld, remap.oldToNew))
            return fail(error, QStringLiteral("顶点映射不是可逆的存活点对应：%1").arg(remap.artMeshId));
        addOwner(remap.artMeshId);
    }
    CModelMaterialState beforeMaterials, afterMaterials;
    if (!CaptureDrawableMaterials(beforeMaterials, error) || !candidate.CaptureDrawableMaterials(afterMaterials, error)) return false;
    CModelMaterialOverrides beforeMaterialMap, afterMaterialMap;
    for (const auto& material : beforeMaterials) beforeMaterialMap.insert(material.id, material);
    for (const auto& material : afterMaterials) afterMaterialMap.insert(material.id, material);
    for (int index = 0; index < m_drawables.size(); ++index) {
        const auto& before = m_drawables[index]; const auto& after = candidate.m_drawables[index];
        if (!sameAxes(m_keyformBindings->Axes(CKeyformBindingGrid::ArtMesh, before.id),
                      candidate.m_keyformBindings->Axes(CKeyformBindingGrid::ArtMesh, after.id)) ||
            before.keyforms.size() != after.keyforms.size() || DrawableMaskIndices(before.id) != candidate.DrawableMaskIndices(after.id) ||
            DrawableMaskInverted(before.id) != candidate.DrawableMaskInverted(after.id))
            return fail(error, QStringLiteral("网格结构不能改变绑定或蒙版：%1").arg(before.id));
        if (!affected.contains(before.id) && beforeMaterialMap.value(before.id) != afterMaterialMap.value(after.id))
            return fail(error, QStringLiteral("网格结构改变了未映射对象的材质：%1").arg(before.id));
        if (!affected.contains(before.id) && (before.vertexCount != after.vertexCount ||
            DrawableIndices(before.id) != candidate.DrawableIndices(after.id)))
            return fail(error, QStringLiteral("网格结构改变了未映射的网格：%1").arg(before.id));
        for (int form = 0; form < before.keyforms.size(); ++form) {
            if (before.keyforms[form].opacity != after.keyforms[form].opacity || before.keyforms[form].drawOrder != after.keyforms[form].drawOrder ||
                (!affected.contains(before.id) && DrawableKeyformPositions(before.id, form) != candidate.DrawableKeyformPositions(after.id, form)))
                return fail(error, QStringLiteral("网格结构改变了无关关键形数据：%1").arg(before.id));
        }
    }
    for (const auto& deformer : m_deformers) {
        const auto type = deformer.type == CModelDeformer::Warp ? CKeyformBindingGrid::Warp : CKeyformBindingGrid::Rotation;
        if (!sameAxes(m_keyformBindings->Axes(type, deformer.id), candidate.m_keyformBindings->Axes(type, deformer.id)) ||
            DeformerParentIndex(deformer.id) != candidate.DeformerParentIndex(deformer.id) ||
            DeformerWarpKeyforms(deformer.id) != candidate.DeformerWarpKeyforms(deformer.id) ||
            DeformerRotationKeyforms(deformer.id) != candidate.DeformerRotationKeyforms(deformer.id))
            return fail(error, QStringLiteral("网格结构不能改变变形器：%1").arg(deformer.id));
    }
    auto beforeReader = CReader::FromBytes(m_workingBaseBytes), afterReader = CReader::FromBytes(candidate.m_workingBaseBytes);
    CSectionTable beforeTable(beforeReader), afterTable(afterReader);
    if (!beforeTable.IsValid() || !afterTable.IsValid()) return fail(error, QStringLiteral("无法验证网格结构的 Glue 数据"));
    qint64 glueReferences = 0;
    for (int index = 0; index < beforeTable.glue.size(); ++index) {
        const auto* before = beforeTable.glue[index]; const auto* after = afterTable.glue[index];
        if (before->art_mesh_indices_a < 0 || before->art_mesh_indices_b < 0 ||
            before->art_mesh_indices_a >= beforeTable.art_meshes.size() || before->art_mesh_indices_b >= beforeTable.art_meshes.size() ||
            !validSlice(before->glue_info_sources_begin_indices, before->glue_info_sources_counts, beforeTable.glue_info.size()) ||
            !validSlice(after->glue_info_sources_begin_indices, after->glue_info_sources_counts, afterTable.glue_info.size()) ||
            before->glue_info_sources_counts % 2 || after->glue_info_sources_counts % 2 ||
            before->glue_info_sources_counts > 1000000 - glueReferences)
            return fail(error, QStringLiteral("Glue 配对范围无效或超过检查总量"));
        glueReferences += before->glue_info_sources_counts;
        bool changed = before->glue_info_sources_counts != after->glue_info_sources_counts;
        for (int endpoint = 0; !changed && endpoint < before->glue_info_sources_counts; ++endpoint) {
            const auto* a = beforeTable.glue_info[before->glue_info_sources_begin_indices + endpoint];
            const auto* b = afterTable.glue_info[after->glue_info_sources_begin_indices + endpoint];
            changed = a->position_indices != b->position_indices || a->weights != b->weights;
        }
        if (!changed) continue;
        if (IsLocked(QStringLiteral("glue"), before->ids)) return fail(error, QStringLiteral("Glue 已锁定：%1").arg(before->ids));
        addOwner(beforeTable.art_meshes[before->art_mesh_indices_a]->ids);
        addOwner(beforeTable.art_meshes[before->art_mesh_indices_b]->ids);
    }
    if (!canBind(*this, owners, error)) return false;
    const auto& origin = forward ? *this : candidate;
    const auto& destination = forward ? candidate : *this;
    QByteArray base;
    CMocMeshStructureBuilder::Result replay;
    Builder::ColumnState expected;
    const QByteArray& expectedBase = origin.m_artMeshClones.isEmpty() ? origin.m_sourceBytes : origin.m_workingBaseBytes;
    if (!origin.BuildMeshBase(*edit.meshRequest, base, error) ||
        !CMocMeshStructureBuilder::Build(base, *edit.meshRequest, replay, error) ||
        !Builder::CaptureColumnState(expectedBase, replay.bytes, expected, error)) return false;
    if (!(expected == destination.m_structure) || replay.meshes.size() != edit.meshRemaps.size())
        return fail(error, QStringLiteral("网格历史包含请求之外的结构修改"));
    for (int index = 0; index < replay.meshes.size(); ++index) {
        const auto& actual = replay.meshes[index]; const auto& recorded = edit.meshRemaps[index];
        if (actual.artMeshId != recorded.artMeshId || actual.oldVertexCount != recorded.oldVertexCount ||
            actual.newVertexCount != recorded.newVertexCount || actual.oldToNew != recorded.oldToNew || actual.newToOld != recorded.newToOld)
            return fail(error, QStringLiteral("网格历史的顶点映射与请求不匹配"));
    }
    QVector<CGeometryObjectPairs::VertexRemap> pairRemaps;
    for (const auto& remap : replay.meshes) pairRemaps.append({{CModelGeometryKeyform::ArtMesh, remap.artMeshId}, remap.oldToNew});
    CModelDocument paired = destination;
    if (!origin.ReconcileGeometryObjectPairs(paired, pairRemaps, nullptr, nullptr, error) ||
        paired.m_geometryObjectPairs != destination.m_geometryObjectPairs)
        return fail(error, QStringLiteral("网格历史的几何配对身份与顶点映射不一致"));
    return true;
}

bool CModelDocument::ValidateBindingKeysTransition(const CModelDocument& candidate, const CModelEdit& edit,
                                                  bool forward, QString* error) const
{
    if (!HasValidStructureRequests(edit) || (!edit.bindingKeysRequest && !edit.bindingKeysBatchRequest) ||
        candidate.m_sourceBytes != m_sourceBytes || candidate.m_sourceDigest != m_sourceDigest ||
        candidate.m_sourcePath != m_sourcePath)
        return fail(error, QStringLiteral("绑定关键值历史的请求或模型来源无效"));
    const auto owner = edit.bindingKeysRequest ? edit.bindingKeysRequest->owner : edit.bindingKeysBatchRequest->owner;
    if (!canBind(*this, {{owner, {}}}, error)) return false;
    const auto& origin = forward ? *this : candidate;
    const auto& destination = forward ? candidate : *this;
    QByteArray base;
    Builder::Result result;
    CModelEdit expected;
    if (!origin.BuildPreviewMoc(base, error)) return false;
    if (edit.bindingKeysRequest) {
        if (!Builder::EditBindingKeys(base, *edit.bindingKeysRequest, result, error)) return false;
    } else if (!Builder::EditBindingKeysBatch(base, *edit.bindingKeysBatchRequest, result, error)) return false;
    if (result.bytes == base) return fail(error, QStringLiteral("绑定关键值请求没有改变模型"));
    if (!origin.MakeParameterStructureEdit(result, edit.name, nullptr, false, expected, error,
                                           edit.bindingKeysRequest.get(), edit.bindingKeysBatchRequest.get())) return false;
    if (expected.after.toMap() != destination.AuthoringStructureState())
        return fail(error, QStringLiteral("绑定关键值历史包含请求之外的结构或属性修改"));
    return true;
}

bool CModelDocument::BuildMeshBase(const CMocMeshStructureBuilder::Request& request, QByteArray& bytes, QString* error) const
{
    CMocRuntimeBuilder::Overrides indices;
    for (const auto& mesh : request.meshes) {
        const auto found = m_drawableIndexOverrides.constFind(mesh.artMeshId);
        if (found != m_drawableIndexOverrides.cend()) indices.drawableIndices.insert(mesh.artMeshId, *found);
    }
    QByteArray preview;
    return BuildPreviewMoc(preview, error) && CMocRuntimeBuilder::Build(preview, indices, bytes, error);
}

bool CModelDocument::MakeMeshStructureEdit(const CMocMeshStructureBuilder::Request& sourceRequest,
                                          CModelEdit& edit, QString* error) const
{
    try {
        CMocMeshStructureBuilder::Request request;
        if (!FreezeMeshRequest(sourceRequest, request, error)) return false;
        if (request.meshes.isEmpty() && request.glueWeights.isEmpty()) return fail(error, QStringLiteral("没有选定编辑网格或 Glue 端点"));
        QVector<Builder::BindingRequest> owners;
        for (const auto& mesh : request.meshes) {
            owners.append(Builder::BindingRequest{{Builder::Owner::ArtMesh, mesh.artMeshId}, {}});
        }
        if (!canBind(*this, owners, error)) return false;
        QByteArray base;
        if (!BuildMeshBase(request, base, error)) return false;
        CMocMeshStructureBuilder::Result result;
        if (!CMocMeshStructureBuilder::Build(base, request, result, error)) return false;
        bool remapped = false;
        for (const auto& mesh : result.meshes)
            for (int index = 0; index < mesh.oldToNew.size(); ++index) remapped = remapped || mesh.oldToNew[index] != index;
        if (result.bytes == base && !remapped) return fail(error, QStringLiteral("网格没有发生变化"));
        Builder::ColumnState structure;
        CModelDocument candidate;
        const QByteArray& structureBase = m_artMeshClones.isEmpty() ? m_sourceBytes : m_workingBaseBytes;
        if (!Builder::CaptureColumnState(structureBase, result.bytes, structure, error)) return false;
        if (!m_artMeshClones.isEmpty()) {
            candidate = *this;
            if (!candidate.SetSessionAuthoringStructure(structure, m_artMeshClones, m_hasMaterialStructure,
                                                        m_hasDeformerStructure, m_sourceDeformerIds, error)) return false;
        } else if (!PrepareStructure(structure, true, candidate, error) || !PreserveStructureProperties(candidate, error)) return false;
        for (const auto& mesh : result.meshes) candidate.m_drawableIndexOverrides.remove(mesh.artMeshId);
        auto artwork = m_artwork;
        for (const auto& mesh : request.meshes) {
            CModelDrawableMaterial material;
            CModelArtwork::State remappedArtwork;
            if (!candidate.CaptureDrawableMaterial(mesh.artMeshId, material, error) ||
                !CArtworkReconciler::RemapMesh(artwork, mesh, material, candidate.DrawableIndices(mesh.artMeshId), remappedArtwork, error)) return false;
            artwork = std::move(remappedArtwork);
        }
        if (!candidate.SetArtwork(artwork, error)) return false;
        QVector<CGeometryObjectPairs::VertexRemap> pairRemaps;
        for (const auto& mesh : result.meshes)
            pairRemaps.append({{CModelGeometryKeyform::ArtMesh, mesh.artMeshId}, mesh.oldToNew});
        if (!ReconcileGeometryObjectPairs(candidate, pairRemaps, nullptr, nullptr, error)) return false;
        const QString commandName = request.meshes.isEmpty() ? QStringLiteral("Glue") : request.meshes.first().artMeshId;
        CModelEdit command{commandName, QStringLiteral("mesh"), CModelEdit::MeshStructure,
                           AuthoringStructureState(), candidate.AuthoringStructureState(), -1, false};
        command.meshRemaps = result.meshes;
        command.meshRequest = std::make_shared<const CMocMeshStructureBuilder::Request>(request);
        if (!ValidateMeshTransition(candidate, command, true, error)) return false;
        if (!SealMeshStructureEdit(command, candidate, error)) return false;
        edit = std::move(command);
        if (error) error->clear();
        return true;
    } catch (const std::exception&) { return fail(error, QStringLiteral("无法构建网格结构编辑")); }
}

bool CModelDocument::BuildCloneArtMeshCandidate(const CMocMeshStructureBuilder::CloneRequest& sourceRequest,
                                                CModelDocument& candidate, QString* error) const
{
    try {
        if (!m_blendShapeConstraints.isEmpty())
            return fail(error, QStringLiteral("存在融合限制曲线编辑，克隆前必须先提交或清除曲线状态"));
        if (sourceRequest.sourceArtMeshId.isEmpty() || sourceRequest.newArtMeshId.isEmpty() ||
            sourceRequest.sourceArtMeshId == sourceRequest.newArtMeshId)
            return fail(error, QStringLiteral("ArtMesh 克隆身份无效"));
        if (IsLocked(QStringLiteral("artmesh"), sourceRequest.sourceArtMeshId))
            return fail(error, QStringLiteral("源 ArtMesh 已锁定：%1").arg(sourceRequest.sourceArtMeshId));
        if (std::none_of(m_drawables.cbegin(), m_drawables.cend(), [&](const auto& mesh) {
                return mesh.id == sourceRequest.sourceArtMeshId;
            }))
            return fail(error, QStringLiteral("未知 ArtMesh：%1").arg(sourceRequest.sourceArtMeshId));
        QByteArray base;
        CMocMeshStructureBuilder::Result result;
        if (!BuildPreviewMoc(base, error) || !CMocMeshStructureBuilder::Clone(base, sourceRequest, result, error)) return false;
        Builder::ColumnState structure;
        if (!Builder::CaptureColumnState(m_sourceBytes, result.bytes, structure, error)) return false;
        if (!PrepareCloneStructure(structure, sourceRequest, candidate, error) || !PreserveStructureProperties(candidate, error)) return false;
        const auto duplicate = [&](auto& values) {
            const auto found = values.constFind(sourceRequest.sourceArtMeshId);
            if (found != values.cend()) values.insert(sourceRequest.newArtMeshId, *found);
        };
        duplicate(candidate.m_drawableOpacityOverrides);
        duplicate(candidate.m_drawableDrawOrderOverrides);
        duplicate(candidate.m_drawableKeyformPositionOverrides);
        duplicate(candidate.m_drawableIndexOverrides);
        duplicate(candidate.m_drawableMaskIndexOverrides);
        duplicate(candidate.m_drawableMaskInvertedOverrides);
        if (const auto found = m_drawableMaterialOverrides.constFind(sourceRequest.sourceArtMeshId);
            found != m_drawableMaterialOverrides.cend()) {
            auto material = *found;
            material.id = sourceRequest.newArtMeshId;
            candidate.m_drawableMaterialOverrides.insert(sourceRequest.newArtMeshId, std::move(material));
        }
        if (const auto found = m_artwork.bindings.constFind(sourceRequest.sourceArtMeshId);
            found != m_artwork.bindings.cend()) {
            auto artwork = candidate.m_artwork;
            auto clone = *found;
            clone.meshId = sourceRequest.newArtMeshId;
            clone.sampledMaterial.id = sourceRequest.newArtMeshId;
            artwork.bindings.insert(sourceRequest.newArtMeshId, std::move(clone));
            if (!candidate.SetArtwork(artwork, error)) return false;
        }
        auto names = m_displayNames;
        names.artMeshes.insert(sourceRequest.newArtMeshId,
                               m_displayNames.artMeshes.value(sourceRequest.sourceArtMeshId,
                                                               sourceRequest.sourceArtMeshId) + QStringLiteral(" Copy"));
        if (!candidate.SetDisplayNames(names, error) || !ReconcileGeometryObjectPairs(candidate, {}, nullptr, nullptr, error)) return false;
        return true;
    } catch (const std::exception&) {
        return fail(error, QStringLiteral("无法构建 ArtMesh 克隆候选"));
    }
}

bool CModelDocument::MakeCloneArtMeshEdit(const CMocMeshStructureBuilder::CloneRequest& sourceRequest,
                                          CModelEdit& edit, QString* error) const
{
    try {
        CModelDocument candidate;
        if (!BuildCloneArtMeshCandidate(sourceRequest, candidate, error)) return false;
        CModelEdit command{sourceRequest.newArtMeshId, QStringLiteral("mesh"), CModelEdit::MeshStructure,
                           AuthoringStructureState(), candidate.AuthoringStructureState(), -1, false};
        command.cloneRequest = std::make_shared<const CMocMeshStructureBuilder::CloneRequest>(sourceRequest);
        if (!ValidateCloneTransition(candidate, command, true, error)) return false;
        edit = std::move(command);
        if (error) error->clear();
        return true;
    } catch (const std::exception&) {
        return fail(error, QStringLiteral("无法构建 ArtMesh 克隆编辑"));
    }
}

bool CModelDocument::AnalyzeMeshStructureEdit(const CMocMeshStructureBuilder::Request& request,
                                             CMocMeshStructureBuilder::Impact& impact, QString* error) const
{
    try {
        if (request.meshes.isEmpty() && request.glueWeights.isEmpty()) return fail(error, QStringLiteral("没有选定编辑网格或 Glue 端点"));
        QVector<Builder::BindingRequest> owners;
        for (const auto& mesh : request.meshes)
            owners.append(Builder::BindingRequest{{Builder::Owner::ArtMesh, mesh.artMeshId}, {}});
        if (!canBind(*this, owners, error)) return false;
        QByteArray bytes;
        return BuildMeshBase(request, bytes, error) && CMocMeshStructureBuilder::Analyze(bytes, request, impact, error);
    } catch (const std::exception&) { return fail(error, QStringLiteral("无法分析网格结构编辑")); }
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

bool CModelDocument::MakeBindingKeysEdit(const Builder::EditBindingKeysRequest& request,
                                       CModelEdit& edit, QString* error) const
{
    try {
        if (!canBind(*this, {{request.owner, {}}}, error)) return false;
        QByteArray base;
        Builder::Result result;
        if (!BuildPreviewMoc(base, error) || !Builder::EditBindingKeys(base, request, result, error)) return false;
        if (result.bytes == base) return fail(error, QStringLiteral("绑定轴关键值没有变化"));
        CModelEdit command;
        if (!MakeParameterStructureEdit(result, request.parameterId, nullptr, false, command, error, &request)) return false;
        command.parameterChange = CModelEdit::ParameterChange::BindingKeys;
        command.bindingKeysRequest = std::make_shared<const Builder::EditBindingKeysRequest>(request);
        edit = std::move(command);
        return true;
    } catch (const std::exception&) { return fail(error, QStringLiteral("无法编辑绑定轴关键值")); }
}

bool CModelDocument::MakeBindingKeysBatchEdit(const Builder::EditBindingKeysBatchRequest& request,
                                             CModelEdit& edit, QString* error) const
{
    try {
        if (!canBind(*this, {{request.owner, {}}}, error)) return false;
        QByteArray base;
        Builder::Result result;
        if (!BuildPreviewMoc(base, error) || !Builder::EditBindingKeysBatch(base, request, result, error)) return false;
        if (result.bytes == base) return fail(error, QStringLiteral("绑定轴关键值没有变化"));
        CModelEdit command;
        if (!MakeParameterStructureEdit(result, request.parameterId, nullptr, false, command, error, nullptr, &request)) return false;
        command.parameterChange = CModelEdit::ParameterChange::BindingKeys;
        command.bindingKeysBatchRequest = std::make_shared<const Builder::EditBindingKeysBatchRequest>(request);
        edit = std::move(command);
        return true;
    } catch (const std::exception&) { return fail(error, QStringLiteral("无法编辑绑定轴关键值")); }
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
