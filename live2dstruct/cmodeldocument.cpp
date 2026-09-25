#include "cmodeldocument.h"
#include "cmeshtopology.h"
#include "cdrawableflags.h"
#include "csectiontable.h"
#include "ccanvasgeometry.h"
#include "ckeyformbindinggrid.h"
#include "cmocpreviewbuilder.h"
#include "util/cfilereader.h"
#include "struct/cart_mesh_keyforms.h"
#include <QHash>
#include <QCryptographicHash>
#include <QSet>
#include <QtGlobal>
#include <cmath>
#include <limits>
#include <utility>

namespace live2dstruct {

namespace {
void setError(QString* error, const QString& value)
{
    if (error) *error = value;
}

bool inRange(int value, int size)
{
    return value >= 0 && value < size;
}

bool validSlice(int begin, int count, int size)
{
    return begin >= 0 && count >= 0 && begin <= size && count <= size - begin;
}

bool metadataFromVariant(const QVariant& value, const CModelParameterMetadata& current,
                         CModelParameterMetadata* metadata)
{
    if (!metadata || !value.canConvert<QVariantMap>()) return false;
    const QVariantMap map = value.toMap();
    CModelParameterMetadata parsed = current;
    if (map.contains(QStringLiteral("displayName"))) {
        if (!map.value(QStringLiteral("displayName")).canConvert<QString>()) return false;
        parsed.displayName = map.value(QStringLiteral("displayName")).toString();
    }
    const auto readFloatField = [&map](const QString& key, float* target) {
        if (!map.contains(key)) return true;
        bool ok = false;
        const double number = map.value(key).toDouble(&ok);
        if (!ok || !std::isfinite(number) || number < -std::numeric_limits<float>::max() ||
            number > std::numeric_limits<float>::max()) return false;
        *target = static_cast<float>(number);
        return std::isfinite(*target);
    };
    if (!readFloatField(QStringLiteral("minimum"), &parsed.minimum) ||
        !readFloatField(QStringLiteral("maximum"), &parsed.maximum) ||
        !readFloatField(QStringLiteral("defaultValue"), &parsed.defaultValue)) return false;
    if (map.contains(QStringLiteral("decimalPlaces"))) {
        bool ok = false;
        const int decimalPlaces = map.value(QStringLiteral("decimalPlaces")).toInt(&ok);
        if (!ok) return false;
        parsed.decimalPlaces = decimalPlaces;
    }
    if (map.contains(QStringLiteral("repeat"))) {
        if (!map.value(QStringLiteral("repeat")).canConvert<bool>()) return false;
        parsed.repeat = map.value(QStringLiteral("repeat")).toBool();
    }
    *metadata = parsed;
    return true;
}
}

void CModelDocument::Clear()
{
    m_loaded = false;
    m_sourcePath.clear();
    m_sourceBytes.clear();
    m_sourceDigest.clear();
    m_keyformBindings.reset();
    m_previewBuilder.reset();
    m_previewError.clear();
    m_canvasPixels = {};
    m_parameters.clear();
    m_parts.clear();
    m_deformers.clear();
    m_drawables.clear();
    m_drawOrderGroups.clear();
    m_drawableOpacityOverrides.clear();
    m_drawableDrawOrderOverrides.clear();
    m_parameterValues.clear();
    m_parameterMetadataOverrides.clear();
    m_drawableKeyformPositionOverrides.clear();
    m_drawableIndexOverrides.clear();
    m_drawableMaskIndexOverrides.clear();
    m_deformerParentOverrides.clear();
    m_deformerRotationKeyformOverrides.clear();
    m_deformerWarpKeyformOverrides.clear();
    m_drawableMaskInvertedOverrides.clear();
    m_lockedKeys.clear();
    ClearEditHistory();
}

bool CModelDocument::Load(const QString& mocPath, QString* error)
{
    CReader reader(mocPath);
    CModelDocument candidate;
    if (!candidate.LoadInternal(reader, mocPath, error)) return false;
    *this = std::move(candidate);
    return true;
}

bool CModelDocument::LoadBytes(const QByteArray& bytes, const QString& sourcePath, QString* error)
{
    auto reader = CReader::FromBytes(bytes);
    CModelDocument candidate;
    if (!candidate.LoadInternal(reader, sourcePath, error)) return false;
    *this = std::move(candidate);
    return true;
}

bool CModelDocument::SourceMatchesDisk(QString* error) const
{
    if (error) error->clear();
    if (!m_loaded || m_sourceDigest.isEmpty()) {
        setError(error, QStringLiteral("尚未加载模型快照"));
        return false;
    }
    QFile file(m_sourcePath);
    QCryptographicHash digest(QCryptographicHash::Sha256);
    if (!file.open(QIODevice::ReadOnly) || !digest.addData(&file) || file.error() != QFileDevice::NoError) {
        setError(error, QStringLiteral("无法读取加载时的源模型：%1").arg(m_sourcePath));
        return false;
    }
    if (digest.result() != m_sourceDigest) {
        setError(error, QStringLiteral("源模型已在外部更改，当前编辑仍对应加载时的版本：%1").arg(m_sourcePath));
        return false;
    }
    return true;
}

bool CModelDocument::KeyformParameterValues(const QString& id, const QString& type, int keyformIndex,
                                           QMap<QString, float>& values, QString* error) const
{
    if (error) error->clear();
    if (!m_loaded || !m_keyformBindings) {
        setError(error, QStringLiteral("尚未加载关键形绑定"));
        return false;
    }
    CKeyformBindingGrid::ObjectType objectType;
    if (type == QStringLiteral("artmesh")) objectType = CKeyformBindingGrid::ArtMesh;
    else if (type == QStringLiteral("warp")) objectType = CKeyformBindingGrid::Warp;
    else if (type == QStringLiteral("rotation")) objectType = CKeyformBindingGrid::Rotation;
    else {
        setError(error, QStringLiteral("对象类型没有关键形绑定：%1").arg(type));
        return false;
    }
    QMap<QString, float> candidate;
    if (!m_keyformBindings->Pose(objectType, id, keyformIndex, candidate, error)) return false;
    QHash<QString, const CModelParameter*> parameters;
    for (const auto& parameter : m_parameters)
        if (!parameters.contains(parameter.id)) parameters.insert(parameter.id, &parameter);
    for (auto value = candidate.cbegin(); value != candidate.cend(); ++value) {
        const auto* parameter = parameters.value(value.key(), nullptr);
        if (!parameter) {
            setError(error, QStringLiteral("关键形引用的参数不存在：%1").arg(value.key()));
            return false;
        }
        const auto metadata = m_parameterMetadataOverrides.constFind(value.key());
        const float minimum = metadata == m_parameterMetadataOverrides.cend() ? parameter->minimum : metadata->minimum;
        const float maximum = metadata == m_parameterMetadataOverrides.cend() ? parameter->maximum : metadata->maximum;
        if (value.value() < minimum || value.value() > maximum) {
            setError(error, QStringLiteral("关键形的参数值超出当前范围：%1 = %2").arg(value.key()).arg(value.value()));
            return false;
        }
    }
    values = std::move(candidate);
    return true;
}

bool CModelDocument::BuildPreviewMoc(QByteArray& bytes, QString* error) const
{
    if (error) error->clear();
    if (!m_loaded) {
        setError(error, QStringLiteral("尚未加载模型快照"));
        return false;
    }
    if (m_drawableKeyformPositionOverrides.isEmpty()) {
        bytes = m_sourceBytes;
        return true;
    }
    if (!m_previewBuilder) {
        setError(error, m_previewError);
        return false;
    }
    return m_previewBuilder->Build(m_drawableKeyformPositionOverrides, bytes, error);
}

bool CModelDocument::LoadInternal(CReader& reader, const QString& sourcePath, QString* error)
{
    Clear();
    if (error) error->clear();
    CSectionTable table(reader);
    if (!table.IsValid()) {
        setError(error, reader.Error().isEmpty() ? QStringLiteral("moc3 解析失败") : reader.Error());
        return false;
    }

    m_sourcePath = sourcePath;
    m_sourceBytes = reader.m_byte;
    m_sourceDigest = QCryptographicHash::hash(m_sourceBytes, QCryptographicHash::Sha256);
    auto bindings = std::make_shared<CKeyformBindingGrid>();
    if (!bindings->Build(table, error)) return false;
    m_keyformBindings = std::move(bindings);
    auto preview = std::make_shared<CMocPreviewBuilder>();
    if (preview->Initialize(m_sourceBytes, &m_previewError)) m_previewBuilder = std::move(preview);
    m_canvasPixels = CCanvasGeometry::PixelSize(table.canvas_info->width, table.canvas_info->height);

    m_parameters.reserve(table.parameters.size());
    for (const CParameters* source : table.parameters) {
        CModelParameter parameter;
        parameter.id = source->ids;
        parameter.minimum = source->min_values;
        parameter.maximum = source->max_values;
        parameter.defaultValue = source->default_values;
        parameter.decimalPlaces = source->decimal_places;
        parameter.repeat = source->is_repeat;
        parameter.type = source->parameter_types;
        if (table.keys && validSlice(source->keys_sources_begin_indices, source->keys_sources_counts,
                                     static_cast<int>(table.keys->values.size()))) {
            parameter.keyValues.reserve(source->keys_sources_counts);
            for (int keyIndex = 0; keyIndex < source->keys_sources_counts; ++keyIndex) {
                parameter.keyValues.append(table.keys->GetData(source->keys_sources_begin_indices + keyIndex));
            }
        }
        m_parameters.append(parameter);
    }

    m_parts.reserve(table.parts.size());
    for (const CParts* source : table.parts) {
        m_parts.append({source->ids, source->parent_part_indices, source->is_visible, source->is_enabled});
    }

    m_deformers.reserve(table.deformers.size());
    for (const CDeformers* source : table.deformers) {
        CModelDeformer deformer;
        deformer.id = source->ids;
        deformer.type = source->types == 0 ? CModelDeformer::Warp : CModelDeformer::Rotation;
        deformer.parentPartIndex = source->parent_part_indices;
        deformer.parentDeformerIndex = source->parent_deformer_indices;
        deformer.visible = source->is_visible;
        deformer.enabled = source->is_enabled;
        if (deformer.type == CModelDeformer::Warp && inRange(source->specific_sources_indices, table.warp_deformers.size())) {
            const CWarp_deformers* warp = table.warp_deformers[source->specific_sources_indices];
            deformer.rows = warp->rows;
            deformer.columns = warp->columns;
            deformer.quadTransform = warp->is_quad_source;
            const qint64 gridVertexCount = (warp->rows >= 0 && warp->columns >= 0)
                ? (static_cast<qint64>(warp->rows) + 1) * (static_cast<qint64>(warp->columns) + 1) : 0;
            if (warp->rows <= 1000 && warp->columns <= 1000 && gridVertexCount > 0 && gridVertexCount <= 1000000 &&
                validSlice(warp->keyform_sources_begin_indices, warp->keyform_sources_counts,
                           table.warp_deformer_keyforms.size())) {
                deformer.warpKeyforms.reserve(warp->keyform_sources_counts);
                for (int keyformIndex = 0; keyformIndex < warp->keyform_sources_counts; ++keyformIndex) {
                    const CWarp_deformer_keyforms* sourceKeyform = table.warp_deformer_keyforms[
                        warp->keyform_sources_begin_indices + keyformIndex];
                    CModelWarpKeyform keyform;
                    keyform.opacity = sourceKeyform->opacities;
                    if (validSlice(sourceKeyform->keyform_position_sources_begin_indices,
                                   static_cast<int>(gridVertexCount * 2), table.keyform_positions->xys.size())) {
                        keyform.positions.reserve(static_cast<int>(gridVertexCount * 2));
                        for (int point = 0; point < gridVertexCount; ++point) {
                            keyform.positions.append(table.keyform_positions->GetData(
                                sourceKeyform->keyform_position_sources_begin_indices + point * 2));
                            keyform.positions.append(table.keyform_positions->GetData(
                                sourceKeyform->keyform_position_sources_begin_indices + point * 2 + 1));
                        }
                    }
                    deformer.warpKeyforms.append(keyform);
                }
            }
        } else if (deformer.type == CModelDeformer::Rotation &&
                   inRange(source->specific_sources_indices, table.rotation_deformers.size())) {
            const CRotation_deformers* rotation = table.rotation_deformers[source->specific_sources_indices];
            deformer.baseAngle = rotation->base_angles;
            if (validSlice(rotation->keyform_sources_begin_indices, rotation->keyform_sources_counts,
                           table.rotation_deformer_keyforms.size())) {
                deformer.rotationKeyforms.reserve(rotation->keyform_sources_counts);
                for (int keyformIndex = 0; keyformIndex < rotation->keyform_sources_counts; ++keyformIndex) {
                    const CRotation_deformer_keyforms* sourceKeyform = table.rotation_deformer_keyforms[
                        rotation->keyform_sources_begin_indices + keyformIndex];
                    deformer.rotationKeyforms.append({sourceKeyform->opacities, sourceKeyform->angles,
                                                      sourceKeyform->origin_x, sourceKeyform->origin_y,
                                                      sourceKeyform->scales, sourceKeyform->is_reflect_x,
                                                      sourceKeyform->is_reflect_y});
                }
            }
        }
        m_deformers.append(deformer);
    }

    m_drawables.reserve(table.art_meshes.size());
    for (const CArt_meshes* source : table.art_meshes) {
        CModelDrawable drawable;
        drawable.id = source->ids;
        drawable.parentPartIndex = source->parent_part_indices;
        drawable.parentDeformerIndex = source->parent_deformer_indices;
        drawable.textureIndex = source->texture_nos;
        drawable.flags = source->drawable_flags;
        drawable.blendMode = CDrawableFlags::BlendMode(source->drawable_flags);
        drawable.culling = CDrawableFlags::Culling(source->drawable_flags);
        drawable.vertexCount = source->vertex_counts;
        drawable.visible = source->is_visible;
        drawable.enabled = source->is_enabled;
        const bool vertexCountValid = source->vertex_counts >= 0 && source->vertex_counts <= 1000000 &&
            source->vertex_counts <= std::numeric_limits<int>::max() / 2;
        if (vertexCountValid && validSlice(source->uv_sources_begin_indices, source->vertex_counts * 2,
                                           static_cast<int>(table.uvs->uvs.size()))) {
            for (int i = 0; i < source->vertex_counts * 2; ++i) {
                drawable.uvs.append(table.uvs->uvs[source->uv_sources_begin_indices + i]);
            }
        }
        if (validSlice(source->position_index_sources_begin_indices, source->position_index_sources_counts,
                       static_cast<int>(table.position_indices->indices.size()))) {
            for (int i = 0; i < source->position_index_sources_counts; ++i) {
                const short rawIndex = table.position_indices->indices[
                    source->position_index_sources_begin_indices + i];
                if (rawIndex < 0 || rawIndex >= source->vertex_counts) continue;
                drawable.indices.append(static_cast<quint16>(rawIndex));
            }
        }
        if (validSlice(source->drawable_mask_sources_begin_indices, source->drawable_mask_sources_counts,
                       static_cast<int>(table.drawable_masks.size()))) {
            for (int i = 0; i < source->drawable_mask_sources_counts; ++i) {
                const int index = source->drawable_mask_sources_begin_indices + i;
                if (inRange(index, table.drawable_masks.size())) {
                    drawable.maskIndices.append(table.drawable_masks[index]->art_mesh_sources_indices);
                }
            }
        }
        if (validSlice(source->keyform_sources_begin_indices, source->keyform_sources_counts,
                       static_cast<int>(table.art_mesh_keyforms.size()))) {
            for (int i = 0; i < source->keyform_sources_counts; ++i) {
                const int index = source->keyform_sources_begin_indices + i;
                if (!inRange(index, table.art_mesh_keyforms.size())) continue;
                const CArt_mesh_keyforms* sourceKeyform = table.art_mesh_keyforms[index];
                CModelKeyform keyform;
                keyform.opacity = sourceKeyform->opacities;
                keyform.drawOrder = sourceKeyform->draw_orders;
                if (i == 0) {
                    drawable.opacity = keyform.opacity;
                    drawable.drawOrder = keyform.drawOrder;
                }
                const int begin = sourceKeyform->keyform_position_sources_begin_indices;
                if (vertexCountValid && validSlice(begin, source->vertex_counts * 2,
                                                   static_cast<int>(table.keyform_positions->xys.size()))) {
                    for (int v = 0; v < source->vertex_counts * 2; ++v) {
                        keyform.positions.append(table.keyform_positions->xys[begin + v]);
                    }
                }
                drawable.keyforms.append(keyform);
            }
        }
        m_drawables.append(drawable);
    }

    m_drawOrderGroups.reserve(table.draw_order_groups.size());
    for (const CDraw_order_groups* source : table.draw_order_groups) {
        CModelDrawOrderGroup group;
        group.minimumDrawOrder = source->minimum_draw_orders;
        group.maximumDrawOrder = source->maximum_draw_orders;
        group.objectCount = source->object_sources_counts;
        group.totalObjectCount = source->object_sources_total_counts;
        group.objectsValid = validSlice(source->object_sources_begin_indices, group.objectCount,
                                        table.draw_order_group_objects.size());
        if (group.objectsValid) {
            group.objects.reserve(group.objectCount);
            for (int i = 0; i < group.objectCount; ++i) {
                const CDraw_order_group_objects* object = table.draw_order_group_objects[
                    source->object_sources_begin_indices + i];
                group.objects.append({object->types, object->indices, object->self_indices});
            }
        }
        m_drawOrderGroups.append(group);
    }

    m_loaded = true;
    return true;
}

bool CModelDocument::SetPartVisible(const QString& id, bool visible)
{
    for (CModelPart& part : m_parts) {
        if (part.id != id) continue;
        part.visible = visible;
        return true;
    }
    return false;
}

bool CModelDocument::SetParameterValue(const QString& id, float value)
{
    if (!std::isfinite(value)) return false;
    const CModelParameterMetadata metadata = ParameterMetadata(id);
    if (metadata.displayName.isEmpty() && !m_parameterMetadataOverrides.contains(id)) {
        bool found = false;
        for (const CModelParameter& parameter : m_parameters) if (parameter.id == id) { found = true; break; }
        if (!found) return false;
    }
    if (value < metadata.minimum || value > metadata.maximum) return false;
    m_parameterValues.insert(id, value);
    return true;
}

float CModelDocument::ParameterValue(const QString& id) const
{
    const CModelParameterMetadata metadata = ParameterMetadata(id);
    if (metadata.displayName.isEmpty() && !m_parameterMetadataOverrides.contains(id)) return 0.0f;
    return m_parameterValues.value(id, metadata.defaultValue);
}

CModelParameterMetadata CModelDocument::ParameterMetadata(const QString& id) const
{
    for (const CModelParameter& parameter : m_parameters) {
        if (parameter.id != id) continue;
        CModelParameterMetadata metadata;
        metadata.displayName = parameter.id;
        metadata.minimum = parameter.minimum;
        metadata.maximum = parameter.maximum;
        metadata.defaultValue = parameter.defaultValue;
        metadata.decimalPlaces = parameter.decimalPlaces;
        metadata.repeat = parameter.repeat;
        if (m_parameterMetadataOverrides.contains(id)) {
            const CModelParameterMetadata& override = m_parameterMetadataOverrides.value(id);
            if (!override.displayName.isEmpty()) metadata.displayName = override.displayName;
            metadata.minimum = override.minimum;
            metadata.maximum = override.maximum;
            metadata.defaultValue = override.defaultValue;
            metadata.decimalPlaces = override.decimalPlaces;
            metadata.repeat = override.repeat;
        }
        return metadata;
    }
    return {};
}

bool CModelDocument::SetParameterMetadata(const QString& id, const CModelParameterMetadata& metadata)
{
    const CModelParameterMetadata base = ParameterMetadata(id);
    bool found = false;
    const CModelParameter* source = nullptr;
    for (const CModelParameter& parameter : m_parameters) {
        if (parameter.id == id) { found = true; source = &parameter; break; }
    }
    if (!found || !source || !std::isfinite(metadata.minimum) || !std::isfinite(metadata.maximum) ||
        !std::isfinite(metadata.defaultValue) || metadata.minimum > metadata.maximum ||
        metadata.defaultValue < metadata.minimum || metadata.defaultValue > metadata.maximum ||
        metadata.decimalPlaces < 0 || metadata.decimalPlaces > 9) return false;
    float previousKey = -std::numeric_limits<float>::infinity();
    for (float key : source->keyValues) {
        if (!std::isfinite(key) || key < metadata.minimum || key > metadata.maximum || key < previousKey) return false;
        previousKey = key;
    }
    if (m_parameterValues.contains(id)) {
        const float value = m_parameterValues.value(id);
        if (!std::isfinite(value) || value < metadata.minimum || value > metadata.maximum) return false;
    }
    CModelParameterMetadata normalized = metadata;
    if (normalized.displayName.isEmpty()) normalized.displayName = base.displayName;
    m_parameterMetadataOverrides.insert(id, normalized);
    return true;
}

QVector<float> CModelDocument::DrawableKeyformPositions(const QString& id, int keyformIndex) const
{
    if (keyformIndex < 0) return {};
    const auto overrideIt = m_drawableKeyformPositionOverrides.constFind(id);
    if (overrideIt != m_drawableKeyformPositionOverrides.cend() && keyformIndex < overrideIt->size() &&
        !overrideIt->at(keyformIndex).isEmpty()) {
        return overrideIt->at(keyformIndex);
    }
    for (const CModelDrawable& drawable : m_drawables) {
        if (drawable.id != id || keyformIndex >= drawable.keyforms.size()) continue;
        return drawable.keyforms.at(keyformIndex).positions;
    }
    return {};
}

bool CModelDocument::SetDrawableKeyformPositions(const QString& id, int keyformIndex,
                                                 const QVector<float>& positions)
{
    if (keyformIndex < 0) return false;
    for (const CModelDrawable& drawable : m_drawables) {
        if (drawable.id != id || keyformIndex >= drawable.keyforms.size()) continue;
        if (positions.size() != drawable.vertexCount * 2) return false;
        for (float value : positions) if (!std::isfinite(value)) return false;
        auto& overrides = m_drawableKeyformPositionOverrides[id];
        if (overrides.size() < drawable.keyforms.size()) overrides.resize(drawable.keyforms.size());
        overrides[keyformIndex] = positions;
        return true;
    }
    return false;
}

QVector<quint16> CModelDocument::DrawableIndices(const QString& id) const
{
    if (m_drawableIndexOverrides.contains(id)) return m_drawableIndexOverrides.value(id);
    for (const CModelDrawable& drawable : m_drawables) {
        if (drawable.id == id) return drawable.indices;
    }
    return {};
}

bool CModelDocument::SetDrawableIndices(const QString& id, const QVector<quint16>& indices)
{
    for (const CModelDrawable& drawable : m_drawables) {
        if (drawable.id != id) continue;
        if (!CMeshTopology::IsValid(indices, drawable.vertexCount)) return false;
        m_drawableIndexOverrides.insert(id, indices);
        return true;
    }
    return false;
}

QVector<int> CModelDocument::DrawableMaskIndices(const QString& id) const
{
    if (m_drawableMaskIndexOverrides.contains(id)) return m_drawableMaskIndexOverrides.value(id);
    for (const CModelDrawable& drawable : m_drawables) {
        if (drawable.id == id) return drawable.maskIndices;
    }
    return {};
}

int CModelDocument::DeformerParentIndex(const QString& id) const
{
    if (m_deformerParentOverrides.contains(id)) return m_deformerParentOverrides.value(id);
    for (const CModelDeformer& deformer : m_deformers) {
        if (deformer.id == id) return deformer.parentDeformerIndex;
    }
    return -1;
}

QSet<QString> CModelDocument::HiddenDeformerDrawables() const
{
    const int count = m_deformers.size();
    QVector<int> firstChild(count, -1), nextSibling(count, -1), pending;
    QVector<quint8> hidden(count, 0);
    pending.reserve(count);
    for (int index = 0; index < count; ++index) {
        const auto& deformer = m_deformers[index];
        const int parent = m_deformerParentOverrides.value(deformer.id, deformer.parentDeformerIndex);
        if (inRange(parent, count)) {
            nextSibling[index] = firstChild[parent];
            firstChild[parent] = index;
        }
        if (!deformer.visible) {
            hidden[index] = 1;
            pending.append(index);
        }
    }
    for (int next = 0; next < pending.size(); ++next) {
        for (int child = firstChild[pending[next]]; child != -1; child = nextSibling[child]) {
            if (hidden[child]) continue;
            hidden[child] = 1;
            pending.append(child);
        }
    }
    QSet<QString> result;
    for (const auto& drawable : m_drawables)
        if (inRange(drawable.parentDeformerIndex, count) && hidden[drawable.parentDeformerIndex])
            result.insert(drawable.id);
    return result;
}

bool CModelDocument::SetDeformerParentIndex(const QString& id, int parentIndex)
{
    int deformerIndex = -1;
    for (int i = 0; i < m_deformers.size(); ++i) {
        if (m_deformers[i].id == id) { deformerIndex = i; break; }
    }
    if (deformerIndex < 0) return false;
    if (parentIndex < -1 || parentIndex >= m_deformers.size() || parentIndex == deformerIndex) return false;
    if (parentIndex >= 0 && m_deformers[parentIndex].parentPartIndex != m_deformers[deformerIndex].parentPartIndex) return false;
    QVector<int> states(m_deformers.size(), 0);
    int current = parentIndex;
    while (current >= 0) {
        if (current >= m_deformers.size() || states[current] == 1) return false;
        if (current == deformerIndex) return false;
        states[current] = 1;
        current = DeformerParentIndex(m_deformers[current].id);
    }
    m_deformerParentOverrides.insert(id, parentIndex);
    return true;
}

bool CModelDocument::SetDeformerParentIndices(const QHash<QString, int>& parents)
{
    QHash<QString, int> candidate = m_deformerParentOverrides;
    for (auto it = parents.cbegin(); it != parents.cend(); ++it) {
        int deformerIndex = -1;
        for (int i = 0; i < m_deformers.size(); ++i) {
            if (m_deformers[i].id == it.key()) { deformerIndex = i; break; }
        }
        const int parentIndex = it.value();
        if (deformerIndex < 0 || parentIndex < -1 || parentIndex >= m_deformers.size() || parentIndex == deformerIndex ||
            (parentIndex >= 0 && m_deformers[parentIndex].parentPartIndex != m_deformers[deformerIndex].parentPartIndex)) {
            return false;
        }
        candidate.insert(it.key(), parentIndex);
    }
    const auto parentOf = [&candidate, this](int index) {
        if (index < 0 || index >= m_deformers.size()) return -1;
        return candidate.value(m_deformers[index].id, m_deformers[index].parentDeformerIndex);
    };
    for (auto it = parents.cbegin(); it != parents.cend(); ++it) {
        int current = it.value();
        QSet<int> visited;
        while (current >= 0) {
            if (current >= m_deformers.size() || visited.contains(current)) return false;
            visited.insert(current);
            current = parentOf(current);
        }
    }
    m_deformerParentOverrides = candidate;
    return true;
}

QVector<CModelRotationKeyform> CModelDocument::DeformerRotationKeyforms(const QString& id) const
{
    if (m_deformerRotationKeyformOverrides.contains(id))
        return m_deformerRotationKeyformOverrides.value(id);
    for (const CModelDeformer& deformer : m_deformers) {
        if (deformer.id == id && deformer.type == CModelDeformer::Rotation)
            return deformer.rotationKeyforms;
    }
    return {};
}

bool CModelDocument::SetDeformerRotationKeyforms(const QString& id,
                                                 const QVector<CModelRotationKeyform>& keyforms)
{
    for (const CModelDeformer& deformer : m_deformers) {
        if (deformer.id != id || deformer.type != CModelDeformer::Rotation) continue;
        if (keyforms.isEmpty() || keyforms.size() != deformer.rotationKeyforms.size() || keyforms.size() > 10000)
            return false;
        for (const CModelRotationKeyform& keyform : keyforms) {
            if (!std::isfinite(keyform.opacity) || keyform.opacity < 0.0f || keyform.opacity > 1.0f ||
                !std::isfinite(keyform.angle) || !std::isfinite(keyform.originX) ||
                !std::isfinite(keyform.originY) || !std::isfinite(keyform.scale) || keyform.scale < 0.0f)
                return false;
        }
        m_deformerRotationKeyformOverrides.insert(id, keyforms);
        return true;
    }
    return false;
}

QVector<CModelWarpKeyform> CModelDocument::DeformerWarpKeyforms(const QString& id) const
{
    if (m_deformerWarpKeyformOverrides.contains(id))
        return m_deformerWarpKeyformOverrides.value(id);
    for (const CModelDeformer& deformer : m_deformers) {
        if (deformer.id == id && deformer.type == CModelDeformer::Warp)
            return deformer.warpKeyforms;
    }
    return {};
}

bool CModelDocument::SetDeformerWarpKeyforms(const QString& id,
                                             const QVector<CModelWarpKeyform>& keyforms)
{
    for (const CModelDeformer& deformer : m_deformers) {
        if (deformer.id != id || deformer.type != CModelDeformer::Warp) continue;
        if (deformer.rows < 0 || deformer.columns < 0 || deformer.rows > 1000 || deformer.columns > 1000)
            return false;
        const qint64 pointCount = (static_cast<qint64>(deformer.rows) + 1) *
                                  (static_cast<qint64>(deformer.columns) + 1);
        if (pointCount <= 0 || pointCount > 1000000 || keyforms.isEmpty() ||
            keyforms.size() != deformer.warpKeyforms.size() || keyforms.size() > 10000)
            return false;
        for (const CModelWarpKeyform& keyform : keyforms) {
            if (keyform.positions.size() != pointCount * 2 || !std::isfinite(keyform.opacity) ||
                keyform.opacity < 0.0f || keyform.opacity > 1.0f)
                return false;
            for (float position : keyform.positions) if (!std::isfinite(position)) return false;
        }
        m_deformerWarpKeyformOverrides.insert(id, keyforms);
        return true;
    }
    return false;
}

bool CModelDocument::SetDrawableMaskIndices(const QString& id, const QVector<int>& maskIndices)
{
    for (int drawableIndex = 0; drawableIndex < m_drawables.size(); ++drawableIndex) {
        if (m_drawables[drawableIndex].id != id) continue;
        QSet<int> unique;
        for (int maskIndex : maskIndices) {
            if (!inRange(maskIndex, m_drawables.size()) || maskIndex == drawableIndex ||
                unique.contains(maskIndex)) return false;
            unique.insert(maskIndex);
        }
        m_drawableMaskIndexOverrides.insert(id, maskIndices);
        return true;
    }
    return false;
}

bool CModelDocument::DrawableMaskInverted(const QString& id) const
{
    const auto overrideIt = m_drawableMaskInvertedOverrides.constFind(id);
    if (overrideIt != m_drawableMaskInvertedOverrides.cend()) return overrideIt.value();
    for (const auto& drawable : m_drawables)
        if (drawable.id == id) return CDrawableFlags::MaskInverted(drawable.flags);
    return false;
}

bool CModelDocument::SetDrawableMaskInverted(const QString& id, bool inverted)
{
    for (const CModelDrawable& drawable : m_drawables) {
        if (drawable.id != id) continue;
        m_drawableMaskInvertedOverrides.insert(id, inverted);
        return true;
    }
    return false;
}

bool CModelDocument::SetDeformerVisible(const QString& id, bool visible)
{
    for (CModelDeformer& deformer : m_deformers) {
        if (deformer.id != id) continue;
        deformer.visible = visible;
        return true;
    }
    return false;
}

bool CModelDocument::SetDrawableVisible(const QString& id, bool visible)
{
    for (CModelDrawable& drawable : m_drawables) {
        if (drawable.id != id) continue;
        drawable.visible = visible;
        return true;
    }
    return false;
}

bool CModelDocument::SetDrawableOpacity(const QString& id, float opacity)
{
    if (!std::isfinite(opacity)) return false;
    const float clamped = qBound(0.0f, opacity, 1.0f);
    for (const CModelDrawable& drawable : m_drawables) {
        if (drawable.id != id) continue;
        m_drawableOpacityOverrides.insert(id, clamped);
        return true;
    }
    return false;
}

float CModelDocument::DrawableOpacity(const CModelDrawable& drawable) const
{
    return m_drawableOpacityOverrides.value(drawable.id, drawable.opacity);
}

bool CModelDocument::SetDrawableDrawOrder(const QString& id, int drawOrder)
{
    for (const CModelDrawable& drawable : m_drawables) {
        if (drawable.id != id) continue;
        m_drawableDrawOrderOverrides.insert(id, qBound(0, drawOrder, 1000));
        return true;
    }
    return false;
}

float CModelDocument::DrawableDrawOrder(const CModelDrawable& drawable) const
{
    return m_drawableDrawOrderOverrides.value(drawable.id, static_cast<int>(qBound(0.0f, drawable.drawOrder, 1000.0f)));
}

void CModelDocument::SetLocked(const QString& type, const QString& id, bool locked)
{
    if (type.isEmpty() || id.isEmpty()) return;
    const QString key = type + QLatin1Char('|') + id;
    if (locked) m_lockedKeys.insert(key);
    else m_lockedKeys.remove(key);
}

void CModelDocument::ClearLocked()
{
    m_lockedKeys.clear();
}

bool CModelDocument::IsLocked(const QString& type, const QString& id) const
{
    return m_lockedKeys.contains(type + QLatin1Char('|') + id);
}

bool CModelDocument::ApplyEdit(const CModelEdit& edit, bool forward)
{
    const QVariant& value = forward ? edit.after : edit.before;
    if (edit.field == CModelEdit::Visibility) {
        const bool visible = value.toBool();
        if (edit.type == QLatin1String("part")) return SetPartVisible(edit.name, visible);
        if (edit.type == QLatin1String("warp") || edit.type == QLatin1String("rotation")) return SetDeformerVisible(edit.name, visible);
        if (edit.type == QLatin1String("artmesh")) return SetDrawableVisible(edit.name, visible);
        return false;
    }
    if (edit.field == CModelEdit::Opacity) {
        if (edit.type != QLatin1String("artmesh") || !value.canConvert<float>()) return false;
        const float opacity = value.toFloat();
        return std::isfinite(opacity) && opacity >= 0.0f && opacity <= 1.0f && SetDrawableOpacity(edit.name, opacity);
    }
    if (edit.field == CModelEdit::DrawOrder) {
        if (edit.type != QLatin1String("artmesh") || !value.canConvert<int>()) return false;
        const double number = value.toDouble();
        if (!std::isfinite(number) || std::floor(number) != number || number < 0.0 || number > 1000.0) return false;
        return SetDrawableDrawOrder(edit.name, static_cast<int>(number));
    }
    if (edit.field == CModelEdit::ParameterValue) {
        if (edit.type != QLatin1String("parameter") || !value.canConvert<float>()) return false;
        return SetParameterValue(edit.name, value.toFloat());
    }
    if (edit.field == CModelEdit::ParameterValues) {
        if (edit.type != QLatin1String("parameters") || !value.canConvert<QMap<QString, float>>()) return false;
        const QMap<QString, float> values = value.value<QMap<QString, float>>();
        if (values.isEmpty()) return false;
        for (auto it = values.cbegin(); it != values.cend(); ++it) {
            const CModelParameterMetadata metadata = ParameterMetadata(it.key());
            if (metadata.displayName.isEmpty() || !std::isfinite(it.value()) ||
                it.value() < metadata.minimum || it.value() > metadata.maximum) return false;
        }
        for (auto it = values.cbegin(); it != values.cend(); ++it)
            m_parameterValues.insert(it.key(), it.value());
        return true;
    }
    if (edit.field == CModelEdit::ParameterMetadata) {
        if (edit.type != QLatin1String("parameter")) return false;
        CModelParameterMetadata metadata;
        if (!metadataFromVariant(value, ParameterMetadata(edit.name), &metadata)) return false;
        return SetParameterMetadata(edit.name, metadata);
    }
    if (edit.field == CModelEdit::KeyformPositions) {
        if (edit.type != QLatin1String("artmesh") || !value.canConvert<QVector<float>>()) return false;
        return SetDrawableKeyformPositions(edit.name, edit.index, value.value<QVector<float>>());
    }
    if (edit.field == CModelEdit::MeshIndices) {
        if (edit.type != QLatin1String("artmesh") || !value.canConvert<QVector<quint16>>()) return false;
        return SetDrawableIndices(edit.name, value.value<QVector<quint16>>());
    }
    if (edit.field == CModelEdit::MaskIndices) {
        if (edit.type != QLatin1String("artmesh") || !value.canConvert<QVector<int>>()) return false;
        return SetDrawableMaskIndices(edit.name, value.value<QVector<int>>());
    }
    if (edit.field == CModelEdit::DeformerParent) {
        if (edit.type != QLatin1String("warp") && edit.type != QLatin1String("rotation")) return false;
        if (!value.canConvert<int>()) return false;
        return SetDeformerParentIndex(edit.name, value.toInt());
    }
    if (edit.field == CModelEdit::RotationKeyforms) {
        if (edit.type != QLatin1String("rotation") || !value.canConvert<QVector<CModelRotationKeyform>>())
            return false;
        return SetDeformerRotationKeyforms(edit.name, value.value<QVector<CModelRotationKeyform>>());
    }
    if (edit.field == CModelEdit::WarpKeyforms) {
        if (edit.type != QLatin1String("warp") || !value.canConvert<QVector<CModelWarpKeyform>>())
            return false;
        return SetDeformerWarpKeyforms(edit.name, value.value<QVector<CModelWarpKeyform>>());
    }
    if (edit.field == CModelEdit::MaskInverted) {
        if (edit.type != QLatin1String("artmesh") || !value.canConvert<bool>()) return false;
        return SetDrawableMaskInverted(edit.name, value.toBool());
    }
    return false;
}

bool CModelDocument::ExecuteEdit(const CModelEdit& edit)
{
    if (edit.name.isEmpty() || edit.before == edit.after) return false;
    if (!ApplyEdit(edit, true)) return false;
    if (m_editIndex < m_editHistory.size()) m_editHistory.resize(m_editIndex);
    if (edit.coalesce && !m_editHistory.isEmpty() && m_editIndex == m_editHistory.size()) {
        CModelEdit& last = m_editHistory.last();
        if (last.name == edit.name && last.type == edit.type && last.field == edit.field &&
            last.index == edit.index && last.coalesce && last.after == edit.before) {
            last.after = edit.after;
            return true;
        }
    }
    m_editHistory.append(edit);
    m_editIndex = m_editHistory.size();
    return true;
}

bool CModelDocument::PeekUndoEdit(CModelEdit* edit) const
{
    if (m_editIndex <= 0) return false;
    if (edit) *edit = m_editHistory[m_editIndex - 1];
    return true;
}

bool CModelDocument::PeekRedoEdit(CModelEdit* edit) const
{
    if (m_editIndex >= m_editHistory.size()) return false;
    if (edit) *edit = m_editHistory[m_editIndex];
    return true;
}

bool CModelDocument::UndoEdit(CModelEdit* edit)
{
    if (m_editIndex <= 0) return false;
    const CModelEdit command = m_editHistory[m_editIndex - 1];
    if (!ApplyEdit(command, false)) return false;
    --m_editIndex;
    if (edit) *edit = command;
    return true;
}

bool CModelDocument::RedoEdit(CModelEdit* edit)
{
    if (m_editIndex >= m_editHistory.size()) return false;
    const CModelEdit command = m_editHistory[m_editIndex];
    if (!ApplyEdit(command, true)) return false;
    ++m_editIndex;
    if (edit) *edit = command;
    return true;
}

void CModelDocument::ClearEditHistory()
{
    m_editHistory.clear();
    m_editIndex = 0;
}

QSet<QString> CModelDocument::HiddenIds() const
{
    QSet<QString> hidden;
    for (const CModelPart& part : m_parts) if (!part.visible) hidden.insert(part.id);
    for (const CModelDeformer& deformer : m_deformers) if (!deformer.visible) hidden.insert(deformer.id);
    for (const CModelDrawable& drawable : m_drawables) if (!drawable.visible) hidden.insert(drawable.id);
    return hidden;
}

QVector<CModelDiagnostic> CModelDocument::Validate() const
{
    QVector<CModelDiagnostic> diagnostics;
    if (!m_loaded) {
        diagnostics.append({CModelDiagnostic::Error, QStringLiteral("not_loaded"),
                             QStringLiteral("模型尚未加载"), -1});
        return diagnostics;
    }
    if (m_canvasPixels.isEmpty()) {
        diagnostics.append({CModelDiagnostic::Error, QStringLiteral("canvas_size"),
                             QStringLiteral("画布尺寸无效"), -1});
    }

    QSet<QString> parameterIds;
    for (int i = 0; i < m_parameters.size(); ++i) {
        const CModelParameter& parameter = m_parameters[i];
        if (parameter.id.isEmpty() || parameterIds.contains(parameter.id)) {
            diagnostics.append({CModelDiagnostic::Error, QStringLiteral("parameter_id"),
                                 QStringLiteral("参数 ID 为空或重复"), i});
        }
        parameterIds.insert(parameter.id);
        if (parameter.minimum > parameter.maximum ||
            parameter.defaultValue < parameter.minimum || parameter.defaultValue > parameter.maximum) {
            diagnostics.append({CModelDiagnostic::Error, QStringLiteral("parameter_range"),
                                 QStringLiteral("参数范围或默认值无效"), i});
        }
        if (!std::isfinite(parameter.minimum) || !std::isfinite(parameter.maximum) ||
            !std::isfinite(parameter.defaultValue)) {
            diagnostics.append({CModelDiagnostic::Error, QStringLiteral("parameter_nan"),
                                 QStringLiteral("参数包含非有限数值"), i});
        }
        float previousKey = -std::numeric_limits<float>::infinity();
        for (float key : parameter.keyValues) {
            if (!std::isfinite(key) || key < parameter.minimum || key > parameter.maximum || key < previousKey) {
                diagnostics.append({CModelDiagnostic::Warning, QStringLiteral("parameter_keys"),
                                     QStringLiteral("参数关键值无效或未递增"), i});
                break;
            }
            previousKey = key;
        }
    }

    QSet<QString> partIds;
    QVector<int> partStates(m_parts.size(), 0);
    for (int i = 0; i < m_parts.size(); ++i) {
        const CModelPart& part = m_parts[i];
        if (part.id.isEmpty() || partIds.contains(part.id)) {
            diagnostics.append({CModelDiagnostic::Error, QStringLiteral("part_id"),
                                 QStringLiteral("Part ID 为空或重复"), i});
        }
        partIds.insert(part.id);
        if (part.parentIndex != -1 && !inRange(part.parentIndex, m_parts.size())) {
            diagnostics.append({CModelDiagnostic::Error, QStringLiteral("part_parent"),
                                 QStringLiteral("Part 父索引越界"), i});
        }
        if (partStates[i] != 0) continue;
        QVector<int> path;
        int current = i;
        while (inRange(current, m_parts.size()) && partStates[current] == 0) {
            partStates[current] = 1;
            path.append(current);
            current = m_parts[current].parentIndex;
        }
        if (inRange(current, m_parts.size()) && partStates[current] == 1) {
            diagnostics.append({CModelDiagnostic::Error, QStringLiteral("part_cycle"),
                                 QStringLiteral("Part 父子关系存在循环"), current});
        }
        for (int member : path) partStates[member] = 2;
    }

    QVector<int> states(m_deformers.size(), 0);
    for (int i = 0; i < m_deformers.size(); ++i) {
        if (!std::isfinite(m_deformers[i].baseAngle)) {
            diagnostics.append({CModelDiagnostic::Error, QStringLiteral("deformer_angle"),
                                 QStringLiteral("Deformer 基准角度不是有限数值"), i});
        }
        if (m_deformers[i].parentPartIndex != -1 && !inRange(m_deformers[i].parentPartIndex, m_parts.size())) {
            diagnostics.append({CModelDiagnostic::Error, QStringLiteral("deformer_part"),
                                 QStringLiteral("Deformer 所属 Part 越界"), i});
        }
        if (m_deformers[i].type == CModelDeformer::Rotation) {
            for (const CModelRotationKeyform& keyform : DeformerRotationKeyforms(m_deformers[i].id)) {
                if (!std::isfinite(keyform.opacity) || keyform.opacity < 0.0f || keyform.opacity > 1.0f ||
                    !std::isfinite(keyform.angle) || !std::isfinite(keyform.originX) ||
                    !std::isfinite(keyform.originY) || !std::isfinite(keyform.scale) || keyform.scale < 0.0f) {
                    diagnostics.append({CModelDiagnostic::Error, QStringLiteral("rotation_keyform"),
                                         QStringLiteral("Rotation Deformer 关键形数值无效"), i});
                    break;
                }
            }
        } else if (m_deformers[i].type == CModelDeformer::Warp) {
            if (m_deformers[i].rows < 0 || m_deformers[i].columns < 0 ||
                m_deformers[i].rows > 1000 || m_deformers[i].columns > 1000) {
                diagnostics.append({CModelDiagnostic::Error, QStringLiteral("warp_grid"),
                                     QStringLiteral("Warp Deformer 网格尺寸无效"), i});
            }
            const qint64 pointCount = (static_cast<qint64>(m_deformers[i].rows) + 1) *
                                      (static_cast<qint64>(m_deformers[i].columns) + 1);
            for (const CModelWarpKeyform& keyform : DeformerWarpKeyforms(m_deformers[i].id)) {
                if (pointCount <= 0 || pointCount > 1000000 || keyform.positions.size() != pointCount * 2 ||
                    !std::isfinite(keyform.opacity) || keyform.opacity < 0.0f || keyform.opacity > 1.0f) {
                    diagnostics.append({CModelDiagnostic::Error, QStringLiteral("warp_keyform"),
                                         QStringLiteral("Warp Deformer 关键形网格无效"), i});
                    break;
                }
                bool finite = true;
                for (float position : keyform.positions) if (!std::isfinite(position)) { finite = false; break; }
                if (!finite) {
                    diagnostics.append({CModelDiagnostic::Error, QStringLiteral("warp_keyform_nan"),
                                         QStringLiteral("Warp Deformer 关键形包含非有限坐标"), i});
                    break;
                }
            }
        }
        if (states[i] != 0) continue;
        QVector<int> path;
        int current = i;
        while (inRange(current, m_deformers.size()) && states[current] == 0) {
            states[current] = 1;
            path.append(current);
            const int parent = m_deformers[current].parentDeformerIndex;
            if (parent != -1 && !inRange(parent, m_deformers.size())) {
                diagnostics.append({CModelDiagnostic::Error, QStringLiteral("deformer_parent"),
                                     QStringLiteral("Deformer 父索引越界"), current});
                current = -1;
                break;
            }
            current = parent;
        }
        if (inRange(current, m_deformers.size()) && states[current] == 1) {
            diagnostics.append({CModelDiagnostic::Error, QStringLiteral("deformer_cycle"),
                                 QStringLiteral("Deformer 父子关系存在循环"), current});
        }
        for (int member : path) states[member] = 2;
    }

    QSet<QString> drawableIds;
    for (int i = 0; i < m_drawables.size(); ++i) {
        const CModelDrawable& drawable = m_drawables[i];
        if (drawable.id.isEmpty() || drawableIds.contains(drawable.id)) {
            diagnostics.append({CModelDiagnostic::Error, QStringLiteral("drawable_id"),
                                 QStringLiteral("ArtMesh ID 为空或重复"), i});
        }
        drawableIds.insert(drawable.id);
        if (drawable.vertexCount < 0 || drawable.vertexCount > 1000000) {
            diagnostics.append({CModelDiagnostic::Error, QStringLiteral("vertex_count"),
                                 QStringLiteral("ArtMesh 顶点数量无效"), i});
        }
        if (drawable.parentPartIndex != -1 && !inRange(drawable.parentPartIndex, m_parts.size())) {
            diagnostics.append({CModelDiagnostic::Error, QStringLiteral("drawable_part"),
                                 QStringLiteral("ArtMesh 所属 Part 越界"), i});
        }
        if (drawable.parentDeformerIndex != -1 && !inRange(drawable.parentDeformerIndex, m_deformers.size())) {
            diagnostics.append({CModelDiagnostic::Error, QStringLiteral("drawable_deformer"),
                                 QStringLiteral("ArtMesh 所属 Deformer 越界"), i});
        }
        for (quint16 index : drawable.indices) {
            if (index >= drawable.vertexCount || index > 32767) {
                diagnostics.append({CModelDiagnostic::Error, QStringLiteral("index_range"),
                                     QStringLiteral("ArtMesh 三角形索引越界"), i});
                break;
            }
        }
        if (!drawable.indices.isEmpty() && drawable.indices.size() % 3 != 0) {
            diagnostics.append({CModelDiagnostic::Error, QStringLiteral("index_triangle_count"),
                                 QStringLiteral("ArtMesh 索引数量不是三角形的倍数"), i});
        }
        QSet<int> maskIndices;
        for (int mask : drawable.maskIndices) {
            if (!inRange(mask, m_drawables.size())) {
                diagnostics.append({CModelDiagnostic::Error, QStringLiteral("mask_range"),
                                     QStringLiteral("Clipping mask 索引越界"), i});
            } else if (mask == i) {
                diagnostics.append({CModelDiagnostic::Error, QStringLiteral("mask_self"),
                                     QStringLiteral("ArtMesh 不能引用自身作为 Clipping mask"), i});
            } else if (maskIndices.contains(mask)) {
                diagnostics.append({CModelDiagnostic::Error, QStringLiteral("mask_duplicate"),
                                     QStringLiteral("Clipping mask 引用重复"), i});
            }
            maskIndices.insert(mask);
        }
        if (!drawable.uvs.isEmpty() && drawable.uvs.size() != drawable.vertexCount * 2) {
            diagnostics.append({CModelDiagnostic::Error, QStringLiteral("uv_count"),
                                 QStringLiteral("UV 数量与顶点数量不一致"), i});
        }
        if (drawable.vertexCount > 0 && drawable.uvs.isEmpty()) {
            diagnostics.append({CModelDiagnostic::Error, QStringLiteral("uv_missing"),
                                 QStringLiteral("ArtMesh 缺少 UV 数据"), i});
        }
        if (drawable.vertexCount > 0 && drawable.indices.isEmpty()) {
            diagnostics.append({CModelDiagnostic::Error, QStringLiteral("index_missing"),
                                 QStringLiteral("ArtMesh 缺少三角形索引"), i});
        }
        for (float uv : drawable.uvs) {
            if (!std::isfinite(uv)) {
                diagnostics.append({CModelDiagnostic::Error, QStringLiteral("uv_nan"),
                                     QStringLiteral("ArtMesh UV 包含非有限数值"), i});
                break;
            }
        }
        if (!std::isfinite(drawable.opacity) || drawable.opacity < -0.001f || drawable.opacity > 1.001f) {
            diagnostics.append({CModelDiagnostic::Error, QStringLiteral("opacity_range"),
                                 QStringLiteral("ArtMesh 默认透明度超出 0 到 1 范围"), i});
        }
        if (!std::isfinite(drawable.drawOrder)) {
            diagnostics.append({CModelDiagnostic::Error, QStringLiteral("draw_order_nan"),
                                 QStringLiteral("ArtMesh 默认绘制顺序不是有限数值"), i});
        }
        for (const CModelKeyform& keyform : drawable.keyforms) {
            if (!keyform.positions.isEmpty() && keyform.positions.size() != drawable.vertexCount * 2) {
                diagnostics.append({CModelDiagnostic::Error, QStringLiteral("keyform_position_count"),
                                     QStringLiteral("关键形顶点数量与 ArtMesh 不一致"), i});
                break;
            }
            if (!std::isfinite(keyform.opacity) || keyform.opacity < -0.001f || keyform.opacity > 1.001f ||
                !std::isfinite(keyform.drawOrder)) {
                diagnostics.append({CModelDiagnostic::Error, QStringLiteral("keyform_nan"),
                                     QStringLiteral("关键形透明度或绘制顺序无效"), i});
                break;
            }
            bool finitePositions = true;
            for (float value : keyform.positions) {
                if (!std::isfinite(value)) { finitePositions = false; break; }
            }
            if (!finitePositions) {
                diagnostics.append({CModelDiagnostic::Error, QStringLiteral("keyform_position_nan"),
                                     QStringLiteral("关键形顶点包含非有限数值"), i});
                break;
            }
        }
    }

    for (auto it = m_parameterValues.cbegin(); it != m_parameterValues.cend(); ++it) {
        bool found = false;
        for (const CModelParameter& parameter : m_parameters) {
            if (parameter.id != it.key()) continue;
            found = true;
            if (!std::isfinite(it.value()) || it.value() < parameter.minimum || it.value() > parameter.maximum) {
                diagnostics.append({CModelDiagnostic::Error, QStringLiteral("parameter_override_range"),
                                     QStringLiteral("参数覆盖值无效：%1").arg(it.key()), -1});
            }
            break;
        }
        if (!found) {
            diagnostics.append({CModelDiagnostic::Error, QStringLiteral("parameter_override_id"),
                                 QStringLiteral("参数覆盖引用了不存在的参数：%1").arg(it.key()), -1});
        }
    }
    for (auto it = m_drawableOpacityOverrides.cbegin(); it != m_drawableOpacityOverrides.cend(); ++it) {
        bool found = false;
        for (const CModelDrawable& drawable : m_drawables) if (drawable.id == it.key()) { found = true; break; }
        if (!found) {
            diagnostics.append({CModelDiagnostic::Error, QStringLiteral("opacity_override_id"),
                                 QStringLiteral("透明度覆盖引用了不存在的 ArtMesh：%1").arg(it.key()), -1});
        } else if (!std::isfinite(it.value()) || it.value() < 0.0f || it.value() > 1.0f) {
            diagnostics.append({CModelDiagnostic::Error, QStringLiteral("opacity_override_range"),
                                 QStringLiteral("透明度覆盖值无效：%1").arg(it.key()), -1});
        }
    }
    for (auto it = m_drawableDrawOrderOverrides.cbegin(); it != m_drawableDrawOrderOverrides.cend(); ++it) {
        bool found = false;
        for (const CModelDrawable& drawable : m_drawables) if (drawable.id == it.key()) { found = true; break; }
        if (!found) {
            diagnostics.append({CModelDiagnostic::Error, QStringLiteral("draw_order_override_id"),
                                 QStringLiteral("绘制顺序覆盖引用了不存在的 ArtMesh：%1").arg(it.key()), -1});
        } else if (it.value() < 0 || it.value() > 1000) {
            diagnostics.append({CModelDiagnostic::Error, QStringLiteral("draw_order_override_range"),
                                 QStringLiteral("绘制顺序覆盖值无效：%1").arg(it.key()), -1});
        }
    }
    for (auto meshIt = m_drawableKeyformPositionOverrides.cbegin();
         meshIt != m_drawableKeyformPositionOverrides.cend(); ++meshIt) {
        const CModelDrawable* drawable = nullptr;
        for (const CModelDrawable& candidate : m_drawables) {
            if (candidate.id == meshIt.key()) { drawable = &candidate; break; }
        }
        if (!drawable) {
            diagnostics.append({CModelDiagnostic::Error, QStringLiteral("keyform_override_id"),
                                 QStringLiteral("关键形覆盖引用了不存在的 ArtMesh：%1").arg(meshIt.key()), -1});
            continue;
        }
        if (meshIt.value().size() > drawable->keyforms.size()) {
            diagnostics.append({CModelDiagnostic::Error, QStringLiteral("keyform_override_count"),
                                 QStringLiteral("关键形覆盖数量超出 ArtMesh：%1").arg(meshIt.key()), -1});
            continue;
        }
        for (const QVector<float>& positions : meshIt.value()) {
            if (positions.isEmpty()) continue;
            if (positions.size() != drawable->vertexCount * 2) {
                diagnostics.append({CModelDiagnostic::Error, QStringLiteral("keyform_override_position_count"),
                                     QStringLiteral("关键形覆盖顶点数量无效：%1").arg(meshIt.key()), -1});
                break;
            }
            bool finite = true;
            for (float value : positions) if (!std::isfinite(value)) { finite = false; break; }
            if (!finite) {
                diagnostics.append({CModelDiagnostic::Error, QStringLiteral("keyform_override_nan"),
                                     QStringLiteral("关键形覆盖包含非有限数值：%1").arg(meshIt.key()), -1});
                break;
            }
        }
    }
    for (auto indexIt = m_drawableIndexOverrides.cbegin(); indexIt != m_drawableIndexOverrides.cend(); ++indexIt) {
        const CModelDrawable* drawable = nullptr;
        for (const CModelDrawable& candidate : m_drawables) {
            if (candidate.id == indexIt.key()) { drawable = &candidate; break; }
        }
        if (!drawable) {
            diagnostics.append({CModelDiagnostic::Error, QStringLiteral("index_override_id"),
                                 QStringLiteral("索引覆盖引用了不存在的 ArtMesh：%1").arg(indexIt.key()), -1});
            continue;
        }
        if (indexIt.value().isEmpty() || indexIt.value().size() % 3 != 0 || indexIt.value().size() > 3000000) {
            diagnostics.append({CModelDiagnostic::Error, QStringLiteral("index_override_count"),
                                 QStringLiteral("索引覆盖数量无效：%1").arg(indexIt.key()), -1});
            continue;
        }
        if (!CMeshTopology::IsValid(indexIt.value(), drawable->vertexCount)) {
            diagnostics.append({CModelDiagnostic::Error, QStringLiteral("index_override_range"),
                                 QStringLiteral("索引覆盖越界：%1").arg(indexIt.key()), -1});
        }
    }
    for (auto maskIt = m_drawableMaskIndexOverrides.cbegin(); maskIt != m_drawableMaskIndexOverrides.cend(); ++maskIt) {
        int drawableIndex = -1;
        for (int i = 0; i < m_drawables.size(); ++i) {
            if (m_drawables[i].id == maskIt.key()) { drawableIndex = i; break; }
        }
        if (drawableIndex < 0) {
            diagnostics.append({CModelDiagnostic::Error, QStringLiteral("mask_override_id"),
                                 QStringLiteral("蒙版覆盖引用了不存在的 ArtMesh：%1").arg(maskIt.key()), -1});
            continue;
        }
        QSet<int> unique;
        for (int maskIndex : maskIt.value()) {
            if (!inRange(maskIndex, m_drawables.size()) || maskIndex == drawableIndex) {
                diagnostics.append({CModelDiagnostic::Error, QStringLiteral("mask_override_range"),
                                     QStringLiteral("蒙版覆盖索引无效：%1").arg(maskIt.key()), -1});
                break;
            }
            if (unique.contains(maskIndex)) {
                diagnostics.append({CModelDiagnostic::Error, QStringLiteral("mask_override_duplicate"),
                                     QStringLiteral("蒙版覆盖存在重复引用：%1").arg(maskIt.key()), -1});
                break;
            }
            unique.insert(maskIndex);
        }
    }
    for (auto parentIt = m_deformerParentOverrides.cbegin(); parentIt != m_deformerParentOverrides.cend(); ++parentIt) {
        int deformerIndex = -1;
        for (int i = 0; i < m_deformers.size(); ++i) {
            if (m_deformers[i].id == parentIt.key()) { deformerIndex = i; break; }
        }
        const int parentIndex = parentIt.value();
        if (deformerIndex < 0 || parentIndex < -1 || parentIndex >= m_deformers.size() || parentIndex == deformerIndex ||
            (parentIndex >= 0 && m_deformers[parentIndex].parentPartIndex != m_deformers[deformerIndex].parentPartIndex)) {
            diagnostics.append({CModelDiagnostic::Error, QStringLiteral("deformer_parent_override"),
                                 QStringLiteral("变形器父级覆盖无效：%1").arg(parentIt.key()), -1});
            continue;
        }
        QSet<int> visited;
        int current = parentIndex;
        while (current >= 0 && current < m_deformers.size()) {
            if (current == deformerIndex || visited.contains(current)) {
                diagnostics.append({CModelDiagnostic::Error, QStringLiteral("deformer_parent_cycle"),
                                     QStringLiteral("变形器父级覆盖形成循环：%1").arg(parentIt.key()), -1});
                break;
            }
            visited.insert(current);
            current = DeformerParentIndex(m_deformers[current].id);
        }
    }
    for (auto invertIt = m_drawableMaskInvertedOverrides.cbegin(); invertIt != m_drawableMaskInvertedOverrides.cend(); ++invertIt) {
        bool found = false;
        for (const CModelDrawable& drawable : m_drawables) {
            if (drawable.id != invertIt.key()) continue;
            found = true;
            break;
        }
        if (!found) {
            diagnostics.append({CModelDiagnostic::Error, QStringLiteral("mask_inverted_id"),
                                 QStringLiteral("反向蒙版覆盖引用了不存在的 ArtMesh：%1").arg(invertIt.key()), -1});
        }
    }
    for (const QString& key : m_lockedKeys) {
        const int separator = key.indexOf(QLatin1Char('|'));
        if (separator <= 0 || separator >= key.size() - 1) {
            diagnostics.append({CModelDiagnostic::Error, QStringLiteral("locked_key"),
                                 QStringLiteral("锁定键格式无效"), -1});
        }
    }

    for (int i = 0; i < m_drawOrderGroups.size(); ++i) {
        const CModelDrawOrderGroup& group = m_drawOrderGroups[i];
        if (group.minimumDrawOrder < 0 || group.maximumDrawOrder > 1000 ||
            group.minimumDrawOrder > group.maximumDrawOrder) {
            diagnostics.append({CModelDiagnostic::Error, QStringLiteral("draw_order_group_range"),
                                 QStringLiteral("Draw Order Group 范围无效"), i});
        }
        if (group.objectCount < 0 || group.totalObjectCount < group.objectCount ||
            group.objectCount != group.objects.size() || !group.objectsValid) {
            diagnostics.append({CModelDiagnostic::Error, QStringLiteral("draw_order_group_objects"),
                                 QStringLiteral("Draw Order Group 成员切片无效"), i});
            continue;
        }
        QSet<QString> members;
        for (const CModelDrawOrderGroupObject& object : group.objects) {
            if (object.type == 0) {
                if (!inRange(object.index, m_drawables.size())) {
                    diagnostics.append({CModelDiagnostic::Error, QStringLiteral("draw_order_group_drawable"),
                                         QStringLiteral("Draw Order Group ArtMesh 索引越界"), i});
                    continue;
                }
                if (object.selfIndex != -1) {
                    diagnostics.append({CModelDiagnostic::Error, QStringLiteral("draw_order_group_self"),
                                         QStringLiteral("ArtMesh 的组索引应为空"), i});
                }
                const QString key = QStringLiteral("drawable:%1").arg(object.index);
                if (members.contains(key)) {
                    diagnostics.append({CModelDiagnostic::Error, QStringLiteral("draw_order_group_duplicate"),
                                         QStringLiteral("Draw Order Group 成员重复"), i});
                }
                members.insert(key);
            } else if (object.type == 1) {
                if (!inRange(object.index, m_parts.size()) ||
                    !inRange(object.selfIndex, m_drawOrderGroups.size())) {
                    diagnostics.append({CModelDiagnostic::Error, QStringLiteral("draw_order_group_part"),
                                         QStringLiteral("Draw Order Group Part 或组索引越界"), i});
                    continue;
                }
                const QString key = QStringLiteral("part:%1").arg(object.index);
                if (members.contains(key)) {
                    diagnostics.append({CModelDiagnostic::Error, QStringLiteral("draw_order_group_duplicate"),
                                         QStringLiteral("Draw Order Group 成员重复"), i});
                }
                members.insert(key);
            } else {
                diagnostics.append({CModelDiagnostic::Error, QStringLiteral("draw_order_group_type"),
                                     QStringLiteral("Draw Order Group 成员类型无效"), i});
            }
        }
    }
    return diagnostics;
}

}
