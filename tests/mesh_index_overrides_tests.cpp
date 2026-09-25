#include "live2dstruct/cmeshindexoverrides.h"

#include <QtTest>

using live2dstruct::CMeshIndexOverrides;

class MeshIndexOverridesTests : public QObject
{
    Q_OBJECT
private slots:
    void drawableViewsExposeMatchingCountsAndValues()
    {
        CMeshIndexOverrides overrides;
        QVERIFY(!overrides.Find(0));
        QVERIFY(overrides.Set(0, 4, {0, 1, 2, 0, 2, 3}));
        QVERIFY(overrides.Set(7, 5, {4, 2, 1}));
        const auto* first = overrides.Find(0);
        const auto* second = overrides.Find(7);
        QVERIFY(first);
        QVERIFY(second);
        QCOMPARE(first->size(), 6);
        QCOMPARE(second->size(), 3);
        QVERIFY(first->constData());
        QVERIFY(second->constData());
        QCOMPARE(first->constData()[5], quint16(3));
        QCOMPARE(second->constData()[0], quint16(4));
        QCOMPARE(*first, (QVector<quint16>{0, 1, 2, 0, 2, 3}));
        QCOMPARE(*second, (QVector<quint16>{4, 2, 1}));
        QVERIFY(!overrides.Find(1));

        QVERIFY(overrides.Set(0, 4, {3, 1, 2}));
        QCOMPARE(*overrides.Find(0), (QVector<quint16>{3, 1, 2}));
        QCOMPARE(overrides.Find(0)->size(), 3);
        QCOMPARE(*overrides.Find(7), (QVector<quint16>{4, 2, 1}));
    }

    void invalidUpdatesPreserveStoredTopology()
    {
        CMeshIndexOverrides overrides;
        const QVector<quint16> original{0, 1, 2};
        QVERIFY(overrides.Set(2, 3, original));
        for (const auto& invalid : {QVector<quint16>{}, QVector<quint16>{0, 1},
                                    QVector<quint16>{0, 1, 3}, QVector<quint16>{0, 1, 65535}}) {
            QVERIFY(!overrides.Set(2, 3, invalid));
            QCOMPARE(*overrides.Find(2), original);
            QVERIFY(!overrides.Set(5, 3, invalid));
            QVERIFY(!overrides.Find(5));
        }
        QVERIFY(!overrides.Set(2, 0, original));
        QVERIFY(!overrides.Set(2, -1, original));
        QVERIFY(!overrides.Set(2, 65536, {0, 1, 32768}));
        QVERIFY(!overrides.Set(2, 3, QVector<quint16>(3000003, 0)));
        QCOMPARE(*overrides.Find(2), original);
        QVERIFY(!overrides.Set(-1, 3, original));
        QVERIFY(!overrides.Find(-1));
        QCOMPARE(*overrides.Find(2), original);
        QCOMPARE(overrides.Revision(), quint64(1));
    }

    void removalAndClearRestoreMissingOverride()
    {
        CMeshIndexOverrides overrides;
        QVERIFY(overrides.IsEmpty());
        QVERIFY(overrides.Set(0, 3, {0, 1, 2}));
        QVERIFY(overrides.Set(1, 4, {3, 1, 2}));
        QVERIFY(!overrides.IsEmpty());
        QVERIFY(!overrides.Remove(-1));
        QVERIFY(!overrides.Remove(9));
        QVERIFY(overrides.Remove(0));
        QVERIFY(!overrides.Find(0));
        QVERIFY(!overrides.Remove(0));
        QVERIFY(!overrides.IsEmpty());
        QCOMPARE(*overrides.Find(1), (QVector<quint16>{3, 1, 2}));
        overrides.Clear();
        QVERIFY(overrides.IsEmpty());
        QVERIFY(!overrides.Find(0));
        QVERIFY(!overrides.Find(1));
        QVERIFY(!overrides.Remove(1));
        overrides.Clear();
        QVERIFY(overrides.Set(1, 3, {2, 0, 1}));
        QCOMPARE(*overrides.Find(1), (QVector<quint16>{2, 0, 1}));
    }

    void storageOwnsInputAndSeparatesInstances()
    {
        CMeshIndexOverrides first;
        CMeshIndexOverrides second;
        QVector<quint16> input{0, 1, 2};
        QVERIFY(first.Set(0, 4, input));
        QVERIFY(second.Set(0, 4, {3, 1, 2}));
        input[0] = 3;
        input.append({1, 2, 3});
        QCOMPARE(*first.Find(0), (QVector<quint16>{0, 1, 2}));
        QCOMPARE(*second.Find(0), (QVector<quint16>{3, 1, 2}));
        first.Clear();
        QCOMPARE(*second.Find(0), (QVector<quint16>{3, 1, 2}));
    }

    void copiedStoresDetachOnChanges()
    {
        CMeshIndexOverrides original;
        QVERIFY(original.Set(0, 4, {0, 1, 2}));
        QVERIFY(original.Set(1, 4, {0, 2, 3}));
        CMeshIndexOverrides copied = original;
        CMeshIndexOverrides assigned;
        assigned = original;
        QCOMPARE(copied.Revision(), quint64(2));
        QCOMPARE(assigned.Revision(), quint64(2));
        QVERIFY(original.Set(0, 4, {3, 1, 2}));
        QVERIFY(original.Remove(1));
        QCOMPARE(original.Revision(), quint64(4));
        QCOMPARE(copied.Revision(), quint64(2));
        QCOMPARE(assigned.Revision(), quint64(2));
        QCOMPARE(*copied.Find(0), (QVector<quint16>{0, 1, 2}));
        QCOMPARE(*copied.Find(1), (QVector<quint16>{0, 2, 3}));
        QCOMPARE(*assigned.Find(0), (QVector<quint16>{0, 1, 2}));
        QVERIFY(copied.Set(1, 4, {1, 2, 3}));
        assigned.Clear();
        QCOMPARE(*copied.Find(1), (QVector<quint16>{1, 2, 3}));
        QCOMPARE(*original.Find(0), (QVector<quint16>{3, 1, 2}));
        QVERIFY(!original.Find(1));
        QVERIFY(!assigned.Find(0));
        QCOMPARE(original.Revision(), quint64(4));
        QCOMPARE(copied.Revision(), quint64(3));
        QCOMPARE(assigned.Revision(), quint64(3));
    }

    void revisionOnlyAdvancesForChangedOverrides()
    {
        CMeshIndexOverrides overrides;
        QCOMPARE(overrides.Revision(), quint64(0));
        QVERIFY(!overrides.Set(-1, 3, {0, 1, 2}));
        QVERIFY(!overrides.Remove(0));
        overrides.Clear();
        QCOMPARE(overrides.Revision(), quint64(0));
        QVERIFY(overrides.Set(0, 3, {0, 1, 2}));
        QCOMPARE(overrides.Revision(), quint64(1));
        QVERIFY(overrides.Set(0, 3, {0, 1, 2}));
        QVERIFY(overrides.Set(0, 4, {0, 1, 2}));
        QVERIFY(!overrides.Set(0, 2, {0, 1, 2}));
        QVERIFY(!overrides.Set(0, 3, {0, 1}));
        QCOMPARE(overrides.Revision(), quint64(1));
        QVERIFY(overrides.Set(1, 3, {0, 1, 2}));
        QCOMPARE(overrides.Revision(), quint64(2));
        QVERIFY(overrides.Set(0, 3, {2, 1, 0}));
        QCOMPARE(overrides.Revision(), quint64(3));
        QVERIFY(!overrides.Remove(7));
        QCOMPARE(overrides.Revision(), quint64(3));
        QVERIFY(overrides.Remove(0));
        QCOMPARE(overrides.Revision(), quint64(4));
        QVERIFY(!overrides.Remove(0));
        QCOMPARE(overrides.Revision(), quint64(4));
        overrides.Clear();
        QCOMPARE(overrides.Revision(), quint64(5));
        overrides.Clear();
        QCOMPARE(overrides.Revision(), quint64(5));
        QVERIFY(overrides.Set(0, 3, {0, 1, 2}));
        QCOMPARE(overrides.Revision(), quint64(6));
    }
};

QTEST_GUILESS_MAIN(MeshIndexOverridesTests)
#include "mesh_index_overrides_tests.moc"
