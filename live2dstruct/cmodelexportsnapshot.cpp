#include "cmodelexportsnapshot.h"
#include "cmodeldocument.h"

namespace live2dstruct {
struct CModelExportSnapshot::Data {
    QByteArray sourceDigest, runtimeDigest;
    CModelDisplayNames::State names;
};

namespace {
bool fail(QString* error, const QString& message)
{
    if (error) *error = message;
    return false;
}
}

bool CModelExportSnapshot::Create(const QByteArray& sourceDigest, const QByteArray& runtimeBytes,
                                  const CModelDisplayNames::State& names, CModelExportSnapshot& result, QString* error) try
{
    if (error) error->clear();
    if (sourceDigest.size() != 32) return fail(error, QStringLiteral("导出名称缺少有效的模型来源摘要"));
    if (!CModelDisplayNames::Validate(names, error)) return false;
    CModelDocument runtime;
    if (!runtime.LoadBytes(runtimeBytes, {}, error) || !runtime.SetDisplayNames(names, error)) return false;
    auto data = std::make_shared<Data>();
    data->sourceDigest = QByteArray(sourceDigest.constData(), sourceDigest.size());
    data->runtimeDigest = runtime.SourceDigest();
    data->names = runtime.DisplayNames();
    CModelExportSnapshot candidate;
    candidate.m_data = std::move(data);
    result = std::move(candidate);
    return true;
} catch (...) {
    return fail(error, QStringLiteral("无法捕获导出名称，原快照保持不变"));
}

bool CModelExportSnapshot::Matches(const QByteArray& sourceDigest, const QByteArray& runtimeDigest, QString* error) const
{
    if (error) error->clear();
    return (m_data && m_data->sourceDigest == sourceDigest && m_data->runtimeDigest == runtimeDigest) ||
        fail(error, QStringLiteral("导出名称与当前模型或预览版本不一致"));
}

QString CModelExportSnapshot::DisplayName(const QString& type, const QString& id) const
{
    return CModelDisplayNames::EffectiveName(DisplayNames(), type, id);
}

const CModelDisplayNames::State& CModelExportSnapshot::DisplayNames() const
{
    static const CModelDisplayNames::State empty;
    return m_data ? m_data->names : empty;
}

const QByteArray& CModelExportSnapshot::SourceDigest() const
{
    static const QByteArray empty;
    return m_data ? m_data->sourceDigest : empty;
}

const QByteArray& CModelExportSnapshot::RuntimeDigest() const
{
    static const QByteArray empty;
    return m_data ? m_data->runtimeDigest : empty;
}
}
