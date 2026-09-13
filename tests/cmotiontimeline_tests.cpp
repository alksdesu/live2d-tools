#include "ui/cmotiontimeline.h"
#include <QtTest>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QFile>

class CMotionTimelineTests : public QObject
{
    Q_OBJECT
private slots:
    void writesCubismMotionFormat()
    {
        CMotionTimeline timeline;
        QVERIFY(timeline.AddPoint(QStringLiteral("ParamAngleX"), 0.0f, -1.0f));
        QVERIFY(timeline.AddPoint(QStringLiteral("ParamAngleX"), 0.5f, 1.0f));
        QVERIFY(timeline.AddPoint(QStringLiteral("ParamAngleY"), 0.0f, 0.25f));
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString path = temp.filePath(QStringLiteral("test.motion3.json"));
        QString error;
        QVERIFY2(timeline.Save(path, 30.0f, true, &error), qPrintable(error));
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
        QCOMPARE(root.value(QStringLiteral("Version")).toInt(), 3);
        const QJsonObject meta = root.value(QStringLiteral("Meta")).toObject();
        QCOMPARE(meta.value(QStringLiteral("CurveCount")).toInt(), 2);
        QCOMPARE(meta.value(QStringLiteral("TotalSegmentCount")).toInt(), 2);
        QCOMPARE(meta.value(QStringLiteral("TotalPointCount")).toInt(), 4);
        QCOMPARE(root.value(QStringLiteral("Curves")).toArray().size(), 2);
    }

    void singlePointGetsValidSegment()
    {
        CMotionTimeline timeline;
        QVERIFY(timeline.AddPoint(QStringLiteral("ParamA"), 0.0f, 0.5f));
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QString error;
        QVERIFY(timeline.Save(temp.filePath(QStringLiteral("single.motion3.json")), 30.0f, false, &error));
        QFile file(temp.filePath(QStringLiteral("single.motion3.json")));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QJsonArray segments = QJsonDocument::fromJson(file.readAll()).object()
            .value(QStringLiteral("Curves")).toArray().first().toObject().value(QStringLiteral("Segments")).toArray();
        QCOMPARE(segments.size(), 5);
        QCOMPARE(segments.at(2).toInt(), 0);
    }

    void separatesSameTimestampValues()
    {
        CMotionTimeline timeline;
        QVERIFY(timeline.AddPoint(QStringLiteral("ParamA"), 1.0f, 0.0f));
        QVERIFY(timeline.AddPoint(QStringLiteral("ParamA"), 1.0f, 1.0f));
        const auto points = timeline.Curves().value(QStringLiteral("ParamA"));
        QCOMPARE(points.size(), 2);
        QVERIFY(points.at(1).x() > points.at(0).x());
    }
};

QTEST_GUILESS_MAIN(CMotionTimelineTests)
#include "cmotiontimeline_tests.moc"
