#include "cmodeldocument.h"
#include "cmocmaterialbuilder.h"
#include "cmocruntimebuilder.h"
#include "cmocdeformerorder.h"
#include "cmeshtopology.h"
#include "cdrawableflags.h"
#include "csectiontable.h"
#include "ccanvasgeometry.h"
#include "ckeyformbindinggrid.h"
#include "cmocpreviewbuilder.h"
#include "cparentgraph.h"
#include "util/cfilereader.h"
#include "struct/cart_mesh_keyforms.h"
#include <QHash>
#include <QCryptographicHash>
#include <QSet>
#include <QtGlobal>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <exception>
#include <type_traits>
#include <limits>
#include <utility>

namespace live2dstruct {

namespace {
void setError(QString* error, const QString& value)
{
    if (error) *error = value;
}

bool sameBlendShapeEndpoint(const CModelBlendShapeGeometryEntry& a, const CModelBlendShapeGeometryEntry& b)
{
    return a.meshId == b.meshId && a.shapeIndex == b.shapeIndex &&
        a.bindingOffset == b.bindingOffset && a.keyformOffset == b.keyformOffset;
}

bool samePositions(const QVector<float>& a, const QVector<float>& b)
{
    return a.size() == b.size() && (a.isEmpty() ||
        std::memcmp(a.constData(), b.constData(), size_t(a.size()) * sizeof(float)) == 0);
}

CModelBlendShapeGeometryState freezeBlendShapeGeometry(const CModelBlendShapeGeometryState& state)
{
    CModelBlendShapeGeometryState result;
    result.reserve(state.size());
    for (const auto& entry : state) {
        auto copy = entry;
        copy.meshId = QString(entry.meshId.constData(), entry.meshId.size());
        copy.sourceDigest = QByteArray(entry.sourceDigest.constData(), entry.sourceDigest.size());
        copy.deltas = QVector<float>(entry.deltas.cbegin(), entry.deltas.cend());
        result.append(std::move(copy));
    }
    return result;
}

bool inRange(int value, int size)
{
    return value >= 0 && value < size;
}

bool validSlice(int begin, int count, int size)
{
    return begin >= 0 && count >= 0 && begin <= size && count <= size - begin;
}

CModelParameterMetadata sourceMetadata(const CModelParameter& parameter)
{
    return {parameter.id, parameter.minimum, parameter.maximum, parameter.defaultValue,
            parameter.decimalPlaces, parameter.repeat};
}

QVariantMap metadataVariant(const CModelParameterMetadata& metadata)
{
    return {{QStringLiteral("displayName"), metadata.displayName},
            {QStringLiteral("minimum"), metadata.minimum}, {QStringLiteral("maximum"), metadata.maximum},
            {QStringLiteral("defaultValue"), metadata.defaultValue},
            {QStringLiteral("decimalPlaces"), metadata.decimalPlaces}, {QStringLiteral("repeat"), metadata.repeat}};
}

bool validMetadata(const CModelParameterMetadata& metadata)
{
    return !metadata.displayName.contains(QChar::Null) && std::isfinite(metadata.minimum) &&
        std::isfinite(metadata.maximum) && std::isfinite(metadata.defaultValue) &&
        metadata.minimum <= metadata.maximum && metadata.defaultValue >= metadata.minimum &&
        metadata.defaultValue <= metadata.maximum && metadata.decimalPlaces >= 0 && metadata.decimalPlaces <= 9;
}

bool hasMetadataState(const QVariantMap& state)
{
    return state.contains(QStringLiteral("hasMetadataOverride")) || state.contains(QStringLiteral("hasValueOverride")) ||
           state.contains(QStringLiteral("value"));
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
        return std::isfinite(*target) && (number == 0.0 || *target != 0.0f);
    };
    if (!readFloatField(QStringLiteral("minimum"), &parsed.minimum) ||
        !readFloatField(QStringLiteral("maximum"), &parsed.maximum) ||
        !readFloatField(QStringLiteral("defaultValue"), &parsed.defaultValue)) return false;
    if (map.contains(QStringLiteral("decimalPlaces"))) {
        bool ok = false;
        const double decimalPlaces = map.value(QStringLiteral("decimalPlaces")).toDouble(&ok);
        if (!ok || !std::isfinite(decimalPlaces) || decimalPlaces < 0 || decimalPlaces > 9 ||
            std::floor(decimalPlaces) != decimalPlaces) return false;
        parsed.decimalPlaces = static_cast<int>(decimalPlaces);
    }
    if (map.contains(QStringLiteral("repeat"))) {
        if (!map.value(QStringLiteral("repeat")).canConvert<bool>()) return false;
        parsed.repeat = map.value(QStringLiteral("repeat")).toBool();
    }
    *metadata = parsed;
    return true;
}
}

bool CModelRotationKeyform::IsValid() const
{
    return std::isfinite(opacity) && opacity >= 0.0f && opacity <= 1.0f &&
           std::isfinite(angle) && std::isfinite(originX) && std::isfinite(originY) && std::isfinite(scale);
}

void CModelDocument::Clear()
{
    ++m_revision;
    m_loaded = false;
    m_sourcePath.clear();
    m_sourceBytes.clear();
    m_sourceDigest.clear();
    m_workingBaseBytes.clear();
    m_maskSource.reset();
    m_invalidMaskSlices.clear();
    m_structure = {};
    m_hasMeshStructure = false;
    m_hasMaterialStructure = false;
    m_drawableMaterialOverrides.clear();
    m_textureSources.clear();
    m_textureWorkspace = {};
    m_textureWorkspaceFingerprint.clear();
    m_artwork = {};
    m_artworkFingerprint.clear();
    m_sourceDeformerParents.clear();
    m_sourceDeformerIds.clear();
    m_hasDeformerStructure = false;
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
    m_displayNames = {};
    m_parameterOrganization = {};
    m_geometryObjectPairs = {};
    m_drawableKeyformPositionOverrides.clear();
    m_drawableIndexOverrides.clear();
    m_drawableMaskIndexOverrides.clear();
    m_deformerParentOverrides.clear();
    m_deformerRotationKeyformOverrides.clear();
    m_deformerWarpKeyformOverrides.clear();
    m_drawableMaskInvertedOverrides.clear();
    m_blendShapeGeometry.clear();
    m_lockedKeys.clear();
    ClearEditHistory();
}

bool CModelDocument::Load(const QString& mocPath, QString* error)
{
    CReader reader(mocPath);
    CModelDocument candidate;
    if (!candidate.LoadInternal(reader, mocPath, error)) return false;
    CommitCandidate(std::move(candidate));
    return true;
}

bool CModelDocument::LoadBytes(const QByteArray& bytes, const QString& sourcePath, QString* error)
{
    auto reader = CReader::FromBytes(bytes);
    CModelDocument candidate;
    if (!candidate.LoadInternal(reader, sourcePath, error)) return false;
    CommitCandidate(std::move(candidate));
    return true;
}

void CModelDocument::CommitCandidate(CModelDocument&& candidate) noexcept
{
    static_assert(std::is_nothrow_move_assignable_v<CModelDocument>);
    if (this == &candidate) return;
    const quint64 revision = m_revision + 1;
    *this = std::move(candidate);
    m_revision = revision;
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

bool CModelDocument::DescribeKeyformGrid(const QString& id, const QString& type, CKeyformGridSnapshot& snapshot,
                                        QString* error) const
{
    const auto fail = [&](const QString& message) { setError(error, message); return false; };
    if (!m_loaded || !m_keyformBindings) return fail(QStringLiteral("尚未加载关键形绑定"));
    CKeyformGridSnapshot candidate;
    if (type == QLatin1String("artmesh")) candidate.m_type = CKeyformBindingGrid::ArtMesh;
    else if (type == QLatin1String("warp")) candidate.m_type = CKeyformBindingGrid::Warp;
    else if (type == QLatin1String("rotation")) candidate.m_type = CKeyformBindingGrid::Rotation;
    else return fail(QStringLiteral("对象类型没有关键形绑定：%1").arg(type));
    const bool exists = candidate.m_type == CKeyformBindingGrid::ArtMesh
        ? std::any_of(m_drawables.cbegin(), m_drawables.cend(), [&](const auto& value) { return value.id == id; })
        : std::any_of(m_deformers.cbegin(), m_deformers.cend(), [&](const auto& value) {
            return value.id == id && ((candidate.m_type == CKeyformBindingGrid::Warp && value.type == CModelDeformer::Warp) ||
                                     (candidate.m_type == CKeyformBindingGrid::Rotation && value.type == CModelDeformer::Rotation));
        });
    if (!exists) return fail(QStringLiteral("未找到关键形对象：%1").arg(id));
    candidate.objectId = id;
    candidate.objectType = type;
    candidate.revision = m_revision;
    candidate.sourceDigest = m_sourceDigest;
    candidate.m_bindings = m_keyformBindings;
    const auto description = m_keyformBindings->Describe(candidate.m_type, id);
    candidate.keyformCount = description.keyformCount;
    candidate.expectedKeyformCount = description.expectedKeyformCount;
    candidate.axesValid = description.axesValid;
    candidate.diagnostic = description.diagnostic;
    QSet<QString> required;
    for (const auto& axis : description.axes) required.insert(axis.parameterId);
    QHash<QString, CModelParameterMetadata> metadataById;
    for (const auto& parameter : m_parameters) if (required.contains(parameter.id)) {
        auto metadata = m_parameterMetadataOverrides.value(parameter.id, sourceMetadata(parameter));
        if (metadata.displayName.isEmpty()) metadata.displayName = parameter.id;
        metadataById.insert(parameter.id, std::move(metadata));
    }
    for (const auto& axis : description.axes) {
        const auto found = metadataById.constFind(axis.parameterId);
        if (found == metadataById.cend()) return fail(QStringLiteral("关键形引用的参数不存在：%1").arg(axis.parameterId));
        const auto& metadata = *found;
        candidate.axes.append({axis.bindingIndex, axis.parameterId, metadata.displayName, axis.parameterType,
                               axis.keys, metadata.minimum, metadata.maximum});
        candidate.m_ranges.insert(axis.parameterId, {metadata.minimum, metadata.maximum});
    }
    snapshot = std::move(candidate);
    if (error) error->clear();
    return true;
}

bool CModelDocument::BuildPreviewMoc(QByteArray& bytes, QString* error) const
{
    if (error) error->clear();
    if (!m_loaded) {
        setError(error, QStringLiteral("尚未加载模型快照"));
        return false;
    }
    QByteArray candidate;
    if (m_drawableKeyformPositionOverrides.isEmpty() && m_deformerWarpKeyformOverrides.isEmpty() &&
        m_deformerRotationKeyformOverrides.isEmpty() && m_deformerParentOverrides.isEmpty() &&
        m_drawableMaskIndexOverrides.isEmpty() && m_drawableMaskInvertedOverrides.isEmpty() &&
        m_parameterMetadataOverrides.isEmpty()) {
        if (!CMocMaterialBuilder::Build(m_workingBaseBytes, m_drawableMaterialOverrides, candidate, error)) return false;
    } else {
        if (!m_previewBuilder) {
            setError(error, m_previewError);
            return false;
        }
        CMocPreviewBuilder::Overrides overrides;
        overrides.artMeshPositions = m_drawableKeyformPositionOverrides;
        overrides.warpKeyforms = m_deformerWarpKeyformOverrides;
        overrides.rotationKeyforms = m_deformerRotationKeyformOverrides;
        overrides.deformerParents = m_deformerParentOverrides;
        overrides.drawableMasks = m_drawableMaskIndexOverrides;
        overrides.drawableMaskInverted = m_drawableMaskInvertedOverrides;
        overrides.parameterMetadata = m_parameterMetadataOverrides;
        QByteArray preview;
        if (!m_previewBuilder->BuildOverrides(overrides, preview, error) ||
            !CMocMaterialBuilder::Build(preview, m_drawableMaterialOverrides, candidate, error)) return false;
    }
    for (const auto& entry : m_blendShapeGeometry) {
        CMocBlendShapeGeometryBuilder::Key key{entry.meshId, entry.shapeIndex, entry.bindingOffset, entry.keyformOffset};
        CMocBlendShapeGeometryBuilder::Frame current;
        if (!CMocBlendShapeGeometryBuilder::Capture(candidate, key, current, error)) return false;
        if (current.deltas.size() != entry.deltas.size()) {
            setError(error, QStringLiteral("融合几何状态与当前模型结构不匹配"));
            return false;
        }
        if (samePositions(current.deltas, entry.deltas)) continue;
        CMocBlendShapeGeometryBuilder::Result result;
        if (!CMocBlendShapeGeometryBuilder::Build(candidate, {current, entry.deltas}, result, error)) return false;
        candidate = std::move(result.bytes);
    }
    bytes = std::move(candidate);
    return true;
}

bool CModelDocument::SetBlendShapeGeometry(const CModelBlendShapeGeometryState& state, QString* error)
{
    if (error) error->clear();
    if (!m_loaded) return setError(error, QStringLiteral("尚未加载模型快照")), false;
    if (state.size() > 100000) return setError(error, QStringLiteral("融合几何条目数量超出限制")), false;
    const QByteArray digest = QCryptographicHash::hash(m_workingBaseBytes, QCryptographicHash::Sha256);
    qint64 total = 0;
    QSet<QString> endpoints;
    for (const auto& entry : state) {
        const QByteArray id = entry.meshId.toUtf8();
        if (id.isEmpty() || id.size() >= 64 || id.contains('\0') || QString::fromUtf8(id) != entry.meshId ||
            entry.sourceDigest != digest || entry.shapeIndex < 0 || entry.bindingOffset < 0 || entry.keyformOffset < 0 ||
            entry.sourceKeyformIndex < 0 || entry.deltas.isEmpty() || (entry.deltas.size() & 1) ||
            std::any_of(entry.deltas.cbegin(), entry.deltas.cend(), [](float value) { return !std::isfinite(value); }))
            return setError(error, QStringLiteral("BlendShape 几何状态条目无效")), false;
        total += entry.deltas.size();
        if (total > 2000000) return setError(error, QStringLiteral("融合几何坐标总量超出限制")), false;
        const QString endpoint = QStringLiteral("%1|%2|%3|%4").arg(entry.meshId).arg(entry.shapeIndex).arg(entry.bindingOffset).arg(entry.keyformOffset);
        if (endpoints.contains(endpoint)) return setError(error, QStringLiteral("融合几何状态包含重复端点")), false;
        endpoints.insert(endpoint);
        CMocBlendShapeGeometryBuilder::Frame frame;
        if (!CMocBlendShapeGeometryBuilder::Capture(m_workingBaseBytes,
                {entry.meshId, entry.shapeIndex, entry.bindingOffset, entry.keyformOffset}, frame, error) ||
            frame.sourceKeyformIndex != entry.sourceKeyformIndex || frame.deltas.size() != entry.deltas.size()) return false;
    }
    CModelDocument candidate = *this;
    candidate.m_blendShapeGeometry = freezeBlendShapeGeometry(state);
    QByteArray preview;
    if (!candidate.BuildPreviewMoc(preview, error)) return false;
    if (candidate.m_blendShapeGeometry == m_blendShapeGeometry) return true;
    m_blendShapeGeometry = std::move(candidate.m_blendShapeGeometry);
    ++m_revision;
    return true;
}

bool CModelDocument::CaptureBlendShapeGeometry(const CMocBlendShapeGeometryBuilder::Key& key,
                                               CModelBlendShapeGeometryEntry& result, QString* error) const
{
    if (error) error->clear();
    if (!m_loaded) return setError(error, QStringLiteral("尚未加载模型快照")), false;
    CMocBlendShapeGeometryBuilder::Frame frame;
    if (!CMocBlendShapeGeometryBuilder::Capture(m_workingBaseBytes, key, frame, error)) return false;
    for (const auto& entry : m_blendShapeGeometry)
        if (entry.meshId == key.meshId && entry.shapeIndex == key.shapeIndex &&
            entry.bindingOffset == key.bindingOffset && entry.keyformOffset == key.keyformOffset) {
            if (entry.sourceDigest != frame.sourceDigest || entry.sourceKeyformIndex != frame.sourceKeyformIndex ||
                entry.deltas.size() != frame.deltas.size()) {
                setError(error, QStringLiteral("融合几何状态来源已失效"));
                return false;
            }
            result = entry;
            return true;
        }
    result = {QString(key.meshId.constData(), key.meshId.size()), key.shapeIndex, key.bindingOffset, key.keyformOffset,
              frame.sourceKeyformIndex, frame.sourceDigest, frame.deltas};
    return true;
}

bool CModelDocument::MakeBlendShapeGeometryEdit(const CModelBlendShapeGeometryEntry& before,
                                                const QVector<float>& deltas, CModelEdit& edit,
                                                QString* error) const
{
    if (error) error->clear();
    CModelBlendShapeGeometryEntry current;
    if (!CaptureBlendShapeGeometry({before.meshId, before.shapeIndex, before.bindingOffset, before.keyformOffset}, current, error) ||
        !(current == before) || deltas.size() != before.deltas.size() || deltas.isEmpty() ||
        std::any_of(deltas.cbegin(), deltas.cend(), [](float value) { return !std::isfinite(value); })) {
        setError(error, QStringLiteral("融合几何来源已改变或坐标无效"));
        return false;
    }
    if (!CanEditBlendShapeGeometry(before.meshId, error)) return false;
    CModelBlendShapeGeometryState next = m_blendShapeGeometry;
    CMocBlendShapeGeometryBuilder::Frame base;
    if (!CMocBlendShapeGeometryBuilder::Capture(m_workingBaseBytes,
            {before.meshId, before.shapeIndex, before.bindingOffset, before.keyformOffset}, base, error)) return false;
    const auto match = std::find_if(next.begin(), next.end(), [&](const auto& value) {
        return sameBlendShapeEndpoint(value, before);
    });
    if (samePositions(base.deltas, deltas)) {
        if (match != next.end()) next.erase(match);
    } else {
        CModelBlendShapeGeometryEntry updated = before;
        updated.sourceDigest = base.sourceDigest;
        updated.sourceKeyformIndex = base.sourceKeyformIndex;
        updated.deltas = deltas;
        if (match == next.end()) next.append(std::move(updated)); else *match = std::move(updated);
    }
    if (next == m_blendShapeGeometry) return setError(error, QStringLiteral("融合几何没有变化")), false;
    CModelDocument candidate = *this;
    if (!candidate.SetBlendShapeGeometry(next, error)) return false;
    edit = {QStringLiteral("blendShapeGeometry"), QStringLiteral("blendshape"), CModelEdit::BlendShapeGeometry,
            QVariant::fromValue(m_blendShapeGeometry), QVariant::fromValue(candidate.m_blendShapeGeometry), -1, false};
    return true;
}

bool CModelDocument::BuildBlendShapeGeometryPreview(const CMocBlendShapeGeometryBuilder::Key& key,
                                                    const QVector<float>& deltas, QByteArray& bytes,
                                                    QString* error) const
{
    CModelBlendShapeGeometryEntry entry;
    if (!CaptureBlendShapeGeometry(key, entry, error)) return false;
    CMocBlendShapeGeometryBuilder::Frame base;
    if (!CMocBlendShapeGeometryBuilder::Capture(m_workingBaseBytes, key, base, error)) return false;
    auto state = m_blendShapeGeometry;
    const auto match = std::find_if(state.begin(), state.end(), [&](const auto& value) {
        return sameBlendShapeEndpoint(value, entry);
    });
    if (samePositions(base.deltas, deltas)) {
        if (match != state.end()) state.erase(match);
    } else {
        entry.deltas = deltas;
        if (match == state.end()) state.append(std::move(entry)); else *match = std::move(entry);
    }
    CModelDocument candidate = *this;
    return candidate.SetBlendShapeGeometry(state, error) && candidate.BuildPreviewMoc(bytes, error);
}

bool CModelDocument::BuildRuntimeMoc(QByteArray& bytes, QString* error) const
{
    QByteArray authoring;
    if (!BuildPreviewMoc(authoring, error)) return false;
    CMocRuntimeBuilder::Overrides overrides;
    overrides.visibility = VisibilityState();
    overrides.drawableOpacity = m_drawableOpacityOverrides;
    overrides.drawableDrawOrder = m_drawableDrawOrderOverrides;
    overrides.drawableIndices = m_drawableIndexOverrides;
    QByteArray runtime;
    return CMocRuntimeBuilder::Build(authoring, overrides, runtime, error) &&
           CMocDeformerOrder::Normalize(runtime, bytes, error);
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
    m_workingBaseBytes = m_sourceBytes;
    m_sourceDigest = QCryptographicHash::hash(m_sourceBytes, QCryptographicHash::Sha256);
    for (const auto* deformer : table.deformers) {
        m_sourceDeformerParents.insert(deformer->ids, deformer->parent_deformer_indices);
        m_sourceDeformerIds.insert(deformer->ids);
    }
    auto bindings = std::make_shared<CKeyformBindingGrid>();
    if (!bindings->Build(table, error)) return false;
    m_keyformBindings = std::move(bindings);
    auto preview = std::make_shared<CMocPreviewBuilder>();
    if (preview->Initialize(m_sourceBytes, m_sourceDeformerParents, &m_previewError)) m_previewBuilder = std::move(preview);
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
                const quint16 index = table.position_indices->indices[source->position_index_sources_begin_indices + i];
                if (index >= source->vertex_counts) continue;
                drawable.indices.append(index);
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

    CaptureMaskSource(table);
    m_loaded = true;
    return true;
}

bool CModelDocument::SetPartVisible(const QString& id, bool visible)
{
    for (CModelPart& part : m_parts) {
        if (part.id != id) continue;
        part.visible = visible;
        ++m_revision;
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
    ++m_revision;
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

QVariantMap CModelDocument::ParameterMetadataState(const QString& id) const
{
    int matches = 0;
    for (const auto& parameter : m_parameters) if (parameter.id == id) ++matches;
    if (id.isEmpty() || matches != 1) return {};
    auto state = metadataVariant(ParameterMetadata(id));
    state.insert(QStringLiteral("hasMetadataOverride"), m_parameterMetadataOverrides.contains(id));
    state.insert(QStringLiteral("hasValueOverride"), m_parameterValues.contains(id));
    state.insert(QStringLiteral("value"), ParameterValue(id));
    return state;
}

bool CModelDocument::MakeParameterMetadataEdit(const QString& id, const CModelParameterMetadata& metadata,
                                               CModelEdit& edit, QString* error) const
{
    if (error) error->clear();
    const auto before = ParameterMetadataState(id);
    if (before.isEmpty()) {
        setError(error, QStringLiteral("参数不存在或 ID 重复：%1").arg(id));
        return false;
    }
    const auto current = ParameterMetadata(id);
    auto normalized = metadata;
    if (normalized.displayName.isEmpty()) normalized.displayName = current.displayName;
    if (!validMetadata(normalized) || !std::isfinite(ParameterValue(id))) {
        setError(error, QStringLiteral("参数范围、默认值、小数位或当前值无效"));
        return false;
    }
    CModelEdit command{id, QStringLiteral("parameter"), CModelEdit::ParameterMetadata, before, before, -1, false};
    if (!(normalized == current)) {
        const auto source = std::find_if(m_parameters.cbegin(), m_parameters.cend(),
                                         [&](const auto& parameter) { return parameter.id == id; });
        const float value = std::clamp(ParameterValue(id), normalized.minimum, normalized.maximum);
        auto after = metadataVariant(normalized);
        after.insert(QStringLiteral("hasMetadataOverride"), !(normalized == sourceMetadata(*source)));
        after.insert(QStringLiteral("hasValueOverride"), m_parameterValues.contains(id) || value != normalized.defaultValue);
        after.insert(QStringLiteral("value"), value);
        command.after = after;
    }
    edit = std::move(command);
    return true;
}

bool CModelDocument::ApplyParameterMetadataState(const QString& id, const QVariantMap& state)
{
    const auto current = ParameterMetadataState(id);
    if (current.isEmpty()) return false;
    for (const QString& field : {QStringLiteral("displayName"), QStringLiteral("minimum"), QStringLiteral("maximum"),
            QStringLiteral("defaultValue"), QStringLiteral("decimalPlaces"), QStringLiteral("repeat"),
            QStringLiteral("hasMetadataOverride"), QStringLiteral("hasValueOverride"), QStringLiteral("value")})
        if (!state.contains(field)) return false;
    if (state.value(QStringLiteral("hasMetadataOverride")).metaType().id() != QMetaType::Bool ||
        state.value(QStringLiteral("hasValueOverride")).metaType().id() != QMetaType::Bool) return false;
    CModelParameterMetadata metadata;
    if (!metadataFromVariant(state, ParameterMetadata(id), &metadata) || !validMetadata(metadata)) return false;
    bool validValue = false;
    const double number = state.value(QStringLiteral("value")).toDouble(&validValue);
    if (!validValue || !std::isfinite(number) || number < metadata.minimum || number > metadata.maximum) return false;
    const float value = static_cast<float>(number);
    if (number != 0.0 && value == 0.0f) return false;
    const bool hasMetadata = state.value(QStringLiteral("hasMetadataOverride")).toBool();
    const bool hasValue = state.value(QStringLiteral("hasValueOverride")).toBool();
    const auto source = std::find_if(m_parameters.cbegin(), m_parameters.cend(),
                                     [&](const auto& parameter) { return parameter.id == id; });
    if ((!hasMetadata && !(metadata == sourceMetadata(*source))) || (!hasValue && value != metadata.defaultValue)) return false;
    auto metadataOverrides = m_parameterMetadataOverrides;
    auto parameterValues = m_parameterValues;
    if (hasMetadata) metadataOverrides.insert(id, metadata);
    else metadataOverrides.remove(id);
    if (hasValue) parameterValues.insert(id, value);
    else parameterValues.remove(id);
    m_parameterMetadataOverrides.swap(metadataOverrides);
    m_parameterValues.swap(parameterValues);
    ++m_revision;
    return true;
}

bool CModelDocument::SetParameterMetadata(const QString& id, const CModelParameterMetadata& metadata)
{
    const CModelParameterMetadata base = ParameterMetadata(id);
    bool found = false;
    const CModelParameter* source = nullptr;
    for (const CModelParameter& parameter : m_parameters) {
        if (parameter.id == id) { found = true; source = &parameter; break; }
    }
    if (!found || !source || !validMetadata(metadata)) return false;
    float previousKey = -std::numeric_limits<float>::infinity();
    for (float key : source->keyValues) {
        if (!std::isfinite(key) || key < previousKey) return false;
        previousKey = key;
    }
    if (m_parameterValues.contains(id)) {
        const float value = m_parameterValues.value(id);
        if (!std::isfinite(value) || value < metadata.minimum || value > metadata.maximum) return false;
    }
    CModelParameterMetadata normalized = metadata;
    if (normalized.displayName.isEmpty()) normalized.displayName = base.displayName;
    m_parameterMetadataOverrides.insert(id, normalized);
    ++m_revision;
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
        ++m_revision;
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
    try {
        const CModelDrawable* target = nullptr;
        for (const auto& drawable : m_drawables) if (drawable.id == id) {
            if (target) return false;
            target = &drawable;
        }
        if (!target || !CMeshTopology::IsValid(indices, target->vertexCount)) return false;
        CModelAtlasState before;
        if (!m_artwork.bindings.isEmpty() && !CaptureAtlasState(before)) return false;
        CModelDocument candidate = *this;
        candidate.m_drawableIndexOverrides.insert(id, indices);
        QVector<int> identity;
        identity.reserve(target->vertexCount);
        for (int index = 0; index < target->vertexCount; ++index) identity.append(index);
        const QVector<CGeometryObjectPairs::VertexRemap> remaps{{{CModelGeometryKeyform::ArtMesh, id}, std::move(identity)}};
        if (!ReconcileGeometryObjectPairs(candidate, remaps, nullptr, nullptr, nullptr)) return false;
        if (!candidate.ReconcileArtworkAppearance(before, nullptr)) return false;
        CommitCandidate(std::move(candidate));
        return true;
    } catch (const std::exception&) { return false; }
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
    return SetDeformerParentIndices({{id, parentIndex}});
}

bool CModelDocument::SetDeformerParentIndices(const QHash<QString, int>& parents)
{
    QHash<QString, int> indices;
    for (int index = 0; index < m_deformers.size(); ++index)
        if (!indices.contains(m_deformers.at(index).id)) indices.insert(m_deformers.at(index).id, index);
    auto candidate = m_deformerParentOverrides;
    for (auto it = parents.cbegin(); it != parents.cend(); ++it) {
        const int index = indices.value(it.key(), -1);
        const int parent = it.value();
        if (index < 0 || parent < -1 || parent >= m_deformers.size() || parent == index ||
            (parent >= 0 && parent != m_deformers.at(index).parentDeformerIndex && parent != m_sourceDeformerParents.value(it.key(), -1) &&
             m_deformers.at(parent).parentPartIndex != m_deformers.at(index).parentPartIndex)) return false;
        candidate.insert(it.key(), parent);
    }
    QVector<int> effective;
    effective.reserve(m_deformers.size());
    for (const auto& deformer : m_deformers) effective.append(candidate.value(deformer.id, deformer.parentDeformerIndex));
    const CParentGraph graph(effective);
    for (auto it = parents.cbegin(); it != parents.cend(); ++it)
        if (!graph.IsValid(indices.value(it.key(), -1))) return false;
    m_deformerParentOverrides.swap(candidate);
    ++m_revision;
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
            if (!keyform.IsValid())
                return false;
        }
        m_deformerRotationKeyformOverrides.insert(id, keyforms);
        ++m_revision;
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
        ++m_revision;
        return true;
    }
    return false;
}

bool CModelDocument::SetDrawableMaskIndices(const QString& id, const QVector<int>& maskIndices)
{
    for (const auto& drawable : m_drawables) {
        if (drawable.id != id) continue;
        if (!MaskReferencesAllowed(id, maskIndices, true)) return false;
        if (maskIndices == drawable.maskIndices) m_drawableMaskIndexOverrides.remove(id);
        else m_drawableMaskIndexOverrides.insert(id, maskIndices);
        ++m_revision;
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
        ++m_revision;
        return true;
    }
    return false;
}

bool CModelDocument::SetDeformerVisible(const QString& id, bool visible)
{
    for (CModelDeformer& deformer : m_deformers) {
        if (deformer.id != id) continue;
        deformer.visible = visible;
        ++m_revision;
        return true;
    }
    return false;
}

bool CModelDocument::SetDrawableVisible(const QString& id, bool visible)
{
    for (CModelDrawable& drawable : m_drawables) {
        if (drawable.id != id) continue;
        drawable.visible = visible;
        ++m_revision;
        return true;
    }
    return false;
}

QMap<QString, bool> CModelDocument::VisibilityState() const
{
    QMap<QString, bool> result;
    for (const auto& part : m_parts) result.insert(QStringLiteral("part|") + part.id, part.visible);
    for (const auto& mesh : m_drawables) result.insert(QStringLiteral("artmesh|") + mesh.id, mesh.visible);
    for (const auto& deformer : m_deformers)
        result.insert((deformer.type == CModelDeformer::Warp ? QStringLiteral("warp|") : QStringLiteral("rotation|")) +
                      deformer.id, deformer.visible);
    return result;
}

bool CModelDocument::SetVisibilityValues(const QMap<QString, bool>& values)
{
    if (values.isEmpty()) return false;
    try {
        struct Change { int kind; int index; bool visible; };
        QVector<Change> changes;
        QSet<QString> matched;
        const auto collect = [&](int kind, int index, const QString& prefix, const QString& id) {
            const QString key = prefix + id;
            const auto value = values.constFind(key);
            if (value == values.cend()) return true;
            if (id.isEmpty() || matched.contains(key)) return false;
            matched.insert(key);
            changes.append({kind, index, value.value()});
            return true;
        };
        for (int index = 0; index < m_parts.size(); ++index)
            if (!collect(0, index, QStringLiteral("part|"), m_parts.at(index).id)) return false;
        for (int index = 0; index < m_drawables.size(); ++index)
            if (!collect(1, index, QStringLiteral("artmesh|"), m_drawables.at(index).id)) return false;
        for (int index = 0; index < m_deformers.size(); ++index) {
            const auto& deformer = m_deformers.at(index);
            if (!collect(2, index, deformer.type == CModelDeformer::Warp ? QStringLiteral("warp|") : QStringLiteral("rotation|"),
                         deformer.id)) return false;
        }
        if (matched.size() != values.size()) return false;
        auto parts = m_parts;
        auto meshes = m_drawables;
        auto deformers = m_deformers;
        for (const auto& change : changes) {
            if (change.kind == 0) parts[change.index].visible = change.visible;
            else if (change.kind == 1) meshes[change.index].visible = change.visible;
            else deformers[change.index].visible = change.visible;
        }
        m_parts.swap(parts);
        m_drawables.swap(meshes);
        m_deformers.swap(deformers);
        ++m_revision;
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

bool CModelDocument::SetDrawableOpacity(const QString& id, float opacity)
{
    if (!std::isfinite(opacity)) return false;
    const float clamped = qBound(0.0f, opacity, 1.0f);
    for (const CModelDrawable& drawable : m_drawables) {
        if (drawable.id != id) continue;
        m_drawableOpacityOverrides.insert(id, clamped);
        ++m_revision;
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
        ++m_revision;
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
    if (m_lockedKeys.contains(key) == locked) return;
    ++m_revision;
    if (locked) m_lockedKeys.insert(key);
    else m_lockedKeys.remove(key);
}

void CModelDocument::ClearLocked()
{
    if (!m_lockedKeys.isEmpty()) ++m_revision;
    m_lockedKeys.clear();
}

bool CModelDocument::SetLockedValues(const QMap<QString, bool>& values)
{
    if (values.isEmpty()) return false;
    try {
        const auto objects = VisibilityState();
        auto candidate = m_lockedKeys;
        for (auto it = values.cbegin(); it != values.cend(); ++it) {
            const int separator = it.key().indexOf(QLatin1Char('|'));
            if (separator <= 0 || separator == it.key().size() - 1 || !objects.contains(it.key())) return false;
            if (it.value()) candidate.insert(it.key());
            else candidate.remove(it.key());
        }
        if (candidate == m_lockedKeys) return true;
        m_lockedKeys.swap(candidate);
        ++m_revision;
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

bool CModelDocument::IsLocked(const QString& type, const QString& id) const
{
    return m_lockedKeys.contains(type + QLatin1Char('|') + id);
}

bool CModelDocument::HasValidStructureRequests(const CModelEdit& edit)
{
    if (!edit.meshVerification.IsEmpty() && edit.field != CModelEdit::MeshStructure) return false;
    if (edit.field == CModelEdit::DeformerStructure) {
        if (!edit.deformerRequest || edit.deformerRequest->operation.valueless_by_exception() ||
            edit.type != QLatin1String("deformer") || edit.parameterChange != CModelEdit::ParameterChange::None ||
            edit.freezeValue || edit.bindingKeysRequest || edit.meshRequest || !edit.meshRemaps.isEmpty()) return false;
        return edit.name == std::visit([](const auto& request) { return request.id; }, edit.deformerRequest->operation);
    }
    if (edit.deformerRequest) return false;
    if (!edit.bindingKeysRequest) return edit.parameterChange != CModelEdit::ParameterChange::BindingKeys;
    return edit.parameterChange == CModelEdit::ParameterChange::BindingKeys && edit.field == CModelEdit::ParameterStructure &&
        edit.type == QLatin1String("parameter") && edit.name == edit.bindingKeysRequest->parameterId &&
        !edit.freezeValue && edit.meshRemaps.isEmpty() && !edit.meshRequest;
}

bool CModelDocument::ApplyEdit(const CModelEdit& edit, bool forward)
{
    if (!edit.meshVerification.IsEmpty() && edit.field != CModelEdit::MeshStructure) return false;
    if (edit.field == CModelEdit::BlendShapeGeometry) {
        const auto type = QMetaType::fromType<CModelBlendShapeGeometryState>();
        if (edit.name != QLatin1String("blendShapeGeometry") || edit.type != QLatin1String("blendshape") ||
            edit.index != -1 || edit.coalesce || edit.parameterChange != CModelEdit::ParameterChange::None ||
            edit.freezeValue || edit.meshRequest || !edit.meshRemaps.isEmpty() || edit.bindingKeysRequest || edit.deformerRequest ||
            edit.before.metaType() != type || edit.after.metaType() != type ||
            m_blendShapeGeometry != (forward ? edit.before : edit.after).value<CModelBlendShapeGeometryState>()) return false;
        const auto next = (forward ? edit.after : edit.before).value<CModelBlendShapeGeometryState>();
        if (forward) {
            QSet<QString> changed;
            for (const auto& entry : m_blendShapeGeometry)
                if (!next.contains(entry)) changed.insert(entry.meshId);
            for (const auto& entry : next)
                if (!m_blendShapeGeometry.contains(entry)) changed.insert(entry.meshId);
            for (const auto& id : changed)
                if (!CanEditBlendShapeGeometry(id)) return false;
        }
        return SetBlendShapeGeometry(next);
    }
    if (edit.field == CModelEdit::AtlasLayout) return ApplyAtlasEdit(edit, forward);
    if (edit.field == CModelEdit::DrawableMaterials) return ApplyMaterialEdit(edit, forward);
    if (edit.field == CModelEdit::GeometryKeyforms) return ApplyGeometryEdit(edit, forward);
    if (edit.field == CModelEdit::GeometryObjectPairs) {
        const auto type = QMetaType::fromType<CGeometryObjectPairs::State>();
        if (edit.type != QLatin1String("objects") || edit.name != QLatin1String("geometryPairs") || edit.index != -1 ||
            edit.coalesce || edit.parameterChange != CModelEdit::ParameterChange::None || edit.freezeValue ||
            edit.before.metaType() != type || edit.after.metaType() != type ||
            m_geometryObjectPairs != (forward ? edit.before : edit.after).value<CGeometryObjectPairs::State>()) return false;
        const auto state = (forward ? edit.after : edit.before).value<CGeometryObjectPairs::State>();
        CGeometryObjectPairs::State normalized;
        if (!CGeometryObjectPairs::Normalize(state, normalized) || state != normalized) return false;
        return SetGeometryObjectPairs(state);
    }
    if (!HasValidStructureRequests(edit)) return false;
    if (edit.field == CModelEdit::DeformerDisplayName) return ApplyDeformerDisplayNameEdit(edit, forward);
    if (edit.field == CModelEdit::DisplayNames) return ApplyDisplayNamesEdit(edit, forward);
    const QVariant& value = forward ? edit.after : edit.before;
    if (edit.field == CModelEdit::ParameterOrganization) {
        const auto type = QMetaType::fromType<CParameterOrganization::State>();
        if (edit.type != QLatin1String("parameters") || edit.name != QLatin1String("organization") ||
            edit.before.metaType() != type || edit.after.metaType() != type ||
            m_parameterOrganization != (forward ? edit.before : edit.after).value<CParameterOrganization::State>()) return false;
        const auto state = value.value<CParameterOrganization::State>();
        CParameterOrganization::State normalized;
        if (!CParameterOrganization::Normalize(state, normalized) || state != normalized) return false;
        return SetParameterOrganization(state);
    }
    if (edit.field == CModelEdit::ParameterStructure || edit.field == CModelEdit::MeshStructure || edit.field == CModelEdit::DeformerStructure) {
        const QString type = edit.field == CModelEdit::ParameterStructure ? QStringLiteral("parameter") :
            edit.field == CModelEdit::MeshStructure ? QStringLiteral("mesh") : QStringLiteral("deformer");
        if (edit.type != type || edit.before.metaType() != QMetaType::fromType<QVariantMap>() ||
            edit.after.metaType() != QMetaType::fromType<QVariantMap>() ||
            ParameterStructureState() != (forward ? edit.before : edit.after).toMap()) return false;
        if (edit.field == CModelEdit::MeshStructure && !edit.meshVerification.IsEmpty())
            return ApplyVerifiedMeshEdit(edit, forward);
        return ApplyStructureState(value.toMap(), edit, forward);
    }
    if (edit.field == CModelEdit::VisibilityValues) {
        if (edit.type != QLatin1String("objects") || !edit.before.canConvert<QMap<QString, bool>>() ||
            !edit.after.canConvert<QMap<QString, bool>>()) return false;
        const auto before = edit.before.value<QMap<QString, bool>>();
        const auto after = edit.after.value<QMap<QString, bool>>();
        if (before.isEmpty() || before.size() != after.size()) return false;
        const auto current = VisibilityState();
        auto next = after.cbegin();
        for (auto previous = before.cbegin(); previous != before.cend(); ++previous, ++next) {
            if (previous.key() != next.key() || !current.contains(previous.key()) ||
                current.value(previous.key()) != (forward ? previous.value() : next.value())) return false;
        }
        return SetVisibilityValues(forward ? after : before);
    }
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
        ++m_revision;
        return true;
    }
    if (edit.field == CModelEdit::ParameterMetadata) {
        if (edit.type != QLatin1String("parameter")) return false;
        if (value.canConvert<QVariantMap>() && hasMetadataState(value.toMap()))
            return ApplyParameterMetadataState(edit.name, value.toMap());
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
    try {
        if (!edit.meshVerification.IsEmpty() && edit.field != CModelEdit::MeshStructure) return false;
        if (edit.name.isEmpty() || !HasValidStructureRequests(edit) ||
            (edit.before == edit.after && edit.field != CModelEdit::MeshStructure)) return false;
        CModelEdit command = edit;
        if (command.field == CModelEdit::DisplayNames) {
            if (command.name != QLatin1String("objects") || command.type != QLatin1String("names") || command.index != -1 || command.coalesce ||
                command.parameterChange != CModelEdit::ParameterChange::None || command.freezeValue || command.meshRequest ||
                !command.meshRemaps.isEmpty() || command.bindingKeysRequest || command.deformerRequest ||
                command.before.metaType() != QMetaType::fromType<CModelDisplayNames::State>() ||
                command.after.metaType() != QMetaType::fromType<CModelDisplayNames::State>() ||
                command.before.value<CModelDisplayNames::State>() != m_displayNames) return false;
            CModelEdit frozen;
            if (!MakeDisplayNamesEdit(command.after.value<CModelDisplayNames::State>(), frozen)) return false;
            frozen.runtime = command.runtime;
            command = std::move(frozen);
        }
        if (command.field == CModelEdit::DeformerDisplayName) {
            if (!CanEditDeformerDisplayName(command.name) || command.before.metaType() != QMetaType::fromType<QString>() ||
                command.after.metaType() != QMetaType::fromType<QString>()) return false;
            const auto copy = [](const QString& text) { return QString(text.constData(), text.size()); };
            command.name = copy(command.name); command.type = copy(command.type);
            command.before = copy(command.before.toString()); command.after = copy(command.after.toString());
        }
        if (command.field == CModelEdit::AtlasLayout) {
            if (command.type != QLatin1String("textures") || command.name != QLatin1String("atlases") || command.index != -1 ||
                command.coalesce || command.parameterChange != CModelEdit::ParameterChange::None || command.freezeValue ||
                command.meshRequest || !command.meshRemaps.isEmpty() || command.bindingKeysRequest || command.deformerRequest) return false;
            CModelEdit frozen;
            if (command.before.metaType() != QMetaType::fromType<CModelAtlasState>() ||
                command.after.metaType() != QMetaType::fromType<CModelAtlasState>() ||
                !MakeAtlasEdit(command.before.value<CModelAtlasState>(), command.after.value<CModelAtlasState>(), frozen)) return false;
            frozen.runtime = command.runtime; command = std::move(frozen);
        }
        if (command.field != CModelEdit::MeshStructure && (!command.meshRemaps.isEmpty() || command.meshRequest)) return false;
        if (command.field == CModelEdit::MeshStructure && command.before == command.after) {
            bool remapped = false;
            for (const auto& mesh : command.meshRemaps)
                for (int index = 0; index < mesh.oldToNew.size(); ++index) remapped = remapped || mesh.oldToNew[index] != index;
            if (!remapped) return false;
        }
        switch (command.parameterChange) {
        case CModelEdit::ParameterChange::None: case CModelEdit::ParameterChange::Create:
        case CModelEdit::ParameterChange::Bind: case CModelEdit::ParameterChange::Delete:
        case CModelEdit::ParameterChange::BindingKeys: break;
        default: return false;
        }
        if ((command.parameterChange != CModelEdit::ParameterChange::None || command.freezeValue) &&
            command.field != CModelEdit::ParameterStructure) return false;
        if (command.freezeValue && (command.parameterChange != CModelEdit::ParameterChange::Delete ||
            !std::isfinite(*command.freezeValue))) return false;
        const auto validParameterValue = [this](const QString& id, float value) {
            if (!std::isfinite(value) || !std::any_of(m_parameters.cbegin(), m_parameters.cend(),
                [&](const auto& parameter) { return parameter.id == id; })) return false;
            const auto metadata = ParameterMetadata(id);
            return value >= metadata.minimum && value <= metadata.maximum;
        };
        if (command.field == CModelEdit::ParameterValue) {
            bool valid = false;
            const float before = command.before.toFloat(&valid);
            if (!valid || !validParameterValue(command.name, before)) return false;
        } else if (command.field == CModelEdit::ParameterValues) {
            if (!command.before.canConvert<QMap<QString, float>>() || !command.after.canConvert<QMap<QString, float>>()) return false;
            const auto before = command.before.value<QMap<QString, float>>();
            const auto after = command.after.value<QMap<QString, float>>();
            if (before.isEmpty() || before.keys() != after.keys()) return false;
            for (auto it = before.cbegin(); it != before.cend(); ++it)
                if (!validParameterValue(it.key(), it.value()) || !validParameterValue(it.key(), after.value(it.key()))) return false;
        }
        if (command.field == CModelEdit::ParameterMetadata) {
            if (command.type != QLatin1String("parameter") || !command.before.canConvert<QVariantMap>() ||
                !command.after.canConvert<QVariantMap>()) return false;
            const auto current = ParameterMetadataState(command.name);
            if (current.isEmpty()) return false;
            CModelParameterMetadata before, after;
            if (!metadataFromVariant(command.before, ParameterMetadata(command.name), &before) ||
                !(before == ParameterMetadata(command.name)) ||
                (hasMetadataState(command.before.toMap()) && command.before.toMap() != current) ||
                !metadataFromVariant(command.after, before, &after)) return false;
            if (!hasMetadataState(command.after.toMap())) {
                if (!MakeParameterMetadataEdit(command.name, after, command)) return false;
                command.coalesce = edit.coalesce;
                command.runtime = edit.runtime;
            } else command.before = current;
            if (command.before == command.after) return false;
        }
        if (command.field == CModelEdit::BlendShapeGeometry &&
            command.before.metaType() == QMetaType::fromType<CModelBlendShapeGeometryState>() &&
            command.after.metaType() == QMetaType::fromType<CModelBlendShapeGeometryState>()) {
            command.before = QVariant::fromValue(freezeBlendShapeGeometry(command.before.value<CModelBlendShapeGeometryState>()));
            command.after = QVariant::fromValue(freezeBlendShapeGeometry(command.after.value<CModelBlendShapeGeometryState>()));
        }
        CModelDocument candidate = *this;
        std::shared_ptr<CModelEditStorageState> storage;
        if (command.field != CModelEdit::ParameterStructure && command.field != CModelEdit::MeshStructure && command.field != CModelEdit::DeformerStructure) {
            storage = std::make_shared<CModelEditStorageState>();
            storage->before = EditStorage(command);
        }
        if (!candidate.ApplyEdit(command, true)) return false;
        if (command.field == CModelEdit::BlendShapeGeometry) {
            command.name = QStringLiteral("blendShapeGeometry");
            command.type = QStringLiteral("blendshape");
            command.before = QVariant::fromValue(m_blendShapeGeometry);
            command.after = QVariant::fromValue(candidate.m_blendShapeGeometry);
        }
        if (!command.meshVerification.IsEmpty()) FreezeVerifiedMeshCommand(command);
        if (command.parameterChange != CModelEdit::ParameterChange::None) {
            QStringList beforeIds, afterIds;
            for (const auto& parameter : m_parameters) beforeIds.append(parameter.id);
            for (const auto& parameter : candidate.m_parameters) afterIds.append(parameter.id);
            if (command.parameterChange == CModelEdit::ParameterChange::Create) {
                if (beforeIds.contains(command.name) || afterIds.removeAll(command.name) != 1 || beforeIds != afterIds) return false;
            } else if (command.parameterChange == CModelEdit::ParameterChange::Delete) {
                if (afterIds.contains(command.name) || beforeIds.removeAll(command.name) != 1 || beforeIds != afterIds) return false;
                if (command.freezeValue && !validParameterValue(command.name, *command.freezeValue)) return false;
            } else if (!beforeIds.contains(command.name) || beforeIds != afterIds) return false;
        }
        if (storage) storage->after = candidate.EditStorage(command);
        command.storage = std::move(storage);
        command.meshVerification = {};
        if (candidate.m_editIndex < candidate.m_editHistory.size()) candidate.m_editHistory.resize(candidate.m_editIndex);
        bool merged = false;
        if (command.field != CModelEdit::ParameterStructure && command.field != CModelEdit::MeshStructure && command.field != CModelEdit::DeformerStructure &&
            command.coalesce && !candidate.m_editHistory.isEmpty()) {
            auto& last = candidate.m_editHistory.last();
            if (last.coalesce && last.name == command.name && last.type == command.type && last.field == command.field &&
                last.index == command.index && last.after == command.before && last.storage && command.storage &&
                last.storage->after == command.storage->before) {
                auto combined = std::make_shared<CModelEditStorageState>();
                combined->before = last.storage->before;
                combined->after = command.storage->after;
                last.after = command.after;
                last.storage = std::move(combined);
                merged = true;
            }
        }
        if (!merged) candidate.m_editHistory.append(command);
        candidate.m_editIndex = candidate.m_editHistory.size();
        CommitCandidate(std::move(candidate));
        return true;
    } catch (const std::exception&) { return false; }
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
    try {
        if (m_editIndex <= 0) return false;
        const CModelEdit command = m_editHistory[m_editIndex - 1];
        CModelDocument candidate = *this;
        if (command.storage && candidate.EditStorage(command) != command.storage->after) return false;
        if (command.storage && !command.storage->before.isEmpty()) {
            if (!candidate.RestoreEditStorage(command.storage->before) || !candidate.ValidateEditStorage(command)) return false;
        } else if (!candidate.ApplyEdit(command, false)) return false;
        --candidate.m_editIndex;
        CommitCandidate(std::move(candidate));
        if (edit) *edit = command;
        return true;
    } catch (const std::exception&) { return false; }
}

bool CModelDocument::RedoEdit(CModelEdit* edit)
{
    try {
        if (m_editIndex >= m_editHistory.size()) return false;
        const CModelEdit command = m_editHistory[m_editIndex];
        CModelDocument candidate = *this;
        if (command.storage && candidate.EditStorage(command) != command.storage->before) return false;
        if (command.storage && !command.storage->after.isEmpty()) {
            if (!candidate.RestoreEditStorage(command.storage->after) || !candidate.ValidateEditStorage(command)) return false;
        } else if (!candidate.ApplyEdit(command, true)) return false;
        ++candidate.m_editIndex;
        CommitCandidate(std::move(candidate));
        if (edit) *edit = command;
        return true;
    } catch (const std::exception&) { return false; }
}

void CModelDocument::ClearEditHistory()
{
    if (!m_editHistory.isEmpty()) ++m_revision;
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
        const auto metadata = m_parameterMetadataOverrides.value(parameter.id, sourceMetadata(parameter));
        if (parameter.id.isEmpty() || parameterIds.contains(parameter.id)) {
            diagnostics.append({CModelDiagnostic::Error, QStringLiteral("parameter_id"),
                                 QStringLiteral("参数 ID 为空或重复"), i});
        }
        parameterIds.insert(parameter.id);
        if (metadata.minimum > metadata.maximum ||
            metadata.defaultValue < metadata.minimum || metadata.defaultValue > metadata.maximum) {
            diagnostics.append({CModelDiagnostic::Error, QStringLiteral("parameter_range"),
                                 QStringLiteral("参数范围或默认值无效"), i});
        }
        if (!std::isfinite(metadata.minimum) || !std::isfinite(metadata.maximum) ||
            !std::isfinite(metadata.defaultValue)) {
            diagnostics.append({CModelDiagnostic::Error, QStringLiteral("parameter_nan"),
                                 QStringLiteral("参数包含非有限数值"), i});
        }
        float previousKey = -std::numeric_limits<float>::infinity();
        for (float key : parameter.keyValues) {
            if (!std::isfinite(key) || key < metadata.minimum || key > metadata.maximum || key < previousKey) {
                diagnostics.append({CModelDiagnostic::Warning, QStringLiteral("parameter_keys"),
                                     QStringLiteral("参数关键值非有限、超出当前范围或未递增"), i});
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
                if (!keyform.IsValid()) {
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
            if (index >= drawable.vertexCount) {
                diagnostics.append({CModelDiagnostic::Error, QStringLiteral("index_range"),
                                     QStringLiteral("ArtMesh 三角形索引越界"), i});
                break;
            }
        }
        if (!drawable.indices.isEmpty() && drawable.indices.size() % 3 != 0) {
            diagnostics.append({CModelDiagnostic::Error, QStringLiteral("index_triangle_count"),
                                 QStringLiteral("ArtMesh 索引数量不是三角形的倍数"), i});
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
            const auto metadata = m_parameterMetadataOverrides.value(parameter.id, sourceMetadata(parameter));
            if (!std::isfinite(it.value()) || it.value() < metadata.minimum || it.value() > metadata.maximum) {
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
    diagnostics += MaskDiagnostics();
    QHash<QString, int> deformerIndices;
    QVector<int> effectiveParents;
    if (!m_deformerParentOverrides.isEmpty()) {
        effectiveParents.reserve(m_deformers.size());
        for (int index = 0; index < m_deformers.size(); ++index) {
            const auto& deformer = m_deformers[index];
            if (!deformerIndices.contains(deformer.id)) deformerIndices.insert(deformer.id, index);
            effectiveParents.append(m_deformerParentOverrides.value(deformer.id, deformer.parentDeformerIndex));
        }
    }
    const CParentGraph effectiveGraph(effectiveParents);
    for (auto parentIt = m_deformerParentOverrides.cbegin(); parentIt != m_deformerParentOverrides.cend(); ++parentIt) {
        const int deformerIndex = deformerIndices.value(parentIt.key(), -1);
        const int parentIndex = parentIt.value();
        if (deformerIndex < 0 || parentIndex < -1 || parentIndex >= m_deformers.size() || parentIndex == deformerIndex ||
            (parentIndex >= 0 && parentIndex != m_deformers[deformerIndex].parentDeformerIndex &&
             parentIndex != m_sourceDeformerParents.value(parentIt.key(), -1) &&
             m_deformers[parentIndex].parentPartIndex != m_deformers[deformerIndex].parentPartIndex)) {
            diagnostics.append({CModelDiagnostic::Error, QStringLiteral("deformer_parent_override"),
                                 QStringLiteral("变形器父级覆盖无效：%1").arg(parentIt.key()), -1});
            continue;
        }
        if (!effectiveGraph.IsValid(deformerIndex))
            diagnostics.append({CModelDiagnostic::Error, QStringLiteral("deformer_parent_cycle"),
                                 QStringLiteral("变形器父级覆盖存在循环或无效祖先：%1").arg(parentIt.key()), -1});
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
                                 QStringLiteral("反向蒙版覆盖引用了不存在的 ArtMesh：%1").arg(invertIt.key()), -1,
                                 QStringLiteral("artmesh"), invertIt.key()});
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
    QString pairError;
    if (!CGeometryObjectPairs::Validate(m_geometryObjectPairs, *this, &pairError))
        diagnostics.append({CModelDiagnostic::Error, QStringLiteral("geometry_object_pairs"), pairError, -1});
    for (int index = 0; index < m_geometryObjectPairs.pairs.size(); ++index)
        if (m_geometryObjectPairs.pairs[index].requiresReview)
            diagnostics.append({CModelDiagnostic::Warning, QStringLiteral("geometry_pair_review"),
                QStringLiteral("几何对象配对需要重新确认：%1").arg(m_geometryObjectPairs.pairs[index].id), index});
    return diagnostics;
}

}
