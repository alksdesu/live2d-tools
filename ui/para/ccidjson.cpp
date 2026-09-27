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
    std::optional<live2dstruct::CParameterOrganization::State> organization;
    std::optional<live2dstruct::CGeometryObjectPairs::State> geometryPairs;
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
    const auto extension = qt.value(QStringLiteral("Live2DToolsParameterOrganization"));
    if (!extension.isUndefined()) {
        const auto object = extension.toObject();
        if (!extension.isObject() || object.value(QStringLiteral("Version")) != QJsonValue(1) ||
            !object.value(QStringLiteral("State")).isObject()) {
            if (error) *error = QStringLiteral("CDI 参数组织扩展格式或版本无效");
            return false;
        }
        organization.emplace();
        if (!live2dstruct::CParameterOrganization::FromJson(object.value(QStringLiteral("State")).toObject(), *organization, error)) return false;
    }
    const auto geometryExtension = qt.value(QStringLiteral("Live2DToolsGeometryObjectPairs"));
    if (!geometryExtension.isUndefined()) {
        const auto object = geometryExtension.toObject();
        if (!geometryExtension.isObject() || object.value(QStringLiteral("Version")) != QJsonValue(1) ||
            !object.value(QStringLiteral("State")).isObject()) {
            if (error) *error = QStringLiteral("CDI 几何配对扩展格式或版本无效");
            return false;
        }
        geometryPairs.emplace();
        if (!live2dstruct::CGeometryObjectPairs::FromJson(object.value(QStringLiteral("State")).toObject(), *geometryPairs, error)) return false;
    }
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
    QMap<QString, int> groupRows;
    for (const auto& value : Parts) {
        const auto parameter = value.toObject();
        const auto id = parameter.value(QStringLiteral("Id")).toString();
        if (id.isEmpty()) continue;
        parametersIdToName.insert(id, parameter.value(QStringLiteral("Name")).toString());
        const auto groupId = parameter.value(QStringLiteral("GroupId")).toString();
        if (!groupRows.contains(groupId)) {
            groupRows.insert(groupId, groupsToParameters.size());
            CVetGroup group;
            group.strGroup = groupId;
            groupsToParameters.append(group);
        }
        groupsToParameters[groupRows.value(groupId)].vetParam.append(id);
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
    m_parameterOrganization.swap(organization);
    m_geometryObjectPairs.swap(geometryPairs);
    return true;
}
