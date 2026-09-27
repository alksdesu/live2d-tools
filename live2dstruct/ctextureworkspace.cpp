#include "ctextureworkspace.h"
#include "std/cpngimage.h"
#include <QCryptographicHash>
#include <QDataStream>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QIODevice>
#include <QJsonArray>
#include <QSet>
#include <cmath>
#include <exception>
#include <limits>

namespace live2dstruct {
namespace {
using Workspace = CTextureWorkspace;
bool fail(QString* error, const QString& message) { if (error) *error = message; return false; }
bool validSlot(int slot) { return slot >= 0 && slot < CTextureAssets::MaximumSlots; }
bool validText(const QString& value, qsizetype maximum)
{ return !value.trimmed().isEmpty() && value.size() <= maximum && !value.contains(QChar::Null); }
QByteArray owned(const QByteArray& value) { return QByteArray(value.constData(), value.size()); }
QString owned(const QString& value) { return QString(value.constData(), value.size()); }
QMap<int, Workspace::Origin> origins(const CTextureAssets::Catalog& sources)
{
    QMap<int, Workspace::Origin> result;
    for (auto it = sources.cbegin(); it != sources.cend(); ++it)
        result.insert(it.key(), {QDir::cleanPath(it->path), it->digest, it->size});
    return result;
}
QByteArray sourceFingerprint(const QMap<int, Workspace::Origin>& sources)
{
    QByteArray data; QDataStream stream(&data, QIODevice::WriteOnly); stream.setVersion(QDataStream::Qt_6_0);
    stream << qint64(sources.size());
    for (auto it = sources.cbegin(); it != sources.cend(); ++it)
        stream << it.key() << it->path << it->digest << it->size;
    return stream.status() == QDataStream::Ok ? QCryptographicHash::hash(data, QCryptographicHash::Sha256) : QByteArray{};
}
bool pixelBudget(const QByteArray& digest, const QSize& size, QMap<QByteArray, QSize>& sizes,
                 qint64& total, QString* error)
{
    if (size.width() < 1 || size.height() < 1 || qint64(size.width()) * size.height() > Workspace::MaximumDecodedBytes / 4)
        return fail(error, QStringLiteral("图集像素尺寸超出支持范围"));
    const auto found = sizes.constFind(digest);
    if (found != sizes.cend()) return *found == size || fail(error, QStringLiteral("同一图集摘要的尺寸不一致"));
    const qint64 bytes = qint64(size.width()) * size.height() * 4;
    if (bytes > Workspace::MaximumDecodedBytes - total) return fail(error, QStringLiteral("图集解码像素总量超出支持范围"));
    total += bytes; sizes.insert(digest, size); return true;
}
bool bytesBudget(const QByteArray& bytes, QSet<QByteArray>& seen, qint64& total, QString* error)
{
    if (bytes.isEmpty() || bytes.size() > CTextureAssets::MaximumBytes) return fail(error, QStringLiteral("图集 PNG 数据为空或过大"));
    const auto digest = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
    if (seen.contains(digest)) return true;
    if (bytes.size() > CTextureAssets::MaximumBytes - total) return fail(error, QStringLiteral("图集 PNG 总量超出支持范围"));
    total += bytes.size(); seen.insert(digest); return true;
}
bool fields(const QJsonObject& object, const QSet<QString>& expected)
{
    const auto keys = object.keys(); return QSet<QString>(keys.cbegin(), keys.cend()) == expected;
}
bool integer(const QJsonValue& value, int& result, int minimum = 0)
{
    const double number = value.toDouble(std::numeric_limits<double>::quiet_NaN());
    if (!value.isDouble() || !std::isfinite(number) || std::floor(number) != number ||
        number < minimum || number > std::numeric_limits<int>::max()) return false;
    result = int(number); return true;
}
bool digestValue(const QJsonValue& value, QByteArray& result)
{
    if (!value.isString()) return false;
    const auto text = value.toString().toLatin1(); const auto bytes = QByteArray::fromHex(text);
    if (text.size() != 64 || bytes.size() != 32 || bytes.toHex() != text) return false;
    result = bytes; return true;
}
}
bool CTextureWorkspace::Atlas::operator==(const Atlas& other) const
{ return id == other.id && name == other.name && sourceSlot == other.sourceSlot && size == other.size && png == other.png; }
bool CTextureWorkspace::State::operator==(const State& other) const
{ return sourceFingerprint == other.sourceFingerprint && atlases == other.atlases; }
bool CTextureWorkspace::Origin::operator==(const Origin& other) const
{ return path == other.path && digest == other.digest && size == other.size; }
bool CTextureWorkspace::Draft::operator==(const Draft& other) const
{ return origins == other.origins && state == other.state; }

bool CTextureWorkspace::ValidateDraft(const Draft& draft, QString* error) try
{
    if (draft.origins.size() > CTextureAssets::MaximumSlots || draft.state.atlases.size() > CTextureAssets::MaximumSlots ||
        draft.state.sourceFingerprint.size() != 32 || draft.state.sourceFingerprint != sourceFingerprint(draft.origins))
        return fail(error, QStringLiteral("图集工作区来源指纹无效"));
    QMap<QString, QByteArray> paths; QMap<QByteArray, QSize> sizes; qint64 decoded = 0, compressed = 0;
    QSet<QByteArray> pngs;
    for (auto it = draft.origins.cbegin(); it != draft.origins.cend(); ++it) {
        if (!validSlot(it.key()) || !validText(it->path, 32768) || !QFileInfo(it->path).isAbsolute() ||
            QDir::cleanPath(it->path) != it->path || it->digest.size() != 32 ||
            (paths.contains(it->path) && paths.value(it->path) != it->digest))
            return fail(error, QStringLiteral("图集来源身份无效"));
        if (!pixelBudget(it->digest, it->size, sizes, decoded, error)) return false;
        paths.insert(it->path, it->digest);
    }
    QSet<QString> ids;
    int generated = draft.origins.isEmpty() ? 0 : draft.origins.lastKey() + 1;
    for (auto it = draft.state.atlases.cbegin(); it != draft.state.atlases.cend(); ++it) {
        if (!validSlot(it.key()) || !validText(it->id, 256) || !validText(it->name, 256) || ids.contains(it->id))
            return fail(error, QStringLiteral("图集槽、名称或稳定标识无效"));
        ids.insert(it->id);
        const auto original = draft.origins.constFind(it.key());
        if (original != draft.origins.cend()) {
            if (it->sourceSlot != it.key() || it->id != QStringLiteral("source:%1").arg(it.key()) ||
                (it->png.isEmpty() && it->size != original->size)) return fail(error, QStringLiteral("原图集槽身份或尺寸不能被替换"));
        } else if (it->sourceSlot != -1 || it.key() != generated++ || it->id.startsWith(QLatin1String("source:")) || it->png.isEmpty())
            return fail(error, QStringLiteral("生成图集必须使用新的连续槽和独立标识"));
        if (it->png.isEmpty()) continue;
        QSize size; const auto digest = QCryptographicHash::hash(it->png, QCryptographicHash::Sha256);
        if (!CPngImage::Size(it->png, size, error) || size != it->size)
            return fail(error, QStringLiteral("图集声明尺寸与 PNG 不一致"));
        if (original != draft.origins.cend() && digest == original->digest)
            return fail(error, QStringLiteral("原始图集不应另存为覆盖 PNG"));
        if (!bytesBudget(it->png, pngs, compressed, error) || !pixelBudget(digest, size, sizes, decoded, error)) return false;
    }
    for (auto it = draft.origins.cbegin(); it != draft.origins.cend(); ++it)
        if (!draft.state.atlases.contains(it.key())) return fail(error, QStringLiteral("图集工作区缺少原始槽"));
    pngs.clear();
    for (const auto& atlas : draft.state.atlases) if (!atlas.png.isEmpty()) {
        const auto digest = QCryptographicHash::hash(atlas.png, QCryptographicHash::Sha256);
        if (pngs.contains(digest)) continue;
        QImage image; if (!CPngImage::Decode(atlas.png, image, error)) return false;
        pngs.insert(digest);
    }
    if (error) error->clear(); return true;
}
catch (const std::exception&) { return fail(error, QStringLiteral("无法验证图集工作区")); }

bool CTextureWorkspace::Validate(const CTextureAssets::Catalog& sources, const State& state, QString* error) try
{
    Draft draft{origins(sources), state};
    QSet<QByteArray> seen; qint64 bytes = 0;
    for (const auto& source : sources) if (!bytesBudget(source.png, seen, bytes, error)) return false;
    for (const auto& atlas : state.atlases) if (!atlas.png.isEmpty() && !bytesBudget(atlas.png, seen, bytes, error)) return false;
    return ValidateDraft(draft, error) && CTextureAssets::ValidateCatalog(sources, error);
}
catch (const std::exception&) { return fail(error, QStringLiteral("无法验证图集素材")); }

bool CTextureWorkspace::Freeze(const CTextureAssets::Catalog& sources, const State& state, State& result, QString* error) try
{
    State candidate; candidate.sourceFingerprint = owned(state.sourceFingerprint);
    QMap<QByteArray, QByteArray> blobs; qint64 total = 0;
    for (auto it = state.atlases.cbegin(); it != state.atlases.cend(); ++it) {
        QByteArray png;
        if (!it->png.isEmpty()) {
            const auto digest = QCryptographicHash::hash(it->png, QCryptographicHash::Sha256);
            const auto found = blobs.constFind(digest);
            if (found != blobs.cend()) {
                if (*found != it->png) return fail(error, QStringLiteral("同一图集摘要对应不同 PNG"));
                png = *found;
            } else {
                if (it->png.size() > CTextureAssets::MaximumBytes - total) return fail(error, QStringLiteral("图集 PNG 总量超出支持范围"));
                total += it->png.size(); png = owned(it->png); blobs.insert(digest, png);
            }
        }
        candidate.atlases.insert(it.key(), {owned(it->id), owned(it->name), it->sourceSlot, it->size, std::move(png)});
    }
    if (!Validate(sources, candidate, error)) return false;
    result = std::move(candidate); return true;
}
catch (const std::exception&) { return fail(error, QStringLiteral("无法冻结图集素材")); }

bool CTextureWorkspace::Initialize(const CTextureAssets::Catalog& sources, const CTextureAssets::State& legacy,
                                   State& result, QString* error) try
{
    if (!CTextureAssets::ValidateState(sources, legacy, error)) return false;
    State candidate; candidate.sourceFingerprint = sourceFingerprint(origins(sources));
    for (auto it = sources.cbegin(); it != sources.cend(); ++it) {
        auto name = QFileInfo(it->path).fileName(); if (name.size() > 256) name = name.left(256);
        candidate.atlases.insert(it.key(), {QStringLiteral("source:%1").arg(it.key()), name, it.key(), it->size, legacy.value(it.key())});
    }
    return Freeze(sources, candidate, result, error);
}
catch (const std::exception&) { return fail(error, QStringLiteral("无法初始化图集工作区")); }

bool CTextureWorkspace::ValidateTransition(const CTextureAssets::Catalog& sources, const State& before,
                                           const State& after, QString* error)
{
    return ValidateTransition(sources, before, after, Transition::PreserveIdentity, error);
}

bool CTextureWorkspace::ValidateTransition(const CTextureAssets::Catalog& sources, const State& before,
                                           const State& after, Transition transition, QString* error)
{
    if (transition != Transition::PreserveIdentity && transition != Transition::ReplaceGenerated)
        return fail(error, QStringLiteral("图集身份替换策略无效"));
    if (!Validate(sources, before, error) || !Validate(sources, after, error)) return false;
    for (auto it = before.atlases.cbegin(); it != before.atlases.cend(); ++it) {
        const auto next = after.atlases.constFind(it.key());
        if (next == after.atlases.cend()) continue;
        const bool replace = transition == Transition::ReplaceGenerated && it->sourceSlot == -1 && next->sourceSlot == -1;
        if (next->sourceSlot != it->sourceSlot || (next->id != it->id && !replace))
            return fail(error, QStringLiteral("已有图集槽的稳定标识不能改变"));
    }
    return true;
}

bool CTextureWorkspace::Effective(const CTextureAssets::Catalog& sources, const State& state,
                                  CTextureAssets::State& result, QString* error)
{
    if (!Validate(sources, state, error)) return false;
    CTextureAssets::State candidate;
    for (auto it = state.atlases.cbegin(); it != state.atlases.cend(); ++it)
        candidate.insert(it.key(), it->png.isEmpty() ? sources.value(it->sourceSlot).png : it->png);
    result.swap(candidate); return true;
}

bool CTextureWorkspace::Fingerprint(const CTextureAssets::Catalog& sources, const State& state,
                                    QByteArray& result, QString* error)
{
    if (!Validate(sources, state, error)) return false;
    QByteArray data; QDataStream stream(&data, QIODevice::WriteOnly); stream.setVersion(QDataStream::Qt_6_0);
    stream << state.sourceFingerprint << qint64(state.atlases.size());
    for (auto it = state.atlases.cbegin(); it != state.atlases.cend(); ++it)
        stream << it.key() << it->id << it->name << it->sourceSlot << it->size <<
            (it->png.isEmpty() ? sources.value(it->sourceSlot).digest : QCryptographicHash::hash(it->png, QCryptographicHash::Sha256));
    if (stream.status() != QDataStream::Ok) return fail(error, QStringLiteral("无法计算图集指纹"));
    result = QCryptographicHash::hash(data, QCryptographicHash::Sha256); return true;
}

bool CTextureWorkspace::CaptureDraft(const CTextureAssets::Catalog& sources, const State& state, Draft& result, QString* error)
{
    Draft candidate; if (!Freeze(sources, state, candidate.state, error)) return false;
    for (auto it = sources.cbegin(); it != sources.cend(); ++it)
        candidate.origins.insert(it.key(), {owned(QDir::cleanPath(it->path)), owned(it->digest), it->size});
    result = std::move(candidate); return true;
}

bool CTextureWorkspace::RestoreDraft(const CTextureAssets::Catalog& sources, const Draft& draft, State& result, QString* error)
{
    if (!ValidateDraft(draft, error)) return false;
    if (origins(sources) != draft.origins) return fail(error, QStringLiteral("会话图集来源与当前模型不同"));
    return Freeze(sources, draft.state, result, error);
}

bool CTextureWorkspace::ToJson(const Draft& draft, QJsonObject& result, QString* error)
{
    if (!ValidateDraft(draft, error)) return false;
    QJsonArray sourceRows, atlasRows;
    for (auto it = draft.origins.cbegin(); it != draft.origins.cend(); ++it) sourceRows.append(QJsonObject{
        {"slot", it.key()}, {"path", it->path}, {"digest", QString::fromLatin1(it->digest.toHex())},
        {"width", it->size.width()}, {"height", it->size.height()}});
    for (auto it = draft.state.atlases.cbegin(); it != draft.state.atlases.cend(); ++it) atlasRows.append(QJsonObject{
        {"slot", it.key()}, {"id", it->id}, {"name", it->name}, {"sourceSlot", it->sourceSlot},
        {"width", it->size.width()}, {"height", it->size.height()}, {"png", QString::fromLatin1(it->png.toBase64())}});
    result = QJsonObject{{"sourceFingerprint", QString::fromLatin1(draft.state.sourceFingerprint.toHex())},
                         {"origins", sourceRows}, {"atlases", atlasRows}};
    return true;
}

bool CTextureWorkspace::FromJson(const QJsonObject& object, Draft& result, QString* error) try
{
    Draft candidate;
    if (!fields(object, {"sourceFingerprint", "origins", "atlases"}) ||
        !digestValue(object.value("sourceFingerprint"), candidate.state.sourceFingerprint) ||
        !object.value("origins").isArray() || !object.value("atlases").isArray())
        return fail(error, QStringLiteral("会话图集工作区字段无效"));
    const auto sourceRows = object.value("origins").toArray(), atlasRows = object.value("atlases").toArray();
    if (sourceRows.size() > CTextureAssets::MaximumSlots || atlasRows.size() > CTextureAssets::MaximumSlots)
        return fail(error, QStringLiteral("会话图集数量超出限制"));
    for (const auto& row : sourceRows) {
        if (!row.isObject()) return fail(error, QStringLiteral("会话图集来源格式无效"));
        const auto item = row.toObject(); int slot, width, height; Origin origin;
        if (!fields(item, {"slot", "path", "digest", "width", "height"}) || !integer(item.value("slot"), slot) ||
            !validSlot(slot) || candidate.origins.contains(slot) || !item.value("path").isString() ||
            !digestValue(item.value("digest"), origin.digest) || !integer(item.value("width"), width, 1) ||
            !integer(item.value("height"), height, 1)) return fail(error, QStringLiteral("会话图集来源字段无效"));
        origin.path = item.value("path").toString(); origin.size = QSize(width, height); candidate.origins.insert(slot, std::move(origin));
    }
    qint64 encodedBytes = 0;
    for (const auto& row : atlasRows) {
        if (!row.isObject()) return fail(error, QStringLiteral("会话图集条目格式无效"));
        const auto item = row.toObject(); int slot, width, height; Atlas atlas;
        if (!fields(item, {"slot", "id", "name", "sourceSlot", "width", "height", "png"}) ||
            !integer(item.value("slot"), slot) || !validSlot(slot) || candidate.state.atlases.contains(slot) ||
            !integer(item.value("sourceSlot"), atlas.sourceSlot, -1) || !integer(item.value("width"), width, 1) ||
            !integer(item.value("height"), height, 1) || !item.value("id").isString() || !item.value("name").isString() ||
            !item.value("png").isString()) return fail(error, QStringLiteral("会话图集字段无效"));
        const auto encoded = item.value("png").toString();
        if (encoded.size() > (CTextureAssets::MaximumBytes - encodedBytes + 2) / 3 * 4)
            return fail(error, QStringLiteral("会话图集数据超出限制"));
        const auto ascii = encoded.toLatin1(); const auto decoded = QByteArray::fromBase64Encoding(ascii, QByteArray::AbortOnBase64DecodingErrors);
        if (QString::fromLatin1(ascii) != encoded || !decoded || decoded.decoded.toBase64() != ascii)
            return fail(error, QStringLiteral("会话图集 PNG 编码无效"));
        encodedBytes += decoded.decoded.size();
        if (encodedBytes > CTextureAssets::MaximumBytes) return fail(error, QStringLiteral("会话图集数据超出限制"));
        atlas.id = item.value("id").toString(); atlas.name = item.value("name").toString();
        atlas.size = QSize(width, height); atlas.png = decoded.decoded; candidate.state.atlases.insert(slot, std::move(atlas));
    }
    if (!ValidateDraft(candidate, error)) return false;
    result = std::move(candidate); return true;
}
catch (const std::exception&) { return fail(error, QStringLiteral("无法读取会话图集工作区")); }
}
