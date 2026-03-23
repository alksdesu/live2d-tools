#ifndef CCIDJSON_H
#define CCIDJSON_H
#include <QString>
#include <QFile>
#include <QDebug>
#include <QJsonDocument>
#include <QMap>
#include <QVector>
#include <QJsonObject>
#include <QJsonArray>


class CVetGroup{
    friend class CCidJson;
    friend class CParaTree;
private:
    QString strGroup;
    QVector<QString> vetParam;
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
    void Parse(QString fileName);//传递解析文件

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
private:
    QMap<QString,QString> m_PartsIdToName;
    QMap<QString,QString> m_GroupIdToName;
    QMap<QString,QString> m_ParametersIdToName;
    QVector<CVetGroup> m_VetGroupToParam;
    QString empty;

};

#endif // CCIDJSON_H
