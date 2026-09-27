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
#include <QSpinBox>
#include <QLabel>
#include <QLineEdit>
#include <QComboBox>
#include <QItemSelectionModel>
#include <QJsonDocument>
#include <QMimeData>
#include <QPushButton>
#include <QTableView>
#include <QTimer>
#include <QToolButton>
#include <QToolBar>
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
        emit view->TangentDragStarted({parameter("A"), 0}, CMotionTimeline::HandleSide::Outgoing, false);
        emit view->TangentHandleEdited({parameter("A"), 0}, CMotionTimeline::HandleSide::Outgoing, {.5, 8});
        emit view->TangentDragFinished();
        QCOMPARE(panel.Timeline().BezierControls(parameter("A"), 1).first(), QPointF(.5, 8));
        QCOMPARE(panel.Timeline().BezierControls(parameter("A"), 1).last(), before.BezierControls(parameter("A"), 1).last());
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
        auto* toolbar = panel.findChild<QToolBar*>("motionKeyframeTools");
        auto* tangent = qobject_cast<QToolButton*>(toolbar->widgetForAction(panel.findChild<QAction*>("motionTangentMode")));
        QTRY_VERIFY(tangent->isVisible() && tangent->width() >= tangent->sizeHint().width());
        auto* onion = toolbar->widgetForAction(panel.findChild<QAction*>("motionOnionSkin"));
        QTRY_VERIFY(onion->isVisible() && onion->width() >= onion->sizeHint().width());
        const QImage image = panel.grab().toImage(); QVERIFY(!image.isNull());
        const QString folder = qEnvironmentVariable("LIVE2D_MOTION_UI_OUTPUT");
        if (!folder.isEmpty()) {
            QVERIFY(QDir().mkpath(folder));
            QVERIFY(image.save(QDir(folder).filePath(QString("%1-%2.png").arg(dark ? "dark" : "light").arg(width))));
        }
        panel.hide(); qApp->setStyleSheet({});
    }


    void tangentModesShowMixedAndSkipLinearKeys()
    {
        CMotionPanel panel; QVERIFY(panel.RestoreTimelineState(state()));
        select(panel, {{parameter("A"), 1}}); panel.findChild<QAction*>("motionInterpolation1")->trigger();
        const auto before = panel.Timeline();
        select(panel, {{parameter("A"), 0}, {parameter("B"), 0}});
        auto* action = panel.findChild<QAction*>("motionTangentMode"); QVERIFY(action->isEnabled());
        panel.findChild<QAction*>("motionTangentMode2")->trigger();
        QCOMPARE(panel.Timeline().Tangent(parameter("A"), 0).mode, CMotionTimeline::TangentMode::Flat);
        QCOMPARE(panel.Timeline().Tangent(parameter("B"), 0).mode, CMotionTimeline::TangentMode::Broken);
        QVERIFY(action->text().contains(QStringLiteral("混合")));
        for (int mode = 0; mode < 4; ++mode) QVERIFY(!panel.findChild<QAction*>(QString("motionTangentMode%1").arg(mode))->isChecked());
        auto* model = panel.findChild<CMotionKeyModel*>(); QVERIFY(model);
        const auto cell = model->index(model->RowFor({parameter("A"), 0}), 4);
        QVERIFY(!(model->flags(cell) & Qt::ItemIsEditable)); QCOMPARE(model->data(cell).toString(), QStringLiteral("水平"));
        panel.UndoTimeline(); QVERIFY(panel.Timeline().HasSameContent(before));
        panel.RedoTimeline(); QCOMPARE(panel.Timeline().Tangent(parameter("A"), 0).mode, CMotionTimeline::TangentMode::Flat);
        select(panel, {{parameter("A"), 1}}); panel.findChild<QAction*>("motionInterpolation0")->trigger();
        select(panel, {{parameter("A"), 0}}); QVERIFY(!action->isEnabled());
        QVERIFY(model->data(model->index(model->RowFor({parameter("A"), 0}), 4)).toString().contains(QStringLiteral("待用")));
    }

    void automaticHandleGestureRestoresModeAndOneHistory_data()
    {
        QTest::addColumn<int>("ending");
        QTest::newRow("no-op") << 0; QTest::newRow("return-origin") << 1;
        QTest::newRow("cancel") << 2; QTest::newRow("commit") << 3;
    }
    void automaticHandleGestureRestoresModeAndOneHistory()
    {
        QFETCH(int, ending); CMotionPanel panel; QVERIFY(panel.RestoreTimelineState(state()));
        select(panel, {{parameter("A"), 1}}); panel.findChild<QAction*>("motionInterpolation1")->trigger();
        select(panel, {{parameter("A"), 0}}); panel.findChild<QAction*>("motionTangentMode3")->trigger(); panel.MarkTimelineSaved();
        const auto before = panel.Timeline(); auto* history = panel.findChild<QUndoStack*>(); const int count = history->count();
        auto* view = panel.findChild<CMotionCurveView*>(); const CMotionPointRef key{parameter("A"), 0};
        const auto side = CMotionTimeline::HandleSide::Outgoing; const auto anchor = *before.TangentHandle(key, side);
        emit view->TangentDragStarted(key, side, false);
        if (ending) {
            emit view->TangentHandleEdited(key, side, anchor + QPointF(0, 1));
            QCOMPARE(panel.Timeline().Tangent(key.track, key.index).mode, CMotionTimeline::TangentMode::Aligned);
            emit view->TangentHandleEdited(key, side, anchor + QPointF(0, 2));
            QCOMPARE(*panel.Timeline().TangentHandle(key, side), anchor + QPointF(0, 2));
        }
        if (ending <= 1) emit view->TangentHandleEdited(key, side, anchor);
        if (ending == 2) emit view->TangentDragCanceled(); else emit view->TangentDragFinished();
        QCOMPARE(history->count(), count + int(ending == 3));
        if (ending == 3) { QVERIFY(panel.HasUnsavedTimelineChanges()); panel.UndoTimeline(); }
        QVERIFY(panel.Timeline().HasSameContent(before)); QVERIFY(!panel.HasUnsavedTimelineChanges());
    }


    void restrictedNumbersLockTimeAndExplicitConversionPreservesSamples()
    {
        auto saved = state(); auto motion = saved["motion"].toObject(); auto meta = motion["Meta"].toObject(); meta["AreBeziersRestricted"] = true; motion["Meta"] = meta; saved["motion"] = motion;
        CMotionPanel panel; QVERIFY(panel.RestoreTimelineState(saved)); select(panel, {{parameter("A"), 1}}); panel.findChild<QAction*>("motionInterpolation1")->trigger();
        select(panel, {{parameter("A"), 0}}); const auto before = panel.Timeline();
        QTimer::singleShot(0, &panel, [&] {
            auto* dialog = panel.findChild<QDialog*>("motionTangentValues"); QVERIFY(dialog);
            QVERIFY(dialog->findChild<QLineEdit*>("motionHandleTime")->isReadOnly());
            QVERIFY(!dialog->findChild<QLineEdit*>("motionHandleValue")->isReadOnly()); dialog->reject();
        });
        panel.findChild<QAction*>("motionTangentNumbers")->trigger(); QVERIFY(panel.Timeline().HasSameContent(before));
        auto* convert = panel.findChild<QAction*>("motionConvertUnrestricted"); QVERIFY(convert->isEnabled()); convert->trigger();
        QVERIFY(!panel.Timeline().AreBeziersRestricted());
        for (int i = 0; i <= 20; ++i) QCOMPARE(panel.Timeline().Evaluate("A", i * .1f), before.Evaluate("A", i * .1f));
        panel.UndoTimeline(); QVERIFY(panel.Timeline().HasSameContent(before));
    }


    void flatHandleCannotCollapseOntoItsKeyInDragOrNumericEditor()
    {
        CMotionPanel panel; QVERIFY(panel.RestoreTimelineState(state())); select(panel, {{parameter("A"), 1}});
        panel.findChild<QAction*>("motionInterpolation1")->trigger(); select(panel, {{parameter("A"), 0}});
        panel.findChild<QAction*>("motionTangentMode2")->trigger(); panel.MarkTimelineSaved(); const auto before = panel.Timeline();
        auto* view = panel.findChild<CMotionCurveView*>(); const CMotionPointRef key{parameter("A"), 0}; const auto side = CMotionTimeline::HandleSide::Outgoing;
        const int count = panel.findChild<QUndoStack*>()->count(); emit view->TangentDragStarted(key, side, false);
        emit view->TangentHandleEdited(key, side, {0, 0}); QVERIFY(panel.Timeline().HasSameContent(before));
        QVERIFY(!panel.findChild<QLabel*>("motionTimelineStatus")->text().isEmpty()); emit view->TangentDragFinished();
        QCOMPARE(panel.findChild<QUndoStack*>()->count(), count); QVERIFY(!panel.HasUnsavedTimelineChanges());
        QTRY_VERIFY(panel.findChild<QAction*>("motionTangentNumbers")->isEnabled());
        QTimer::singleShot(0, &panel, [&] {
            auto* dialog = panel.findChild<QDialog*>("motionTangentValues"); QVERIFY(dialog);
            auto* time = dialog->findChild<QLineEdit*>("motionHandleTime"); auto* value = dialog->findChild<QLineEdit*>("motionHandleValue");
            QVERIFY(!time->isReadOnly()); QVERIFY(value->isReadOnly()); auto* buttons = dialog->findChild<QDialogButtonBox*>();
            time->setText("0"); buttons->button(QDialogButtonBox::Ok)->click(); QVERIFY(dialog->isVisible()); QCOMPARE(time->text(), QString("0"));
            QVERIFY(panel.Timeline().HasSameContent(before)); QVERIFY(!dialog->findChild<QLabel*>("motionHandleStatus")->text().isEmpty());
            time->setText(".4"); buttons->button(QDialogButtonBox::Ok)->click();
        });
        panel.findChild<QAction*>("motionTangentNumbers")->trigger();
        QCOMPARE(panel.Timeline().Tangent(key.track, key.index).mode, CMotionTimeline::TangentMode::Flat);
        QCOMPARE(*panel.Timeline().TangentHandle(key, side), QPointF(.4, 0)); QCOMPARE(panel.findChild<QUndoStack*>()->count(), count + 1);
        panel.UndoTimeline(); QVERIFY(panel.Timeline().HasSameContent(before));
    }

    void motionExportKeepsAuthoringBaselineUntilSessionSaved()
    {
        QTemporaryDir folder; CMotionPanel panel; QVERIFY(panel.RestoreTimelineState(state()));
        select(panel, {{parameter("A"), 1}}); panel.findChild<QAction*>("motionInterpolation1")->trigger(); panel.MarkTimelineSaved();
        const auto baseline = panel.Timeline(); select(panel, {{parameter("A"), 0}});
        panel.findChild<QAction*>("motionTangentMode1")->trigger(); QVERIFY(panel.HasUnsavedTimelineChanges());
        QVERIFY(panel.SaveTimeline(folder.filePath("author.motion3.json"))); QVERIFY(panel.HasUnsavedTimelineChanges());
        panel.UndoTimeline(); QVERIFY(panel.Timeline().HasSameContent(baseline)); QVERIFY(!panel.HasUnsavedTimelineChanges());
        panel.RedoTimeline(); panel.MarkTimelineSaved(); QVERIFY(!panel.HasUnsavedTimelineChanges());
        QVERIFY(panel.SaveTimeline(folder.filePath("saved.motion3.json"))); QVERIFY(!panel.HasUnsavedTimelineChanges());
        panel.findChild<QAction*>("motionTangentMode0")->trigger(); QVERIFY(panel.HasUnsavedTimelineChanges());
        QVERIFY(panel.SaveTimeline(folder.filePath("broken.motion3.json"))); QVERIFY(!panel.HasUnsavedTimelineChanges());
        CMotionTimeline reimported; QVERIFY(reimported.Load(folder.filePath("author.motion3.json"))); QVERIFY(!reimported.HasAuthoringTangents());
    }

    void tangentNumberDialogRetainsInvalidDraftAndUsesScientificInput()
    {
        CMotionPanel panel; QVERIFY(panel.RestoreTimelineState(state()));
        select(panel, {{parameter("A"), 1}}); panel.findChild<QAction*>("motionInterpolation1")->trigger();
        select(panel, {{parameter("A"), 0}}); const int count = panel.findChild<QUndoStack*>()->count();
        QTimer::singleShot(0, &panel, [&] {
            auto* dialog = panel.findChild<QDialog*>("motionTangentValues"); QVERIFY(dialog);
            auto* value = dialog->findChild<QLineEdit*>("motionHandleValue"); auto* buttons = dialog->findChild<QDialogButtonBox*>();
            value->setText("nan"); buttons->button(QDialogButtonBox::Ok)->click(); QVERIFY(dialog->isVisible()); QCOMPARE(value->text(), QString("nan"));
            QVERIFY(!dialog->findChild<QLabel*>("motionHandleStatus")->text().isEmpty());
            value->setText("4.125e1"); buttons->button(QDialogButtonBox::Ok)->click();
        });
        panel.findChild<QAction*>("motionTangentNumbers")->trigger();
        QCOMPARE(panel.Timeline().TangentHandle({parameter("A"), 0}, CMotionTimeline::HandleSide::Outgoing)->y(), 41.25);
        QCOMPARE(panel.findChild<QUndoStack*>()->count(), count + 1);
    }

    void onionCaptureIsReadOnlyAndReusesFrozenTimelineAcrossSeek()
    {
        CMotionPanel panel; QVERIFY(panel.RestoreTimelineState(state())); panel.findChild<QAction*>("motionOnionSkin")->setChecked(true);
        QCoreApplication::processEvents(); const auto before = panel.Timeline(); const auto revision = panel.TimelineRevision();
        QSignalSpy changes(&panel, &CMotionPanel::MotionTimelineChanged), requests(&panel, &CMotionPanel::OnionSkinRequestChanged);
        live2dstruct::COnionSkin::Request first, second; QVERIFY(panel.CaptureOnionSkinRequest(first)); QVERIFY(first.timeline.IsValid());
        for (int index = 0; index < 20; ++index) { QVERIFY(panel.CaptureOnionSkinRequest(second)); QCOMPARE(second.timeline.Fingerprint(), first.timeline.Fingerprint()); }
        QCOMPARE(changes.size(), 0); QCOMPARE(requests.size(), 0); QCOMPARE(panel.TimelineRevision(), revision); QVERIFY(panel.Timeline().HasSameContent(before));
        panel.findChild<QSlider*>("motionTimelinePlayhead")->setValue(500);
        QVERIFY(panel.CaptureOnionSkinRequest(second)); QVERIFY(second.time > first.time); QCOMPARE(second.timeline.Fingerprint(), first.timeline.Fingerprint());
        QVERIFY(panel.AddTimelinePoint(parameter("A"), 1, 7)); QVERIFY(panel.CaptureOnionSkinRequest(second));
        QVERIFY(second.timeline.Fingerprint() != first.timeline.Fingerprint());
    }

    void onionSuspendsForDraftGestureAndTransportWithoutFinishingThem()
    {
        CMotionPanel panel; QVERIFY(panel.RestoreTimelineState(state())); panel.findChild<QAction*>("motionOnionSkin")->setChecked(true);
        auto* model = panel.findChild<CMotionKeyModel*>(); auto* view = panel.findChild<CMotionCurveView*>(); live2dstruct::COnionSkin::Request request;
        const auto index = model->index(model->RowFor({parameter("A"), 0}), 0); QVERIFY(!model->setData(index, "invalid"));
        QVERIFY(panel.CaptureOnionSkinRequest(request)); QVERIFY(request.suspended); QCOMPARE(model->data(index, Qt::EditRole).toString(), QString("invalid"));
        QVERIFY(model->setData(index, "0")); select(panel, {{parameter("A"), 0}});
        emit view->SelectionDragStarted({{parameter("A"), 0}}); emit view->SelectionTranslated(.25, 1);
        const auto draft = panel.Timeline(); const auto count = panel.findChild<QUndoStack*>()->count();
        QVERIFY(panel.CaptureOnionSkinRequest(request)); QVERIFY(request.suspended); QVERIFY(panel.Timeline().HasSameContent(draft)); QCOMPARE(panel.findChild<QUndoStack*>()->count(), count);
        emit view->SelectionDragCanceled(); CMotionPanel::PreviewTransportLease lease; QVERIFY(panel.SuspendPreviewTransport(lease));
        QVERIFY(panel.CaptureOnionSkinRequest(request)); QVERIFY(request.suspended); QVERIFY(panel.IsPreviewTransportSuspended());
        QVERIFY(panel.ResumePreviewTransport(lease)); QVERIFY(panel.CaptureOnionSkinRequest(request)); QVERIFY(!request.suspended);
        CMotionPanel empty; empty.findChild<QAction*>("motionOnionSkin")->setChecked(true); QString error;
        QVERIFY(empty.CaptureOnionSkinRequest(request, &error)); QVERIFY(error.isEmpty()); QVERIFY(request.suspended); QVERIFY(!request.timeline.IsValid());
    }


    void onionCaptureDoesNotCommitAnOpenValidCellEditor()
    {
        CMotionPanel panel; QVERIFY(panel.RestoreTimelineState(state())); panel.findChild<QAction*>("motionOnionSkin")->setChecked(true);
        auto* table = panel.findChild<QTableView*>("motionTimelineTable"); auto* model = panel.findChild<CMotionKeyModel*>();
        const auto before = panel.Timeline(); const auto index = model->index(model->RowFor({parameter("A"), 0}), 2);
        table->edit(index); auto* editor = table->findChild<QLineEdit*>(); QVERIFY(editor); editor->setText("1.2345e1");
        live2dstruct::COnionSkin::Request request; QVERIFY(panel.CaptureOnionSkinRequest(request)); QVERIFY(request.suspended);
        QCOMPARE(editor->text(), QString("1.2345e1")); QVERIFY(panel.Timeline().HasSameContent(before)); QCOMPARE(panel.findChild<QUndoStack*>()->count(), 0);
        QTest::keyClick(editor, Qt::Key_Escape);
    }

    void tangentCallbacksMayDestroyThePanel_data()
    {
        QTest::addColumn<int>("stage"); QTest::newRow("mode") << 0; QTest::newRow("press") << 1; QTest::newRow("move") << 2;
    }
    void tangentCallbacksMayDestroyThePanel()
    {
        QFETCH(int, stage); QPointer<CMotionPanel> panel = new CMotionPanel; QVERIFY(panel->RestoreTimelineState(state()));
        select(*panel, {{parameter("A"), 1}}); panel->findChild<QAction*>("motionInterpolation1")->trigger(); select(*panel, {{parameter("A"), 0}});
        QPointer<CMotionCurveView> view = panel->findChild<CMotionCurveView*>(); const CMotionPointRef key{parameter("A"), 0}; const auto side = CMotionTimeline::HandleSide::Outgoing;
        if (stage == 1) connect(panel, &CMotionPanel::TimelineActivated, this, [&] { delete panel; });
        else connect(panel, &CMotionPanel::MotionTimelineChanged, this, [&] { delete panel; });
        if (stage == 0) panel->findChild<QAction*>("motionTangentMode2")->trigger();
        else {
            emit view->TangentDragStarted(key, side, false);
            if (stage == 2) emit view->TangentHandleEdited(key, side, {.5, 5});
        }
        QVERIFY(!panel); QVERIFY(!view); QCoreApplication::processEvents();
    }

    void onionSettingsStayInLayoutAndRejectMalformedValues()
    {
        CMotionPanel panel; QVERIFY(panel.RestoreTimelineState(state())); const auto before = panel.Timeline();
        panel.findChild<QAction*>("motionOnionSkin")->setChecked(true); panel.findChild<QSpinBox*>("motionOnionBefore")->setValue(4);
        panel.findChild<QSpinBox*>("motionOnionAfter")->setValue(0); panel.findChild<QSpinBox*>("motionOnionInterval")->setValue(13);
        panel.findChild<QDoubleSpinBox*>("motionOnionBeforeOpacity")->setValue(37.4);
        panel.findChild<QCheckBox*>("motionOnionDuringPlayback")->setChecked(true);
        live2dstruct::COnionSkin::Request expected, actual; QVERIFY(panel.CaptureOnionSkinRequest(expected));
        CMotionPanel restored; restored.RestoreLayout(panel.SaveLayout()); QVERIFY(restored.CaptureOnionSkinRequest(actual)); QCOMPARE(actual.settings, expected.settings);
        QVERIFY(panel.Timeline().HasSameContent(before)); QVERIFY(!panel.HasUnsavedTimelineChanges()); QCOMPARE(panel.findChild<QUndoStack*>()->count(), 0);
        auto malformed = QJsonDocument::fromJson(panel.SaveLayout()).object(); auto options = malformed["onion"].toObject(); options["before"] = 1.5; malformed["onion"] = options;
        restored.RestoreLayout(QJsonDocument(malformed).toJson()); QVERIFY(restored.CaptureOnionSkinRequest(actual)); QCOMPARE(actual.settings, live2dstruct::COnionSkin::Settings{});
    }

    void onionStatusReportsActualTimesAndIgnoresOlderGeneration()
    {
        CMotionPanel panel; panel.findChild<QAction*>("motionOnionSkin")->setChecked(true);
        live2dstruct::COnionSkin::Status status; status.state = decltype(status.state)::Ready; status.generation = 7;
        status.sampleTimes = {.1, .2}; status.omitted = 3; status.reducedResolution = true; status.renderedSize = {640, 360};
        panel.SetOnionSkinStatus(status); auto* label = panel.findChild<QLabel*>("motionOnionStatus");
        QVERIFY(label->text().contains("0.1s")); QVERIFY(label->text().contains("0.2s")); QVERIFY(label->text().contains("640")); QVERIFY(label->text().contains(QStringLiteral("省略 3")));
        const auto previous = label->text(); status.generation = 6; status.state = decltype(status.state)::Failed; status.message = "stale failure";
        panel.SetOnionSkinStatus(status); QCOMPARE(label->text(), previous);
        QPointer<CMotionPanel> pending = new CMotionPanel; pending->findChild<QAction*>("motionOnionSkin")->setChecked(true); delete pending; QCoreApplication::processEvents(); QVERIFY(!pending);
    }


    void onionControlsWrapAndRemainReachable_data()
    {
        QTest::addColumn<bool>("dark"); QTest::addColumn<int>("width");
        for (bool dark : {false, true}) for (int width : {520, 900})
            QTest::newRow(qPrintable(QString("%1-%2").arg(dark ? "dark" : "light").arg(width))) << dark << width;
    }
    void onionControlsWrapAndRemainReachable()
    {
        QFETCH(bool, dark); QFETCH(int, width); qApp->setStyleSheet(Theme::globalStyleSheet(dark));
        CMotionPanel panel; QVERIFY(panel.RestoreTimelineState(state())); select(panel, {{parameter("A"), 1}});
        panel.findChild<QAction*>("motionInterpolation1")->trigger(); select(panel, {{parameter("A"), 0}});
        panel.findChild<QAction*>("motionTangentMode3")->trigger(); panel.findChild<QAction*>("motionOnionSkin")->setChecked(true);
        panel.findChild<QAction*>("motionOnionSettings")->setChecked(true);
        live2dstruct::COnionSkin::Status status; status.state = decltype(status.state)::Suspended; status.message = QStringLiteral("请在主画布选择时间轴帧，随后显示实际采样结果"); panel.SetOnionSkinStatus(status);
        panel.resize(width, 740); panel.show(); QTest::qWait(30);
        auto* scroll = panel.findChild<QScrollArea*>("motionWorkspaceScroll"); auto* controls = panel.findChild<QWidget*>("motionOnionSettingsPanel");
        QVERIFY(controls->isVisible()); QCOMPARE(scroll->horizontalScrollBar()->maximum(), 0); QCOMPARE(panel.width(), width);
        for (const char* name : {"motionOnionBefore", "motionOnionAfter", "motionOnionInterval", "motionOnionBeforeOpacity", "motionOnionAfterOpacity", "motionOnionDuringPlayback"}) {
            auto* input = panel.findChild<QWidget*>(name); QVERIFY(input); scroll->ensureWidgetVisible(input); QCoreApplication::processEvents();
            const auto rect = QRect(input->mapTo(scroll->viewport(), QPoint()), input->size());
            QVERIFY2(scroll->viewport()->rect().contains(rect), name); QVERIFY(input->focusPolicy() != Qt::NoFocus);
        }
        scroll->ensureWidgetVisible(controls, 0, 0); QTest::qWait(30);
        const QString folder = qEnvironmentVariable("LIVE2D_MOTION_UI_OUTPUT");
        if (!folder.isEmpty()) { QVERIFY(QDir().mkpath(folder)); QVERIFY(panel.grab().save(QDir(folder).filePath(QString("onion-%1-%2.png").arg(dark ? "dark" : "light").arg(width)))); }
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
