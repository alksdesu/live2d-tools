#include "ui/cmotioncurveview.h"
#include <QtTest>
#include <QImage>

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
};

QTEST_MAIN(CMotionCurveViewTests)
#include "cmotioncurveview_tests.moc"
