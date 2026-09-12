#include "cmodelsession.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>

namespace live2dstruct {

namespace {
void setError(QString* error, const QString& value)
{
    if (error) *error = value;
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
    const int parsedVersion = root.value(QStringLiteral("version")).toInt(1);
    if (parsedVersion < 1 || parsedVersion > 1) {
        setError(error, QStringLiteral("不支持的会话版本：%1").arg(parsedVersion));
        return false;
    }

    CModelSession result;
    result.version = parsedVersion;
    result.sourceMoc = root.value(QStringLiteral("sourceMoc")).toString();
    const QJsonObject parameterObject = root.value(QStringLiteral("parameters")).toObject();
    for (const QString& key : parameterObject.keys()) result.parameters.insert(key, static_cast<float>(parameterObject.value(key).toDouble()));
    const QJsonObject visibilityObject = root.value(QStringLiteral("visibility")).toObject();
    for (const QString& key : visibilityObject.keys()) result.visibility.insert(key, visibilityObject.value(key).toBool());
    const QJsonObject view = root.value(QStringLiteral("view")).toObject();
    result.viewScale = static_cast<float>(view.value(QStringLiteral("scale")).toDouble(1.0));
    result.viewPosition = QPointF(view.value(QStringLiteral("x")).toDouble(), view.value(QStringLiteral("y")).toDouble());
    *session = result;
    return true;
}

bool CModelSession::Save(const QString& path, QString* error) const
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        setError(error, QStringLiteral("无法写入会话：%1").arg(path));
        return false;
    }
    if (file.write(ToJson()) < 0) {
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
    return FromJson(file.readAll(), session, error);
}

}
