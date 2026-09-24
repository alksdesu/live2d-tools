#include "cmodeluserdata.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSaveFile>
#include <QSet>

namespace live2dstruct {

namespace {
constexpr qint64 maximumDataSize = 16 * 1024 * 1024;
constexpr int maximumEntryCount = 100000;

void setError(QString* error, const QString& value)
{
    if (error) *error = value;
}
}

bool CModelUserData::Load(const QString& path, QString* error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        setError(error, file.errorString());
        return false;
    }
    if (file.size() > maximumDataSize) {
        setError(error, QStringLiteral("UserData 文件过大"));
        return false;
    }
    const QByteArray bytes = file.read(maximumDataSize + 1);
    if (file.error() != QFileDevice::NoError) {
        setError(error, file.errorString());
        return false;
    }
    return FromJson(bytes, error);
}

bool CModelUserData::FromJson(const QByteArray& bytes, QString* error)
{
    if (bytes.size() > maximumDataSize) {
        setError(error, QStringLiteral("UserData 文件过大"));
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        setError(error, QStringLiteral("userdata3 JSON 无效：%1").arg(parseError.errorString()));
        return false;
    }
    const QJsonObject root = document.object();
    if (root.value(QStringLiteral("Version")).toDouble(-1) != 3.0 ||
        !root.value(QStringLiteral("UserData")).isArray() ||
        (root.contains(QStringLiteral("Meta")) && !root.value(QStringLiteral("Meta")).isObject())) {
        setError(error, QStringLiteral("userdata3 版本或 UserData 字段无效"));
        return false;
    }

    QVector<CModelUserDataEntry> entries;
    const QJsonArray values = root.value(QStringLiteral("UserData")).toArray();
    if (values.size() > maximumEntryCount) {
        setError(error, QStringLiteral("UserData 条目过多"));
        return false;
    }
    entries.reserve(values.size());
    for (int index = 0; index < values.size(); ++index) {
        if (!values.at(index).isObject()) {
            setError(error, QStringLiteral("userdata3 第 %1 项不是对象").arg(index + 1));
            return false;
        }
        const QJsonObject object = values.at(index).toObject();
        const QJsonValue target = object.value(QStringLiteral("Target"));
        const QJsonValue id = object.value(QStringLiteral("Id"));
        const QJsonValue text = object.value(QStringLiteral("Value"));
        if (!target.isString() || !id.isString() || !text.isString()) {
            setError(error, QStringLiteral("userdata3 第 %1 项字段类型无效").arg(index + 1));
            return false;
        }
        entries.append({target.toString(), id.toString(), text.toString(), object});
    }

    CModelUserData candidate;
    candidate.SetEntries(entries);
    if (!candidate.Validate(error)) return false;
    m_entries.swap(entries);
    m_root = root;
    return true;
}

bool CModelUserData::Validate(QString* error) const
{
    if (m_entries.size() > maximumEntryCount) {
        setError(error, QStringLiteral("UserData 条目过多"));
        return false;
    }
    qint64 totalSize = 0;
    QSet<QString> keys;
    for (int index = 0; index < m_entries.size(); ++index) {
        const CModelUserDataEntry& entry = m_entries.at(index);
        if (entry.target != QStringLiteral("ArtMesh") || entry.id.trimmed().isEmpty() ||
            entry.id.contains(QChar::Null) || entry.value.contains(QChar::Null)) {
            setError(error, QStringLiteral("UserData 第 %1 项必须是有效的 ArtMesh ID").arg(index + 1));
            return false;
        }
        totalSize += entry.value.toUtf8().size();
        if (totalSize > maximumDataSize) {
            setError(error, QStringLiteral("UserData 第 %1 项内容过大").arg(index + 1));
            return false;
        }
        const QString key = entry.target + QLatin1Char('|') + entry.id;
        if (keys.contains(key)) {
            setError(error, QStringLiteral("UserData 存在重复目标：%1").arg(entry.id));
            return false;
        }
        keys.insert(key);
    }
    return true;
}

bool CModelUserData::Save(const QString& path, QString* error) const
{
    if (path.isEmpty()) {
        setError(error, QStringLiteral("UserData 保存路径为空"));
        return false;
    }
    if (!Validate(error)) return false;

    const QByteArray data = ToJson();
    if (data.size() > maximumDataSize) {
        setError(error, QStringLiteral("UserData 文件过大"));
        return false;
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) {
        setError(error, file.errorString());
        return false;
    }
    return true;
}

QByteArray CModelUserData::ToJson() const
{
    QJsonArray values;
    qint64 totalSize = 0;
    for (const CModelUserDataEntry& entry : m_entries) {
        QJsonObject object = entry.fields;
        object.insert(QStringLiteral("Target"), entry.target);
        object.insert(QStringLiteral("Id"), entry.id);
        object.insert(QStringLiteral("Value"), entry.value);
        values.append(object);
        totalSize += entry.value.toUtf8().size();
    }
    QJsonObject root = m_root;
    QJsonObject meta = root.value(QStringLiteral("Meta")).toObject();
    meta.insert(QStringLiteral("UserDataCount"), m_entries.size());
    meta.insert(QStringLiteral("TotalUserDataSize"), totalSize);
    root.insert(QStringLiteral("Version"), 3);
    root.insert(QStringLiteral("Meta"), meta);
    root.insert(QStringLiteral("UserData"), values);
    return QJsonDocument(root).toJson(QJsonDocument::Indented);
}

}
