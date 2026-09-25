#ifndef MOC_COLUMN_ORACLE_H
#define MOC_COLUMN_ORACLE_H

#include <QByteArray>
#include <QSet>
#include <QString>
#include <QtEndian>

namespace MocColumnOracle {
inline int Word(const QByteArray& bytes, int offset)
{
    return qFromLittleEndian<qint32>(bytes.constData() + offset);
}

inline void Pad(QByteArray& bytes)
{
    bytes.append(QByteArray((64 - bytes.size() % 64) % 64, '\0'));
}

inline QString Preserved(const QByteArray& before, const QByteArray& after,
                         const QSet<int>& changedColumns, const QSet<int>& changedCounts = {})
{
    if (before.size() < 0x264 || after.size() < 0x264 || before.left(0x40) != after.left(0x40))
        return QStringLiteral("File header changed");
    const auto valid = [](const QByteArray& bytes, qint64 offset, qint64 length) {
        return offset >= 0 && length >= 0 && offset <= bytes.size() && length <= bytes.size() - offset;
    };
    const int beforeCounts = Word(before, 0x40), afterCounts = Word(after, 0x40);
    if (!valid(before, beforeCounts, 128) || !valid(after, afterCounts, 128))
        return QStringLiteral("Count table is truncated");
    for (int index = 0; index < 32; ++index)
        if (!changedCounts.contains(index) && Word(before, beforeCounts + index * 4) != Word(after, afterCounts + index * 4))
            return QStringLiteral("Unedited count %1 changed").arg(index);
    const int beforeCanvas = Word(before, 0x44), afterCanvas = Word(after, 0x44);
    if (!valid(before, beforeCanvas, 20) || !valid(after, afterCanvas, 20) ||
        before.mid(beforeCanvas, 20) != after.mid(afterCanvas, 20)) return QStringLiteral("Canvas changed");
    struct Group { int first; int last; int count; };
    constexpr Group groups[]{
        {0x48, 0x64, 0}, {0x68, 0x88, 1}, {0x8c, 0xa0, 2}, {0xa4, 0xb0, 3},
        {0xb4, 0x100, 4}, {0x104, 0x124, 5}, {0x128, 0x128, 6}, {0x12c, 0x130, 7},
        {0x134, 0x14c, 8}, {0x150, 0x158, 9}, {0x15c, 0x15c, 10}, {0x160, 0x160, 11},
        {0x164, 0x168, 12}, {0x16c, 0x170, 13}, {0x174, 0x174, 14}, {0x178, 0x178, 15},
        {0x17c, 0x17c, 16}, {0x180, 0x180, 17}, {0x184, 0x194, 18}, {0x198, 0x1a0, 19},
        {0x1a4, 0x1c4, 20}, {0x1c8, 0x1cc, 21}, {0x1d0, 0x1d0, 22}, {0x1d4, 0x1d4, 2},
        {0x1d8, 0x1e0, 5}, {0x1e4, 0x1e4, 2}, {0x1e8, 0x1e8, 3}, {0x1ec, 0x1ec, 4},
        {0x1f0, 0x1f8, 23}, {0x1fc, 0x204, 24}, {0x208, 0x210, 5}, {0x214, 0x21c, 25},
        {0x220, 0x230, 26}, {0x234, 0x23c, 27}, {0x240, 0x248, 28}, {0x24c, 0x24c, 29},
        {0x250, 0x258, 30}, {0x25c, 0x260, 31}
    };
    for (const auto& group : groups) for (int slot = group.first; slot <= group.last; slot += 4) {
        if (changedColumns.contains(slot)) continue;
        const int oldOffset = Word(before, slot), newOffset = Word(after, slot);
        if (!oldOffset) {
            if (newOffset) return QStringLiteral("Absent column 0x%1 was materialized").arg(slot, 0, 16);
            continue;
        }
        int width = 4;
        if (slot == 0x4c || slot == 0x6c || slot == 0xc4 || slot == 0x108 || slot == 0x1a8) width = 64;
        else if (slot == 0xe8) width = 1;
        else if (slot == 0x17c || slot == 0x1cc) width = 2;
        else if (slot == 0x104 || slot == 0x1d8) width = 8;
        const qint64 length = qint64(Word(before, beforeCounts + group.count * 4)) * width;
        if (!valid(before, oldOffset, length) || !valid(after, newOffset, length))
            return QStringLiteral("Column 0x%1 was truncated").arg(slot, 0, 16);
        if (before.mid(oldOffset, length) != after.mid(newOffset, length))
            return QStringLiteral("Unedited column 0x%1 changed").arg(slot, 0, 16);
    }
    return {};
}
}

#endif
