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

    void editsAndRemovesPoints()
    {
        CMotionTimeline timeline;
        QVERIFY(timeline.AddPoint(QStringLiteral("ParamA"), 0.0f, 0.0f));
        QVERIFY(timeline.AddPoint(QStringLiteral("ParamA"), 1.0f, 1.0f));
        QVERIFY(timeline.UpdatePoint(QStringLiteral("ParamA"), 1, 0.25f, 0.5f));
        QCOMPARE(timeline.Curves().value(QStringLiteral("ParamA")).at(1).x(), 0.25);
        QVERIFY(!timeline.UpdatePoint(QStringLiteral("ParamA"), 0, 0.25f, 0.2f));
        QVERIFY(timeline.RemovePoint(QStringLiteral("ParamA"), 1));
        QCOMPARE(timeline.PointCount(), 1);
    }

    void writesBezierSegmentMetadata()
    {
        CMotionTimeline timeline;
        QVERIFY(timeline.AddPoint(QStringLiteral("ParamA"), 0.0f, 0.0f));
        QVERIFY(timeline.AddPoint(QStringLiteral("ParamA"), 1.0f, 1.0f));
        QVERIFY(timeline.SetSegmentType(QStringLiteral("ParamA"), 1, 1));
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QString error;
        QVERIFY(timeline.Save(temp.filePath(QStringLiteral("bezier.motion3.json")), 30.0f, false, &error));
        QFile file(temp.filePath(QStringLiteral("bezier.motion3.json")));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
        const QJsonArray segments = root.value(QStringLiteral("Curves")).toArray().first().toObject()
            .value(QStringLiteral("Segments")).toArray();
        QCOMPARE(segments.at(2).toInt(), 1);
        QCOMPARE(root.value(QStringLiteral("Meta")).toObject().value(QStringLiteral("TotalPointCount")).toInt(), 4);
    }

    void evaluatesInterpolation()
    {
        CMotionTimeline timeline;
        QVERIFY(timeline.AddPoint(QStringLiteral("Linear"), 0.0f, 0.0f));
        QVERIFY(timeline.AddPoint(QStringLiteral("Linear"), 1.0f, 10.0f));
        QCOMPARE(timeline.Evaluate(QStringLiteral("Linear"), 0.5f), 5.0f);
        QVERIFY(timeline.AddPoint(QStringLiteral("Step"), 0.0f, 1.0f));
        QVERIFY(timeline.AddPoint(QStringLiteral("Step"), 1.0f, 9.0f));
        QVERIFY(timeline.SetSegmentType(QStringLiteral("Step"), 1, 2));
        QCOMPARE(timeline.Evaluate(QStringLiteral("Step"), 0.5f), 1.0f);
        QCOMPARE(timeline.EvaluateAll(0.5f).size(), 2);
    }

    void loadsExportedMotion()
    {
        CMotionTimeline source;
        QVERIFY(source.AddPoint(QStringLiteral("ParamA"), 0.0f, 0.0f));
        QVERIFY(source.AddPoint(QStringLiteral("ParamA"), 1.0f, 1.0f));
        QVERIFY(source.SetSegmentType(QStringLiteral("ParamA"), 1, 2));
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString path = temp.filePath(QStringLiteral("roundtrip.motion3.json"));
        QString error;
        QVERIFY(source.Save(path, 30.0f, true, &error));
        CMotionTimeline loaded;
        QVERIFY2(loaded.Load(path, &error), qPrintable(error));
        QCOMPARE(loaded.Curves().keys(), QStringList{QStringLiteral("ParamA")});
        QCOMPARE(loaded.SegmentType(QStringLiteral("ParamA"), 1), 2);
        QCOMPARE(loaded.Evaluate(QStringLiteral("ParamA"), 0.5f), 0.0f);
        QVERIFY(!loaded.Load(temp.filePath(QStringLiteral("missing.motion3.json")), &error));
        QCOMPARE(loaded.PointCount(), 2);
    }

    void preservesCustomBezierControls()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QJsonArray segments{0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 1.0, 1.0};
        QJsonObject curve;
        curve.insert(QStringLiteral("Target"), QStringLiteral("Parameter"));
        curve.insert(QStringLiteral("Id"), QStringLiteral("ParamA"));
        curve.insert(QStringLiteral("Segments"), segments);
        QJsonArray curves;
        curves.append(curve);
        QJsonObject root;
        root.insert(QStringLiteral("Version"), 3);
        root.insert(QStringLiteral("Curves"), curves);
        const QString sourcePath = temp.filePath(QStringLiteral("custom.motion3.json"));
        QFile sourceFile(sourcePath);
        QVERIFY(sourceFile.open(QIODevice::WriteOnly));
        QVERIFY(sourceFile.write(QJsonDocument(root).toJson()) > 0);
        sourceFile.close();

        CMotionTimeline loaded;
        QString error;
        QVERIFY2(loaded.Load(sourcePath, &error), qPrintable(error));
        QVERIFY(qAbs(loaded.Evaluate(QStringLiteral("ParamA"), 0.125f) - 0.5f) < 0.001f);

        const QString outputPath = temp.filePath(QStringLiteral("custom-out.motion3.json"));
        QVERIFY2(loaded.Save(outputPath, 30.0f, false, &error), qPrintable(error));
        QFile outputFile(outputPath);
        QVERIFY(outputFile.open(QIODevice::ReadOnly));
        const QJsonArray outputSegments = QJsonDocument::fromJson(outputFile.readAll()).object()
            .value(QStringLiteral("Curves")).toArray().first().toObject().value(QStringLiteral("Segments")).toArray();
        QCOMPARE(outputSegments, segments);
    }
};

QTEST_GUILESS_MAIN(CMotionTimelineTests)
#include "cmotiontimeline_tests.moc"
