#include "cmodelsession.h"
#include "cmaskeditpolicy.h"
#include "cmotiontimeline.h"
#include "cmeshtopology.h"
#include "csectiontable.h"

#include <QFile>
#include <QSaveFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QCryptographicHash>
#include <QSet>
#include <cmath>
#include <limits>
#include <utility>
#include <exception>

namespace live2dstruct {

namespace {
constexpr int StructureCountFields = 34;
constexpr int MaximumBlendShapeGeometryEntries = 100000;
constexpr int MaximumBlendShapeGeometryDeltas = 2000000;
constexpr int MaximumAnimationScenes = 128;
constexpr int MaximumAnimationSceneIdBytes = 128;
constexpr qint64 MaximumAnimationSceneBytes = 64 * 1024 * 1024;

void setError(QString* error, const QString& value)
{
    if (error) *error = value;
}

bool readObject(const QJsonObject& root, const QString& key, QJsonObject* object, QString* error)
{
    const QJsonValue value = root.value(key);
    if (value.isUndefined()) {
        *object = {};
        return true;
    }
    if (!value.isObject()) {
        setError(error, QStringLiteral("会话字段类型错误：%1").arg(key));
        return false;
    }
    *object = value.toObject();
    return true;
}

bool readNumber(const QJsonValue& value, double* number)
{
    if (!value.isDouble()) return false;
    *number = value.toDouble();
    return std::isfinite(*number);
}

bool readFloat(const QJsonValue& value, float* number)
{
    double parsed = 0.0;
    if (!readNumber(value, &parsed) || parsed < -std::numeric_limits<float>::max() ||
        parsed > std::numeric_limits<float>::max()) return false;
    *number = static_cast<float>(parsed);
    return std::isfinite(*number);
}

bool validPhysicsDraft(const CModelSession& session, int version, QString* error)
{
    if (!session.physicsDraft) return true;
    const auto& draft = *session.physicsDraft;
    if (draft.document.isEmpty() || draft.sourcePath.contains(QChar::Null)) {
        setError(error, QStringLiteral("会话物理草稿来源无效"));
        return false;
    }
    if (draft.sourcePath.isEmpty() && (version < 4 || session.sourceMoc.isEmpty() ||
        session.sourceMoc.contains(QChar::Null) || session.sourceDigest.size() != 32)) {
        setError(error, QStringLiteral("无源文件的 physicsDraft 需要版本 4、sourceMoc 和有效的 sourceDigest"));
        return false;
    }
    CParameterReferenceEditor::Prepared prepared;
    return CParameterReferenceEditor::Prepare(CParameterReferenceEditor::Kind::Physics,
        QJsonDocument(draft.document).toJson(QJsonDocument::Compact), {}, prepared, error);
}

bool validMeshStructure(const CModelSession& session, int version, QString* error)
{
    if (!session.meshStructure) return true;
    if (version < 5 || session.sourceMoc.isEmpty() || session.sourceMoc.contains(QChar::Null) || session.sourceDigest.size() != 32) {
        setError(error, QStringLiteral("meshStructure 需要版本 5、sourceMoc 和有效的 sourceDigest"));
        return false;
    }
    return true;
}

bool validArtMeshClones(const CModelSession& session, int version, QString* error)
{
    if (session.artMeshClones.isEmpty()) return true;
    if (version < 18 || !session.meshStructure || session.sourceMoc.isEmpty() || session.sourceMoc.contains(QChar::Null) ||
        session.sourceDigest.size() != 32) {
        setError(error, QStringLiteral("ArtMesh 克隆目录需要版本 18、网格结构和有效模型来源"));
        return false;
    }
    if (session.artMeshClones.size() > 1000000) {
        setError(error, QStringLiteral("ArtMesh 克隆数量超出限制"));
        return false;
    }
    QSet<QString> created;
    for (const auto& clone : session.artMeshClones) {
        const auto source = clone.sourceArtMeshId.toUtf8(), target = clone.newArtMeshId.toUtf8();
        if (source.isEmpty() || target.isEmpty() || source.size() >= 64 || target.size() >= 64 || source.contains('\0') || target.contains('\0') ||
            QString::fromUtf8(source) != clone.sourceArtMeshId || QString::fromUtf8(target) != clone.newArtMeshId ||
            clone.sourceArtMeshId == clone.newArtMeshId || created.contains(clone.newArtMeshId) ||
            clone.parentPartIndex < -2 || clone.parentDeformerIndex < -2 || clone.textureIndex < -2) {
            setError(error, QStringLiteral("ArtMesh 克隆目录包含无效或重复身份"));
            return false;
        }
        created.insert(clone.newArtMeshId);
    }
    return true;
}

bool validDeformerStructure(const CModelSession& session, int version, QString* error)
{
    if (!session.deformerStructure) {
        if (session.sourceDeformerIds.isEmpty()) return true;
        setError(error, QStringLiteral("变形器源身份需要启用 deformerStructure"));
        return false;
    }
    if (version < 6 || session.sourceMoc.isEmpty() || session.sourceMoc.contains(QChar::Null) ||
        session.sourceDigest.size() != 32 || session.sourceDeformerIds.size() > 1000000) {
        setError(error, QStringLiteral("deformerStructure 需要版本 6 和有效的源模型快照"));
        return false;
    }
    for (const auto& id : session.sourceDeformerIds) {
        const auto bytes = id.toUtf8();
        if (bytes.isEmpty() || bytes.size() >= 64 || bytes.contains('\0') || QString::fromUtf8(bytes) != id) {
            setError(error, QStringLiteral("变形器源身份 ID 无效"));
            return false;
        }
    }
    return true;
}

bool validMaterials(const CModelSession& session, int version, QString* error)
{
    if ((session.materialStructure || !session.drawableMaterials.isEmpty()) &&
        (version < 10 || session.sourceDigest.size() != 32 || session.sourceMoc.contains(QChar::Null))) {
        setError(error, QStringLiteral("材质编辑需要版本 10 及有效源模型指纹")); return false;
    }
    QJsonObject checked;
    return CModelMaterialEdit::ToJson(session.drawableMaterials, checked, error);
}

bool validWorkspace(const CModelSession& session, int version, QString* error)
{
    if (!session.textureWorkspace) return true;
    if (version < 11 || !session.textures.isEmpty() || session.sourceMoc.isEmpty() ||
        session.sourceMoc.contains(QChar::Null) || session.sourceDigest.size() != 32) {
        setError(error, QStringLiteral("图集工作区需要版本 11、独立来源指纹且不能混用旧纹理覆盖")); return false;
    }
    return CTextureWorkspace::ValidateDraft(*session.textureWorkspace, error);
}

bool validArtwork(const CModelSession& session, int version, QString* error)
{
    if (session.artwork.IsEmpty()) return true;
    if (version < 12 || session.sourceMoc.isEmpty() || session.sourceMoc.contains(QChar::Null) || session.sourceDigest.size() != 32) {
        setError(error, QStringLiteral("素材关联需要版本 12 及有效源模型指纹")); return false;
    }
    return CModelArtwork::Validate(session.artwork, nullptr, error);
}

bool validDisplayNames(const CModelSession& session, int version, QString* error)
{
    if (session.objectDisplayNames) {
        if (version < 15 || session.sourceMoc.contains(QChar::Null) || session.sourceDigest.size() != 32) {
            setError(error, QStringLiteral("对象名称需要版本 15 及有效源模型指纹")); return false;
        }
        CModelDisplayNames::State normalized;
        if (!CModelDisplayNames::Normalize(*session.objectDisplayNames, normalized, error)) return false;
        if (session.deformerDisplayNames) {
            CDeformerDisplayNames::State legacy;
            if (!CDeformerDisplayNames::Normalize(*session.deformerDisplayNames, legacy, error)) return false;
            if (legacy != normalized.deformers) {
                setError(error, QStringLiteral("新旧对象名称状态冲突")); return false;
            }
        }
        return true;
    }
    if (version >= 15) { setError(error, QStringLiteral("版本 15 会话缺少对象名称目录")); return false; }
    if (!session.deformerDisplayNames) {
        if (version < 13) return true;
        setError(error, QStringLiteral("版本 13 会话缺少变形器名称对象"));
        return false;
    }
    if (version < 13 || session.sourceMoc.contains(QChar::Null) || session.sourceDigest.size() != 32) {
        setError(error, QStringLiteral("变形器名称需要版本 13 及有效源模型指纹"));
        return false;
    }
    return CDeformerDisplayNames::Validate(*session.deformerDisplayNames, error);
}

bool validStructure(const CMocParameterStructureBuilder::ColumnState& state, QString* error)
{
    qint64 bytes = 0;
    if (state.columns.size() > 1000000 || state.counts.size() > StructureCountFields) {
        setError(error, QStringLiteral("会话参数结构字段数量超出范围"));
        return false;
    }
    for (auto it = state.columns.cbegin(); it != state.columns.cend(); ++it) {
        if (it.key() < 0x48 || it.key() >= 0x40 + 1000000 * 4 || it.key() % 4 ||
            it->size() > std::numeric_limits<int>::max() - bytes) {
            setError(error, QStringLiteral("会话参数结构区段或总长度无效"));
            return false;
        }
        bytes += it->size();
    }
    for (auto it = state.counts.cbegin(); it != state.counts.cend(); ++it)
        if (it.key() < 0 || it.key() >= StructureCountFields || it.value() < 0 || it.value() > 1000000) {
            setError(error, QStringLiteral("会话参数结构计数无效"));
            return false;
        }
    return true;
}

bool validBlendShapeGeometry(const CModelSession& session, int version, QString* error)
{
    if (session.blendShapeGeometry.isEmpty()) return true;
    if (version < 16 || session.sourceMoc.isEmpty() || session.sourceMoc.contains(QChar::Null) ||
        session.sourceDigest.size() != QCryptographicHash::hashLength(QCryptographicHash::Sha256)) {
        setError(error, QStringLiteral("BlendShape 几何状态需要版本 16 和有效源模型指纹")); return false;
    }
    if (session.blendShapeGeometry.size() > MaximumBlendShapeGeometryEntries) {
        setError(error, QStringLiteral("BlendShape 几何条目数量超出限制")); return false;
    }
    QSet<QString> endpoints;
    qsizetype deltaCount = 0;
    for (const auto& entry : session.blendShapeGeometry) {
        const auto id = entry.meshId.toUtf8();
        if (id.isEmpty() || id.size() >= 64 || id.contains('\0') || QString::fromUtf8(id) != entry.meshId ||
            entry.shapeIndex < 0 || entry.bindingOffset < 0 || entry.keyformOffset < 0 || entry.sourceKeyformIndex < 0 ||
            entry.sourceDigest.size() != QCryptographicHash::hashLength(QCryptographicHash::Sha256) ||
            entry.deltas.isEmpty() || entry.deltas.size() % 2 ||
            entry.deltas.size() > MaximumBlendShapeGeometryDeltas - deltaCount ||
            std::any_of(entry.deltas.cbegin(), entry.deltas.cend(), [](float value) { return !std::isfinite(value); })) {
            setError(error, QStringLiteral("BlendShape 几何状态条目无效")); return false;
        }
        deltaCount += entry.deltas.size();
        const QString endpoint = QStringLiteral("%1|%2|%3|%4").arg(entry.meshId).arg(entry.shapeIndex)
            .arg(entry.bindingOffset).arg(entry.keyformOffset);
        if (endpoints.contains(endpoint)) {
            setError(error, QStringLiteral("BlendShape 几何状态包含重复端点：%1").arg(endpoint)); return false;
        }
        endpoints.insert(endpoint);
    }
    return true;
}

bool validAnimationScenes(const CModelSession& session, int version, QString* error)
{
    if (version < 17 && (!session.animationScenes.isEmpty() || !session.activeAnimationScene.isEmpty())) {
        setError(error, QStringLiteral("动画场景需要版本 17"));
        return false;
    }
    if (session.animationScenes.isEmpty()) {
        if (version == 18 && session.animation.isEmpty()) return true;
        if (version >= 17) {
            setError(error, QStringLiteral("版本 17 会话缺少动画场景"));
            return false;
        }
        return true;
    }
    if (session.animationScenes.size() > MaximumAnimationScenes || session.activeAnimationScene.isEmpty()) {
        setError(error, QStringLiteral("动画场景数量或活动场景无效"));
        return false;
    }
    QSet<QString> ids;
    qint64 totalBytes = 0;
    bool activeFound = false;
    for (const auto& value : session.animationScenes) {
        if (!value.isObject()) {
            setError(error, QStringLiteral("动画场景条目必须是对象"));
            return false;
        }
        const auto object = value.toObject();
        const QString id = object.value(QStringLiteral("id")).toString();
        const QString name = object.value(QStringLiteral("name")).toString();
        const auto state = object.value(QStringLiteral("state"));
        const auto idBytes = id.toUtf8();
        if (id.isEmpty() || idBytes.size() > MaximumAnimationSceneIdBytes || id.contains(QChar::Null) ||
            name.isEmpty() || name.toUtf8().size() > MaximumAnimationSceneIdBytes || name.contains(QChar::Null) ||
            !state.isObject() || ids.contains(id)) {
            setError(error, QStringLiteral("动画场景身份或状态无效"));
            return false;
        }
        CMotionTimeline timeline;
        if (!timeline.RestoreState(state.toObject(), nullptr, error)) return false;
        totalBytes += QJsonDocument(object).toJson(QJsonDocument::Compact).size();
        if (totalBytes > MaximumAnimationSceneBytes) {
            setError(error, QStringLiteral("动画场景数据总量超出限制"));
            return false;
        }
        ids.insert(id);
        activeFound |= id == session.activeAnimationScene;
    }
    if (!activeFound) {
        setError(error, QStringLiteral("活动动画场景不存在"));
        return false;
    }
    return true;
}

bool readStructure(const QJsonObject& root, CMocParameterStructureBuilder::ColumnState& state, QString* error)
{
    QJsonObject object, columns, counts;
    if (!readObject(root, QStringLiteral("structure"), &object, error)) return false;
    if (object.isEmpty()) return true;
    if (object.size() != 2 || !object.contains(QStringLiteral("columns")) || !object.contains(QStringLiteral("counts")) ||
        !readObject(object, QStringLiteral("columns"), &columns, error) ||
        !readObject(object, QStringLiteral("counts"), &counts, error) || columns.size() > 1000000 || counts.size() > StructureCountFields) {
        setError(error, QStringLiteral("会话参数结构格式无效"));
        return false;
    }
    qint64 bytes = 0;
    for (auto it = columns.begin(); it != columns.end(); ++it) {
        bool converted = false;
        const int slot = it.key().toInt(&converted);
        if (!converted || QString::number(slot) != it.key() || slot < 0x48 || slot >= 0x40 + 1000000 * 4 || slot % 4 || !it->isString()) {
            setError(error, QStringLiteral("会话参数结构区段编号或编码无效"));
            return false;
        }
        const QString encoded = it->toString();
        const auto ascii = encoded.toLatin1();
        if (QString::fromLatin1(ascii) != encoded || qint64(ascii.size() / 4) * 3 > std::numeric_limits<int>::max() - bytes + 2) {
            setError(error, QStringLiteral("会话参数结构编码或总长度无效"));
            return false;
        }
        const auto decoded = QByteArray::fromBase64Encoding(ascii, QByteArray::AbortOnBase64DecodingErrors);
        if (!decoded || decoded.decoded.toBase64() != ascii || decoded.decoded.size() > std::numeric_limits<int>::max() - bytes) {
            setError(error, QStringLiteral("会话参数结构 Base64 数据无效"));
            return false;
        }
        bytes += decoded.decoded.size();
        state.columns.insert(slot, decoded.decoded);
    }
    for (auto it = counts.begin(); it != counts.end(); ++it) {
        bool converted = false;
        const int index = it.key().toInt(&converted);
        double value = 0;
        if (!converted || QString::number(index) != it.key() || index < 0 || index >= StructureCountFields ||
            !readNumber(it.value(), &value) || value < 0 || value > 1000000 || std::floor(value) != value) {
            setError(error, QStringLiteral("会话参数结构计数无效"));
            return false;
        }
        state.counts.insert(index, int(value));
    }
    return validStructure(state, error);
}
}

bool CModelSession::HasMaskMultiplicityOverrides() const
{
    for (const auto& values : drawableMaskIndices) {
        QSet<int> unique;
        for (int index : values) {
            if (unique.contains(index)) return true;
            unique.insert(index);
        }
    }
    return false;
}

QByteArray CModelSession::ToJson() const
{
    if (version < 1 || version > 18) return {};
    if (drawableMaskIndices.size() > 100000) return {};
    for (const auto& values : drawableMaskIndices) {
        if (values.size() > CMaskEditPolicy::MaximumOverrideReferences || std::any_of(values.cbegin(), values.cend(), [](int index) { return index < 0; })) return {};
    }
    QJsonObject root;
    const bool hasStructure = !structure.columns.isEmpty() || !structure.counts.isEmpty();
    QJsonObject resources;
    if (!validStructure(structure, nullptr) || !CParameterResourceEdits::ToJson(parameterResources, resources)) return {};
    int storedVersion = hasStructure && version == 1 ? 2 : version;
    if ((!resources.isEmpty() || physicsDraft) && storedVersion >= 1 && storedVersion <= 3) storedVersion = 3;
    if (physicsDraft && physicsDraft->sourcePath.isEmpty() && storedVersion < 4) storedVersion = 4;
    if (meshStructure && storedVersion < 5) storedVersion = 5;
    if (deformerStructure && storedVersion < 6) storedVersion = 6;
    if (!textures.isEmpty() && storedVersion < 7) storedVersion = 7;
    if (parameterOrganization.configured && storedVersion < 8) storedVersion = 8;
    if (geometryObjectPairs != CGeometryObjectPairs::State{} && storedVersion < 9) storedVersion = 9;
    if ((materialStructure || !drawableMaterials.isEmpty()) && storedVersion < 10) storedVersion = 10;
    if (textureWorkspace && storedVersion < 11) storedVersion = 11;
    if (!artwork.IsEmpty() && storedVersion < 12) storedVersion = 12;
    if (deformerDisplayNames && storedVersion < 13) storedVersion = 13;
    if (HasMaskMultiplicityOverrides() && storedVersion < 14) storedVersion = 14;
    if (objectDisplayNames && storedVersion < 15) storedVersion = 15;
    if (!blendShapeGeometry.isEmpty() && storedVersion < 16) storedVersion = 16;
    if (!animationScenes.isEmpty() || !activeAnimationScene.isEmpty()) storedVersion = std::max(storedVersion, 17);
    if (!artMeshClones.isEmpty()) storedVersion = std::max(storedVersion, 18);
    if (!validDisplayNames(*this, storedVersion, nullptr) || !validBlendShapeGeometry(*this, storedVersion, nullptr) ||
        !validAnimationScenes(*this, storedVersion, nullptr) || !validArtMeshClones(*this, storedVersion, nullptr)) return {};
    if (storedVersion >= 15) {
        QJsonObject names;
        if (!CModelDisplayNames::ToJson(*objectDisplayNames, names)) return {};
        root.insert(QStringLiteral("objectDisplayNames"), names);
    } else if (storedVersion >= 13) {
        QJsonObject names;
        for (auto item = deformerDisplayNames->cbegin(); item != deformerDisplayNames->cend(); ++item)
            names.insert(item.key(), item.value());
        root.insert(QStringLiteral("deformerDisplayNames"), names);
    }
    if (!validArtwork(*this, storedVersion, nullptr)) return {};
    if (storedVersion >= 12) {
        QJsonObject sources;
        if (!CModelArtwork::ToJson(artwork, sources)) return {};
        root.insert(QStringLiteral("artwork"), sources);
    }
    if (!validWorkspace(*this, storedVersion, nullptr)) return {};
    if (storedVersion >= 11) {
        QJsonObject workspace;
        if (textureWorkspace && !CTextureWorkspace::ToJson(*textureWorkspace, workspace)) return {};
        root.insert(QStringLiteral("textureWorkspace"), textureWorkspace ? QJsonValue(workspace) : QJsonValue(QJsonValue::Null));
    }
    QJsonObject materials;
    if (!validMaterials(*this, storedVersion, nullptr) || !CModelMaterialEdit::ToJson(drawableMaterials, materials)) return {};
    if (storedVersion >= 10) {
        root.insert(QStringLiteral("materialStructure"), materialStructure);
        root.insert(QStringLiteral("drawableMaterials"), materials);
    }
    QJsonObject pairs;
    if (!CGeometryObjectPairs::ToJson(geometryObjectPairs, pairs) ||
        (!geometryObjectPairs.sourceDigest.isEmpty() && (sourceDigest.size() != 32 ||
         geometryObjectPairs.sourceDigest != sourceDigest || sourceMoc.contains(QChar::Null)))) return {};
    if (storedVersion >= 9) root.insert(QStringLiteral("geometryObjectPairs"), pairs);
    QJsonObject organization;
    if (!CParameterOrganization::ToJson(parameterOrganization, organization) ||
        (parameterOrganization.configured && (sourceDigest.size() != 32 || sourceMoc.contains(QChar::Null)))) return {};
    if (storedVersion >= 8) root.insert(QStringLiteral("parameterOrganization"), organization);
    if (!validPhysicsDraft(*this, storedVersion, nullptr) || !validMeshStructure(*this, storedVersion, nullptr) || !validDeformerStructure(*this, storedVersion, nullptr)) return {};
    QJsonArray textureRows;
    if (!CTextureAssets::ToJson(textures, textureRows)) return {};
    if (!textures.isEmpty() && (sourceMoc.isEmpty() || sourceMoc.contains(QChar::Null) || sourceDigest.size() != 32)) return {};
    if (storedVersion >= 7) root.insert(QStringLiteral("textures"), textureRows);
    root.insert(QStringLiteral("version"), storedVersion);
    if (storedVersion >= 5) root.insert(QStringLiteral("meshStructure"), meshStructure);
    if (storedVersion >= 18) {
        QJsonArray clones;
        for (const auto& clone : artMeshClones)
            clones.append(QJsonObject{{QStringLiteral("sourceId"), clone.sourceArtMeshId},
                                      {QStringLiteral("newId"), clone.newArtMeshId},
                                      {QStringLiteral("parentPartIndex"), clone.parentPartIndex},
                                      {QStringLiteral("parentDeformerIndex"), clone.parentDeformerIndex},
                                      {QStringLiteral("textureIndex"), clone.textureIndex}});
        root.insert(QStringLiteral("artMeshClones"), clones);
    }
    if (storedVersion >= 6) {
        root.insert(QStringLiteral("deformerStructure"), deformerStructure);
        QStringList ids(sourceDeformerIds.cbegin(), sourceDeformerIds.cend());
        std::sort(ids.begin(), ids.end());
        root.insert(QStringLiteral("sourceDeformerIds"), QJsonArray::fromStringList(ids));
    }
    if (!resources.isEmpty()) root.insert(QStringLiteral("parameterResources"), resources);
    if (physicsDraft) root.insert(QStringLiteral("physicsDraft"), QJsonObject{
        {QStringLiteral("sourcePath"), physicsDraft->sourcePath}, {QStringLiteral("document"), physicsDraft->document},
        {QStringLiteral("dirty"), physicsDraft->dirty}});
    if (hasStructure) {
        QJsonObject columns, counts;
        for (auto it = structure.columns.cbegin(); it != structure.columns.cend(); ++it)
            columns.insert(QString::number(it.key()), QString::fromLatin1(it->toBase64()));
        for (auto it = structure.counts.cbegin(); it != structure.counts.cend(); ++it)
            counts.insert(QString::number(it.key()), it.value());
        root.insert(QStringLiteral("structure"), QJsonObject{{QStringLiteral("columns"), columns}, {QStringLiteral("counts"), counts}});
    }
    QJsonObject animationAlias = animation;
    if (storedVersion >= 17) {
        for (const auto& value : animationScenes) {
            const auto scene = value.toObject();
            if (scene.value(QStringLiteral("id")).toString() == activeAnimationScene) {
                animationAlias = scene.value(QStringLiteral("state")).toObject();
                break;
            }
        }
    }
    root.insert(QStringLiteral("animation"), animationAlias);
    if (storedVersion >= 17) {
        root.insert(QStringLiteral("animationScenes"), animationScenes);
        root.insert(QStringLiteral("activeAnimationScene"), activeAnimationScene);
    }
    root.insert(QStringLiteral("sourceMoc"), sourceMoc);
    if (!sourceDigest.isEmpty()) root.insert(QStringLiteral("sourceDigest"), QString::fromLatin1(sourceDigest.toHex()));
    QJsonObject parameterObject;
    for (auto it = parameters.cbegin(); it != parameters.cend(); ++it) parameterObject.insert(it.key(), static_cast<double>(it.value()));
    root.insert(QStringLiteral("parameters"), parameterObject);
    QJsonObject parameterMetadataObject;
    for (auto it = parameterMetadata.cbegin(); it != parameterMetadata.cend(); ++it) {
        QJsonObject metadata;
        metadata.insert(QStringLiteral("displayName"), it.value().displayName);
        metadata.insert(QStringLiteral("minimum"), static_cast<double>(it.value().minimum));
        metadata.insert(QStringLiteral("maximum"), static_cast<double>(it.value().maximum));
        metadata.insert(QStringLiteral("defaultValue"), static_cast<double>(it.value().defaultValue));
        metadata.insert(QStringLiteral("decimalPlaces"), it.value().decimalPlaces);
        metadata.insert(QStringLiteral("repeat"), it.value().repeat);
        parameterMetadataObject.insert(it.key(), metadata);
    }
    root.insert(QStringLiteral("parameterMetadata"), parameterMetadataObject);
    QJsonObject visibilityObject;
    for (auto it = visibility.cbegin(); it != visibility.cend(); ++it) visibilityObject.insert(it.key(), it.value());
    root.insert(QStringLiteral("visibility"), visibilityObject);
    QJsonObject lockedObject;
    for (auto it = locked.cbegin(); it != locked.cend(); ++it) lockedObject.insert(it.key(), it.value());
    root.insert(QStringLiteral("locked"), lockedObject);
    QJsonObject opacityObject;
    for (auto it = drawableOpacity.cbegin(); it != drawableOpacity.cend(); ++it) opacityObject.insert(it.key(), static_cast<double>(it.value()));
    root.insert(QStringLiteral("drawableOpacity"), opacityObject);
    QJsonObject drawOrderObject;
    for (auto it = drawableDrawOrder.cbegin(); it != drawableDrawOrder.cend(); ++it) drawOrderObject.insert(it.key(), it.value());
    root.insert(QStringLiteral("drawableDrawOrder"), drawOrderObject);
    QJsonObject keyformObject;
    for (auto meshIt = drawableKeyformPositions.cbegin(); meshIt != drawableKeyformPositions.cend(); ++meshIt) {
        QJsonArray keyforms;
        for (const QVector<float>& positions : meshIt.value()) {
            QJsonArray values;
            for (float value : positions) values.append(static_cast<double>(value));
            keyforms.append(values);
        }
        keyformObject.insert(meshIt.key(), keyforms);
    }
    root.insert(QStringLiteral("drawableKeyformPositions"), keyformObject);
    QJsonArray blendShapeGeometry;
    for (const auto& entry : this->blendShapeGeometry) {
        QJsonArray deltas;
        for (float value : entry.deltas)
            deltas.append(value == 0.0f && std::signbit(value) ? QJsonValue(QStringLiteral("-0"))
                                                           : QJsonValue(static_cast<double>(value)));
        blendShapeGeometry.append(QJsonObject{{QStringLiteral("meshId"), entry.meshId},
            {QStringLiteral("shapeIndex"), entry.shapeIndex}, {QStringLiteral("bindingOffset"), entry.bindingOffset},
            {QStringLiteral("keyformOffset"), entry.keyformOffset}, {QStringLiteral("sourceKeyformIndex"), entry.sourceKeyformIndex},
            {QStringLiteral("sourceDigest"), QString::fromLatin1(entry.sourceDigest.toHex())},
            {QStringLiteral("deltas"), deltas}});
    }
    if (storedVersion >= 16) root.insert(QStringLiteral("blendShapeGeometry"), blendShapeGeometry);
    QJsonObject indexObject;
    for (auto meshIt = drawableIndices.cbegin(); meshIt != drawableIndices.cend(); ++meshIt) {
        QJsonArray values;
        for (quint16 index : meshIt.value()) values.append(static_cast<int>(index));
        indexObject.insert(meshIt.key(), values);
    }
    root.insert(QStringLiteral("drawableIndices"), indexObject);
    QJsonObject maskObject;
    for (auto meshIt = drawableMaskIndices.cbegin(); meshIt != drawableMaskIndices.cend(); ++meshIt) {
        QJsonArray values;
        for (int index : meshIt.value()) values.append(index);
        maskObject.insert(meshIt.key(), values);
    }
    root.insert(QStringLiteral("drawableMaskIndices"), maskObject);
    QJsonObject deformerParentObject;
    for (auto it = deformerParentIndices.cbegin(); it != deformerParentIndices.cend(); ++it)
        deformerParentObject.insert(it.key(), it.value());
    root.insert(QStringLiteral("deformerParentIndices"), deformerParentObject);
    QJsonObject rotationKeyformObject;
    for (auto it = deformerRotationKeyforms.cbegin(); it != deformerRotationKeyforms.cend(); ++it) {
        QJsonArray keyforms;
        for (const CModelRotationKeyform& keyform : it.value()) {
            QJsonObject value;
            value.insert(QStringLiteral("opacity"), static_cast<double>(keyform.opacity));
            value.insert(QStringLiteral("angle"), static_cast<double>(keyform.angle));
            value.insert(QStringLiteral("originX"), static_cast<double>(keyform.originX));
            value.insert(QStringLiteral("originY"), static_cast<double>(keyform.originY));
            value.insert(QStringLiteral("scale"), static_cast<double>(keyform.scale));
            value.insert(QStringLiteral("reflectX"), keyform.reflectX);
            value.insert(QStringLiteral("reflectY"), keyform.reflectY);
            keyforms.append(value);
        }
        rotationKeyformObject.insert(it.key(), keyforms);
    }
    root.insert(QStringLiteral("deformerRotationKeyforms"), rotationKeyformObject);
    QJsonObject warpKeyformObject;
    for (auto it = deformerWarpKeyforms.cbegin(); it != deformerWarpKeyforms.cend(); ++it) {
        QJsonArray keyforms;
        for (const CModelWarpKeyform& keyform : it.value()) {
            QJsonObject value;
            value.insert(QStringLiteral("opacity"), static_cast<double>(keyform.opacity));
            QJsonArray positions;
            for (float position : keyform.positions) positions.append(static_cast<double>(position));
            value.insert(QStringLiteral("positions"), positions);
            keyforms.append(value);
        }
        warpKeyformObject.insert(it.key(), keyforms);
    }
    root.insert(QStringLiteral("deformerWarpKeyforms"), warpKeyformObject);
    QJsonObject maskInvertedObject;
    for (auto it = drawableMaskInverted.cbegin(); it != drawableMaskInverted.cend(); ++it)
        maskInvertedObject.insert(it.key(), it.value());
    root.insert(QStringLiteral("drawableMaskInverted"), maskInvertedObject);
    QJsonObject view;
    view.insert(QStringLiteral("scale"), static_cast<double>(viewScale));
    view.insert(QStringLiteral("x"), viewPosition.x());
    view.insert(QStringLiteral("y"), viewPosition.y());
    root.insert(QStringLiteral("view"), view);
    return QJsonDocument(root).toJson(QJsonDocument::Indented);
}

bool CModelSession::FromJson(const QByteArray& data, CModelSession* session, QString* error) try
{
    if (!session) {
        setError(error, QStringLiteral("会话目标为空"));
        return false;
    }
    if (data.size() > std::numeric_limits<int>::max()) {
        setError(error, QStringLiteral("会话文件长度超出范围"));
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(data, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        setError(error, parseError.errorString());
        return false;
    }
    const QJsonObject root = document.object();
    double versionValue = 1.0;
    if (!root.value(QStringLiteral("version")).isUndefined() &&
        (!readNumber(root.value(QStringLiteral("version")), &versionValue) || std::floor(versionValue) != versionValue)) {
        setError(error, QStringLiteral("会话版本不是整数"));
        return false;
    }
    if (versionValue < 0.0 || versionValue > static_cast<double>(std::numeric_limits<int>::max())) {
        setError(error, QStringLiteral("会话版本超出范围"));
        return false;
    }
    const int parsedVersion = static_cast<int>(versionValue);
    if (parsedVersion < 1 || parsedVersion > 18) {
        setError(error, QStringLiteral("不支持的会话版本：%1").arg(parsedVersion));
        return false;
    }
    CModelSession result;
    result.version = parsedVersion;

    const auto blendShapeGeometry = root.value(QStringLiteral("blendShapeGeometry"));
    if ((parsedVersion < 16 && !blendShapeGeometry.isUndefined()) ||
        (parsedVersion >= 16 && !blendShapeGeometry.isArray())) {
        setError(error, QStringLiteral("BlendShape 几何状态需要版本 16 及数组")); return false;
    }
    if (parsedVersion >= 16) {
        const auto values = blendShapeGeometry.toArray();
        if (values.size() > MaximumBlendShapeGeometryEntries) { setError(error, QStringLiteral("BlendShape 几何条目数量超出限制")); return false; }
        qsizetype deltaCount = 0;
        for (const auto& value : values) {
            if (!value.isObject()) { setError(error, QStringLiteral("BlendShape 几何条目类型无效")); return false; }
            const auto object = value.toObject(); CModelBlendShapeGeometryEntry entry;
            if (!object.value(QStringLiteral("meshId")).isString()) { setError(error, QStringLiteral("BlendShape 几何目标无效")); return false; }
            entry.meshId = object.value(QStringLiteral("meshId")).toString();
            const auto readInt = [&](const QString& key, int& target) {
                double number = 0; if (!readNumber(object.value(key), &number) || std::floor(number) != number ||
                    number < 0 || number > std::numeric_limits<int>::max()) return false;
                target = int(number); return true;
            };
            if (!readInt(QStringLiteral("shapeIndex"), entry.shapeIndex) || !readInt(QStringLiteral("bindingOffset"), entry.bindingOffset) ||
                !readInt(QStringLiteral("keyformOffset"), entry.keyformOffset) || !readInt(QStringLiteral("sourceKeyformIndex"), entry.sourceKeyformIndex) ||
                !object.value(QStringLiteral("sourceDigest")).isString()) { setError(error, QStringLiteral("BlendShape 几何索引无效")); return false; }
            const QString encodedDigest = object.value(QStringLiteral("sourceDigest")).toString();
            entry.sourceDigest = QByteArray::fromHex(encodedDigest.toLatin1());
            if (encodedDigest.size() != 64 || entry.sourceDigest.size() != 32 ||
                QString::fromLatin1(entry.sourceDigest.toHex()).compare(encodedDigest, Qt::CaseInsensitive) != 0) {
                setError(error, QStringLiteral("BlendShape 几何来源指纹无效")); return false;
            }
            const auto deltas = object.value(QStringLiteral("deltas"));
            if (!deltas.isArray() || deltas.toArray().isEmpty() || deltas.toArray().size() % 2 ||
                deltas.toArray().size() > MaximumBlendShapeGeometryDeltas - deltaCount) {
                setError(error, QStringLiteral("BlendShape 几何坐标数组无效")); return false;
            }
            const auto coordinates = deltas.toArray();
            deltaCount += coordinates.size();
            entry.deltas.reserve(coordinates.size());
            for (const auto& delta : coordinates) {
                float number = 0;
                if (delta.isString() && delta.toString() == QStringLiteral("-0")) number = -0.0f;
                else if (!readFloat(delta, &number)) {
                    setError(error, QStringLiteral("BlendShape 几何坐标不是有限数")); return false;
                }
                entry.deltas.append(number);
            }
            result.blendShapeGeometry.append(std::move(entry));
        }
    }

    const auto objectNames = root.value(QStringLiteral("objectDisplayNames"));
    if ((parsedVersion < 15 && !objectNames.isUndefined()) || (parsedVersion >= 15 && !objectNames.isObject())) {
        setError(error, QStringLiteral("对象名称需要版本 15 及完整目录")); return false;
    }
    if (parsedVersion >= 15) {
        result.objectDisplayNames.emplace();
        if (!CModelDisplayNames::FromJson(objectNames.toObject(), *result.objectDisplayNames, error)) return false;
    }
    const auto names = root.value(QStringLiteral("deformerDisplayNames"));
    if (((parsedVersion < 13 || parsedVersion >= 15) && !names.isUndefined()) ||
        (parsedVersion >= 13 && parsedVersion < 15 && !names.isObject())) {
        setError(error, QStringLiteral("变形器名称需要版本 13 及有效对象")); return false;
    }
    if (parsedVersion >= 13 && parsedVersion < 15) {
        result.deformerDisplayNames.emplace();
        if (!CDeformerDisplayNames::FromJson(names.toObject(), *result.deformerDisplayNames, error)) return false;
    }
    const auto artwork = root.value(QStringLiteral("artwork"));
    if ((parsedVersion < 12 && !artwork.isUndefined()) || (parsedVersion >= 12 && !artwork.isObject())) {
        setError(error, QStringLiteral("素材关联需要版本 12 及有效对象")); return false;
    }
    if (parsedVersion >= 12 && !CModelArtwork::FromJson(artwork.toObject(), result.artwork, error)) return false;
    const auto workspace = root.value(QStringLiteral("textureWorkspace"));
    if ((parsedVersion < 11 && !workspace.isUndefined()) ||
        (parsedVersion >= 11 && !workspace.isObject() && !workspace.isNull())) {
        setError(error, QStringLiteral("图集工作区需要版本 11 及有效字段")); return false;
    }
    if (workspace.isObject()) {
        CTextureWorkspace::Draft draft;
        if (!CTextureWorkspace::FromJson(workspace.toObject(), draft, error)) return false;
        result.textureWorkspace = std::move(draft);
    }
    if ((parsedVersion < 10 && (root.contains(QStringLiteral("materialStructure")) || root.contains(QStringLiteral("drawableMaterials")))) ||
        (parsedVersion >= 10 && (!root.value(QStringLiteral("materialStructure")).isBool() || !root.value(QStringLiteral("drawableMaterials")).isObject()))) {
        setError(error, QStringLiteral("材质编辑需要版本 10 及有效字段")); return false;
    }
    result.materialStructure = root.value(QStringLiteral("materialStructure")).toBool();
    if (!CModelMaterialEdit::FromJson(root.value(QStringLiteral("drawableMaterials")).toObject(), result.drawableMaterials, error)) return false;
    if ((parsedVersion < 9 && root.contains(QStringLiteral("geometryObjectPairs"))) ||
        (parsedVersion >= 9 && !root.value(QStringLiteral("geometryObjectPairs")).isObject())) {
        setError(error, QStringLiteral("几何对象配对需要版本 9 及有效对象"));
        return false;
    }
    if (parsedVersion >= 9 && !CGeometryObjectPairs::FromJson(root.value(QStringLiteral("geometryObjectPairs")).toObject(),
                                                            result.geometryObjectPairs, error)) return false;
    if ((parsedVersion < 8 && root.contains(QStringLiteral("parameterOrganization"))) ||
        (parsedVersion >= 8 && !root.value(QStringLiteral("parameterOrganization")).isObject())) {
        setError(error, QStringLiteral("参数组织需要版本 8 及有效对象"));
        return false;
    }
    if (parsedVersion >= 8 && !CParameterOrganization::FromJson(root.value(QStringLiteral("parameterOrganization")).toObject(),
                                                              result.parameterOrganization, error)) return false;
    if ((parsedVersion < 5 && root.contains(QStringLiteral("meshStructure"))) ||
        (parsedVersion >= 5 && !root.value(QStringLiteral("meshStructure")).isBool())) {
        setError(error, QStringLiteral("会话 meshStructure 字段需要版本 5 且必须为布尔值"));
        return false;
    }
    result.meshStructure = root.value(QStringLiteral("meshStructure")).toBool(false);
    const auto cloneValue = root.value(QStringLiteral("artMeshClones"));
    if ((parsedVersion < 18 && !cloneValue.isUndefined()) || (parsedVersion >= 18 && !cloneValue.isArray())) {
        setError(error, QStringLiteral("ArtMesh 克隆目录需要版本 18 及有效数组"));
        return false;
    }
    if (parsedVersion >= 18) {
        const auto clones = cloneValue.toArray();
        if (clones.size() > 1000000) {
            setError(error, QStringLiteral("ArtMesh 克隆数量超出限制"));
            return false;
        }
        for (const auto& value : clones) {
            if (!value.isObject()) {
                setError(error, QStringLiteral("ArtMesh 克隆条目必须是对象"));
                return false;
            }
            const auto object = value.toObject();
            const auto readInteger = [](const QJsonValue& value, int* result) {
                double number = 0.0;
                if (!readNumber(value, &number) || std::floor(number) != number ||
                    number < std::numeric_limits<int>::min() || number > std::numeric_limits<int>::max()) return false;
                *result = static_cast<int>(number);
                return true;
            };
            int parentPart = -2, parentDeformer = -2, texture = -2;
            if (!object.value(QStringLiteral("sourceId")).isString() || !object.value(QStringLiteral("newId")).isString() ||
                !readInteger(object.value(QStringLiteral("parentPartIndex")), &parentPart) ||
                !readInteger(object.value(QStringLiteral("parentDeformerIndex")), &parentDeformer) ||
                !readInteger(object.value(QStringLiteral("textureIndex")), &texture)) {
                setError(error, QStringLiteral("ArtMesh 克隆条目字段类型无效"));
                return false;
            }
            result.artMeshClones.append({object.value(QStringLiteral("sourceId")).toString(),
                                         object.value(QStringLiteral("newId")).toString(), parentPart, parentDeformer, texture});
        }
    }
    if ((parsedVersion < 6 && (root.contains(QStringLiteral("deformerStructure")) || root.contains(QStringLiteral("sourceDeformerIds")))) ||
        (parsedVersion >= 6 && (!root.value(QStringLiteral("deformerStructure")).isBool() || !root.value(QStringLiteral("sourceDeformerIds")).isArray()))) {
        setError(error, QStringLiteral("变形器结构字段需要版本 6 及有效的能力与身份列表"));
        return false;
    }
    result.deformerStructure = root.value(QStringLiteral("deformerStructure")).toBool(false);
    const auto sourceIds = root.value(QStringLiteral("sourceDeformerIds")).toArray();
    if (sourceIds.size() > 1000000) { setError(error, QStringLiteral("变形器源身份数量超出范围")); return false; }
    for (const auto& value : sourceIds) {
        if (!value.isString() || result.sourceDeformerIds.contains(value.toString())) {
            setError(error, QStringLiteral("变形器源身份必须是不重复的字符串")); return false;
        }
        result.sourceDeformerIds.insert(value.toString());
    }
    if ((parsedVersion < 7 && root.contains(QStringLiteral("textures"))) ||
        (parsedVersion >= 7 && !root.value(QStringLiteral("textures")).isArray())) {
        setError(error, QStringLiteral("纹理覆盖需要版本 7 及有效数组"));
        return false;
    }
    if (!CTextureAssets::FromJson(root.value(QStringLiteral("textures")).toArray(), result.textures, error)) return false;
    QJsonObject resources;
    if (!readObject(root, QStringLiteral("parameterResources"), &resources, error) ||
        !CParameterResourceEdits::FromJson(resources, result.parameterResources, error)) return false;
    if (root.contains(QStringLiteral("physicsDraft"))) {
        QJsonObject draft;
        if (!readObject(root, QStringLiteral("physicsDraft"), &draft, error) ||
            !draft.value(QStringLiteral("sourcePath")).isString() || !draft.value(QStringLiteral("document")).isObject() ||
            !draft.value(QStringLiteral("dirty")).isBool()) {
            setError(error, QStringLiteral("会话物理草稿格式无效"));
            return false;
        }
        result.physicsDraft = PhysicsDraft{draft.value(QStringLiteral("sourcePath")).toString(),
            draft.value(QStringLiteral("document")).toObject(), draft.value(QStringLiteral("dirty")).toBool()};
    }
    if (parsedVersion < 3 && (!result.parameterResources.IsEmpty() || result.physicsDraft)) {
        setError(error, QStringLiteral("资源引用或物理草稿需要版本 3 会话"));
        return false;
    }
    if (!readStructure(root, result.structure, error)) return false;
    if (parsedVersion == 1 && (!result.structure.columns.isEmpty() || !result.structure.counts.isEmpty())) {
        setError(error, QStringLiteral("版本 1 会话不能包含参数结构"));
        return false;
    }
    if (!readObject(root, QStringLiteral("animation"), &result.animation, error)) return false;
    CMotionTimeline timeline;
    if (!timeline.RestoreState(result.animation, nullptr, error)) return false;
    const auto animationScenes = root.value(QStringLiteral("animationScenes"));
    const auto activeAnimationScene = root.value(QStringLiteral("activeAnimationScene"));
    if ((parsedVersion < 17 && (!animationScenes.isUndefined() || !activeAnimationScene.isUndefined())) ||
        (parsedVersion >= 17 && (!animationScenes.isArray() || !activeAnimationScene.isString()))) {
        setError(error, QStringLiteral("动画场景需要版本 17 及有效数组和活动身份"));
        return false;
    }
    if (parsedVersion >= 17) {
        result.animationScenes = animationScenes.toArray();
        result.activeAnimationScene = activeAnimationScene.toString();
        if (!validAnimationScenes(result, parsedVersion, error)) return false;
        for (const auto& value : result.animationScenes) {
            const auto scene = value.toObject();
            if (scene.value(QStringLiteral("id")).toString() == result.activeAnimationScene &&
                scene.value(QStringLiteral("state")).toObject() != result.animation) {
                setError(error, QStringLiteral("活动动画场景与兼容 animation 字段不一致"));
                return false;
            }
        }
    }
    const QJsonValue sourceMoc = root.value(QStringLiteral("sourceMoc"));
    if (!sourceMoc.isUndefined() && !sourceMoc.isString()) {
        setError(error, QStringLiteral("会话字段类型错误：sourceMoc"));
        return false;
    }
    result.sourceMoc = sourceMoc.toString();
    const QJsonValue sourceDigest = root.value(QStringLiteral("sourceDigest"));
    if (!sourceDigest.isUndefined()) {
        const QString encoded = sourceDigest.toString();
        const QByteArray decoded = QByteArray::fromHex(encoded.toLatin1());
        if (!sourceDigest.isString() || encoded.size() != 64 || decoded.size() != QCryptographicHash::hashLength(QCryptographicHash::Sha256) ||
            QString::fromLatin1(decoded.toHex()).compare(encoded, Qt::CaseInsensitive) != 0) {
            setError(error, QStringLiteral("会话源文件指纹无效"));
            return false;
        }
        result.sourceDigest = decoded;
    }
    if (!validDisplayNames(result, parsedVersion, error) ||
        !validBlendShapeGeometry(result, parsedVersion, error) ||
        !validArtMeshClones(result, parsedVersion, error)) return false;
    if (!result.textures.isEmpty() && (result.sourceMoc.isEmpty() || result.sourceMoc.contains(QChar::Null) || result.sourceDigest.size() != 32)) {
        setError(error, QStringLiteral("会话纹理覆盖缺少模型来源"));
        return false;
    }
    if (result.parameterOrganization.configured && (result.sourceDigest.size() != 32 || result.sourceMoc.contains(QChar::Null))) {
        setError(error, QStringLiteral("会话参数组织缺少有效模型来源"));
        return false;
    }
    if (!result.geometryObjectPairs.sourceDigest.isEmpty() &&
        (result.geometryObjectPairs.sourceDigest != result.sourceDigest || result.sourceDigest.size() != 32 || result.sourceMoc.contains(QChar::Null))) {
        setError(error, QStringLiteral("会话几何对象配对与模型来源不匹配"));
        return false;
    }
    if (!validArtwork(result, parsedVersion, error) || !validWorkspace(result, parsedVersion, error) || !validMaterials(result, parsedVersion, error) || !validPhysicsDraft(result, parsedVersion, error) || !validMeshStructure(result, parsedVersion, error) || !validDeformerStructure(result, parsedVersion, error)) return false;

    QJsonObject parameterObject;
    QJsonObject parameterMetadataObject;
    QJsonObject visibilityObject;
    QJsonObject lockedObject;
    QJsonObject opacityObject;
    QJsonObject drawOrderObject;
    QJsonObject keyformObject;
    QJsonObject indexObject;
    QJsonObject maskObject;
    QJsonObject deformerParentObject;
    QJsonObject rotationKeyformObject;
    QJsonObject warpKeyformObject;
    QJsonObject maskInvertedObject;
    if (!readObject(root, QStringLiteral("parameters"), &parameterObject, error) ||
        !readObject(root, QStringLiteral("parameterMetadata"), &parameterMetadataObject, error) ||
        !readObject(root, QStringLiteral("visibility"), &visibilityObject, error) ||
        !readObject(root, QStringLiteral("locked"), &lockedObject, error) ||
        !readObject(root, QStringLiteral("drawableOpacity"), &opacityObject, error) ||
        !readObject(root, QStringLiteral("drawableDrawOrder"), &drawOrderObject, error) ||
        !readObject(root, QStringLiteral("drawableKeyformPositions"), &keyformObject, error) ||
        !readObject(root, QStringLiteral("drawableIndices"), &indexObject, error) ||
        !readObject(root, QStringLiteral("drawableMaskIndices"), &maskObject, error) ||
        !readObject(root, QStringLiteral("deformerParentIndices"), &deformerParentObject, error) ||
        !readObject(root, QStringLiteral("deformerRotationKeyforms"), &rotationKeyformObject, error) ||
        !readObject(root, QStringLiteral("deformerWarpKeyforms"), &warpKeyformObject, error) ||
        !readObject(root, QStringLiteral("drawableMaskInverted"), &maskInvertedObject, error)) return false;
    for (const QString& key : parameterObject.keys()) {
        float value = 0.0f;
        if (!readFloat(parameterObject.value(key), &value)) {
            setError(error, QStringLiteral("参数值不是有限数字：%1").arg(key));
            return false;
        }
        result.parameters.insert(key, value);
    }
    for (const QString& key : parameterMetadataObject.keys()) {
        const QJsonValue metadataValue = parameterMetadataObject.value(key);
        if (!metadataValue.isObject()) {
            setError(error, QStringLiteral("参数元数据类型错误：%1").arg(key));
            return false;
        }
        const QJsonObject metadataObject = metadataValue.toObject();
        CModelParameterMetadata metadata;
        if (!metadataObject.value(QStringLiteral("displayName")).isUndefined() &&
            !metadataObject.value(QStringLiteral("displayName")).isString()) {
            setError(error, QStringLiteral("参数显示名类型错误：%1").arg(key));
            return false;
        }
        metadata.displayName = metadataObject.value(QStringLiteral("displayName")).toString();
        if (!readFloat(metadataObject.value(QStringLiteral("minimum")), &metadata.minimum) ||
            !readFloat(metadataObject.value(QStringLiteral("maximum")), &metadata.maximum) ||
            !readFloat(metadataObject.value(QStringLiteral("defaultValue")), &metadata.defaultValue) ||
            metadata.minimum > metadata.maximum || metadata.defaultValue < metadata.minimum ||
            metadata.defaultValue > metadata.maximum) {
            setError(error, QStringLiteral("参数元数据范围无效：%1").arg(key));
            return false;
        }
        const QJsonValue decimals = metadataObject.value(QStringLiteral("decimalPlaces"));
        double decimalsNumber = 0.0;
        if (!readNumber(decimals, &decimalsNumber) || std::floor(decimalsNumber) != decimalsNumber ||
            decimalsNumber < 0.0 || decimalsNumber > 9.0) {
            setError(error, QStringLiteral("参数元数据小数位无效：%1").arg(key));
            return false;
        }
        metadata.decimalPlaces = static_cast<int>(decimalsNumber);
        const QJsonValue repeat = metadataObject.value(QStringLiteral("repeat"));
        if (!repeat.isBool()) {
            setError(error, QStringLiteral("参数元数据循环标记无效：%1").arg(key));
            return false;
        }
        metadata.repeat = repeat.toBool();
        result.parameterMetadata.insert(key, metadata);
    }
    for (const QString& key : visibilityObject.keys()) {
        if (!visibilityObject.value(key).isBool()) {
            setError(error, QStringLiteral("可见性值类型错误：%1").arg(key));
            return false;
        }
        result.visibility.insert(key, visibilityObject.value(key).toBool());
    }
    for (const QString& key : lockedObject.keys()) {
        if (!lockedObject.value(key).isBool()) {
            setError(error, QStringLiteral("锁定值类型错误：%1").arg(key));
            return false;
        }
        result.locked.insert(key, lockedObject.value(key).toBool());
    }
    for (const QString& key : opacityObject.keys()) {
        double value = 0.0;
        if (!readNumber(opacityObject.value(key), &value) || value < 0.0 || value > 1.0) {
            setError(error, QStringLiteral("透明度超出范围：%1").arg(key));
            return false;
        }
        result.drawableOpacity.insert(key, static_cast<float>(value));
    }
    for (const QString& key : drawOrderObject.keys()) {
        double value = 0.0;
        if (!readNumber(drawOrderObject.value(key), &value) || std::floor(value) != value || value < 0.0 || value > 1000.0) {
            setError(error, QStringLiteral("绘制顺序超出范围：%1").arg(key));
            return false;
        }
        result.drawableDrawOrder.insert(key, static_cast<int>(value));
    }
    constexpr int kMaxKeyformMeshes = 100000;
    constexpr int kMaxKeyformsPerMesh = 10000;
    constexpr int kMaxPositionsPerKeyform = 2000000;
    if (keyformObject.size() > kMaxKeyformMeshes) {
        setError(error, QStringLiteral("关键形网格数量超出限制"));
        return false;
    }
    for (const QString& key : keyformObject.keys()) {
        const QJsonValue meshValue = keyformObject.value(key);
        if (!meshValue.isArray() || meshValue.toArray().size() > kMaxKeyformsPerMesh) {
            setError(error, QStringLiteral("关键形数组类型或数量错误：%1").arg(key));
            return false;
        }
        QVector<QVector<float>> keyforms;
        const QJsonArray keyformArray = meshValue.toArray();
        keyforms.reserve(keyformArray.size());
        for (const QJsonValue& keyformValue : keyformArray) {
            if (!keyformValue.isArray() || keyformValue.toArray().size() > kMaxPositionsPerKeyform) {
                setError(error, QStringLiteral("关键形位置数组类型或数量错误：%1").arg(key));
                return false;
            }
            QVector<float> positions;
            const QJsonArray positionArray = keyformValue.toArray();
            positions.reserve(positionArray.size());
            for (const QJsonValue& positionValue : positionArray) {
                float value = 0.0f;
                if (!readFloat(positionValue, &value)) {
                    setError(error, QStringLiteral("关键形位置不是有限数字：%1").arg(key));
                    return false;
                }
                positions.append(value);
            }
            keyforms.append(positions);
        }
        result.drawableKeyformPositions.insert(key, keyforms);
    }
    constexpr int kMaxIndexMeshes = 100000;
    constexpr int kMaxIndicesPerMesh = 3000000;
    if (indexObject.size() > kMaxIndexMeshes) {
        setError(error, QStringLiteral("索引覆盖网格数量超出限制"));
        return false;
    }
    for (const QString& key : indexObject.keys()) {
        const QJsonValue meshValue = indexObject.value(key);
        if (!meshValue.isArray() || meshValue.toArray().isEmpty() || meshValue.toArray().size() % 3 != 0 ||
            meshValue.toArray().size() > kMaxIndicesPerMesh) {
            setError(error, QStringLiteral("索引覆盖数组类型或数量错误：%1").arg(key));
            return false;
        }
        QVector<quint16> indices;
        indices.reserve(meshValue.toArray().size());
        for (const QJsonValue& value : meshValue.toArray()) {
            double number = 0.0;
            if (!readNumber(value, &number) || std::floor(number) != number || number < 0.0 || number > CMeshTopology::MaximumIndex) {
                setError(error, QStringLiteral("索引覆盖值无效：%1").arg(key));
                return false;
            }
            indices.append(static_cast<quint16>(number));
        }
        result.drawableIndices.insert(key, indices);
    }
    constexpr int kMaxMaskMeshes = 100000;
    constexpr int kMaxMasksPerMesh = CMaskEditPolicy::MaximumOverrideReferences;
    if (maskObject.size() > kMaxMaskMeshes) {
        setError(error, QStringLiteral("蒙版覆盖网格数量超出限制"));
        return false;
    }
    for (const QString& key : maskObject.keys()) {
        const QJsonValue meshValue = maskObject.value(key);
        if (!meshValue.isArray() || meshValue.toArray().size() > kMaxMasksPerMesh) {
            setError(error, QStringLiteral("蒙版覆盖数组类型或数量错误：%1").arg(key));
            return false;
        }
        QVector<int> masks;
        QSet<int> unique;
        for (const QJsonValue& value : meshValue.toArray()) {
            double number = 0.0;
            if (!readNumber(value, &number) || std::floor(number) != number || number < 0.0 ||
                number > static_cast<double>(std::numeric_limits<int>::max()) ||
                (parsedVersion < 14 && unique.contains(static_cast<int>(number)))) {
                setError(error, QStringLiteral("蒙版覆盖值无效：%1").arg(key));
                return false;
            }
            masks.append(static_cast<int>(number));
            unique.insert(masks.last());
        }
        result.drawableMaskIndices.insert(key, masks);
    }
    constexpr int kMaxDeformerParents = 100000;
    if (deformerParentObject.size() > kMaxDeformerParents) {
        setError(error, QStringLiteral("变形器父级覆盖数量超出限制"));
        return false;
    }
    for (const QString& key : deformerParentObject.keys()) {
        double number = 0.0;
        if (!readNumber(deformerParentObject.value(key), &number) || std::floor(number) != number ||
            number < -1.0 || number > static_cast<double>(std::numeric_limits<int>::max())) {
            setError(error, QStringLiteral("变形器父级覆盖值无效：%1").arg(key));
            return false;
        }
        result.deformerParentIndices.insert(key, static_cast<int>(number));
    }
    if (rotationKeyformObject.size() > 100000) {
        setError(error, QStringLiteral("Rotation Deformer 覆盖数量超出限制"));
        return false;
    }
    for (const QString& key : rotationKeyformObject.keys()) {
        const QJsonValue keyformValue = rotationKeyformObject.value(key);
        if (!keyformValue.isArray() || keyformValue.toArray().isEmpty() || keyformValue.toArray().size() > 10000) {
            setError(error, QStringLiteral("Rotation Deformer 关键形数组无效：%1").arg(key));
            return false;
        }
        QVector<CModelRotationKeyform> keyforms;
        for (const QJsonValue& value : keyformValue.toArray()) {
            if (!value.isObject()) {
                setError(error, QStringLiteral("Rotation Deformer 关键形不是对象：%1").arg(key));
                return false;
            }
            const QJsonObject object = value.toObject();
            CModelRotationKeyform keyform;
            if (!readFloat(object.value(QStringLiteral("opacity")), &keyform.opacity) ||
                !readFloat(object.value(QStringLiteral("angle")), &keyform.angle) ||
                !readFloat(object.value(QStringLiteral("originX")), &keyform.originX) ||
                !readFloat(object.value(QStringLiteral("originY")), &keyform.originY) ||
                !readFloat(object.value(QStringLiteral("scale")), &keyform.scale) ||
                !keyform.IsValid() ||
                !object.value(QStringLiteral("reflectX")).isBool() ||
                !object.value(QStringLiteral("reflectY")).isBool()) {
                setError(error, QStringLiteral("Rotation Deformer 关键形数值无效：%1").arg(key));
                return false;
            }
            keyform.reflectX = object.value(QStringLiteral("reflectX")).toBool();
            keyform.reflectY = object.value(QStringLiteral("reflectY")).toBool();
            keyforms.append(keyform);
        }
        result.deformerRotationKeyforms.insert(key, keyforms);
    }
    if (warpKeyformObject.size() > 100000) {
        setError(error, QStringLiteral("Warp Deformer 覆盖数量超出限制"));
        return false;
    }
    for (const QString& key : warpKeyformObject.keys()) {
        const QJsonValue keyformValue = warpKeyformObject.value(key);
        if (!keyformValue.isArray() || keyformValue.toArray().isEmpty() || keyformValue.toArray().size() > 10000) {
            setError(error, QStringLiteral("Warp Deformer 关键形数组无效：%1").arg(key));
            return false;
        }
        QVector<CModelWarpKeyform> keyforms;
        for (const QJsonValue& value : keyformValue.toArray()) {
            if (!value.isObject()) {
                setError(error, QStringLiteral("Warp Deformer 关键形不是对象：%1").arg(key));
                return false;
            }
            const QJsonObject object = value.toObject();
            CModelWarpKeyform keyform;
            if (!readFloat(object.value(QStringLiteral("opacity")), &keyform.opacity) ||
                keyform.opacity < 0.0f || keyform.opacity > 1.0f ||
                !object.value(QStringLiteral("positions")).isArray() ||
                object.value(QStringLiteral("positions")).toArray().size() > 2000000) {
                setError(error, QStringLiteral("Warp Deformer 关键形数值无效：%1").arg(key));
                return false;
            }
            for (const QJsonValue& position : object.value(QStringLiteral("positions")).toArray()) {
                float number = 0.0f;
                if (!readFloat(position, &number)) {
                    setError(error, QStringLiteral("Warp Deformer 关键形坐标无效：%1").arg(key));
                    return false;
                }
                keyform.positions.append(number);
            }
            keyforms.append(keyform);
        }
        result.deformerWarpKeyforms.insert(key, keyforms);
    }
    for (const QString& key : maskInvertedObject.keys()) {
        if (!maskInvertedObject.value(key).isBool()) {
            setError(error, QStringLiteral("反向蒙版值类型错误：%1").arg(key));
            return false;
        }
        result.drawableMaskInverted.insert(key, maskInvertedObject.value(key).toBool());
    }
    QJsonObject view;
    if (!readObject(root, QStringLiteral("view"), &view, error)) return false;
    double scale = 1.0;
    if (!view.value(QStringLiteral("scale")).isUndefined() &&
        (!readNumber(view.value(QStringLiteral("scale")), &scale) || scale <= 0.0)) {
        setError(error, QStringLiteral("视图缩放值无效"));
        return false;
    }
    double x = 0.0;
    double y = 0.0;
    if ((!view.value(QStringLiteral("x")).isUndefined() && !readNumber(view.value(QStringLiteral("x")), &x)) ||
        (!view.value(QStringLiteral("y")).isUndefined() && !readNumber(view.value(QStringLiteral("y")), &y))) {
        setError(error, QStringLiteral("视图位置值无效"));
        return false;
    }
    if (scale > static_cast<double>(std::numeric_limits<float>::max()) ||
        std::abs(x) > static_cast<double>(std::numeric_limits<float>::max()) ||
        std::abs(y) > static_cast<double>(std::numeric_limits<float>::max())) {
        setError(error, QStringLiteral("视图数值超出范围"));
        return false;
    }
    result.viewScale = static_cast<float>(scale);
    result.viewPosition = QPointF(x, y);
    *session = std::move(result);
    return true;
}
catch (const std::exception&)
{
    setError(error, QStringLiteral("无法读取会话数据"));
    return false;
}

bool CModelSession::RestoreDocument(const CModelDocument& loaded, CModelDocument& result, QString* error) const
{
    return RestoreDocumentImpl(loaded, nullptr, result, error);
}

bool CModelSession::RestorePreparedDocument(const CModelDocument& loaded, const CModelDocument& prepared,
                                          CModelDocument& result, QString* error) const
{
    if (!prepared.IsLoaded() || !meshStructure || !prepared.HasMeshStructure()
        || prepared.SourceBytes() != loaded.SourceBytes() || prepared.SourceDigest() != loaded.SourceDigest()
        || prepared.SourcePath() != loaded.SourcePath() || prepared.ParameterStructure() != structure
        || prepared.HasMaterialStructure() != materialStructure || prepared.HasDeformerStructure() != deformerStructure
        || (deformerStructure ? prepared.SourceDeformerIds() : QSet<QString> {}) != sourceDeformerIds
        || prepared.HasTextureWorkspace() != loaded.HasTextureWorkspace()
        || prepared.TextureSources() != loaded.TextureSources() || !textures.isEmpty()
        || !animation.isEmpty() || !animationScenes.isEmpty() || !parameterResources.IsEmpty() || physicsDraft) {
        setError(error, QStringLiteral("已准备的网格结构与会话来源或结构状态不匹配"));
        return false;
    }
    return RestoreDocumentImpl(loaded, &prepared, result, error);
}

bool CModelSession::RestoreDocumentImpl(const CModelDocument& loaded, const CModelDocument* prepared,
                                      CModelDocument& result, QString* error) const try
{
    const auto fail = [error](const QString& field, const QString& id = QString()) {
        setError(error, id.isEmpty() ? QStringLiteral("会话字段无效：%1").arg(field)
                                    : QStringLiteral("会话字段无效：%1 / %2").arg(field, id));
        return false;
    };
    if ((version < 1 || version > 18) ||
        (version == 1 && (!structure.columns.isEmpty() || !structure.counts.isEmpty())) ||
        (version < 3 && (!parameterResources.IsEmpty() || physicsDraft)) || (version < 7 && !textures.isEmpty()) ||
        (version < 8 && parameterOrganization.configured) ||
        (version < 9 && geometryObjectPairs != CGeometryObjectPairs::State{}) ||
        (version < 10 && (materialStructure || !drawableMaterials.isEmpty())) || (version < 11 && textureWorkspace) ||
        (version < 12 && !artwork.IsEmpty()) || (version < 18 && !artMeshClones.isEmpty()) ||
        !validAnimationScenes(*this, version, error))
        return fail(QStringLiteral("version"));
    if (!validDisplayNames(*this, version, error)) return false;
    CGeometryObjectPairs::State normalizedPairs;
    if (!CGeometryObjectPairs::Normalize(geometryObjectPairs, normalizedPairs, error) ||
        (!geometryObjectPairs.sourceDigest.isEmpty() && (sourceDigest.size() != 32 ||
         geometryObjectPairs.sourceDigest != sourceDigest || sourceMoc.contains(QChar::Null)))) return fail(QStringLiteral("geometryObjectPairs"));
    CParameterOrganization::State normalizedOrganization;
    if (!CParameterOrganization::Normalize(parameterOrganization, normalizedOrganization, error) ||
        (parameterOrganization.configured && (sourceDigest.size() != 32 || sourceMoc.contains(QChar::Null)))) return fail(QStringLiteral("parameterOrganization"));
    if (!validArtwork(*this, version, error) || !validWorkspace(*this, version, error) || !validMaterials(*this, version, error) || !validStructure(structure, error) || !CParameterResourceEdits::Validate(parameterResources, error) ||
        !validPhysicsDraft(*this, version, error) || !validMeshStructure(*this, version, error) || !validArtMeshClones(*this, version, error) || !validDeformerStructure(*this, version, error) ||
        !validBlendShapeGeometry(*this, version, error)) return false;
    if (!CTextureAssets::ValidateDraft(textures, error)) return false;
    if (!textures.isEmpty() && (sourceMoc.isEmpty() || sourceMoc.contains(QChar::Null) || sourceDigest.size() != 32)) return fail(QStringLiteral("textures"));
    if (!loaded.IsLoaded() || loaded.SourceBytes().isEmpty()) {
        setError(error, QStringLiteral("没有可恢复会话的模型快照"));
        return false;
    }
    if (!sourceDigest.isEmpty() && (sourceDigest.size() != 32 || sourceDigest != loaded.SourceDigest())) {
        setError(error, QStringLiteral("会话 sourceDigest 与模型快照不匹配"));
        return false;
    }
    QJsonObject effectiveAnimation = animation;
    for (const auto& value : animationScenes) {
        const auto scene = value.toObject();
        if (scene.value(QStringLiteral("id")).toString() == activeAnimationScene)
            effectiveAnimation = scene.value(QStringLiteral("state")).toObject();
    }
    CMotionTimeline timeline;
    if (!timeline.RestoreState(effectiveAnimation, nullptr, error)) return false;
    if (!std::isfinite(viewScale) || viewScale <= 0.0f ||
        !std::isfinite(viewPosition.x()) || !std::isfinite(viewPosition.y()) ||
        std::abs(viewPosition.x()) > std::numeric_limits<float>::max() ||
        std::abs(viewPosition.y()) > std::numeric_limits<float>::max()) return fail(QStringLiteral("view"));

    const struct { const char* name; qsizetype count; } limitedMaps[] = {
        {"drawableKeyformPositions", drawableKeyformPositions.size()},
        {"drawableIndices", drawableIndices.size()},
        {"drawableMaskIndices", drawableMaskIndices.size()},
        {"deformerParentIndices", deformerParentIndices.size()},
        {"deformerRotationKeyforms", deformerRotationKeyforms.size()},
        {"deformerWarpKeyforms", deformerWarpKeyforms.size()}
    };
    for (const auto& map : limitedMaps)
        if (map.count > 100000) return fail(QString::fromLatin1(map.name));

    CModelDocument candidate;
    QSet<QString> removedParameters;
    if (prepared) {
        candidate = *prepared;
        auto sourceReader = CReader::FromBytes(loaded.SourceBytes());
        CSectionTable sourceTable(sourceReader);
        if (!sourceTable.IsValid()) return fail(QStringLiteral("preparedSource"));
        for (const auto* parameter : sourceTable.parameters) removedParameters.insert(parameter->ids);
        if (bool(textureWorkspace) != loaded.HasTextureWorkspace()) return fail(QStringLiteral("textureWorkspace"));
        if (textureWorkspace) {
            CTextureWorkspace::State restored;
            if (!CTextureWorkspace::RestoreDraft(loaded.TextureSources(), *textureWorkspace, restored, error)
                || restored != candidate.TextureWorkspace()) return fail(QStringLiteral("textureWorkspace"));
        }
    } else {
        if (!candidate.LoadBytes(loaded.SourceBytes(), loaded.SourcePath(), error)) return false;
        if (textureWorkspace && !loaded.HasTextureWorkspace()) return fail(QStringLiteral("textureWorkspace"));
        if (loaded.HasTextureWorkspace()) {
            CTextureAssets::State legacy;
            if (!CTextureAssets::RestoreDraft(loaded.TextureSources(), textures, legacy, error) ||
                !candidate.InitializeTextureWorkspace(loaded.TextureSources(), legacy, error)) return false;
            if (textureWorkspace) {
                CTextureWorkspace::State restored;
                if (!CTextureWorkspace::RestoreDraft(loaded.TextureSources(), *textureWorkspace, restored, error) ||
                    !candidate.SetTextureWorkspace(restored, error)) return false;
            }
        }
        for (const auto& parameter : candidate.Parameters()) removedParameters.insert(parameter.id);
        if (meshStructure && !artMeshClones.isEmpty()) {
            if (!candidate.SetSessionAuthoringStructure(structure, artMeshClones, materialStructure, deformerStructure,
                                                        sourceDeformerIds, error)) return false;
        } else if (materialStructure) {
            if (!candidate.SetMaterialStructure(structure, meshStructure, deformerStructure, sourceDeformerIds, error)) return false;
        } else if (deformerStructure) {
            if (!candidate.SetDeformerStructure(structure, meshStructure, sourceDeformerIds, error)) return false;
        } else if (meshStructure) {
            if (!candidate.SetAuthoringStructure(structure, error)) return false;
        } else if ((!structure.columns.isEmpty() || !structure.counts.isEmpty()) && !candidate.SetParameterStructure(structure, error)) return false;
    }
    CModelMaterialState materialValues;
    auto names = objectDisplayNames.value_or(loaded.DisplayNames());
    if (!objectDisplayNames) {
        const auto retain = [](const auto& objects, auto& entries) {
            QSet<QString> current;
            for (const auto& object : objects) current.insert(object.id);
            for (auto item = entries.begin(); item != entries.end();)
                if (!current.contains(item.key())) item = entries.erase(item); else ++item;
        };
        retain(candidate.Parts(), names.parts);
        retain(candidate.Drawables(), names.artMeshes);
        if (deformerDisplayNames) names.deformers = *deformerDisplayNames;
    }
    if (!objectDisplayNames && !deformerDisplayNames) {
        QSet<QString> currentIds;
        for (const auto& object : candidate.Deformers()) currentIds.insert(object.id);
        for (auto item = names.deformers.begin(); item != names.deformers.end();)
            if (!currentIds.contains(item.key()) || (candidate.HasDeformerStructure() && !candidate.SourceDeformerIds().contains(item.key())))
                item = names.deformers.erase(item);
            else ++item;
    }
    if (!candidate.SetDisplayNames(names, error)) return false;
    for (const auto& material : drawableMaterials) materialValues.append(material);
    if (!candidate.SetDrawableMaterials(materialValues, error)) return false;
    if (!candidate.SetParameterOrganization(normalizedOrganization, error)) return false;
    for (const auto& parameter : candidate.Parameters()) removedParameters.remove(parameter.id);
    const auto validateTimelineParameters = [&](const CMotionTimeline& candidateTimeline, const QString& field) {
        for (auto track = candidateTimeline.Tracks().cbegin(); track != candidateTimeline.Tracks().cend(); ++track)
            if (track.key().target == CMotionTarget::Parameter && removedParameters.contains(track.key().id))
                return fail(field, track.key().id);
        return true;
    };
    if (!validateTimelineParameters(timeline, QStringLiteral("animation"))) return false;
    for (const auto& value : animationScenes) {
        const auto scene = value.toObject();
        CMotionTimeline sceneTimeline;
        if (!sceneTimeline.RestoreState(scene.value(QStringLiteral("state")).toObject(), nullptr, error)) return false;
        if (!validateTimelineParameters(sceneTimeline, QStringLiteral("animationScenes/%1").arg(scene.value(QStringLiteral("id")).toString()))) return false;
    }
    if (!removedParameters.isEmpty()) {
        const auto validateRemainingReferences = [&](CParameterReferenceEditor::Kind kind, const QByteArray& source,
                                                   const QSet<QString>& suppressed, const QString& field) {
            CParameterReferenceEditor::Prepared effective, checked;
            if (!CParameterReferenceEditor::Prepare(kind, source, suppressed, effective, error) ||
                !CParameterReferenceEditor::Prepare(kind, effective.effectiveBytes, removedParameters, checked, error))
                return false;
            return checked.references.isEmpty() || fail(field, checked.references.first().parameterId);
        };
        if (physicsDraft && !validateRemainingReferences(CParameterReferenceEditor::Kind::Physics,
            QJsonDocument(physicsDraft->document).toJson(QJsonDocument::Compact), {}, QStringLiteral("physicsDraft"))) return false;
        if (!parameterResources.IsEmpty()) {
            if (!validateRemainingReferences(CParameterReferenceEditor::Kind::ModelSettings, parameterResources.modelSource,
                parameterResources.modelSuppressedIds, QStringLiteral("parameterResources/modelSource"))) return false;
            for (auto resource = parameterResources.resources.cbegin(); resource != parameterResources.resources.cend(); ++resource)
                if (!validateRemainingReferences(resource->kind, resource->source, resource->suppressedIds,
                    QStringLiteral("parameterResources/%1").arg(resource.key()))) return false;
        }
    }
    QSet<QString> objectKeys;
    QHash<QString, const CModelDrawable*> meshes;
    for (const auto& part : candidate.Parts())
        if (!part.id.isEmpty()) objectKeys.insert(QStringLiteral("part|") + part.id);
    for (const auto& deformer : candidate.Deformers()) {
        const QString type = deformer.type == CModelDeformer::Warp ? QStringLiteral("warp|")
                                                                 : QStringLiteral("rotation|");
        if (!deformer.id.isEmpty()) objectKeys.insert(type + deformer.id);
    }
    for (const auto& mesh : candidate.Drawables()) {
        if (!mesh.id.isEmpty()) objectKeys.insert(QStringLiteral("artmesh|") + mesh.id);
        meshes.insert(mesh.id, &mesh);
    }
    for (auto it = visibility.cbegin(); it != visibility.cend(); ++it) {
        if (!objectKeys.contains(it.key())) return fail(QStringLiteral("visibility"), it.key());
        const int separator = it.key().indexOf(QLatin1Char('|'));
        const QString type = it.key().left(separator), id = it.key().mid(separator + 1);
        const bool applied = type == QStringLiteral("part") ? candidate.SetPartVisible(id, it.value())
                           : type == QStringLiteral("artmesh") ? candidate.SetDrawableVisible(id, it.value())
                                                                : candidate.SetDeformerVisible(id, it.value());
        if (!applied) return fail(QStringLiteral("visibility"), it.key());
    }
    for (auto it = parameterMetadata.cbegin(); it != parameterMetadata.cend(); ++it)
        if (!candidate.SetParameterMetadata(it.key(), it.value())) return fail(QStringLiteral("parameterMetadata"), it.key());
    for (auto it = parameters.cbegin(); it != parameters.cend(); ++it)
        if (!candidate.SetParameterValue(it.key(), it.value())) return fail(QStringLiteral("parameters"), it.key());
    for (auto it = drawableOpacity.cbegin(); it != drawableOpacity.cend(); ++it) {
        if (!std::isfinite(it.value()) || it.value() < 0.0f || it.value() > 1.0f ||
            !candidate.SetDrawableOpacity(it.key(), it.value())) return fail(QStringLiteral("drawableOpacity"), it.key());
    }
    for (auto it = drawableDrawOrder.cbegin(); it != drawableDrawOrder.cend(); ++it) {
        if (it.value() < 0 || it.value() > 1000 || !candidate.SetDrawableDrawOrder(it.key(), it.value()))
            return fail(QStringLiteral("drawableDrawOrder"), it.key());
    }
    for (auto it = drawableKeyformPositions.cbegin(); it != drawableKeyformPositions.cend(); ++it) {
        const auto mesh = meshes.value(it.key(), nullptr);
        if (!mesh || it.value().size() > mesh->keyforms.size() || it.value().size() > 10000)
            return fail(QStringLiteral("drawableKeyformPositions"), it.key());
        for (int index = 0; index < it.value().size(); ++index) {
            const auto& positions = it.value()[index];
            if (positions.isEmpty()) continue;
            if (positions.size() > 2000000 || !candidate.SetDrawableKeyformPositions(it.key(), index, positions))
                return fail(QStringLiteral("drawableKeyformPositions"), QStringLiteral("%1[%2]").arg(it.key()).arg(index));
        }
    }
    for (auto it = drawableIndices.cbegin(); it != drawableIndices.cend(); ++it)
        if (!candidate.SetDrawableIndices(it.key(), it.value())) return fail(QStringLiteral("drawableIndices"), it.key());
    for (auto it = drawableMaskIndices.cbegin(); it != drawableMaskIndices.cend(); ++it) {
        if (it.value().size() > CMaskEditPolicy::MaximumOverrideReferences)
            return fail(QStringLiteral("drawableMaskIndices"), it.key());
        QSet<int> unique;
        bool repeated = false;
        for (int index : it.value()) {
            repeated |= unique.contains(index);
            unique.insert(index);
        }
        const auto base = std::find_if(candidate.Drawables().cbegin(), candidate.Drawables().cend(),
            [&](const auto& drawable) { return drawable.id == it.key(); });
        if (version < 14 && repeated && (base == candidate.Drawables().cend() || it.value() != base->maskIndices))
            return fail(QStringLiteral("drawableMaskIndices"), it.key());
        if (!candidate.SetDrawableMaskIndices(it.key(), it.value()))
            return fail(QStringLiteral("drawableMaskIndices"), it.key());
    }
    for (auto it = drawableMaskInverted.cbegin(); it != drawableMaskInverted.cend(); ++it) {
        if (!candidate.SetDrawableMaskInverted(it.key(), it.value()))
            return fail(QStringLiteral("drawableMaskInverted"), it.key());
    }
    if (!candidate.SetDeformerParentIndices(deformerParentIndices)) return fail(QStringLiteral("deformerParentIndices"));
    for (auto it = deformerRotationKeyforms.cbegin(); it != deformerRotationKeyforms.cend(); ++it) {
        if (!candidate.SetDeformerRotationKeyforms(it.key(), it.value()))
            return fail(QStringLiteral("deformerRotationKeyforms"), it.key());
    }
    for (auto it = deformerWarpKeyforms.cbegin(); it != deformerWarpKeyforms.cend(); ++it) {
        if (!candidate.SetDeformerWarpKeyforms(it.key(), it.value()))
            return fail(QStringLiteral("deformerWarpKeyforms"), it.key());
    }
    if (!candidate.SetBlendShapeGeometry(blendShapeGeometry, error)) return false;
    if (!candidate.SetGeometryObjectPairs(normalizedPairs, error)) return false;
    for (auto it = locked.cbegin(); it != locked.cend(); ++it) {
        if (!objectKeys.contains(it.key())) return fail(QStringLiteral("locked"), it.key());
        const int separator = it.key().indexOf(QLatin1Char('|'));
        candidate.SetLocked(it.key().left(separator), it.key().mid(separator + 1), it.value());
    }
    if (!validArtwork(*this, version, error) || !candidate.SetArtwork(artwork, error)) return false;
    candidate.ClearEditHistory();
    result.CommitCandidate(std::move(candidate));
    setError(error, {});
    return true;
}
catch (const std::exception&)
{
    setError(error, QStringLiteral("无法恢复会话模型"));
    return false;
}

bool CModelSession::Save(const QString& path, QString* error) const
{
    const QByteArray data = ToJson();
    CModelSession validated;
    if (!FromJson(data, &validated, error)) return false;
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        setError(error, QStringLiteral("无法写入会话：%1").arg(path));
        return false;
    }
    if (file.write(data) != data.size() || !file.commit()) {
        setError(error, QStringLiteral("写入会话失败：%1").arg(path));
        return false;
    }
    return true;
}

QByteArray CModelSession::ComputeSourceDigest(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    const QByteArray data = file.readAll();
    if (file.error() != QFileDevice::NoError) return {};
    return QCryptographicHash::hash(data, QCryptographicHash::Sha256);
}

bool CModelSession::Load(const QString& path, CModelSession* session, QString* error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        setError(error, QStringLiteral("无法读取会话：%1").arg(path));
        return false;
    }
    if (file.size() > std::numeric_limits<int>::max()) {
        setError(error, QStringLiteral("会话文件长度超出范围"));
        return false;
    }
    const QByteArray data = file.readAll();
    if (file.error() != QFileDevice::NoError) {
        setError(error, QStringLiteral("读取会话失败：%1").arg(path));
        return false;
    }
    return FromJson(data, session, error);
}

}
