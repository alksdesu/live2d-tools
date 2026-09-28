#ifndef PSD_ORIGINAL_FIXTURE_H
#define PSD_ORIGINAL_FIXTURE_H

#include <QByteArray>
#include <QString>
#include <QtEndian>

namespace psd_original_fixture {
inline QByteArray u16(quint16 value) { char data[2]; qToBigEndian(value, data); return QByteArray(data, 2); }
inline QByteArray u32(quint32 value) { char data[4]; qToBigEndian(value, data); return QByteArray(data, 4); }
inline QByteArray block(const QByteArray& bytes) { return u32(bytes.size()) + bytes; }
inline QByteArray tag(const QByteArray& key, const QByteArray& bytes) { return QByteArray("8BIM") + key + block(bytes); }
inline QByteArray unicode(const QString& name)
{
    QByteArray bytes = u32(name.size());
    for (QChar character : name) bytes += u16(character.unicode());
    return bytes;
}
inline QByteArray channel(const QByteArray& bytes, int width, int compression)
{
    QByteArray result = u16(compression);
    if (compression == 0) return result + bytes;
    if (compression == 1) {
        for (int y = 0; y < bytes.size() / width; ++y) result += u16(width + 1);
        for (int y = 0; y < bytes.size() / width; ++y) result += QByteArray(1, char(width - 1)) + bytes.mid(y * width, width);
        return result;
    }
    QByteArray predicted = bytes;
    if (compression == 3) for (int i = bytes.size() - 1; i >= 0; --i)
        if (i % width) predicted[i] = char(quint8(bytes[i]) - quint8(bytes[i - 1]));
    return result + qCompress(predicted).mid(4);
}
inline QByteArray repeatedChannel(int width, int height, int planes, uchar value)
{
    QByteArray row;
    for (int remaining = width; remaining > 0;) {
        const int count = qMin(128, remaining);
        row += char(1 - count);
        row += char(value);
        remaining -= count;
    }
    const int rows = height * planes;
    return u16(1) + u16(row.size()).repeated(rows) + row.repeated(rows);
}
inline QByteArray largeSolid(int width, int height, int compression = 1)
{
    const auto layer = compression == 1 ? repeatedChannel(width, height, 1, 91)
        : channel(QByteArray(width * height, char(91)), width, compression);
    QByteArray record = u32(0) + u32(0) + u32(height) + u32(width) + u16(4);
    for (int id : {0, 1, 2, -1}) record += u16(quint16(id)) + u32(layer.size());
    record += QByteArray("8BIMnorm") + QByteArray::fromHex("ff000000");
    record += block(u32(0) + u32(0) + QByteArray::fromHex("03626967"));
    QByteArray info = u16(quint16(-1)) + record + layer.repeated(4);
    if (info.size() % 2) info += char(0);
    return QByteArray("8BPS") + u16(1) + QByteArray(6, 0) + u16(4) + u32(height) + u32(width) + u16(8) + u16(3) +
        u32(0) + u32(0) + block(block(info) + u32(0)) + (compression == 1 ? repeatedChannel(width, height, 4, 73)
            : channel(QByteArray(width * height * 4, char(73)), width, compression));
}
inline QByteArray maskFixture(const QByteArray& mask, int realWidth = 2, int realHeight = 1, bool includeReal = true)
{
    QByteArray record = u32(0) + u32(0) + u32(2) + u32(2) + u16(includeReal ? 6 : 5);
    QByteArray channels;
    for (int id : {0, 1, 2, -1, -2, -3}) {
        if (id == -3 && !includeReal) continue;
        const auto count = id == -3 ? realWidth * realHeight : 4;
        const auto data = u16(0) + QByteArray(count, char(127));
        record += u16(quint16(id)) + u32(data.size());
        channels += data;
    }
    record += QByteArray("8BIMnorm") + QByteArray::fromHex("ff000000");
    record += block(block(mask) + u32(0) + QByteArray::fromHex("046d61736b000000") + tag("luni", unicode("mask")));
    QByteArray info = u16(quint16(-1)) + record + channels;
    if (info.size() % 2) info += char(0);
    return QByteArray("8BPS") + u16(1) + QByteArray(6, 0) + u16(4) + u32(2) + u32(2) + u16(8) + u16(3) +
        u32(0) + u32(0) + block(block(info) + u32(0)) + u16(0) + QByteArray(16, char(127));
}
inline QByteArray fixture(int compression = 0, bool effects = false, bool preview = true, const QString& name = QStringLiteral("原始图层😀"), bool grouped = false)
{
    const QByteArray rgba[] = {QByteArray::fromHex("01c840e0"), QByteArray::fromHex("11dd9080"),
                              QByteArray::fromHex("ffc010a0"), QByteArray::fromHex("0080ff30")};
    QByteArray record = u32(quint32(-2)) + u32(quint32(-1)) + u32(0) + u32(1) + u16(effects ? 5 : 4);
    QByteArray channels;
    for (int index = 0; index < (effects ? 5 : 4); ++index) {
        const auto bytes = channel(index < 4 ? rgba[index] : QByteArray::fromHex("ff0080ff"), 2, compression);
        record += u16(index < 3 ? index : index == 3 ? quint16(-1) : quint16(-2)) + u32(bytes.size());
        channels += bytes;
    }
    record += QByteArray("8BIMnorm") + QByteArray::fromHex("7f000200");
    const auto mask = effects ? u32(0) + u32(0) + u32(2) + u32(2) + QByteArray::fromHex("ff000000") : QByteArray();
    QByteArray extra = block(mask) + u32(0) + QByteArray::fromHex("036f6c64");
    extra += tag("luni", unicode(name)) + tag("lyid", u32(0xf1234567));
    if (effects) extra += tag("lrFX", QByteArray::fromHex("000000000000")) + tag("zzzz", "opaque-effects!");
    record += block(extra);
    QByteArray info;
    if (grouped) {
        const auto group = [](int kind, const QString& label) {
            const auto mode = kind == 1 ? QByteArray("pass") : QByteArray("norm");
            const auto extra = u32(0) + u32(0) + QByteArray(4, 0) + tag("luni", unicode(label)) +
                tag("lsct", u32(kind) + "8BIM" + mode);
            return QByteArray(16, 0) + u16(0) + "8BIM" + mode + QByteArray::fromHex("ff000000") + block(extra);
        };
        info = u16(quint16(-3)) + group(3, "divider") + record + group(1, QStringLiteral("源文件夹")) + channels;
    } else info = u16(quint16(-1)) + record + channels;
    if (info.size() % 2) info += char(0);
    const QByteArray icc("source-icc\0data", 15);
    QByteArray resources = QByteArray("8BIM") + u16(1039) + u16(0) + block(icc);
    if (icc.size() % 2) resources += char(0);
    const auto version = u32(1) + QByteArray(1, preview ? char(1) : char(0)) + u32(0) + u32(0) + u32(1);
    resources += QByteArray("8BIM") + u16(1057) + u16(0) + block(version) + QByteArray(1, 0);
    QByteArray merged;
    for (const auto& bytes : rgba) merged += bytes;
    return QByteArray("8BPS") + u16(1) + QByteArray(6, 0) + u16(4) + u32(2) + u32(2) + u16(8) + u16(3) +
        u32(0) + block(resources) + block(block(info) + u32(0)) + channel(merged, 2, compression);
}
}
#endif
