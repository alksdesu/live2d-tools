#include "cmaterialvalidationcache.h"
#include "ctextureassets.h"
#include "std/cpngimage.h"
#include <QCryptographicHash>
#include <QImage>

namespace live2dstruct {
namespace {
bool fail(QString* error, const QString& message) { if (error) *error = message; return false; }
}

bool CMaterialValidationCache::Resolve(const QByteArray& bytes, const QByteArray& expected,
                                       QByteArray& digest, QByteArray& frozen, QString* error)
{
    if (bytes.size() > CTextureAssets::MaximumBytes || (!expected.isEmpty() && expected.size() != 32))
        return fail(error, QStringLiteral("素材缓存的字节数量或摘要无效"));
    if (!expected.isEmpty()) {
        const auto found = m_records.constFind(expected);
        if (found != m_records.cend()) {
            if (found->bytes != bytes) return fail(error, QStringLiteral("素材字节与已验证摘要不一致"));
            digest = found.key(); return true;
        }
    } else {
        const auto hint = m_hints.constFind({quintptr(bytes.constData()), bytes.size()});
        if (hint != m_hints.cend()) {
            const auto found = m_records.constFind(*hint);
            if (found != m_records.cend() && found->bytes == bytes) { digest = found.key(); return true; }
        }
    }
    frozen = QByteArray(bytes.constData(), bytes.size());
    const auto actual = QCryptographicHash::hash(frozen, QCryptographicHash::Sha256); m_hashedBytes += frozen.size();
    if (!expected.isEmpty() && actual != expected) return fail(error, QStringLiteral("素材摘要与内容不一致"));
    const auto found = m_records.constFind(actual);
    if (found != m_records.cend()) {
        if (found->bytes != frozen) return fail(error, QStringLiteral("素材内容摘要发生冲突"));
        frozen.clear();
    }
    digest = actual; return true;
}

bool CMaterialValidationCache::Store(const QByteArray& source, const QByteArray& digest, QByteArray frozen,
                                     std::optional<QSize> size, QString* error)
{
    auto found = m_records.find(digest);
    if (found == m_records.end()) {
        if (frozen.size() > CTextureAssets::MaximumBytes - m_ownedBytes)
            return fail(error, QStringLiteral("单次素材验证的不同字节总量超出限制"));
        m_ownedBytes += frozen.size(); m_records.insert(digest, {std::move(frozen), size});
    } else if (size) found->pngSize = *size;
    m_hints.insert({quintptr(source.constData()), source.size()}, digest);
    return true;
}

bool CMaterialValidationCache::ValidateBlob(const QByteArray& bytes, QByteArray& digest, QString* error,
                                            const QByteArray& expectedDigest) try
{
    if (error) error->clear(); QByteArray key, frozen;
    if (!Resolve(bytes, expectedDigest, key, frozen, error) || !Store(bytes, key, std::move(frozen), {}, error)) return false;
    digest = std::move(key); return true;
}
catch (...) { return fail(error, QStringLiteral("无法验证素材字节")); }

bool CMaterialValidationCache::ValidatePng(const QByteArray& bytes, const QSize& size, QByteArray& digest,
                                           QString* error, const QByteArray& expectedDigest) try
{
    if (error) error->clear(); QByteArray key, frozen;
    if (bytes.isEmpty() || size.isEmpty() || !Resolve(bytes, expectedDigest, key, frozen, error))
        return error && !error->isEmpty() ? false : fail(error, QStringLiteral("素材 PNG 或尺寸为空"));
    const auto found = m_records.constFind(key);
    if (found != m_records.cend() && found->pngSize) {
        if (*found->pngSize != size) return fail(error, QStringLiteral("同一素材 PNG 的声明尺寸不一致"));
    } else {
        const auto& owned = found == m_records.cend() ? frozen : found->bytes;
        QSize actual; QImage decoded;
        if (!CPngImage::Size(owned, actual, error) || actual != size)
            return fail(error, QStringLiteral("素材 PNG 与声明尺寸不一致"));
        ++m_pngDecodeAttempts;
        if (!CPngImage::Decode(owned, decoded, error) || decoded.size() != size) return false;
    }
    if (!Store(bytes, key, std::move(frozen), size, error)) return false;
    digest = std::move(key); return true;
}
catch (...) { return fail(error, QStringLiteral("无法验证素材 PNG")); }

bool CMaterialValidationCache::Merge(const CMaterialValidationCache& verified, QString* error)
{
    if (m_records.isEmpty()) {
        m_records = verified.m_records; m_hints = verified.m_hints; m_ownedBytes = verified.m_ownedBytes; return true;
    }
    for (auto item = verified.m_records.cbegin(); item != verified.m_records.cend(); ++item) {
        const auto existing = m_records.constFind(item.key());
        if (existing != m_records.cend() && (existing->bytes != item->bytes ||
            (existing->pngSize && item->pngSize && existing->pngSize != item->pngSize)))
            return fail(error, QStringLiteral("已验证素材缓存的内容或尺寸发生冲突"));
        if (!Store(item->bytes, item.key(), item->bytes, item->pngSize, error)) return false;
    }
    return true;
}

void CMaterialValidationCache::Retain(const QSet<QByteArray>& digests)
{
    QVector<QByteArray> remove;
    for (auto item = m_records.cbegin(); item != m_records.cend(); ++item) if (!digests.contains(item.key())) remove.append(item.key());
    for (const auto& key : remove) { m_ownedBytes -= m_records.value(key).bytes.size(); m_records.remove(key); }
    QVector<QPair<quintptr, qsizetype>> obsolete;
    for (auto item = m_hints.cbegin(); item != m_hints.cend(); ++item) if (!digests.contains(*item)) obsolete.append(item.key());
    for (const auto& key : obsolete) m_hints.remove(key);
}
}
