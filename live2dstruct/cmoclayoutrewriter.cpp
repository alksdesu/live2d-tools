#include "cmoclayoutrewriter.h"
#include "csectiontable.h"
#include "csectionvalidation.h"

#include <QSet>
#include <QtEndian>
#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace live2dstruct {
namespace {
constexpr int MaximumHeaderEntries = 1000000;

bool fail(QString* error, const QString& message)
{
    if (error) *error = message;
    return false;
}

qint64 word(const QByteArray& bytes, int offset)
{
    return qFromLittleEndian<quint32>(bytes.constData() + offset);
}

qint64 aligned(qint64 size) { return (size + 63) & ~qint64(63); }

bool range(qint64 offset, qint64 size, qsizetype available)
{
    return offset >= 0 && size >= 0 && offset <= available && size <= available - offset;
}

QVector<CSectionValidation::ColumnSpan> spans(const CSectionTable& table)
{
    auto result = CSectionValidation::ArraySpans(*table.sectionOffsetTable, *table.count_info);
    for (auto& span : result) {
        span.width = CSectionValidation::StorageWidth(span);
    }
    return result;
}

struct Column
{
    int slot;
    qint64 sourceOffset;
    qint64 candidateOffset;
};

struct Copy
{
    int slot;
    qint64 input;
    qint64 size;
    qint64 output;
};

struct Insertion
{
    qint64 sourceOffset;
    qint64 outputOffset;
    qint64 size;
    QVector<Copy> copies;
};
}

bool CMocLayoutRewriter::Rewrite(const QByteArray& source, const QByteArray& candidate,
                                const QMap<int, qint64>& relocatedColumnByteSizes,
                                QByteArray& result, QString* error)
{
    if (error) error->clear();
    try {
        if (candidate.size() < source.size() || source.size() > std::numeric_limits<int>::max() ||
            candidate.size() > std::numeric_limits<int>::max() || source.size() < CSectionValidation::OffsetTableEnd ||
            source.size() % 64 || candidate.size() % 64 || source.left(0x40) != candidate.left(0x40))
            return fail(error, QStringLiteral("MOC 重排需要保留文件头、完整源数据和 64 字节文件对齐"));
        CReader sourceReader = CReader::FromBytes(source);
        CReader candidateReader = CReader::FromBytes(candidate);
        if (!sourceReader.IsMoc3() || source[5] != 0 || !candidateReader.IsMoc3())
            return fail(error, QStringLiteral("MOC 重排需要受支持的小端文件"));
        const qint64 headerEnd = word(source, 0x40);
        if (headerEnd < CSectionValidation::OffsetTableEnd || headerEnd % 4 ||
            (headerEnd - 0x40) / 4 > MaximumHeaderEntries ||
            !range(0x40, headerEnd - 0x40, source.size()) ||
            word(candidate, 0x40) != headerEnd || word(candidate, 0x44) != word(source, 0x44))
            return fail(error, QStringLiteral("MOC 重排文件头长度或元数据偏移无效"));
        CSectionTable sourceTable(sourceReader), candidateTable(candidateReader);
        if (!sourceTable.IsValid() || !candidateTable.IsValid())
            return fail(error, QStringLiteral("MOC 重排区段无效：%1")
                        .arg(sourceTable.IsValid() ? candidateReader.Error() : sourceReader.Error()));
        const int countInfoSize = CSectionValidation::CountInfoSize(sourceTable.sectionOffsetTable->mocVersion);
        const int knownOffsetEnd = CSectionValidation::KnownOffsetEnd(sourceTable.sectionOffsetTable->mocVersion);
        const qint64 metadataEnd = std::max(headerEnd + countInfoSize, word(source, 0x44) + 20);
        QVector<Column> columns;
        QMap<qint64, QVector<int>> atOffset;
        QMap<qint64, qint64> extents;
        for (int slot = 0x40; slot < headerEnd; slot += 4) {
            const qint64 original = word(source, slot), edited = word(candidate, slot);
            if ((original && (original < headerEnd || original > source.size())) ||
                (edited && (edited < headerEnd || edited > candidate.size())))
                return fail(error, QStringLiteral("MOC 文件头字段 0x%1 不是有效区段偏移").arg(slot, 0, 16));
            if (!relocatedColumnByteSizes.contains(slot) && original != edited)
                return fail(error, QStringLiteral("MOC 区段 0x%1 移动后缺少明确长度").arg(slot, 0, 16));
            columns.append({slot, original, edited});
            if (original) atOffset[original].append(slot);
        }
        for (auto it = atOffset.cbegin(); it != atOffset.cend(); ++it) {
            auto next = it;
            ++next;
            extents.insert(it.key(), (next == atOffset.cend() ? source.size() : next.key()) - it.key());
        }
        const qint64 countTail = extents.value(headerEnd) - countInfoSize;
        if (countTail > 0 && source.mid(headerEnd + countInfoSize, countTail) != candidate.mid(headerEnd + countInfoSize, countTail))
            return fail(error, QStringLiteral("MOC 重排尚不支持修改未识别的区段数量"));
        const auto sourceSpans = spans(sourceTable), candidateSpans = spans(candidateTable);
        if (sourceSpans.size() != CSectionValidation::KnownOffsetCount(sourceTable.sectionOffsetTable->mocVersion) - 2 ||
            sourceSpans.size() != candidateSpans.size())
            return fail(error, QStringLiteral("MOC 重排缺少已知区段信息"));
        QSet<int> knownSlots, emptyOpaqueMarkers;
        for (const auto& span : sourceSpans) knownSlots.insert(span.slot);
        for (const auto& span : sourceSpans)
            if (!span.count && span.offset == source.size()) emptyOpaqueMarkers.insert(span.slot);
        for (const auto& column : columns)
            if (column.slot >= 0x48 && !knownSlots.contains(column.slot) &&
                column.sourceOffset == source.size() && !relocatedColumnByteSizes.contains(column.slot))
                emptyOpaqueMarkers.insert(column.slot);
        QMap<int, qint64> required;
        for (qsizetype index = 0; index < sourceSpans.size(); ++index) {
            const auto& before = sourceSpans[index];
            const auto& after = candidateSpans[index];
            for (const auto* span : {&before, &after}) {
                if (span->optional && !span->offset) continue;
                const auto available = span == &before ? source.size() : candidate.size();
                if (!range(span->offset, span->count * span->width, available))
                    return fail(error, QStringLiteral("MOC 区段 %1 的完整存储空间不足").arg(QString::fromLatin1(span->name)));
            }
            if (before.offset && before.count)
                extents[before.offset] = std::max(extents.value(before.offset), before.count * before.width);
            if (after.optional && !after.offset) continue;
            if (before.slot != after.slot || after.slot < 0x48 || after.slot >= knownOffsetEnd ||
                word(source, before.slot) != before.offset || word(candidate, after.slot) != after.offset)
                return fail(error, QStringLiteral("MOC 区段定义与文件头不一致"));
            required.insert(after.slot, after.count * after.width);
            if (after.count && before.count != after.count) {
                if (!relocatedColumnByteSizes.contains(after.slot))
                    return fail(error, QStringLiteral("MOC 数量变化后未重建区段：%1").arg(QString::fromLatin1(after.name)));
            }
        }
        QMap<qint64, QSet<int>> events;
        QSet<int> cleared;
        for (auto it = relocatedColumnByteSizes.cbegin(); it != relocatedColumnByteSizes.cend(); ++it) {
            const int slot = it.key();
            if (slot < 0x48 || slot >= headerEnd || slot % 4 || it.value() < 0)
                return fail(error, QStringLiteral("MOC 重排区段编号或长度无效"));
            const auto& column = columns[(slot - 0x40) / 4];
            if (!range(column.candidateOffset, it.value(), candidate.size()) ||
                it.value() < required.value(slot))
                return fail(error, QStringLiteral("MOC 区段 0x%1 的候选长度不足").arg(slot, 0, 16));
            if (!column.candidateOffset && it.value())
                return fail(error, QStringLiteral("MOC 非空候选区段没有偏移"));
            if (!column.candidateOffset) {
                cleared.insert(slot);
                continue;
            }
            qint64 anchor = column.sourceOffset;
            if (!anchor) {
                anchor = source.size();
                for (const auto& next : columns)
                    if (next.slot > slot && next.sourceOffset) { anchor = next.sourceOffset; break; }
            }
            if (anchor < metadataEnd)
                return fail(error, QStringLiteral("MOC 重排区段与元数据重叠"));
            events[anchor].insert(slot);
        }
        if (events.isEmpty()) {
            result = candidate;
            return true;
        }
        bool expanded = true;
        while (expanded) {
            expanded = false;
            for (const auto& span : sourceSpans) {
                if (!span.offset || !span.count || cleared.contains(span.slot)) continue;
                const auto inside = events.upperBound(span.offset);
                if (inside == events.cend() || inside.key() >= span.offset + span.count * span.width ||
                    events.contains(span.offset)) continue;
                if (span.offset < metadataEnd || !atOffset.contains(span.offset))
                    return fail(error, QStringLiteral("MOC 共享区段无法安全隔离"));
                events.insert(span.offset, {});
                expanded = true;
            }
        }
        QVector<Insertion> insertions;
        QMap<int, qint64> destinations;
        qint64 extra = 0;
        for (auto it = events.cbegin(); it != events.cend(); ++it) {
            QSet<int> selected = it.value();
            for (int slot : atOffset.value(it.key()))
                if (!cleared.contains(slot)) selected.insert(slot);
            auto ordered = selected.values();
            std::sort(ordered.begin(), ordered.end());
            Insertion insertion{it.key(), it.key() + extra, 0, {}};
            qint64 cursor = insertion.outputOffset;
            for (int slot : ordered) {
                const auto& column = columns[(slot - 0x40) / 4];
                const qint64 size = relocatedColumnByteSizes.contains(slot)
                    ? relocatedColumnByteSizes.value(slot) : extents.value(column.sourceOffset);
                const qint64 input = relocatedColumnByteSizes.contains(slot) ? column.candidateOffset : column.sourceOffset;
                if (!range(input, size, candidate.size()))
                    return fail(error, QStringLiteral("MOC 共享区段的完整字节范围越界"));
                cursor = aligned(cursor);
                insertion.copies.append({slot, input, size, cursor});
                destinations.insert(slot, cursor);
                cursor += size;
            }
            insertion.size = aligned(cursor - insertion.outputOffset);
            extra += insertion.size;
            if (extra > std::numeric_limits<int>::max() - candidate.size())
                return fail(error, QStringLiteral("MOC 重排文件长度超出范围"));
            insertions.append(std::move(insertion));
        }
        QByteArray output(candidate.size() + extra, '\0');
        qint64 sourceCursor = 0, outputCursor = 0;
        for (const auto& insertion : insertions) {
            const qint64 length = insertion.sourceOffset - sourceCursor;
            std::memcpy(output.data() + outputCursor, candidate.constData() + sourceCursor, length);
            outputCursor += length;
            for (const auto& copy : insertion.copies)
                if (copy.size) std::memcpy(output.data() + copy.output, candidate.constData() + copy.input, copy.size);
            outputCursor += insertion.size;
            sourceCursor = insertion.sourceOffset;
        }
        std::memcpy(output.data() + outputCursor, candidate.constData() + sourceCursor, candidate.size() - sourceCursor);
        for (const auto& column : columns) {
            if (cleared.contains(column.slot)) {
                qToLittleEndian(quint32(0), output.data() + column.slot);
                continue;
            }
            if (emptyOpaqueMarkers.contains(column.slot)) {
                qToLittleEndian(quint32(output.size()), output.data() + column.slot);
                continue;
            }
            if (!column.sourceOffset && !destinations.contains(column.slot)) continue;
            qint64 destination = destinations.value(column.slot, -1);
            if (destination < 0) {
                destination = column.sourceOffset;
                for (const auto& insertion : insertions)
                    if (insertion.sourceOffset < column.sourceOffset) destination += insertion.size;
            }
            qToLittleEndian(quint32(destination), output.data() + column.slot);
        }
        CReader checkReader = CReader::FromBytes(output);
        CSectionTable checkTable(checkReader);
        if (!checkTable.IsValid()) return fail(error, QStringLiteral("MOC 重排结果无效：%1").arg(checkReader.Error()));
        result = std::move(output);
        return true;
    } catch (const std::bad_alloc&) {
        return fail(error, QStringLiteral("MOC 重排内存不足"));
    } catch (const std::length_error&) {
        return fail(error, QStringLiteral("MOC 重排长度超出范围"));
    }
}
}
