#include "ccidjson.h"
#include <QJsonParseError>
#include <QCryptographicHash>
#include <QFileInfo>
#include <QDir>

CCidJson::CCidJson() {
    empty="";

}

bool CCidJson::Parse(const QString& fileName, QString* error)
{
    if (error) error->clear();
    const auto sourcePath = QDir::cleanPath(QFileInfo(fileName).absoluteFilePath());
    QMap<QString, QString> partsIdToName;
    QMap<QString, QString> groupIdToName;
    QMap<QString, QString> parametersIdToName;
    QMap<QString, QString> deformersIdToName;
    QVector<CVetGroup> groupsToParameters;
    QVector<QVector<QString>> combinedParameters;
    QSet<QString> combinedParameterSet;
    QFile file(sourcePath);
    if (!file.open(QIODevice::ReadOnly)) {
        qDebug() << "Failed to open file";
        if (error) *error = file.errorString();
        return false;
    }
    const QByteArray jsonData = file.readAll();
    if (file.error() != QFileDevice::NoError) {
        if (error) *error = file.errorString();
        return false;
    }
    file.close();
    QJsonParseError parseError;
    QJsonDocument jsonDoc = QJsonDocument::fromJson(jsonData, &parseError);
    if (parseError.error != QJsonParseError::NoError || !jsonDoc.isObject()) {
        qDebug() << "Failed to parse JSON";
        if (error) *error = parseError.errorString();
        return false;
    }

    QJsonObject qt = jsonDoc.object();
    const auto readArray = [&qt, error](const QString& key, QJsonArray* array) {
        const QJsonValue value = qt.value(key);
        if (value.isUndefined()) { *array = {}; return true; }
        if (!value.isArray()) {
            if (error) *error = QStringLiteral("CDI 字段必须是数组：%1").arg(key);
            return false;
        }
        *array = value.toArray();
        return true;
    };
    QJsonArray Parts;
    if (!readArray(QStringLiteral("Parts"), &Parts)) { return false; }
    for(int i = 0 ;i < Parts.size() ;i++){
        QJsonObject pO = Parts[i].toObject();
        const QString id = pO["Id"].toString();
        if (!id.isEmpty()) partsIdToName.insert(id,pO["Name"].toString());
    }


    if (!readArray(QStringLiteral("ParameterGroups"), &Parts)) { return false; }
    for(int i = 0 ;i < Parts.size() ;i++){
        QJsonObject pO = Parts[i].toObject();
        const QString id = pO["Id"].toString();
        if (!id.isEmpty()) groupIdToName.insert(id,pO["Name"].toString());
    }

    if (!readArray(QStringLiteral("Parameters"), &Parts)) { return false; }
    CVetGroup vetgrop;
    QString curGroupName;
    QString lastGroupName;
    for(int i = 0 ;i < Parts.size() ;i++){
        QJsonObject pO = Parts[i].toObject();
        const QString id = pO["Id"].toString();
        if (id.isEmpty()) continue;
        parametersIdToName.insert(id,pO["Name"].toString());
        curGroupName = pO["GroupId"].toString();

        if(i == 0){
            lastGroupName = curGroupName;
        }

        if(curGroupName != lastGroupName){
            groupsToParameters.push_back(vetgrop);
            vetgrop.clear();
            lastGroupName = curGroupName;
        }

        vetgrop.vetParam.push_back(id);
        vetgrop.strGroup = curGroupName;

        if(i == (Parts.size() - 1)){
           groupsToParameters.push_back(vetgrop);
        }
    }

    // 解析 Deformers（部分模型可能包含）
    if(qt.contains("Deformers")){
        QJsonArray deformers;
        if (!readArray(QStringLiteral("Deformers"), &deformers)) { return false; }
        for(int i = 0; i < deformers.size(); i++){
            QJsonObject dO = deformers[i].toObject();
            const QString id = dO["Id"].toString();
            if (!id.isEmpty()) deformersIdToName.insert(id, dO["Name"].toString());
        }
    }

    // 解析 CombinedParameters（融合变形参数组）
    if(qt.contains("CombinedParameters")){
        QJsonArray combined;
        if (!readArray(QStringLiteral("CombinedParameters"), &combined)) { return false; }
        for(int i = 0; i < combined.size(); i++){
            if (!combined[i].isArray()) continue;
            QJsonArray group = combined[i].toArray();
            QVector<QString> paramIds;
            for(int j = 0; j < group.size(); j++){
                QString id = group[j].toString();
                if (id.isEmpty()) continue;
                paramIds.append(id);
                combinedParameterSet.insert(id);
            }
            if (!paramIds.isEmpty()) combinedParameters.append(paramIds);
        }
    }

    QByteArray digest = QCryptographicHash::hash(jsonData, QCryptographicHash::Sha256);
    m_PartsIdToName.swap(partsIdToName);
    m_GroupIdToName.swap(groupIdToName);
    m_ParametersIdToName.swap(parametersIdToName);
    m_DeformersIdToName.swap(deformersIdToName);
    m_VetGroupToParam.swap(groupsToParameters);
    m_CombinedParameters.swap(combinedParameters);
    m_CombinedParamSet.swap(combinedParameterSet);
    m_sourcePath = sourcePath;
    m_sourceDigest.swap(digest);
    return true;
}
