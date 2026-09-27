#ifndef CCIDJSON_H
#define CCIDJSON_H
#include <QString>
#include <QFile>
#include <QDebug>
#include <QJsonDocument>
#include <QMap>
#include <QVector>
#include <QSet>
#include <QJsonObject>
#include <QJsonArray>
#include <optional>
#include "live2dstruct/cparameterorganization.h"


class CVetGroup{
    friend class CCidJson;
    friend class CParaTree;
public:
    QString strGroup;
    QVector<QString> vetParam;
private:
    void clear(){
        strGroup="";
        vetParam.clear();
    }
};

class CCidJson
{
    friend class CParaTree;
public:
    CCidJson();
    bool Parse(const QString& fileName, QString* error = nullptr);//传递解析文件
    const QString& SourcePath() const { return m_sourcePath; }
    const QByteArray& SourceDigest() const { return m_sourceDigest; }
    const std::optional<live2dstruct::CParameterOrganization::State>& ParameterOrganization() const { return m_parameterOrganization; }

    const QString& getPartName(const QString& Id){
        if(m_PartsIdToName.contains(Id)){
            return m_PartsIdToName[Id];
        }
        return empty;

    }
    const QString getPartName(const QString& Id) const{
        if(m_PartsIdToName.contains(Id)){
            return m_PartsIdToName[Id];
        }
        return empty;

    }
    const QString getParaName(const QString& Id) const{
        if(m_ParametersIdToName.contains(Id)){
            return m_ParametersIdToName[Id];
        }
        return empty;

    }
    const QVector<CVetGroup>& getGroups() const { return m_VetGroupToParam; }
    const QMap<QString,QString>& getGroupIdToName() const { return m_GroupIdToName; }

    const QString getDeformerName(const QString& Id) const{
        if(m_DeformersIdToName.contains(Id)){
            return m_DeformersIdToName[Id];
        }
        return empty;
    }

    // 融合变形参数组：每组是一个参数 ID 列表
    const QVector<QVector<QString>>& getCombinedParameters() const { return m_CombinedParameters; }
    bool isCombinedParameter(const QString& id) const { return m_CombinedParamSet.contains(id); }

private:
    QMap<QString,QString> m_PartsIdToName;
    QMap<QString,QString> m_GroupIdToName;
    QMap<QString,QString> m_ParametersIdToName;
    QMap<QString,QString> m_DeformersIdToName;
    QVector<CVetGroup> m_VetGroupToParam;
    QVector<QVector<QString>> m_CombinedParameters;
    QSet<QString> m_CombinedParamSet;
    QString empty;
    QString m_sourcePath;
    QByteArray m_sourceDigest;
    std::optional<live2dstruct::CParameterOrganization::State> m_parameterOrganization;

};

#endif // CCIDJSON_H
