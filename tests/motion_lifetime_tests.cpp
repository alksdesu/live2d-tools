#include "ui/cmotionpanel.h"
#include "ui/cmotioncurveview.h"
#include "ui/cmotionkeymodel.h"
#include <QtTest>
#include <QPointer>
#include <QJsonArray>
#include <QTableView>
#include <QLineEdit>
#include <QTemporaryDir>
#include <QAction>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QPushButton>
#include <QTimer>

namespace {
QJsonObject state()
{
    const QJsonObject curve{{"Target", "Parameter"}, {"Id", "A"}, {"Segments", QJsonArray{0, 0, 0, 1, 1}}};
    const QJsonObject motion{{"Version", 3}, {"Curves", QJsonArray{curve}}};
    return {{"version", 1}, {"playhead", 0}, {"motion", motion}};
}
}
class MotionLifetimeTests : public QObject
{
    Q_OBJECT
private slots:
    void gestureNotificationCanClosePanel_data()
    {
        QTest::addColumn<QString>("signal");
        for (const auto& name : {"frame", "dirty", "history", "reset", "aboutToReset"})
            QTest::newRow(name) << QString::fromLatin1(name);
    }
    void gestureNotificationCanClosePanel()
    {
        QFETCH(QString, signal);
        auto* panel = new CMotionPanel; QVERIFY(panel->RestoreTimelineState(state()));
        QPointer<CMotionPanel> alive(panel); auto* view = panel->findChild<CMotionCurveView*>();
        const CMotionPointSelection selection{{{CMotionTarget::Parameter, "A"}, 1}};
        view->SetSelectedPoints(selection); emit view->SelectionChanged(selection); emit view->SelectionDragStarted(selection);
        const auto close = [alive] { if (alive) delete alive.data(); };
        if (signal == "frame") connect(panel, &CMotionPanel::TimelineFrameChanged, qApp, close);
        if (signal == "dirty") connect(panel, &CMotionPanel::TimelineDirtyChanged, qApp, close);
        if (signal == "history") connect(panel, &CMotionPanel::TimelineHistoryChanged, qApp, close);
        auto* model = panel->findChild<CMotionKeyModel*>();
        if (signal == "reset") connect(model, &QAbstractItemModel::modelReset, qApp, close);
        if (signal == "aboutToReset") connect(model, &QAbstractItemModel::modelAboutToBeReset, qApp, close);
        emit view->SelectionTranslated(.25, .5);
        QVERIFY(alive.isNull());
    }

    void tablePublicationCanClosePanel_data()
    {
        QTest::addColumn<bool>("duringReset");
        QTest::newRow("activated") << false; QTest::newRow("model-reset") << true;
    }
    void tablePublicationCanClosePanel()
    {
        QFETCH(bool, duringReset);
        auto* panel = new CMotionPanel; QVERIFY(panel->RestoreTimelineState(state()));
        QPointer<CMotionPanel> alive(panel);
        auto* model = panel->findChild<CMotionKeyModel*>();
        const auto close = [alive] { if (alive) delete alive.data(); };
        if (duringReset) connect(model, &QAbstractItemModel::modelReset, qApp, close);
        else connect(panel, &CMotionPanel::TimelineActivated, qApp, close);
        model->setData(model->index(1, 2), "0.75");
        QVERIFY(alive.isNull());
    }

    void finishingGestureCanClosePanelFromHistoryNotification()
    {
        auto* panel = new CMotionPanel; QVERIFY(panel->RestoreTimelineState(state()));
        QPointer<CMotionPanel> alive(panel); auto* view = panel->findChild<CMotionCurveView*>();
        const CMotionPointSelection selection{{{CMotionTarget::Parameter, "A"}, 1}};
        view->SetSelectedPoints(selection); emit view->SelectionChanged(selection); emit view->SelectionDragStarted(selection);
        emit view->SelectionTranslated(.25, .5);
        connect(panel, &CMotionPanel::TimelineHistoryChanged, qApp, [alive] { if (alive) delete alive.data(); });
        emit view->SelectionDragFinished();
        QVERIFY(alive.isNull());
    }

    void saveEntryCanClosePanelWhileCommittingEditor_data()
    {
        QTest::addColumn<QString>("entry");
        for (const auto& name : {"export", "session", "saved"}) QTest::newRow(name) << QString::fromLatin1(name);
    }
    void saveEntryCanClosePanelWhileCommittingEditor()
    {
        QFETCH(QString, entry); QTemporaryDir folder; QVERIFY(folder.isValid());
        auto* panel = new CMotionPanel; QVERIFY(panel->RestoreTimelineState(state()));
        QPointer<CMotionPanel> alive(panel);
        panel->resize(900, 650); panel->show();
        auto* table = panel->findChild<QTableView*>("motionTimelineTable");
        table->edit(table->model()->index(1, 2));
        auto* editor = table->findChild<QLineEdit*>(); QVERIFY(editor); editor->setText("0.75");
        connect(panel, &CMotionPanel::TimelineFrameChanged, qApp, [alive] { if (alive) delete alive.data(); });
        if (entry == "export") QVERIFY(!panel->SaveTimeline(folder.filePath("motion.motion3.json")));
        if (entry == "session") { QJsonObject saved; QVERIFY(!panel->CaptureTimelineState(&saved)); }
        if (entry == "saved") panel->MarkTimelineSaved();
        QVERIFY(alive.isNull());
    }

    void transformCanClosePanelWhileApplying()
    {
        auto* panel = new CMotionPanel; QVERIFY(panel->RestoreTimelineState(state()));
        QPointer<CMotionPanel> alive(panel);
        auto* view = panel->findChild<CMotionCurveView*>();
        const CMotionPointSelection selection{{{CMotionTarget::Parameter, "A"}, 1}};
        view->SetSelectedPoints(selection); emit view->SelectionChanged(selection);
        connect(panel, &CMotionPanel::TimelineActivated, qApp, [alive] { if (alive) delete alive.data(); });
        QTimer::singleShot(0, [] {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget()); QVERIFY(dialog);
            dialog->findChild<QDoubleSpinBox*>("motionValueOffset")->setValue(.5);
            dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
        });
        panel->findChild<QAction*>("transformMotionKeys")->trigger();
        QVERIFY(alive.isNull());
    }

    void undoPreviewCanClosePanel()
    {
        auto* panel = new CMotionPanel; QVERIFY(panel->RestoreTimelineState(state()));
        QPointer<CMotionPanel> alive(panel);
        QVERIFY(panel->AddTimelinePoint({CMotionTarget::Parameter, "A"}, 2, .5));
        connect(panel, &CMotionPanel::TimelineFrameChanged, qApp, [alive] { if (alive) delete alive.data(); });
        panel->UndoTimeline();
        QVERIFY(alive.isNull());
    }

    void gesturePreviewCanSynchronouslyDestroyPanel()
    {
        auto* panel = new CMotionPanel; QVERIFY(panel->RestoreTimelineState(state()));
        QPointer<CMotionPanel> alive(panel); auto* view = panel->findChild<CMotionCurveView*>(); QVERIFY(view);
        const CMotionPointSelection selection{{{CMotionTarget::Parameter, "A"}, 1}};
        view->SetSelectedPoints(selection); emit view->SelectionChanged(selection); emit view->SelectionDragStarted(selection);
        connect(panel, &CMotionPanel::TimelineFrameChanged, qApp, [panel] { delete panel; });
        emit view->SelectionTranslated(.25, .5);
        QVERIFY(alive.isNull());
    }
};
QTEST_MAIN(MotionLifetimeTests)
#include "motion_lifetime_tests.moc"
