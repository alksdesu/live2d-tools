#include "ui/cmotionpanel.h"
#include "ui/cmotioncurveview.h"
#include "ui/cmotionkeymodel.h"
#include "ui/theme.h"
#include <QtTest>
#include <QAction>
#include <QComboBox>
#include <QDir>
#include <QLineEdit>
#include <QMenu>
#include <QScrollArea>
#include <QScrollBar>
#include <QSplitter>
#include <QTableView>
#include <QToolBar>
#include <QToolButton>
#include <QUndoStack>

namespace {
QJsonObject source()
{
    return {{"version", 1}, {"playhead", 0}, {"motion", QJsonObject{
        {"Version", 3}, {"Meta", QJsonObject{{"Fps", 30}, {"Duration", 8}}},
        {"Curves", QJsonArray{
            QJsonObject{{"Target", "Parameter"}, {"Id", "A"}, {"Segments", QJsonArray{0, 0, 0, 2, 2}}},
            QJsonObject{{"Target", "Parameter"}, {"Id", "B"}, {"Segments", QJsonArray{1, 10, 0, 3, 30}}}}}}}};
}
CMotionPointRef point(const char* id, int index) { return {{CMotionTarget::Parameter, id}, index}; }
QLineEdit* editCell(CMotionPanel& panel, const CMotionPointRef& ref, int column)
{
    auto* table = panel.findChild<QTableView*>("motionTimelineTable");
    auto* model = qobject_cast<CMotionKeyModel*>(table->model());
    const auto cell = model->index(model->RowFor(ref), column);
    panel.findChild<QScrollArea*>("motionWorkspaceScroll")->ensureWidgetVisible(table);
    table->setCurrentIndex(cell); table->selectionModel()->select(cell, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    table->setFocus(Qt::TabFocusReason); table->scrollTo(cell);
    QApplication::processEvents();
    #ifdef Q_OS_MACOS
    QTest::keyClick(table, Qt::Key_Return);
#else
    QTest::keyClick(table, Qt::Key_F2);
#endif
    return table->findChild<QLineEdit*>();
}
}

class MotionWorkspaceTests : public QObject
{
    Q_OBJECT
private slots:
    void responsiveWorkspacePreservesTimelineAndSelection_data()
    {
        QTest::addColumn<int>("width"); QTest::addColumn<bool>("dark");
        for (const int width : {320, 460, 900}) for (const bool dark : {false, true})
            QTest::newRow(qPrintable(QString("%1-%2").arg(width).arg(dark))) << width << dark;
    }
    void responsiveWorkspacePreservesTimelineAndSelection()
    {
        QFETCH(int, width); QFETCH(bool, dark); qApp->setStyleSheet(Theme::globalStyleSheet(dark));
        CMotionPanel panel; QVERIFY(panel.RestoreTimelineState(source())); panel.resize(1200, 600); panel.show();
        auto* view = panel.findChild<CMotionCurveView*>(); const CMotionPointSelection selection{point("A", 1), point("B", 0)};
        view->SetSelectedPoints(selection); emit view->SelectionChanged(selection);
        const auto timeline = panel.Timeline();
        auto* split = panel.findChild<QSplitter*>("animationWorkspaceSplitter");
        auto* scroll = panel.findChild<QScrollArea*>("motionWorkspaceScroll");
        QTRY_COMPARE(split->orientation(), Qt::Horizontal);
        panel.resize(width, 420); QTest::qWait(30);
        QCOMPARE(panel.width(), width);
        if (width <= 460) QCOMPARE(split->orientation(), Qt::Vertical);
        const auto size = scroll->widget()->size();
        QTest::qWait(30); QCOMPARE(scroll->widget()->size(), size);
        QCOMPARE(view->SelectedPoints(), selection); QVERIFY(panel.Timeline().HasSameContent(timeline)); QVERIFY(!panel.HasUnsavedTimelineChanges());
        auto* table = panel.findChild<QTableView*>("motionTimelineTable");
        scroll->ensureWidgetVisible(table); QApplication::processEvents();
        const QRect viewport(scroll->viewport()->mapToGlobal(QPoint()), scroll->viewport()->size());
        QVERIFY(viewport.intersects(QRect(table->mapToGlobal(QPoint()), table->size())));
        const auto path = qEnvironmentVariable("LIVE2D_MOTION_WORKSPACE_QA_DIR");
        if (!path.isEmpty()) { QDir().mkpath(path); QVERIFY(panel.grab().save(path + QString("/workspace-%1-%2.png").arg(dark ? "dark" : "light").arg(width))); }
        panel.resize(1200, 600); QTRY_COMPARE(split->orientation(), Qt::Horizontal);
        QCOMPARE(view->SelectedPoints(), selection); QVERIFY(panel.Timeline().HasSameContent(timeline));
    }

    void overflowMenuCanToggleFrameSnapping_data()
    {
        QTest::addColumn<bool>("dark"); QTest::newRow("light") << false; QTest::newRow("dark") << true;
    }
    void overflowMenuCanToggleFrameSnapping()
    {
        QFETCH(bool, dark); qApp->setStyleSheet(Theme::globalStyleSheet(dark));
        CMotionPanel panel; QVERIFY(panel.RestoreTimelineState(source())); panel.resize(320, 420); panel.show();
        auto* tools = panel.findChild<QToolBar*>("motionKeyframeTools");
        auto* snap = panel.findChild<QAction*>("motionFrameSnapping"); QVERIFY(snap);
        auto* extension = tools->findChild<QToolButton*>("qt_toolbar_ext_button"); QVERIFY(extension);
        panel.findChild<QScrollArea*>("motionWorkspaceScroll")->ensureWidgetVisible(tools); QTest::qWait(30);
        QVERIFY(extension->isVisible()); QVERIFY(!tools->widgetForAction(snap)->isVisible());
        QVERIFY(extension->focusPolicy() & Qt::TabFocus);
        for (auto* button : tools->findChildren<QToolButton*>()) QVERIFY(button->focusPolicy() & Qt::TabFocus);
        QStringList hidden;
        for (auto* action : tools->actions()) if (auto* widget = tools->widgetForAction(action); widget && !widget->isVisible()) hidden.append(action->objectName());
        QStringList reachable; bool clicked = false;
        QTimer::singleShot(0, tools, [&] {
            auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
            if (!menu) menu = extension->menu();
            if (!menu) return;
            for (auto* action : menu->actions()) reachable.append(action->objectName());
            if (menu->actions().contains(snap)) { QTest::mouseClick(menu, Qt::LeftButton, Qt::NoModifier, menu->actionGeometry(snap).center()); clicked = true; }
            else menu->close();
        });
        QTest::mouseClick(extension, Qt::LeftButton);
        QVERIFY(clicked); for (const auto& name : hidden) QVERIFY2(reachable.contains(name), qPrintable(name));
        QVERIFY(!snap->isChecked()); QVERIFY(!panel.findChild<CMotionCurveView*>()->FrameSnapping());
        clicked = false;
        panel.activateWindow(); QTRY_VERIFY(panel.isActiveWindow());
        extension->setFocus(Qt::TabFocusReason); QTRY_VERIFY(extension->hasFocus());
        QTimer::singleShot(0, tools, [&] {
            auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
            if (!menu) menu = extension->menu();
            if (!menu) return;
            if (menu->actions().contains(snap)) {
                menu->setActiveAction(snap); QTest::keyClick(menu, Qt::Key_Return); clicked = true;
            } else menu->close();
        });
        QTest::keyClick(extension, Qt::Key_Space);
        QVERIFY(clicked); QVERIFY(snap->isChecked()); QVERIFY(panel.findChild<CMotionCurveView*>()->FrameSnapping());
        QVERIFY(!panel.HasUnsavedTimelineChanges()); QCOMPARE(panel.findChild<QUndoStack*>()->count(), 0);
        const auto saved = panel.SaveLayout(); snap->trigger(); QVERIFY(!snap->isChecked()); panel.RestoreLayout(saved); QVERIFY(snap->isChecked());
    }

    void keyboardCellEditKeepsTheSamePointAndCurrentColumnAfterSorting()
    {
        qApp->setStyleSheet(Theme::globalStyleSheet(false));
        CMotionPanel panel; QVERIFY(panel.RestoreTimelineState(source())); panel.resize(900, 600); panel.show(); QTest::qWait(20);
        auto* table = panel.findChild<QTableView*>("motionTimelineTable"); auto* model = qobject_cast<CMotionKeyModel*>(table->model());
        auto* editor = editCell(panel, point("A", 1), 0); QVERIFY(editor);
        editor->selectAll(); QTest::keyClicks(editor, "0.5"); QTest::keyClick(editor, Qt::Key_Return); QCoreApplication::processEvents();
        QCOMPARE(table->currentIndex().column(), 0); QCOMPARE(table->currentIndex().data(Qt::UserRole).value<CMotionPointRef>(), point("A", 1));
        QCOMPARE(panel.Timeline().Tracks().value(point("A", 1).track)[1], QPointF(.5, 2));
        editor = editCell(panel, point("A", 1), 2); QVERIFY(editor);
        editor->selectAll(); QTest::keyClicks(editor, "7.5"); QTest::keyClick(editor, Qt::Key_Return); QCoreApplication::processEvents();
        QCOMPARE(table->currentIndex().column(), 2); QCOMPARE(table->currentIndex().data(Qt::UserRole).value<CMotionPointRef>(), point("A", 1));
        QCOMPARE(model->RowFor(point("A", 1)), table->currentIndex().row());
        QCOMPARE(panel.Timeline().Tracks().value(point("A", 1).track)[1], QPointF(.5, 7.5));
        panel.UndoTimeline(); QCOMPARE(panel.Timeline().Tracks().value(point("A", 1).track)[1], QPointF(.5, 2));
        panel.UndoTimeline(); QCOMPARE(panel.Timeline().Tracks().value(point("A", 1).track)[1], QPointF(2, 2));
    }

    void rejectedDelegateDraftSurvivesFilterAndCurveUntilExplicitlyCanceled()
    {
        CMotionPanel panel; QVERIFY(panel.RestoreTimelineState(source())); panel.resize(900, 600); panel.show(); QTest::qWait(20);
        const auto original = panel.Timeline(); auto* editor = editCell(panel, point("A", 1), 2); QVERIFY(editor);
        editor->selectAll(); QTest::keyClicks(editor, "not-a-number"); QTest::keyClick(editor, Qt::Key_Return); QCoreApplication::processEvents();
        auto* table = panel.findChild<QTableView*>("motionTimelineTable"); auto* model = qobject_cast<CMotionKeyModel*>(table->model());
        QVERIFY(model->HasRejectedDraft());
        const auto assertDraft = [&] {
            QVERIFY(panel.Timeline().HasSameContent(original)); QVERIFY(model->HasRejectedDraft());
            QCOMPARE(model->index(model->RowFor(point("A", 1)), 2).data(Qt::EditRole).toString(), QString("not-a-number"));
            QCOMPARE(panel.findChild<QUndoStack*>()->count(), 0);
        };
        auto* filter = panel.findChild<QComboBox*>("motionTargetFilter"); filter->setCurrentIndex(filter->findData(int(CMotionTarget::Model)));
        QCOMPARE(filter->currentData().toInt(), -1); assertDraft();
        auto* view = panel.findChild<CMotionCurveView*>();
        panel.findChild<QScrollArea*>("motionWorkspaceScroll")->ensureWidgetVisible(view); QCoreApplication::processEvents();
        const QRect plot = view->rect().adjusted(34, 16, -14, -22);
        const QPoint press(qRound(plot.left() + plot.width() * 2.0 / 8), qRound(plot.bottom() - plot.height() * 3.5 / 33));
        QTest::mousePress(view, Qt::LeftButton, Qt::NoModifier, press);
        QMouseEvent move(QEvent::MouseMove, press + QPoint(20, -10), press + QPoint(20, -10), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(view, &move); QTest::mouseRelease(view, Qt::LeftButton, Qt::NoModifier, press + QPoint(20, -10)); assertDraft();
        table->setFocus(Qt::TabFocusReason); QTest::keyClick(table, Qt::Key_Escape);
        QVERIFY(!model->HasRejectedDraft()); QVERIFY(panel.Timeline().HasSameContent(original)); QVERIFY(!panel.HasUnsavedTimelineChanges());
    }
};
QTEST_MAIN(MotionWorkspaceTests)
#include "motion_workspace_tests.moc"
