#ifndef CMO3_USERDATA_TEST_HELPER_H
#define CMO3_USERDATA_TEST_HELPER_H

#include <QMap>
#include <QXmlStreamReader>

inline bool readMeshUserData(const QByteArray& bytes, QMap<QString, QString>* tags, QString* error)
{
    QXmlStreamReader xml(bytes);
    xml.setNamespaceProcessing(false);
    int depth = 0;
    int meshDepth = -1;
    QString meshId;
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement()) {
            ++depth;
            if (xml.name() == QLatin1String("CArtMeshSource") && xml.attributes().hasAttribute("xs.id")) {
                meshDepth = depth;
                meshId.clear();
            }
            if (meshDepth < 0) continue;
            if (xml.name() == QLatin1String("CDrawableId") && xml.attributes().value("xs.n") == QLatin1String("id"))
                meshId = xml.attributes().value("idstr").toString();
            if (depth == meshDepth + 1 && xml.name() == QLatin1String("s") &&
                xml.attributes().value("xs.n") == QLatin1String("userData")) {
                if (meshId.isEmpty() || tags->contains(meshId)) {
                    if (error) *error = QStringLiteral("缺失或重复的 ArtMesh ID");
                    return false;
                }
                tags->insert(meshId, xml.readElementText());
                --depth;
            }
        } else if (xml.isEndElement()) {
            if (depth == meshDepth) meshDepth = -1;
            --depth;
        }
    }
    if (error) *error = xml.errorString();
    return !xml.hasError();
}

#endif
