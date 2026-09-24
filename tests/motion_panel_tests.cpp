#include "ui/cmotionpanel.h"
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
