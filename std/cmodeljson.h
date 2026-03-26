#ifndef CMODELJSON_H
#define CMODELJSON_H
#include <QString>
#include <QVector>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QFileInfo>
class CModelJson
{
public:
    CModelJson();
    void Parse(const QString& fileName);
    bool IsOpen();
    QStringList getAllZipFiles();
    QString getMocFileName();

    QString m_Physics;
    QString m_Pose;
    QString m_DisplayInfo;
    QVector<QString> m_Expressions;
    QString m_moc;
    QVector<QString> m_Textures;
    QVector<QString> m_Motions;
    bool m_IsOpen;
    QString m_parentPath;
    QString m_modelName;


};

#endif // CMODELJSON_H
