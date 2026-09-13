#include "cmodelsession.h"

#include <QFile>
#include <QSaveFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QCryptographicHash>
#include <QSet>
#include <cmath>
#include <limits>

namespace live2dstruct {

namespace {
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
}

QByteArray CModelSession::ToJson() const
{
    QJsonObject root;
    root.insert(QStringLiteral("version"), version);
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

bool CModelSession::FromJson(const QByteArray& data, CModelSession* session, QString* error)
{
    if (!session) {
        setError(error, QStringLiteral("会话目标为空"));
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
    if (parsedVersion != 1) {
        setError(error, QStringLiteral("不支持的会话版本：%1").arg(parsedVersion));
        return false;
    }

    CModelSession result;
    result.version = parsedVersion;
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
    *session = result;
    return true;
}

bool CModelSession::Save(const QString& path, QString* error) const
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        setError(error, QStringLiteral("无法写入会话：%1").arg(path));
        return false;
    }
    const QByteArray data = ToJson();
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
    const QByteArray data = file.readAll();
    if (file.error() != QFileDevice::NoError) {
        setError(error, QStringLiteral("读取会话失败：%1").arg(path));
        return false;
    }
    return FromJson(data, session, error);
}

}
