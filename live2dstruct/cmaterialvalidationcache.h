#ifndef CMATERIALVALIDATIONCACHE_H
#define CMATERIALVALIDATIONCACHE_H

#include <QByteArray>
#include <QMap>
#include <QSize>
#include <QString>
#include <QSet>
#include <optional>

namespace live2dstruct {
class CMaterialValidationCache
{
public:
    bool ValidateBlob(const QByteArray& bytes, QByteArray& digest, QString* error = nullptr,
                      const QByteArray& expectedDigest = {});
    bool ValidatePng(const QByteArray& bytes, const QSize& size, QByteArray& digest, QString* error = nullptr,
                     const QByteArray& expectedDigest = {});
    qint64 HashedBytes() const { return m_hashedBytes; }
    int PngDecodeAttempts() const { return m_pngDecodeAttempts; }

private:
    friend class CModelArtwork;
    struct Record { QByteArray bytes; std::optional<QSize> pngSize; };
    bool Resolve(const QByteArray& bytes, const QByteArray& expected, QByteArray& digest, QByteArray& frozen, QString* error);
    bool Store(const QByteArray& source, const QByteArray& digest, QByteArray frozen, std::optional<QSize> size, QString* error);
    bool Merge(const CMaterialValidationCache& verified, QString* error);
    void Retain(const QSet<QByteArray>& digests);
    QMap<QByteArray, Record> m_records;
    QMap<QPair<quintptr, qsizetype>, QByteArray> m_hints;
    qint64 m_ownedBytes = 0, m_hashedBytes = 0;
    int m_pngDecodeAttempts = 0;
};
}

#endif
