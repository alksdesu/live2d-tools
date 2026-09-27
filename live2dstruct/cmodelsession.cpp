#include "cmodelsession.h"
#include "cmotiontimeline.h"

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

QByteArray CModelSession::ToJson() const
{
    if (version < 1 || version > 8) return {};
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
    root.insert(QStringLiteral("animation"), animation);
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
    if (parsedVersion < 1 || parsedVersion > 8) {
        setError(error, QStringLiteral("不支持的会话版本：%1").arg(parsedVersion));
        return false;
    }

    CModelSession result;
    result.version = parsedVersion;
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
    if (!result.textures.isEmpty() && (result.sourceMoc.isEmpty() || result.sourceMoc.contains(QChar::Null) || result.sourceDigest.size() != 32)) {
        setError(error, QStringLiteral("会话纹理覆盖缺少模型来源"));
        return false;
    }
    if (result.parameterOrganization.configured && (result.sourceDigest.size() != 32 || result.sourceMoc.contains(QChar::Null))) {
        setError(error, QStringLiteral("会话参数组织缺少有效模型来源"));
        return false;
    }
    if (!validPhysicsDraft(result, parsedVersion, error) || !validMeshStructure(result, parsedVersion, error) || !validDeformerStructure(result, parsedVersion, error)) return false;

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
            if (!readNumber(value, &number) || std::floor(number) != number || number < 0.0 || number > 32767.0) {
                setError(error, QStringLiteral("索引覆盖值无效：%1").arg(key));
                return false;
            }
            indices.append(static_cast<quint16>(number));
        }
        result.drawableIndices.insert(key, indices);
    }
    constexpr int kMaxMaskMeshes = 100000;
    constexpr int kMaxMasksPerMesh = 10000;
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
                number > static_cast<double>(std::numeric_limits<int>::max()) || unique.contains(static_cast<int>(number))) {
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

bool CModelSession::RestoreDocument(const CModelDocument& loaded, CModelDocument& result, QString* error) const try
{
    const auto fail = [error](const QString& field, const QString& id = QString()) {
        setError(error, id.isEmpty() ? QStringLiteral("会话字段无效：%1").arg(field)
                                    : QStringLiteral("会话字段无效：%1 / %2").arg(field, id));
        return false;
    };
    if ((version < 1 || version > 8) ||
        (version == 1 && (!structure.columns.isEmpty() || !structure.counts.isEmpty())) ||
        (version < 3 && (!parameterResources.IsEmpty() || physicsDraft)) || (version < 7 && !textures.isEmpty()) ||
        (version < 8 && parameterOrganization.configured))
        return fail(QStringLiteral("version"));
    CParameterOrganization::State normalizedOrganization;
    if (!CParameterOrganization::Normalize(parameterOrganization, normalizedOrganization, error) ||
        (parameterOrganization.configured && (sourceDigest.size() != 32 || sourceMoc.contains(QChar::Null)))) return fail(QStringLiteral("parameterOrganization"));
    if (!validStructure(structure, error) || !CParameterResourceEdits::Validate(parameterResources, error) ||
        !validPhysicsDraft(*this, version, error) || !validMeshStructure(*this, version, error) || !validDeformerStructure(*this, version, error)) return false;
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
    CMotionTimeline timeline;
    if (!timeline.RestoreState(animation, nullptr, error)) return false;
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
    if (!candidate.LoadBytes(loaded.SourceBytes(), loaded.SourcePath(), error)) return false;
    QSet<QString> removedParameters;
    for (const auto& parameter : candidate.Parameters()) removedParameters.insert(parameter.id);
    if (deformerStructure) {
        if (!candidate.SetDeformerStructure(structure, meshStructure, sourceDeformerIds, error)) return false;
    } else if (meshStructure) {
        if (!candidate.SetAuthoringStructure(structure, error)) return false;
    } else if ((!structure.columns.isEmpty() || !structure.counts.isEmpty()) && !candidate.SetParameterStructure(structure, error)) return false;
    if (!candidate.SetParameterOrganization(normalizedOrganization, error)) return false;
    for (const auto& parameter : candidate.Parameters()) removedParameters.remove(parameter.id);
    for (auto track = timeline.Tracks().cbegin(); track != timeline.Tracks().cend(); ++track)
        if (track.key().target == CMotionTarget::Parameter && removedParameters.contains(track.key().id))
            return fail(QStringLiteral("animation"), track.key().id);
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
        if (it.value().size() > 10000 || !candidate.SetDrawableMaskIndices(it.key(), it.value()))
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
    for (auto it = locked.cbegin(); it != locked.cend(); ++it) {
        if (!objectKeys.contains(it.key())) return fail(QStringLiteral("locked"), it.key());
        const int separator = it.key().indexOf(QLatin1Char('|'));
        candidate.SetLocked(it.key().left(separator), it.key().mid(separator + 1), it.value());
    }
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
