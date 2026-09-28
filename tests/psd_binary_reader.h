#pragma once

#include <QByteArray>
#include <QMap>
#include <QRect>
#include <QString>
#include <QVector>
#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>

namespace PsdBinary {

inline void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

class Reader
{
public:
    explicit Reader(QByteArray bytes) : m_bytes(std::move(bytes)) {}
    qsizetype Remaining() const { return m_bytes.size() - m_position; }
    QByteArray Take(quint64 count)
    {
        Require(count <= static_cast<quint64>(Remaining()), "truncated field");
        const QByteArray result = m_bytes.mid(m_position, static_cast<qsizetype>(count));
        m_position += static_cast<qsizetype>(count);
        return result;
    }
    quint8 U8() { return static_cast<quint8>(Take(1)[0]); }
    quint16 U16()
    {
        const quint16 high = U8();
        return (high << 8) | U8();
    }
    quint32 U32()
    {
        const quint32 high = U16();
        return (high << 16) | U16();
    }
    qint16 I16() { return static_cast<qint16>(U16()); }
    qint32 I32() { return static_cast<qint32>(U32()); }
    Reader Section() { return Reader(Take(U32())); }
    void Signature(const QByteArray& signature)
    {
        Require(Take(signature.size()) == signature, "invalid signature");
    }
    void ZeroPadding()
    {
        const QByteArray padding = Take(Remaining());
        Require(padding == QByteArray(padding.size(), '\0'), "nonzero padding");
    }
private:
    QByteArray m_bytes;
    qsizetype m_position = 0;
};

inline QByteArray DecodePackBits(const QByteArray& bytes, qsizetype width)
{
    Require(width >= 0 && width <= 30000, "invalid row width");
    Reader input(bytes);
    QByteArray output;
    while (input.Remaining()) {
        const qint8 code = static_cast<qint8>(input.U8());
        if (code == -128) continue;
        const qsizetype count = code >= 0 ? code + 1 : 1 - code;
        Require(count <= width - output.size(), "RLE row overflow");
        if (code >= 0) output += input.Take(count);
        else output += QByteArray(count, static_cast<char>(input.U8()));
    }
    Require(output.size() == width, "RLE row underflow");
    return output;
}

struct Channel
{
    qint16 id = 0;
    quint32 length = 0;
    quint16 compression = 0;
    QByteArray pixels;
};

struct Layer
{
    QRect bounds;
    QString name;
    QByteArray pascalName;
    QByteArray blend;
    quint8 opacity = 0;
    quint8 clipping = 0;
    quint32 sectionType = 0;
    std::optional<quint32> layerId;
    QVector<Channel> channels;
    QByteArray Plane(qint16 id) const
    {
        for (const Channel& channel : channels)
            if (channel.id == id) return channel.pixels;
        return {};
    }
};

struct Document
{
    QSize canvas;
    quint16 channels = 0;
    QMap<quint16, QByteArray> resources;
    QVector<Layer> layers;
};

inline Layer ReadLayer(Reader& input)
{
    Layer layer;
    const qint32 top = input.I32(), left = input.I32(), bottom = input.I32(), right = input.I32();
    const qint64 width = static_cast<qint64>(right) - left;
    const qint64 height = static_cast<qint64>(bottom) - top;
    Require(width >= 0 && height >= 0 && width <= 30000 && height <= 30000,
            "invalid layer dimensions");
    Require(right > std::numeric_limits<qint32>::min() && bottom > std::numeric_limits<qint32>::min(),
            "unrepresentable layer endpoint");
    Require(width * height <= 100000000, "layer pixel limit exceeded");
    layer.bounds = QRect(left, top, static_cast<int>(width), static_cast<int>(height));
    const quint16 count = input.U16();
    Require(count <= 56, "invalid channel count");
    for (quint16 i = 0; i < count; ++i) {
        Channel channel;
        channel.id = input.I16();
        channel.length = input.U32();
        Require(channel.length >= 2, "channel missing compression");
        for (const Channel& previous : layer.channels)
            Require(previous.id != channel.id, "duplicate channel ID");
        layer.channels.append(channel);
    }
    input.Signature("8BIM");
    layer.blend = input.Take(4);
    layer.opacity = input.U8();
    layer.clipping = input.U8();
    input.Take(2);
    Reader extra = input.Section();
    extra.Section();
    extra.Section();
    const quint8 nameLength = extra.U8();
    layer.pascalName = extra.Take(nameLength);
    extra.Take((4 - (nameLength + 1) % 4) % 4);
    while (extra.Remaining()) {
        extra.Signature("8BIM");
        const QByteArray key = extra.Take(4);
        const quint32 size = extra.U32();
        Reader block(extra.Take(size));
        extra.Take(size % 2);
        if (key == "luni") {
            const quint32 units = block.U32();
            Require(units <= static_cast<quint64>(block.Remaining()) / 2, "truncated Unicode name");
            for (quint32 i = 0; i < units; ++i) layer.name += QChar(block.U16());
            block.ZeroPadding();
        } else if (key == "lyid") {
            Require(size == 4 && !layer.layerId, "invalid or repeated layer ID");
            layer.layerId = block.U32();
        } else if (key == "lsct") {
            layer.sectionType = block.U32();
            Require(layer.sectionType <= 3, "invalid section type");
        }
    }
    return layer;
}

inline void ReadPixels(Reader& input, Layer& layer)
{
    const qsizetype width = layer.bounds.width(), height = layer.bounds.height();
    for (Channel& channel : layer.channels) {
        Reader data(input.Take(channel.length));
        channel.compression = data.U16();
        if (channel.compression == 0) {
            channel.pixels = data.Take(static_cast<quint64>(width) * height);
        } else if (channel.compression == 1) {
            QVector<quint16> sizes;
            for (qsizetype row = 0; row < height; ++row) sizes.append(data.U16());
            for (quint16 size : sizes) channel.pixels += DecodePackBits(data.Take(size), width);
        } else Require(false, "unsupported channel compression");
        Require(data.Remaining() == 0, "extra channel bytes");
    }
}

inline bool Parse(const QByteArray& bytes, Document& document, QString* error = nullptr)
{
    try {
        Reader input(bytes);
        Document candidate;
        input.Signature("8BPS");
        Require(input.U16() == 1, "unsupported PSD version");
        Require(input.Take(6) == QByteArray(6, '\0'), "invalid reserved header bytes");
        candidate.channels = input.U16();
        Require(candidate.channels >= 1 && candidate.channels <= 56, "invalid image channels");
        const quint32 height = input.U32(), width = input.U32();
        Require(width && height && width <= 30000 && height <= 30000, "invalid canvas");
        Require(static_cast<quint64>(width) * height <= 100000000, "canvas pixel limit exceeded");
        candidate.canvas = QSize(width, height);
        Require(input.U16() == 8 && input.U16() == 3, "unsupported depth or color mode");
        Require(input.Section().Remaining() == 0, "unexpected RGB color mode data");
        Reader resources = input.Section();
        while (resources.Remaining()) {
            resources.Signature("8BIM");
            const quint16 id = resources.U16();
            const quint8 nameLength = resources.U8();
            resources.Take(nameLength);
            resources.Take((nameLength + 1) % 2);
            const quint32 length = resources.U32();
            Require(!candidate.resources.contains(id), "duplicate resource ID");
            candidate.resources.insert(id, resources.Take(length));
            resources.Take(length % 2);
        }
        Reader mask = input.Section();
        if (mask.Remaining()) {
            Reader info = mask.Section();
            if (info.Remaining()) {
                const int count = qAbs(static_cast<int>(info.I16()));
                Require(count <= info.Remaining() / 34, "invalid layer count");
                for (int i = 0; i < count; ++i) candidate.layers.append(ReadLayer(info));
                for (Layer& layer : candidate.layers) ReadPixels(info, layer);
                Require(info.Remaining() <= 3, "extra layer info bytes");
                info.ZeroPadding();
            }
            mask.Section();
            Require(mask.Remaining() == 0, "unsupported global layer information");
        }
        Require(input.U16() == 0, "unsupported merged compression");
        input.Take(static_cast<quint64>(width) * height * candidate.channels);
        Require(input.Remaining() == 0, "extra image bytes");
        document = std::move(candidate);
        if (error) error->clear();
        return true;
    } catch (const std::runtime_error& failure) {
        if (error) *error = QString::fromLatin1(failure.what());
        return false;
    }
}

}
