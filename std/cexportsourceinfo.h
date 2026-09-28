#ifndef CEXPORTSOURCEINFO_H
#define CEXPORTSOURCEINFO_H

#include <QByteArray>
#include <QJsonObject>
#include <QString>

class CExportSourceInfo
{
public:
    enum class Method { OriginalPsd, ReconstructedLayers };
    struct Details {
        Method method = Method::ReconstructedLayers;
        QString documentId, documentName, familyId, revisionId;
        QByteArray inputDigest, outputDigest;
    };
    static CExportSourceInfo Capture(const Details& details);
    bool IsEmpty() const { return !m_configured; }
    bool IsValid() const;
    const Details& Get() const { return m_details; }
    QString MethodText() const;
    QJsonObject ToJson() const;
    bool operator==(const CExportSourceInfo& other) const;
    bool operator!=(const CExportSourceInfo& other) const { return !(*this == other); }

private:
    Details m_details;
    bool m_configured = false;
};

#endif
