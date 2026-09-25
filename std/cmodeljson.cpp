#include "cmodeljson.h"
#include <cmath>
#include <limits>

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
}

void CModelJson::Parse(const QString &fileName)
{
    Clear();
    QFile file(fileName);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return;
    }
    QByteArray jsonData = file.readAll();
    file.close();


    // 使用QFileInfo来获取文件信息
    QFileInfo fileInfo(fileName);

    // 获取文件的父路径
    m_parentPath = fileInfo.absolutePath();
    m_modelName = fileInfo.fileName();

    QJsonDocument jsonDoc = QJsonDocument::fromJson(jsonData);
    if (jsonDoc.isNull() || !jsonDoc.isObject()) {
        return;
    }

    QJsonObject qt = jsonDoc.object();
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
        }
    }


    m_IsOpen = true;

}
bool CModelJson::IsOpen() { return m_IsOpen; }

QStringList CModelJson::getAllZipFiles()
{
    QStringList list;


    list.push_back(m_parentPath+ "/" + m_modelName);
    list.push_back(m_parentPath+ "/" + m_moc);
    if (!m_DisplayInfo.isEmpty()) list.push_back(m_parentPath+ "/" + m_DisplayInfo);
    if (!m_Physics.isEmpty()) list.push_back(m_parentPath+ "/" + m_Physics);
    if (!m_Pose.isEmpty()) list.push_back(m_parentPath+ "/" + m_Pose);
    if (!m_UserData.isEmpty()) list.push_back(m_parentPath+ "/" + m_UserData);

    for(int i = 0 ;i < m_Textures.size() ;i++){
        list.push_back(m_parentPath+ "/" + m_Textures[i]);
    }

    for(int i = 0 ;i < m_Expressions.size() ;i++){
        list.push_back(m_parentPath+ "/" + m_Expressions[i]);
    }




    for(int i = 0 ;i < m_Motions.size() ;i++){
        list.push_back(m_parentPath+ "/" + m_Motions[i]);
    }


    return list;
}

QString CModelJson::getMocFileName()
{
    return m_parentPath+ "/" + m_moc;
}
