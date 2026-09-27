#include "live2dstruct/cmotiontimeline.h"
#include <QtTest>
#include <QJsonArray>
#include <QJsonDocument>

using Values = QMap<QString, float>;
#include <limits>
#include <cmath>

class MotionRecordValuesTests : public QObject
{
    Q_OBJECT
private slots:
    void groupUsesOneClockReadAndSharedCollisionTimestamp()
    {
        qint64 now = 0; int reads = 0; CMotionTimeline timeline([&] { ++reads; return now; });
        QVERIFY(timeline.AddPoint("A", 0, 1)); QVERIFY(timeline.AddPoint("B", 1, 1));
        timeline.StartRecording(); const int startReads = reads;
        QVERIFY(timeline.RecordValues({{"A", 2}, {"B", 3}})); QCOMPARE(reads, startReads + 1);
        auto a = timeline.Tracks().value({CMotionTarget::Parameter, "A"});
        auto b = timeline.Tracks().value({CMotionTarget::Parameter, "B"});
        const double next = std::nextafterf(1.0f, std::numeric_limits<float>::infinity());
        QCOMPARE(a.last().x(), next); QCOMPARE(b.last().x(), next);
        const int count = timeline.TotalPointCount(); QVERIFY(timeline.RecordValues({{"A", 2}, {"B", 3}})); QCOMPARE(timeline.TotalPointCount(), count);
        QVERIFY(timeline.RecordValues({{"A", 2}, {"B", 4}}));
        a = timeline.Tracks().value({CMotionTarget::Parameter, "A"}); b = timeline.Tracks().value({CMotionTarget::Parameter, "B"});
        QCOMPARE(a.last().x(), b.last().x()); QVERIFY(a.last().x() > next); QCOMPARE(a.last().y(), 2.0);
        now = 1000; QVERIFY(timeline.RecordValues({{"A", 5}, {"B", 6}}));
        QCOMPARE(timeline.Tracks().value({CMotionTarget::Parameter, "A"}).last().x(), 2.0);
        QCOMPARE(timeline.Tracks().value({CMotionTarget::Parameter, "B"}).last().x(), 2.0);
    }
    void invalidBatchDoesNotPartiallyAppend_data()
    {
        QTest::addColumn<QMap<QString, float>>("values");
        QTest::newRow("empty") << QMap<QString, float>{};
        QTest::newRow("empty-id") << QMap<QString, float>{{"A", 1}, {"", 2}};
        QTest::newRow("nul-id") << QMap<QString, float>{{"A", 1}, {QString("B") + QChar::Null, 2}};
        QTest::newRow("nan") << QMap<QString, float>{{"A", 1}, {"B", std::numeric_limits<float>::quiet_NaN()}};
        QTest::newRow("infinity") << QMap<QString, float>{{"A", 1}, {"B", std::numeric_limits<float>::infinity()}};
    }
    void invalidBatchDoesNotPartiallyAppend()
    {
        QFETCH(Values, values); qint64 now = 0; CMotionTimeline timeline([&] { return now; });
        QVERIFY(timeline.AddPoint("A", 0, 2)); timeline.StartRecording(); const auto before = timeline;
        QString error; QVERIFY(!timeline.RecordValues(values, &error)); QVERIFY(!error.isEmpty()); QVERIFY(timeline.HasSameContent(before)); QVERIFY(timeline.IsRecording());
    }
    void budgetIncludesEveryBatchMemberAndRetainedTypedTrack()
    {
        CMotionTimeline timeline; CMotionTimeline::BatchEdit edit;
        const CMotionTrack track{CMotionTarget::PartOpacity, "Part"};
        for (int i = 0; i < CMotionTimeline::MaximumPointCount - 1; ++i) edit.insertions.append({track, {double(i), .5}, 0, {}, {}});
        CMotionTimeline::BatchResult result; QVERIFY(timeline.EditPoints(edit, result)); timeline.StartRecording(); const auto before = timeline;
        QString error; QVERIFY(!timeline.RecordValues({{"A", 1}, {"B", 2}}, &error)); QVERIFY(timeline.HasSameContent(before));
        QVERIFY(timeline.RecordValues({{"A", 1}}, &error)); QCOMPARE(timeline.TotalPointCount(), CMotionTimeline::MaximumPointCount);
    }
    void overflowAndClockFailurePreserveAllCurves()
    {
        qint64 now = 0; bool throws = false;
        CMotionTimeline timeline([&]() -> qint64 { if (throws) throw 1; return now; });
        QVERIFY(timeline.AddPoint("A", std::numeric_limits<float>::max(), 1)); timeline.StartRecording(); auto before = timeline;
        QVERIFY(!timeline.RecordValues({{"A", 2}, {"B", 3}})); QVERIFY(timeline.HasSameContent(before));
        timeline.Clear(); timeline.StartRecording(); now = 100; QVERIFY(timeline.RecordValues({{"A", 2}, {"B", 3}})); before = timeline;
        now = 50; QVERIFY(!timeline.RecordValues({{"A", 3}, {"B", 4}})); QVERIFY(timeline.HasSameContent(before));
        throws = true; QVERIFY(!timeline.RecordValues({{"A", 3}, {"B", 4}})); QVERIFY(timeline.HasSameContent(before));
    }
    void recordingPauseAndMetadataArePreserved()
    {
        qint64 now = 0; CMotionTimeline timeline([&] { return now; });
        const QJsonObject motion{{"Version", 3}, {"Extra", "keep"}, {"Meta", QJsonObject{{"Fps", 24}, {"Duration", 3}, {"FadeInTime", .4}}},
            {"Curves", QJsonArray{QJsonObject{{"Target", "Model"}, {"Id", "Opacity"}, {"Segments", QJsonArray{0, 1, 0, 3, .5}}, {"FadeOutTime", .7}}}},
            {"UserData", QJsonArray{QJsonObject{{"Time", 2}, {"Value", "event"}}}}};
        QVERIFY(timeline.RestoreState({{"version", 1}, {"playhead", 0}, {"motion", motion}}));
        const auto events = timeline.Events(); const auto model = timeline.Tracks().value({CMotionTarget::Model, "Opacity"});
        QVERIFY(!timeline.RecordValues({{"A", 1}})); timeline.StartRecording(); now = 100;
        QVERIFY(timeline.PauseRecordingClock()); QVERIFY(!timeline.RecordValues({{"A", 1}, {"B", 2}})); now = 1100; QVERIFY(timeline.ResumeRecordingClock());
        now = 1200; QVERIFY(timeline.RecordValues({{"A", 1}, {"B", 2}})); QCOMPARE(timeline.Events(), events);
        QCOMPARE(timeline.Tracks().value({CMotionTarget::Model, "Opacity"}), model);
        QByteArray bytes; QVERIFY(timeline.ToJson(bytes, timeline.Fps(), false));
        const auto output = QJsonDocument::fromJson(bytes).object(); QCOMPARE(output.value("Extra").toString(), QString("keep"));
        QCOMPARE(output.value("Meta").toObject().value("FadeInTime").toDouble(), .4);
        QCOMPARE(timeline.Tracks().value({CMotionTarget::Parameter, "A"}).last().x(), double(float(3.2)));
    }
};
QTEST_GUILESS_MAIN(MotionRecordValuesTests)
#include "motion_record_values_tests.moc"
