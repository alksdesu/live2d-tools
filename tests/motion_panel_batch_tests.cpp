#include "ui/cmotionpanel.h"
#include "ui/cmotioncurveview.h"
#include "ui/cmotionkeymodel.h"
#include "ui/theme.h"
#include "live2dstruct/cmotionkeyclipboard.h"
#include <QtTest>
#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QItemSelectionModel>
#include <QJsonDocument>
#include <QMimeData>
#include <QPushButton>
#include <QTableView>
#include <QTimer>
#include <QToolButton>
#include <QUndoStack>
#include <QScrollBar>
#include <QScrollArea>
#include <QStyleFactory>
#include <QDir>
#include <cmath>
#include <QTemporaryDir>
#include <memory>

namespace {
CMotionTrack parameter(const QString& id) { return {CMotionTarget::Parameter, id}; }
QJsonObject state()
{
    return {{"version", 1}, {"playhead", 0}, {"motion", QJsonObject{
        {"Version", 3}, {"Meta", QJsonObject{{"Fps", 59.940000123}, {"Duration", 8}, {"AreBeziersRestricted", false}}},
        {"Curves", QJsonArray{QJsonObject{{"Target", "Parameter"}, {"Id", "A"}, {"Segments", QJsonArray{0, 0, 0, 2, 2}}},
                             QJsonObject{{"Target", "Parameter"}, {"Id", "B"}, {"Segments", QJsonArray{1, 10, 0, 3, 30}}}}},
        {"UserData", QJsonArray{QJsonObject{{"Time", 7}, {"Value", "event"}}}}, {"Extra", true}}}};
}
void select(CMotionPanel& panel, const CMotionPointSelection& refs)
{
    auto* view = panel.findChild<CMotionCurveView*>();
    view->SetSelectedPoints(refs);
    emit view->SelectionChanged(refs);
}
void clipboard(const CMotionTimeline& source, const CMotionPointSelection& refs)
{
    QByteArray bytes;
    QVERIFY(CMotionKeyClipboard::Capture(source, refs, bytes));
    auto* mime = new QMimeData;
    mime->setData(CMotionKeyClipboard::MimeType, bytes);
    QApplication::clipboard()->setMimeData(mime);
}
}

class MotionPanelBatchTests : public QObject
{
    Q_OBJECT
    QMap<QString, QByteArray> m_clipboard;
private slots:
    void initTestCase()
    {
        const auto* mime = QApplication::clipboard()->mimeData();
        if (mime) for (const auto& format : mime->formats()) m_clipboard.insert(format, mime->data(format));
    }
    void cleanupTestCase()
    {
        auto* mime = new QMimeData;
        for (auto it = m_clipboard.cbegin(); it != m_clipboard.cend(); ++it) mime->setData(it.key(), it.value());
        QApplication::clipboard()->setMimeData(mime);
    }
    void batchDeleteKeepsIdentityAfterTableReorder()
    {
        CMotionPanel panel; QVERIFY(panel.RestoreTimelineState(state()));
        auto* table = panel.findChild<QTableView*>("motionTimelineTable");
        auto* model = qobject_cast<CMotionKeyModel*>(table->model()); QVERIFY(model);
        select(panel, {{parameter("A"), 1}, {parameter("B"), 1}});
        const int row = model->RowFor({parameter("A"), 1});
        QVERIFY(model->setData(model->index(row, 0), "0.5"));
        QCOMPARE(panel.findChild<CMotionCurveView*>()->SelectedPoints().size(), 2);
        panel.findChild<QPushButton*>("removeTimelineKeyframe")->click();
        QCOMPARE(panel.Timeline().Tracks().value(parameter("A")), QVector<QPointF>({{0, 0}}));
        QCOMPARE(panel.Timeline().Tracks().value(parameter("B")), QVector<QPointF>({{1, 10}}));
        panel.UndoTimeline(); QCOMPARE(panel.Timeline().TotalPointCount(), 4);
        QCOMPARE(panel.findChild<CMotionCurveView*>()->SelectedPoints().size(), 2);
        panel.UndoTimeline(); QCOMPARE(panel.Timeline().Tracks().value(parameter("A"))[1], QPointF(2, 2));
    }

    void dragUsesOriginalSnapshotAndOneUndo()
    {
        CMotionPanel panel; QVERIFY(panel.RestoreTimelineState(state()));
        const auto original = panel.Timeline();
        auto* view = panel.findChild<CMotionCurveView*>();
        auto* history = panel.findChild<QUndoStack*>();
        const CMotionPointSelection refs{{parameter("A"), 1}, {parameter("B"), 0}};
        select(panel, refs);
        emit view->SelectionDragStarted(refs);
        emit view->SelectionTranslated(2, 3);
        emit view->SelectionTranslated(3, 4);
        QCOMPARE(panel.Timeline().Tracks().value(parameter("A"))[1], QPointF(5, 6));
        QCOMPARE(panel.Timeline().Tracks().value(parameter("B"))[1], QPointF(4, 14));
        emit view->SelectionDragFinished();
        QCOMPARE(history->count(), 1);
        panel.UndoTimeline(); QVERIFY(panel.Timeline().HasSameContent(original));
        QCOMPARE(view->SelectedPoints(), refs);
        panel.RedoTimeline(); QCOMPARE(panel.Timeline().Tracks().value(parameter("B"))[1], QPointF(4, 14));
        QCOMPARE(view->SelectedPoints(), CMotionPointSelection({{parameter("A"), 1}, {parameter("B"), 1}}));
    }

    void dragReturningToOriginCreatesNoEdit()
    {
        CMotionPanel panel; QVERIFY(panel.RestoreTimelineState(state()));
        auto* view = panel.findChild<CMotionCurveView*>(); const auto original = panel.Timeline();
        const CMotionPointSelection refs{{parameter("A"), 1}}; select(panel, refs);
        emit view->SelectionDragStarted(refs); emit view->SelectionTranslated(1, 1);
        QVERIFY(panel.HasUnsavedTimelineChanges());
        emit view->SelectionTranslated(0, 0); emit view->SelectionDragFinished();
        QVERIFY(panel.Timeline().HasSameContent(original)); QVERIFY(!panel.HasUnsavedTimelineChanges());
        QCOMPARE(panel.findChild<QUndoStack*>()->count(), 0);
    }

    void cancelRestoresDataSelectionAndDirtyState()
    {
        CMotionPanel panel; QVERIFY(panel.RestoreTimelineState(state()));
        auto* view = panel.findChild<CMotionCurveView*>(); const auto original = panel.Timeline();
        const CMotionPointSelection refs{{parameter("B"), 0}}; select(panel, refs);
        emit view->SelectionDragStarted(refs); emit view->SelectionTranslated(4, 1);
        emit view->SelectionDragCanceled();
        QVERIFY(panel.Timeline().HasSameContent(original)); QCOMPARE(view->SelectedPoints(), refs);
        QVERIFY(!panel.HasUnsavedTimelineChanges()); QCOMPARE(panel.findChild<QUndoStack*>()->count(), 0);
    }

    void collisionRetainsLastAcceptedPreviewAndCanRecover()
    {
        CMotionPanel panel; QVERIFY(panel.RestoreTimelineState(state()));
        auto* view = panel.findChild<CMotionCurveView*>();
        const CMotionPointSelection refs{{parameter("B"), 0}}; select(panel, refs);
        emit view->SelectionDragStarted(refs); emit view->SelectionTranslated(.5, 1);
        const auto accepted = panel.Timeline();
        emit view->SelectionTranslated(2, 1); QVERIFY(panel.Timeline().HasSameContent(accepted));
        emit view->SelectionTranslated(3, 2); emit view->SelectionDragFinished();
        QCOMPARE(panel.Timeline().Tracks().value(parameter("B"))[1], QPointF(4, 12));
        QCOMPARE(panel.findChild<QUndoStack*>()->count(), 1);
    }

    void copyPasteAndCutAreSingleUndoCommands()
    {
        CMotionPanel panel; QVERIFY(panel.RestoreTimelineState(state()));
        select(panel, {{parameter("A"), 0}, {parameter("A"), 1}});
        panel.findChild<QAction*>("copyMotionKeys")->trigger();
        QVERIFY(QApplication::clipboard()->mimeData()->hasFormat(CMotionKeyClipboard::MimeType));
        panel.findChild<QSlider*>("motionTimelinePlayhead")->setValue(500);
        panel.findChild<QToolButton*>("pasteMotionKeys")->click();
        QCOMPARE(panel.Timeline().Tracks().value(parameter("A")), QVector<QPointF>({{0, 0}, {2, 2}, {4, 0}, {6, 2}}));
        QCOMPARE(panel.findChild<QUndoStack*>()->count(), 1);
        panel.findChild<QAction*>("cutMotionKeys")->trigger();
        QCOMPARE(panel.Timeline().TotalPointCount(), 4);
        QCOMPARE(panel.findChild<QUndoStack*>()->count(), 2);
        panel.UndoTimeline(); QCOMPARE(panel.Timeline().TotalPointCount(), 6);
        panel.UndoTimeline(); QCOMPARE(panel.Timeline().TotalPointCount(), 4);
    }

    void cutDoesNotDeleteChangedSelectionFromClipboardCallback()
    {
        CMotionPanel panel; QVERIFY(panel.RestoreTimelineState(state()));
        select(panel, {{parameter("A"), 1}});
        const auto before = panel.Timeline();
        const auto connection = connect(QApplication::clipboard(), &QClipboard::dataChanged, &panel, [&] {
            select(panel, {{parameter("B"), 0}});
        });
        panel.findChild<QAction*>("cutMotionKeys")->trigger();
        disconnect(connection);
        QVERIFY(panel.Timeline().HasSameContent(before));
        QCOMPARE(panel.findChild<QUndoStack*>()->count(), 0);
    }

    void clipboardCallbackCanDestroyPanel()
    {
        auto panel = std::make_unique<CMotionPanel>(); QVERIFY(panel->RestoreTimelineState(state()));
        select(*panel, {{parameter("A"), 1}});
        connect(QApplication::clipboard(), &QClipboard::dataChanged, panel.get(), [&] { panel.reset(); });
        panel->findChild<QAction*>("cutMotionKeys")->trigger();
        QVERIFY(!panel);
    }

    void editedMotionAndSessionRoundTripPreserveMetadata()
    {
        CMotionPanel panel; QVERIFY(panel.RestoreTimelineState(state()));
        select(panel, {{parameter("A"), 1}, {parameter("B"), 0}});
        auto* view = panel.findChild<CMotionCurveView*>();
        emit view->SelectionDragStarted(view->SelectedPoints());
        emit view->SelectionTranslated(3, -.5); emit view->SelectionDragFinished();
        panel.findChild<QAction*>("motionInterpolation1")->trigger();
        const auto expected = panel.Timeline();
        QTemporaryDir folder; QVERIFY(folder.isValid());
        const auto path = folder.filePath("edited.motion3.json");
        QString error; QVERIFY2(panel.SaveTimeline(path, &error), qPrintable(error));
        CMotionTimeline loaded; QVERIFY(loaded.Load(path, &error));
        for (const auto& track : expected.Tracks().keys()) {
            QCOMPARE(loaded.Tracks().value(track), expected.Tracks().value(track));
            for (int sample = 0; sample <= 80; ++sample)
                QCOMPARE(loaded.Evaluate(track, sample / 10.0f), expected.Evaluate(track, sample / 10.0f));
        }
        QJsonObject saved; QVERIFY(panel.CaptureTimelineState(&saved, &error));
        CMotionPanel restored; QVERIFY(restored.RestoreTimelineState(saved, &error));
        QJsonObject recaptured; QVERIFY(restored.CaptureTimelineState(&recaptured, &error));
        QCOMPARE(recaptured, saved);
        QCOMPARE(restored.Timeline().Tracks(), expected.Tracks());
        QCOMPARE(loaded.Timing().fps, expected.Timing().fps);
        QCOMPARE(restored.Timeline().Events(), expected.Events());
        QCOMPARE(restored.Timeline().Timing().fps, expected.Timing().fps);
        QVERIFY(!panel.HasUnsavedTimelineChanges()); QVERIFY(!restored.HasUnsavedTimelineChanges());
    }

    void pasteRejectsUnknownNewTrack()
    {
        CMotionPanel panel; QVERIFY(panel.RestoreTimelineState(state()));
        CMotionTimeline source; source.AddPoint("Missing", 0, 3); clipboard(source, {{parameter("Missing"), 0}});
        const auto before = panel.Timeline();
        panel.findChild<QToolButton*>("pasteMotionKeys")->click();
        QVERIFY(panel.Timeline().HasSameContent(before)); QCOMPARE(panel.findChild<QUndoStack*>()->count(), 0);
        panel.SetParameterIds({"Missing"}); panel.findChild<QToolButton*>("pasteMotionKeys")->click();
        QVERIFY(panel.Timeline().Tracks().contains(parameter("Missing")));
    }

    void pasteIntoSelectedTrackRequiresExplicitCoincidenceReplacement()
    {
        CMotionPanel panel; QVERIFY(panel.RestoreTimelineState(state()));
        select(panel, {{parameter("A"), 0}});
        CMotionTimeline source; source.AddPoint("Source", 0, 9); clipboard(source, {{parameter("Source"), 0}});
        panel.findChild<QAction*>("pasteMotionKeysIntoTrack")->trigger();
        QCOMPARE(panel.Timeline().Tracks().value(parameter("A"))[0].y(), 0.0);
        panel.findChild<QAction*>("replaceCoincidentMotionKeys")->setChecked(true);
        panel.findChild<QAction*>("pasteMotionKeysIntoTrack")->trigger();
        QCOMPARE(panel.Timeline().Tracks().value(parameter("A"))[0].y(), 9.0);
        QCOMPARE(panel.Timeline().TotalPointCount(), 4);
        panel.UndoTimeline(); QCOMPARE(panel.Timeline().Tracks().value(parameter("A"))[0].y(), 0.0);
    }

    void transformPreservesEventsAndUsesEarliestSelectedPivot()
    {
        CMotionPanel panel; QVERIFY(panel.RestoreTimelineState(state()));
        const auto before = panel.Timeline();
        select(panel, {{parameter("A"), 1}, {parameter("B"), 0}});
        QTimer::singleShot(0, [] {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget()); QVERIFY(dialog);
            dialog->findChild<QDoubleSpinBox*>("motionTimeOffset")->setValue(2);
            dialog->findChild<QDoubleSpinBox*>("motionTimeScale")->setValue(3);
            dialog->findChild<QDoubleSpinBox*>("motionValueOffset")->setValue(-1);
            dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
            if (dialog->isVisible()) dialog->reject();
        });
        panel.findChild<QAction*>("transformMotionKeys")->trigger();
        // B reaches the existing key at t=3, so the entire operation must be rejected.
        QVERIFY(panel.Timeline().HasSameContent(before));
        QTimer::singleShot(0, [] {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget()); QVERIFY(dialog);
            dialog->findChild<QDoubleSpinBox*>("motionTimeOffset")->setValue(3);
            dialog->findChild<QDoubleSpinBox*>("motionTimeScale")->setValue(2);
            dialog->findChild<QDoubleSpinBox*>("motionValueOffset")->setValue(-1);
            dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
        });
        panel.findChild<QAction*>("transformMotionKeys")->trigger();
        QCOMPARE(panel.Timeline().Tracks().value(parameter("A"))[1], QPointF(6, 1));
        QCOMPARE(panel.Timeline().Tracks().value(parameter("B"))[1], QPointF(4, 9));
        QCOMPARE(panel.Timeline().Events(), before.Events()); QCOMPARE(panel.Timeline().Timing().fps, before.Timing().fps);
        QCOMPARE(panel.findChild<QUndoStack*>()->count(), 1);
        panel.UndoTimeline(); QVERIFY(panel.Timeline().HasSameContent(before));
    }

    void interpolationAndBezierHandlesUndoTogether()
    {
        CMotionPanel panel; QVERIFY(panel.RestoreTimelineState(state()));
        select(panel, {{parameter("A"), 0}, {parameter("A"), 1}});
        panel.findChild<QAction*>("motionInterpolation1")->trigger();
        QCOMPARE(panel.Timeline().SegmentType(parameter("A"), 1), 1);
        const auto before = panel.Timeline();
        auto* view = panel.findChild<CMotionCurveView*>();
        emit view->BezierDragStarted(parameter("A"), 1);
        emit view->BezierControlsEdited(parameter("A"), 1, {{.5, 8}, {1.5, -8}});
        emit view->BezierDragFinished();
        QCOMPARE(panel.Timeline().BezierControls(parameter("A"), 1), QVector<QPointF>({{.5, 8}, {1.5, -8}}));
        panel.UndoTimeline(); QVERIFY(panel.Timeline().HasSameContent(before));
        panel.UndoTimeline(); QCOMPARE(panel.Timeline().SegmentType(parameter("A"), 1), 0);
    }

    void workspaceRendersAndScrolls_data()
    {
        QTest::addColumn<bool>("dark"); QTest::addColumn<int>("width");
        for (bool dark : {false, true}) for (int width : {520, 900})
            QTest::newRow(qPrintable(QString("%1-%2").arg(dark ? "dark" : "light").arg(width))) << dark << width;
    }
    void workspaceRendersAndScrolls()
    {
        QFETCH(bool, dark); QFETCH(int, width);
        qApp->setStyleSheet(Theme::globalStyleSheet(dark));
        CMotionTimeline source;
        for (int index = 0; index < 80; ++index) QVERIFY(source.AddPoint("A", index / 10.0f, std::sin(index / 6.0f)));
        QJsonObject saved; QVERIFY(source.SaveState(&saved, false, 0));
        CMotionPanel panel; QVERIFY(panel.RestoreTimelineState(saved));
        panel.resize(width, 720); panel.show(); QTest::qWait(20);
        auto* scroll = panel.findChild<QScrollArea*>("motionWorkspaceScroll");
        QCOMPARE(scroll->horizontalScrollBar()->maximum(), 0);
        auto* table = panel.findChild<QTableView*>("motionTimelineTable");
        auto* model = qobject_cast<CMotionKeyModel*>(table->model()); QVERIFY(model);
        table->scrollTo(model->index(79, 2), QAbstractItemView::PositionAtBottom);
        QCoreApplication::processEvents();
        QVERIFY(table->viewport()->rect().intersects(table->visualRect(model->index(79, 2))));
        QVERIFY(table->verticalScrollBar()->maximum() > 0);
        select(panel, {{parameter("A"), 10}, {parameter("A"), 11}, {parameter("A"), 12}});
        panel.findChild<QAction*>("motionInterpolation1")->trigger();
        QVERIFY(panel.findChild<QToolButton*>("pasteMotionKeys")->isEnabled());
        const QImage image = panel.grab().toImage(); QVERIFY(!image.isNull());
        const QString folder = qEnvironmentVariable("LIVE2D_MOTION_UI_OUTPUT");
        if (!folder.isEmpty()) {
            QVERIFY(QDir().mkpath(folder));
            QVERIFY(image.save(QDir(folder).filePath(QString("%1-%2.png").arg(dark ? "dark" : "light").arg(width))));
        }
        panel.hide(); qApp->setStyleSheet({});
    }

    void frameSnappingPersistsAsLayoutOnly()
    {
        CMotionPanel panel; QVERIFY(panel.RestoreTimelineState(state()));
        auto* check = panel.findChild<QAction*>("motionFrameSnapping"); QVERIFY(check->isChecked());
        check->setChecked(false); QVERIFY(!panel.HasUnsavedTimelineChanges());
        CMotionPanel other; other.RestoreLayout(panel.SaveLayout());
        QVERIFY(!other.findChild<CMotionCurveView*>()->FrameSnapping());
        QVERIFY(!other.HasUnsavedTimelineChanges());
    }
};

QTEST_MAIN(MotionPanelBatchTests)
#include "motion_panel_batch_tests.moc"
