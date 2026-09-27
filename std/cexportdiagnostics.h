#ifndef CEXPORTDIAGNOSTICS_H
#define CEXPORTDIAGNOSTICS_H

#include "live2dstruct/cmodeldiagnostic.h"
#include <QByteArray>
#include <QJsonObject>
#include <QVector>

class CExportDiagnostics
{
public:
    static constexpr qsizetype MaximumEntries = 256;
    static constexpr qsizetype MaximumCharacters = 65536;
    static CExportDiagnostics Capture(const QVector<live2dstruct::CModelDiagnostic>& diagnostics,
        const QString& sourceMoc = {}, const QByteArray& sourceDigest = {}, quint64 revision = 0);
    qsizetype ErrorCount() const { return m_errors; }
    qsizetype WarningCount() const { return m_warnings; }
    bool IsTruncated() const { return m_entries.size() < m_errors + m_warnings; }
    const QVector<live2dstruct::CModelDiagnostic>& Entries() const { return m_entries; }
    QString FirstError() const;
    QString Text() const;
    QJsonObject ToJson() const;
    bool operator==(const CExportDiagnostics& other) const;
    bool operator!=(const CExportDiagnostics& other) const { return !(*this == other); }

private:
    qsizetype m_errors = 0, m_warnings = 0;
    QVector<live2dstruct::CModelDiagnostic> m_entries;
    QString m_sourceMoc;
    QByteArray m_sourceDigest;
    quint64 m_revision = 0;
};

#endif
