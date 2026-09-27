#include "cexportdiagnostics.h"
#include <QJsonArray>
#include <QStringList>

namespace {
using Diagnostic = live2dstruct::CModelDiagnostic;
QString text(const Diagnostic& value)
{
    return value.objectId.isEmpty() ? value.message : QStringLiteral("%1：%2").arg(value.objectId, value.message);
}
QString owned(const QString& value) { return QString(value.constData(), value.size()); }
}

CExportDiagnostics CExportDiagnostics::Capture(const QVector<Diagnostic>& diagnostics, const QString& sourceMoc,
                                               const QByteArray& sourceDigest, quint64 revision)
{
    CExportDiagnostics result;
    result.m_sourceMoc = owned(sourceMoc);
    result.m_sourceDigest = QByteArray(sourceDigest.constData(), sourceDigest.size());
    result.m_revision = revision;
    for (const auto& value : diagnostics) {
        if (value.severity == Diagnostic::Warning) ++result.m_warnings;
        else ++result.m_errors;
    }
    qsizetype remaining = MaximumCharacters;
    for (const auto severity : {Diagnostic::Error, Diagnostic::Warning}) {
        for (const auto& value : diagnostics) {
            const auto normalized = value.severity == Diagnostic::Warning ? Diagnostic::Warning : Diagnostic::Error;
            if (normalized != severity || result.m_entries.size() >= MaximumEntries) continue;
            qsizetype available = remaining;
            bool fits = true;
            for (const auto* field : {&value.code, &value.message, &value.objectType, &value.objectId}) {
                if (field->size() > available) { fits = false; break; }
                available -= field->size();
            }
            if (!fits) continue;
            result.m_entries.append({normalized, owned(value.code), owned(value.message), value.index,
                                     owned(value.objectType), owned(value.objectId)});
            remaining = available;
        }
    }
    return result;
}

QString CExportDiagnostics::FirstError() const
{
    for (const auto& value : m_entries) if (value.severity == Diagnostic::Error) return text(value);
    return m_errors ? QStringLiteral("模型检查发现错误，请查看完整诊断报告") : QString();
}

QString CExportDiagnostics::Text() const
{
    QStringList lines;
    for (const auto& value : m_entries)
        lines.append(QStringLiteral("%1：%2").arg(value.severity == Diagnostic::Error ? QStringLiteral("错误")
                                                                                     : QStringLiteral("提示"), text(value)));
    if (IsTruncated()) lines.append(QStringLiteral("共 %1 条诊断，当前显示 %2 条；请打开对应源模型，在模型检查中查看完整报告。")
        .arg(m_errors + m_warnings).arg(m_entries.size()));
    return lines.join(QLatin1Char('\n'));
}

QJsonObject CExportDiagnostics::ToJson() const
{
    QJsonArray entries;
    for (const auto& value : m_entries)
        entries.append(QJsonObject{{QStringLiteral("severity"), value.severity == Diagnostic::Error ? QStringLiteral("error") : QStringLiteral("warning")},
            {QStringLiteral("code"), value.code}, {QStringLiteral("message"), value.message}, {QStringLiteral("index"), value.index},
            {QStringLiteral("objectType"), value.objectType}, {QStringLiteral("objectId"), value.objectId}});
    return {{QStringLiteral("sourceMoc"), m_sourceMoc}, {QStringLiteral("sourceDigest"), QString::fromLatin1(m_sourceDigest.toHex())},
        {QStringLiteral("documentRevision"), QString::number(m_revision)}, {QStringLiteral("errorCount"), qint64(m_errors)},
        {QStringLiteral("warningCount"), qint64(m_warnings)}, {QStringLiteral("truncated"), IsTruncated()},
        {QStringLiteral("entries"), entries}};
}

bool CExportDiagnostics::operator==(const CExportDiagnostics& other) const
{
    return m_errors == other.m_errors && m_warnings == other.m_warnings && m_entries == other.m_entries &&
        m_sourceMoc == other.m_sourceMoc && m_sourceDigest == other.m_sourceDigest && m_revision == other.m_revision;
}
