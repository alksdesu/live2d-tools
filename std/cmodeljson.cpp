#include "cmodeljson.h"

CModelJson::CModelJson() { Clear(); }

void CModelJson::Clear()
{
    m_IsOpen = false;
    m_parentPath.clear();
    m_modelName.clear();
    m_moc.clear();
    m_DisplayInfo.clear();
    m_Physics.clear();
    m_Pose.clear();
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
    m_moc = FileReferences["Moc"].toString();
    if (m_moc.isEmpty()) return;

    QJsonArray Textures = FileReferences["Textures"].toArray();
    for(int i = 0 ;i < Textures.size() ;i++){
        m_Textures.push_back(Textures[i].toString());
    }

    m_DisplayInfo = FileReferences["DisplayInfo"].toString();


    m_Physics = FileReferences["Physics"].toString();
    m_Pose = FileReferences["Pose"].toString();

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
