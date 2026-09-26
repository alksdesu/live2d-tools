#include "live2dstruct/cmocdeformerorder.h"
#include "deformer_order_fixture.h"
#include <QtTest>

using namespace live2dstruct;
namespace Fixture = DeformerOrderFixture;

class DeformerOrderMocTests : public QObject
{
    Q_OBJECT
private slots:
    void reversePermutationPreservesAllColumns_data()
    {
        QTest::addColumn<QString>("path");
        for (const QString& name : {"Mark", "Rice", "Natori", "Wanko"})
            QTest::newRow(qPrintable(name)) << QString(TEST_RESOURCES) + '/' + name + '/' + name + ".moc3";
        const auto extra = qEnvironmentVariable("LIVE2D_REAL_CORE_EXTRA_MOC");
        if (!extra.isEmpty()) QTest::newRow("Mao-v5") << extra;
    }
    void reversePermutationPreservesAllColumns()
    {
        QFETCH(QString, path);
        const auto original = Fixture::read(path);
        auto reader = CReader::FromBytes(original); CSectionTable table(reader); QVERIFY(table.IsValid());
        QByteArray unchanged("sentinel"); QString error;
        QVERIFY2(CMocDeformerOrder::Normalize(original, unchanged, &error), qPrintable(error));
        QCOMPARE(unchanged, original);
        const auto source = Fixture::permute(original, Fixture::reverseOrder(table)); QVERIFY(!source.isEmpty());
        QVERIFY(source != original);
        QByteArray result("sentinel");
        QVERIFY2(CMocDeformerOrder::Normalize(source, result, &error), qPrintable(error));
        QCOMPARE(Fixture::semanticDifference(source, result), QString());
        QByteArray repeated;
        QVERIFY2(CMocDeformerOrder::Normalize(result, repeated, &error), qPrintable(error));
        QCOMPARE(result, repeated);
        QCOMPARE(Fixture::read(path), original);
    }

    void aliasedColumnsAndScratchBytesArePreserved_data()
    {
        QTest::addColumn<int>("alias");
        QTest::newRow("exact") << 0;
        QTest::newRow("partial") << 1;
        QTest::newRow("parent-exact") << 2;
        QTest::newRow("parent-partial") << 3;
    }
    void aliasedColumnsAndScratchBytesArePreserved()
    {
        QFETCH(int, alias);
        const auto original = Fixture::read(QString(TEST_RESOURCES) + "/Rice/Rice.moc3");
        auto ar = CReader::FromBytes(original); CSectionTable a(ar); QVERIFY(a.IsValid());
        auto source = Fixture::permute(original, Fixture::reverseOrder(a));
        auto reader = CReader::FromBytes(source); CSectionTable table(reader); QVERIFY(table.IsValid());
        const auto* offsets = table.sectionOffsetTable->deformers;
        const auto spans = CSectionValidation::ArraySpans(*table.sectionOffsetTable, *table.count_info);
        const auto enabled = std::find_if(spans.cbegin(), spans.cend(), [](const auto& s) { return QString::fromLatin1(s.name) == "deformers.is_enabled"; });
        QVERIFY(enabled != spans.cend());
        if (alias < 2) Fixture::put(source, enabled->slot, offsets->is_visible + alias * 4);
        else {
            const auto meshParent = std::find_if(spans.cbegin(), spans.cend(), [](const auto& s) { return QString::fromLatin1(s.name) == "art_meshes.parent_deformer_indices"; });
            QVERIFY(meshParent != spans.cend());
            Fixture::put(source, meshParent->slot, offsets->parent_deformer_indices + (alias - 2) * 4);
        }
        for (int byte = 0; byte < table.deformers.size() * 8; ++byte)
            source[offsets->runtime_space_0 + byte] = char((byte * 17 + 3) % 251);
        const auto before = source;
        QByteArray result; QString error;
        QVERIFY2(CMocDeformerOrder::Normalize(source, result, &error), qPrintable(error));
        QCOMPARE(Fixture::semanticDifference(source, result), QString());
        QCOMPARE(source, before);
    }

    void invalidReferencesAndExtensionsFailAtomically_data()
    {
        QTest::addColumn<int>("fault");
        QTest::newRow("self-cycle") << 0;
        QTest::newRow("cycle") << 1;
        QTest::newRow("bad-parent") << 2;
        QTest::newRow("bad-mesh-parent") << 3;
        QTest::newRow("unknown-active-column") << 4;
        QTest::newRow("bad-extension-offset") << 5;
        QTest::newRow("scratch-overlap") << 6;
        QTest::newRow("unknown-kind") << 7;
        QTest::newRow("bad-specific-source") << 8;
        QTest::newRow("truncated") << 9;
        QTest::newRow("unknown-active-count") << 10;
        QTest::newRow("active-uv-alias") << 11;
        QTest::newRow("active-ebo-alias") << 12;
        QTest::newRow("other-runtime-high-half-alias") << 13;
    }
    void invalidReferencesAndExtensionsFailAtomically()
    {
        QFETCH(int, fault);
        const auto original = Fixture::read(QString(TEST_RESOURCES) + "/Mark/Mark.moc3");
        auto ar = CReader::FromBytes(original); CSectionTable a(ar); QVERIFY(a.IsValid());
        auto source = Fixture::permute(original, Fixture::reverseOrder(a));
        auto reader = CReader::FromBytes(source); CSectionTable table(reader); QVERIFY(table.IsValid());
        const auto* offsets = table.sectionOffsetTable->deformers;
        const int unknown = CSectionValidation::KnownOffsetEnd(table.sectionOffsetTable->mocVersion);
        if (fault == 0) Fixture::put(source, offsets->parent_deformer_indices, 0);
        if (fault == 1) { Fixture::put(source, offsets->parent_deformer_indices, 1); Fixture::put(source, offsets->parent_deformer_indices + 4, 0); }
        if (fault == 2) Fixture::put(source, offsets->parent_deformer_indices, table.deformers.size());
        if (fault == 3) Fixture::put(source, table.sectionOffsetTable->art_meshes->parent_deformer_indices, table.deformers.size());
        if (fault == 4) Fixture::put(source, unknown, table.sectionOffsetTable->keys->values);
        if (fault == 5) Fixture::put(source, unknown, 1);
        if (fault == 6) Fixture::put(source, 0x68, offsets->ids);
        if (fault == 7) Fixture::put(source, offsets->types, 99);
        if (fault == 8) Fixture::put(source, offsets->specific_sources_indices, 1000000);
        if (fault == 9) source.chop(128);
        if (fault == 10) {
            const int canvas = table.sectionOffsetTable->canvas_info->CanvasOffset;
            const auto values = source.mid(canvas, 20);
            const int next = source.size(); source.append(QByteArray(64, '\0'));
            std::memcpy(source.data() + next, values.constData(), values.size());
            std::memset(source.data() + canvas, 0, 20);
            Fixture::put(source, 0x44, next);
            Fixture::put(source, table.sectionOffsetTable->count_info->count_info_offset + CSectionValidation::CountInfoSize(table.sectionOffsetTable->mocVersion), 1);
        }
        if (fault >= 11) {
            const auto spans = CSectionValidation::ArraySpans(*table.sectionOffsetTable, *table.count_info);
            const auto visible = std::find_if(spans.cbegin(), spans.cend(), [](const auto& span) { return QString::fromLatin1(span.name) == "deformers.is_visible"; });
            QVERIFY(visible != spans.cend());
            const auto* mesh = table.art_meshes.front();
            const int offset = fault == 11 ? table.sectionOffsetTable->uvs->uvs + mesh->uv_sources_begin_indices * 4 :
                fault == 12 ? table.sectionOffsetTable->position_indices->indices + mesh->position_index_sources_begin_indices * 2 :
                table.sectionOffsetTable->parameters->runtime_space_0 + table.parameters.size() * 4;
            Fixture::put(source, visible->slot, offset);
        }
        const auto before = source;
        QByteArray result("keep original output"); QString error;
        QVERIFY(!CMocDeformerOrder::Normalize(source, result, &error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(result, QByteArray("keep original output"));
        QCOMPARE(source, before);
    }

    void unusedUvStorageDoesNotBlockSorting()
    {
        const auto original = Fixture::read(QString(TEST_RESOURCES) + "/Mark/Mark.moc3");
        auto firstReader = CReader::FromBytes(original); CSectionTable first(firstReader); QVERIFY(first.IsValid());
        auto source = Fixture::permute(original, Fixture::reverseOrder(first));
        auto reader = CReader::FromBytes(source); CSectionTable table(reader); QVERIFY(table.IsValid());
        const auto spans = CSectionValidation::ArraySpans(*table.sectionOffsetTable, *table.count_info);
        const auto uv = std::find_if(spans.cbegin(), spans.cend(), [](const auto& span) { return QString::fromLatin1(span.name) == "uvs.uvs"; });
        const auto visible = std::find_if(spans.cbegin(), spans.cend(), [](const auto& span) { return QString::fromLatin1(span.name) == "deformers.is_visible"; });
        QVERIFY(uv != spans.cend()); QVERIFY(visible != spans.cend());
        const auto uvData = Fixture::payload(source, *uv), visibleData = Fixture::payload(source, *visible);
        const int start = source.size();
        source.append(uvData); source.append(visibleData);
        source.resize((source.size() + 63) & ~qsizetype(63), '\0');
        Fixture::put(source, uv->slot, start);
        Fixture::put(source, visible->slot, start + uvData.size());
        Fixture::put(source, table.sectionOffsetTable->count_info->count_info_offset + uv->countIndex * 4, uv->count + visible->count);
        QByteArray result; QString error;
        QVERIFY2(CMocDeformerOrder::Normalize(source, result, &error), qPrintable(error));
        QCOMPARE(Fixture::semanticDifference(source, result), QString());
    }

    void opaqueEofMarkerMovesAndSortedOpaqueDataIsUntouched()
    {
        const auto original = Fixture::read(QString(TEST_RESOURCES) + "/Mark/Mark.moc3");
        auto ar = CReader::FromBytes(original); CSectionTable a(ar); QVERIFY(a.IsValid());
        const int unknown = CSectionValidation::KnownOffsetEnd(a.sectionOffsetTable->mocVersion);
        auto sorted = original;
        Fixture::put(sorted, unknown, a.sectionOffsetTable->keys->values);
        QByteArray result; QString error;
        QVERIFY2(CMocDeformerOrder::Normalize(sorted, result, &error), qPrintable(error));
        QCOMPARE(result, sorted);
        auto reversed = Fixture::permute(original, Fixture::reverseOrder(a));
        Fixture::put(reversed, unknown, reversed.size());
        QVERIFY2(CMocDeformerOrder::Normalize(reversed, result, &error), qPrintable(error));
        QCOMPARE(Fixture::word(result, unknown), result.size());
        QCOMPARE(Fixture::semanticDifference(reversed, result), QString());
    }
};

QTEST_GUILESS_MAIN(DeformerOrderMocTests)
#include "deformer_order_moc_tests.moc"
