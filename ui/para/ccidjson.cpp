#include "ccidjson.h"

CCidJson::CCidJson() {
    empty="";

}

void CCidJson::Parse(QString fileName)
{

    QFile file(fileName);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qDebug() << "Failed to open file";
        return;
    }
    QByteArray jsonData = file.readAll();

    file.close();
    QJsonDocument jsonDoc = QJsonDocument::fromJson(jsonData);
    if (jsonDoc.isNull()) {
        qDebug() << "Failed to parse JSON";
        return;
    }

    QJsonObject qt = jsonDoc.object();
    QJsonArray Parts = qt["Parts"].toArray();
    for(int i = 0 ;i < Parts.size() ;i++){
        QJsonObject pO = Parts[i].toObject();
        m_PartsIdToName.insert(pO["Id"].toString(),pO["Name"].toString());
    }


    Parts = qt["ParameterGroups"].toArray();
    for(int i = 0 ;i < Parts.size() ;i++){
        QJsonObject pO = Parts[i].toObject();
        m_GroupIdToName.insert(pO["Id"].toString(),pO["Name"].toString());
    }

    Parts = qt["Parameters"].toArray();
    CVetGroup vetgrop;
    QString curGroupName;
    QString lastGroupName;
    for(int i = 0 ;i < Parts.size() ;i++){
        QJsonObject pO = Parts[i].toObject();
        m_ParametersIdToName.insert(pO["Id"].toString(),pO["Name"].toString());
        curGroupName = pO["GroupId"].toString();

        if(i == 0){
            lastGroupName = curGroupName;
        }

        if(curGroupName != lastGroupName){
            m_VetGroupToParam.push_back(vetgrop);
            vetgrop.clear();
            lastGroupName = curGroupName;
        }

        vetgrop.vetParam.push_back(pO["Id"].toString());
        vetgrop.strGroup = curGroupName;

        if(i == (Parts.size() - 1)){
           m_VetGroupToParam.push_back(vetgrop);
        }
    }

    // 解析 Deformers（部分模型可能包含）
    if(qt.contains("Deformers")){
        QJsonArray deformers = qt["Deformers"].toArray();
        for(int i = 0; i < deformers.size(); i++){
            QJsonObject dO = deformers[i].toObject();
            m_DeformersIdToName.insert(dO["Id"].toString(), dO["Name"].toString());
        }
    }

    // 解析 CombinedParameters（融合变形参数组）
    if(qt.contains("CombinedParameters")){
        QJsonArray combined = qt["CombinedParameters"].toArray();
        for(int i = 0; i < combined.size(); i++){
            QJsonArray group = combined[i].toArray();
            QVector<QString> paramIds;
            for(int j = 0; j < group.size(); j++){
                QString id = group[j].toString();
                paramIds.append(id);
                m_CombinedParamSet.insert(id);
            }
            m_CombinedParameters.append(paramIds);
        }
    }
}
