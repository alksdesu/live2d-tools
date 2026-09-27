#include "cmocruntimesnapshot.h"
#include "cmocdeformerorder.h"

#include <QCryptographicHash>
#include <exception>
#include <limits>

namespace live2dstruct {
struct CMocRuntimeSnapshot::Data
{
    QByteArray bytes;
    QByteArray digest;
};

bool CMocRuntimeSnapshot::Create(const QByteArray& bytes, CMocRuntimeSnapshot& result, QString* error)
{
    if (error) error->clear();
    if (bytes.isEmpty() || bytes.size() > std::numeric_limits<int>::max()) {
        if (error) *error = QStringLiteral("运行时 MOC 字节为空或超出范围");
        return false;
    }
    try {
        const QByteArray owned(bytes.constData(), bytes.size());
        QByteArray normalized;
        if (!CMocDeformerOrder::Normalize(owned, normalized, error)) return false;
        auto data = std::make_shared<Data>();
        data->bytes = std::move(normalized);
        data->digest = QCryptographicHash::hash(data->bytes, QCryptographicHash::Sha256);
        CMocRuntimeSnapshot candidate;
        candidate.m_data = std::move(data);
        result.m_data.swap(candidate.m_data);
        return true;
    } catch (const std::exception&) {
        if (error) *error = QStringLiteral("无法准备运行时 MOC 快照");
        return false;
    }
}

bool CMocRuntimeSnapshot::IsValid() const noexcept
{
    return bool(m_data);
}

const QByteArray& CMocRuntimeSnapshot::Bytes() const noexcept
{
    static const QByteArray empty;
    return m_data ? m_data->bytes : empty;
}

const QByteArray& CMocRuntimeSnapshot::Digest() const noexcept
{
    static const QByteArray empty;
    return m_data ? m_data->digest : empty;
}
}
