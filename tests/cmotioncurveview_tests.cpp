#include "ui/cmotioncurveview.h"
#include <QtTest>
#include <QImage>
#include <QSignalSpy>
#include <QMouseEvent>
#include <QHelpEvent>
#include <QToolTip>
#include <limits>
#include <cmath>

class CMotionCurveViewTests : public QObject
{
    Q_OBJECT
private slots:
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
