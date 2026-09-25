#include "live2dstruct/cmoclayoutrewriter.h"
#include "live2dstruct/csectiontable.h"
#include "live2dstruct/csectionvalidation.h"
#include <QtTest>
#include <QFile>
#include <QtEndian>
#include <cstring>
#include <limits>

using namespace live2dstruct;

namespace {
int word(const QByteArray& bytes, int offset) { return qFromLittleEndian<qint32>(bytes.constData() + offset); }
void put(QByteArray& bytes, int offset, int value) { qToLittleEndian(qint32(value), bytes.data() + offset); }
int count(const QByteArray& bytes, int index) { return word(bytes, word(bytes, 0x40) + index * 4); }

QByteArray source(const QString& name)
{
    QFile file(QString(TEST_RESOURCES) + '/' + name + '/' + name + ".moc3");
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

void models()
{
    QTest::addColumn<QString>("name");
    for (const char* name : {"Mark", "Rice", "Natori", "Wanko"}) QTest::newRow(name) << QString::fromLatin1(name);
}

void relocate(QByteArray& bytes, int slot, const QByteArray& values, QMap<int, qint64>& lengths)
{
    const int start = (bytes.size() + 63) & ~63;
    bytes.resize(start);
    bytes.append(values);
    bytes.append(QByteArray((-bytes.size()) & 63, '\0'));
    put(bytes, slot, start);
    lengths.insert(slot, values.size());
}

bool sameColumns(const QByteArray& candidate, const QByteArray& output, QString& error)
{
    auto beforeReader = CReader::FromBytes(candidate), afterReader = CReader::FromBytes(output);
    CSectionTable before(beforeReader), after(afterReader);
    if (!before.IsValid() || !after.IsValid()) { error = beforeReader.Error() + afterReader.Error(); return false; }
    const auto a = CSectionValidation::ArraySpans(*before.sectionOffsetTable, *before.count_info);
    const auto b = CSectionValidation::ArraySpans(*after.sectionOffsetTable, *after.count_info);
    if (a.size() != b.size()) return false;
    for (qsizetype index = 0; index < a.size(); ++index) {
        const auto& first = a[index];
        const auto& second = b[index];
        if (first.count != second.count || first.width != second.width) { error = first.name; return false; }
        if (first.optional && !first.offset) {
            if (second.offset) { error = first.name; return false; }
            continue;
        }
        const qint64 size = first.count * first.width;
        if (candidate.mid(first.offset, size) != output.mid(second.offset, size)) { error = first.name; return false; }
    }
    for (int slot : {0x104, 0x1d8}) {
        const int first = word(candidate, slot), second = word(output, slot);
        if (first && candidate.mid(first, count(candidate, 5) * 8) != output.mid(second, count(output, 5) * 8)) {
            error = QStringLiteral("runtime 0x%1").arg(slot, 0, 16);
            return false;
        }
    }
    return true;
}
}

class MocLayoutRewriterTests : public QObject
{
    Q_OBJECT
private slots:
    void knownSchemaMatchesEveryDistinctHeaderSlot()
    {
        auto bytes = source("Mark");
        auto reader = CReader::FromBytes(bytes);
        CSectionTable table(reader);
        QVERIFY(table.IsValid());
        for (int slot = 0x40; slot < CSectionValidation::OffsetTableEnd; slot += 4)
            put(bytes, slot, 10000 + slot * 4);
        auto uniqueReader = CReader::FromBytes(bytes);
        CSectionOffsetTable offsets(uniqueReader);
        const auto columns = CSectionValidation::ArraySpans(offsets, *table.count_info);
        QCOMPARE(columns.size(), 135);
        QSet<int> visited;
        for (const auto& column : columns) {
            QVERIFY(column.slot >= 0x48 && column.slot < CSectionValidation::OffsetTableEnd);
            QVERIFY(!visited.contains(column.slot));
            visited.insert(column.slot);
            QCOMPARE(column.offset, word(bytes, column.slot));
        }
        QCOMPARE(visited.size(), 135);
    }

    void noOpAndInPlaceEditsKeepExactBytes_data() { models(); }
    void noOpAndInPlaceEditsKeepExactBytes()
    {
        QFETCH(QString, name);
        const auto baseline = source(name);
        QVERIFY(!baseline.isEmpty());
        QByteArray output("sentinel");
        QString error;
        QVERIFY2(CMocLayoutRewriter::Rewrite(baseline, baseline, {}, output, &error), qPrintable(error));
        QCOMPARE(output, baseline);
        auto reader = CReader::FromBytes(baseline);
        CSectionTable table(reader);
        const auto columns = CSectionValidation::ArraySpans(*table.sectionOffsetTable, *table.count_info);
        for (const auto& column : columns) QCOMPARE(column.offset, word(baseline, column.slot));
        auto candidate = baseline;
        put(candidate, word(candidate, 0x11c), 7);
        QVERIFY2(CMocLayoutRewriter::Rewrite(baseline, candidate, {}, output, &error), qPrintable(error));
        QCOMPARE(output, candidate);
        QCOMPARE(baseline, source(name));
    }

    void relocatedColumnsPreserveAllOtherData_data()
    {
        QTest::addColumn<QString>("name");
        QTest::addColumn<int>("slot");
        QTest::addColumn<int>("countIndex");
        QTest::addColumn<int>("width");
        struct Field { int slot, countIndex, width; };
        const Field fields[]{{0x104, 5, 8}, {0x108, 5, 64}, {0x10c, 5, 4}, {0x12c, 7, 4},
                             {0x138, 8, 4}, {0x150, 9, 4}, {0x158, 9, 4}, {0x15c, 10, 4},
                             {0x180, 17, 4}, {0x90, 2, 4}, {0x9c, 2, 4}, {0x88, 1, 4}};
        for (const char* name : {"Mark", "Rice", "Natori", "Wanko"}) for (const auto& field : fields)
            QTest::newRow(qPrintable(QStringLiteral("%1-%2").arg(name).arg(field.slot, 0, 16)))
                << QString::fromLatin1(name) << field.slot << field.countIndex << field.width;
    }

    void relocatedColumnsPreserveAllOtherData()
    {
        QFETCH(QString, name);
        QFETCH(int, slot);
        QFETCH(int, countIndex);
        QFETCH(int, width);
        const auto baseline = source(name);
        QVERIFY(!baseline.isEmpty());
        auto candidate = baseline;
        const int size = count(candidate, countIndex) * width;
        const auto values = candidate.mid(word(candidate, slot), size);
        QMap<int, qint64> lengths;
        relocate(candidate, slot, values, lengths);
        QByteArray output;
        QString error;
        QVERIFY2(CMocLayoutRewriter::Rewrite(baseline, candidate, lengths, output, &error), qPrintable(error));
        QCOMPARE(output.mid(word(output, slot), size), values);
        QVERIFY2(sameColumns(candidate, output, error), qPrintable(error));
        QCOMPARE(output.size() % 64, 0);
        QCOMPARE(baseline, source(name));
        if (size) QVERIFY(word(output, slot) < baseline.size() + 64);
    }

    void growingPositionsAndMasksKeepUnreferencedValues()
    {
        const auto baseline = source("Wanko");
        auto candidate = baseline;
        QMap<int, qint64> lengths;
        const auto oldKeys = baseline.mid(word(baseline, 0x174), count(baseline, 14) * 4);
        QCOMPARE(count(baseline, 14), 126);
        for (const auto& field : {QPair<int, int>{0x15c, 10}, QPair<int, int>{0x180, 17}}) {
            const int originalCount = count(baseline, field.second);
            QByteArray values = baseline.mid(word(baseline, field.first), originalCount * 4);
            values.append(QByteArray(8, '\0'));
            relocate(candidate, field.first, values, lengths);
            put(candidate, word(candidate, 0x40) + field.second * 4, originalCount + 2);
        }
        QByteArray output;
        QString error;
        QVERIFY2(CMocLayoutRewriter::Rewrite(baseline, candidate, lengths, output, &error), qPrintable(error));
        QVERIFY2(sameColumns(candidate, output, error), qPrintable(error));
        QCOMPARE(output.mid(word(output, 0x174), oldKeys.size()), oldKeys);
    }

    void wholeAndPartialAliasesRemainIsolated_data()
    {
        QTest::addColumn<int>("shift");
        QTest::newRow("whole") << 0;
        QTest::newRow("partial-byte") << 1;
    }

    void wholeAndPartialAliasesRemainIsolated()
    {
        QFETCH(int, shift);
        auto baseline = source("Mark");
        put(baseline, 0x10c, word(baseline, 0x110) + shift);
        const auto original = baseline;
        auto candidate = baseline;
        const int size = count(candidate, 5) * 4;
        auto values = candidate.mid(word(candidate, 0x10c), size);
        values[0] = char(quint8(values[0]) ^ 0x20);
        QMap<int, qint64> lengths;
        relocate(candidate, 0x10c, values, lengths);
        QByteArray output;
        QString error;
        QVERIFY2(CMocLayoutRewriter::Rewrite(baseline, candidate, lengths, output, &error), qPrintable(error));
        QVERIFY2(sameColumns(candidate, output, error), qPrintable(error));
        QCOMPARE(output.mid(word(output, 0x110), size), baseline.mid(word(baseline, 0x110), size));
        QVERIFY(word(output, 0x10c) != word(output, 0x110));
        QCOMPARE(baseline, original);
    }

    void preservesUnknownHeaderColumnsAndCountInfoTail()
    {
        auto baseline = source("Mark");
        const int oldCanvas = word(baseline, 0x44);
        baseline.insert(oldCanvas, QByteArray(128, '\0'));
        for (int slot = 0x40; slot < word(baseline, 0x40); slot += 4)
            if (word(baseline, slot) >= oldCanvas) put(baseline, slot, word(baseline, slot) + 128);
        const QByteArray opaqueCount("unknown count metadata");
        baseline.replace(word(baseline, 0x40) + 128, opaqueCount.size(), opaqueCount);
        const QByteArray opaqueColumn = QByteArray("opaque column payload\0with embedded bytes", 40).leftJustified(128, char(0xa7));
        const int unknownOffset = baseline.size();
        baseline.append(opaqueColumn);
        put(baseline, 0x264, unknownOffset);
        auto candidate = baseline;
        QMap<int, qint64> lengths;
        relocate(candidate, 0x15c, baseline.mid(word(baseline, 0x15c), count(baseline, 10) * 4), lengths);
        QByteArray output;
        QString error;
        QVERIFY2(CMocLayoutRewriter::Rewrite(baseline, candidate, lengths, output, &error), qPrintable(error));
        QCOMPARE(output.mid(word(output, 0x264), opaqueColumn.size()), opaqueColumn);
        QCOMPARE(output.mid(word(output, 0x40) + 128, opaqueCount.size()), opaqueCount);
        QVERIFY2(sameColumns(candidate, output, error), qPrintable(error));
        candidate[word(candidate, 0x40) + 128] = 'X';
        const auto retained = output;
        QVERIFY(!CMocLayoutRewriter::Rewrite(baseline, candidate, lengths, output, &error));
        QCOMPARE(output, retained);
    }

    void multipleRelocationsAndEmptyOptionalColumn()
    {
        const auto baseline = source("Mark");
        auto candidate = baseline;
        QMap<int, qint64> lengths;
        for (int slot : {0x10c, 0x114, 0x11c})
            relocate(candidate, slot, baseline.mid(word(baseline, slot), count(baseline, 5) * 4), lengths);
        relocate(candidate, 0x208, QByteArray(count(baseline, 5) * 4, '\0'), lengths);
        QByteArray output;
        QString error;
        QVERIFY2(CMocLayoutRewriter::Rewrite(baseline, candidate, lengths, output, &error), qPrintable(error));
        QVERIFY2(sameColumns(candidate, output, error), qPrintable(error));
        QVERIFY(word(output, 0x208) > 0);
    }

    void aliasCannotAuthorizeAnotherColumnsCountGrowth()
    {
        auto baseline = source("Mark");
        const int info = word(baseline, 0x40);
        const int originalCount = count(baseline, 14);
        const int paddedCount = ((originalCount * 4 + 63) & ~63) / 4;
        const int offset = baseline.size();
        QByteArray values(paddedCount * 4, '\0');
        values.replace(0, originalCount * 4, baseline.mid(word(baseline, 0x174), originalCount * 4));
        baseline.append(values);
        put(baseline, 0x174, offset);
        put(baseline, 0x264, offset);
        put(baseline, info + 14 * 4, paddedCount);
        auto candidate = baseline;
        candidate.append(QByteArray(64, '\0'));
        put(candidate, offset + paddedCount * 4, 0x42480000);
        put(candidate, info + 14 * 4, paddedCount + 1);
        QByteArray output("unchanged");
        QString error;
        const qint64 bytes = qint64(paddedCount + 1) * 4;
        QVERIFY(!CMocLayoutRewriter::Rewrite(baseline, candidate, {{0x264, bytes}}, output, &error));
        QVERIFY(error.contains(QStringLiteral("keys.values")));
        QCOMPARE(output, QByteArray("unchanged"));
        QVERIFY2(CMocLayoutRewriter::Rewrite(baseline, candidate, {{0x174, bytes}, {0x264, bytes}}, output, &error), qPrintable(error));
        for (int slot : {0x174, 0x264}) {
            QCOMPARE(word(output, word(output, slot) + paddedCount * 4), 0x42480000);
            QCOMPARE(output.mid(word(output, slot), bytes), candidate.mid(offset, bytes));
        }
        QVERIFY2(sameColumns(candidate, output, error), qPrintable(error));
    }

    void explicitOptionalClearSurvivesRelocations_data()
    {
        QTest::addColumn<int>("mode");
        QTest::newRow("clear-only") << 0;
        QTest::newRow("clear-and-scalar-relocation") << 1;
        QTest::newRow("clear-and-aliased-opaque-relocation") << 2;
        QTest::newRow("clear-before-invalid-plan") << 3;
    }

    void explicitOptionalClearSurvivesRelocations()
    {
        QFETCH(int, mode);
        auto baseline = source("Mark");
        QMap<int, qint64> ignored;
        relocate(baseline, 0x208, QByteArray(count(baseline, 5) * 4, '\0'), ignored);
        if (mode == 2) put(baseline, 0x264, word(baseline, 0x208));
        const auto original = baseline;
        auto candidate = baseline;
        put(candidate, 0x208, 0);
        QMap<int, qint64> lengths{{0x208, 0}};
        QByteArray payload;
        if (mode == 1 || mode == 3)
            relocate(candidate, 0x10c, baseline.mid(word(baseline, 0x10c), count(baseline, 5) * 4), lengths);
        if (mode == 2) {
            payload = QByteArray(count(baseline, 5) * 4, char(0x5a));
            relocate(candidate, 0x264, payload, lengths);
        }
        if (mode == 3) lengths[0x10c] = std::numeric_limits<qint64>::max();
        QByteArray output("unchanged");
        QString error;
        const bool ok = CMocLayoutRewriter::Rewrite(baseline, candidate, lengths, output, &error);
        if (mode == 3) {
            QVERIFY(!ok);
            QCOMPARE(output, QByteArray("unchanged"));
        } else {
            QVERIFY2(ok, qPrintable(error));
            QCOMPARE(word(output, 0x208), 0);
            QVERIFY2(sameColumns(candidate, output, error), qPrintable(error));
            if (mode == 0) QCOMPARE(output, candidate);
            if (mode == 2) QCOMPARE(output.mid(word(output, 0x264), payload.size()), payload);
        }
        QCOMPARE(baseline, original);
    }

    void incompleteCountChangesAndMalformedPlansKeepOutput_data()
    {
        QTest::addColumn<int>("mutation");
        for (int mutation = 0; mutation < 9; ++mutation)
            QTest::newRow(qPrintable(QString::number(mutation))) << mutation;
    }

    void oversizedHeaderIsRejectedBeforeCountAllocation()
    {
        auto baseline = source("Mark");
        const int headerEnd = 0x40 + 1000001 * 4;
        baseline.resize((headerEnd + 255) & ~63);
        put(baseline, 0x40, headerEnd);
        QByteArray output("unchanged");
        QString error;
        QVERIFY(!CMocLayoutRewriter::Rewrite(baseline, baseline, {}, output, &error));
        QVERIFY(error.contains(QStringLiteral("文件头")));
        QCOMPARE(output, QByteArray("unchanged"));
    }

    void incompleteCountChangesAndMalformedPlansKeepOutput()
    {
        QFETCH(int, mutation);
        const auto baseline = source("Mark");
        auto candidate = baseline;
        QMap<int, qint64> lengths;
        relocate(candidate, 0x10c, baseline.mid(word(baseline, 0x10c), count(baseline, 5) * 4), lengths);
        switch (mutation) {
        case 0: lengths.clear(); break;
        case 1: lengths[0x10c] -= 1; break;
        case 2: lengths[0x10c] = std::numeric_limits<qint64>::max(); break;
        case 3: lengths[0x10d] = 4; break;
        case 4: lengths[0x40] = 128; break;
        case 5: put(candidate, word(candidate, 0x40) + 5 * 4, count(candidate, 5) + 1); break;
        case 6: relocate(candidate, 0x104, QByteArray(count(candidate, 5) * 4, '\0'), lengths); break;
        case 7: candidate.append('x'); break;
        case 8: candidate[4] = 6; break;
        }
        QByteArray output("unchanged");
        QString error;
        QVERIFY(!CMocLayoutRewriter::Rewrite(baseline, candidate, lengths, output, &error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(output, QByteArray("unchanged"));
        QCOMPARE(baseline, source("Mark"));
    }
};

QTEST_APPLESS_MAIN(MocLayoutRewriterTests)
#include "layout_moc_rewriter_tests.moc"
