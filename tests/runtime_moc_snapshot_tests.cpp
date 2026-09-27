#include "live2dstruct/cmocruntimesnapshot.h"
#include "live2dstruct/cmocdeformerorder.h"
#include "deformer_snapshot_fixture.h"
#include "deformer_order_fixture.h"

#include <QtTest>
#include <QCryptographicHash>
#include <type_traits>
#include <limits>

using namespace live2dstruct;

class MocRuntimeSnapshotTests : public QObject
{
    Q_OBJECT
private slots:
    void emptyHandleHasNoBytesOrDigest()
    {
        static_assert(!std::is_aggregate_v<CMocRuntimeSnapshot>);
        const CMocRuntimeSnapshot snapshot;
        QVERIFY(!snapshot.IsValid());
        QVERIFY(snapshot.Bytes().isEmpty());
        QVERIFY(snapshot.Digest().isEmpty());
    }

    void freezesBorrowedBytesAndPreservesCopies()
    {
        DeformerSnapshotData::Fixture fixture;
        QByteArray expected;
        QString error;
        QVERIFY2(CMocDeformerOrder::Normalize(fixture.bytes, expected, &error), qPrintable(error));
        QByteArray storage(fixture.bytes.constData(), fixture.bytes.size());
        const auto borrowed = QByteArray::fromRawData(storage.constData(), storage.size());
        CMocRuntimeSnapshot snapshot;
        QVERIFY2(CMocRuntimeSnapshot::Create(borrowed, snapshot, &error), qPrintable(error));
        QVERIFY(error.isEmpty());
        const auto copied = snapshot;
        storage.fill('\0');
        storage.clear();
        QCOMPARE(snapshot.Bytes(), expected);
        QCOMPARE(snapshot.Digest(), QCryptographicHash::hash(expected, QCryptographicHash::Sha256));
        auto returnedBytes = snapshot.Bytes();
        auto returnedDigest = snapshot.Digest();
        returnedBytes.fill('\0');
        returnedDigest.fill('\0');
        snapshot = {};
        QVERIFY(!snapshot.IsValid());
        QVERIFY(copied.IsValid());
        QCOMPARE(copied.Bytes(), expected);
        QCOMPARE(copied.Digest(), QCryptographicHash::hash(expected, QCryptographicHash::Sha256));
    }

    void normalizesNonTopologicalDeformersWithoutChangingSource()
    {
        DeformerSnapshotData::Fixture fixture;
        const auto source = DeformerOrderFixture::permute(fixture.bytes, {2, 1, 0});
        QVERIFY(!source.isEmpty());
        const auto original = source;
        CMocRuntimeSnapshot snapshot;
        QString error;
        QVERIFY2(CMocRuntimeSnapshot::Create(source, snapshot, &error), qPrintable(error));
        QVERIFY(snapshot.Bytes() != source);
        QCOMPARE(source, original);
        QVERIFY2(DeformerOrderFixture::semanticDifference(source, snapshot.Bytes()).isEmpty(),
                 qPrintable(DeformerOrderFixture::semanticDifference(source, snapshot.Bytes())));
        QCOMPARE(snapshot.Digest(), QCryptographicHash::hash(snapshot.Bytes(), QCryptographicHash::Sha256));
        QVERIFY(snapshot.Digest() != QCryptographicHash::hash(source, QCryptographicHash::Sha256));
        const auto bytes = snapshot.Bytes(), digest = snapshot.Digest();
        QVERIFY2(CMocRuntimeSnapshot::Create(snapshot.Bytes(), snapshot, &error), qPrintable(error));
        QCOMPARE(snapshot.Bytes(), bytes);
        QCOMPARE(snapshot.Digest(), digest);
    }

    void failurePreservesPreviouslyAcceptedSnapshot_data()
    {
        QTest::addColumn<QString>("invalid");
        for (const char* name : {"empty", "truncated", "parent", "cycle", "oversized"})
            QTest::newRow(name) << QString::fromLatin1(name);
    }

    void failurePreservesPreviouslyAcceptedSnapshot()
    {
        QFETCH(QString, invalid);
        DeformerSnapshotData::Fixture fixture;
        CMocRuntimeSnapshot snapshot;
        QString error;
        QVERIFY2(CMocRuntimeSnapshot::Create(fixture.bytes, snapshot, &error), qPrintable(error));
        const auto original = snapshot;
        QByteArray bytes = fixture.bytes;
        if (invalid == "empty") bytes.clear();
        if (invalid == "truncated") bytes = "MOC3";
        if (invalid == "parent") DeformerSnapshotData::put(bytes,
            fixture.table.sectionOffsetTable->deformers->parent_deformer_indices, 99);
        if (invalid == "cycle") DeformerSnapshotData::put(bytes,
            fixture.table.sectionOffsetTable->deformers->parent_deformer_indices, 2);
        const char sentinel = 0;
        if (invalid == "oversized") bytes = QByteArray::fromRawData(&sentinel,
            qsizetype(std::numeric_limits<int>::max()) + 1);
        QVERIFY(!CMocRuntimeSnapshot::Create(bytes, snapshot, &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(snapshot.IsValid());
        QCOMPARE(snapshot.Bytes(), original.Bytes());
        QCOMPARE(snapshot.Digest(), original.Digest());
        QCOMPARE(snapshot.Bytes().constData(), original.Bytes().constData());
    }
};

QTEST_GUILESS_MAIN(MocRuntimeSnapshotTests)
#include "runtime_moc_snapshot_tests.moc"
