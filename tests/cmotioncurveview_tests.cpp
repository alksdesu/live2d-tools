#include "ui/cmotioncurveview.h"
#include <QtTest>
#include <QImage>
#include <QSignalSpy>
#include <QMouseEvent>

class CMotionCurveViewTests : public QObject
{
    Q_OBJECT
private slots:
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
        QCOMPARE(selected.at(0).at(0).toString(), QStringLiteral("ParamA"));
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
