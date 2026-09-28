#include "cpsdreader.h"
#include "std/cexportcontrol.h"
#include "psd/psdSamples/Zip/Psdminiz.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QScopeGuard>
#include <QSet>
#include <QtEndian>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>

namespace {
struct Failure { QString message; };
[[noreturn]] void invalid(const QString& message) { throw Failure{message}; }

struct Cursor
{
    const char* data = nullptr;
    qsizetype size = 0, position = 0;
    qsizetype remaining() const { return size - position; }
    const char* take(qint64 count) {
        if (count < 0 || count > remaining()) invalid(QStringLiteral("PSD 区段或通道超出文件边界"));
        const char* result = data + position; position += qsizetype(count); return result;
    }
    quint8 u8() { return quint8(*take(1)); }
    quint16 u16() { return qFromBigEndian<quint16>(take(2)); }
    qint16 i16() { return qFromBigEndian<qint16>(take(2)); }
    quint32 u32() { return qFromBigEndian<quint32>(take(4)); }
    qint32 i32() { return qFromBigEndian<qint32>(take(4)); }
    Cursor sub(qint64 count) { return {take(count), qsizetype(count), 0}; }
    QByteArray bytes(qint64 count) { return QByteArray(take(count), qsizetype(count)); }
    void padding(int maximum) {
        if (remaining() > maximum) invalid(QStringLiteral("PSD 区段包含未解释的尾随数据"));
        while (remaining()) if (u8()) invalid(QStringLiteral("PSD 区段填充不是零"));
    }
};

struct ChannelSpan
{
    qsizetype offset = 0, length = 0;
};

struct Reader
{
    const CPsdReadLimits& limits;
    CExportControl& control;
    CPsdSnapshot snapshot;
    qint64 decoded = 0, metadata = 0;
    int metadataEntries = 0;
    bool mergedAvailable = true;
    bool directoryOnly = false;
    QVector<QMap<int, ChannelSpan>>* channelIndex = nullptr;
    qint64 maximumLayerWorkingBytes = 0;

    qint64 mergedBytes() const {
        return qint64(snapshot.canvasSize.width()) * snapshot.canvasSize.height() *
            (snapshot.channels + (!directoryOnly && mergedAvailable ? 4 : 0));
    }
    void check() { if (!control.Checkpoint()) invalid(control.Error()); }
    void reserve(qint64 bytes, qint64& used, qint64 maximum, const QString& label) {
        check();
        if (bytes < 0 || bytes > maximum - used) invalid(QStringLiteral("PSD %1超出预算").arg(label));
        used += bytes;
    }
    void metadataBytes(qint64 bytes) { reserve(bytes, metadata, limits.maximumMetadataBytes, QStringLiteral("元数据")); }
    QRect rectangle(Cursor& cursor) {
        const qint64 top = cursor.i32(), left = cursor.i32(), bottom = cursor.i32(), right = cursor.i32();
        if (right < left || bottom < top || right - left > limits.maximumDimension || bottom - top > limits.maximumDimension ||
            (right == left && left == std::numeric_limits<int>::min()) ||
            (bottom == top && top == std::numeric_limits<int>::min()) ||
            (right - left) * (bottom - top) > limits.maximumLayerPixels)
            invalid(QStringLiteral("PSD 图层或蒙版边界无效或超出预算"));
        return QRect(int(left), int(top), int(right - left), int(bottom - top));
    }
    void diagnostic(CPsdLayer& layer, const QString& code, const QString& message, bool equivalent = false) {
        layer.diagnostics.append({code, message});
        if (!equivalent) layer.pixelEquivalent = false;
    }
    QByteArray decode(Cursor data, int width, int height, int planes = 1) {
        const qint64 count = qint64(width) * height * planes;
        reserve(count, decoded, limits.maximumDecodedBytes, QStringLiteral("通道解码总量"));
        const int compression = data.u16();
        QByteArray output;
        if (!directoryOnly) output = QByteArray(qsizetype(count), Qt::Uninitialized);
        if (compression == 0) {
            if (data.remaining() != count) invalid(QStringLiteral("PSD RAW 通道长度与边界不一致"));
            for (qint64 at = 0; at < count; at += 65536) {
                check();
                const auto amount = std::min(qint64(65536), count - at);
                const auto* bytes = data.take(amount);
                if (!directoryOnly) std::memcpy(output.data() + at, bytes, size_t(amount));
            }
        } else if (compression == 1) {
            const qint64 rows = qint64(height) * planes;
            Cursor lengths = data.sub(rows * 2);
            for (qint64 row = 0; row < rows; ++row) {
                check(); Cursor packed = data.sub(lengths.u16()); int written = 0;
                qsizetype checkpointAt = 0;
                while (packed.remaining()) {
                    if (packed.position - checkpointAt >= 65536) { check(); checkpointAt = packed.position; }
                    const int value = int(qint8(packed.u8()));
                    if (value == -128) continue;
                    const int amount = value < 0 ? 1 - value : value + 1;
                    if (amount > width - written) invalid(QStringLiteral("PSD RLE 行解压超过声明宽度"));
                    if (value < 0) {
                        const auto repeated = packed.u8();
                        if (!directoryOnly) std::memset(output.data() + row * width + written, repeated, size_t(amount));
                    } else {
                        const auto* bytes = packed.take(amount);
                        if (!directoryOnly) std::memcpy(output.data() + row * width + written, bytes, size_t(amount));
                    }
                    written += amount;
                }
                if (written != width) invalid(QStringLiteral("PSD RLE 行数据不完整"));
            }
            if (data.remaining()) invalid(QStringLiteral("PSD RLE 通道包含尾随数据"));
        } else if (compression == 2 || compression == 3) {
            mz_stream stream{};
            if (mz_inflateInit(&stream) != MZ_OK) invalid(QStringLiteral("无法初始化 PSD ZIP 解码"));
            const auto cleanup = qScopeGuard([&] { mz_inflateEnd(&stream); });
            stream.next_in = reinterpret_cast<const unsigned char*>(data.data + data.position);
            stream.avail_in = unsigned(data.remaining());
            qint64 produced = 0; unsigned char excess = 0;
            std::array<unsigned char, 65536> scratch;
            while (true) {
                check();
                const auto capacity = unsigned(std::min(qint64(65536), count - produced));
                stream.next_out = capacity ? directoryOnly ? scratch.data() : reinterpret_cast<unsigned char*>(output.data() + produced) : &excess;
                stream.avail_out = capacity ? capacity : 1;
                const auto inputBefore = stream.avail_in, available = stream.avail_out;
                const int status = mz_inflate(&stream, MZ_NO_FLUSH);
                const auto wrote = available - stream.avail_out;
                if (wrote > count - produced) invalid(QStringLiteral("PSD ZIP 解压超过声明长度"));
                produced += wrote;
                if (status == MZ_STREAM_END) {
                    if (produced != count || stream.avail_in) invalid(QStringLiteral("PSD ZIP 长度不一致或包含尾随数据"));
                    break;
                }
                if (status != MZ_OK || (inputBefore == stream.avail_in && !wrote)) invalid(QStringLiteral("PSD ZIP 通道损坏或截断"));
            }
            if (compression == 3 && !directoryOnly) for (qint64 row = 0; row < qint64(height) * planes; ++row) {
                check(); auto* pixels = reinterpret_cast<unsigned char*>(output.data() + row * width);
                for (int x = 1; x < width; ++x) pixels[x] = static_cast<unsigned char>(pixels[x] + pixels[x - 1]);
            }
        } else invalid(QStringLiteral("不支持 PSD 通道压缩方式 %1").arg(compression));
        return output;
    }
    QImage rgba(const QMap<int, QByteArray>& planes, QSize size, bool whiteMatte = false) {
        if (size.isEmpty()) return {};
        const qint64 count = qint64(size.width()) * size.height();
        reserve(count * 4, decoded, limits.maximumDecodedBytes, QStringLiteral("图层和预览 RGBA 总量"));
        QImage image(size, QImage::Format_ARGB32);
        if (image.isNull()) invalid(QStringLiteral("无法分配 PSD 图层图像"));
        for (int y = 0; y < size.height(); ++y) {
            check(); auto* row = reinterpret_cast<QRgb*>(image.scanLine(y));
            for (int x = 0; x < size.width(); ++x) {
                const qsizetype at = qsizetype(y) * size.width() + x;
                const auto channel = [&](int id, int fallback) { auto found = planes.constFind(id); return found == planes.cend() ? fallback : int(quint8(found->at(at))); };
                const int alpha = channel(-1, 255);
                const auto color = [&](int id) {
                    const int value = channel(id, 0);
                    return !whiteMatte ? value : !alpha ? 0 :
                        std::clamp(((value + alpha - 255) * 255 + alpha / 2) / alpha, 0, 255);
                };
                row[x] = qRgba(color(0), color(1), color(2), alpha);
            }
        }
        return image;
    }
    bool zeroPadding(Cursor& payload, qsizetype maximum) {
        if (payload.remaining() > maximum) return false;
        while (payload.remaining()) if (payload.u8()) return false;
        return true;
    }
    bool finiteDouble(Cursor& payload) {
        if (payload.remaining() < 8) return false;
        const auto bits = qFromBigEndian<quint64>(payload.take(8));
        double value;
        std::memcpy(&value, &bits, sizeof(value));
        return std::isfinite(value);
    }
    bool timestampMetadata(Cursor payload) {
        if (payload.remaining() < 8 || payload.u32() != 16) return false;
        const auto nameLength = payload.u32();
        if (nameLength > 1 || (nameLength && (payload.remaining() < 2 || payload.u16()))) return false;
        if (payload.remaining() < 16 || payload.u32() != 8 || payload.bytes(8) != "metadata" || payload.u32() != 1) return false;
        if (payload.remaining() < 17 || payload.u32() != 9 || payload.bytes(9) != "layerTime" || payload.bytes(4) != "doub") return false;
        return finiteDouble(payload) && zeroPadding(payload, 3);
    }
    bool metadataSettings(Cursor payload) {
        if (payload.remaining() < 4) return false;
        const auto count = payload.u32();
        if (count > quint64(payload.remaining() / 16)) return false;
        if (count > quint32(65536 - metadataEntries)) invalid(QStringLiteral("PSD 元数据记录数量超出限制"));
        bool equivalent = true;
        QSet<QByteArray> keys;
        for (quint32 index = 0; index < count; ++index) {
            check(); ++metadataEntries;
            if (payload.remaining() < 16) return false;
            const auto signature = payload.bytes(4), key = payload.bytes(4);
            const auto copy = payload.u8(); auto padding = payload.sub(3);
            const auto length = payload.u32();
            if (length > quint64(payload.remaining())) return false;
            auto item = payload.sub(length);
            const bool neutral = signature == "8BIM" && key == "cust" && copy <= 1 &&
                zeroPadding(padding, 3) && timestampMetadata(item);
            equivalent = neutral && !keys.contains(key) && equivalent;
            keys.insert(key);
        }
        return zeroPadding(payload, 3) && equivalent;
    }
    bool neutralMetadata(const QByteArray& key, Cursor payload) {
        if (key == "clbl" || key == "infx" || key == "knko") {
            if (payload.remaining() != 4) return false;
            const auto value = payload.u8();
            return value == (key == "clbl" ? 1 : 0) && zeroPadding(payload, 3);
        }
        if (key == "lspf") return payload.remaining() == 4 && !(payload.u32() & ~quint32(15));
        if (key == "lclr") {
            if (payload.remaining() != 8) return false;
            payload.u16(); return zeroPadding(payload, 6);
        }
        if (key == "fxrp") return payload.remaining() == 16 && finiteDouble(payload) && finiteDouble(payload);
        if (key == "lyvr") {
            if (payload.remaining() != 4) return false;
            const auto version = payload.u32(); return version == 70 || version == 80;
        }
        if (key == "shmd") return metadataSettings(payload);
        return false;
    }
    void tags(Cursor& cursor, CPsdLayer* layer, int alignment) {
        QSet<QByteArray> unique;
        while (cursor.remaining() >= 12) {
            check();
            if (++metadataEntries > 65536) invalid(QStringLiteral("PSD 元数据记录数量超出限制"));
            const auto signature = cursor.bytes(4);
            if (signature != "8BIM" && signature != "8B64") invalid(QStringLiteral("PSD 附加图层区段签名无效"));
            const auto key = cursor.bytes(4); const auto length = cursor.u32();
            metadataBytes(qint64(length) + 12); auto payload = cursor.sub(length);
            if (alignment > 1) cursor.take((alignment - length % alignment) % alignment);
            if (!layer) {
                if (key == "Mtrn") {
                    if (unique.contains(key) || payload.remaining()) invalid(QStringLiteral("PSD 合成透明标记重复或长度无效"));
                    unique.insert(key);
                    snapshot.hasMergedTransparency = true;
                    continue;
                }
                snapshot.diagnostics.append({QStringLiteral("global-tag"), QStringLiteral("保留文件中的全局区段 %1，未应用其显示效果").arg(QString::fromLatin1(key))});
                continue;
            }
            layer->additionalInfo.append({key, QByteArray(payload.data, payload.size)});
            const bool structural = key == "luni" || key == "lyid" || key == "lsct" || key == "lsdk";
            const bool duplicate = unique.contains(key);
            if (structural && duplicate) invalid(QStringLiteral("PSD 图层结构字段重复：%1").arg(QString::fromLatin1(key)));
            unique.insert(key);
            if (key == "luni") {
                const quint32 count = payload.u32();
                if (count > 65536 || qint64(count) * 2 > payload.remaining()) invalid(QStringLiteral("PSD Unicode 图层名长度无效"));
                QString name; name.reserve(int(count));
                for (quint32 index = 0; index < count; ++index) name.append(QChar(payload.u16()));
                if (!name.isValidUtf16()) invalid(QStringLiteral("PSD Unicode 图层名包含无效代理项"));
                if (name.endsWith(QChar(0))) name.chop(1);
                payload.padding(3); layer->name = name;
            } else if (key == "lyid") {
                layer->layerId = payload.u32(); payload.padding(0);
            } else if (key == "lsct" || key == "lsdk") {
                if (unique.contains(key == "lsct" ? "lsdk" : "lsct")) invalid(QStringLiteral("PSD 图层分组定义冲突"));
                const auto kind = payload.u32(); if (kind > 3) invalid(QStringLiteral("PSD 图层分组类型无效"));
                layer->kind = kind == 3 ? CPsdLayer::Kind::GroupEnd : kind ? CPsdLayer::Kind::Group : CPsdLayer::Kind::Raster;
                if (payload.remaining() >= 8) { if (payload.bytes(4) != "8BIM") invalid(QStringLiteral("PSD 分组混合模式签名无效")); layer->blendMode = payload.bytes(4); }
                if (payload.remaining() >= 4) payload.u32();
                payload.padding(0);
            } else {
                const bool neutral = neutralMetadata(key, payload);
                if (neutral && !duplicate)
                    diagnostic(*layer, QStringLiteral("metadata"), QStringLiteral("已保留图层元数据 %1").arg(QString::fromLatin1(key)), true);
                else diagnostic(*layer, QStringLiteral("unapplied-tag"), QStringLiteral("区段 %1 包含未支持或无效的设置，栅格通道不能视为完整图层外观").arg(QString::fromLatin1(key)));
            }
        }
        cursor.padding(3);
    }
    struct Record { QVector<QPair<int, quint32>> channels; std::optional<QRect> mask, realMask; };
    struct MaskTail {
        std::optional<QRect> realMask;
        quint8 realFlags = 0, realBackground = 0;
        QByteArray parameters;
        bool operator==(const MaskTail& other) const {
            return realMask == other.realMask && realFlags == other.realFlags &&
                realBackground == other.realBackground && parameters == other.parameters;
        }
    };
    QByteArray maskParameters(Cursor& cursor, bool present) {
        if (!present) return {};
        const auto start = cursor.position;
        const int flags = cursor.u8();
        if (flags & ~15) invalid(QStringLiteral("PSD 蒙版参数标记无效"));
        if (flags & 1) cursor.take(1);
        if (flags & 2) cursor.take(8);
        if (flags & 4) cursor.take(1);
        if (flags & 8) cursor.take(8);
        return QByteArray(cursor.data + start, cursor.position - start);
    }
    MaskTail maskTail(Cursor cursor, int flags, bool realFirst) {
        MaskTail result;
        const auto real = [&] {
            result.realFlags = cursor.u8(); result.realBackground = cursor.u8();
            result.realMask = rectangle(cursor);
        };
        if (realFirst) {
            real();
            result.parameters = maskParameters(cursor, (flags | result.realFlags) & 16);
        } else {
            result.parameters = maskParameters(cursor, flags & 16);
            if (cursor.remaining() >= 18) {
                real();
                if (!(flags & 16)) result.parameters = maskParameters(cursor, result.realFlags & 16);
            }
        }
        cursor.padding(3);
        return result;
    }
    MaskTail readMaskTail(Cursor cursor, int flags) {
        std::optional<MaskTail> parameterFirst, realFirst;
        QString reason;
        try { parameterFirst = maskTail(cursor, flags, false); }
        catch (const Failure& failure) { reason = failure.message; }
        if (cursor.remaining() >= 18) {
            try { realFirst = maskTail(cursor, flags, true); }
            catch (const Failure& failure) { if (reason.isEmpty()) reason = failure.message; }
        }
        if (parameterFirst && realFirst && !(*parameterFirst == *realFirst))
            invalid(QStringLiteral("PSD 双蒙版参数布局存在歧义"));
        if (realFirst) return *realFirst;
        if (parameterFirst) return *parameterFirst;
        invalid(reason.isEmpty() ? QStringLiteral("PSD 蒙版扩展数据无效") : reason);
    }
    Record record(Cursor& cursor, CPsdLayer& layer) {
        Record result; layer.bounds = rectangle(cursor);
        const int count = cursor.u16(); if (count > 56) invalid(QStringLiteral("PSD 图层通道数量超出限制"));
        QSet<int> ids;
        for (int index = 0; index < count; ++index) {
            const int id = cursor.i16(); const auto length = cursor.u32();
            if (id < -3 || id > 55 || ids.contains(id) || length < 2) invalid(QStringLiteral("PSD 图层通道 ID 或长度无效"));
            ids.insert(id); result.channels.append({id, length});
        }
        if (cursor.bytes(4) != "8BIM") invalid(QStringLiteral("PSD 图层混合模式签名无效"));
        layer.blendMode = cursor.bytes(4); layer.opacity = cursor.u8();
        const int clipping = cursor.u8(); if (clipping > 1) invalid(QStringLiteral("PSD 裁剪标记无效"));
        layer.clipping = clipping; const int flags = cursor.u8(); layer.visible = !(flags & 2);
        if ((flags & 24) == 24) diagnostic(layer, QStringLiteral("irrelevant-pixels"), QStringLiteral("此图层声明栅格数据不代表最终外观"));
        if (cursor.u8()) invalid(QStringLiteral("PSD 图层保留字节无效"));
        auto extra = cursor.sub(cursor.u32());
        const auto maskLength = extra.u32(); auto mask = extra.sub(maskLength); metadataBytes(maskLength);
        if (maskLength) {
            diagnostic(layer, QStringLiteral("mask"), QStringLiteral("图层含蒙版，已保留原通道但未烘焙蒙版"));
            result.mask = rectangle(mask); mask.u8(); const int maskFlags = mask.u8();
            result.realMask = readMaskTail(mask, maskFlags).realMask;
        }
        const auto rangeLength = extra.u32(); auto ranges = extra.sub(rangeLength); metadataBytes(rangeLength);
        if (rangeLength % 8) invalid(QStringLiteral("PSD 混合范围长度无效"));
        bool standard = true; while (ranges.remaining()) standard = (ranges.u32() == 0x0000ffff) && standard;
        if (!standard) diagnostic(layer, QStringLiteral("blend-ranges"), QStringLiteral("自定义混合范围未应用"));
        const auto nameLength = extra.u8(); metadataBytes(nameLength);
        const auto legacyName = extra.bytes(nameLength); layer.name = QString::fromLatin1(legacyName);
        extra.take((4 - (nameLength + 1) % 4) % 4);
        tags(extra, &layer, 1);
        if (std::none_of(layer.additionalInfo.cbegin(), layer.additionalInfo.cend(), [](const auto& value) { return value.key == "luni"; }) &&
            std::any_of(legacyName.cbegin(), legacyName.cend(), [](char value) { return quint8(value) >= 128; }))
            diagnostic(layer, QStringLiteral("legacy-name-encoding"), QStringLiteral("图层仅提供旧编码名称，当前显示不能用作自动身份匹配"), true);
        if (layer.opacity != 255) diagnostic(layer, QStringLiteral("opacity"), QStringLiteral("图层透明度单独保存，由素材采样按设置应用"), true);
        if (layer.clipping) diagnostic(layer, QStringLiteral("clipping"), QStringLiteral("图层裁剪关系单独保存，未烘焙"));
        if (layer.blendMode != "norm" && !(layer.kind == CPsdLayer::Kind::Group && layer.blendMode == "pass"))
            diagnostic(layer, QStringLiteral("blend-mode"), QStringLiteral("图层混合模式 %1 未烘焙").arg(QString::fromLatin1(layer.blendMode)));
        return result;
    }
    void resources(Cursor cursor) {
        bool hasIcc = false, hasVersion = false;
        while (cursor.remaining()) {
            check();
            if (++metadataEntries > 65536) invalid(QStringLiteral("PSD 元数据记录数量超出限制"));
            if (cursor.bytes(4) != "8BIM") invalid(QStringLiteral("PSD 资源区段签名无效"));
            const auto id = cursor.u16(); const auto length = cursor.u8(); cursor.take(length); if ((length + 1) % 2) cursor.take(1);
            const auto size = cursor.u32(); metadataBytes(qint64(size) + length + 12); const auto data = cursor.bytes(size); if (size % 2) cursor.take(1);
            if (id == 1039) { if (hasIcc) invalid(QStringLiteral("PSD ICC 资源重复")); hasIcc = true; snapshot.iccProfile = data; }
            if (id == 1057) {
                if (hasVersion) invalid(QStringLiteral("PSD 版本信息资源重复")); hasVersion = true;
                Cursor version{data.constData(), data.size(), 0};
                if (version.u32() != 1) invalid(QStringLiteral("PSD 版本信息资源格式不支持"));
                const int available = version.u8(); if (available > 1) invalid(QStringLiteral("PSD 合成预览标记无效"));
                mergedAvailable = available;
            }
        }
    }
    void layerSection(Cursor section) {
        if (!section.remaining()) return;
        auto info = section.sub(section.u32());
        if (info.remaining()) {
            const int signedCount = info.i16(), count = std::abs(signedCount);
            if (count > limits.maximumLayers) invalid(QStringLiteral("PSD 图层数量超出预算"));
            snapshot.hasMergedTransparency = signedCount < 0;
            QVector<Record> records; records.reserve(count); snapshot.layers.reserve(count);
            QMap<quint32, int> ids;
            for (int index = 0; index < count; ++index) {
                check(); CPsdLayer layer; layer.recordIndex = index; records.append(record(info, layer));
                if (layer.layerId) {
                    if (ids.contains(*layer.layerId)) {
                        diagnostic(layer, QStringLiteral("duplicate-layer-id"), QStringLiteral("图层 ID 重复，不能自动作为稳定匹配身份"), true);
                        auto& first = snapshot.layers[ids.value(*layer.layerId)];
                        if (std::none_of(first.diagnostics.cbegin(), first.diagnostics.cend(), [](const auto& value) { return value.code == QLatin1String("duplicate-layer-id"); }))
                            diagnostic(first, QStringLiteral("duplicate-layer-id"), QStringLiteral("图层 ID 重复，不能自动作为稳定匹配身份"), true);
                    } else ids.insert(*layer.layerId, index);
                }
                snapshot.layers.append(std::move(layer));
            }
            qint64 required = mergedBytes();
            const auto account = [&](qint64 amount) {
                if (amount < 0 || required > limits.maximumDecodedBytes || amount > limits.maximumDecodedBytes - required)
                    invalid(QStringLiteral("PSD 通道和图像总量超出预算"));
                required += amount;
            };
            account(0);
            for (int index = 0; index < count; ++index) {
                check();
                const auto& layer = snapshot.layers[index]; const auto& record = records[index];
                int rgb = 0;
                for (const auto& channel : record.channels) {
                    QRect bounds = layer.bounds;
                    if (channel.first == -2 || channel.first == -3) {
                        const auto& mask = channel.first == -2 ? record.mask : record.realMask;
                        if (!mask) invalid(QStringLiteral("PSD 蒙版通道缺少对应边界"));
                        bounds = *mask;
                    }
                    account(qint64(bounds.width()) * bounds.height());
                    if (channel.first >= 0 && channel.first <= 2) rgb |= 1 << channel.first;
                }
                if (rgb == 7 && !layer.bounds.isEmpty() && layer.kind == CPsdLayer::Kind::Raster) {
                    const qint64 pixels = qint64(layer.bounds.width()) * layer.bounds.height();
                    if (!directoryOnly) account(pixels * 4);
                    if (channelIndex && (pixels * 5 + 65536 > maximumLayerWorkingBytes))
                        invalid(QStringLiteral("PSD 单图层读取工作内存超出预算"));
                }
            }
            if (channelIndex) {
                metadataBytes(qint64(count) * sizeof(QMap<int, ChannelSpan>));
                channelIndex->resize(count);
            }
            for (int index = 0; index < count; ++index) {
                auto& layer = snapshot.layers[index]; const auto& record = records[index];
                if (!control.Report(CExportControl::Phase::Preparing, index, count, layer.name)) invalid(control.Error());
                QMap<int, QByteArray> planes;
                for (const auto& channel : record.channels) {
                    auto data = info.sub(channel.second); QRect bounds = layer.bounds;
                    if (channel.first == -2 || channel.first == -3) {
                        const auto& mask = channel.first == -2 ? record.mask : record.realMask;
                        if (!mask) invalid(QStringLiteral("PSD 蒙版通道缺少对应边界")); bounds = *mask;
                    }
                    auto decoded = decode(data, bounds.width(), bounds.height());
                    if (channel.first >= -1 && channel.first <= 2) {
                        planes.insert(channel.first, std::move(decoded));
                        if (channelIndex) {
                            metadataBytes(sizeof(ChannelSpan) + 64);
                            (*channelIndex)[index].insert(channel.first,
                                {data.data - snapshot.bytes.constData(), data.size});
                        }
                    }
                    else diagnostic(layer, QStringLiteral("extra-channel"), QStringLiteral("附加通道 %1 未应用，原数据保留在文件快照").arg(channel.first));
                }
                if (!layer.bounds.isEmpty() && layer.kind == CPsdLayer::Kind::Raster) {
                    if (!planes.contains(0) || !planes.contains(1) || !planes.contains(2)) {
                        diagnostic(layer, QStringLiteral("missing-rgb"), QStringLiteral("此图层没有完整 RGB 栅格素材"));
                    } else if (!directoryOnly) layer.image = rgba(planes, layer.bounds.size());
                }
            }
            info.padding(3);
        }
        if (section.remaining()) {
            const auto globalMaskLength = section.u32(); metadataBytes(globalMaskLength);
            if (globalMaskLength && globalMaskLength < 13) invalid(QStringLiteral("PSD 全局蒙版记录不完整"));
            section.take(globalMaskLength);
            if (globalMaskLength) {
                snapshot.diagnostics.append({QStringLiteral("global-mask"), QStringLiteral("全局蒙版未应用，原始信息保留在文件快照")});
                for (auto& layer : snapshot.layers) diagnostic(layer, QStringLiteral("global-mask"), QStringLiteral("文档存在未应用的全局蒙版"));
            }
            tags(section, nullptr, 4);
        }
        QVector<int> stack;
        for (int index = snapshot.layers.size() - 1; index >= 0; --index) {
            auto& layer = snapshot.layers[index]; layer.parentIndex = stack.isEmpty() ? -1 : stack.last();
            if (layer.kind == CPsdLayer::Kind::GroupEnd) {
                if (stack.isEmpty()) invalid(QStringLiteral("PSD 分组结束标记没有对应分组")); stack.removeLast();
            } else {
                snapshot.displayOrder.append(index);
                if (layer.kind == CPsdLayer::Kind::Group) {
                    if (stack.size() >= limits.maximumDepth) invalid(QStringLiteral("PSD 分组层级超出预算")); stack.append(index);
                }
            }
        }
        if (!stack.isEmpty()) invalid(QStringLiteral("PSD 分组缺少结束标记"));
    }
    void parse() {
        Cursor cursor{snapshot.bytes.constData(), snapshot.bytes.size(), 0};
        if (cursor.bytes(4) != "8BPS") invalid(QStringLiteral("文件不是有效的 PSD"));
        snapshot.version = cursor.u16();
        if (snapshot.version != 1) invalid(QStringLiteral("暂不支持 PSD 版本 %1（PSB 大文档尚未开放）").arg(snapshot.version));
        for (int index = 0; index < 6; ++index) if (cursor.u8()) invalid(QStringLiteral("PSD 文件头保留字节无效"));
        snapshot.channels = cursor.u16(); const auto height = cursor.u32(), width = cursor.u32();
        snapshot.depth = cursor.u16(); snapshot.colorMode = cursor.u16();
        if (snapshot.depth != 8 || snapshot.colorMode != 3) invalid(QStringLiteral("暂仅支持 RGB 8 位 PSD，当前深度 %1、色彩模式 %2").arg(snapshot.depth).arg(snapshot.colorMode));
        if (snapshot.channels < 3 || snapshot.channels > 56 || !width || !height || width > quint32(limits.maximumDimension) || height > quint32(limits.maximumDimension) || qint64(width) * height > limits.maximumLayerPixels)
            invalid(QStringLiteral("PSD 画布尺寸或通道数量超出预算"));
        snapshot.canvasSize = QSize(int(width), int(height));
        if (cursor.u32() != 0) invalid(QStringLiteral("RGB PSD 不应包含色彩模式数据"));
        resources(cursor.sub(cursor.u32()));
        if (mergedBytes() > limits.maximumDecodedBytes) invalid(QStringLiteral("PSD 合成预览总量超出预算"));
        layerSection(cursor.sub(cursor.u32()));
        const auto merged = decode(cursor, width, height, snapshot.channels);
        if (snapshot.hasMergedTransparency && snapshot.channels < 4)
            invalid(QStringLiteral("PSD 合成透明标记缺少透明通道"));
        if (!directoryOnly && mergedAvailable) {
            const qsizetype count = qsizetype(width) * height;
            QMap<int, QByteArray> planes;
            // 临时平面只引用本次已拥有的 merged 缓冲，不向调用方暴露借用数据。
            for (int index = 0; index < 3; ++index) planes.insert(index, QByteArray::fromRawData(merged.constData() + count * index, count));
            if (snapshot.hasMergedTransparency)
                planes.insert(-1, QByteArray::fromRawData(merged.constData() + count * 3, count));
            snapshot.mergedImage = rgba(planes, snapshot.canvasSize, snapshot.hasMergedTransparency);
        }
        if (!mergedAvailable) snapshot.diagnostics.append({QStringLiteral("missing-merged-preview"), QStringLiteral("文件声明没有有效合成预览，不使用占位像素")});
        snapshot.diagnostics.append({QStringLiteral("color-policy"), snapshot.iccProfile.isEmpty() ? QStringLiteral("保留原始 RGB 数值；文件未提供 ICC，不推断或转换色彩空间") : QStringLiteral("保留原始 ICC 与 RGB 数值，未执行色彩空间转换")});
        snapshot.diagnostics.append({QStringLiteral("merged-preview"), QStringLiteral("合成图仅供参考，不替代缺失或不支持的独立图层素材")});
        check();
    }
    CPsdDirectory takeDirectory() {
        CPsdDirectory candidate;
        candidate.bytes = std::move(snapshot.bytes);
        candidate.sha256 = std::move(snapshot.sha256);
        candidate.iccProfile = std::move(snapshot.iccProfile);
        candidate.canonicalPath = std::move(snapshot.canonicalPath);
        candidate.canvasSize = snapshot.canvasSize;
        candidate.version = snapshot.version;
        candidate.depth = snapshot.depth;
        candidate.colorMode = snapshot.colorMode;
        candidate.channels = snapshot.channels;
        candidate.hasMergedTransparency = snapshot.hasMergedTransparency;
        candidate.hasMergedPreview = mergedAvailable;
        candidate.displayOrder = std::move(snapshot.displayOrder);
        candidate.diagnostics = std::move(snapshot.diagnostics);
        candidate.layers.reserve(snapshot.layers.size());
        for (auto& layer : snapshot.layers) {
            check(); CPsdDirectoryLayer next;
            next.recordIndex = layer.recordIndex; next.parentIndex = layer.parentIndex; next.kind = layer.kind;
            next.layerId = layer.layerId; next.name = std::move(layer.name); next.bounds = layer.bounds;
            next.opacity = layer.opacity; next.blendMode = std::move(layer.blendMode);
            next.visible = layer.visible; next.clipping = layer.clipping; next.pixelEquivalent = layer.pixelEquivalent;
            next.additionalInfo = std::move(layer.additionalInfo); next.diagnostics = std::move(layer.diagnostics);
            candidate.layers.append(std::move(next));
        }
        check(); return candidate;
    }
};

void validateLimits(const CPsdReadLimits& limits)
{
    if (limits.maximumFileBytes <= 0 || limits.maximumFileBytes > std::numeric_limits<int>::max() ||
        limits.maximumDecodedBytes <= 0 || limits.maximumDecodedBytes > 8LL * 1024 * 1024 * 1024 ||
        limits.maximumLayerPixels <= 0 || limits.maximumLayerPixels > std::numeric_limits<int>::max() / 4 ||
        limits.maximumMetadataBytes <= 0 || limits.maximumMetadataBytes > std::numeric_limits<int>::max() ||
        limits.maximumLayers <= 0 || limits.maximumLayers > 32768 || limits.maximumDepth <= 0 || limits.maximumDepth > 4096 ||
        limits.maximumDimension <= 0 || limits.maximumDimension > 30000) invalid(QStringLiteral("PSD 读取预算配置无效"));
}
void validateStreamLimits(const CPsdStreamLimits& limits)
{
    validateLimits(limits.document);
    if (limits.maximumLayerWorkingBytes <= 0 || limits.maximumLayerWorkingBytes > 2LL * 1024 * 1024 * 1024)
        invalid(QStringLiteral("PSD 逐层读取工作预算无效"));
}
bool failed(const QString& reason, QString* error, CExportControl& control)
{
    const auto message = reason.isEmpty() ? QStringLiteral("PSD 读取已取消或失败") : reason;
    if (error) *error = message;
    if (control.Checkpoint()) control.Fail(message);
    return false;
}

struct FileData { QByteArray bytes; QString canonicalPath; };
FileData readFile(const QString& path, CExportControl& control, const CPsdReadLimits& limits)
{
    validateLimits(limits); const QString ownedPath(path.constData(), path.size());
    const QFileInfo before(ownedPath); const auto canonical = before.canonicalFilePath();
    const qint64 size = before.size(); const auto modified = before.lastModified(), changed = before.metadataChangeTime();
    if (!before.isFile() || canonical.isEmpty() || size < 26 || size > limits.maximumFileBytes)
        invalid(QStringLiteral("PSD 文件不存在或大小超出预算"));
    QFile file(ownedPath); if (!file.open(QIODevice::ReadOnly)) invalid(QStringLiteral("无法打开 PSD 文件"));
    QByteArray bytes; bytes.reserve(qsizetype(size));
    while (!file.atEnd()) {
        if (!control.Report(CExportControl::Phase::Preparing, bytes.size(), size, QStringLiteral("读取 PSD"))) invalid(control.Error());
        const auto chunk = file.read(256 * 1024);
        if (file.error() != QFileDevice::NoError || (chunk.isEmpty() && !file.atEnd()) || chunk.size() > limits.maximumFileBytes - bytes.size())
            invalid(QStringLiteral("PSD 文件读取失败或超出预算"));
        bytes.append(chunk);
    }
    const QFileInfo after(ownedPath);
    if (after.canonicalFilePath() != canonical || size != bytes.size() || after.size() != bytes.size() ||
        modified != after.lastModified() || changed != after.metadataChangeTime())
        invalid(QStringLiteral("PSD 文件在读取期间发生变化，请重新读取"));
    return {std::move(bytes), canonical};
}
}

struct CPsdReadDocument::Data
{
    CPsdDirectory directory;
    QVector<QMap<int, ChannelSpan>> channels;
    CPsdStreamLimits limits;
};

CPsdReadLimits CPsdReadLimits::Directory()
{
    CPsdReadLimits limits;
    limits.maximumDecodedBytes = 4LL * 1024 * 1024 * 1024;
    return limits;
}

bool CPsdReader::ReadBytes(const QByteArray& bytes, CPsdSnapshot& result, QString* error,
                           CExportControl* requested, const CPsdReadLimits& requestedLimits)
{
    const auto limits = requestedLimits;
    if (error) error->clear(); CExportControl local; auto& control = requested ? *requested : local;
    try {
        validateLimits(limits);
        if (bytes.size() > limits.maximumFileBytes) invalid(QStringLiteral("PSD 文件大小超出预算"));
        Reader reader{limits, control, {}, 0, 0};
        reader.snapshot.bytes = QByteArray(bytes.constData(), bytes.size());
        reader.check(); reader.snapshot.sha256 = QCryptographicHash::hash(reader.snapshot.bytes, QCryptographicHash::Sha256);
        reader.parse(); result = std::move(reader.snapshot); return true;
    } catch (const Failure& failure) { return failed(failure.message, error, control); }
    catch (...) { return failed(QStringLiteral("PSD 读取或内存分配失败，原快照保持不变"), error, control); }
}

bool CPsdReader::ReadFile(const QString& path, CPsdSnapshot& result, QString* error,
                          CExportControl* requested, const CPsdReadLimits& requestedLimits)
{
    const auto limits = requestedLimits;
    if (error) error->clear(); CExportControl local; auto& control = requested ? *requested : local;
    try {
        const auto file = readFile(path, control, limits);
        CPsdSnapshot candidate;
        if (!ReadBytes(file.bytes, candidate, error, &control, limits)) return false;
        candidate.canonicalPath = file.canonicalPath; result = std::move(candidate); return true;
    } catch (const Failure& failure) { return failed(failure.message, error, control); }
    catch (...) { return failed(QStringLiteral("PSD 文件读取失败，原快照保持不变"), error, control); }
}

bool CPsdReader::ReadDirectoryBytes(const QByteArray& bytes, CPsdDirectory& result, QString* error,
                                    CExportControl* requested, const CPsdReadLimits& requestedLimits)
{
    const auto limits = requestedLimits;
    if (error) error->clear();
    CExportControl local;
    auto& control = requested ? *requested : local;
    try {
        validateLimits(limits);
        if (bytes.size() > limits.maximumFileBytes) invalid(QStringLiteral("PSD 文件大小超出预算"));
        Reader reader{limits, control, {}, 0, 0};
        reader.directoryOnly = true;
        reader.snapshot.bytes = QByteArray(bytes.constData(), bytes.size());
        reader.check();
        reader.snapshot.sha256 = QCryptographicHash::hash(reader.snapshot.bytes, QCryptographicHash::Sha256);
        reader.parse();
        result = reader.takeDirectory();
        return true;
    } catch (const Failure& failure) {
        return failed(failure.message, error, control);
    } catch (...) {
        return failed(QStringLiteral("PSD 目录读取或内存分配失败，原目录保持不变"), error, control);
    }
}

const CPsdDirectory* CPsdReadDocument::Directory() const { return m_data ? &m_data->directory : nullptr; }

bool CPsdReadDocument::ReadLayer(int recordIndex, QImage& result, QString* error, CExportControl* requested) const
{
    const auto data = m_data;
    if (error) error->clear(); CExportControl local; auto& control = requested ? *requested : local;
    try {
        if (!data || recordIndex < 0 || recordIndex >= data->directory.layers.size())
            invalid(QStringLiteral("PSD 图层读取对象或编号无效"));
        Reader reader{data->limits.document, control, {}, 0, 0};
        const auto& layer = data->directory.layers[recordIndex]; const auto& channels = data->channels[recordIndex];
        reader.check(); QImage image;
        if (!layer.bounds.isEmpty() && layer.kind == CPsdLayer::Kind::Raster &&
            channels.contains(0) && channels.contains(1) && channels.contains(2)) {
            const qint64 pixels = qint64(layer.bounds.width()) * layer.bounds.height();
            if (pixels * 5 + 65536 > data->limits.maximumLayerWorkingBytes)
                invalid(QStringLiteral("PSD 单图层读取工作内存超出预算"));
            image = QImage(layer.bounds.size(), QImage::Format_ARGB32);
            if (image.isNull()) invalid(QStringLiteral("无法分配 PSD 图层像素"));
            image.fill(qRgba(0, 0, 0, 255));
            for (auto it = channels.cbegin(); it != channels.cend(); ++it) {
                const auto& span = it.value();
                if (span.offset < 0 || span.length < 2 || span.offset > data->directory.bytes.size() - span.length)
                    invalid(QStringLiteral("PSD 已验证通道目录无效"));
                const auto plane = reader.decode({data->directory.bytes.constData() + span.offset, span.length, 0},
                                                 image.width(), image.height());
                const int shift = it.key() == -1 ? 24 : (2 - it.key()) * 8;
                const quint32 mask = ~(quint32(255) << shift);
                for (int y = 0; y < image.height(); ++y) {
                    reader.check(); auto* row = reinterpret_cast<QRgb*>(image.scanLine(y));
                    const auto* source = reinterpret_cast<const uchar*>(plane.constData()) + qsizetype(y) * image.width();
                    for (int x = 0; x < image.width(); ++x) row[x] = (row[x] & mask) | (quint32(source[x]) << shift);
                }
            }
        }
        reader.check(); result = std::move(image); return true;
    } catch (const Failure& failure) { return failed(failure.message, error, control); }
    catch (...) { return failed(QStringLiteral("PSD 图层读取失败，原图像保持不变"), error, control); }
}

bool CPsdReader::OpenOwned(QByteArray bytes, QString canonicalPath, CPsdReadDocument& result, QString* error,
                           CExportControl& control, const CPsdStreamLimits& limits)
{
    if (error) error->clear();
    try {
        validateStreamLimits(limits);
        if (bytes.size() > limits.document.maximumFileBytes) invalid(QStringLiteral("PSD 文件大小超出预算"));
        auto candidate = std::make_shared<CPsdReadDocument::Data>(); candidate->limits = limits;
        Reader reader{limits.document, control, {}, 0, 0};
        reader.directoryOnly = true; reader.channelIndex = &candidate->channels;
        reader.maximumLayerWorkingBytes = limits.maximumLayerWorkingBytes;
        reader.snapshot.bytes = std::move(bytes); reader.snapshot.canonicalPath = std::move(canonicalPath);
        reader.check(); reader.snapshot.sha256 = QCryptographicHash::hash(reader.snapshot.bytes, QCryptographicHash::Sha256);
        reader.parse(); candidate->directory = reader.takeDirectory();
        result.m_data = std::move(candidate); return true;
    } catch (const Failure& failure) { return failed(failure.message, error, control); }
    catch (...) { return failed(QStringLiteral("PSD 逐层读取准备失败，原读取对象保持不变"), error, control); }
}

bool CPsdReader::OpenBytes(const QByteArray& bytes, CPsdReadDocument& result, QString* error,
                           CExportControl* requested, const CPsdStreamLimits& requestedLimits)
{
    const auto limits = requestedLimits;
    if (error) error->clear(); CExportControl local; auto& control = requested ? *requested : local;
    try {
        validateStreamLimits(limits);
        if (bytes.size() > limits.document.maximumFileBytes) invalid(QStringLiteral("PSD 文件大小超出预算"));
        return OpenOwned(QByteArray(bytes.constData(), bytes.size()), {}, result, error, control, limits);
    } catch (const Failure& failure) { return failed(failure.message, error, control); }
    catch (...) { return failed(QStringLiteral("PSD 读取输入复制失败，原读取对象保持不变"), error, control); }
}

bool CPsdReader::OpenFile(const QString& path, CPsdReadDocument& result, QString* error,
                          CExportControl* requested, const CPsdStreamLimits& requestedLimits)
{
    const auto limits = requestedLimits;
    if (error) error->clear(); CExportControl local; auto& control = requested ? *requested : local;
    try {
        validateStreamLimits(limits);
        auto file = readFile(path, control, limits.document);
        return OpenOwned(std::move(file.bytes), std::move(file.canonicalPath), result, error, control, limits);
    } catch (const Failure& failure) { return failed(failure.message, error, control); }
    catch (...) { return failed(QStringLiteral("PSD 文件读取失败，原读取对象保持不变"), error, control); }
}
