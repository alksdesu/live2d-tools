#ifndef CSETTINGCONFIG_H
#define CSETTINGCONFIG_H
#include <QString>
#include <QVector2D>
class CSettingConfig
{
public:
    CSettingConfig();

    void SetInitFileName(const QString& name){
        m_InitName = name;
    }
    const QString& GetInitFileName(){
        return m_InitName;
    }
    const QVector2D& GetLastPos(){
        return m_lastpos;
    }
    void SetLastPos(const QVector2D& lastpos){
        m_lastpos = lastpos;
    }
    float GeteScalefactor(){
        return m_Scalefactor;
    }
    void SetScalefactor(float Scalefactor){
        this->m_Scalefactor = Scalefactor;
    }
private:
    QVector2D m_lastpos;
    QString m_InitName;
    float m_Scalefactor;
};

#endif // CSETTINGCONFIG_H
