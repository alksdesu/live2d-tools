#ifndef BLEND_SHAPE_V5_FIXTURE_H
#define BLEND_SHAPE_V5_FIXTURE_H

#include <QByteArray>
#include <QVector>
#include <QtEndian>

namespace BlendShapeV5Fixture {
inline QByteArray Extend(QByteArray bytes, int binding = 0)
{
    const auto word = [&](int offset) { return qFromLittleEndian<qint32>(bytes.constData() + offset); };
    const auto put = [&](int offset, int value) { qToLittleEndian(qint32(value), bytes.data() + offset); };
    if (bytes.size() < 0x840 || bytes[4] != 3) return {};
    const int counts = word(0x40), canvas = word(0x44);
    bytes.insert(canvas, QByteArray(128, '\0'));
    for (int slot = 0x40; slot < counts; slot += 4)
        if (word(slot) >= canvas) put(slot, word(slot) + 128);
    bytes[4] = 5;
    const auto count = [&](int index, int value) { put(counts + index * 4, value); };
    count(0, 1);
    count(6, 3);
    count(8, 3);
    count(32, 1);
    count(33, 1);
    bytes.replace(word(0x4c), 64, QByteArray("Shared").leftJustified(64, '\0'));
    bytes.replace(word(0x6c) + 64, 64, QByteArray("RotationTarget").leftJustified(64, '\0'));
    const auto array = [&](int slot, const QVector<int>& values) {
        const int begin = (bytes.size() + 63) & ~63;
        bytes.append(QByteArray(begin + values.size() * 4 - bytes.size(), '\0'));
        for (int row = 0; row < values.size(); ++row) put(begin + row * 4, values[row]);
        put(slot, begin);
    };
    array(0x27c, {0});
    array(0x280, {binding});
    array(0x284, {1});
    array(0x288, {0});
    array(0x28c, {binding});
    array(0x290, {1});
    return bytes;
}
}

#endif
