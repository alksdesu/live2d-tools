#include "ui/cmotionpanel.h"
#include "ui/cmotioncurveview.h"
#include "ui/cmotionsettingsdialog.h"
#include "ui/theme.h"
#include "live2dstruct/cmodelsession.h"
#include <QtTest>
#include <QApplication>
#include <QCryptographicHash>
#include <QJsonArray>
#include <QInputDialog>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalSpy>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QSplitter>
#include <QTabWidget>
#include <QScrollArea>
#include <QScrollBar>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QScopeGuard>
#include <QPointer>
#include <QUndoStack>
#include <QUndoCommand>
#include <type_traits>
#include <memory>
#include <functional>

namespace {
class RefuseEnabledRestore : public QObject
{
public:
    bool armed = true;
    int calls = 0;
    std::function<void()> beforeRefusal;
protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        auto* widget = qobject_cast<QWidget*>(watched);
        if (armed && widget && event->type() == QEvent::EnabledChange && widget->isEnabled()) {
            armed = false;
            ++calls;
            if (beforeRefusal) beforeRefusal();
            widget->setEnabled(false);
        }
        return false;
    }
};
}

class MotionPanelTests : public QObject
{
    Q_OBJECT
private:
    static bool makeClip(const QString& path, float value = 1.0f)
    {
        CMotionTimeline timeline;
        timeline.AddPoint("ParamA", 0, 0);
        timeline.AddPoint("ParamA", 1, value);
        return timeline.Save(path, 60, false);
    }

    static void answer(QMessageBox::StandardButton choice)
    {
        QTimer::singleShot(0, [choice](){
            auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
            QVERIFY(box);
            QVERIFY(box->button(choice));
            box->button(choice)->click();
        });
    }

    static QJsonObject settingsState()
    {
        QJsonArray curves;
        for (const QString& target : {QStringLiteral("Model"), QStringLiteral("Parameter"), QStringLiteral("PartOpacity")})
            curves.append(QJsonObject{{"Target", target}, {"Id", "Opacity"}, {"Segments", QJsonArray{0, 0, 0, 1, 1}},
                {"FadeInTime", 0.123456789012345}, {"FadeOutTime", -1}, {"Extension", target}});
        const QJsonObject motion{{"Version", 3}, {"Curves", curves}, {"RootExtension", "keep"},
            {"Meta", QJsonObject{{"Fps", 59.940000123456}, {"Duration", 3.000000123456},
                {"FadeInTime", 0.987654321012345}, {"FadeOutTime", -1}, {"Loop", false}, {"MetaExtension", true}}},
            {"UserData", QJsonArray{QJsonObject{{"Time", 0.123456789012345}, {"Value", QStringLiteral("原始事件")},
                {"Extension", QJsonObject{{"payload", 7}}}}}}};
        return QJsonObject{{"version", 1}, {"motion", motion}, {"playhead", 0}};
    }

    static int trackIndex(const QComboBox* combo, const CMotionTrack& track)
    {
        for (int i = 0; i < combo->count(); ++i)
            if (combo->itemData(i).value<CMotionTrack>() == track) return i;
        return -1;
    }

    static QJsonObject eventState(const QJsonArray& events, double duration, bool loop = false)
    {
        return QJsonObject{{"version", 1}, {"playhead", 0}, {"motion", QJsonObject{
            {"Version", 3}, {"Curves", QJsonArray{}}, {"UserData", events},
            {"Meta", QJsonObject{{"Fps", 30}, {"Duration", duration}, {"Loop", loop}}}}}};
    }

    static QVector<CMotionEventOccurrence> occurrences(const QSignalSpy& spy)
    {
        QVector<CMotionEventOccurrence> result;
        for (const auto& arguments : spy)
            result += arguments.first().value<CMotionEventBatch>().occurrences;
        return result;
    }

private slots:
    void parameterTracksCommitIsSilentAndPreservesTypedPeers()
    {
        CMotionPanel panel;
        QVERIFY(panel.RestoreTimelineState(settingsState()));
        QVERIFY(panel.AddTimelinePoint({CMotionTarget::Parameter, "Opacity"}, 2, 0.7f));
        panel.MarkTimelineSaved();
        auto* history = panel.findChild<QUndoStack*>(); QVERIFY(history);
        const int count = history->count(), index = history->index();
        const QString label = history->undoText();
        const auto original = panel.Timeline();
        const auto revision = panel.TimelineRevision();
        const double time = panel.TimelineTime();
        QSignalSpy frame(&panel, &CMotionPanel::TimelineFrameChanged);
        QSignalSpy stopped(&panel, &CMotionPanel::TimelinePreviewStopped);
        QSignalSpy changed(&panel, &CMotionPanel::MotionTimelineChanged);
        QSignalSpy historySignal(&panel, &CMotionPanel::TimelineHistoryChanged);
        QSignalSpy activated(&panel, &CMotionPanel::TimelineActivated);
        CMotionPanel::PreparedParameterTracks prepared;
        QString error;
        QVERIFY2(panel.PrepareParameterTrackRemoval({"Opacity"}, prepared, &error), qPrintable(error));
        QVERIFY(prepared.IsValid() && prepared.HasChanges() && prepared.Timeline());
        QVERIFY(panel.Timeline().HasSameContent(original));
        QVERIFY(!prepared.Timeline()->Tracks().contains({CMotionTarget::Parameter, "Opacity"}));
        QCOMPARE(prepared.Timeline()->Tracks().value({CMotionTarget::PartOpacity, "Opacity"}),
                 original.Tracks().value({CMotionTarget::PartOpacity, "Opacity"}));
        QCOMPARE(prepared.Timeline()->Tracks().value({CMotionTarget::Model, "Opacity"}),
                 original.Tracks().value({CMotionTarget::Model, "Opacity"}));
        QCOMPARE(prepared.Time(), time); QCOMPARE(prepared.Loop(), panel.TimelineLoop());
        QVERIFY(panel.ValidatePreparedParameterTracks(prepared, &error));
        CMotionPanel::ParameterTrackArchive archive;
        QVERIFY(panel.CommitPreparedParameterTracks(prepared, archive));
        QVERIFY(archive.IsValid() && !prepared.IsValid());
        QCOMPARE(panel.TimelineRevision(), revision + 1);
        QCOMPARE(history->index(), index); QCOMPARE(history->count(), count); QCOMPARE(history->undoText(), label);
        QVERIFY(panel.HasUnsavedTimelineChanges());
        QCOMPARE(frame.size(), 0); QCOMPARE(stopped.size(), 0); QCOMPARE(changed.size(), 0);
        QCOMPARE(historySignal.size(), 0); QCOMPARE(activated.size(), 0);
        panel.RefreshParameterTrackUi();
        QCOMPARE(changed.size(), 1); QCOMPARE(frame.size(), 0); QCOMPARE(stopped.size(), 0);
        QCOMPARE(activated.size(), 0); QCOMPARE(historySignal.size(), 1);
        panel.RefreshParameterTrackUi(); QCOMPARE(changed.size(), 1);
        QVERIFY(panel.PrepareParameterTrackRestoration(archive, prepared, &error));
        QVERIFY(panel.CommitPreparedParameterTracks(prepared, archive));
        QVERIFY(panel.Timeline().HasSameContent(original));
        QVERIFY(!panel.HasUnsavedTimelineChanges());
        panel.RefreshParameterTrackUi();
        QCOMPARE(frame.size(), 0); QCOMPARE(stopped.size(), 0);
        QVERIFY(!panel.PrepareParameterTrackRestoration(archive, prepared, &error));
        panel.UndoTimeline();
        QCOMPARE(panel.Timeline().Tracks().value({CMotionTarget::Parameter, "Opacity"}).size(), 2);
        panel.RedoTimeline(); QVERIFY(panel.Timeline().HasSameContent(original));
    }

    void parameterTrackRestorationKeepsIndependentEditsAndOldVersions()
    {
        const CMotionTrack a{CMotionTarget::Parameter, "A"}, b{CMotionTarget::Parameter, "B"};
        CMotionPanel panel;
        QVERIFY(panel.AddTimelinePoint(a, 0, 1));
        QVERIFY(panel.AddTimelinePoint(a, 1, 2));
        panel.MarkTimelineSaved();
        CMotionPanel::PreparedParameterTracks prepared;
        CMotionPanel::ParameterTrackArchive archive;
        QString error;
        QVERIFY(panel.PrepareParameterTrackRemoval({"A"}, prepared, &error));
        QVERIFY(panel.CommitPreparedParameterTracks(prepared, archive)); panel.RefreshParameterTrackUi();
        QVERIFY(panel.AddTimelinePoint(b, 0, 8));
        QVERIFY(panel.AddTimelinePoint(b, 1, 9));
        QVERIFY2(panel.PrepareParameterTrackRestoration(archive, prepared, &error), qPrintable(error));
        QVERIFY(panel.CommitPreparedParameterTracks(prepared, archive)); panel.RefreshParameterTrackUi();
        QCOMPARE(panel.Timeline().Tracks().value(a), QVector<QPointF>({{0, 1}, {1, 2}}));
        QCOMPARE(panel.Timeline().Tracks().value(b), QVector<QPointF>({{0, 8}, {1, 9}}));
        panel.UndoTimeline();
        QCOMPARE(panel.Timeline().Tracks().value(a).size(), 2);
        QCOMPARE(panel.Timeline().Tracks().value(b), QVector<QPointF>({{0, 8}}));
        panel.UndoTimeline();
        QVERIFY(!panel.Timeline().Tracks().contains(b)); QVERIFY(!panel.HasUnsavedTimelineChanges());
        panel.UndoTimeline(); QCOMPARE(panel.Timeline().Tracks().value(a), QVector<QPointF>({{0, 1}}));
        panel.UndoTimeline(); QVERIFY(!panel.Timeline().Tracks().contains(a));
        panel.RedoTimeline(); panel.RedoTimeline(); panel.RedoTimeline(); panel.RedoTimeline();
        QCOMPARE(panel.Timeline().Tracks().value(a).size(), 2); QCOMPARE(panel.Timeline().Tracks().value(b).size(), 2);
        QVERIFY(panel.AddTimelinePoint(a, 2, 3));
        QVERIFY(panel.PrepareParameterTrackRemoval({"A"}, prepared, &error));
        QVERIFY(panel.CommitPreparedParameterTracks(prepared, archive));
        QVERIFY(panel.PrepareParameterTrackRestoration(archive, prepared, &error));
        QVERIFY(panel.CommitPreparedParameterTracks(prepared, archive));
        QCOMPARE(panel.Timeline().Tracks().value(a), QVector<QPointF>({{0, 1}, {1, 2}, {2, 3}}));
    }

    void hiddenTrackVersionsFollowHistoryBeforeNewEdits()
    {
        const CMotionTrack a{CMotionTarget::Parameter, "A"}, b{CMotionTarget::Parameter, "B"};
        CMotionPanel panel;
        QVERIFY(panel.AddTimelinePoint(a, 0, 1)); QVERIFY(panel.AddTimelinePoint(a, 1, 2));
        CMotionPanel::PreparedParameterTracks prepared; CMotionPanel::ParameterTrackArchive archive;
        QVERIFY(panel.PrepareParameterTrackRemoval({"A"}, prepared));
        QVERIFY(panel.CommitPreparedParameterTracks(prepared, archive)); panel.RefreshParameterTrackUi();
        panel.UndoTimeline();
        QVERIFY(!panel.Timeline().Tracks().contains(a));
        QVERIFY(panel.AddTimelinePoint(b, 0, 7));
        QVERIFY(!panel.CanRedoTimeline());
        QVERIFY(panel.PrepareParameterTrackRestoration(archive, prepared));
        QVERIFY(panel.CommitPreparedParameterTracks(prepared, archive)); panel.RefreshParameterTrackUi();
        QCOMPARE(panel.Timeline().Tracks().value(a), QVector<QPointF>({{0, 1}}));
        panel.UndoTimeline();
        QCOMPARE(panel.Timeline().Tracks().value(a), QVector<QPointF>({{0, 1}}));
        QVERIFY(!panel.Timeline().Tracks().contains(b));
        panel.UndoTimeline(); QVERIFY(!panel.Timeline().Tracks().contains(a));
        panel.RedoTimeline(); panel.RedoTimeline();
        QCOMPARE(panel.Timeline().Tracks().value(a), QVector<QPointF>({{0, 1}}));
        QCOMPARE(panel.Timeline().Tracks().value(b), QVector<QPointF>({{0, 7}}));
    }

    void redoOnlyTrackIsRemovedAndRestoredWithoutMovingHistory()
    {
        CMotionPanel panel;
        const CMotionTrack a{CMotionTarget::Parameter, "A"};
        QVERIFY(panel.AddTimelinePoint(a, 0, 1)); panel.UndoTimeline();
        auto* history = panel.findChild<QUndoStack*>(); QVERIFY(history);
        QCOMPARE(history->index(), 0); QCOMPARE(history->count(), 1);
        CMotionPanel::PreparedParameterTracks prepared; CMotionPanel::ParameterTrackArchive archive;
        QVERIFY(panel.PrepareParameterTrackRemoval({"A"}, prepared)); QVERIFY(prepared.HasChanges());
        QVERIFY(panel.CommitPreparedParameterTracks(prepared, archive)); panel.RefreshParameterTrackUi();
        QCOMPARE(history->index(), 0); QCOMPARE(history->count(), 1);
        panel.RedoTimeline(); QVERIFY(!panel.Timeline().Tracks().contains(a));
        QVERIFY(panel.PrepareParameterTrackRestoration(archive, prepared));
        QVERIFY(panel.CommitPreparedParameterTracks(prepared, archive)); panel.RefreshParameterTrackUi();
        QCOMPARE(panel.Timeline().Tracks().value(a), QVector<QPointF>({{0, 1}}));
        panel.UndoTimeline(); QVERIFY(!panel.Timeline().Tracks().contains(a));
    }

    void recreatedTrackConflictIsAtomic_data()
    {
        QTest::addColumn<bool>("redoOnly");
        QTest::newRow("current") << false;
        QTest::newRow("redo-only") << true;
    }
    void recreatedTrackConflictIsAtomic()
    {
        QFETCH(bool, redoOnly);
        CMotionPanel panel; const CMotionTrack a{CMotionTarget::Parameter, "A"};
        QVERIFY(panel.AddTimelinePoint(a, 0, 1));
        CMotionPanel::PreparedParameterTracks prepared; CMotionPanel::ParameterTrackArchive archive;
        QVERIFY(panel.PrepareParameterTrackRemoval({"A"}, prepared));
        QVERIFY(panel.CommitPreparedParameterTracks(prepared, archive)); panel.RefreshParameterTrackUi();
        QVERIFY(panel.AddTimelinePoint(a, 0, 1));
        if (redoOnly) panel.UndoTimeline();
        const auto before = panel.Timeline();
        const auto revision = panel.TimelineRevision();
        auto* history = panel.findChild<QUndoStack*>(); QVERIFY(history);
        const int index = history->index(), count = history->count();
        const bool dirty = panel.HasUnsavedTimelineChanges();
        QSignalSpy frames(&panel, &CMotionPanel::TimelineFrameChanged);
        QSignalSpy changed(&panel, &CMotionPanel::MotionTimelineChanged);
        QVERIFY(panel.PrepareParameterTrackRemoval({"Unknown"}, prepared));
        QString error;
        QVERIFY(!panel.PrepareParameterTrackRestoration(archive, prepared, &error));
        QVERIFY(error.contains(QStringLiteral("同名"))); QVERIFY(prepared.IsValid());
        QVERIFY(panel.Timeline().HasSameContent(before)); QCOMPARE(panel.TimelineRevision(), revision);
        QCOMPARE(history->index(), index); QCOMPARE(history->count(), count);
        QCOMPARE(panel.HasUnsavedTimelineChanges(), dirty);
        QCOMPARE(frames.size(), 0); QCOMPARE(changed.size(), 0);
    }

    void emptyTrackArchiveAndStalePreparationsRemainSafe()
    {
        CMotionPanel panel, other;
        CMotionPanel::PreparedParameterTracks first, second;
        CMotionPanel::ParameterTrackArchive archive, untouched;
        QVERIFY(panel.PrepareParameterTrackRemoval({"Missing"}, first));
        QVERIFY(first.IsValid() && !first.HasChanges());
        QVERIFY(panel.PrepareParameterTrackRemoval({"Missing"}, second));
        const auto revision = panel.TimelineRevision();
        QVERIFY(panel.CommitPreparedParameterTracks(first, archive));
        QVERIFY(archive.IsValid()); QCOMPARE(panel.TimelineRevision(), revision);
        QVERIFY(!second.IsValid()); QVERIFY(!panel.CommitPreparedParameterTracks(second, untouched));
        QVERIFY(!untouched.IsValid());
        QVERIFY(!other.PrepareParameterTrackRestoration(archive, second));
        QVERIFY(panel.PrepareParameterTrackRestoration(archive, first));
        QVERIFY(!first.HasChanges()); QVERIFY(panel.CommitPreparedParameterTracks(first, archive));
        QVERIFY(!panel.HasUnsavedTimelineChanges());
        QVERIFY(panel.PrepareParameterTrackRemoval({"Missing"}, first));
        QVERIFY(panel.AddTimelinePoint({CMotionTarget::Parameter, "B"}, 0, 1));
        QVERIFY(!panel.ValidatePreparedParameterTracks(first));
        QVERIFY(!panel.CommitPreparedParameterTracks(first, untouched));
        QVERIFY(panel.PrepareParameterTrackRemoval({"B"}, first));
        QVERIFY(panel.CommitPreparedParameterTracks(first, archive));
        QVERIFY(panel.RestoreTimelineState(settingsState()));
        QVERIFY(!archive.IsValid()); QVERIFY(!panel.PrepareParameterTrackRestoration(archive, first));
        auto temporary = std::make_unique<CMotionPanel>();
        QVERIFY(temporary->PrepareParameterTrackRemoval({"A"}, first));
        QVERIFY(temporary->CommitPreparedParameterTracks(first, archive));
        QVERIFY(temporary->PrepareParameterTrackRemoval({"B"}, first));
        temporary.reset();
        QVERIFY(!first.IsValid() && !archive.IsValid());
        QVERIFY(!panel.CommitPreparedParameterTracks(first, untouched));
    }

    void parameterTrackPreparationRejectsSynchronousEditReentry()
    {
        CMotionPanel panel;
        int calls = 0;
        connect(&panel, &CMotionPanel::TimelineActivated, &panel, [&] {
            ++calls;
            CMotionPanel::PreparedParameterTracks prepared;
            QString error;
            QVERIFY(!panel.PrepareParameterTrackRemoval({"A"}, prepared, &error));
            QVERIFY(!error.isEmpty());
        });
        QVERIFY(panel.AddTimelinePoint({CMotionTarget::Parameter, "A"}, 0, 1));
        QVERIFY(calls > 0);
        panel.UndoTimeline(); panel.RedoTimeline();
        auto* loop = panel.findChild<QCheckBox*>("motionLoop"); QVERIFY(loop);
        loop->setChecked(true);
        auto* record = panel.findChild<QCheckBox*>("recordTimeline"); QVERIFY(record);
        record->setChecked(true); record->setChecked(false);
    }

    void parameterTrackCommitKeepsPlaybackAndRecording_data()
    {
        QTest::addColumn<bool>("recording");
        QTest::newRow("playing") << false;
        QTest::newRow("recording") << true;
    }
    void parameterTrackCommitKeepsPlaybackAndRecording()
    {
        QFETCH(bool, recording);
        CMotionPanel panel; const CMotionTrack a{CMotionTarget::Parameter, "A"};
        QVERIFY(panel.AddTimelinePoint(a, 0, 1)); QVERIFY(panel.AddTimelinePoint(a, 10, 2));
        auto* play = panel.findChild<QPushButton*>("previewTimeline");
        auto* head = panel.findChild<QSlider*>("motionTimelinePlayhead");
        auto* speed = panel.findChild<QSlider*>("motionPlaybackSpeed");
        auto* loop = panel.findChild<QCheckBox*>("motionLoop");
        auto* record = panel.findChild<QCheckBox*>("recordTimeline");
        auto* timer = panel.findChild<QTimer*>();
        QVERIFY(play && head && speed && loop && record && timer);
        speed->setValue(150); loop->setChecked(true); head->setValue(400);
        if (recording) record->setChecked(true); else play->click();
        timer->setInterval(71);
        const double time = panel.TimelineTime();
        auto* history = panel.findChild<QUndoStack*>(); QVERIFY(history);
        const int count = history->count(), index = history->index();
        QSignalSpy frames(&panel, &CMotionPanel::TimelineFrameChanged);
        QSignalSpy stopped(&panel, &CMotionPanel::TimelinePreviewStopped);
        QSignalSpy events(&panel, &CMotionPanel::TimelineEventsTriggered);
        CMotionPanel::PreparedParameterTracks prepared; CMotionPanel::ParameterTrackArchive archive;
        QVERIFY(panel.PrepareParameterTrackRemoval({"A"}, prepared));
        QVERIFY(panel.CommitPreparedParameterTracks(prepared, archive)); panel.RefreshParameterTrackUi();
        QCOMPARE(timer->isActive(), !recording); QCOMPARE(timer->interval(), 71);
        QCOMPARE(panel.Timeline().IsRecording(), recording); QCOMPARE(record->isChecked(), recording);
        QCOMPARE(speed->value(), 150); QVERIFY(loop->isChecked()); QCOMPARE(panel.TimelineTime(), time);
        QCOMPARE(history->count(), count); QCOMPARE(history->index(), index);
        QCOMPARE(frames.size(), 0); QCOMPARE(stopped.size(), 0); QCOMPARE(events.size(), 0);
        if (recording) {
            panel.RecordParameterValue("B", 7);
            QVERIFY(panel.Timeline().Tracks().contains({CMotionTarget::Parameter, "B"}));
            record->setChecked(false);
            QVERIFY(panel.PrepareParameterTrackRestoration(archive, prepared));
            QVERIFY(panel.CommitPreparedParameterTracks(prepared, archive)); panel.RefreshParameterTrackUi();
            QVERIFY(panel.Timeline().Tracks().contains(a));
            panel.UndoTimeline();
            QVERIFY(panel.Timeline().Tracks().contains(a));
            QVERIFY(!panel.Timeline().Tracks().contains({CMotionTarget::Parameter, "B"}));
            panel.RedoTimeline();
            QVERIFY(panel.Timeline().Tracks().contains(a));
            QVERIFY(panel.Timeline().Tracks().contains({CMotionTarget::Parameter, "B"}));
        } else {
            QVERIFY(play->isEnabled()); play->click(); QVERIFY(!timer->isActive());
            QVERIFY(panel.PrepareParameterTrackRestoration(archive, prepared));
            QVERIFY(panel.CommitPreparedParameterTracks(prepared, archive)); panel.RefreshParameterTrackUi();
            QVERIFY(!timer->isActive());
        }
    }

    void parameterTrackHistoryHonorsSavedBaselineAndUndoLimit()
    {
        CMotionPanel panel;
        const CMotionTrack a{CMotionTarget::Parameter, "A"}, b{CMotionTarget::Parameter, "B"};
        auto* history = panel.findChild<QUndoStack*>(); QVERIFY(history); history->setUndoLimit(3);
        QVERIFY(panel.AddTimelinePoint(a, 0, 1));
        QVERIFY(panel.AddTimelinePoint(a, 1, 2)); panel.MarkTimelineSaved();
        CMotionPanel::PreparedParameterTracks prepared; CMotionPanel::ParameterTrackArchive archive;
        QVERIFY(panel.PrepareParameterTrackRemoval({"A"}, prepared));
        QVERIFY(panel.CommitPreparedParameterTracks(prepared, archive)); panel.RefreshParameterTrackUi();
        QVERIFY(panel.HasUnsavedTimelineChanges()); panel.MarkTimelineSaved();
        QVERIFY(!panel.HasUnsavedTimelineChanges());
        for (int index = 0; index < 5; ++index) QVERIFY(panel.AddTimelinePoint(b, float(index), float(index + 10)));
        QCOMPARE(history->count(), 3);
        QVERIFY(panel.PrepareParameterTrackRestoration(archive, prepared));
        QVERIFY(panel.CommitPreparedParameterTracks(prepared, archive)); panel.RefreshParameterTrackUi();
        QVERIFY(panel.HasUnsavedTimelineChanges()); QCOMPARE(history->count(), 3);
        for (int index = 0; index < 3; ++index) {
            panel.UndoTimeline();
            QCOMPARE(panel.Timeline().Tracks().value(a), QVector<QPointF>({{0, 1}, {1, 2}}));
        }
        QVERIFY(!panel.CanUndoTimeline());
        QCOMPARE(panel.Timeline().Tracks().value(b), QVector<QPointF>({{0, 10}, {1, 11}}));
        for (int index = 0; index < 3; ++index) panel.RedoTimeline();
        QCOMPARE(panel.Timeline().Tracks().value(b).size(), 5);
    }

    void nestedParameterTrackArchivesRestoreInModelHistoryOrder()
    {
        CMotionPanel panel;
        const CMotionTrack a{CMotionTarget::Parameter, "A"}, b{CMotionTarget::Parameter, "B"};
        QVERIFY(panel.AddTimelinePoint(a, 0, 1)); QVERIFY(panel.AddTimelinePoint(a, 1, 2));
        QVERIFY(panel.AddTimelinePoint(b, 0, 3)); QVERIFY(panel.AddTimelinePoint(b, 2, 4));
        const auto original = panel.Timeline(); panel.MarkTimelineSaved();
        CMotionPanel::PreparedParameterTracks prepared; CMotionPanel::ParameterTrackArchive first, second;
        QVERIFY(panel.PrepareParameterTrackRemoval({"A"}, prepared));
        QVERIFY(panel.CommitPreparedParameterTracks(prepared, first));
        QVERIFY(panel.PrepareParameterTrackRemoval({"B"}, prepared));
        QVERIFY(panel.CommitPreparedParameterTracks(prepared, second)); panel.RefreshParameterTrackUi();
        QVERIFY(panel.Timeline().Tracks().isEmpty());
        QVERIFY(panel.PrepareParameterTrackRestoration(second, prepared));
        QVERIFY(panel.CommitPreparedParameterTracks(prepared, second));
        QVERIFY(panel.PrepareParameterTrackRestoration(first, prepared));
        QVERIFY(panel.CommitPreparedParameterTracks(prepared, first)); panel.RefreshParameterTrackUi();
        QVERIFY(panel.Timeline().HasSameContent(original)); QVERIFY(!panel.HasUnsavedTimelineChanges());
        panel.UndoTimeline(); QCOMPARE(panel.Timeline().Tracks().value(b).size(), 1);
        panel.UndoTimeline(); QVERIFY(!panel.Timeline().Tracks().contains(b));
        QCOMPARE(panel.Timeline().Tracks().value(a).size(), 2);
    }

    void parameterTrackPreparationRejectsPendingEditorAndChangedPlayhead()
    {
        CMotionPanel panel;
        QVERIFY(panel.AddTimelinePoint({CMotionTarget::Parameter, "A"}, 0, 1));
        QVERIFY(panel.AddTimelinePoint({CMotionTarget::Parameter, "A"}, 1, 2));
        panel.resize(900, 700); panel.show(); QTest::qWait(10);
        CMotionPanel::PreparedParameterTracks prepared;
        QVERIFY(panel.PrepareParameterTrackRemoval({"A"}, prepared));
        auto* head = panel.findChild<QSlider*>("motionTimelinePlayhead"); QVERIFY(head);
        head->setValue(500);
        QVERIFY(!panel.ValidatePreparedParameterTracks(prepared));
        CMotionPanel::ParameterTrackArchive archive;
        QVERIFY(!panel.CommitPreparedParameterTracks(prepared, archive));
        QVERIFY(panel.PrepareParameterTrackRemoval({"A"}, prepared));
        auto* table = panel.findChild<QTableWidget*>("motionTimelineTable"); QVERIFY(table);
        QTableWidgetItem* editable = nullptr;
        for (int column = 0; column < table->columnCount(); ++column) {
            auto* item = table->item(0, column);
            if (item && (item->flags() & Qt::ItemIsEditable)) { editable = item; break; }
        }
        QVERIFY(editable); table->editItem(editable);
        auto* editor = table->findChild<QLineEdit*>(); QTRY_VERIFY(editor && editor->isVisible());
        QTest::keyClick(editor, Qt::Key_A, Qt::ControlModifier); QTest::keyClicks(editor, "0.123");
        QVERIFY(!panel.ValidatePreparedParameterTracks(prepared));
        CMotionPanel::PreparedParameterTracks rejected;
        QVERIFY(!panel.PrepareParameterTrackRemoval({"A"}, rejected));
        QTest::keyClick(editor, Qt::Key_Escape);
        QVERIFY(panel.PrepareParameterTrackRemoval({"A"}, rejected));
        QVERIFY(panel.CommitPreparedParameterTracks(rejected, archive)); panel.RefreshParameterTrackUi();
        QVERIFY(!panel.Timeline().Tracks().contains({CMotionTarget::Parameter, "A"}));
    }

    void poseSourceFingerprintTracksBinaryLoadAndSuccessfulSave()
    {
        QTemporaryDir temp; const auto path = temp.filePath("pose.json");
        const QByteArray raw = "{\r\n\"Groups\":[[{\"Id\":\"A\",\"Extra\":1},{\"Id\":\"B\"}]],\r\n\"FadeInTime\":0.25}\r\n";
        QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write(raw), raw.size()); file.close();
        CMotionPanel panel; QVERIFY(panel.PoseSourcePath().isEmpty()); QVERIFY(panel.PoseSourceDigest().isEmpty());
        QVERIFY(panel.BuildPoseGroups(QDir::current().relativeFilePath(path)));
        QCOMPARE(panel.PoseSourcePath(), path); const auto loaded = QCryptographicHash::hash(raw, QCryptographicHash::Sha256);
        QCOMPARE(panel.PoseSourceDigest(), loaded);
        QByteArray snapshot; QVERIFY(panel.CapturePoseBytes(snapshot));
        QVERIFY(QCryptographicHash::hash(snapshot, QCryptographicHash::Sha256) != loaded);
        QVERIFY(!panel.BuildPoseGroups(temp.filePath("missing"))); QCOMPARE(panel.PoseSourcePath(), path); QCOMPARE(panel.PoseSourceDigest(), loaded);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate)); QCOMPARE(file.write("{"), 1); file.close();
        QVERIFY(!panel.BuildPoseGroups(path)); QCOMPARE(panel.PoseSourceDigest(), loaded);
        QVERIFY(QFile::remove(path)); QVERIFY(QDir().mkdir(path)); QVERIFY(!panel.SavePose());
        QCOMPARE(panel.PoseSourceDigest(), loaded); QCOMPARE(panel.PoseSourcePath(), path);
        QVERIFY(QDir().rmdir(path)); QVERIFY(panel.SavePose());
        QVERIFY(file.open(QIODevice::ReadOnly)); const auto bytes = file.readAll(); file.close();
        QCOMPARE(bytes, snapshot); QCOMPARE(panel.PoseSourceDigest(), QCryptographicHash::hash(bytes, QCryptographicHash::Sha256));
        const auto saved = panel.PoseSourceDigest(); QVERIFY(saved != loaded);
        QVERIFY(QFile::remove(path)); QVERIFY(panel.CapturePoseBytes(snapshot)); QCOMPARE(panel.PoseSourceDigest(), saved);
        const auto secondPath = temp.filePath("other.json"); QFile other(secondPath); QVERIFY(other.open(QIODevice::WriteOnly));
        const QByteArray second = "{\r\n\"Groups\":[],\"Extra\":true}\r\n";
        QCOMPARE(other.write(second), second.size()); other.close();
        QVERIFY(panel.BuildPoseGroups(secondPath)); QCOMPARE(panel.PoseSourcePath(), secondPath);
        QCOMPARE(panel.PoseSourceDigest(), QCryptographicHash::hash(second, QCryptographicHash::Sha256));
        QVERIFY(panel.BuildPoseGroups({})); QVERIFY(panel.PoseSourcePath().isEmpty()); QVERIFY(panel.PoseSourceDigest().isEmpty());
    }

    void poseCaptureKeepsDraftMetadataAndRecordingAfterSourceDisappears()
    {
        QTemporaryDir temp; const auto modelPath = temp.filePath("model.json"), posePath = temp.filePath("pose.json");
        QFile model(modelPath); QVERIFY(model.open(QIODevice::WriteOnly));
        QVERIFY(model.write(R"({"FileReferences":{"Moc":"missing.moc3"}})") > 0); model.close();
        const QByteArray source = R"({"Version":3,"FadeInTime":0.25,"RootExtra":{"x":7},"Groups":[
            [{"Id":"D","SingletonExtra":4}],[{"Id":"A","Link":["D"],"Extra":"a"},{"Id":"B","Extra":"b"}]]})";
        QFile pose(posePath); QVERIFY(pose.open(QIODevice::WriteOnly)); QCOMPARE(pose.write(source), source.size()); pose.close();
        CMotionPanel panel; QVERIFY(panel.BuildLists(modelPath, {"A", "B", "C", "D"})); QVERIFY(panel.BuildPoseGroups(posePath));
        QPushButton* add = nullptr; QPushButton* remove = nullptr;
        for (auto* button : panel.findChildren<QPushButton*>()) {
            if (button->text() == QStringLiteral("添加 Part")) add = button;
            if (button->text() == QStringLiteral("删除 Part")) remove = button;
        }
        QVERIFY(add && remove);
        QTimer::singleShot(0, [] {
            auto* dialog = qobject_cast<QInputDialog*>(QApplication::activeModalWidget()); QVERIFY(dialog);
            dialog->setTextValue("C"); dialog->accept();
        });
        add->click(); QVERIFY(panel.HasUnsavedPoseChanges());
        QComboBox* group = nullptr;
        for (auto* combo : panel.findChildren<QComboBox*>()) if (combo->findData("A") >= 0 && combo->findData("B") >= 0) group = combo;
        QVERIFY(group); group->setCurrentIndex(group->findData("A")); remove->click();
        QVERIFY(panel.AddTimelinePoint({CMotionTarget::Parameter, "P"}, 0, 0));
        auto* record = panel.findChild<QCheckBox*>("recordTimeline"); QVERIFY(record); record->setChecked(true);
        panel.RecordParameterValue("P", 1);
        const auto timeline = panel.Timeline(); const auto revision = panel.TimelineRevision();
        auto* history = panel.findChild<QUndoStack*>(); QVERIFY(history);
        const int index = history->index(), count = history->count();
        QSignalSpy saved(&panel, &CMotionPanel::PoseSaved), changed(&panel, &CMotionPanel::PoseConfigChanged);
        QSignalSpy frames(&panel, &CMotionPanel::TimelineFrameChanged), stopped(&panel, &CMotionPanel::TimelinePreviewStopped);
        QByteArray captured; QString error;
        QVERIFY2(panel.CapturePoseBytes(captured, &error), qPrintable(error)); QVERIFY(error.isEmpty());
        const auto root = QJsonDocument::fromJson(captured).object();
        QCOMPARE(root.value("RootExtra"), QJsonValue(QJsonObject{{"x", 7}})); QCOMPARE(root.value("FadeInTime").toDouble(), 0.25);
        const auto groups = root.value("Groups").toArray();
        QCOMPARE(groups[0].toArray()[0].toObject().value("SingletonExtra").toInt(), 4);
        QCOMPARE(groups[1].toArray().size(), 2);
        QCOMPARE(groups[1].toArray()[0].toObject().value("Id").toString(), QString("B"));
        QCOMPARE(groups[1].toArray()[0].toObject().value("Extra").toString(), QString("b"));
        QCOMPARE(groups[1].toArray()[1].toObject().value("Id").toString(), QString("C"));
        QVERIFY(!groups[1].toArray()[1].toObject().contains("Extra"));
        QVERIFY(pose.open(QIODevice::ReadOnly)); QCOMPARE(pose.readAll(), source); pose.close();
        QVERIFY(QFile::remove(posePath)); QByteArray second; QVERIFY(panel.CapturePoseBytes(second)); QCOMPARE(second, captured);
        QVERIFY(!QFileInfo::exists(posePath));
        QVERIFY(panel.Timeline().IsRecording()); QVERIFY(panel.Timeline().HasSameContent(timeline));
        QCOMPARE(panel.TimelineRevision(), revision); QCOMPARE(history->index(), index); QCOMPARE(history->count(), count);
        QVERIFY(panel.HasUnsavedPoseChanges()); QCOMPARE(saved.size(), 0); QCOMPARE(changed.size(), 0);
        QCOMPARE(frames.size(), 0); QCOMPARE(stopped.size(), 0);
        CMotionPanel::PreviewTransportLease lease; QVERIFY(panel.SuspendPreviewTransport(lease));
        QVERIFY(panel.CapturePoseBytes(second)); QCOMPARE(second, captured); QVERIFY(lease.IsValid());
        QVERIFY(panel.Timeline().IsRecordingClockPaused()); QVERIFY(panel.HasUnsavedPoseChanges());
        QVERIFY(panel.ResumePreviewTransport(lease));
        QVERIFY(panel.SavePose()); QVERIFY(pose.open(QIODevice::ReadOnly)); QCOMPARE(pose.readAll(), captured); pose.close();
        QCOMPARE(saved.size(), 1); QVERIFY(!panel.HasUnsavedPoseChanges());
    }

    void poseCaptureFailureKeepsOutputAndDoesNotSaveOrEndPreview()
    {
        CMotionPanel panel; QByteArray bytes("unchanged"); QString error;
        QVERIFY(!panel.CapturePoseBytes(bytes, &error)); QCOMPARE(bytes, QByteArray("unchanged")); QVERIFY(!error.isEmpty());
        QTemporaryDir temp; QFile model(temp.filePath("model.json")); QVERIFY(model.open(QIODevice::WriteOnly));
        QVERIFY(model.write(R"({"FileReferences":{"Moc":"absent.moc3"}})") > 0); model.close();
        QFile pose(temp.filePath("pose.json")); QVERIFY(pose.open(QIODevice::WriteOnly));
        const QByteArray source = R"({"Groups":[[{"Id":"Missing","Extra":"preserve"}],[{"Id":"A"},{"Id":"B"}]]})";
        QCOMPARE(pose.write(source), source.size()); pose.close();
        QVERIFY(panel.BuildLists(model.fileName(), {"A", "B"})); QVERIFY(panel.BuildPoseGroups(pose.fileName()));
        QVERIFY(panel.AddTimelinePoint({CMotionTarget::Parameter, "P"}, 0, 1));
        const auto timeline = panel.Timeline(); const auto revision = panel.TimelineRevision();
        QSignalSpy saved(&panel, &CMotionPanel::PoseSaved), frames(&panel, &CMotionPanel::TimelineFrameChanged);
        QSignalSpy stopped(&panel, &CMotionPanel::TimelinePreviewStopped);
        QVERIFY(!panel.CapturePoseBytes(bytes, &error)); QCOMPARE(bytes, QByteArray("unchanged")); QVERIFY(error.contains("Missing"));
        QVERIFY(!panel.HasUnsavedPoseChanges()); QVERIFY(panel.Timeline().HasSameContent(timeline));
        QCOMPARE(panel.TimelineRevision(), revision); QCOMPARE(saved.size(), 0); QCOMPARE(frames.size(), 0); QCOMPARE(stopped.size(), 0);
        QVERIFY(pose.open(QIODevice::ReadOnly)); QCOMPARE(pose.readAll(), source); pose.close();
        QVERIFY(panel.BuildLists(model.fileName(), {"A", "B", "Missing"}));
        QVERIFY(panel.CapturePoseBytes(bytes)); const auto expected = bytes;
        QVERIFY(QFile::remove(pose.fileName())); QVERIFY(QDir().mkdir(pose.fileName()));
        QVERIFY(!panel.SavePose(&error)); QVERIFY(panel.CapturePoseBytes(bytes)); QCOMPARE(bytes, expected);
        QCOMPARE(saved.size(), 0);
    }

    void timelineByteCapturePreservesPlaybackHistoryAndRejectsPendingInput()
    {
        CMotionPanel panel;
        QVERIFY(panel.AddTimelinePoint({CMotionTarget::Parameter, "A"}, 0, 0));
        QVERIFY(panel.AddTimelinePoint({CMotionTarget::Parameter, "A"}, 5, 1));
        auto* play = panel.findChild<QPushButton*>("previewTimeline");
        auto* head = panel.findChild<QSlider*>("motionTimelinePlayhead");
        auto* loop = panel.findChild<QCheckBox*>("motionLoop");
        auto* timer = panel.findChild<QTimer*>(); auto* stack = panel.findChild<QUndoStack*>();
        QVERIFY(play && head && loop && timer && stack); loop->setChecked(true); head->setValue(250); play->click();
        timer->setInterval(71);
        const auto before = panel.Timeline(); const auto revision = panel.TimelineRevision();
        const double time = panel.TimelineTime(); const int index = stack->index(), count = stack->count();
        const bool dirty = panel.HasUnsavedTimelineChanges();
        QSignalSpy frames(&panel, &CMotionPanel::TimelineFrameChanged), stopped(&panel, &CMotionPanel::TimelinePreviewStopped);
        QSignalSpy changed(&panel, &CMotionPanel::MotionTimelineChanged), history(&panel, &CMotionPanel::TimelineHistoryChanged);
        QByteArray bytes; QString error;
        QVERIFY2(panel.CaptureTimelineBytes(bytes, &error), qPrintable(error));
        QByteArray expected; QVERIFY(panel.Timeline().ToJson(expected, panel.Timeline().Fps(), true)); QCOMPARE(bytes, expected);
        QVERIFY(timer->isActive()); QCOMPARE(timer->interval(), 71); QCOMPARE(panel.TimelineTime(), time);
        QCOMPARE(panel.TimelineRevision(), revision); QCOMPARE(stack->index(), index); QCOMPARE(stack->count(), count);
        QCOMPARE(panel.HasUnsavedTimelineChanges(), dirty); QVERIFY(panel.Timeline().HasSameContent(before));
        QCOMPARE(frames.size(), 0); QCOMPARE(stopped.size(), 0); QCOMPARE(changed.size(), 0); QCOMPARE(history.size(), 0);
        panel.PauseTimelinePreview(); panel.resize(900, 650); panel.show(); QTest::qWait(10);
        auto* table = panel.findChild<QTableWidget*>("motionTimelineTable"); QVERIFY(table);
        table->setCurrentCell(0, 2); table->editItem(table->item(0, 2));
        auto* editor = table->findChild<QLineEdit*>(); QVERIFY(editor); editor->setText("not-a-number");
        const auto current = table->currentIndex(); const auto selected = table->selectionModel()->selectedIndexes();
        const auto pendingRevision = panel.TimelineRevision();
        const int pendingIndex = stack->index(), pendingCount = stack->count();
        QVERIFY(!panel.CaptureTimelineBytes(bytes, &error)); QCOMPARE(bytes, expected); QVERIFY(!error.isEmpty());
        QCOMPARE(editor->text(), QString("not-a-number")); QCOMPARE(table->currentIndex(), current);
        QCOMPARE(table->selectionModel()->selectedIndexes(), selected); QCOMPARE(panel.TimelineRevision(), pendingRevision);
        QCOMPARE(stack->index(), pendingIndex); QCOMPARE(stack->count(), pendingCount); QVERIFY(panel.Timeline().HasSameContent(before));
        QTest::keyClick(editor, Qt::Key_Escape); QVERIFY(panel.CaptureTimelineBytes(bytes));
        auto* record = panel.findChild<QCheckBox*>("recordTimeline"); QVERIFY(record); record->setChecked(true);
        const auto recording = panel.Timeline(); const auto recordingRevision = panel.TimelineRevision();
        QVERIFY(!panel.CaptureTimelineBytes(bytes, &error)); QCOMPARE(bytes, expected);
        QVERIFY(panel.Timeline().IsRecording()); QVERIFY(panel.Timeline().HasSameContent(recording));
        QCOMPARE(panel.TimelineRevision(), recordingRevision); QCOMPARE(stack->index(), pendingIndex); QCOMPARE(stack->count(), pendingCount);
    }

    void transportLeasePreservesPreviewRecordingAndHistory_data()
    {
        QTest::addColumn<int>("mode");
        QTest::newRow("inactive") << 0;
        QTest::newRow("still-preview") << 1;
        QTest::newRow("playing") << 2;
        QTest::newRow("recording") << 3;
    }

    void transportLeasePreservesPreviewRecordingAndHistory()
    {
        QFETCH(int, mode);
        CMotionPanel panel;
        QVERIFY(panel.AddTimelinePoint({CMotionTarget::Parameter, "A"}, 0, 0));
        QVERIFY(panel.AddTimelinePoint({CMotionTarget::Parameter, "A"}, 10, 1));
        panel.MarkTimelineSaved();
        QVERIFY(panel.AddTimelinePoint({CMotionTarget::Parameter, "A"}, 4, 0.5f));
        panel.UndoTimeline();
        auto* play = panel.findChild<QPushButton*>("previewTimeline");
        auto* end = panel.findChild<QPushButton*>("endTimelinePreview");
        auto* head = panel.findChild<QSlider*>("motionTimelinePlayhead");
        auto* speed = panel.findChild<QSlider*>("motionPlaybackSpeed");
        auto* loop = panel.findChild<QCheckBox*>("motionLoop");
        auto* fades = panel.findChild<QCheckBox*>("previewMotionFades");
        auto* record = panel.findChild<QCheckBox*>("recordTimeline");
        auto* timer = panel.findChild<QTimer*>();
        QVERIFY(play && end && head && speed && loop && fades && record && timer);
        speed->setValue(150);
        loop->setChecked(true);
        fades->setChecked(true);
        head->setValue(250);
        if (mode == 0) panel.StopTimelinePreview();
        if (mode == 2) play->click();
        if (mode == 3) record->setChecked(true);
        timer->setInterval(71);
        const auto content = panel.Timeline();
        const bool dirty = panel.HasUnsavedTimelineChanges(), undo = panel.CanUndoTimeline(), redo = panel.CanRedoTimeline();
        const auto revision = panel.TimelineRevision();
        const double time = panel.TimelineTime();
        const bool endEnabled = end->isEnabled(), playing = timer->isActive();
        const QString playText = play->text();
        QJsonObject before;
        QVERIFY(panel.CaptureTimelineState(&before));
        QSignalSpy frame(&panel, &CMotionPanel::TimelineFrameChanged);
        QSignalSpy stopped(&panel, &CMotionPanel::TimelinePreviewStopped);
        QSignalSpy changed(&panel, &CMotionPanel::MotionTimelineChanged);
        QSignalSpy history(&panel, &CMotionPanel::TimelineHistoryChanged);
        QSignalSpy activated(&panel, &CMotionPanel::TimelineActivated);
        QSignalSpy playback(&panel, &CMotionPanel::MotionPlaybackChanged);
        CMotionPanel::PreviewTransportLease lease;
        QString error;
        QVERIFY2(panel.SuspendPreviewTransport(lease, &error), qPrintable(error));
        QVERIFY(error.isEmpty());
        QVERIFY(lease.IsValid() && panel.IsPreviewTransportSuspended());
        QVERIFY(!timer->isActive());
        QVERIFY(!panel.widget()->isEnabled());
        QVERIFY(panel.signalsBlocked());
        QCOMPARE(panel.Timeline().IsRecording(), mode == 3);
        QCOMPARE(panel.Timeline().IsRecordingClockPaused(), mode == 3);
        panel.PauseTimelinePreview();
        panel.StopTimelinePreview();
        panel.SetTimelinePreviewAvailable(!endEnabled);
        panel.RecordParameterValue("A", 8);
        panel.UndoTimeline();
        panel.RedoTimeline();
        panel.MarkTimelineSaved();
        QVERIFY(!panel.AddTimelinePoint({CMotionTarget::Parameter, "A"}, 9, 7));
        QVERIFY(!panel.RestoreTimelineState({}, &error));
        QVERIFY(QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection));
        speed->setValue(25);
        loop->setChecked(false);
        fades->setChecked(false);
        record->setChecked(mode != 3);
        head->setValue(900);
        QCOMPARE(speed->value(), 150);
        QCOMPARE(loop->isChecked(), true);
        QCOMPARE(fades->isChecked(), true);
        QCOMPARE(record->isChecked(), mode == 3);
        QJsonObject frozen;
        QVERIFY(panel.CaptureTimelineState(&frozen));
        QCOMPARE(frozen, before);
        QCOMPARE(panel.TimelineTime(), time);
        QCOMPARE(panel.TimelineRevision(), revision);
        QVERIFY(panel.Timeline().HasSameContent(content));
        QCOMPARE(panel.HasUnsavedTimelineChanges(), dirty);
        QCOMPARE(panel.CanUndoTimeline(), undo);
        QCOMPARE(panel.CanRedoTimeline(), redo);
        QVERIFY2(panel.ResumePreviewTransport(lease, &error), qPrintable(error));
        QVERIFY(!lease.IsValid() && !panel.IsPreviewTransportSuspended());
        QCOMPARE(timer->isActive(), playing);
        QCOMPARE(timer->interval(), 71);
        QCOMPARE(panel.TimelineTime(), time);
        QCOMPARE(head->value(), 250);
        QCOMPARE(play->text(), playText);
        QCOMPARE(end->isEnabled(), endEnabled);
        QVERIFY(panel.widget()->isEnabled());
        QVERIFY(!panel.signalsBlocked());
        QCOMPARE(panel.Timeline().IsRecording(), mode == 3);
        QVERIFY(!panel.Timeline().IsRecordingClockPaused());
        QCOMPARE(panel.TimelineRevision(), revision);
        QCOMPARE(panel.HasUnsavedTimelineChanges(), dirty);
        QCOMPARE(panel.CanUndoTimeline(), undo);
        QCOMPARE(panel.CanRedoTimeline(), redo);
        QCOMPARE(frame.size(), 0);
        QCOMPARE(stopped.size(), 0);
        QCOMPARE(changed.size(), 0);
        QCOMPARE(history.size(), 0);
        QCOMPARE(activated.size(), 0);
        QCOMPARE(playback.size(), 0);
    }

    void failedTransportRestoreKeepsLeaseFrozenAndRetryable_data()
    {
        QTest::addColumn<bool>("recording");
        QTest::newRow("playing") << false;
        QTest::newRow("recording") << true;
    }

    void failedTransportRestoreKeepsLeaseFrozenAndRetryable()
    {
        QFETCH(bool, recording);
        CMotionPanel panel;
        QVERIFY(panel.AddTimelinePoint({CMotionTarget::Parameter, "A"}, 0, 0));
        QVERIFY(panel.AddTimelinePoint({CMotionTarget::Parameter, "A"}, 10, 1));
        panel.MarkTimelineSaved();
        auto* record = panel.findChild<QCheckBox*>("recordTimeline");
        auto* play = panel.findChild<QPushButton*>("previewTimeline");
        auto* timer = panel.findChild<QTimer*>();
        QVERIFY(record && play && timer);
        if (recording) record->setChecked(true);
        else play->click();
        const auto content = panel.Timeline();
        const auto revision = panel.TimelineRevision();
        const auto time = panel.TimelineTime();
        const bool undo = panel.CanUndoTimeline(), redo = panel.CanRedoTimeline(), dirty = panel.HasUnsavedTimelineChanges();
        QSignalSpy frames(&panel, &CMotionPanel::TimelineFrameChanged);
        QSignalSpy stopped(&panel, &CMotionPanel::TimelinePreviewStopped);
        QSignalSpy changed(&panel, &CMotionPanel::MotionTimelineChanged);
        CMotionPanel::PreviewTransportLease lease;
        QVERIFY(panel.SuspendPreviewTransport(lease));
        RefuseEnabledRestore failure;
        bool resumeRejected = false, suspendRejected = false;
        failure.beforeRefusal = [&] {
            QString error;
            CMotionPanel::PreviewTransportLease nested;
            resumeRejected = !panel.ResumePreviewTransport(lease, &error);
            suspendRejected = !panel.SuspendPreviewTransport(nested, &error);
        };
        panel.widget()->installEventFilter(&failure);
        QString error;
        QVERIFY(!panel.ResumePreviewTransport(lease, &error));
        QCOMPARE(failure.calls, 1);
        QVERIFY(error.contains(QStringLiteral("仍被禁用")));
        QVERIFY(resumeRejected && suspendRejected);
        QVERIFY(lease.IsValid() && panel.IsPreviewTransportSuspended());
        QVERIFY(panel.signalsBlocked());
        QVERIFY(!panel.widget()->isEnabled());
        QVERIFY(!timer->isActive());
        QCOMPARE(panel.Timeline().IsRecording(), recording);
        QCOMPARE(panel.Timeline().IsRecordingClockPaused(), recording);
        QCOMPARE(panel.TimelineRevision(), revision);
        QCOMPARE(panel.TimelineTime(), time);
        QVERIFY(panel.Timeline().HasSameContent(content));
        QCOMPARE(panel.CanUndoTimeline(), undo);
        QCOMPARE(panel.CanRedoTimeline(), redo);
        QCOMPARE(panel.HasUnsavedTimelineChanges(), dirty);
        QCOMPARE(frames.size(), 0);
        QCOMPARE(stopped.size(), 0);
        QCOMPARE(changed.size(), 0);
        panel.widget()->removeEventFilter(&failure);
        QVERIFY2(panel.ResumePreviewTransport(lease, &error), qPrintable(error));
        QVERIFY(!lease.IsValid() && !panel.IsPreviewTransportSuspended());
        QVERIFY(!panel.signalsBlocked());
        QVERIFY(panel.widget()->isEnabled());
        QCOMPARE(timer->isActive(), !recording);
        QVERIFY(!timer->signalsBlocked());
        QCOMPARE(panel.Timeline().IsRecording(), recording);
        QVERIFY(!panel.Timeline().IsRecordingClockPaused());
        QCOMPARE(panel.TimelineRevision(), revision);
        QCOMPARE(frames.size(), 0);
        QCOMPARE(stopped.size(), 0);
    }

    void frozenPoseDraftCanBeSavedWithoutRuntimeOrPreviewSignals()
    {
        QTemporaryDir directory;
        QFile model(directory.filePath("model.json"));
        QVERIFY(model.open(QIODevice::WriteOnly));
        QVERIFY(model.write(R"({"FileReferences":{"Moc":"absent.moc3"}})") > 0);
        model.close();
        QFile pose(directory.filePath("pose.json"));
        QVERIFY(pose.open(QIODevice::WriteOnly));
        QVERIFY(pose.write(R"({"Groups":[[{"Id":"A"},{"Id":"B"}]],"FadeInTime":0.4,"Extra":"keep"})") > 0);
        pose.close();
        CMotionPanel panel;
        QString error;
        QVERIFY(panel.BuildLists(model.fileName(), {"A", "B", "C"}, &error));
        QVERIFY(panel.BuildPoseGroups(pose.fileName(), &error));
        QPushButton* add = nullptr;
        for (auto* button : panel.findChildren<QPushButton*>())
            if (button->text() == QStringLiteral("添加 Part")) add = button;
        QVERIFY(add);
        QTimer::singleShot(0, [] {
            auto* input = qobject_cast<QInputDialog*>(QApplication::activeModalWidget());
            QVERIFY(input);
            input->setTextValue("C");
            input->accept();
        });
        add->click();
        QVERIFY(panel.HasUnsavedPoseChanges());
        QVERIFY(panel.AddTimelinePoint({CMotionTarget::Model, "Opacity"}, 0, 0));
        QVERIFY(panel.AddTimelinePoint({CMotionTarget::Model, "Opacity"}, 1, 1));
        const auto content = panel.Timeline();
        const auto revision = panel.TimelineRevision();
        QSignalSpy saved(&panel, &CMotionPanel::PoseSaved);
        QSignalSpy frame(&panel, &CMotionPanel::TimelineFrameChanged);
        QSignalSpy stopped(&panel, &CMotionPanel::TimelinePreviewStopped);
        CMotionPanel::PreviewTransportLease lease;
        QVERIFY(panel.SuspendPreviewTransport(lease));
        QVERIFY(!panel.BuildLists(model.fileName(), {"foreign-part"}, &error));
        QVERIFY2(panel.SavePose(&error), qPrintable(error));
        QVERIFY(!panel.HasUnsavedPoseChanges());
        QVERIFY(panel.IsPreviewTransportSuspended() && lease.IsValid());
        QVERIFY(!panel.widget()->isEnabled() && panel.signalsBlocked());
        QVERIFY(pose.open(QIODevice::ReadOnly));
        const auto output = QJsonDocument::fromJson(pose.readAll()).object();
        QCOMPARE(output.value("Groups").toArray().first().toArray().size(), 3);
        QCOMPARE(output.value("Groups").toArray().first().toArray().last().toObject().value("Id").toString(), QString("C"));
        QCOMPARE(output.value("FadeInTime").toDouble(), 0.4);
        QCOMPARE(output.value("Extra").toString(), QString("keep"));
        QCOMPARE(saved.size(), 0);
        QCOMPARE(frame.size(), 0);
        QCOMPARE(stopped.size(), 0);
        QVERIFY(panel.ResumePreviewTransport(lease));
        QVERIFY(!panel.HasUnsavedPoseChanges());
        QVERIFY(panel.Timeline().HasSameContent(content));
        QCOMPARE(panel.TimelineRevision(), revision);
        QCOMPARE(frame.size(), 0);
        QCOMPARE(stopped.size(), 0);
        QCOMPARE(saved.size(), 0);
    }

    void transportLeaseIsMoveOnlyAndRejectsReentryAndForeignResume()
    {
        using Lease = CMotionPanel::PreviewTransportLease;
        static_assert(!std::is_copy_constructible_v<Lease> && !std::is_copy_assignable_v<Lease>);
        static_assert(std::is_nothrow_move_constructible_v<Lease> && std::is_nothrow_move_assignable_v<Lease>);
        CMotionPanel first, second;
        Lease lease, other;
        QString error;
        QVERIFY(first.SuspendPreviewTransport(lease));
        QVERIFY(!first.SuspendPreviewTransport(other, &error));
        QVERIFY(!error.isEmpty() && !other.IsValid());
        QVERIFY(!second.SuspendPreviewTransport(lease));
        QVERIFY(!second.ResumePreviewTransport(lease));
        QVERIFY(first.IsPreviewTransportSuspended());
        QVERIFY(!second.IsPreviewTransportSuspended());
        QVERIFY(lease.IsValid());
        Lease moved(std::move(lease));
        QVERIFY(!lease.IsValid() && moved.IsValid());
        QVERIFY(!first.ResumePreviewTransport(lease));
        other = std::move(moved);
        QVERIFY(!moved.IsValid() && other.IsValid());
        QVERIFY(first.ResumePreviewTransport(other));
        QVERIFY(!other.IsValid());
        QVERIFY(!first.ResumePreviewTransport(other));
        QVERIFY(first.SuspendPreviewTransport(other));
        QVERIFY(!first.ResumePreviewTransport(lease));
        QVERIFY(other.IsValid());
        QVERIFY(first.ResumePreviewTransport(other));
        {
            Lease abandoned;
            QVERIFY(first.SuspendPreviewTransport(abandoned));
        }
        QVERIFY(first.IsPreviewTransportSuspended());
        QVERIFY(!first.SuspendPreviewTransport(lease));
        QVERIFY(!first.ResumePreviewTransport(lease));
        auto temporary = std::make_unique<CMotionPanel>();
        QVERIFY(temporary->SuspendPreviewTransport(lease));
        QVERIFY(lease.IsValid());
        temporary.reset();
        QVERIFY(!lease.IsValid());
        QVERIFY(!second.ResumePreviewTransport(lease));
    }

    void transportRestoresInheritedDisabledAndSignalStates()
    {
        QWidget parent;
        CMotionPanel panel(&parent);
        QVERIFY(panel.AddTimelinePoint({CMotionTarget::Parameter, "A"}, 0, 0));
        QVERIFY(panel.AddTimelinePoint({CMotionTarget::Parameter, "A"}, 1, 1));
        auto* end = panel.findChild<QPushButton*>("endTimelinePreview");
        auto* loop = panel.findChild<QCheckBox*>("motionLoop");
        auto* speed = panel.findChild<QSlider*>("motionPlaybackSpeed");
        end->setEnabled(false);
        speed->blockSignals(true);
        loop->blockSignals(true);
        panel.blockSignals(true);
        parent.setEnabled(false);
        QVERIFY(!panel.widget()->testAttribute(Qt::WA_ForceDisabled));
        CMotionPanel::PreviewTransportLease lease;
        QVERIFY(panel.SuspendPreviewTransport(lease));
        loop->setChecked(true);
        speed->setValue(25);
        QJsonObject frozen;
        QVERIFY(panel.CaptureTimelineState(&frozen));
        QCOMPARE(frozen.value("motion").toObject().value("Meta").toObject().value("Loop").toBool(), false);
        QVERIFY(panel.ResumePreviewTransport(lease));
        QVERIFY(panel.signalsBlocked());
        QVERIFY(loop->signalsBlocked() && speed->signalsBlocked());
        QVERIFY(!loop->isChecked());
        QCOMPARE(speed->value(), 100);
        QVERIFY(!panel.widget()->isEnabled());
        QVERIFY(!panel.widget()->testAttribute(Qt::WA_ForceDisabled));
        parent.setEnabled(true);
        QVERIFY(panel.widget()->isEnabled());
        QVERIFY(!end->isEnabled());
        QVERIFY(speed->isEnabled());
    }

    void transportResumeDoesNotReplayEventsOrIncludeSuspensionTime()
    {
        CMotionPanel panel;
        QVERIFY(panel.RestoreTimelineState(eventState({QJsonObject{{"Time", 0}, {"Value", "start"}},
                                                       QJsonObject{{"Time", 5}, {"Value", "later"}}}, 10, true)));
        auto* timer = panel.findChild<QTimer*>();
        auto* play = panel.findChild<QPushButton*>("previewTimeline");
        auto* end = panel.findChild<QPushButton*>("endTimelinePreview");
        auto* speed = panel.findChild<QSlider*>("motionPlaybackSpeed");
        speed->setValue(200);
        QSignalSpy events(&panel, &CMotionPanel::TimelineEventsTriggered);
        QSignalSpy frames(&panel, &CMotionPanel::TimelineFrameChanged);
        QSignalSpy stopped(&panel, &CMotionPanel::TimelinePreviewStopped);
        CMotionPanel::PreviewTransportLease lease;
        QVERIFY(panel.SuspendPreviewTransport(lease));
        QVERIFY(panel.ResumePreviewTransport(lease));
        QVERIFY(!timer->isActive());
        QCOMPARE(events.size(), 0);
        QCOMPARE(frames.size(), 0);
        play->click();
        QCOMPARE(occurrences(events).size(), 1);
        QCOMPARE(occurrences(events).first().value, QString("start"));
        const double time = panel.TimelineTime();
        const auto revision = panel.TimelineRevision();
        events.clear(); frames.clear();
        QVERIFY(panel.SuspendPreviewTransport(lease));
        QTest::qWait(40);
        QCOMPARE(panel.TimelineTime(), time);
        QCOMPARE(events.size(), 0);
        QCOMPARE(frames.size(), 0);
        QElapsedTimer resumed;
        resumed.start();
        QVERIFY(panel.ResumePreviewTransport(lease));
        QVERIFY(timer->isActive() && end->isEnabled());
        QCOMPARE(events.size(), 0);
        QCOMPARE(frames.size(), 0);
        QCOMPARE(panel.TimelineRevision(), revision);
        QVERIFY(QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection));
        const double wallTime = resumed.nsecsElapsed() / 1e9;
        QVERIFY(panel.TimelineTime() >= time);
        QVERIFY(panel.TimelineTime() - time <= wallTime * 2.0 + 0.000001);
        QCOMPARE(events.size(), 0);
        QCOMPARE(frames.size(), 1);
        QCOMPARE(stopped.size(), 0);
    }

    void transportRejectsPendingEditorAndCurveDragWithoutCommitting()
    {
        QTemporaryDir directory;
        const QString path = directory.filePath("motion.json");
        QVERIFY(makeClip(path));
        CMotionPanel panel;
        QVERIFY(panel.LoadTimeline(path));
        panel.resize(1000, 850);
        panel.show();
        auto* table = panel.findChild<QTableWidget*>("motionTimelineTable");
        table->setCurrentCell(1, 2);
        table->editItem(table->item(1, 2));
        QPointer<QLineEdit> editor = table->findChild<QLineEdit*>();
        QVERIFY(editor);
        editor->setText("0.75");
        const auto original = panel.Timeline();
        const auto revision = panel.TimelineRevision();
        CMotionPanel::PreviewTransportLease lease;
        QString error;
        QVERIFY(!panel.SuspendPreviewTransport(lease, &error));
        QVERIFY(!error.isEmpty() && !lease.IsValid());
        QVERIFY(editor && editor->text() == "0.75");
        QVERIFY(panel.widget()->isEnabled());
        QVERIFY(!panel.signalsBlocked());
        QVERIFY(panel.Timeline().HasSameContent(original));
        QCOMPARE(panel.TimelineRevision(), revision);
        QTest::keyClick(editor, Qt::Key_Escape);
        auto* curve = panel.findChild<CMotionCurveView*>("motionCurveView");
        curve->setFixedSize(320, 190);
        QTest::mousePress(curve, Qt::LeftButton, Qt::NoModifier, QPoint(34, 161));
        QVERIFY(!panel.SuspendPreviewTransport(lease, &error));
        QVERIFY(!lease.IsValid());
        QTest::mouseRelease(curve, Qt::LeftButton, Qt::NoModifier, QPoint(34, 161));
        QVERIFY(panel.SuspendPreviewTransport(lease));
        QVERIFY(panel.ResumePreviewTransport(lease));
    }

    void pauseKeepsTheDisplayedFrameAndHistory_data()
    {
        QTest::addColumn<bool>("clean");
        QTest::newRow("clean-with-redo") << true;
        QTest::newRow("dirty-with-redo") << false;
    }

    void pauseKeepsTheDisplayedFrameAndHistory()
    {
        QFETCH(bool, clean);
        CMotionPanel panel;
        const CMotionTrack track{CMotionTarget::Model, "Opacity"};
        QVERIFY(panel.AddTimelinePoint(track, 0, 0));
        QVERIFY(panel.AddTimelinePoint(track, 10, 1));
        panel.MarkTimelineSaved();
        QVERIFY(panel.AddTimelinePoint(track, 3, 0.3f));
        QVERIFY(panel.AddTimelinePoint(track, 5, 0.5f));
        panel.UndoTimeline();
        if (clean) panel.UndoTimeline();
        QCOMPARE(panel.HasUnsavedTimelineChanges(), !clean);
        QVERIFY(panel.CanUndoTimeline());
        QVERIFY(panel.CanRedoTimeline());
        auto* play = panel.findChild<QPushButton*>("previewTimeline");
        auto* end = panel.findChild<QPushButton*>("endTimelinePreview");
        auto* playhead = panel.findChild<QSlider*>("motionTimelinePlayhead");
        auto* timer = panel.findChild<QTimer*>();
        QVERIFY(play && end && playhead && timer);
        playhead->setValue(250);
        play->click();
        QVERIFY(timer->isActive());
        QVERIFY(end->isEnabled());
        QCOMPARE(play->text(), QStringLiteral("暂停"));
        const auto content = panel.Timeline();
        const double time = panel.TimelineTime();
        const int slider = playhead->value();
        QJsonObject before;
        QVERIFY(panel.CaptureTimelineState(&before));
        QSignalSpy frame(&panel, &CMotionPanel::TimelineFrameChanged);
        QSignalSpy parameters(&panel, &CMotionPanel::MotionTimelinePreviewChanged);
        QSignalSpy stopped(&panel, &CMotionPanel::TimelinePreviewStopped);
        QSignalSpy dirty(&panel, &CMotionPanel::TimelineDirtyChanged);
        QSignalSpy history(&panel, &CMotionPanel::TimelineHistoryChanged);
        QSignalSpy activated(&panel, &CMotionPanel::TimelineActivated);
        QSignalSpy edits(&panel, &CMotionPanel::MotionTimelineChanged);
        QTest::qSleep(45);
        const quint64 revision = panel.TimelineRevision();
        panel.PauseTimelinePreview();
        QVERIFY(!timer->isActive());
        QVERIFY(panel.TimelineRevision() > revision);
        QCOMPARE(panel.TimelineTime(), time);
        QCOMPARE(playhead->value(), slider);
        QCOMPARE(play->text(), QStringLiteral("预览"));
        QVERIFY(end->isEnabled());
        QTest::qWait(60);
        QCOMPARE(panel.TimelineTime(), time);
        QCOMPARE(frame.size(), 0);
        QCOMPARE(parameters.size(), 0);
        QCOMPARE(stopped.size(), 0);
        QCOMPARE(dirty.size(), 0);
        QCOMPARE(history.size(), 0);
        QCOMPARE(activated.size(), 0);
        QCOMPARE(edits.size(), 0);
        QVERIFY(panel.Timeline().HasSameContent(content));
        QCOMPARE(panel.HasUnsavedTimelineChanges(), !clean);
        QVERIFY(panel.CanUndoTimeline());
        QVERIFY(panel.CanRedoTimeline());
        QJsonObject after;
        QVERIFY(panel.CaptureTimelineState(&after));
        QCOMPARE(after, before);

        play->click();
        QVERIFY(timer->isActive());
        QCOMPARE(panel.TimelineTime(), time);
        QCOMPARE(frame.size(), 1);
        QCOMPARE(frame.last().first().value<CMotionPreviewFrame>().model, content.EvaluateFrame(float(time)).model);
        QTRY_VERIFY_WITH_TIMEOUT(panel.TimelineTime() > time, 500);
        panel.PauseTimelinePreview();
        QVERIFY(end->isEnabled());
        end->click();
        QCOMPARE(stopped.size(), 1);
        QVERIFY(!end->isEnabled());
        QVERIFY(!timer->isActive());
        QCOMPARE(panel.HasUnsavedTimelineChanges(), !clean);
        QVERIFY(panel.Timeline().HasSameContent(content));
        QVERIFY(panel.CanUndoTimeline());
        QVERIFY(panel.CanRedoTimeline());
    }

    void pauseNeverEnablesOrEndsAnInactivePreview()
    {
        CMotionPanel panel;
        auto* play = panel.findChild<QPushButton*>("previewTimeline");
        auto* end = panel.findChild<QPushButton*>("endTimelinePreview");
        QVERIFY(play && end);
        QSignalSpy frame(&panel, &CMotionPanel::TimelineFrameChanged);
        QSignalSpy stopped(&panel, &CMotionPanel::TimelinePreviewStopped);
        panel.PauseTimelinePreview();
        QVERIFY(!play->isEnabled());
        QVERIFY(!end->isEnabled());
        QCOMPARE(panel.TimelineTime(), 0.0);
        QCOMPARE(frame.size(), 0);
        QCOMPARE(stopped.size(), 0);
        QVERIFY(panel.AddTimelinePoint({CMotionTarget::Parameter, "P"}, 0, 0));
        QVERIFY(panel.AddTimelinePoint({CMotionTarget::Parameter, "P"}, 1, 1));
        panel.StopTimelinePreview();
        QVERIFY(!end->isEnabled());
        frame.clear();
        stopped.clear();
        panel.PauseTimelinePreview();
        panel.PauseTimelinePreview();
        QVERIFY(!end->isEnabled());
        QCOMPARE(play->text(), QStringLiteral("预览"));
        QCOMPARE(frame.size(), 0);
        QCOMPARE(stopped.size(), 0);
    }

    void previewAvailabilityOnlyChangesTheRestoreButton_data()
    {
        QTest::addColumn<bool>("playing");
        QTest::newRow("paused") << false;
        QTest::newRow("playing") << true;
    }

    void previewAvailabilityOnlyChangesTheRestoreButton()
    {
        QFETCH(bool, playing);
        CMotionPanel panel;
        const CMotionTrack track{CMotionTarget::Model, "Opacity"};
        QVERIFY(panel.AddTimelinePoint(track, 0, 0));
        QVERIFY(panel.AddTimelinePoint(track, 10, 1));
        panel.MarkTimelineSaved();
        QVERIFY(panel.AddTimelinePoint(track, 5, 0.5f));
        panel.UndoTimeline();
        auto* play = panel.findChild<QPushButton*>("previewTimeline");
        auto* end = panel.findChild<QPushButton*>("endTimelinePreview");
        auto* playhead = panel.findChild<QSlider*>("motionTimelinePlayhead");
        auto* timer = panel.findChild<QTimer*>();
        QVERIFY(play && end && playhead && timer);
        playhead->setValue(250);
        if (playing) play->click();
        const double time = panel.TimelineTime();
        const int slider = playhead->value();
        const auto revision = panel.TimelineRevision();
        const QString playText = play->text();
        QJsonObject before;
        QVERIFY(panel.CaptureTimelineState(&before));
        QSignalSpy frame(&panel, &CMotionPanel::TimelineFrameChanged);
        QSignalSpy stopped(&panel, &CMotionPanel::TimelinePreviewStopped);
        QSignalSpy dirty(&panel, &CMotionPanel::TimelineDirtyChanged);
        QSignalSpy history(&panel, &CMotionPanel::TimelineHistoryChanged);
        for (bool available : {false, true, true, false}) {
            panel.SetTimelinePreviewAvailable(available);
            QCOMPARE(end->isEnabled(), available);
            QCOMPARE(timer->isActive(), playing);
            QCOMPARE(play->text(), playText);
            QCOMPARE(panel.TimelineTime(), time);
            QCOMPARE(playhead->value(), slider);
            QCOMPARE(panel.TimelineRevision(), revision);
            QVERIFY(!panel.HasUnsavedTimelineChanges());
            QVERIFY(panel.CanUndoTimeline());
            QVERIFY(panel.CanRedoTimeline());
            QJsonObject after;
            QVERIFY(panel.CaptureTimelineState(&after));
            QCOMPARE(after, before);
        }
        QCOMPARE(frame.size(), 0);
        QCOMPARE(stopped.size(), 0);
        QCOMPARE(dirty.size(), 0);
        QCOMPARE(history.size(), 0);
    }

    void failedPreviewStopCanRestoreTheButtonForRetry()
    {
        CMotionPanel panel;
        QVERIFY(panel.AddTimelinePoint({CMotionTarget::Model, "Opacity"}, 0, 1));
        auto* end = panel.findChild<QPushButton*>("endTimelinePreview");
        QVERIFY(end);
        panel.SetTimelinePreviewAvailable(true);
        int attempts = 0;
        connect(&panel, &CMotionPanel::TimelinePreviewStopped, &panel, [&] {
            panel.SetTimelinePreviewAvailable(++attempts == 1);
        });
        QSignalSpy frame(&panel, &CMotionPanel::TimelineFrameChanged);
        QSignalSpy stopped(&panel, &CMotionPanel::TimelinePreviewStopped);
        end->click();
        QCOMPARE(attempts, 1);
        QVERIFY(end->isEnabled());
        end->click();
        QCOMPARE(attempts, 2);
        QVERIFY(!end->isEnabled());
        QCOMPARE(stopped.size(), 2);
        QCOMPARE(frame.size(), 0);
    }

    void eventOnlyClipExportsImportsAndPlays()
    {
        const QJsonArray events{QJsonObject{{"Time", 0.02}, {"Value", QStringLiteral("開始 🌸")}},
                                QJsonObject{{"Time", 0.04}, {"Value", "end"}}};
        CMotionPanel panel;
        QVERIFY(panel.RestoreTimelineState(eventState(events, 0.06)));
        QVERIFY(panel.Timeline().HasAnimation());
        QVERIFY(!panel.Timeline().HasCurves());
        QVERIFY(panel.findChild<QPushButton*>("previewTimeline")->isEnabled());
        QVERIFY(panel.findChild<QSlider*>("motionTimelinePlayhead")->isEnabled());
        auto* list = panel.findChild<QTableWidget*>("timelineEventList");
        QCOMPARE(list->rowCount(), 2);
        QCOMPARE(list->editTriggers(), QAbstractItemView::NoEditTriggers);
        QCOMPARE(panel.findChild<QTabWidget*>("animationDetailTabs")->currentWidget(), list);
        QTemporaryDir temp;
        const QString path = temp.filePath("events.motion3.json");
        QVERIFY(panel.SaveTimeline(path));
        CMotionPanel imported;
        QVERIFY(imported.LoadTimeline(path));
        QCOMPARE(imported.Timeline().Events(), events);
        QVERIFY(!imported.Timeline().HasCurves());
        QCOMPARE(imported.findChild<QTableWidget*>("timelineEventList")->rowCount(), 2);
        QSignalSpy fired(&imported, &CMotionPanel::TimelineEventsTriggered);
        QSignalSpy preview(&imported, &CMotionPanel::TimelineFrameChanged);
        auto* play = imported.findChild<QPushButton*>("previewTimeline");
        play->click();
        QTest::qSleep(90);
        QCoreApplication::processEvents();
        const auto played = occurrences(fired);
        QCOMPARE(played.size(), 2);
        QCOMPARE(played.at(0).time, 0.02);
        QCOMPARE(played.at(0).value, QStringLiteral("開始 🌸"));
        QCOMPARE(played.at(1).value, QStringLiteral("end"));
        QCOMPARE(imported.TimelineTime(), 0.06);
        QCOMPARE(play->text(), QStringLiteral("预览"));
        QVERIFY(!preview.isEmpty());
        QVERIFY(preview.last().first().value<CMotionPreviewFrame>().IsEmpty());
        QVERIFY(!imported.HasUnsavedTimelineChanges());
        QVERIFY(!imported.CanUndoTimeline());
    }

    void zeroTimeEventsDoNotRepeatOnPauseResumeOrStoppedTicks()
    {
        CMotionPanel panel;
        QVERIFY(panel.RestoreTimelineState(eventState({QJsonObject{{"Time", 0}, {"Value", "start"}}}, 10)));
        QSignalSpy fired(&panel, &CMotionPanel::TimelineEventsTriggered);
        auto* play = panel.findChild<QPushButton*>("previewTimeline");
        play->click();
        QCOMPARE(occurrences(fired).size(), 1);
        QCOMPARE(occurrences(fired).first().value, QStringLiteral("start"));
        play->click();
        const double paused = panel.TimelineTime();
        QTest::qSleep(20);
        play->click();
        QCOMPARE(panel.TimelineTime(), paused);
        QCOMPARE(occurrences(fired).size(), 1);
        QTest::qSleep(20);
        play->click();
        QCOMPARE(occurrences(fired).size(), 1);
        QVERIFY(panel.TimelineTime() > paused);
        panel.StopTimelinePreview();

        QVERIFY(panel.RestoreTimelineState(eventState({QJsonObject{{"Time", 0}, {"Value", "zero"}}}, 0, true)));
        fired.clear();
        play->click();
        QCOMPARE(occurrences(fired).size(), 1);
        QTRY_COMPARE(play->text(), QStringLiteral("预览"));
        QTest::qWait(30);
        QCOMPARE(occurrences(fired).size(), 1);
        QCOMPARE(panel.TimelineTime(), 0.0);
    }

    void eventSeeksUseExactTimesWithoutFiringOrDirtying()
    {
        const double exactTime = 1.000000000123;
        CMotionPanel panel;
        QVERIFY(panel.RestoreTimelineState(eventState({
            QJsonObject{{"Time", 0.5}, {"Value", "middle"}},
            QJsonObject{{"Time", exactTime}, {"Value", QStringLiteral("精确定位 🌸")}}}, 2)));
        panel.resize(740, 520);
        panel.show();
        QCoreApplication::processEvents();
        QSignalSpy fired(&panel, &CMotionPanel::TimelineEventsTriggered);
        auto* slider = panel.findChild<QSlider*>("motionTimelinePlayhead");
        auto* list = panel.findChild<QTableWidget*>("timelineEventList");
        auto* view = panel.findChild<CMotionCurveView*>();
        slider->setValue(250);
        QCOMPARE(panel.TimelineTime(), 0.5);
        QVERIFY(fired.isEmpty());
        list->setCurrentCell(1, 1);
        list->setFocus();
        QTest::keyClick(list, Qt::Key_Return);
        QCOMPARE(panel.TimelineTime(), exactTime);
        QVERIFY(fired.isEmpty());
        slider->setValue(0);
        const double laneWidth = view->width() - 48;
        const int buckets = qMax(1, static_cast<int>(laneWidth / 14));
        const int bucket = qMin(buckets - 1, static_cast<int>(exactTime / 2 * buckets));
        const int markerX = qRound(34 + (bucket + 0.5) * laneWidth / buckets);
        QTest::mouseClick(view, Qt::LeftButton, Qt::NoModifier, QPoint(markerX, 27));
        QCOMPARE(panel.TimelineTime(), exactTime);
        QVERIFY(fired.isEmpty());
        QVERIFY(panel.findChild<QLabel*>("timelineEventStatus")->text().contains(QStringLiteral("精确定位 🌸")));
        QVERIFY(!panel.HasUnsavedTimelineChanges());
        QVERIFY(!panel.CanUndoTimeline());
        QJsonObject state;
        QVERIFY(panel.CaptureTimelineState(&state));
        QCOMPARE(state.value("playhead").toDouble(), exactTime);
        CMotionPanel restored;
        QVERIFY(restored.RestoreTimelineState(state));
        QCOMPARE(restored.TimelineTime(), exactTime);
        auto* play = panel.findChild<QPushButton*>("previewTimeline");
        play->click();
        QCOMPARE(occurrences(fired).size(), 1);
        QCOMPARE(occurrences(fired).first().time, exactTime);
        QCOMPARE(occurrences(fired).first().sourceIndex, 1);
        play->click();
        QCOMPARE(occurrences(fired).size(), 1);
        QVERIFY(!panel.HasUnsavedTimelineChanges());
    }

    void delayedPlaybackDeliversCrossedEventsAndBoundsLoopBatches()
    {
        CMotionPanel panel;
        QVERIFY(panel.RestoreTimelineState(eventState({
            QJsonObject{{"Time", 0.01}, {"Value", "one"}},
            QJsonObject{{"Time", 0.02}, {"Value", "two"}},
            QJsonObject{{"Time", 0.03}, {"Value", "three"}}}, 10)));
        QSignalSpy fired(&panel, &CMotionPanel::TimelineEventsTriggered);
        auto* play = panel.findChild<QPushButton*>("previewTimeline");
        play->click();
        QVERIFY(fired.isEmpty());
        QTest::qSleep(70);
        play->click();
        QCOMPARE(fired.size(), 1);
        const auto crossed = occurrences(fired);
        QCOMPARE(crossed.size(), 3);
        for (int i = 0; i < crossed.size(); ++i) QCOMPARE(crossed.at(i).sourceIndex, i);

        QVERIFY(panel.RestoreTimelineState(eventState({QJsonObject{{"Time", 0.0000005}, {"Value", "loop"}}}, 0.000001, true)));
        fired.clear();
        QVERIFY(panel.TimelineLoop());
        play->click();
        QTest::qSleep(30);
        play->click();
        QCOMPARE(fired.size(), 1);
        const auto batch = fired.first().first().value<CMotionEventBatch>();
        QVERIFY(batch.valid);
        QVERIFY(!batch.countSaturated);
        QVERIFY(!batch.occurrences.isEmpty());
        QVERIFY(batch.occurrences.size() <= 100);
        QVERIFY(batch.omittedCount > 0);
        QVERIFY(batch.occurrences.last().cycle > 0);
        QCOMPARE(batch.occurrences.first().time, 0.0000005);
        const auto* status = panel.findChild<QLabel*>("timelineEventStatus");
        QVERIFY(status->text().contains(QStringLiteral("loop")));
        QVERIFY(status->text().contains(QString::number(batch.omittedCount)));
        QVERIFY(status->toolTip().size() < 10000);
        QVERIFY(panel.TimelineTime() >= 0 && panel.TimelineTime() < panel.Timeline().Timing().duration);
    }

    void eventListTracksSettingsUndoRedoAndSessionRestore()
    {
        CMotionPanel panel;
        QVERIFY(panel.RestoreTimelineState(eventState({QJsonObject{{"Time", 1}, {"Value", "before"}}}, 2)));
        auto* list = panel.findChild<QTableWidget*>("timelineEventList");
        QTimer::singleShot(0, [](){
            auto* dialog = qobject_cast<CMotionSettingsDialog*>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            const auto closeDialog = qScopeGuard([dialog](){ if (dialog->isVisible()) dialog->reject(); });
            auto* table = dialog->findChild<QTableWidget*>("motionEventTable");
            table->item(0, 0)->setText("1.5");
            table->item(0, 1)->setText("after");
            dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
        });
        panel.findChild<QPushButton*>("editMotionSettings")->click();
        QCOMPARE(list->item(0, 0)->text().toDouble(), 1.5);
        QCOMPARE(list->item(0, 1)->text(), QStringLiteral("after"));
        QVERIFY(panel.HasUnsavedTimelineChanges());
        panel.UndoTimeline();
        QCOMPARE(list->item(0, 1)->text(), QStringLiteral("before"));
        QVERIFY(!panel.HasUnsavedTimelineChanges());
        panel.RedoTimeline();
        QCOMPARE(list->item(0, 1)->text(), QStringLiteral("after"));
        QJsonObject snapshot;
        QVERIFY(panel.CaptureTimelineState(&snapshot));
        CMotionPanel restored;
        QVERIFY(restored.RestoreTimelineState(snapshot));
        QCOMPARE(restored.findChild<QTableWidget*>("timelineEventList")->item(0, 1)->text(), QStringLiteral("after"));
        QVERIFY(!restored.HasUnsavedTimelineChanges());
        QVERIFY(restored.RestoreTimelineState({}));
        QCOMPARE(restored.findChild<QTableWidget*>("timelineEventList")->rowCount(), 0);
        QVERIFY(!restored.findChild<QTabWidget*>("animationDetailTabs")->isTabVisible(1));
        QVERIFY(restored.findChild<QLabel*>("timelineEventStatus")->isHidden());
    }

    void previewFadesPersistOnlyAsLayoutPreference()
    {
        CMotionPanel panel;
        QVERIFY(panel.RestoreTimelineState(settingsState()));
        QVERIFY(!panel.UsesMotionFades());
        const CMotionTimeline before = panel.Timeline();
        const quint64 revision = panel.TimelineRevision();
        QSignalSpy preview(&panel, &CMotionPanel::TimelineFrameChanged);
        auto* fades = panel.findChild<QCheckBox*>("previewMotionFades");
        fades->setChecked(true);
        QVERIFY(panel.UsesMotionFades());
        QVERIFY(panel.TimelineRevision() > revision);
        QVERIFY(!preview.isEmpty());
        QVERIFY(panel.Timeline().HasSameContent(before));
        QVERIFY(!panel.HasUnsavedTimelineChanges());
        QVERIFY(!panel.CanUndoTimeline());
        const QByteArray layout = panel.SaveLayout();
        QVERIFY(QJsonDocument::fromJson(layout).object().value("fades").toBool());
        CMotionPanel restored;
        restored.RestoreLayout(layout);
        QVERIFY(restored.UsesMotionFades());
        QVERIFY(!restored.HasUnsavedTimelineChanges());
        QVERIFY(!restored.CanUndoTimeline());
        auto legacy = QJsonDocument::fromJson(layout).object();
        legacy.remove("fades");
        restored.RestoreLayout(QJsonDocument(legacy).toJson());
        QVERIFY(!restored.UsesMotionFades());
        QVERIFY(!restored.HasUnsavedTimelineChanges());
    }

    void eventListsFitCompactWorkspaceInBothThemes()
    {
        QJsonArray events;
        for (int i = 0; i < 80; ++i)
            events.append(QJsonObject{{"Time", i / 40.0}, {"Value", QStringLiteral("事件 %1 · 动作与口型 🌸").arg(i)}});
        for (const bool dark : {false, true}) {
            CMotionPanel panel;
            panel.setStyleSheet(Theme::globalStyleSheet(dark));
            QVERIFY(panel.RestoreTimelineState(eventState(events, 2)));
            panel.resize(700, 520);
            panel.show();
            QCoreApplication::processEvents();
            auto* list = panel.findChild<QTableWidget*>("timelineEventList");
            if (!qEnvironmentVariableIsEmpty("MOTION_EVENT_SCREENSHOT")) {
                const QString output = qEnvironmentVariable("MOTION_EVENT_SCREENSHOT") +
                    (dark ? QStringLiteral("-dark.png") : QStringLiteral("-light.png"));
                QVERIFY(panel.grab().save(output));
            }
            QVERIFY2(panel.width() <= 740, qPrintable(QStringLiteral("panel width %1").arg(panel.width())));
            QVERIFY2(panel.height() <= 540, qPrintable(QStringLiteral("panel height %1").arg(panel.height())));
            QVERIFY(list->isVisible());
            QVERIFY2(list->viewport()->height() >= list->rowHeight(0) * 2,
                     qPrintable(QStringLiteral("viewport %1, row %2, panel %3x%4")
                         .arg(list->viewport()->height()).arg(list->rowHeight(0)).arg(panel.width()).arg(panel.height())));
            QVERIFY(list->verticalScrollBar()->maximum() > 0);
            list->scrollToBottom();
            QCOMPARE(list->verticalScrollBar()->value(), list->verticalScrollBar()->maximum());
            QVERIFY(panel.findChild<QPushButton*>("previewTimeline")->isVisible());
            QVERIFY(panel.findChild<QCheckBox*>("previewMotionFades")->isVisible());
        }
    }

    void motionTimingEditsKeepPlayheadPositionConsistent()
    {
        CMotionPanel panel;
        QVERIFY(panel.RestoreTimelineState(settingsState()));
        auto* slider = panel.findChild<QSlider*>("motionTimelinePlayhead");
        slider->setValue(500);
        QJsonObject before;
        QVERIFY(panel.CaptureTimelineState(&before));
        QTimer::singleShot(0, [](){
            auto* dialog = qobject_cast<CMotionSettingsDialog*>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            const auto closeDialog = qScopeGuard([dialog](){ if (dialog->isVisible()) dialog->reject(); });
            dialog->findChild<QDoubleSpinBox*>("motionDuration")->setValue(6);
            dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
        });
        panel.findChild<QPushButton*>("editMotionSettings")->click();
        QCOMPARE(slider->value(), 250);
        QJsonObject state;
        QVERIFY(panel.CaptureTimelineState(&state));
        QCOMPARE(state.value("playhead"), before.value("playhead"));
        panel.UndoTimeline();
        QCOMPARE(slider->value(), 500);
        panel.RedoTimeline();
        QCOMPARE(slider->value(), 250);
    }

    void motionSettingsUntouchedFieldsRetainPrecisionAndExtensions()
    {
        CMotionTimeline timeline;
        QVERIFY(timeline.RestoreState(settingsState()));
        QJsonObject before;
        QVERIFY(timeline.SaveState(&before, false, 0));
        CMotionSettingsDialog dialog(timeline);
        dialog.show();
        auto* tracks = dialog.findChild<QComboBox*>("motionFadeTrack");
        QVERIFY(tracks);
        QCOMPARE(tracks->count(), 3);
        for (int i = 0; i < tracks->count(); ++i) tracks->setCurrentIndex(i);
        dialog.findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
        QCOMPARE(dialog.result(), QDialog::Accepted);
        QJsonObject after;
        QVERIFY(dialog.Timeline().SaveState(&after, false, 0));
        QCOMPARE(after, before);
    }

    void motionSettingsEditEventsCommitsActiveEditorAndPreservesExtensions()
    {
        CMotionTimeline timeline;
        QVERIFY(timeline.RestoreState(settingsState()));
        CMotionSettingsDialog dialog(timeline, 0.75);
        dialog.resize(640, 480);
        dialog.show();
        dialog.findChild<QTabWidget*>("motionSettingsTabs")->setCurrentIndex(2);
        auto* table = dialog.findChild<QTableWidget*>("motionEventTable");
        auto* add = dialog.findChild<QPushButton*>("addMotionEvent");
        auto* remove = dialog.findChild<QPushButton*>("removeMotionEvent");
        QVERIFY(table && add && remove);
        QCOMPARE(table->rowCount(), 1);
        table->item(0, 1)->setText(QStringLiteral("修改后的事件 🎵"));
        add->click();
        QCOMPARE(table->rowCount(), 2);
        QCOMPARE(table->item(1, 0)->text().toDouble(), 0.75);
        table->item(1, 0)->setText("2.5");
        table->item(1, 1)->setText(QStringLiteral("删除此事件"));
        table->setCurrentCell(1, 1);
        remove->click();
        QCOMPARE(table->rowCount(), 1);
        add->click();
        table->item(1, 0)->setText("1.25");
        QLineEdit* editor = nullptr;
        for (auto* candidate : table->findChildren<QLineEdit*>())
            if (candidate->isVisible()) editor = candidate;
        QVERIFY(editor);
        editor->setText(QStringLiteral("嘴型：あいう 🌸"));
        dialog.findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
        QCOMPARE(dialog.result(), QDialog::Accepted);
        const QJsonArray events = dialog.Timeline().Events();
        QCOMPARE(events.size(), 2);
        QCOMPARE(events.first().toObject().value("Time").toDouble(), 0.123456789012345);
        QCOMPARE(events.first().toObject().value("Value").toString(), QStringLiteral("修改后的事件 🎵"));
        QCOMPARE(events.first().toObject().value("Extension").toObject().value("payload").toInt(), 7);
        QCOMPARE(events.last().toObject().value("Time").toDouble(), 1.25);
        QCOMPARE(events.last().toObject().value("Value").toString(), QStringLiteral("嘴型：あいう 🌸"));
        QCOMPARE(timeline.Events().first().toObject().value("Value").toString(), QStringLiteral("原始事件"));
        QTemporaryDir temp;
        const QString exported = temp.filePath("events.motion3.json");
        QVERIFY(dialog.Timeline().Save(exported, dialog.Timeline().Fps(), false));
        QFile file(exported);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
        QCOMPARE(root.value("UserData").toArray(), events);
        const int bytes = QStringLiteral("修改后的事件 🎵嘴型：あいう 🌸").toUtf8().size();
        QCOMPARE(root.value("Meta").toObject().value("TotalUserDataSize").toInt(), bytes);
        QCOMPARE(root.value("Meta").toObject().value("UserDataCount").toInt(), 2);
        QCOMPARE(root.value("RootExtension").toString(), QStringLiteral("keep"));
        live2dstruct::CModelSession session;
        QVERIFY(dialog.Timeline().SaveState(&session.animation, false, 0));
        QVERIFY(session.Save(temp.filePath("events.session.json")));
        live2dstruct::CModelSession restored;
        QVERIFY(live2dstruct::CModelSession::Load(temp.filePath("events.session.json"), &restored));
        CMotionTimeline recovered;
        QVERIFY(recovered.RestoreState(restored.animation));
        QCOMPARE(recovered.Events(), events);
    }

    void motionSettingsKeepTypedTrackFadesIndependent()
    {
        CMotionTimeline timeline;
        QVERIFY(timeline.RestoreState(settingsState()));
        CMotionSettingsDialog dialog(timeline);
        dialog.show();
        dialog.findChild<QDoubleSpinBox*>("motionFps")->setValue(24);
        dialog.findChild<QDoubleSpinBox*>("motionDuration")->setValue(5);
        dialog.findChild<QCheckBox*>("motionFadeInEnabled")->setChecked(false);
        dialog.findChild<QCheckBox*>("motionFadeOutEnabled")->setChecked(true);
        dialog.findChild<QDoubleSpinBox*>("motionFadeOut")->setValue(0.5);
        dialog.findChild<QTabWidget*>("motionSettingsTabs")->setCurrentIndex(1);
        auto* tracks = dialog.findChild<QComboBox*>("motionFadeTrack");
        QVERIFY(tracks);
        const CMotionTrack model{CMotionTarget::Model, "Opacity"};
        const CMotionTrack parameter{CMotionTarget::Parameter, "Opacity"};
        const CMotionTrack part{CMotionTarget::PartOpacity, "Opacity"};
        QVERIFY(trackIndex(tracks, model) >= 0);
        QVERIFY(trackIndex(tracks, parameter) >= 0);
        QVERIFY(trackIndex(tracks, part) >= 0);
        tracks->setCurrentIndex(trackIndex(tracks, model));
        dialog.findChild<QDoubleSpinBox*>("curveFadeIn")->setValue(0.25);
        dialog.findChild<QCheckBox*>("curveFadeOutEnabled")->setChecked(true);
        dialog.findChild<QCheckBox*>("curveFadeOutEnabled")->setChecked(false);
        tracks->setCurrentIndex(trackIndex(tracks, parameter));
        dialog.findChild<QDoubleSpinBox*>("curveFadeIn")->setValue(0.75);
        dialog.findChild<QCheckBox*>("curveFadeOutEnabled")->setChecked(true);
        dialog.findChild<QDoubleSpinBox*>("curveFadeOut")->setValue(0.9);
        tracks->setCurrentIndex(trackIndex(tracks, part));
        dialog.findChild<QCheckBox*>("curveFadeInEnabled")->setChecked(false);
        dialog.findChild<QCheckBox*>("curveFadeOutEnabled")->setChecked(true);
        dialog.findChild<QCheckBox*>("curveFadeOutEnabled")->setChecked(false);
        tracks->setCurrentIndex(trackIndex(tracks, model));
        QCOMPARE(dialog.findChild<QDoubleSpinBox*>("curveFadeIn")->value(), 0.25);
        QVERIFY(!dialog.findChild<QCheckBox*>("curveFadeOutEnabled")->isChecked());
        dialog.findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
        QCOMPARE(dialog.result(), QDialog::Accepted);
        const auto& result = dialog.Timeline();
        QCOMPARE(result.Fps(), 24.0f);
        QCOMPARE(result.Duration(), 5.0f);
        QVERIFY(!result.FadeTimes().fadeIn);
        QVERIFY(result.FadeTimes().fadeOut);
        QCOMPARE(*result.FadeTimes().fadeOut, 0.5);
        QVERIFY(result.TrackFadeTimes(model).fadeIn);
        QCOMPARE(*result.TrackFadeTimes(model).fadeIn, 0.25);
        QVERIFY(!result.TrackFadeTimes(model).fadeOut);
        QCOMPARE(*result.TrackFadeTimes(parameter).fadeIn, 0.75);
        QCOMPARE(*result.TrackFadeTimes(parameter).fadeOut, 0.9);
        QVERIFY(!result.TrackFadeTimes(part).fadeIn);
        QVERIFY(!result.TrackFadeTimes(part).fadeOut);
        QJsonObject state;
        QVERIFY(result.SaveState(&state, false, 0));
        for (const auto& value : state.value("motion").toObject().value("Curves").toArray()) {
            const auto curve = value.toObject();
            QCOMPARE(curve.value("Extension"), curve.value("Target"));
        }
    }

    void motionSettingsRejectInvalidEventWithoutChangingCandidate()
    {
        CMotionTimeline timeline;
        QVERIFY(timeline.RestoreState(settingsState()));
        QJsonObject before;
        QVERIFY(timeline.SaveState(&before, false, 0));
        CMotionSettingsDialog dialog(timeline);
        dialog.show();
        auto* tabs = dialog.findChild<QTabWidget*>("motionSettingsTabs");
        dialog.findChild<QDoubleSpinBox*>("motionFps")->setValue(24);
        dialog.findChild<QCheckBox*>("motionFadeInEnabled")->setChecked(false);
        tabs->setCurrentIndex(2);
        auto* table = dialog.findChild<QTableWidget*>("motionEventTable");
        auto* error = dialog.findChild<QLabel*>("motionSettingsError");
        QVERIFY(table && error);
        auto* ok = dialog.findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok);
        for (const QString& invalid : {QStringLiteral("-1"), QStringLiteral("NaN"), QStringLiteral("not a time")}) {
            table->item(0, 0)->setText(invalid);
            ok->click();
            QVERIFY(dialog.isVisible());
            QVERIFY(error->isVisible());
            QVERIFY(!error->text().isEmpty());
            QJsonObject after;
            QVERIFY(dialog.Timeline().SaveState(&after, false, 0));
            QCOMPARE(after, before);
        }
        table->item(0, 0)->setText("0.5");
        table->item(0, 1)->setText(QStringLiteral("修正"));
        ok->click();
        QCOMPARE(dialog.result(), QDialog::Accepted);
        QCOMPARE(dialog.Timeline().Events().first().toObject().value("Time").toDouble(), 0.5);
    }

    void motionSettingsPanelCancelAndUndoRemainTransactional()
    {
        CMotionPanel panel;
        QVERIFY(panel.RestoreTimelineState(settingsState()));
        panel.resize(900, 640);
        panel.show();
        QJsonObject before;
        QVERIFY(panel.CaptureTimelineState(&before));
        auto* settings = panel.findChild<QPushButton*>("editMotionSettings");
        QVERIFY(settings);
        QTimer::singleShot(0, [](){
            auto* dialog = qobject_cast<CMotionSettingsDialog*>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            const auto closeDialog = qScopeGuard([dialog](){ if (dialog->isVisible()) dialog->reject(); });
            dialog->findChild<QDoubleSpinBox*>("motionFps")->setValue(12);
            dialog->findChild<QPushButton*>("addMotionEvent")->click();
            dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Cancel)->click();
        });
        settings->click();
        QJsonObject cancelled;
        QVERIFY(panel.CaptureTimelineState(&cancelled));
        QCOMPARE(cancelled, before);
        QVERIFY(!panel.HasUnsavedTimelineChanges());
        QVERIFY(!panel.CanUndoTimeline());
        QTimer::singleShot(0, [](){
            auto* dialog = qobject_cast<CMotionSettingsDialog*>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            const auto closeDialog = qScopeGuard([dialog](){ if (dialog->isVisible()) dialog->reject(); });
            dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
        });
        settings->click();
        QVERIFY(!panel.HasUnsavedTimelineChanges());
        QVERIFY(!panel.CanUndoTimeline());
        auto* timelineTable = panel.findChild<QTableWidget*>("motionTimelineTable");
        timelineTable->setCurrentCell(0, 2);
        timelineTable->editItem(timelineTable->item(0, 2));
        auto* editor = timelineTable->findChild<QLineEdit*>();
        QVERIFY(editor);
        editor->setText("0.35");
        QTimer::singleShot(0, [](){
            auto* dialog = qobject_cast<CMotionSettingsDialog*>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            const auto closeDialog = qScopeGuard([dialog](){ if (dialog->isVisible()) dialog->reject(); });
            QCOMPARE(dialog->Timeline().Evaluate({CMotionTarget::Model, "Opacity"}, 0), 0.35f);
            dialog->findChild<QDoubleSpinBox*>("motionFps")->setValue(24);
            dialog->findChild<QCheckBox*>("motionFadeInEnabled")->setChecked(false);
            auto* events = dialog->findChild<QTableWidget*>("motionEventTable");
            events->item(0, 1)->setText(QStringLiteral("保存事件"));
            dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
        });
        settings->click();
        QVERIFY(panel.HasUnsavedTimelineChanges());
        QJsonObject changed;
        QVERIFY(panel.CaptureTimelineState(&changed));
        panel.MarkTimelineSaved();
        QVERIFY(!panel.HasUnsavedTimelineChanges());
        panel.UndoTimeline();
        QJsonObject undone;
        QVERIFY(panel.CaptureTimelineState(&undone));
        CMotionTimeline restored;
        QVERIFY(restored.RestoreState(undone));
        QCOMPARE(restored.Evaluate({CMotionTarget::Model, "Opacity"}, 0), 0.35f);
        QCOMPARE(restored.Events().first().toObject().value("Value").toString(), QStringLiteral("原始事件"));
        QVERIFY(restored.FadeTimes().fadeIn);
        QVERIFY(panel.HasUnsavedTimelineChanges());
        panel.UndoTimeline();
        QVERIFY(panel.CaptureTimelineState(&undone));
        QCOMPARE(undone, before);
        QVERIFY(!panel.CanUndoTimeline());
        panel.RedoTimeline();
        panel.RedoTimeline();
        QVERIFY(panel.CaptureTimelineState(&undone));
        QCOMPARE(undone, changed);
        QVERIFY(!panel.HasUnsavedTimelineChanges());
    }

    void motionSettingsLargeEventListFitsBothThemes()
    {
        CMotionTimeline timeline;
        QVERIFY(timeline.RestoreState(settingsState()));
        QJsonArray events;
        for (int i = 0; i < 80; ++i)
            events.append(QJsonObject{{"Time", i / 40.0}, {"Value", QStringLiteral("事件 %1 · 音频与表情").arg(i)}});
        QVERIFY(timeline.SetEvents(events));
        for (bool dark : {false, true}) {
            CMotionSettingsDialog dialog(timeline);
            dialog.setStyleSheet(Theme::globalStyleSheet(dark));
            dialog.resize(640, 480);
            dialog.show();
            auto* tabs = dialog.findChild<QTabWidget*>("motionSettingsTabs");
            auto* table = dialog.findChild<QTableWidget*>("motionEventTable");
            QVERIFY(tabs && table);
            tabs->setCurrentIndex(2);
            QCoreApplication::processEvents();
            QVERIFY2(dialog.width() <= 680, qPrintable(QString::number(dialog.width())));
            QVERIFY2(dialog.height() <= 520, qPrintable(QString::number(dialog.height())));
            QVERIFY(table->viewport()->height() >= table->rowHeight(0) * 3);
            QVERIFY(table->verticalScrollBar()->maximum() > 0);
            table->scrollToBottom();
            QCOMPARE(table->verticalScrollBar()->value(), table->verticalScrollBar()->maximum());
            auto* buttons = dialog.findChild<QDialogButtonBox*>();
            QVERIFY(buttons->button(QDialogButtonBox::Ok)->isVisible());
            QVERIFY(buttons->button(QDialogButtonBox::Cancel)->isVisible());
            if (!qEnvironmentVariableIsEmpty("MOTION_SETTINGS_SCREENSHOT")) {
                const QString output = qEnvironmentVariable("MOTION_SETTINGS_SCREENSHOT") +
                    (dark ? QStringLiteral("-dark.png") : QStringLiteral("-light.png"));
                QVERIFY(dialog.grab().save(output));
                for (int page : {0, 1}) {
                    tabs->setCurrentIndex(page);
                    QCoreApplication::processEvents();
                    QVERIFY(dialog.grab().save(output + QStringLiteral(".%1.png").arg(page)));
                }
            }
        }
    }

    void playbackUsesElapsedTimeAndSelectedSpeed_data()
    {
        QTest::addColumn<int>("percent");
        QTest::newRow("slow") << 50;
        QTest::newRow("fast") << 200;
    }

    void playbackUsesElapsedTimeAndSelectedSpeed()
    {
        QFETCH(int, percent);
        CMotionPanel panel;
        QVERIFY(panel.AddTimelinePoint({CMotionTarget::Model, "Opacity"}, 0, 0));
        QVERIFY(panel.AddTimelinePoint({CMotionTarget::Model, "Opacity"}, 10, 10));
        panel.findChild<QSlider*>("motionPlaybackSpeed")->setValue(percent);
        auto* play = panel.findChild<QPushButton*>("previewTimeline");
        QSignalSpy preview(&panel, &CMotionPanel::TimelineFrameChanged);
        QElapsedTimer elapsed;
        elapsed.start();
        play->click();
        QTest::qSleep(140);
        const double lowerTime = elapsed.nsecsElapsed() / 1e9;
        QCoreApplication::processEvents();
        const double upperTime = elapsed.nsecsElapsed() / 1e9;
        const double value = preview.last().first().value<CMotionPreviewFrame>().model.value("Opacity");
        QVERIFY2(value >= lowerTime * percent / 100.0 - 0.015, qPrintable(QString::number(value)));
        QVERIFY(value <= upperTime * percent / 100.0 + 0.015);
        play->click();
        const float paused = preview.last().first().value<CMotionPreviewFrame>().model.value("Opacity");
        QTest::qSleep(40);
        play->click();
        QCOMPARE(preview.last().first().value<CMotionPreviewFrame>().model.value("Opacity"), paused);
        QTest::qSleep(70);
        panel.findChild<QSlider*>("motionPlaybackSpeed")->setValue(100);
        const float switched = preview.last().first().value<CMotionPreviewFrame>().model.value("Opacity");
        QVERIFY(switched > paused);
        QElapsedTimer afterSwitch;
        afterSwitch.start();
        QTest::qSleep(90);
        const double minimumAdvance = afterSwitch.nsecsElapsed() / 1e9;
        QCoreApplication::processEvents();
        const double maximumAdvance = afterSwitch.nsecsElapsed() / 1e9;
        const float advanced = preview.last().first().value<CMotionPreviewFrame>().model.value("Opacity") - switched;
        QVERIFY(advanced >= minimumAdvance - 0.015);
        QVERIFY(advanced <= maximumAdvance + 0.015);
        panel.StopTimelinePreview();
    }

    void playbackContinuesFromPlayheadAndFinishesAtClipEnd()
    {
        CMotionPanel panel;
        QVERIFY(panel.AddTimelinePoint({CMotionTarget::Model, "Opacity"}, 0, 0));
        QVERIFY(panel.AddTimelinePoint({CMotionTarget::Model, "Opacity"}, 0.1f, 1));
        auto* slider = panel.findChild<QSlider*>("motionTimelinePlayhead");
        slider->setValue(500);
        auto* play = panel.findChild<QPushButton*>("previewTimeline");
        QSignalSpy preview(&panel, &CMotionPanel::TimelineFrameChanged);
        play->click();
        QCOMPARE(preview.last().first().value<CMotionPreviewFrame>().model.value("Opacity"), 0.5f);
        QTest::qSleep(150);
        QCoreApplication::processEvents();
        QCOMPARE(preview.last().first().value<CMotionPreviewFrame>().model.value("Opacity"), 1.0f);
        QCOMPARE(slider->value(), 1000);
        QCOMPARE(play->text(), QStringLiteral("预览"));
        play->click();
        QCOMPARE(preview.last().first().value<CMotionPreviewFrame>().model.value("Opacity"), 0.0f);
        panel.findChild<QCheckBox*>("motionLoop")->setChecked(true);
        QTest::qSleep(150);
        QCoreApplication::processEvents();
        const float value = preview.last().first().value<CMotionPreviewFrame>().model.value("Opacity");
        QVERIFY(value >= 0.0f && value < 1.0f);
        QCOMPARE(play->text(), QStringLiteral("暂停"));
        panel.StopTimelinePreview();
    }

    void modelOnlyClipPreviewsWithEmptyParameterFilterAndStopsRecording()
    {
        CMotionPanel panel;
        QVERIFY(panel.AddTimelinePoint({CMotionTarget::Model, "Opacity"}, 0, 0));
        QVERIFY(panel.AddTimelinePoint({CMotionTarget::Model, "Opacity"}, 2, 1));
        auto* filter = panel.findChild<QComboBox*>("motionTargetFilter");
        filter->setCurrentIndex(filter->findData(static_cast<int>(CMotionTarget::Parameter)));
        QCOMPARE(panel.findChild<QTableWidget*>("motionTimelineTable")->rowCount(), 0);
        auto* slider = panel.findChild<QSlider*>("motionTimelinePlayhead");
        auto* play = panel.findChild<QPushButton*>("previewTimeline");
        auto* record = panel.findChild<QCheckBox*>("recordTimeline");
        QVERIFY(slider->isEnabled());
        QVERIFY(play->isEnabled());
        QSignalSpy preview(&panel, &CMotionPanel::TimelineFrameChanged);
        record->setChecked(true);
        slider->setValue(500);
        QVERIFY(!record->isChecked());
        QCOMPARE(preview.last().first().value<CMotionPreviewFrame>().model.value("Opacity"), 0.5f);
        record->setChecked(true);
        play->click();
        QVERIFY(!record->isChecked());
        panel.StopTimelinePreview();
        const int count = preview.size();
        QTest::qWait(80);
        QCOMPARE(preview.size(), count);
    }


    void typedTracksFilterEditUndoAndPreview()
    {
        CMotionPanel panel;
        const CMotionTrack model{CMotionTarget::Model, "Opacity"};
        const CMotionTrack parameter{CMotionTarget::Parameter, "Opacity"};
        const CMotionTrack part{CMotionTarget::PartOpacity, "Opacity"};
        QSignalSpy preview(&panel, &CMotionPanel::TimelineFrameChanged);
        QSignalSpy stopped(&panel, &CMotionPanel::TimelinePreviewStopped);
        for (const auto& track : {model, parameter, part}) {
            QVERIFY(panel.AddTimelinePoint(track, 0, 0));
            QVERIFY(panel.AddTimelinePoint(track, 2, 1));
        }
        auto* table = panel.findChild<QTableWidget*>("motionTimelineTable");
        auto* filter = panel.findChild<QComboBox*>("motionTargetFilter");
        auto* slider = panel.findChild<QSlider*>("motionTimelinePlayhead");
        QCOMPARE(table->rowCount(), 6);
        slider->setValue(500);
        auto frame = preview.last().first().value<CMotionPreviewFrame>();
        QCOMPARE(frame.parameters.value("Opacity"), 0.5f);
        QCOMPARE(frame.model.value("Opacity"), 0.5f);
        QCOMPARE(frame.parts.value("Opacity"), 0.5f);
        filter->setCurrentIndex(filter->findData(static_cast<int>(CMotionTarget::PartOpacity)));
        QCOMPARE(table->rowCount(), 2);
        QVERIFY(table->item(0, 1)->text().startsWith("PartOpacity"));
        table->item(1, 2)->setText("0.25");
        frame = preview.last().first().value<CMotionPreviewFrame>();
        QCOMPARE(frame.parts.value("Opacity"), 0.125f);
        QCOMPARE(frame.model.value("Opacity"), 0.5f);
        panel.UndoTimeline();
        QCOMPARE(table->item(1, 2)->text().toFloat(), 1.0f);
        panel.RedoTimeline();
        QCOMPARE(table->item(1, 2)->text().toFloat(), 0.25f);
        QVERIFY(panel.AddTimelinePoint(model, 1, 0.75f));
        QCOMPARE(filter->currentData().toInt(), static_cast<int>(CMotionTarget::Model));
        QCOMPARE(table->rowCount(), 3);
        QCOMPARE(table->currentRow(), 1);
        QCOMPARE(table->item(1, 2)->text().toFloat(), 0.75f);
        panel.findChild<QPushButton*>("removeTimelineKeyframe")->click();
        QCOMPARE(table->rowCount(), 2);
        panel.UndoTimeline();
        QCOMPARE(table->rowCount(), 3);
        panel.findChild<QPushButton*>("endTimelinePreview")->click();
        QCOMPARE(stopped.size(), 1);
        QVERIFY(!panel.findChild<QPushButton*>("endTimelinePreview")->isEnabled());
        auto* record = panel.findChild<QCheckBox*>("recordTimeline");
        record->setChecked(true);
        QCOMPARE(stopped.size(), 2);
        QVERIFY(!panel.findChild<QPushButton*>("addTimelineKeyframe")->isEnabled());
        QVERIFY(!panel.AddTimelinePoint(part, 1, 0.5f));
        record->setChecked(false);
        QJsonObject state;
        QVERIFY(panel.CaptureTimelineState(&state));
        CMotionPanel restored;
        QVERIFY(restored.RestoreTimelineState(state));
        QCOMPARE(restored.findChild<QTableWidget*>("motionTimelineTable")->rowCount(), 7);
        QTemporaryDir temp;
        QVERIFY(restored.SaveTimeline(temp.filePath("typed.motion3.json")));
        CMotionTimeline exported;
        QVERIFY(exported.Load(temp.filePath("typed.motion3.json")));
        QCOMPARE(exported.Evaluate(model, 1), 0.75f);
        QCOMPARE(exported.Evaluate(part, 1), 0.125f);
    }

    void manualDialogAddsAllTypesAndCancelsCleanly()
    {
        CMotionPanel panel;
        panel.SetParameterIds({"ParamA"});
        QTemporaryDir temp;
        QFile modelFile(temp.filePath("model.model3.json"));
        QVERIFY(modelFile.open(QIODevice::WriteOnly));
        modelFile.write("{\"Version\":3,\"FileReferences\":{}}");
        modelFile.close();
        QVERIFY(panel.BuildLists(modelFile.fileName(), {"PartA"}));
        for (const auto kind : {CMotionTarget::Parameter, CMotionTarget::Model, CMotionTarget::PartOpacity}) {
            QTimer::singleShot(0, [kind](){
                auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
                QVERIFY(dialog);
                auto* target = dialog->findChild<QComboBox*>("keyframeTarget");
                auto* id = dialog->findChild<QComboBox*>("keyframeId");
                target->setCurrentIndex(target->findData(static_cast<int>(kind)));
                id->setCurrentText(kind == CMotionTarget::Model ? "Opacity" : kind == CMotionTarget::Parameter ? "ParamA" : "PartA");
                dialog->findChild<QDoubleSpinBox*>("keyframeTime")->setValue(0.5);
                dialog->findChild<QDoubleSpinBox*>("keyframeValue")->setValue(0.25);
                dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
            });
            panel.findChild<QPushButton*>("addTimelineKeyframe")->click();
        }
        auto* table = panel.findChild<QTableWidget*>("motionTimelineTable");
        QCOMPARE(table->rowCount(), 3);
        QTimer::singleShot(0, [](){
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            dialog->reject();
        });
        panel.findChild<QPushButton*>("addTimelineKeyframe")->click();
        QCOMPARE(table->rowCount(), 3);
        panel.UndoTimeline();
        QCOMPARE(table->rowCount(), 2);
        panel.UndoTimeline();
        QCOMPARE(table->rowCount(), 1);
        panel.UndoTimeline();
        QCOMPARE(table->rowCount(), 0);
        QVERIFY(!panel.HasUnsavedTimelineChanges());
    }
    void undoRedoTracksSavedPositionAndBranches()
    {
        QTemporaryDir temp;
        const QString path = temp.filePath("input.json");
        QVERIFY(makeClip(path));
        CMotionPanel panel;
        QVERIFY(panel.LoadTimeline(path));
        auto* table = panel.findChild<QTableWidget*>("motionTimelineTable");
        QVERIFY(!panel.CanUndoTimeline());
        table->item(1, 2)->setText("2");
        QVERIFY(panel.CanUndoTimeline());
        QVERIFY(panel.HasUnsavedTimelineChanges());
        panel.UndoTimeline();
        QCOMPARE(table->item(1, 2)->text().toFloat(), 1.0f);
        QVERIFY(!panel.CanUndoTimeline());
        QVERIFY(panel.CanRedoTimeline());
        QVERIFY(!panel.HasUnsavedTimelineChanges());
        panel.RedoTimeline();
        QCOMPARE(table->item(1, 2)->text().toFloat(), 2.0f);
        QVERIFY(panel.HasUnsavedTimelineChanges());
        QVERIFY(!panel.SaveTimeline(temp.path()));
        QVERIFY(panel.HasUnsavedTimelineChanges());
        QVERIFY(panel.SaveTimeline(temp.filePath("saved.json")));
        QVERIFY(!panel.HasUnsavedTimelineChanges());
        panel.UndoTimeline();
        QVERIFY(panel.HasUnsavedTimelineChanges());
        panel.RedoTimeline();
        QVERIFY(!panel.HasUnsavedTimelineChanges());
        panel.UndoTimeline();
        table->item(1, 2)->setText("3");
        QVERIFY(!panel.CanRedoTimeline());
        QVERIFY(panel.HasUnsavedTimelineChanges());
        panel.UndoTimeline();
        QCOMPARE(table->item(1, 2)->text().toFloat(), 1.0f);
        QVERIFY(panel.HasUnsavedTimelineChanges());
    }

    void recordingIsOneEditAndSaveCreatesCheckpoint()
    {
        CMotionPanel panel;
        auto* recording = panel.findChild<QCheckBox*>("recordTimeline");
        auto* table = panel.findChild<QTableWidget*>("motionTimelineTable");
        recording->setChecked(true);
        panel.RecordParameterValue("A", 0);
        panel.RecordParameterValue("B", 3);
        panel.RecordParameterValue("A", 1);
        QCOMPARE(table->rowCount(), 3);
        QVERIFY(panel.CanUndoTimeline());
        QJsonObject recorded;
        QVERIFY(panel.CaptureTimelineState(&recorded));
        panel.UndoTimeline();
        QVERIFY(!recording->isChecked());
        QCOMPARE(table->rowCount(), 0);
        QVERIFY(!panel.CanUndoTimeline());
        QVERIFY(!panel.HasUnsavedTimelineChanges());
        panel.RedoTimeline();
        QCOMPARE(table->rowCount(), 3);
        QJsonObject redone;
        QVERIFY(panel.CaptureTimelineState(&redone));
        QCOMPARE(redone, recorded);
        QVERIFY(panel.RestoreTimelineState({}));
        recording->setChecked(true);
        panel.RecordParameterValue("A", 0);
        panel.MarkTimelineSaved();
        QVERIFY(!panel.HasUnsavedTimelineChanges());
        panel.RecordParameterValue("A", 1);
        QVERIFY(panel.HasUnsavedTimelineChanges());
        panel.UndoTimeline();
        QCOMPARE(table->rowCount(), 1);
        QVERIFY(!panel.HasUnsavedTimelineChanges());
        panel.RedoTimeline();
        QCOMPARE(table->rowCount(), 2);
        QVERIFY(panel.HasUnsavedTimelineChanges());
    }

    void curveDragIsOneUndoStep()
    {
        QTemporaryDir temp;
        const QString path = temp.filePath("input.json");
        QVERIFY(makeClip(path));
        CMotionPanel panel;
        QVERIFY(panel.LoadTimeline(path));
        panel.resize(1000, 850);
        panel.show();
        auto* view = panel.findChild<CMotionCurveView*>("motionCurveView");
        QVERIFY(view);
        view->setFixedSize(320, 190);
        QJsonObject before;
        QVERIFY(panel.CaptureTimelineState(&before));
        QSignalSpy activated(&panel, &CMotionPanel::TimelineActivated);
        QTest::mousePress(view, Qt::LeftButton, Qt::NoModifier, QPoint(34, 161));
        for (const QPointF& point : {QPointF(70, 135), QPointF(90, 120), QPointF(115, 110)}) {
            QMouseEvent move(QEvent::MouseMove, point, point, point, Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
            QCoreApplication::sendEvent(view, &move);
        }
        QTest::mouseRelease(view, Qt::LeftButton, Qt::NoModifier, QPoint(115, 110));
        QVERIFY(!activated.isEmpty());
        QVERIFY(panel.IsTimelineWidget(view));
        QVERIFY(panel.CanUndoTimeline());
        QJsonObject after;
        QVERIFY(panel.CaptureTimelineState(&after));
        QVERIFY(after != before);
        panel.UndoTimeline();
        QVERIFY(!panel.CanUndoTimeline());
        QJsonObject undone;
        QVERIFY(panel.CaptureTimelineState(&undone));
        QCOMPARE(undone, before);
        panel.RedoTimeline();
        QVERIFY(panel.CaptureTimelineState(&undone));
        QCOMPARE(undone, after);
    }

    void undoPreservesBezierEventsAndClearCanBeReversed()
    {
        CMotionPanel panel;
        QString error;
        QVERIFY(panel.LoadTimeline(QString(TEST_RESOURCES) + "/Mark/motions/mark_m06.motion3.json", &error));
        QJsonObject before;
        QVERIFY(panel.CaptureTimelineState(&before));
        auto* table = panel.findChild<QTableWidget*>("motionTimelineTable");
        int row = -1;
        for (int i = 0; i < table->rowCount(); ++i)
            if (table->item(i, 3)->flags() & Qt::ItemIsEditable) { row = i; break; }
        QVERIFY(row >= 0);
        table->item(row, 3)->setText("2");
        QVERIFY(panel.CanUndoTimeline());
        panel.UndoTimeline();
        QJsonObject restored;
        QVERIFY(panel.CaptureTimelineState(&restored));
        QCOMPARE(restored, before);
        table->setCurrentCell(row, 0);
        panel.findChild<QPushButton*>("removeTimelineKeyframe")->click();
        QVERIFY(panel.CanUndoTimeline());
        panel.UndoTimeline();
        QVERIFY(panel.CaptureTimelineState(&restored));
        QCOMPARE(restored, before);
        panel.findChild<QPushButton*>("clearMotionTimeline")->click();
        QCOMPARE(table->rowCount(), 0);
        QVERIFY(panel.CanUndoTimeline());
        panel.MarkTimelineSaved();
        panel.UndoTimeline();
        QVERIFY(panel.HasUnsavedTimelineChanges());
        QVERIFY(panel.CaptureTimelineState(&restored));
        QCOMPARE(restored, before);
        panel.RedoTimeline();
        QCOMPARE(table->rowCount(), 0);
        QVERIFY(!panel.HasUnsavedTimelineChanges());
    }

    void loopAndUncommittedEditsUseTimelineHistory()
    {
        QTemporaryDir temp;
        const QString path = temp.filePath("input.json");
        QVERIFY(makeClip(path));
        CMotionPanel panel;
        QVERIFY(panel.LoadTimeline(path));
        panel.resize(1000, 850);
        panel.show();
        auto* loop = panel.findChild<QCheckBox*>("motionLoop");
        loop->setChecked(true);
        QVERIFY(panel.IsTimelineWidget(loop));
        panel.UndoTimeline();
        QVERIFY(!loop->isChecked());
        QVERIFY(!panel.HasUnsavedTimelineChanges());
        panel.RedoTimeline();
        QVERIFY(loop->isChecked());
        auto* table = panel.findChild<QTableWidget*>("motionTimelineTable");
        table->setCurrentCell(1, 2);
        table->editItem(table->item(1, 2));
        auto* editor = table->findChild<QLineEdit*>();
        QVERIFY(editor);
        editor->setText("8");
        QVERIFY(panel.HasUnsavedTimelineChanges());
        panel.UndoTimeline();
        QCOMPARE(table->item(1, 2)->text().toFloat(), 1.0f);
        panel.RedoTimeline();
        QCOMPARE(table->item(1, 2)->text().toFloat(), 8.0f);
        QJsonObject snapshot;
        QVERIFY(panel.CaptureTimelineState(&snapshot));
        QVERIFY(panel.RestoreTimelineState(snapshot));
        QVERIFY(!panel.CanUndoTimeline());
        QVERIFY(!panel.CanRedoTimeline());
        QVERIFY(!panel.HasUnsavedTimelineChanges());
    }

    void rejectedAndEquivalentEditsDoNotCreateHistory()
    {
        QTemporaryDir temp;
        const QString path = temp.filePath("input.json");
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QVERIFY(file.write(R"({"Version":3,"Curves":[{"Target":"Parameter","Id":"A","Segments":[0,0,0,0.1,1]}]})") > 0);
        file.close();
        CMotionPanel panel;
        QVERIFY(panel.LoadTimeline(path));
        auto* table = panel.findChild<QTableWidget*>("motionTimelineTable");
        table->item(1, 2)->setText("1.00");
        QVERIFY(!panel.CanUndoTimeline());
        QVERIFY(!panel.HasUnsavedTimelineChanges());
        table->item(1, 0)->setText("0");
        QVERIFY(!panel.CanUndoTimeline());
        table->item(1, 2)->setText("5");
        panel.UndoTimeline();
        QVERIFY(panel.CanRedoTimeline());
        table->item(1, 3)->setText("bad");
        QVERIFY(panel.CanRedoTimeline());
        QVERIFY(!panel.HasUnsavedTimelineChanges());
    }

    void destroysPanelWithActiveHistoryAndEditor()
    {
        QTemporaryDir temp;
        const QString path = temp.filePath("input.json");
        QVERIFY(makeClip(path));
        for (bool recording : {false, true}) {
            auto* panel = new CMotionPanel();
            QVERIFY(panel->LoadTimeline(path));
            panel->resize(1000, 850);
            panel->show();
            auto* table = panel->findChild<QTableWidget*>("motionTimelineTable");
            table->item(1, 2)->setText("4");
            QVERIFY(panel->CanUndoTimeline());
            if (recording) {
                panel->findChild<QCheckBox*>("recordTimeline")->setChecked(true);
                panel->RecordParameterValue("ParamA", 7);
            } else {
                table->setCurrentCell(1, 2);
                table->editItem(table->item(1, 2));
                auto* editor = table->findChild<QLineEdit*>();
                QVERIFY(editor);
                editor->setText("5");
            }
            delete panel;
            QCoreApplication::processEvents();
        }
    }

    void historyControlsFitBothThemes()
    {
        QTemporaryDir temp;
        const QString path = temp.filePath("input.json");
        QVERIFY(makeClip(path));
        for (bool dark : {false, true}) {
            CMotionPanel panel;
            panel.setStyleSheet(Theme::globalStyleSheet(dark));
            QVERIFY(panel.LoadTimeline(path));
            panel.resize(700, 520);
            panel.show();
            QCoreApplication::processEvents();
            auto* table = panel.findChild<QTableWidget*>("motionTimelineTable");
            auto* undo = panel.findChild<QPushButton*>("undoMotionTimeline");
            auto* redo = panel.findChild<QPushButton*>("redoMotionTimeline");
            QVERIFY(undo && redo);
            QVERIFY2(panel.width() <= 740, qPrintable(QStringLiteral("panel width %1").arg(panel.width())));
            QVERIFY(panel.height() <= 540);
            QVERIFY(table->viewport()->height() >= table->rowHeight(0) * 2);
            QVERIFY(undo->isVisible());
            QVERIFY(redo->isVisible());
            QVERIFY(!undo->isEnabled());
            table->item(1, 2)->setText("2");
            QVERIFY(undo->isEnabled());
            undo->click();
            QVERIFY(redo->isEnabled());
            redo->click();
            QCOMPARE(table->item(1, 2)->text().toFloat(), 2.0f);
            if (!qEnvironmentVariableIsEmpty("MOTION_PANEL_SCREENSHOT")) {
                const QString output = qEnvironmentVariable("MOTION_PANEL_SCREENSHOT") +
                    (dark ? QStringLiteral("-dark.png") : QStringLiteral("-light.png"));
                QVERIFY(panel.grab().save(output));
            }
        }
    }

    void layoutRoundTripAndLargePoseRemainUsable()
    {
        QTemporaryDir temp;
        QJsonArray groups;
        for (int i = 0; i < 40; ++i)
            groups.append(QJsonArray{QJsonObject{{"Id", QStringLiteral("PartA%1").arg(i)}},
                                     QJsonObject{{"Id", QStringLiteral("PartB%1").arg(i)}}});
        QFile pose(temp.filePath("large.pose3.json"));
        QVERIFY(pose.open(QIODevice::WriteOnly));
        const QByteArray json = QJsonDocument(QJsonObject{{"Groups", groups}}).toJson();
        QCOMPARE(pose.write(json), json.size());
        pose.close();
        CMotionPanel panel;
        panel.resize(1000, 520);
        panel.show();
        QVERIFY(panel.BuildPoseGroups(pose.fileName()));
        auto* tabs = panel.findChild<QTabWidget*>("animationAssetTabs");
        tabs->setCurrentIndex(2);
        QCoreApplication::processEvents();
        QVERIFY(panel.height() <= 540);
        auto* scroll = qobject_cast<QScrollArea*>(tabs->currentWidget());
        QVERIFY(scroll);
        QVERIFY(scroll->verticalScrollBar()->maximum() > 0);
        scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->maximum());
        auto* split = panel.findChild<QSplitter*>("animationWorkspaceSplitter");
        split->setSizes({300, 700});
        const QByteArray layout = panel.SaveLayout();
        CMotionPanel restored;
        restored.resize(1000, 520);
        restored.show();
        restored.RestoreLayout(layout);
        auto* restoredTabs = restored.findChild<QTabWidget*>("animationAssetTabs");
        QVERIFY(!restoredTabs->isTabVisible(2));
        QVERIFY(restored.BuildPoseGroups(pose.fileName()));
        QCOMPARE(restoredTabs->currentIndex(), 2);
        restoredTabs->setCurrentIndex(1);
        QVERIFY(restored.BuildPoseGroups(pose.fileName()));
        QCOMPARE(restoredTabs->currentIndex(), 1);
        const QByteArray before = restored.SaveLayout();
        restored.RestoreLayout("invalid");
        QCOMPARE(restored.SaveLayout(), before);
    }

    void draftSurvivesSessionAndCatalogRefresh()
    {
        QTemporaryDir temp;
        CMotionPanel panel;
        auto* recording = panel.findChild<QCheckBox*>("recordTimeline");
        auto* table = panel.findChild<QTableWidget*>("motionTimelineTable");
        QVERIFY(recording && table);
        recording->setChecked(true);
        panel.RecordParameterValue("ParamA", 0.5f);
        recording->setChecked(false);
        QVERIFY(panel.HasUnsavedTimelineChanges());
        QCOMPARE(table->rowCount(), 1);
        live2dstruct::CModelSession session;
        QString error;
        QVERIFY(panel.CaptureTimelineState(&session.animation, &error));
        QVERIFY(panel.HasUnsavedTimelineChanges());
        const quint64 revision = panel.TimelineRevision();
        QVERIFY2(panel.BuildLists(QString(TEST_RESOURCES) + "/Mark/Mark.model3.json", {}, &error), qPrintable(error));
        QCOMPARE(panel.TimelineRevision(), revision);
        QCOMPARE(table->rowCount(), 1);
        QVERIFY(panel.HasUnsavedTimelineChanges());
        QVERIFY(!panel.BuildLists(temp.filePath("missing.json"), {}, &error));
        QCOMPARE(table->rowCount(), 1);
        const QString path = temp.filePath("session.json");
        QVERIFY(session.Save(path, &error));
        panel.MarkTimelineSaved();
        QVERIFY(!panel.HasUnsavedTimelineChanges());
        live2dstruct::CModelSession restored;
        QVERIFY(live2dstruct::CModelSession::Load(path, &restored, &error));
        CMotionPanel other;
        QVERIFY(other.RestoreTimelineState(restored.animation, &error));
        QCOMPARE(other.findChild<QTableWidget*>("motionTimelineTable")->rowCount(), 1);
        QJsonObject repeated;
        QVERIFY(other.CaptureTimelineState(&repeated, &error));
        QCOMPARE(repeated, session.animation);
        QVERIFY(!other.HasUnsavedTimelineChanges());
        QVERIFY(other.RestoreTimelineState({}, &error));
        QCOMPARE(other.findChild<QTableWidget*>("motionTimelineTable")->rowCount(), 0);
    }

    void commitsEditorAndRestoresPlaybackSettings()
    {
        QTemporaryDir temp;
        const QString path = temp.filePath("input.motion3.json");
        QVERIFY(makeClip(path));
        CMotionPanel panel;
        QVERIFY(panel.LoadTimeline(path));
        panel.resize(1000, 850);
        panel.show();
        auto* table = panel.findChild<QTableWidget*>("motionTimelineTable");
        table->setCurrentCell(1, 2);
        table->editItem(table->item(1, 2));
        auto* editor = table->findChild<QLineEdit*>();
        QVERIFY(editor);
        editor->setText("0.75");
        QVERIFY(panel.HasUnsavedTimelineChanges());
        QJsonObject state;
        QVERIFY(panel.CaptureTimelineState(&state));
        CMotionTimeline timeline;
        QVERIFY(timeline.RestoreState(state));
        QCOMPARE(timeline.PointCount(), 2);
        QCOMPARE(timeline.Evaluate("ParamA", 1), 0.75f);
        QCOMPARE(timeline.Fps(), 60.0f);
        auto* loop = panel.findChild<QCheckBox*>("motionLoop");
        auto* playhead = panel.findChild<QSlider*>("motionTimelinePlayhead");
        loop->setChecked(true);
        playhead->setValue(500);
        QVERIFY(panel.CaptureTimelineState(&state));
        QVERIFY(!panel.SaveTimeline(temp.path()));
        QVERIFY(panel.HasUnsavedTimelineChanges());
        CMotionPanel restored;
        QVERIFY(restored.RestoreTimelineState(state));
        QVERIFY(restored.findChild<QCheckBox*>("motionLoop")->isChecked());
        QCOMPARE(restored.findChild<QSlider*>("motionTimelinePlayhead")->value(), 500);
        QVERIFY(!restored.HasUnsavedTimelineChanges());
        QVERIFY(restored.SaveTimeline(temp.filePath("export.motion3.json")));
        QVERIFY(!restored.HasUnsavedTimelineChanges());
        QVERIFY(timeline.Load(temp.filePath("export.motion3.json")));
        QCOMPARE(timeline.Evaluate("ParamA", 1), 0.75f);
        QVERIFY(timeline.Loop());
    }

    void replacementRequiresSuccessfulSaveOrDiscard()
    {
        QTemporaryDir temp;
        const QString first = temp.filePath("first.json");
        const QString second = temp.filePath("second.json");
        QVERIFY(makeClip(first, 1));
        QVERIFY(makeClip(second, 2));
        CMotionPanel panel;
        QVERIFY(panel.LoadTimeline(first));
        auto* table = panel.findChild<QTableWidget*>("motionTimelineTable");
        table->item(1, 2)->setText("3");
        QVERIFY(panel.HasUnsavedTimelineChanges());
        QJsonObject before;
        QVERIFY(panel.CaptureTimelineState(&before));
        QString error;
        QVERIFY(!panel.LoadTimeline(temp.filePath("missing.json"), &error));
        answer(QMessageBox::Cancel);
        QVERIFY(!panel.LoadTimeline(second));
        QJsonObject after;
        QVERIFY(panel.CaptureTimelineState(&after));
        QCOMPARE(before, after);
        QSignalSpy requested(&panel, &CMotionPanel::SaveSessionRequested);
        answer(QMessageBox::Save);
        QVERIFY(!panel.LoadTimeline(second));
        QCOMPARE(requested.size(), 1);
        QVERIFY(panel.HasUnsavedTimelineChanges());
        connect(&panel, &CMotionPanel::SaveSessionRequested, &panel, [&](){
            live2dstruct::CModelSession session;
            if (panel.CaptureTimelineState(&session.animation) && session.Save(temp.filePath("saved-session.json")))
                panel.MarkTimelineSaved();
        });
        answer(QMessageBox::Save);
        QVERIFY(panel.LoadTimeline(second));
        QVERIFY(!panel.HasUnsavedTimelineChanges());
        QCOMPARE(table->item(1, 2)->text().toFloat(), 2.0f);
        live2dstruct::CModelSession saved;
        QVERIFY(live2dstruct::CModelSession::Load(temp.filePath("saved-session.json"), &saved));
        CMotionTimeline recovered;
        QVERIFY(recovered.RestoreState(saved.animation));
        QCOMPARE(recovered.Evaluate("ParamA", 1), 3.0f);
        table->item(1, 2)->setText("4");
        auto* playhead = panel.findChild<QSlider*>("motionTimelinePlayhead");
        playhead->setValue(750);
        auto* clear = panel.findChild<QPushButton*>("clearMotionTimeline");
        answer(QMessageBox::Cancel);
        clear->click();
        QCOMPARE(table->rowCount(), 2);
        QCOMPARE(playhead->value(), 750);
        answer(QMessageBox::Discard);
        clear->click();
        QCOMPARE(table->rowCount(), 0);
        QCOMPARE(playhead->value(), 0);
        QVERIFY(panel.HasUnsavedTimelineChanges());
        QVERIFY(panel.CaptureTimelineState(&after));
        QVERIFY(recovered.RestoreState(after));
        QVERIFY(!recovered.HasCurves());
    }

    void editingAndScrubbingInvalidateDelayedRestoreRevision()
    {
        QTemporaryDir temp;
        const QString path = temp.filePath("input.json");
        QVERIFY(makeClip(path));
        CMotionPanel panel;
        QVERIFY(panel.LoadTimeline(path));
        const quint64 restoredRevision = panel.TimelineRevision();
        panel.findChild<QSlider*>("motionTimelinePlayhead")->setValue(250);
        QVERIFY(panel.TimelineRevision() > restoredRevision);
        const quint64 scrubRevision = panel.TimelineRevision();
        panel.findChild<QCheckBox*>("motionLoop")->setChecked(true);
        QVERIFY(panel.TimelineRevision() > scrubRevision);
        QVERIFY(panel.SaveTimeline(temp.filePath("saved.json")));
        QVERIFY(!panel.HasUnsavedTimelineChanges());
        QVERIFY(panel.TimelineRevision() > restoredRevision);
        const quint64 savedRevision = panel.TimelineRevision();
        panel.findChild<QCheckBox*>("recordTimeline")->setChecked(true);
        QVERIFY(panel.TimelineRevision() > savedRevision);
    }

    void failedPoseReloadPreservesUnsavedEdits()
    {
        QTemporaryDir temp;
        QFile model(temp.filePath("model.json"));
        QVERIFY(model.open(QIODevice::WriteOnly));
        QVERIFY(model.write(R"({"FileReferences":{"Moc":"model.moc3"}})") > 0);
        model.close();
        QFile pose(temp.filePath("pose.json"));
        QVERIFY(pose.open(QIODevice::WriteOnly));
        QVERIFY(pose.write(R"({"Groups":[[{"Id":"A"},{"Id":"B"}]],"FadeInTime":0.4,"Extra":"keep"})") > 0);
        pose.close();
        CMotionPanel panel;
        QString error;
        QVERIFY(panel.BuildLists(model.fileName(), {"A", "B", "C"}, &error));
        QVERIFY(panel.BuildPoseGroups(pose.fileName(), &error));
        QPushButton* add = nullptr;
        for (auto* button : panel.findChildren<QPushButton*>())
            if (button->text() == QStringLiteral("添加 Part")) add = button;
        QVERIFY(add);
        QTimer::singleShot(0, [](){
            auto* input = qobject_cast<QInputDialog*>(QApplication::activeModalWidget());
            QVERIFY(input);
            input->setTextValue("C");
            input->accept();
        });
        add->click();
        QVERIFY(panel.HasUnsavedPoseChanges());
        QVERIFY(!panel.BuildPoseGroups(temp.filePath("missing.json"), &error));
        QVERIFY(panel.HasUnsavedPoseChanges());
        QFile broken(temp.filePath("broken.json"));
        QVERIFY(broken.open(QIODevice::WriteOnly));
        QVERIFY(broken.write(R"({"Groups":{}})") > 0);
        broken.close();
        QVERIFY(!panel.BuildPoseGroups(broken.fileName(), &error));
        QVERIFY(panel.HasUnsavedPoseChanges());
        QVERIFY(panel.SavePose(&error));
        QVERIFY(pose.open(QIODevice::ReadOnly));
        const auto root = QJsonDocument::fromJson(pose.readAll()).object();
        QCOMPARE(root.value("Groups").toArray().first().toArray().size(), 3);
        QCOMPARE(root.value("Groups").toArray().first().toArray().last().toObject().value("Id").toString(), QStringLiteral("C"));
        QCOMPARE(root.value("FadeInTime").toDouble(), 0.4);
        QCOMPARE(root.value("Extra").toString(), QStringLiteral("keep"));
    }

    void poseReloadRetainsToolbarAndModelReferences()
    {
        live2dstruct::CModelDocument model;
        const QString root = QString(TEST_RESOURCES) + "/Natori/Natori";
        QVERIFY(model.Load(root + ".moc3"));
        QStringList ids;
        for (const auto& part : model.Parts()) ids.append(part.id);
        CMotionPanel panel;
        QString error;
        QVERIFY(panel.BuildLists(root + ".model3.json", ids, &error));
        for (int i = 0; i < 2; ++i) {
            QVERIFY2(panel.BuildPoseGroups(root + ".pose3.json", &error), qPrintable(error));
            bool found = false;
            for (auto* button : panel.findChildren<QPushButton*>())
                if (button->text() == QStringLiteral("保存 Pose")) found = button->isEnabled();
            QVERIFY(found);
        }
    }
};

QTEST_MAIN(MotionPanelTests)
#include "motion_panel_tests.moc"
