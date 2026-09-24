#ifndef CMODELUSERDATA_H
#define CMODELUSERDATA_H

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QVector>

namespace live2dstruct {

struct CModelUserDataEntry
{
    QString target;
    QString id;
    QString value;
    QJsonObject fields;
};

class CModelUserData
{
public:
    bool FromJson(const QByteArray& bytes, QString* error = nullptr);
    QByteArray ToJson() const;
    bool Load(const QString& path, QString* error = nullptr);
    bool Save(const QString& path, QString* error = nullptr) const;
    bool Validate(QString* error = nullptr) const;

    const QVector<CModelUserDataEntry>& Entries() const { return m_entries; }
    void SetEntries(const QVector<CModelUserDataEntry>& entries) { m_entries = entries; }

private:
    QVector<CModelUserDataEntry> m_entries;
    QJsonObject m_root;
};

}

#endif // CMODELUSERDATA_H
