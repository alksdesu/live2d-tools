#include "warp_grid_fixture.h"
#include "live2dstruct/cmocgeometrypool.h"
#include "std/cexportcontrol.h"
#include <QtTest>

using namespace live2dstruct;
namespace Fixture = WarpGridFixture;
using Raw = CMocDeformerStructureBuilder;

class WarpGridStructureTests : public QObject
{
    Q_OBJECT
private slots:
    void resizeRetainsEveryOrdinaryAndBlendForm_data()
    {
        QTest::addColumn<QString>("name");
        QTest::addColumn<bool>("blend");
        for (const QString name : { "Mark", "Rice", "Natori", "Wanko" })
            QTest::newRow(qPrintable(name)) << name << false;
        if (!qEnvironmentVariable("LIVE2D_REAL_CORE_EXTRA_MOC").isEmpty())
            QTest::newRow("Mao-blend") << QString("Mao") << true;
    }
    void resizeRetainsEveryOrdinaryAndBlendForm()
    {
        QFETCH(QString, name);
        QFETCH(bool, blend);
        const auto base = DeformerReplayFixture::read(name);
        QVERIFY(!base.isEmpty());
        auto reader = CReader::FromBytes(base);
        CSectionTable before(reader);
        QVERIFY(before.IsValid());
        const int index = Fixture::target(before, blend);
        QVERIFY(index >= 0);
        const int specific = before.deformers[index]->specific_sources_indices;
        const auto* old = before.warp_deformers[specific];
        Raw::ResizeWarpRequest request { before.deformers[index]->ids, old->rows + 1, old->columns + 2 };
        Raw::Impact impact;
        Raw::Result result;
        QString error;
        QVERIFY2(Raw::AnalyzeResizeWarp(base, request, impact, &error), qPrintable(error));
        QVERIFY2(Raw::ResizeWarp(base, request, result, &error), qPrintable(error));
        QVERIFY(impact.warpGrid);
        QCOMPARE(impact.warpGrid->ordinaryKeyforms, old->keyform_sources_counts);
        if (blend)
            QVERIFY(impact.warpGrid->blendKeyforms > 0);
        auto nextReader = CReader::FromBytes(result.bytes);
        CSectionTable after(nextReader);
        QVERIFY(after.IsValid());
        const int nextSpecific = after.deformers[index]->specific_sources_indices;
        const auto* next = after.warp_deformers[nextSpecific];
        QCOMPARE(next->rows, request.rows);
        QCOMPARE(next->columns, request.columns);
        QCOMPARE(next->is_quad_source, old->is_quad_source);
        QCOMPARE(next->keyform_binding_sources_indices, old->keyform_binding_sources_indices);
        QCOMPARE(after.deformers[index]->keyform_binding_sources_indices,
            before.deformers[index]->keyform_binding_sources_indices);
        QCOMPARE(Fixture::bindingMetadata(before, specific), Fixture::bindingMetadata(after, nextSpecific));
        QCOMPARE(after.deformers.size(), before.deformers.size());
        const auto previousRows = Fixture::formRows(before, specific),
                   nextRows = Fixture::formRows(after, nextSpecific);
        QCOMPARE(nextRows.size(), previousRows.size());
        for (int form = 0; form < previousRows.size(); ++form) {
            QCOMPARE(previousRows[form].first, nextRows[form].first);
            const auto expected
                = Fixture::oracle(before, specific, previousRows[form].second, request.rows, request.columns);
            const auto actual = Fixture::positions(after, nextRows[form].second, next->vertex_counts);
            QVERIFY(Fixture::close(expected, actual));
            const auto* a = before.warp_deformer_keyforms[previousRows[form].second];
            const auto* b = after.warp_deformer_keyforms[nextRows[form].second];
            QCOMPARE(a->opacities, b->opacities);
            QCOMPARE(a->multiply_color_sources_index, b->multiply_color_sources_index);
            QCOMPARE(a->screen_color_sources_index, b->screen_color_sources_index);
        }
        QCOMPARE(Fixture::siblingPositions(before, request.id), Fixture::siblingPositions(after, request.id));
        QCOMPARE(Fixture::fixedColumns(base, before), Fixture::fixedColumns(result.bytes, after));
        QByteArray rebuilt;
        QVERIFY(Fixture::Codec::BuildColumnState(base, result.state, rebuilt, &error));
        QCOMPARE(rebuilt, result.bytes);
    }

    void sharedSpecificSourceIsClonedOnlyForTheTarget_data()
    {
        QTest::addColumn<QString>("name");
        QTest::addColumn<bool>("blend");
        QTest::newRow("ordinary") << QString("Mark") << false;
        if (!qEnvironmentVariable("LIVE2D_REAL_CORE_EXTRA_MOC").isEmpty())
            QTest::newRow("blend") << QString("Mao") << true;
    }
    void sharedSpecificSourceIsClonedOnlyForTheTarget()
    {
        QFETCH(QString, name);
        QFETCH(bool, blend);
        auto base = DeformerReplayFixture::read(name);
        auto reader = CReader::FromBytes(base);
        CSectionTable source(reader);
        const int target = Fixture::target(source, blend);
        QVERIFY(target >= 0);
        int sibling = -1;
        for (int index = 0; index < source.deformers.size(); ++index)
            if (index != target && source.deformers[index]->types == 0) {
                sibling = index;
                break;
            }
        QVERIFY(sibling >= 0);
        DeformerReplayFixture::RawColumns raw(base);
        raw.set("deformers.specific_sources_indices", sibling, source.deformers[target]->specific_sources_indices);
        raw.set("deformers.keyform_binding_sources_indices", sibling,
            source.deformers[target]->keyform_binding_sources_indices);
        base = raw.encode();
        QVERIFY(!base.isEmpty());
        auto beforeReader = CReader::FromBytes(base);
        CSectionTable before(beforeReader);
        const int specific = before.deformers[target]->specific_sources_indices;
        const auto* warp = before.warp_deformers[specific];
        Raw::Result result;
        QString error;
        const QString id = before.deformers[target]->ids;
        QVERIFY2(Raw::ResizeWarp(base, { id, warp->rows + 1, warp->columns + 1 }, result, &error), qPrintable(error));
        auto afterReader = CReader::FromBytes(result.bytes);
        CSectionTable after(afterReader);
        QCOMPARE(after.deformers[sibling]->specific_sources_indices, specific);
        QVERIFY(after.deformers[target]->specific_sources_indices != specific);
        QCOMPARE(Fixture::siblingPositions(before, id), Fixture::siblingPositions(after, id));
        QCOMPARE(Fixture::bindingMetadata(before, specific),
            Fixture::bindingMetadata(after, after.deformers[target]->specific_sources_indices));
        if (blend)
            QVERIFY(result.impact.warpGrid->blendKeyforms > 0);
    }

    void repeatedResizesReusePools()
    {
        auto current = DeformerReplayFixture::read("Mark");
        auto reader = CReader::FromBytes(current);
        CSectionTable source(reader);
        const int target = Fixture::target(source);
        QVERIFY(target >= 0);
        const QString id = source.deformers[target]->ids;
        const auto* original = source.warp_deformers[source.deformers[target]->specific_sources_indices];
        const int oldRows = original->rows, oldColumns = original->columns;
        QVector<int> stable;
        for (int iteration = 0; iteration < 20; ++iteration) {
            auto beforeReader = CReader::FromBytes(current);
            CSectionTable before(beforeReader);
            const auto siblings = Fixture::siblingPositions(before, id);
            const Raw::ResizeWarpRequest request { id, iteration % 2 ? oldRows : oldRows + 1,
                iteration % 2 ? oldColumns : oldColumns + 2 };
            Raw::Result result;
            QString error;
            QVERIFY2(Raw::ResizeWarp(current, request, result, &error), qPrintable(error));
            auto afterReader = CReader::FromBytes(result.bytes);
            CSectionTable after(afterReader);
            QCOMPARE(Fixture::siblingPositions(after, id), siblings);
            if (iteration == 2)
                stable = Fixture::poolCounts(after);
            if (iteration > 2)
                QCOMPARE(Fixture::poolCounts(after), stable);
            current = std::move(result.bytes);
        }
    }

    void sharedPositionBeginProtectsTheLongerOwner()
    {
        auto base = DeformerReplayFixture::read("Mark");
        {
            auto initialReader = CReader::FromBytes(base);
            CSectionTable initial(initialReader);
            const int index = Fixture::target(initial);
            QVERIFY(index >= 0);
            const auto* existing = initial.warp_deformers[initial.deformers[index]->specific_sources_indices];
            Raw::InsertRequest insert;
            insert.id = "LongerPositionOwner";
            insert.shape = Raw::Warp { existing->rows + 1, existing->columns + 1, false, { } };
            Raw::Result added;
            QVERIFY(Raw::Insert(base, insert, added));
            base = std::move(added.bytes);
        }
        auto reader = CReader::FromBytes(base);
        CSectionTable source(reader);
        int small = -1, large = -1;
        for (int index = 0; index < source.deformers.size(); ++index) {
            const auto* node = source.deformers[index];
            if (node->types != 0)
                continue;
            const auto* warp = source.warp_deformers[node->specific_sources_indices];
            if (warp->keyform_sources_counts < 1)
                continue;
            if (small < 0
                || warp->vertex_counts
                    < source.warp_deformers[source.deformers[small]->specific_sources_indices]->vertex_counts)
                small = index;
            if (large < 0
                || warp->vertex_counts
                    > source.warp_deformers[source.deformers[large]->specific_sources_indices]->vertex_counts)
                large = index;
        }
        QVERIFY(small >= 0 && large >= 0 && small != large);
        const auto* first = source.warp_deformers[source.deformers[small]->specific_sources_indices];
        const auto* second = source.warp_deformers[source.deformers[large]->specific_sources_indices];
        QVERIFY(first->vertex_counts < second->vertex_counts);
        DeformerReplayFixture::RawColumns raw(base);
        const int begin = source.warp_deformer_keyforms[second->keyform_sources_begin_indices]
                              ->keyform_position_sources_begin_indices;
        raw.set("warp_deformer_keyforms.keyform_position_sources_begin_indices", first->keyform_sources_begin_indices,
            begin);
        base = raw.encode();
        QVERIFY(!base.isEmpty());
        auto beforeReader = CReader::FromBytes(base);
        CSectionTable before(beforeReader);
        const QString id = before.deformers[small]->ids;
        const auto siblings = Fixture::siblingPositions(before, id);
        Raw::Result result;
        QString error;
        QVERIFY2(Raw::ResizeWarp(base, { id, first->rows + 1, first->columns + 1 }, result, &error), qPrintable(error));
        auto afterReader = CReader::FromBytes(result.bytes);
        CSectionTable after(afterReader);
        QCOMPARE(Fixture::siblingPositions(after, id), siblings);
    }

    void geometryPoolRejectsFragmentationWithoutGrowingTheHighWater()
    {
        detail::CMocGeometryPool pool(1000000, QStringLiteral("关键形位置"));
        pool.protect(1, 499998);
        pool.protect(500000, 500000);
        pool.prepare();
        qint64 work = 0;
        QVERIFY_THROWS_EXCEPTION(detail::CMocGeometryAllocationFailure, pool.allocate(2, work));
        QCOMPARE(pool.size(), 1000000);
        QCOMPARE(work, qint64(2));
        QCOMPARE(pool.allocate(1, work), 0);
        QCOMPARE(pool.allocate(1, work), 499999);
    }

    void unchangedGridAndReplayDigestsAreStrict()
    {
        const auto base = DeformerReplayFixture::read("Mark");
        auto reader = CReader::FromBytes(base);
        CSectionTable table(reader);
        const int target = Fixture::target(table);
        QVERIFY(target >= 0);
        const auto* warp = table.warp_deformers[table.deformers[target]->specific_sources_indices];
        Raw::ResizeWarpRequest resize { table.deformers[target]->ids, warp->rows, warp->columns };
        Raw::Result result;
        QString error;
        QVERIFY(Raw::ResizeWarp(base, resize, result, &error));
        QCOMPARE(result.bytes, base);
        QVERIFY(result.state.columns.isEmpty());
        QVERIFY(result.state.counts.isEmpty());
        const auto digest = QCryptographicHash::hash(base, QCryptographicHash::Sha256);
        CDeformerStructureRequest request { resize, digest, digest };
        CDeformerStructureReplay::Impact impact;
        QVERIFY(CDeformerStructureReplay::Analyze(base, request, impact, &error));
        QVERIFY(impact.structure.warpGrid);
        QCOMPARE(impact.coordinateOwners.size(), 1);
        QVERIFY(CDeformerStructureReplay::Build(base, request, result, &error));
        QCOMPARE(result.bytes, base);
        request.expectedAuthoringDigest[0] ^= 1;
        QVERIFY(!CDeformerStructureReplay::Build(base, request, result, &error));
        QCOMPARE(result.bytes, base);
    }

    void callbackMutationUsesOwnedSourceAndRequest()
    {
        auto backing = DeformerReplayFixture::read("Mark");
        auto reader = CReader::FromBytes(backing);
        CSectionTable table(reader);
        const int target = Fixture::target(table);
        QVERIFY(target >= 0);
        const auto* warp = table.warp_deformers[table.deformers[target]->specific_sources_indices];
        auto id = table.deformers[target]->ids;
        Raw::ResizeWarpRequest request { QString::fromRawData(id.constData(), id.size()), warp->rows + 1,
            warp->columns + 1 };
        Raw::Result expected;
        QVERIFY(Raw::ResizeWarp(backing, request, expected));
        auto* bytes = backing.data();
        auto* characters = id.data();
        const auto borrowed = QByteArray::fromRawData(bytes, backing.size());
        request.id = QString::fromRawData(characters, id.size());
        CExportControl control;
        bool called = false;
        QVERIFY(control.SetHandler([&](const auto&, QString*) {
            if (!called) {
                called = true;
                bytes[0] = 'X';
                characters[0] = QChar('X');
            }
            return true;
        }));
        Raw::Result actual;
        QString error;
        QVERIFY2(Raw::ResizeWarp(borrowed, request, actual, &error, &control), qPrintable(error));
        QVERIFY(called);
        QCOMPARE(actual.bytes, expected.bytes);
        QCOMPARE(actual.impact.id, expected.impact.id);
    }

    void failureAndCancellationDoNotPublishPartialResults()
    {
        const auto base = DeformerReplayFixture::read("Mark");
        auto reader = CReader::FromBytes(base);
        CSectionTable table(reader);
        const int target = Fixture::target(table);
        QVERIFY(target >= 0);
        const auto* warp = table.warp_deformers[table.deformers[target]->specific_sources_indices];
        Raw::ResizeWarpRequest request { table.deformers[target]->ids, warp->rows + 1, warp->columns + 1 };
        Raw::Result result;
        result.bytes = "unchanged";
        result.impact.id = "unchanged";
        CExportControl control;
        QVERIFY(control.SetHandler([&](const auto&, QString*) {
            control.Cancel();
            return true;
        }));
        QVERIFY(!Raw::ResizeWarp(base, request, result, nullptr, &control));
        QCOMPARE(result.bytes, QByteArray("unchanged"));
        QCOMPARE(result.impact.id, QString("unchanged"));
        request.rows = 1001;
        QVERIFY(!Raw::ResizeWarp(base, request, result));
        QCOMPARE(result.bytes, QByteArray("unchanged"));
        request.rows = 499;
        request.columns = 999;
        QVERIFY(!Raw::ResizeWarp(base, request, result));
        QCOMPARE(result.bytes, QByteArray("unchanged"));
    }
};
QTEST_GUILESS_MAIN(WarpGridStructureTests)
#include "warp_grid_structure_tests.moc"
