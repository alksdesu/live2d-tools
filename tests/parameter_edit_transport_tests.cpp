#include "ui/cmotionpanel.h"
#include <QtTest>
#include <QCheckBox>
#include <QPushButton>
#include <QSlider>
#include <QTimer>
#include <QLineEdit>
#include <QTableView>
#include <QJsonArray>
#include <QToolBar>
#include <memory>
#include <limits>

namespace {
QJsonObject animation()
{
    return {{"version", 1}, {"playhead", .75}, {"loop", false}, {"motion", QJsonObject{
        {"Version", 3}, {"Meta", QJsonObject{{"Duration", 3}, {"Fps", 30}}},
        {"Curves", QJsonArray{QJsonObject{{"Target", "Parameter"}, {"Id", "A"}, {"Segments", QJsonArray{0, 0, 0, 3, 1}}}}},
        {"UserData", QJsonArray{QJsonObject{{"Time", 1}, {"Value", "event"}}}}}}};
}
using Snapshot = CMotionPanel::ParameterEditTransportSnapshot;
}

class ParameterEditTransportTests : public QObject
{
    Q_OBJECT
private slots:
    void playingPauseAndRestoreAreSilentAndReusable()
    {
        CMotionPanel panel; QVERIFY(panel.RestoreTimelineState(animation()));
        auto* play = panel.findChild<QPushButton*>("previewTimeline"); auto* timer = panel.findChild<QTimer*>(); QVERIFY(play && timer);
        play->click(); QVERIFY(timer->isActive());
        Snapshot snapshot; QString error; QVERIFY2(panel.CaptureParameterEditTransport(snapshot, &error), qPrintable(error));
        const double time = panel.TimelineTime(); const auto content = panel.Timeline(); const auto revision = panel.TimelineRevision();
        QSignalSpy frame(&panel, &CMotionPanel::TimelineFrameChanged), preview(&panel, &CMotionPanel::MotionTimelinePreviewChanged);
        QSignalSpy events(&panel, &CMotionPanel::TimelineEventsTriggered), changed(&panel, &CMotionPanel::MotionTimelineChanged);
        QSignalSpy activated(&panel, &CMotionPanel::TimelineActivated), stopped(&panel, &CMotionPanel::TimelinePreviewStopped);
        for (int n = 0; n < 2; ++n) {
            QVERIFY(panel.PauseParameterEditTransport(snapshot, &error)); QVERIFY(!timer->isActive());
            QTest::qWait(35); QCOMPARE(panel.TimelineTime(), time);
            QVERIFY(panel.ValidateParameterEditTransport(snapshot, &error));
            QVERIFY(panel.RestoreParameterEditTransport(snapshot, &error)); QVERIFY(timer->isActive());
            QCOMPARE(panel.TimelineTime(), time); QCOMPARE(panel.TimelineRevision(), revision); QVERIFY(panel.Timeline().HasSameContent(content));
        }
        QCOMPARE(frame.size(), 0); QCOMPARE(preview.size(), 0); QCOMPARE(events.size(), 0); QCOMPARE(changed.size(), 0);
        QCOMPARE(activated.size(), 0); QCOMPARE(stopped.size(), 0);
        QVERIFY(panel.PauseParameterEditTransport(snapshot)); snapshot = {}; QVERIFY(!timer->isActive());
    }

    void staleStateCannotRestoreOrChangeTransport_data()
    {
        QTest::addColumn<int>("kind");
        QTest::newRow("content") << 0; QTest::newRow("loop") << 1; QTest::newRow("speed") << 2;
        QTest::newRow("history-generation") << 3; QTest::newRow("record-cycle") << 4; QTest::newRow("explicit-stop") << 5;
    }
    void staleStateCannotRestoreOrChangeTransport()
    {
        QFETCH(int, kind); CMotionPanel panel; QVERIFY(panel.RestoreTimelineState(animation()));
        Snapshot snapshot; QVERIFY(panel.CaptureParameterEditTransport(snapshot)); QVERIFY(panel.PauseParameterEditTransport(snapshot));
        if (kind == 0) QVERIFY(panel.AddTimelinePoint({CMotionTarget::Parameter, "A"}, 4, .5));
        if (kind == 1) panel.findChild<QCheckBox*>("motionLoop")->setChecked(!panel.TimelineLoop());
        if (kind == 2) panel.findChild<QSlider*>("motionPlaybackSpeed")->setValue(120);
        if (kind == 3) QVERIFY(panel.RestoreTimelineState(animation()));
        if (kind == 4) { auto* record = panel.findChild<QCheckBox*>("recordTimeline"); record->setChecked(true); record->setChecked(false); }
        if (kind == 5) panel.StopTimelinePreview();
        const auto before = panel.Timeline(); const auto time = panel.TimelineTime(); const auto revision = panel.TimelineRevision();
        QString error; QVERIFY(!panel.ValidateParameterEditTransport(snapshot, &error)); QVERIFY(!error.isEmpty());
        QVERIFY(!panel.RestoreParameterEditTransport(snapshot, &error)); QVERIFY(panel.Timeline().HasSameContent(before));
        QCOMPARE(panel.TimelineTime(), time); QCOMPARE(panel.TimelineRevision(), revision);
    }

    void recordingClockAndNewPointsSurviveContextRestoration()
    {
        CMotionPanel panel; panel.SetParameterIds({"A", "B"});
        panel.findChild<QCheckBox*>("recordTimeline")->setChecked(true); QVERIFY(panel.Timeline().IsRecording());
        Snapshot snapshot; QVERIFY(panel.CaptureParameterEditTransport(snapshot)); QVERIFY(panel.PauseParameterEditTransport(snapshot));
        QVERIFY(!panel.Timeline().IsRecordingClockPaused());
        QSignalSpy frames(&panel, &CMotionPanel::TimelineFrameChanged);
        panel.RecordParameterValues({{"A", .2f}, {"B", .8f}}); QTest::qWait(35);
        panel.RecordParameterValues({{"A", .4f}, {"B", .6f}});
        const auto before = panel.Timeline(); const auto a = before.Tracks().value({CMotionTarget::Parameter, "A"});
        const auto b = before.Tracks().value({CMotionTarget::Parameter, "B"});
        QCOMPARE(a.size(), 2); QCOMPARE(b.size(), 2); QCOMPARE(a[0].x(), b[0].x()); QCOMPARE(a[1].x(), b[1].x()); QVERIFY(a[1].x() > a[0].x());
        QString error; QVERIFY2(panel.ValidateParameterEditTransport(snapshot, &error), qPrintable(error));
        QVERIFY(panel.RestoreParameterEditTransport(snapshot, &error)); QVERIFY(panel.Timeline().HasSameContent(before));
        QVERIFY(panel.Timeline().IsRecording()); QVERIFY(!panel.Timeline().IsRecordingClockPaused()); QCOMPARE(frames.size(), 0);
        panel.findChild<QCheckBox*>("recordTimeline")->setChecked(false);
        QVERIFY(!panel.ValidateParameterEditTransport(snapshot)); panel.UndoTimeline(); QCOMPARE(panel.Timeline().TotalPointCount(), 0);
    }

    void invalidRecordBatchKeepsTimelineAndSnapshotValid()
    {
        CMotionPanel panel; panel.SetParameterIds({"A", "B"}); panel.findChild<QCheckBox*>("recordTimeline")->setChecked(true);
        Snapshot snapshot; QVERIFY(panel.CaptureParameterEditTransport(snapshot)); const auto before = panel.Timeline();
        panel.RecordParameterValues({{"A", 1}, {"missing", 2}}); QVERIFY(panel.Timeline().HasSameContent(before));
        panel.RecordParameterValues({{"A", 1}, {"B", std::numeric_limits<float>::infinity()}}); QVERIFY(panel.Timeline().HasSameContent(before));
        QVERIFY(panel.ValidateParameterEditTransport(snapshot)); QVERIFY(panel.RestoreParameterEditTransport(snapshot));
    }

    void parameterGuardRunsBeforeMutationAndRestoresRejectedControls_data()
    {
        QTest::addColumn<int>("action"); QTest::addColumn<bool>("accept");
        for (bool accept : {false, true}) for (int action = 0; action < 8; ++action)
            QTest::newRow(qPrintable(QString("%1-%2").arg(accept ? "accept" : "reject").arg(action))) << action << accept;
    }
    void parameterGuardRunsBeforeMutationAndRestoresRejectedControls()
    {
        QFETCH(int, action); QFETCH(bool, accept);
        CMotionPanel panel; QVERIFY(panel.RestoreTimelineState(animation()));
        Snapshot snapshot; QVERIFY(panel.CaptureParameterEditTransport(snapshot)); QVERIFY(panel.PauseParameterEditTransport(snapshot));
        const auto before = panel.Timeline(); const auto revision = panel.TimelineRevision(); const auto time = panel.TimelineTime();
        auto* speed = panel.findChild<QSlider*>("motionPlaybackSpeed"); auto* playhead = panel.findChild<QSlider*>("motionTimelinePlayhead");
        auto* loop = panel.findChild<QCheckBox*>("motionLoop"); auto* record = panel.findChild<QCheckBox*>("recordTimeline");
        auto* fades = panel.findChild<QCheckBox*>("previewMotionFades");
        const int oldSpeed = speed->value(), oldPlayhead = playhead->value(); const bool oldLoop = loop->isChecked(), oldFades = fades->isChecked();
        int calls = 0; bool validatedBeforeMutation = false;
        panel.SetParameterEditGuard([&](QString* error) {
            ++calls; validatedBeforeMutation = panel.ValidateParameterEditTransport(snapshot, error);
            if (!accept && error) *error = "parameter gesture refused";
            return validatedBeforeMutation && accept;
        });
        if (action == 0) QCOMPARE(panel.AddTimelinePoint({CMotionTarget::Parameter, "B"}, 1, .3), accept);
        if (action == 1) loop->setChecked(!oldLoop);
        if (action == 2) speed->setValue(120);
        if (action == 3) record->setChecked(true);
        if (action == 4) fades->setChecked(!oldFades);
        if (action == 5) playhead->setValue(700);
        if (action == 6) panel.findChild<QPushButton*>("previewTimeline")->click();
        if (action == 7) panel.StopTimelinePreview();
        QCOMPARE(calls, 1); QVERIFY(validatedBeforeMutation);
        if (!accept) {
            QVERIFY(panel.Timeline().HasSameContent(before)); QCOMPARE(panel.TimelineRevision(), revision); QCOMPARE(panel.TimelineTime(), time);
            QCOMPARE(speed->value(), oldSpeed); QCOMPARE(playhead->value(), oldPlayhead); QCOMPARE(loop->isChecked(), oldLoop); QCOMPARE(fades->isChecked(), oldFades);
            QVERIFY(!record->isChecked()); QVERIFY(!panel.Timeline().IsRecording());
            QVERIFY(panel.findChild<QPushButton*>("addTimelineKeyframe")->isEnabled()); QVERIFY(panel.findChild<QToolBar*>("motionKeyframeTools")->isEnabled());
            QVERIFY(panel.ValidateParameterEditTransport(snapshot));
        }
    }

    void recordingAndInternalTransportDoNotReenterParameterGuard()
    {
        CMotionPanel panel; panel.SetParameterIds({"A", "B"}); panel.findChild<QCheckBox*>("recordTimeline")->setChecked(true);
        Snapshot snapshot; QVERIFY(panel.CaptureParameterEditTransport(snapshot));
        int calls = 0; panel.SetParameterEditGuard([&](QString*) { ++calls; return false; });
        QVERIFY(panel.PauseParameterEditTransport(snapshot)); panel.RecordParameterValues({{"A", 1}, {"B", 2}});
        QCOMPARE(panel.Timeline().TotalPointCount(), 2); QVERIFY(panel.RestoreParameterEditTransport(snapshot));
        QCOMPARE(calls, 0); QVERIFY(panel.Timeline().IsRecording()); QVERIFY(!panel.Timeline().IsRecordingClockPaused());
    }

    void editorCreationChecksGuardBeforePendingTextExists()
    {
        CMotionPanel panel; QVERIFY(panel.RestoreTimelineState(animation())); panel.resize(900, 650); panel.show();
        Snapshot snapshot; QVERIFY(panel.CaptureParameterEditTransport(snapshot));
        int calls = 0; bool accept = false; bool valid = false;
        panel.SetParameterEditGuard([&](QString* error) { ++calls; valid = panel.ValidateParameterEditTransport(snapshot, error); return accept && valid; });
        auto* table = panel.findChild<QTableView*>("motionTimelineTable");
        table->edit(table->model()->index(1, 2)); QVERIFY(!table->findChild<QLineEdit*>()); QCOMPARE(calls, 1); QVERIFY(valid);
        accept = true; table->edit(table->model()->index(1, 2)); QVERIFY(table->findChild<QLineEdit*>()); QCOMPARE(calls, 2); QVERIFY(valid);
    }

    void parameterGuardRejectsReentryAndHandlesOwnerDestruction()
    {
        CMotionPanel panel; QVERIFY(panel.RestoreTimelineState(animation())); int calls = 0; bool reentered = true;
        panel.SetParameterEditGuard([&](QString*) { ++calls; reentered = panel.AddTimelinePoint({CMotionTarget::Parameter, "B"}, 1, .4); return true; });
        QVERIFY(panel.AddTimelinePoint({CMotionTarget::Parameter, "A"}, 2, .4)); QVERIFY(!reentered); QCOMPARE(calls, 1);
        auto* destroyed = new CMotionPanel; QVERIFY(destroyed->RestoreTimelineState(animation())); QPointer<CMotionPanel> alive(destroyed);
        destroyed->SetParameterEditGuard([destroyed](QString*) { delete destroyed; return true; });
        QVERIFY(!destroyed->AddTimelinePoint({CMotionTarget::Parameter, "A"}, 2, .4)); QVERIFY(alive.isNull());
    }

    void snapshotsAreBoundToOwnerAndRejectPendingEditor()
    {
        auto owner = std::make_unique<CMotionPanel>(); QVERIFY(owner->RestoreTimelineState(animation()));
        Snapshot snapshot; QVERIFY(owner->CaptureParameterEditTransport(snapshot)); QVERIFY(snapshot.IsValid());
        CMotionPanel other; QVERIFY(other.RestoreTimelineState(animation())); QVERIFY(!other.ValidateParameterEditTransport(snapshot));
        owner->resize(900, 650); owner->show(); auto* table = owner->findChild<QTableView*>("motionTimelineTable");
        table->edit(table->model()->index(1, 2)); auto* input = table->findChild<QLineEdit*>(); QVERIFY(input); input->setText("invalid");
        Snapshot preserved = snapshot; QVERIFY(!owner->CaptureParameterEditTransport(snapshot)); QVERIFY(snapshot.IsValid());
        QVERIFY(!owner->ValidateParameterEditTransport(preserved)); QCOMPARE(input->text(), QString("invalid"));
        owner.reset(); QVERIFY(!snapshot.IsValid());
    }
};
QTEST_MAIN(ParameterEditTransportTests)
#include "parameter_edit_transport_tests.moc"
