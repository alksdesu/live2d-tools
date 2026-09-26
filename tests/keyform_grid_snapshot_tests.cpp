#include "live2dstruct/cmodeldocument.h"
#include <QtTest>

using namespace live2dstruct;

class KeyformGridSnapshotTests : public QObject
{
    Q_OBJECT
private slots:
    void realObjectDescriptionsMatchAllPoses_data()
    {
        QTest::addColumn<QString>("model");
        QTest::addColumn<QString>("id");
        QTest::addColumn<QString>("type");
        QTest::addColumn<int>("axisCount");
        QTest::newRow("base") << QString("Mark") << QString("Hair2") << QString("artmesh") << 0;
        QTest::newRow("one-axis") << QString("Mark") << QString("Rotation8") << QString("rotation") << 1;
        QTest::newRow("mesh") << QString("Mark") << QString("EyeL4") << QString("artmesh") << 2;
        QTest::newRow("warp") << QString("Mark") << QString("Warp6") << QString("warp") << 2;
        QTest::newRow("rotation") << QString("Natori") << QString("Rotation2") << QString("rotation") << 2;
        QTest::newRow("opposite-axis-order") << QString("Wanko") << QString("B_EAR_01") << QString("warp") << 2;
        QTest::newRow("three-axes") << QString("Wanko") << QString("D_EFFECT_05") << QString("artmesh") << 3;
    }

    void realObjectDescriptionsMatchAllPoses()
    {
        QFETCH(QString, model);
        QFETCH(QString, id);
        QFETCH(QString, type);
        QFETCH(int, axisCount);
        CModelDocument document;
        QString error;
        QVERIFY2(document.Load(QString(TEST_RESOURCES) + '/' + model + '/' + model + ".moc3", &error), qPrintable(error));
        const auto revision = document.Revision();
        CKeyformGridSnapshot snapshot;
        QVERIFY2(document.DescribeKeyformGrid(id, type, snapshot, &error), qPrintable(error));
        QVERIFY(snapshot.axesValid && snapshot.diagnostic.isEmpty());
        QCOMPARE(snapshot.axes.size(), axisCount);
        QCOMPARE(snapshot.objectId, id);
        QCOMPARE(snapshot.objectType, type);
        QCOMPARE(snapshot.sourceDigest, document.SourceDigest());
        QCOMPARE(snapshot.revision, revision);
        qint64 product = 1;
        for (const auto& axis : snapshot.axes) {
            product *= axis.keys.size();
            QCOMPARE(axis.displayName, document.ParameterMetadata(axis.parameterId).displayName);
        }
        QCOMPARE(snapshot.expectedKeyformCount, product);
        QCOMPARE(snapshot.keyformCount, product);
        for (int keyform = 0; keyform < snapshot.keyformCount; ++keyform) {
            QMap<QString, float> fromDocument, fromSnapshot, expected;
            QVERIFY2(document.KeyformParameterValues(id, type, keyform, fromDocument, &error), qPrintable(error));
            QVERIFY2(snapshot.Pose(keyform, fromSnapshot, &error), qPrintable(error));
            int remainder = keyform;
            for (const auto& axis : snapshot.axes) {
                expected[axis.parameterId] = axis.keys[remainder % axis.keys.size()];
                remainder /= axis.keys.size();
            }
            QCOMPARE(fromSnapshot, fromDocument);
            QCOMPARE(fromSnapshot, expected);
        }
        QCOMPARE(document.Revision(), revision);
        QVERIFY(!document.CanUndoEdit());
    }

    void snapshotOwnsItsBindingAndRangeData()
    {
        CModelDocument document;
        QString error;
        QVERIFY(document.Load(QString(TEST_RESOURCES) + "/Mark/Mark.moc3"));
        CKeyformGridSnapshot old;
        QVERIFY(document.DescribeKeyformGrid("EyeL4", "artmesh", old));
        auto metadata = document.ParameterMetadata("ParamEyeBallX");
        metadata.displayName = QStringLiteral("眼球水平");
        metadata.minimum = 0;
        QVERIFY(document.SetParameterMetadata("ParamEyeBallX", metadata));
        CKeyformGridSnapshot current;
        QVERIFY(document.DescribeKeyformGrid("EyeL4", "artmesh", current));
        QCOMPARE(current.axes.first().displayName, metadata.displayName);
        QCOMPARE(current.axes.first().minimum, 0.0f);
        QCOMPARE(old.axes.first().minimum, -1.0f);
        QVERIFY(current.revision > old.revision);
        QMap<QString, float> pose{{"unchanged", 7}};
        QVERIFY(!current.Pose(0, pose, &error));
        QVERIFY(error.contains("ParamEyeBallX"));
        QCOMPARE(pose, (QMap<QString, float>{{"unchanged", 7}}));
        QVERIFY(old.Pose(0, pose, &error));
        QCOMPARE(pose.value("ParamEyeBallX"), -1.0f);
        QVERIFY(current.Pose(2, pose, &error));
        QCOMPARE(pose.value("ParamEyeBallX"), 1.0f);
        document.Clear();
        QVERIFY(old.Pose(0, pose, &error));
        QVERIFY(current.Pose(2, pose, &error));
        QVERIFY(!current.Pose(0, pose, &error));
    }

    void invalidRequestsDoNotReplaceSnapshotsOrPoses()
    {
        CModelDocument document;
        QVERIFY(document.Load(QString(TEST_RESOURCES) + "/Mark/Mark.moc3"));
        CKeyformGridSnapshot snapshot;
        QVERIFY(document.DescribeKeyformGrid("EyeL4", "artmesh", snapshot));
        const auto revision = snapshot.revision;
        for (const auto& target : {qMakePair(QString("missing"), QString("artmesh")),
                                  qMakePair(QString("EyeL4"), QString("rotation")),
                                  qMakePair(QString("EyeL4"), QString("part"))}) {
            QString error;
            QVERIFY(!document.DescribeKeyformGrid(target.first, target.second, snapshot, &error));
            QVERIFY(!error.isEmpty());
            QCOMPARE(snapshot.objectId, QString("EyeL4"));
            QCOMPARE(snapshot.revision, revision);
        }
        QMap<QString, float> pose{{"retained", 1}};
        for (int invalid : {-1, snapshot.keyformCount}) {
            QVERIFY(!snapshot.Pose(invalid, pose));
            QCOMPARE(pose, (QMap<QString, float>{{"retained", 1}}));
        }
    }
};

QTEST_GUILESS_MAIN(KeyformGridSnapshotTests)
#include "keyform_grid_snapshot_tests.moc"
