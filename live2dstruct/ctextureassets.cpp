#include "ctextureassets.h"
#include "std/cpngimage.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QJsonObject>
#include <QSet>
#include <cmath>
#include <limits>
#include <exception>

namespace live2dstruct {
namespace {
bool fail(QString* error, const QString& message) { if (error) *error = message; return false; }
bool validSlot(int slot) { return slot >= 0 && slot < CTextureAssets::MaximumSlots; }
bool validPath(const QString& path) { return !path.isEmpty() && !path.contains(QChar::Null) && QFileInfo(path).isAbsolute(); }
bool decode(const QByteArray& png, const QSize& size, QString* error)
{
    QSize actual;
    if (!CPngImage::Size(png, actual, error)) return false;
    if (actual != size) return fail(error, QStringLiteral("纹理尺寸不匹配：需要 %1 × %2，实际为 %3 × %4")
        .arg(size.width()).arg(size.height()).arg(actual.width()).arg(actual.height()));
    QImage image;
    return CPngImage::Decode(png, image, error);
}
bool takeBytes(qint64 size, qint64& total, QString* error)
{
    if (size < 1 || size > CTextureAssets::MaximumBytes - total)
        return fail(error, QStringLiteral("纹理数据为空或总大小超出支持范围"));
    total += size;
    return true;
}
bool readInt(const QJsonValue& value, int& result)
{
    const double number = value.toDouble(std::numeric_limits<double>::quiet_NaN());
    if (!value.isDouble() || !std::isfinite(number) || std::floor(number) != number ||
        number < 0 || number > std::numeric_limits<int>::max()) return false;
    result = static_cast<int>(number);
    return true;
}
}

bool CTextureAssets::MakeSource(const QString& path, const QByteArray& png, Source& result, QString* error)
{
    if (!validPath(path) || png.size() > MaximumBytes) return fail(error, QStringLiteral("纹理来源路径或大小无效"));
    QImage image;
    if (!CPngImage::Decode(png, image, error)) return false;
    Source candidate{QDir::cleanPath(path), png, QCryptographicHash::hash(png, QCryptographicHash::Sha256), image.size()};
    result = std::move(candidate);
    if (error) error->clear();
    return true;
}

bool CTextureAssets::ValidateCatalog(const Catalog& sources, QString* error)
{
    if (sources.size() > MaximumSlots) return fail(error, QStringLiteral("纹理槽数量超出支持范围"));
    qint64 total = 0;
    QMap<QString, QByteArray> identities;
    for (auto it = sources.cbegin(); it != sources.cend(); ++it) {
        if (!validSlot(it.key()) || !validPath(it->path) || it->digest.size() != 32 || !it->size.isValid() ||
            it->size.isEmpty() || !takeBytes(it->png.size(), total, error)) return fail(error, QStringLiteral("纹理槽 %1 的来源信息无效").arg(it.key()));
        const auto digest = QCryptographicHash::hash(it->png, QCryptographicHash::Sha256);
        const auto path = QDir::cleanPath(it->path);
        if (digest != it->digest || (identities.contains(path) && identities.value(path) != digest))
            return fail(error, QStringLiteral("纹理来源摘要不一致：%1").arg(it->path));
        if (!identities.contains(path) && !decode(it->png, it->size, error)) return false;
        QSize size;
        if (!CPngImage::Size(it->png, size, error) || size != it->size) return fail(error, QStringLiteral("纹理来源尺寸不一致"));
        identities.insert(path, digest);
    }
    if (error) error->clear();
    return true;
}

bool CTextureAssets::ValidateState(const Catalog& sources, const State& state, QString* error)
{
    if (state.size() > sources.size()) return fail(error, QStringLiteral("纹理覆盖包含不存在的槽"));
    qint64 total = 0;
    for (auto it = state.cbegin(); it != state.cend(); ++it) {
        const auto source = sources.constFind(it.key());
        if (!validSlot(it.key()) || source == sources.cend()) return fail(error, QStringLiteral("纹理槽 %1 不存在").arg(it.key()));
        if (!takeBytes(it->size(), total, error) || !decode(*it, source->size, error)) return false;
        if (*it == source->png) return fail(error, QStringLiteral("原始纹理不应另存为覆盖数据"));
    }
    if (error) error->clear();
    return true;
}

bool CTextureAssets::Replace(const Catalog& sources, const State& state, int slot, const QByteArray& png,
                             State& result, QString* error)
{
    if (!ValidateState(sources, state, error)) return false;
    const auto source = sources.constFind(slot);
    if (!validSlot(slot) || source == sources.cend()) return fail(error, QStringLiteral("所选纹理槽不存在"));
    if (png.size() > MaximumBytes || !decode(png, source->size, error)) return false;
    State candidate = state;
    if (png == source->png) candidate.remove(slot); else candidate.insert(slot, png);
    if (!ValidateState(sources, candidate, error)) return false;
    result.swap(candidate);
    if (error) error->clear();
    return true;
}

CTextureAssets::State CTextureAssets::Effective(const Catalog& sources, const State& state)
{
    State result;
    for (auto it = sources.cbegin(); it != sources.cend(); ++it) result.insert(it.key(), state.value(it.key(), it->png));
    return result;
}

bool CTextureAssets::CaptureDraft(const Catalog& sources, const State& state, Draft& result, QString* error)
{
    if (!ValidateCatalog(sources, error) || !ValidateState(sources, state, error)) return false;
    Draft candidate;
    for (auto it = state.cbegin(); it != state.cend(); ++it) {
        const auto& source = sources[it.key()];
        candidate.insert(it.key(), {source.path, source.digest, source.size, *it});
    }
    result.swap(candidate);
    if (error) error->clear();
    return true;
}

bool CTextureAssets::RestoreDraft(const Catalog& sources, const Draft& draft, State& result, QString* error)
{
    if (!ValidateCatalog(sources, error) || !ValidateDraft(draft, error)) return false;
    State candidate;
    for (auto it = draft.cbegin(); it != draft.cend(); ++it) {
        const auto source = sources.constFind(it.key());
        if (source == sources.cend() || QDir::cleanPath(it->sourcePath) != QDir::cleanPath(source->path) ||
            it->sourceDigest != source->digest || it->size != source->size)
            return fail(error, QStringLiteral("会话纹理槽 %1 与当前加载的原始素材不同").arg(it.key()));
        if (it->png != source->png) candidate.insert(it.key(), it->png);
    }
    if (!ValidateState(sources, candidate, error)) return false;
    result.swap(candidate);
    if (error) error->clear();
    return true;
}

bool CTextureAssets::ValidateDraft(const Draft& draft, QString* error)
{
    if (draft.size() > MaximumSlots) return fail(error, QStringLiteral("会话纹理槽数量超出支持范围"));
    qint64 total = 0;
    for (auto it = draft.cbegin(); it != draft.cend(); ++it) {
        if (!validSlot(it.key()) || !validPath(it->sourcePath) || it->sourceDigest.size() != 32 ||
            !it->size.isValid() || it->size.isEmpty()) return fail(error, QStringLiteral("会话纹理来源信息无效"));
        if (!takeBytes(it->png.size(), total, error) || !decode(it->png, it->size, error)) return false;
    }
    if (error) error->clear();
    return true;
}

bool CTextureAssets::ToJson(const Draft& draft, QJsonArray& result, QString* error)
{
    if (!ValidateDraft(draft, error)) return false;
    QJsonArray array;
    for (auto it = draft.cbegin(); it != draft.cend(); ++it) array.append(QJsonObject{
        {QStringLiteral("slot"), it.key()}, {QStringLiteral("sourcePath"), it->sourcePath},
        {QStringLiteral("sourceDigest"), QString::fromLatin1(it->sourceDigest.toHex())},
        {QStringLiteral("width"), it->size.width()}, {QStringLiteral("height"), it->size.height()},
        {QStringLiteral("png"), QString::fromLatin1(it->png.toBase64())}});
    result.swap(array);
    if (error) error->clear();
    return true;
}

bool CTextureAssets::FromJson(const QJsonArray& array, Draft& result, QString* error)
{
    if (array.size() > MaximumSlots) return fail(error, QStringLiteral("会话纹理槽数量超出支持范围"));
    Draft candidate;
    qint64 total = 0;
    const QSet<QString> fields{QStringLiteral("slot"), QStringLiteral("sourcePath"), QStringLiteral("sourceDigest"),
                              QStringLiteral("width"), QStringLiteral("height"), QStringLiteral("png")};
    for (const auto& value : array) {
        if (!value.isObject()) return fail(error, QStringLiteral("会话纹理条目格式无效"));
        const auto object = value.toObject(); const auto keys = object.keys();
        int slot = -1, width = 0, height = 0;
        if (QSet<QString>(keys.cbegin(), keys.cend()) != fields || !readInt(object.value("slot"), slot) ||
            !validSlot(slot) || candidate.contains(slot) || !readInt(object.value("width"), width) ||
            !readInt(object.value("height"), height) || !object.value("sourcePath").isString() ||
            !object.value("sourceDigest").isString() || !object.value("png").isString())
            return fail(error, QStringLiteral("会话纹理字段、槽号或尺寸无效"));
        const auto encodedDigest = object.value("sourceDigest").toString().toLatin1();
        const auto digest = QByteArray::fromHex(encodedDigest);
        if (encodedDigest.size() != 64 || digest.size() != 32 || digest.toHex() != encodedDigest)
            return fail(error, QStringLiteral("会话纹理来源摘要无效"));
        const auto encoded = object.value("png").toString();
        if (encoded.size() > (MaximumBytes - total + 2) / 3 * 4)
            return fail(error, QStringLiteral("会话纹理数据超出支持范围"));
        const auto ascii = encoded.toLatin1();
        const auto decoded = QByteArray::fromBase64Encoding(ascii, QByteArray::AbortOnBase64DecodingErrors);
        if (QString::fromLatin1(ascii) != encoded || !decoded || decoded.decoded.toBase64() != ascii ||
            !takeBytes(decoded.decoded.size(), total, error)) return fail(error, QStringLiteral("会话纹理 PNG 编码无效"));
        candidate.insert(slot, {object.value("sourcePath").toString(), digest, QSize(width, height), decoded.decoded});
    }
    if (!ValidateDraft(candidate, error)) return false;
    result.swap(candidate);
    if (error) error->clear();
    return true;
}
}
