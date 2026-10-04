#include "ui/cmotioncurveview.h"
#include <QtTest>
#include <QImage>
#include <QSignalSpy>
#include <QMouseEvent>
#include <QHelpEvent>
#include <QToolTip>
#include <QTemporaryFile>
#include <QJsonDocument>
#include <QPointer>
#include <QLineEdit>
#include <QVBoxLayout>
#include <functional>
#include <limits>
#include <cmath>

namespace {
class FocusAction : public QObject
{
public:
    std::function<void()> action;
    bool eventFilter(QObject*, QEvent* event) override
    {
        if (event->type() == QEvent::FocusIn && action) { const auto run = std::move(action); run(); }
        return false;
    }
};

void movePoint(CMotionCurveView& view, QPointF point, Qt::KeyboardModifiers modifiers = {})
{
    QMouseEvent move(QEvent::MouseMove, point, point, point, Qt::NoButton, Qt::LeftButton, modifiers);
    QCoreApplication::sendEvent(&view, &move);
}
bool bezier(CMotionTimeline& timeline, bool restricted = false)
{
    QTemporaryFile file;
    if (!file.open()) return false;
    const QJsonObject source{{"Version", 3}, {"Meta", QJsonObject{{"Fps", 30}, {"Duration", 1}, {"AreBeziersRestricted", restricted}}},
        {"Curves", QJsonArray{QJsonObject{{"Target", "Parameter"}, {"Id", "A"},
            {"Segments", QJsonArray{0, 0, 1, .9, 8, .1, 8, 1, 0}}}}}};
    file.write(QJsonDocument(source).toJson()); file.flush();
    return timeline.Load(file.fileName());
}
QPointF screenPoint(QPointF point, double minimum = -1.1, double maximum = 1.1)
{
    return {34 + 272 * point.x(), 168 - 152 * (point.y() - minimum) / (maximum - minimum)};
}
struct GestureEditor
{
    CMotionTimeline& timeline;
    CMotionCurveView& view;
    CMotionTimeline before;
    CMotionPointSelection anchors;
    int commits = 0, cancels = 0, commands = 0, rejects = 0;
    bool active = false;
    GestureEditor(CMotionTimeline& value, CMotionCurveView& widget) : timeline(value), view(widget)
    {
        QObject::connect(&view, &CMotionCurveView::SelectionDragStarted, &view, [this](const auto& selected) {
            before = timeline; anchors = selected; active = true;
        });
        QObject::connect(&view, &CMotionCurveView::SelectionTranslated, &view, [this](double dt, double dv) {
            if (!active) return;
            auto candidate = before;
            CMotionTimeline::BatchEdit edit;
            for (const auto& ref : anchors) edit.updates.append({ref, before.Tracks().value(ref.track).at(ref.index) + QPointF(dt, dv)});
            CMotionTimeline::BatchResult result;
            if (!candidate.EditPoints(edit, result)) { ++rejects; return; }
            timeline = std::move(candidate); view.SetTimeline(&timeline);
            auto selected = anchors;
            for (auto& ref : selected) if (result.oldToNew.contains(ref.track)) ref.index = result.oldToNew[ref.track].at(ref.index);
            view.SetSelectedPoints(selected);
        });
        QObject::connect(&view, &CMotionCurveView::SelectionDragFinished, &view, [this] {
            if (!active) return;
            ++commits; commands += !timeline.HasSameContent(before); active = false;
        });
        QObject::connect(&view, &CMotionCurveView::SelectionDragCanceled, &view, [this] {
            if (!active) return;
            ++cancels; timeline = before; view.SetTimeline(&timeline); view.SetSelectedPoints(anchors); active = false;
        });
    }
};
}

class CMotionCurveViewTests : public QObject
{
    Q_OBJECT
private slots:
    void multipleOpenCurvesDoNotFillTheAreaBetweenTheirEndpoints()
    {
        CMotionTimeline timeline;
        QVERIFY(timeline.AddPoint("A", 0, -1)); QVERIFY(timeline.AddPoint("A", 1, 1));
        QVERIFY(timeline.AddPoint("B", 0, -1)); QVERIFY(timeline.AddPoint("B", .5, 1)); QVERIFY(timeline.AddPoint("B", 1, -1));
        CMotionCurveView view; view.resize(320, 190); view.SetTimeline(&timeline);
        QImage image(view.size(), QImage::Format_ARGB32); view.render(&image);
        QCOMPARE(image.pixelColor(screenPoint({.4, .1}).toPoint()), view.palette().color(QPalette::Base));
    }

    void focusChangeRefreshesTheTimelineBeforeCapturingPressCoordinates_data()
    {
        QTest::addColumn<bool>("events");
        QTest::newRow("point-projection") << false;
        QTest::newRow("event-marker") << true;
    }
    void focusChangeRefreshesTheTimelineBeforeCapturingPressCoordinates()
    {
        QFETCH(bool, events);
        CMotionTimeline timeline;
        QVERIFY(timeline.AddPoint("A", .25f, -1)); QVERIFY(timeline.AddPoint("A", .75f, 1));
        QVERIFY(timeline.SetTiming(30, 1));
        if (events) QVERIFY(timeline.SetEvents({QJsonObject{{"Time", 1}, {"Value", "old"}}}));
        QWidget host;
        auto* layout = new QVBoxLayout(&host); auto* input = new QLineEdit(&host); auto* view = new CMotionCurveView(&host);
        layout->addWidget(input); layout->addWidget(view); host.resize(360, 270); host.show();
        view->SetTimeline(&timeline); input->setFocus(); QTRY_VERIFY(input->hasFocus());
        FocusAction focus;
        focus.action = [&] {
            if (events) timeline.Clear();
            else {
                QVERIFY(timeline.UpdatePoint("A", 0, .25f, -10));
                QVERIFY(timeline.UpdatePoint("A", 1, .75f, 10));
            }
            view->SetTimeline(&timeline);
        };
        view->installEventFilter(&focus);
        QSignalSpy changed(view, &CMotionCurveView::SelectionTranslated), selected(view, &CMotionCurveView::EventSelected);
        const double plotWidth = view->width() - 48, plotHeight = view->height() - 38;
        const QPoint press = events ? QPoint(view->width() - 21, 27) :
            QPoint(qRound(34 + plotWidth * .25), qRound(view->height() - 22 - plotHeight / 22));
        QTest::mousePress(view, Qt::LeftButton, Qt::NoModifier, press);
        if (events) QCOMPARE(selected.size(), 0);
        else {
            movePoint(*view, press + QPoint(0, -10)); QCOMPARE(changed.size(), 1);
            QVERIFY(std::abs(changed.first()[1].toDouble() - 10.0 / plotHeight * 22) < 1e-10);
        }
        QTest::mouseRelease(view, Qt::LeftButton, Qt::NoModifier, press);
    }

    void acceptedReorderingKeepsTheOriginalGesturePointAndReturnsItsSelection()
    {
        CMotionTimeline timeline; const CMotionTrack track{CMotionTarget::Parameter, "A"};
        QVERIFY(timeline.AddPoint(track, .2f, -1)); QVERIFY(timeline.AddPoint(track, .4f, 0)); QVERIFY(timeline.AddPoint(track, .8f, 1));
        QVERIFY(timeline.SetTiming(30, 1)); const auto original = timeline;
        CMotionCurveView view; view.resize(320, 190); view.SetTimeline(&timeline); view.show(); GestureEditor editor(timeline, view);
        const auto press = screenPoint({.2, -1}).toPoint();
        QTest::mousePress(&view, Qt::LeftButton, Qt::NoModifier, press);
        movePoint(view, press + QPoint(100, 0)); QCOMPARE(view.SelectedPoints(), (CMotionPointSelection{{track, 1}}));
        movePoint(view, press + QPoint(110, 0));
        QCOMPARE(timeline.Tracks().value(track)[0], original.Tracks().value(track)[1]);
        QVERIFY(std::abs(timeline.Tracks().value(track)[1].x() - (double(.2f) + 110.0 / 272)) < 1e-7);
        movePoint(view, press); QVERIFY(timeline.HasSameContent(original));
        QCOMPARE(view.SelectedPoints(), (CMotionPointSelection{{track, 0}}));
        QTest::mouseRelease(&view, Qt::LeftButton, Qt::NoModifier, press); QCOMPARE(editor.commands, 0);
    }

    void marqueeSelectionSpansTypedTracksAndSupportsAddToggleSubtract()
    {
        CMotionTimeline timeline;
        CMotionPointSelection all;
        int kind = 0;
        for (auto target : {CMotionTarget::Model, CMotionTarget::Parameter, CMotionTarget::PartOpacity}) {
            const CMotionTrack track{target, "SameId"};
            QVERIFY(timeline.AddPoint(track, .25f, kind - 1)); QVERIFY(timeline.AddPoint(track, .75f, kind));
            all.append({track, 0}); all.append({track, 1}); ++kind;
        }
        QVERIFY(timeline.SetTiming(30, 1));
        CMotionCurveView view; view.resize(320, 190); view.SetTimeline(&timeline); view.show();
        QSignalSpy started(&view, &CMotionCurveView::SelectionDragStarted);
        const auto box = [&](QPoint first, QPoint last, Qt::KeyboardModifiers modifiers = {}) {
            QTest::mousePress(&view, Qt::LeftButton, modifiers, first); movePoint(view, last, modifiers);
            QTest::mouseRelease(&view, Qt::LeftButton, modifiers, last);
        };
        box({70, 20}, {250, 165}); QCOMPARE(view.SelectedPoints(), all); QCOMPARE(started.size(), 0);
        box({70, 20}, {140, 165}, Qt::AltModifier);
        QCOMPARE(view.SelectedPoints(), (CMotionPointSelection{all[1], all[3], all[5]}));
        box({70, 20}, {140, 165}, Qt::ControlModifier); QCOMPARE(view.SelectedPoints(), all);
        box({70, 20}, {140, 165}, Qt::ControlModifier);
        box({70, 20}, {140, 165}, Qt::ShiftModifier); QCOMPARE(view.SelectedPoints(), all);
        view.SetTargetFilter(int(CMotionTarget::Parameter)); QCOMPARE(view.TargetFilter(), int(CMotionTarget::Parameter));
        QCOMPARE(view.SelectedPoints(), (CMotionPointSelection{all[2], all[3]}));
        QSignalSpy selection(&view, &CMotionCurveView::SelectionChanged);
        view.SetSelectedPoints({all[2], all[2], {{CMotionTarget::Parameter, "missing"}, 0}, {all[2].track, 999}});
        QCOMPARE(view.SelectedPoints(), (CMotionPointSelection{all[2]})); QCOMPARE(selection.size(), 0);
    }

    void groupDragUsesOneFrozenAbsoluteDeltaAndReturningToOriginMakesNoCommand()
    {
        CMotionTimeline timeline;
        const CMotionTrack a{CMotionTarget::Parameter, "A"}, b{CMotionTarget::PartOpacity, "A"};
        for (const auto& track : {a, b}) { QVERIFY(timeline.AddPoint(track, .25f, -1)); QVERIFY(timeline.AddPoint(track, .75f, 1)); }
        QVERIFY(timeline.SetTiming(30, 1)); const auto original = timeline;
        CMotionCurveView view; view.resize(320, 190); view.SetTimeline(&timeline); view.show();
        view.SetSelectedPoints({{a, 0}, {b, 0}}); GestureEditor editor(timeline, view);
        const QPoint press = screenPoint({.25, -1}).toPoint();
        QTest::mousePress(&view, Qt::LeftButton, Qt::NoModifier, press);
        movePoint(view, press + QPoint(38, -31)); const auto first = timeline;
        QVERIFY(!first.HasSameContent(original));
        for (int i = 0; i < 4; ++i) movePoint(view, press + QPoint(38, -31));
        QVERIFY(timeline.HasSameContent(first));
        QCOMPARE(timeline.Tracks().value(a)[0], timeline.Tracks().value(b)[0]);
        movePoint(view, press); QVERIFY(timeline.HasSameContent(original));
        QTest::mouseRelease(&view, Qt::LeftButton, Qt::NoModifier, press);
        QCOMPARE(editor.commits, 1); QCOMPARE(editor.commands, 0);
        QTest::mousePress(&view, Qt::LeftButton, Qt::NoModifier, press);
        movePoint(view, press + QPoint(15, -20)); QVERIFY(!timeline.HasSameContent(original));
        QTest::keyClick(&view, Qt::Key_Escape);
        QVERIFY(timeline.HasSameContent(original)); QCOMPARE(editor.cancels, 1); QCOMPARE(editor.commands, 0);
        QTest::mouseRelease(&view, Qt::LeftButton, Qt::NoModifier, press);
        QCOMPARE(editor.commits, 1);
    }

    void rejectedSamplesKeepTheLastAcceptedFrameAndOriginalAnchor()
    {
        CMotionTimeline timeline; const CMotionTrack track{CMotionTarget::Parameter, "A"};
        QVERIFY(timeline.AddPoint(track, .25f, -1)); QVERIFY(timeline.AddPoint(track, .75f, 1)); QVERIFY(timeline.SetTiming(30, 1));
        const auto original = timeline;
        CMotionCurveView view; view.resize(320, 190); view.SetTimeline(&timeline); view.show();
        GestureEditor editor(timeline, view);
        const QPoint press = screenPoint({.25, -1}).toPoint();
        QTest::mousePress(&view, Qt::LeftButton, Qt::NoModifier, press);
        movePoint(view, press + QPoint(30, -10)); const auto accepted = timeline;
        movePoint(view, press + QPoint(-200, -10)); QVERIFY(editor.rejects > 0); QVERIFY(timeline.HasSameContent(accepted));
        movePoint(view, press + QPoint(20, -10));
        QVERIFY(std::abs(timeline.Tracks().value(track)[0].x() - (.25 + 20.0 / 272)) < 1e-7);
        QTest::mouseRelease(&view, Qt::LeftButton, Qt::NoModifier, press + QPoint(20, -10));
        QCOMPARE(editor.commands, 1); QVERIFY(!timeline.HasSameContent(original));
    }

    void closeNeighborTimesAndGrabOffsetDoNotMoveAnUnmovedPoint()
    {
        CMotionTimeline timeline; const CMotionTrack track{CMotionTarget::Parameter, "A"};
        for (const auto& point : {QPointF(.25, 0), QPointF(.2502, 1), QPointF(.2504, 2)})
            QVERIFY(timeline.AddPoint(track, float(point.x()), float(point.y())));
        QVERIFY(timeline.SetTiming(30, 1)); const auto original = timeline;
        CMotionCurveView view; view.resize(320, 190); view.SetTimeline(&timeline); view.show();
        connect(&view, &CMotionCurveView::PointEdited, &view, [&](const auto& key, int index, float time, float value) {
            QVERIFY(timeline.UpdatePoint(key, index, time, value)); view.SetTimeline(&timeline);
        });
        const QPoint press = screenPoint({.2502, 1}, -.1, 2.1).toPoint() + QPoint(7, 5);
        QTest::mousePress(&view, Qt::LeftButton, Qt::NoModifier, press);
        movePoint(view, press); movePoint(view, press);
        QTest::mouseRelease(&view, Qt::LeftButton, Qt::NoModifier, press);
        QVERIFY(timeline.HasSameContent(original));
    }

    void snappingAndAxisConstraintsUseThePressedKeyAsOneGroupPivot_data()
    {
        QTest::addColumn<double>("fps"); QTest::newRow("30") << 30.0; QTest::newRow("29.97") << 29.97;
    }
    void snappingAndAxisConstraintsUseThePressedKeyAsOneGroupPivot()
    {
        QFETCH(double, fps);
        CMotionTimeline timeline; QVERIFY(timeline.AddPoint("A", .25f, -1)); QVERIFY(timeline.AddPoint("A", .75f, 1)); QVERIFY(timeline.SetTiming(fps, 1));
        CMotionCurveView view; view.resize(320, 190); view.SetTimeline(&timeline); view.SetFrameSnapping(true); QVERIFY(view.FrameSnapping()); view.show();
        QSignalSpy translated(&view, &CMotionCurveView::SelectionTranslated);
        const QPoint press = screenPoint({.25, -1}).toPoint(); QTest::mousePress(&view, Qt::LeftButton, Qt::NoModifier, press);
        movePoint(view, press + QPoint(50, -10));
        const double snapped = translated.last()[0].toDouble();
        QVERIFY(std::abs((.25 + snapped) * fps - std::round((.25 + snapped) * fps)) < 1e-10);
        movePoint(view, press + QPoint(50, -10), Qt::AltModifier); QVERIFY(std::abs(translated.last()[0].toDouble() - 50.0 / 272) < 1e-12);
        movePoint(view, press + QPoint(50, -10), Qt::ShiftModifier); QCOMPARE(translated.last()[1].toDouble(), 0.0);
        movePoint(view, press + QPoint(5, -30), Qt::ShiftModifier); QCOMPARE(translated.last()[0].toDouble(), 0.0);
        view.CancelDrag();
    }

    void bezierHandlesExposeOvershootAndKeepRestrictedIgnoredTimes_data()
    {
        QTest::addColumn<bool>("restricted"); QTest::newRow("free-crossing-handles") << false; QTest::newRow("restricted") << true;
    }
    void bezierHandlesExposeOvershootAndKeepRestrictedIgnoredTimes()
    {
        QFETCH(bool, restricted); CMotionTimeline timeline; QVERIFY(bezier(timeline, restricted));
        const CMotionTrack track{CMotionTarget::Parameter, "A"};
        CMotionCurveView view; view.resize(320, 190); view.SetTimeline(&timeline); view.SetSelectedPoints({{track, 0}}); view.show();
        QSignalSpy started(&view, &CMotionCurveView::TangentDragStarted);
        QSignalSpy edited(&view, &CMotionCurveView::TangentHandleEdited);
        QSignalSpy completed(&view, &CMotionCurveView::TangentDragFinished);
        const auto original = timeline; const auto controls = timeline.BezierControls(track, 1);
        connect(&view, &CMotionCurveView::TangentHandleEdited, &view, [&](const auto& key, auto side, auto position) {
            auto candidate = original; CMotionTimeline::BatchEdit request; request.handles.append({key, side, position});
            CMotionTimeline::BatchResult result; QVERIFY(candidate.EditPoints(request, result)); timeline = candidate; view.SetTimeline(&timeline);
        });
        const QPoint press = screenPoint(controls[0], -.4, 8.4).toPoint();
        QTest::mousePress(&view, Qt::LeftButton, Qt::NoModifier, press); QCOMPARE(started.size(), 1);
        movePoint(view, press + QPoint(90, 20)); QCOMPARE(edited.size(), 1);
        const auto emitted = edited.last()[2].toPointF();
        QVERIFY(emitted.y() < 8);
        if (restricted) QCOMPARE(emitted.x(), controls[0].x());
        else { QCOMPARE(emitted.x(), 1.0); QVERIFY(emitted.x() > controls[1].x()); }
        QTest::mouseRelease(&view, Qt::LeftButton, Qt::NoModifier, press + QPoint(90, 20)); QCOMPARE(completed.size(), 1);
        QByteArray bytes; QVERIFY(timeline.ToJson(bytes, 30, false));
        if (restricted) {
            const auto values = QJsonDocument::fromJson(bytes).object()["Curves"].toArray()[0].toObject()["Segments"].toArray();
            QCOMPARE(values[3].toDouble(), .9); QCOMPARE(values[5].toDouble(), .1);
        }
        QImage image(view.size(), QImage::Format_ARGB32); view.render(&image); QVERIFY(!image.isNull());
    }


    void tangentModifierIsFrozenAtPressAndOwnedBySelectedKey_data()
    {
        QTest::addColumn<bool>("linked"); QTest::newRow("plain") << false; QTest::newRow("control") << true;
    }
    void tangentModifierIsFrozenAtPressAndOwnedBySelectedKey()
    {
        QFETCH(bool, linked); CMotionTimeline timeline; QVERIFY(bezier(timeline));
        const CMotionTrack track{CMotionTarget::Parameter, "A"}; const CMotionPointRef selected{track, 0};
        CMotionCurveView view; view.resize(320, 190); view.SetTimeline(&timeline); view.SetSelectedPoints({selected}); view.show();
        QSignalSpy start(&view, &CMotionCurveView::TangentDragStarted), edit(&view, &CMotionCurveView::TangentHandleEdited), cancel(&view, &CMotionCurveView::TangentDragCanceled);
        const QPoint press = screenPoint({.9, 8}, -.4, 8.4).toPoint();
        QTest::mousePress(&view, Qt::LeftButton, linked ? Qt::ControlModifier : Qt::NoModifier, press);
        QCOMPARE(start.size(), 1); QCOMPARE(start.first()[0].value<CMotionPointRef>(), selected);
        QCOMPARE(start.first()[1].value<CMotionTimeline::HandleSide>(), CMotionTimeline::HandleSide::Outgoing);
        QCOMPARE(start.first()[2].toBool(), linked);
        movePoint(view, press + QPoint(0, 20), linked ? Qt::NoModifier : Qt::ControlModifier);
        QCOMPARE(edit.size(), 1); QCOMPARE(edit.first()[0].value<CMotionPointRef>(), selected);
        QTest::keyClick(&view, Qt::Key_Escape); QCOMPARE(cancel.size(), 1); QCOMPARE(start.size(), 1);
        QTest::mouseRelease(&view, Qt::LeftButton, Qt::NoModifier, press); QCOMPARE(cancel.size(), 1);
        QTest::mousePress(&view, Qt::LeftButton, Qt::NoModifier, screenPoint({.1, 8}, -.4, 8.4).toPoint());
        QCOMPARE(start.size(), 1); view.CancelDrag();
    }

    void flatRestrictedHandleCannotStartGesture()
    {
        CMotionTimeline timeline; QVERIFY(bezier(timeline, true));
        const CMotionPointRef key{{CMotionTarget::Parameter, "A"}, 0}; CMotionTimeline::BatchEdit edit; edit.tangents.append({key, CMotionTimeline::TangentMode::Flat});
        CMotionTimeline::BatchResult result; QVERIFY(timeline.EditPoints(edit, result));
        CMotionCurveView view; view.resize(320, 190); view.SetTimeline(&timeline); view.SetSelectedPoints({key}); view.show();
        QSignalSpy start(&view, &CMotionCurveView::TangentDragStarted);
        const auto handle = *timeline.TangentHandle(key, CMotionTimeline::HandleSide::Outgoing);
        const auto position = screenPoint(handle, -.4, 8.4).toPoint(); movePoint(view, position);
        QCOMPARE(view.cursor().shape(), Qt::ArrowCursor);
        QHelpEvent help(QEvent::ToolTip, position, view.mapToGlobal(position)); QCoreApplication::sendEvent(&view, &help);
        QVERIFY(QToolTip::text().contains(QStringLiteral("固定")));
        QTest::mousePress(&view, Qt::LeftButton, Qt::NoModifier, position);
        QCOMPARE(start.size(), 0); view.CancelDrag(); QToolTip::hideText();
    }


    void automaticHandleExplainsManualTransitionBeforeDragging()
    {
        CMotionTimeline timeline; QVERIFY(bezier(timeline));
        const CMotionPointRef key{{CMotionTarget::Parameter, "A"}, 0}; CMotionTimeline::BatchEdit edit; edit.tangents.append({key, CMotionTimeline::TangentMode::AutoClamped});
        CMotionTimeline::BatchResult result; QVERIFY(timeline.EditPoints(edit, result));
        CMotionCurveView view; view.resize(320, 190); view.SetTimeline(&timeline); view.SetSelectedPoints({key}); view.show();
        const auto handle = *timeline.TangentHandle(key, CMotionTimeline::HandleSide::Outgoing);
        const auto position = screenPoint(handle, -.4, 8.4).toPoint(); movePoint(view, position); QCOMPARE(view.cursor().shape(), Qt::PointingHandCursor);
        QHelpEvent help(QEvent::ToolTip, position, view.mapToGlobal(position)); QCoreApplication::sendEvent(&view, &help);
        QVERIFY(QToolTip::text().contains(QStringLiteral("转为对齐"))); QVERIFY(QToolTip::text().contains(QStringLiteral("Esc")));
        QCOMPARE(timeline.Tangent(key.track, key.index).mode, CMotionTimeline::TangentMode::AutoClamped); QToolTip::hideText();
    }

    void hidingOrVetoingADragCancelsOnceAndStopsLaterSamples()
    {
        CMotionTimeline timeline; QVERIFY(timeline.AddPoint("A", .25f, -1)); QVERIFY(timeline.AddPoint("A", .75f, 1)); QVERIFY(timeline.SetTiming(30, 1));
        CMotionCurveView view; view.resize(320, 190); view.SetTimeline(&timeline); view.show();
        QSignalSpy canceled(&view, &CMotionCurveView::SelectionDragCanceled), moved(&view, &CMotionCurveView::SelectionTranslated);
        const QPoint press = screenPoint({.25, -1}).toPoint();
        QTest::mousePress(&view, Qt::LeftButton, Qt::NoModifier, press); view.hide(); QCOMPARE(canceled.size(), 1);
        movePoint(view, press + QPoint(10, 10)); QCOMPARE(moved.size(), 0);
        view.show(); connect(&view, &CMotionCurveView::SelectionDragStarted, &view, &CMotionCurveView::CancelDrag);
        QTest::mousePress(&view, Qt::LeftButton, Qt::NoModifier, press); QCOMPARE(canceled.size(), 2);
        movePoint(view, press + QPoint(10, 10)); QTest::mouseRelease(&view, Qt::LeftButton, Qt::NoModifier, press);
        QCOMPARE(moved.size(), 0); QCOMPARE(canceled.size(), 2);
    }

    void synchronousSignalMayDestroyTheViewWithoutUsingStaleState_data()
    {
        QTest::addColumn<int>("stage"); for (int stage = 0; stage < 3; ++stage) QTest::newRow(qPrintable(QString::number(stage))) << stage;
    }
    void synchronousSignalMayDestroyTheViewWithoutUsingStaleState()
    {
        QFETCH(int, stage); CMotionTimeline timeline;
        QVERIFY(timeline.AddPoint("A", .25f, -1)); QVERIFY(timeline.AddPoint("A", .75f, 1)); QVERIFY(timeline.SetTiming(30, 1));
        QPointer<CMotionCurveView> view = new CMotionCurveView; view->resize(320, 190); view->SetTimeline(&timeline);
        if (stage == 0) connect(view, &CMotionCurveView::SelectionChanged, [&] { delete view.data(); });
        if (stage == 1) connect(view, &CMotionCurveView::SelectionDragStarted, [&] { delete view.data(); });
        if (stage == 2) connect(view, &CMotionCurveView::SelectionTranslated, [&] { delete view.data(); });
        const auto press = screenPoint({.25, -1});
        QMouseEvent down(QEvent::MouseButtonPress, press, press, press, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(view, &down);
        if (stage == 2) { QVERIFY(view); movePoint(*view, press + QPointF(10, -10)); }
        QVERIFY(view.isNull());
    }

    void selectsEventsWithoutCurvesAndPaintsPlayhead()
    {
        CMotionTimeline timeline;
        const double time = 2.5000000001;
        QVERIFY(timeline.SetEvents({QJsonObject{{"Time", time}, {"Value", QStringLiteral("口型 🌸")}}}));
        CMotionCurveView view;
        view.resize(320, 190);
        view.SetTimeline(&timeline);
        view.show();
        QSignalSpy selected(&view, &CMotionCurveView::EventSelected);
        QSignalSpy edits(&view, &CMotionCurveView::EditStarted);
        QTest::mouseClick(&view, Qt::LeftButton, Qt::NoModifier, QPoint(299, 27));
        QCOMPARE(selected.size(), 1);
        QCOMPARE(selected.first().at(0).toDouble(), time);
        QCOMPARE(selected.first().at(1).toString(), QStringLiteral("口型 🌸"));
        QVERIFY(edits.isEmpty());

        QImage start(view.size(), QImage::Format_ARGB32_Premultiplied);
        view.render(&start);
        view.SetTime(time / 2);
        QImage middle(view.size(), QImage::Format_ARGB32_Premultiplied);
        view.render(&middle);
        QVERIFY(start.pixelColor(170, 70) != middle.pixelColor(170, 70));
    }

    void dragsEventMarkerWithFrameSnappingAndKeepsClickSelection()
    {
        CMotionTimeline timeline;
        QVERIFY(timeline.SetEvents({QJsonObject{{"Time", 1.0}, {"Value", QStringLiteral("cue")}}}));
        QVERIFY(timeline.SetTiming(10, 4));
        CMotionCurveView view;
        view.resize(320, 190); view.SetFrameSnapping(true); view.SetTimeline(&timeline); view.show();
        QSignalSpy selected(&view, &CMotionCurveView::EventSelected);
        QSignalSpy started(&view, &CMotionCurveView::EventDragStarted);
        QSignalSpy dragged(&view, &CMotionCurveView::EventDragged);
        QSignalSpy finished(&view, &CMotionCurveView::EventDragFinished);
        const QPoint press(102, 27);
        QTest::mousePress(&view, Qt::LeftButton, Qt::NoModifier, press);
        movePoint(view, press + QPoint(68, 0));
        QTest::mouseRelease(&view, Qt::LeftButton, Qt::NoModifier, press + QPoint(68, 0));
        QCOMPARE(selected.size(), 1);
        QCOMPARE(started.size(), 1);
        QVERIFY(!dragged.isEmpty());
        QCOMPARE(started.first().at(0).toInt(), 0);
        QCOMPARE(started.first().at(1).toDouble(), 1.0);
        QCOMPARE(dragged.last().at(0).toInt(), 0);
        QCOMPARE(dragged.last().at(1).toDouble(), 2.0);
        QCOMPARE(finished.size(), 1);
    }

    void eventLaneSurvivesTargetFilterAndExtendsPastLastKey()
    {
        CMotionTimeline timeline;
        QVERIFY(timeline.AddPoint("ParamA", 0.0f, 0.0f));
        QVERIFY(timeline.AddPoint("ParamA", 1.0f, 1.0f));
        QVERIFY(timeline.SetEvents({QJsonObject{{"Time", 3.0}, {"Value", "end"}}}));
        CMotionCurveView view;
        view.resize(320, 190);
        view.SetTimeline(&timeline);
        view.SetTargetFilter(static_cast<int>(CMotionTarget::PartOpacity));
        view.show();
        QSignalSpy selected(&view, &CMotionCurveView::EventSelected);
        QSignalSpy points(&view, &CMotionCurveView::PointSelected);
        QTest::mouseClick(&view, Qt::LeftButton, Qt::NoModifier, QPoint(299, 27));
        QCOMPARE(selected.size(), 1);
        QCOMPARE(selected.first().at(0).toDouble(), 3.0);
        QVERIFY(points.isEmpty());
        QImage image(view.size(), QImage::Format_ARGB32_Premultiplied);
        view.render(&image);
        view.SetTargetFilter(-1);
        QTest::mouseClick(&view, Qt::LeftButton, Qt::NoModifier, QPoint(125, 57));
        QCOMPARE(points.size(), 1);
        QCOMPARE(points.first().at(1).toInt(), 1);
        QCOMPARE(selected.size(), 1);
    }

    void groupsDenseEventsAndBoundsEscapedUnicodeTooltip()
    {
        CMotionTimeline timeline;
        QJsonArray events;
        const QString value = QStringLiteral("<img src=x>🌸 表情") + QString(1000, QChar(0x5b57));
        events.append(QJsonObject{{"Time", 1.0}, {"Value", value}});
        for (int i = 1; i < 100000; ++i)
            events.append(QJsonObject{{"Time", i == 1 ? 1.000000001 : 1.0}, {"Value", QStringLiteral("动作 %1").arg(i)}});
        QVERIFY(timeline.SetEvents(events));
        QVERIFY(timeline.SetTiming(30, 2));
        CMotionCurveView view;
        view.resize(320, 190);
        view.SetTimeline(&timeline);
        view.show();
        QSignalSpy selected(&view, &CMotionCurveView::EventSelected);
        QTest::mouseClick(&view, Qt::LeftButton, Qt::NoModifier, QPoint(170, 27));
        QCOMPARE(selected.size(), 1);
        QCOMPARE(selected.first().at(1).toString(), value);
        QHelpEvent help(QEvent::ToolTip, QPoint(170, 27), view.mapToGlobal(QPoint(170, 27)));
        QCoreApplication::sendEvent(&view, &help);
        QVERIFY(QToolTip::text().contains(QStringLiteral("100000 个事件")));
        QVERIFY(QToolTip::text().contains(QStringLiteral("&lt;img src=x&gt;🌸 表情")));
        QVERIFY(QToolTip::text().contains(QStringLiteral("99994")));
        QVERIFY(QToolTip::text().size() < 2000);
        QImage image(view.size(), QImage::Format_ARGB32_Premultiplied);
        view.render(&image);
        QVERIFY(timeline.SetEvents({QJsonObject{{"Time", 1.0}, {"Value", "fresh"}}}));
        view.SetTimeline(&timeline);
        QTRY_VERIFY(!QToolTip::isVisible());
        QCoreApplication::sendEvent(&view, &help);
        QVERIFY(QToolTip::text().contains(QStringLiteral("fresh")));
        QVERIFY(!QToolTip::text().contains(QStringLiteral("100000")));
        view.SetTimeline(nullptr);
        QTRY_VERIFY(!QToolTip::isVisible());
        QTest::mouseClick(&view, Qt::LeftButton, Qt::NoModifier, QPoint(170, 27));
        QCOMPARE(selected.size(), 1);
        view.render(&image);
    }

    void handlesExtremeEventTimesAndSmallWidth()
    {
        CMotionTimeline timeline;
        const double time = std::numeric_limits<float>::max();
        QVERIFY(timeline.SetEvents({QJsonObject{{"Time", time}, {"Value", "late"}}}));
        CMotionCurveView view;
        view.resize(320, 190);
        view.SetTimeline(&timeline);
        view.SetTime(time / 2);
        view.show();
        QSignalSpy selected(&view, &CMotionCurveView::EventSelected);
        QTest::mouseClick(&view, Qt::LeftButton, Qt::NoModifier, QPoint(299, 27));
        QCOMPARE(selected.first().at(0).toDouble(), time);
        CMotionTimeline shortTimeline;
        QVERIFY(shortTimeline.SetEvents({QJsonObject{{"Time", 1e-200}, {"Value", "short"}}}));
        view.SetTimeline(&shortTimeline);
        QTest::mouseClick(&view, Qt::LeftButton, Qt::NoModifier, QPoint(299, 27));
        QCOMPARE(selected.size(), 2);
        QCOMPARE(selected.last().at(0).toDouble(), 1e-200);
        view.resize(40, 150);
        view.SetTime(std::numeric_limits<double>::quiet_NaN());
        QImage image(view.size(), QImage::Format_ARGB32_Premultiplied);
        view.render(&image);
        QTest::mouseClick(&view, Qt::LeftButton, Qt::NoModifier, QPoint(34, 27));
        QCOMPARE(selected.size(), 2);
    }

    void filtersOverlappingTracksBeforeSelectionAndDragging()
    {
        CMotionTimeline timeline;
        for (auto kind : {CMotionTarget::Model, CMotionTarget::Parameter, CMotionTarget::PartOpacity}) {
            QVERIFY(timeline.AddPoint({kind, "SameId"}, 0.25f, -1));
            QVERIFY(timeline.AddPoint({kind, "SameId"}, 0.75f, 1));
        }
        CMotionCurveView view;
        view.resize(320, 190);
        view.SetTimeline(&timeline);
        view.show();
        QSignalSpy selected(&view, &CMotionCurveView::PointSelected);
        QSignalSpy edited(&view, &CMotionCurveView::PointEdited);
        QSignalSpy finished(&view, &CMotionCurveView::EditFinished);
        for (auto kind : {CMotionTarget::Model, CMotionTarget::Parameter, CMotionTarget::PartOpacity}) {
            view.SetTargetFilter(static_cast<int>(kind));
            QTest::mousePress(&view, Qt::LeftButton, Qt::NoModifier, QPoint(125, 161));
            QCOMPARE(selected.last().first().value<CMotionTrack>().target, kind);
            QMouseEvent move(QEvent::MouseMove, QPointF(160, 130), QPointF(160, 130), QPointF(160, 130),
                             Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
            QCoreApplication::sendEvent(&view, &move);
            QCOMPARE(edited.last().first().value<CMotionTrack>().target, kind);
            view.SetTargetFilter(99);
            const int edits = edited.size();
            QCoreApplication::sendEvent(&view, &move);
            QCOMPARE(edited.size(), edits);
            QTest::mouseRelease(&view, Qt::LeftButton, Qt::NoModifier, QPoint(160, 130));
        }
        QCOMPARE(finished.size(), 3);
        const int selections = selected.size();
        QTest::mouseClick(&view, Qt::LeftButton, Qt::NoModifier, QPoint(125, 161));
        QCOMPARE(selected.size(), selections);
    }

    void handlesLargeFiniteValues()
    {
        CMotionTimeline timeline;
        const float limit = std::numeric_limits<float>::max();
        QVERIFY(timeline.AddPoint("A", 0.0f, -limit));
        QVERIFY(timeline.AddPoint("A", limit, limit));
        CMotionCurveView view;
        view.resize(320, 190);
        view.SetTimeline(&timeline);
        view.SetTime(limit * 0.5f);
        view.show();
        QImage image(view.size(), QImage::Format_ARGB32_Premultiplied);
        view.render(&image);
        QSignalSpy selected(&view, &CMotionCurveView::PointSelected);
        QSignalSpy edited(&view, &CMotionCurveView::PointEdited);
        QTest::mousePress(&view, Qt::LeftButton, Qt::NoModifier, QPoint(34, 168));
        QCOMPARE(selected.size(), 1);
        QMouseEvent move(QEvent::MouseMove, QPointF(140, 80), QPointF(140, 80), QPointF(140, 80),
                         Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(&view, &move);
        QCOMPARE(edited.size(), 1);
        QVERIFY(std::isfinite(edited.first().at(2).toFloat()));
        QVERIFY(std::isfinite(edited.first().at(3).toFloat()));
        QTest::mouseRelease(&view, Qt::LeftButton, Qt::NoModifier, QPoint(140, 80));
    }

    void endsDragWhenMouseButtonIsLost()
    {
        CMotionTimeline timeline;
        QVERIFY(timeline.AddPoint("A", 0.25f, -1));
        QVERIFY(timeline.AddPoint("A", 0.75f, 1));
        CMotionCurveView view;
        view.resize(320, 190);
        view.SetTimeline(&timeline);
        view.show();
        QSignalSpy finished(&view, &CMotionCurveView::EditFinished);
        QSignalSpy edited(&view, &CMotionCurveView::PointEdited);
        QTest::mousePress(&view, Qt::LeftButton, Qt::NoModifier, QPoint(125, 161));
        QMouseEvent move(QEvent::MouseMove, QPointF(160, 130), QPointF(160, 130), QPointF(160, 130),
                         Qt::NoButton, Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(&view, &move);
        QCOMPARE(finished.size(), 1);
        QVERIFY(edited.isEmpty());
        QTest::mouseRelease(&view, Qt::LeftButton, Qt::NoModifier, QPoint(160, 130));
        QCOMPARE(finished.size(), 1);
    }

    void rendersTimeline()
    {
        CMotionTimeline timeline;
        QVERIFY(timeline.AddPoint(QStringLiteral("ParamA"), 0.0f, -1.0f));
        QVERIFY(timeline.AddPoint(QStringLiteral("ParamA"), 1.0f, 1.0f));
        CMotionCurveView view;
        view.resize(320, 190);
        view.SetTimeline(&timeline);
        view.SetTime(0.5f);
        QImage image(view.size(), QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        view.render(&image);
        QVERIFY(!image.isNull());
        QCOMPARE(view.objectName(), QStringLiteral("motionCurveView"));
    }

    void selectsAndEditsPoint()
    {
        CMotionTimeline timeline;
        QVERIFY(timeline.AddPoint(QStringLiteral("ParamA"), 0.25f, -1.0f));
        QVERIFY(timeline.AddPoint(QStringLiteral("ParamA"), 0.75f, 1.0f));
        CMotionCurveView view;
        view.resize(320, 190);
        view.SetTimeline(&timeline);
        view.show();
        QSignalSpy selected(&view, &CMotionCurveView::PointSelected);
        QSignalSpy edited(&view, &CMotionCurveView::PointEdited);
        QMouseEvent click(QEvent::MouseButtonPress, QPointF(125, 161), QPointF(125, 161), QPointF(125, 161), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(&view, &click);
        QCOMPARE(selected.size(), 1);
        QCOMPARE(selected.at(0).at(0).value<CMotionTrack>().id, QStringLiteral("ParamA"));
        QCOMPARE(selected.at(0).at(1).toInt(), 0);
        QMouseEvent press(QEvent::MouseButtonPress, QPointF(125, 161), QPointF(125, 161), QPointF(125, 161), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(&view, &press);
        QMouseEvent move(QEvent::MouseMove, QPointF(150, 140), QPointF(150, 140), QPointF(150, 140), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(&view, &move);
        QMouseEvent release(QEvent::MouseButtonRelease, QPointF(150, 140), QPointF(150, 140), QPointF(150, 140), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(&view, &release);
        QVERIFY(!edited.isEmpty());
        QVERIFY(edited.last().at(2).toFloat() > 0.25f);
        QVERIFY(edited.last().at(3).toFloat() > -1.0f);
    }
};

QTEST_MAIN(CMotionCurveViewTests)
#include "cmotioncurveview_tests.moc"
