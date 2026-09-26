#include "cmodeljson.h"
#include "cmodelresourcepath.h"
#include <cmath>
#include <limits>

namespace {
bool validateFileReferences(const QJsonObject& root, QString* error)
{
    const auto invalid = [error](const QString& message) {
        if (error) *error = message;
        return false;
    };
    if (!root.value("FileReferences").isObject())
        return invalid(QStringLiteral("FileReferences 必须是对象"));
    const QJsonObject references = root.value("FileReferences").toObject();
    const QJsonValue moc = references.value("Moc");
    if (!moc.isString() || moc.toString().isEmpty() || moc.toString().contains(QChar::Null))
        return invalid(QStringLiteral("FileReferences.Moc 必须是非空且不含 NUL 的字符串"));
    if (references.contains("Textures")) {
        if (!references.value("Textures").isArray())
            return invalid(QStringLiteral("FileReferences.Textures 必须是数组"));
        const QJsonArray textures = references.value("Textures").toArray();
        for (qsizetype index = 0; index < textures.size(); ++index) {
            const QJsonValue texture = textures[index];
            if (!texture.isString() || texture.toString().contains(QChar::Null))
                return invalid(QStringLiteral("FileReferences.Textures[%1] 必须是不含 NUL 的字符串").arg(index));
        }
    }
    const auto validPath = [&](const QJsonObject& object, const QString& name, const QString& location) {
        if (!object.contains(name)) return true;
        const auto value = object.value(name);
        if (!value.isString() || value.toString().contains(QChar::Null))
            return invalid(QStringLiteral("%1.%2 必须是不含 NUL 的字符串").arg(location, name));
        return true;
    };
    for (const auto& field : {"Physics", "Pose", "UserData", "DisplayInfo"})
        if (!validPath(references, QLatin1String(field), QStringLiteral("FileReferences"))) return false;
    if (references.contains("Expressions")) {
        if (!references.value("Expressions").isArray()) return invalid(QStringLiteral("FileReferences.Expressions 必须是数组"));
        const auto expressions = references.value("Expressions").toArray();
        for (qsizetype index = 0; index < expressions.size(); ++index) {
            const auto location = QStringLiteral("FileReferences.Expressions[%1]").arg(index);
            if (!expressions[index].isObject()) return invalid(location + QStringLiteral(" 必须是对象"));
            const auto entry = expressions[index].toObject();
            if (!validPath(entry, QStringLiteral("File"), location) || !validPath(entry, QStringLiteral("Name"), location)) return false;
        }
    }
    if (references.contains("Motions")) {
        if (!references.value("Motions").isObject()) return invalid(QStringLiteral("FileReferences.Motions 必须是对象"));
        const auto motions = references.value("Motions").toObject();
        for (auto group = motions.constBegin(); group != motions.constEnd(); ++group) {
            const auto location = QStringLiteral("FileReferences.Motions.%1").arg(group.key());
            if (group.key().contains(QChar::Null) || !group->isArray()) return invalid(location + QStringLiteral(" 必须是名称不含 NUL 的动作数组"));
            const auto entries = group->toArray();
            for (qsizetype index = 0; index < entries.size(); ++index) {
                const auto item = QStringLiteral("%1[%2]").arg(location).arg(index);
                if (!entries[index].isObject()) return invalid(item + QStringLiteral(" 必须是对象"));
                const auto entry = entries[index].toObject();
                if (!validPath(entry, QStringLiteral("File"), item) || !validPath(entry, QStringLiteral("Sound"), item)) return false;
            }
        }
    }
    return true;
}
}

CModelJson::CModelJson() { Clear(); }

bool CModelJson::ValidateRuntimeSettings(const QByteArray& data, QString* error)
{
    if (error) error->clear();
    const auto invalid = [error](const QString& message) {
        if (error) *error = message;
        return false;
    };
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(data, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
        return invalid(QStringLiteral("model3.json 必须是有效的 JSON 对象"));
    const QJsonObject root = document.object();
    if (!validateFileReferences(root, error)) return false;
    if (root.contains("Layout")) {
        if (!root.value("Layout").isObject()) return invalid(QStringLiteral("Layout 必须是对象"));
        static const QStringList keys{"width", "height", "x", "y", "left", "right", "top", "bottom", "center_x", "center_y"};
        const QJsonObject layout = root.value("Layout").toObject();
        for (auto it = layout.constBegin(); it != layout.constEnd(); ++it) {
            if (!keys.contains(it.key())) continue;
            if (!it.value().isDouble() || !std::isfinite(it.value().toDouble()) ||
                std::abs(it.value().toDouble()) > std::numeric_limits<float>::max())
                return invalid(QStringLiteral("Layout.%1 必须是可表示为 float 的有限数字").arg(it.key()));
        }
    }
    if (root.contains("Groups")) {
        if (!root.value("Groups").isArray()) return invalid(QStringLiteral("Groups 必须是数组"));
        const QJsonArray groups = root.value("Groups").toArray();
        for (qsizetype index = 0; index < groups.size(); ++index) {
            if (!groups[index].isObject()) return invalid(QStringLiteral("Groups[%1] 必须是对象").arg(index));
            const QJsonObject group = groups[index].toObject();
            if (!group.value("Name").isString() || group.value("Name").toString().contains(QChar::Null))
                return invalid(QStringLiteral("Groups[%1].Name 必须是不含 NUL 的字符串").arg(index));
            if (!group.value("Ids").isArray()) return invalid(QStringLiteral("Groups[%1].Ids 必须是数组").arg(index));
            for (const QJsonValue& id : group.value("Ids").toArray())
                if (!id.isString() || id.toString().contains(QChar::Null))
                    return invalid(QStringLiteral("Groups[%1].Ids 必须只包含不含 NUL 的字符串").arg(index));
        }
    }
    return true;
}

void CModelJson::Clear()
{
    m_IsOpen = false;
    m_parentPath.clear();
    m_modelName.clear();
    m_moc.clear();
    m_DisplayInfo.clear();
    m_Physics.clear();
    m_Pose.clear();
    m_UserData.clear();
    m_Expressions.clear();
    m_Textures.clear();
    m_Motions.clear();
    m_Sounds.clear();
}

void CModelJson::Parse(const QString &fileName)
{
    Clear();
    QFile file(fileName);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return;
    }
    QByteArray jsonData = file.readAll();
    if (file.error() != QFileDevice::NoError) return;
    file.close();


    QJsonDocument jsonDoc = QJsonDocument::fromJson(jsonData);
    if (jsonDoc.isNull() || !jsonDoc.isObject()) {
        return;
    }

    QJsonObject qt = jsonDoc.object();
    if (!validateFileReferences(qt, nullptr)) return;
    const QFileInfo fileInfo(fileName);
    m_parentPath = fileInfo.absolutePath();
    m_modelName = fileInfo.fileName();
    QJsonObject FileReferences = qt["FileReferences"].toObject();
    if (FileReferences.isEmpty()) return;
    if (FileReferences.contains("UserData") && !FileReferences["UserData"].isString()) return;
    m_moc = FileReferences["Moc"].toString();
    if (m_moc.isEmpty()) return;

    QJsonArray Textures = FileReferences["Textures"].toArray();
    for(int i = 0 ;i < Textures.size() ;i++){
        m_Textures.push_back(Textures[i].toString());
    }

    m_DisplayInfo = FileReferences["DisplayInfo"].toString();


    m_Physics = FileReferences["Physics"].toString();
    m_Pose = FileReferences["Pose"].toString();
    m_UserData = FileReferences["UserData"].toString();

    QJsonArray  Expressions = FileReferences["Expressions"].toArray();
    for(int i = 0 ;i < Expressions.size() ;i++){
        m_Expressions.push_back(Expressions[i].toObject()["File"].toString());
    }

    QJsonObject  Motions = FileReferences["Motions"].toObject();
    for (const auto &key : Motions.keys()) {
        QJsonArray motionfile = Motions[key].toArray();

        for(int i = 0 ;i < motionfile.size() ;i++){
            m_Motions.push_back(motionfile[i].toObject()["File"].toString());
            const QString sound = motionfile[i].toObject()["Sound"].toString();
            if (!sound.isEmpty()) m_Sounds.push_back(sound);
        }
    }


    m_IsOpen = true;

}
bool CModelJson::IsOpen() { return m_IsOpen; }

QStringList CModelJson::getAllZipFiles()
{
    QStringList list;
    const auto append = [&](const QString& reference) {
        const auto path = CModelResourcePath::Resolve(m_parentPath, reference);
        if (!path.isEmpty()) list.append(path);
    };
    for (const auto& reference : {m_modelName, m_moc, m_DisplayInfo, m_Physics, m_Pose, m_UserData}) append(reference);
    for (const auto& references : {m_Textures, m_Expressions, m_Motions, m_Sounds})
        for (const auto& reference : references) append(reference);
    return list;
}

QString CModelJson::getMocFileName()
{
    return CModelResourcePath::Resolve(m_parentPath, m_moc);
}
