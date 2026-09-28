#include "cexportsourceinfo.h"

CExportSourceInfo CExportSourceInfo::Capture(const Details& details)
{
    CExportSourceInfo result;
    result.m_configured = true;
    result.m_details.method = details.method;
    for (const auto* value : {&details.documentId, &details.documentName, &details.familyId, &details.revisionId})
        if (value->size() > 32768 || value->contains(QChar::Null) || !value->isValidUtf16()) return result;
    if (details.documentId.isEmpty() || details.inputDigest.size() != 32 ||
        (!details.outputDigest.isEmpty() && details.outputDigest.size() != 32)) return result;
    const auto owned = [](const QString& value) { return QString(value.constData(), value.size()); };
    result.m_details.documentId = owned(details.documentId);
    result.m_details.documentName = owned(details.documentName);
    result.m_details.familyId = owned(details.familyId);
    result.m_details.revisionId = owned(details.revisionId);
    result.m_details.inputDigest = QByteArray(details.inputDigest.constData(), details.inputDigest.size());
    result.m_details.outputDigest = QByteArray(details.outputDigest.constData(), details.outputDigest.size());
    return result;
}

bool CExportSourceInfo::IsValid() const
{
    if (!m_configured) return true;
    if (m_details.method != Method::OriginalPsd && m_details.method != Method::ReconstructedLayers) return false;
    for (const auto* value : {&m_details.documentId, &m_details.documentName, &m_details.familyId, &m_details.revisionId})
        if (value->size() > 32768 || value->contains(QChar::Null) || !value->isValidUtf16()) return false;
    return !m_details.documentId.isEmpty() && m_details.inputDigest.size() == 32 &&
        (m_details.outputDigest.isEmpty() || m_details.outputDigest.size() == 32) &&
        (m_details.method != Method::OriginalPsd || m_details.outputDigest.isEmpty() ||
         m_details.outputDigest == m_details.inputDigest);
}

QString CExportSourceInfo::MethodText() const
{
    return !m_configured ? QString() : m_details.method == Method::OriginalPsd
        ? QStringLiteral("原始 PSD 完整另存") : QStringLiteral("来源图层重建");
}

QJsonObject CExportSourceInfo::ToJson() const
{
    if (!m_configured) return {};
    return {{QStringLiteral("method"), m_details.method == Method::OriginalPsd ? QStringLiteral("original_psd") : QStringLiteral("reconstructed_layers")},
        {QStringLiteral("documentId"), m_details.documentId}, {QStringLiteral("documentName"), m_details.documentName},
        {QStringLiteral("familyId"), m_details.familyId}, {QStringLiteral("revisionId"), m_details.revisionId},
        {QStringLiteral("inputDigest"), QString::fromLatin1(m_details.inputDigest.toHex())},
        {QStringLiteral("outputDigest"), QString::fromLatin1(m_details.outputDigest.toHex())}};
}

bool CExportSourceInfo::operator==(const CExportSourceInfo& other) const
{
    return m_configured == other.m_configured && m_details.method == other.m_details.method &&
        m_details.documentId == other.m_details.documentId && m_details.documentName == other.m_details.documentName &&
        m_details.familyId == other.m_details.familyId && m_details.revisionId == other.m_details.revisionId &&
        m_details.inputDigest == other.m_details.inputDigest && m_details.outputDigest == other.m_details.outputDigest;
}
