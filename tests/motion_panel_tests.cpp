#include "ui/cmotionpanel.h"
#include "ui/cmotioncurveview.h"
#include "ui/theme.h"
#include "live2dstruct/cmodelsession.h"
#include <QtTest>
#include <QApplication>
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

private slots:
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
