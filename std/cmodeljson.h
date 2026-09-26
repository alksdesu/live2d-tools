#ifndef CMODELJSON_H
#define CMODELJSON_H
#include <QString>
#include <QByteArray>
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
    static bool ValidateRuntimeSettings(const QByteArray& data, QString* error = nullptr);
    static bool IsUserDataRegistration(const QByteArray& before, const QByteArray& after, QString* reference = nullptr);
    void Parse(const QString& fileName);
    bool IsOpen();
    const QByteArray& SourceDigest() const { return m_sourceDigest; }
    QStringList getAllZipFiles();
    QString getMocFileName();

    QString m_Physics;
    QString m_Pose;
    QString m_UserData;
    QString m_DisplayInfo;
    QVector<QString> m_Expressions;
    QString m_moc;
    QVector<QString> m_Textures;
    QVector<QString> m_Motions;
    QVector<QString> m_Sounds;
    bool m_IsOpen;
    QString m_parentPath;
    QString m_modelName;

private:
    void Clear();
    QByteArray m_sourceDigest;
};

#endif // CMODELJSON_H
