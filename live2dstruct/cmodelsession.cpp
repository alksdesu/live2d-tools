#include "cmodelsession.h"

#include <QFile>
#include <QSaveFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <cmath>

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
}

QByteArray CModelSession::ToJson() const
{
    QJsonObject root;
    root.insert(QStringLiteral("version"), version);
    root.insert(QStringLiteral("sourceMoc"), sourceMoc);
    QJsonObject parameterObject;
    for (auto it = parameters.cbegin(); it != parameters.cend(); ++it) parameterObject.insert(it.key(), static_cast<double>(it.value()));
    root.insert(QStringLiteral("parameters"), parameterObject);
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

    QJsonObject parameterObject;
    QJsonObject visibilityObject;
    QJsonObject lockedObject;
    QJsonObject opacityObject;
    QJsonObject drawOrderObject;
    QJsonObject keyformObject;
    if (!readObject(root, QStringLiteral("parameters"), &parameterObject, error) ||
        !readObject(root, QStringLiteral("visibility"), &visibilityObject, error) ||
        !readObject(root, QStringLiteral("locked"), &lockedObject, error) ||
        !readObject(root, QStringLiteral("drawableOpacity"), &opacityObject, error) ||
        !readObject(root, QStringLiteral("drawableDrawOrder"), &drawOrderObject, error) ||
        !readObject(root, QStringLiteral("drawableKeyformPositions"), &keyformObject, error)) return false;
    for (const QString& key : parameterObject.keys()) {
        double value = 0.0;
        if (!readNumber(parameterObject.value(key), &value)) {
            setError(error, QStringLiteral("参数值不是有限数字：%1").arg(key));
            return false;
        }
        result.parameters.insert(key, static_cast<float>(value));
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
                double value = 0.0;
                if (!readNumber(positionValue, &value)) {
                    setError(error, QStringLiteral("关键形位置不是有限数字：%1").arg(key));
                    return false;
                }
                positions.append(static_cast<float>(value));
            }
            keyforms.append(positions);
        }
        result.drawableKeyformPositions.insert(key, keyforms);
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
