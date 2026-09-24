#include "live2dstruct/cmotiontimeline.h"
#include <QtTest>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QFile>
#include <QDirIterator>
#include <limits>
#include <cmath>

class CMotionTimelineTests : public QObject
{
    Q_OBJECT
private:
    static bool writeJson(const QString& path, const QJsonObject& root)
    {
        QFile file(path);
        const QByteArray bytes = QJsonDocument(root).toJson();
        return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
    }

    static QJsonObject readJson(const QString& path)
    {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) return {};
        return QJsonDocument::fromJson(file.readAll()).object();
    }

    static QJsonObject curve(const QString& target, const QString& id, const QJsonArray& points)
    {
        return {{"Target", target}, {"Id", id}, {"Segments", points}};
    }

private slots:

    void typedTracksRemainIndependentThroughEditingAndStorage()
    {
        CMotionTimeline timeline;
        const CMotionTrack model{CMotionTarget::Model, "Opacity"};
        const CMotionTrack parameter{CMotionTarget::Parameter, "Opacity"};
        const CMotionTrack part{CMotionTarget::PartOpacity, "Opacity"};
        for (const auto& track : {model, parameter, part}) {
            QVERIFY(timeline.AddPoint(track, 0, 0));
            QVERIFY(timeline.AddPoint(track, 2, 1));
        }
        QVERIFY(timeline.InsertPoint(model, 1, 0.25f));
        QVERIFY(timeline.UpdatePoint(parameter, 1, 2, 4));
        QVERIFY(timeline.SetSegmentType(part, 1, 2));
        const auto frame = timeline.EvaluateFrame(1);
        QCOMPARE(frame.model.value("Opacity"), 0.25f);
        QCOMPARE(frame.parameters.value("Opacity"), 2.0f);
        QCOMPARE(frame.parts.value("Opacity"), 0.0f);
        QCOMPARE(timeline.TotalPointCount(), 7);
        QCOMPARE(timeline.PointCount(), 2);
        QVERIFY(!timeline.InsertPoint(model, 1, 9));
        QVERIFY(!timeline.InsertPoint({static_cast<CMotionTarget>(99), "bad"}, 1, 0));
        QVERIFY(!timeline.InsertPoint(part, -1, 0));
        QVERIFY(!timeline.InsertPoint(part, 1, std::numeric_limits<float>::infinity()));
        QVERIFY(timeline.RemovePoint(parameter, 0));
        QCOMPARE(timeline.Tracks().value(model).size(), 3);
        QCOMPARE(timeline.Tracks().value(part).size(), 2);
        QJsonObject state;
        QVERIFY(timeline.SaveState(&state, true, 1));
        CMotionTimeline restored;
        float playhead = 0;
        QVERIFY(restored.RestoreState(state, &playhead));
        QCOMPARE(playhead, 1.0f);
        QCOMPARE(restored.Tracks(), timeline.Tracks());
        QCOMPARE(restored.SegmentType(part, 1), 2);
        QTemporaryDir temp;
        const auto path = temp.filePath("typed.motion3.json");
        QVERIFY(restored.Save(path, 60, true));
        CMotionTimeline exported;
        QVERIFY(exported.Load(path));
        QCOMPARE(exported.EvaluateFrame(1).model, frame.model);
        QCOMPARE(exported.EvaluateFrame(1).parts, frame.parts);
        QCOMPARE(exported.EvaluateFrame(1).parameters.value("Opacity"), 4.0f);
        const auto curves = readJson(path).value("Curves").toArray();
        QCOMPARE(curves.at(0).toObject().value("Target").toString(), QString("Model"));
        QCOMPARE(curves.at(1).toObject().value("Target").toString(), QString("Parameter"));
        QCOMPARE(curves.at(2).toObject().value("Target").toString(), QString("PartOpacity"));
    }

    void nonParameterBezierInsertionPreservesValidSegments()
    {
        QTemporaryDir temp;
        const auto path = temp.filePath("typed.motion3.json");
        for (const auto target : {CMotionTarget::Model, CMotionTarget::PartOpacity}) {
            const CMotionTrack track{target, "Opacity"};
            auto original = curve(track.TargetName(), track.id, {0, 0, 1, 0.5, 0, 1.5, 1, 2, 1});
            original.insert("FadeInTime", 0.5);
            original.insert("Extension", true);
            QVERIFY(writeJson(path, {{"Version", 3}, {"Curves", QJsonArray{original}}}));
            CMotionTimeline timeline;
            QVERIFY(timeline.Load(path));
            QVERIFY(timeline.InsertPoint(track, 1, 0.25f));
            QCOMPARE(timeline.SegmentType(track, 2), 1);
            QVERIFY(timeline.Save(path, 30, false));
            CMotionTimeline restored;
            QVERIFY(restored.Load(path));
            for (float time : {0.0f, 0.5f, 1.0f, 1.25f, 1.75f, 2.0f})
                QCOMPARE(restored.Evaluate(track, time), timeline.Evaluate(track, time));
            auto saved = readJson(path).value("Curves").toArray().first().toObject();
            QCOMPARE(saved.value("FadeInTime").toDouble(), 0.5);
            QVERIFY(saved.value("Extension").toBool());
            QVERIFY(timeline.RemovePoint(track, 1));
            QVERIFY(timeline.InsertPoint(track, 0.5f, 0.2f));
            QVERIFY(timeline.Save(path, 30, false));
            QVERIFY(restored.Load(path));
            QCOMPARE(restored.Evaluate(track, 1.5f), timeline.Evaluate(track, 1.5f));
        }
    }

    void modelEffectsComposeWithoutAccumulation()
    {
        CMotionPreviewFrame frame;
        frame.parameters = {{"EyeL", 0.8f}, {"Mouth", 0.2f}, {"Both", 0.4f}, {"Body", 5}};
        frame.model = {{"EyeBlink", 0.5f}, {"LipSync", 0.3f}, {"Opacity", 0.25f}};
        frame.parts = {{"PartA", 0.6f}};
        const QSet<QString> eyes{"EyeL", "EyeR", "Both", "Shared"};
        const QSet<QString> lips{"Mouth", "Mouth2", "Both", "Shared"};
        const auto values = frame.ResolveParameters(eyes, lips);
        QCOMPARE(values.value("EyeL"), 0.4f);
        QCOMPARE(values.value("EyeR"), 0.5f);
        QCOMPARE(values.value("Mouth"), 0.5f);
        QCOMPARE(values.value("Mouth2"), 0.3f);
        QCOMPARE(values.value("Both"), 0.5f);
        QCOMPARE(values.value("Shared"), 0.3f);
        QCOMPARE(values.value("Body"), 5.0f);
        QVERIFY(!values.contains("Opacity"));
        QVERIFY(!values.contains("PartA"));
        QCOMPARE(frame.ResolveParameters(eyes, lips), values);
        QCOMPARE(frame.parameters.value("EyeL"), 0.8f);
        frame.parameters["Both"] = std::numeric_limits<float>::max();
        frame.model["EyeBlink"] = std::numeric_limits<float>::max();
        QVERIFY(std::isfinite(frame.ResolveParameters(eyes, lips).value("Both")));
        QVERIFY(frame.IsFinite());
        frame.parts["PartA"] = std::numeric_limits<float>::quiet_NaN();
        QVERIFY(!frame.IsFinite());
        frame.parts.clear();
        frame.model[QString("Bad") + QChar::Null] = 1;
        QVERIFY(!frame.IsFinite());
    }
    void preservesMixedMotionData()
    {
        QTemporaryDir temp;
        auto parameter = curve("Parameter", "ParamA", {0, 0, 0, 1, 10});
        parameter.insert("FadeInTime", 0.25);
        parameter.insert("FadeOutTime", 0.75);
        parameter.insert("Custom", "keep");
        const auto model = curve("Model", "Opacity", {0, 1, 0, 2, 0.5});
        const auto part = curve("PartOpacity", "PartA", {0, 0, 2, 3, 1});
        const QJsonArray events{QJsonObject{{"Time", 0.5}, {"Value", QStringLiteral("口型🌸")}}};
        const QJsonObject root{{"Version", 3}, {"FadeInTime", 0.4}, {"FadeOutTime", 0.6}, {"Custom", 42},
            {"Meta", QJsonObject{{"Duration", 5}, {"Fps", 60}, {"Loop", true}, {"AreBeziersRestricted", false},
                                 {"CurveCount", 900}, {"TotalSegmentCount", 900}, {"TotalPointCount", 900}}},
            {"Curves", QJsonArray{model, parameter, part}}, {"UserData", events}};
        const QString input = temp.filePath("input.motion3.json");
        const QString output = temp.filePath("output.motion3.json");
        QVERIFY(writeJson(input, root));
        CMotionTimeline timeline;
        QString error;
        QVERIFY2(timeline.Load(input, &error), qPrintable(error));
        QCOMPARE(timeline.Fps(), 60.0f);
        QVERIFY(timeline.Loop());
        QCOMPARE(timeline.Duration(), 5.0f);
        QVERIFY(timeline.UpdatePoint("ParamA", 1, 1.5f, 20.0f));
        QVERIFY2(timeline.Save(output, timeline.Fps(), timeline.Loop(), &error), qPrintable(error));
        const auto saved = readJson(output);
        const auto curves = saved.value("Curves").toArray();
        QCOMPARE(curves.size(), 3);
        QCOMPARE(curves.at(0), QJsonValue(model));
        QCOMPARE(curves.at(2), QJsonValue(part));
        QCOMPARE(curves.at(1).toObject().value("FadeInTime").toDouble(), 0.25);
        QCOMPARE(curves.at(1).toObject().value("FadeOutTime").toDouble(), 0.75);
        QCOMPARE(curves.at(1).toObject().value("Segments").toArray(), (QJsonArray{0, 0, 0, 1.5, 20}));
        QCOMPARE(saved.value("UserData").toArray(), events);
        QCOMPARE(saved.value("FadeInTime"), root.value("FadeInTime"));
        QCOMPARE(saved.value("FadeOutTime"), root.value("FadeOutTime"));
        QCOMPARE(saved.value("Custom"), root.value("Custom"));
        const auto meta = saved.value("Meta").toObject();
        QCOMPARE(meta.value("Duration").toDouble(), 5.0);
        QCOMPARE(meta.value("TotalPointCount").toInt(), 6);
        QCOMPARE(meta.value("TotalSegmentCount").toInt(), 3);
        QCOMPARE(meta.value("CurveCount").toInt(), 3);
        QCOMPARE(meta.value("UserDataCount").toInt(), 1);
        QCOMPARE(meta.value("TotalUserDataSize").toInt(), QStringLiteral("口型🌸").toUtf8().size());
        QVERIFY(timeline.RemovePoint("ParamA", 1));
        QVERIFY(timeline.RemovePoint("ParamA", 0));
        QVERIFY(timeline.Save(output, timeline.Fps(), false, &error));
        CMotionTimeline nonParameter;
        QVERIFY(nonParameter.Load(output, &error));
        QCOMPARE(nonParameter.PointCount(), 0);
        QCOMPARE(readJson(output).value("Curves").toArray(), (QJsonArray{model, part}));
        timeline.Clear();
        QVERIFY(timeline.AddPoint("New", 0, 1));
        QVERIFY(timeline.Save(output, timeline.Fps(), false, &error));
        QCOMPARE(readJson(output).value("Curves").toArray().size(), 1);
        QVERIFY(!readJson(output).contains("UserData"));
        QCOMPARE(timeline.Fps(), 30.0f);
    }

    void matchesSdkBezierModes_data()
    {
        QTest::addColumn<bool>("restricted");
        QTest::addColumn<double>("expected");
        QTest::newRow("unrestricted") << false << 0.5;
        QTest::newRow("restricted") << true << 0.04296875;
    }

    void matchesSdkBezierModes()
    {
        QFETCH(bool, restricted);
        QFETCH(double, expected);
        QTemporaryDir temp;
        const QString path = temp.filePath("curve.motion3.json");
        const QJsonArray points{0, 0, 1, 0, 0, 0, 1, 1, 1};
        QVERIFY(writeJson(path, {{"Version", 3}, {"Meta", QJsonObject{{"AreBeziersRestricted", restricted}}},
                                 {"Curves", QJsonArray{curve("Parameter", "A", points)}}}));
        CMotionTimeline timeline;
        QString error;
        QVERIFY2(timeline.Load(path, &error), qPrintable(error));
        QVERIFY(qAbs(timeline.Evaluate("A", 0.125f) - expected) < 0.0001);
        QVERIFY(timeline.Save(path, 30, false, &error));
        QCOMPARE(readJson(path).value("Meta").toObject().value("AreBeziersRestricted").toBool(), restricted);
        QCOMPARE(readJson(path).value("Curves").toArray().first().toObject().value("Segments").toArray(), points);
        QVERIFY(timeline.UpdatePoint("A", 1, 0.5f, 1.0f));
        QVERIFY(qAbs(timeline.Evaluate("A", 0.0625f) - expected) < 0.0001);
        QVERIFY(timeline.Save(path, 30, false, &error));
        CMotionTimeline reloaded;
        QVERIFY(reloaded.Load(path, &error));
        for (float time : {0.0f, 0.0625f, 0.125f, 0.25f, 0.499f, 0.5f})
            QVERIFY(qAbs(reloaded.Evaluate("A", time) - timeline.Evaluate("A", time)) < 0.0001f);
    }

    void acceptsCrossedBezierHandles()
    {
        QTemporaryDir temp;
        const QString path = temp.filePath("curve.motion3.json");
        const QJsonArray points{0, 0, 1, 0.8, 0, 0.2, 1, 1, 1};
        QVERIFY(writeJson(path, {{"Version", 3}, {"Curves", QJsonArray{curve("Parameter", "A", points)}}}));
        CMotionTimeline timeline;
        QString error;
        QVERIFY2(timeline.Load(path, &error), qPrintable(error));
        QVERIFY(qAbs(timeline.Evaluate("A", 0.38125f) - 0.15625f) < 0.0001f);
        QVERIFY(timeline.Save(path, 30, false, &error));
        QCOMPARE(readJson(path).value("Curves").toArray().first().toObject().value("Segments").toArray(), points);
    }

    void handlesSinglePointAndStepBoundaries()
    {
        QTemporaryDir temp;
        const QString path = temp.filePath("curve.motion3.json");
        CMotionTimeline timeline;
        QVERIFY(timeline.AddPoint("A", 5.0f, 1.0f));
        QVERIFY(timeline.Save(path, 30, false));
        CMotionTimeline reloaded;
        QVERIFY(reloaded.Load(path));
        QVERIFY(reloaded.Curves().value("A").last().x() > 5.0);
        timeline.Clear();
        QVERIFY(timeline.AddPoint("Step", 0, 1));
        QVERIFY(timeline.AddPoint("Step", 1, 9));
        QVERIFY(timeline.AddPoint("Step", 2, 4));
        QVERIFY(timeline.SetSegmentType("Step", 1, 2));
        QVERIFY(timeline.SetSegmentType("Step", 2, 2));
        QCOMPARE(timeline.Evaluate("Step", 0.999f), 1.0f);
        QCOMPARE(timeline.Evaluate("Step", 1), 9.0f);
        QCOMPARE(timeline.Evaluate("Step", 2), 4.0f);
        QVERIFY(timeline.SetSegmentType("Step", 1, 3));
        QCOMPARE(timeline.Evaluate("Step", 0), 9.0f);
        timeline.Clear();
        const float maximum = std::numeric_limits<float>::max();
        QVERIFY(timeline.AddPoint("A", maximum, 1));
        QVERIFY(!timeline.AddPoint("A", maximum, 2));
        QVERIFY(!timeline.Save(path, 30, false));
        QVERIFY(reloaded.Load(path));
        QVERIFY(std::isfinite(reloaded.Duration()));
    }

    void rejectsInvalidCurvesWithoutChangingDraft_data()
    {
        QTest::addColumn<QJsonArray>("segments");
        QTest::newRow("first-time-type") << QJsonArray{"bad", 0, 0, 1, 1};
        QTest::newRow("end-time-type") << QJsonArray{0, 0, 0, "bad", 1};
        QTest::newRow("start-value-type") << QJsonArray{0, "bad", 0, 1, 1};
        QTest::newRow("huge-segment") << QJsonArray{0, 0, 1e100, 1, 1};
        QTest::newRow("huge-value") << QJsonArray{0, 0, 0, 1, 1e100};
        QTest::newRow("fractional-segment") << QJsonArray{0, 0, 0.5, 1, 1};
        QTest::newRow("truncated-bezier") << QJsonArray{0, 0, 1, 0.3, 0};
        QTest::newRow("float-time-collision") << QJsonArray{1, 0, 0, 1.000000001, 1};
        QTest::newRow("negative-control") << QJsonArray{0, 0, 1, -1, 0, 0.3, 0, 1, 1};
    }

    void rejectsInvalidCurvesWithoutChangingDraft()
    {
        QFETCH(QJsonArray, segments);
        QTemporaryDir temp;
        const QString path = temp.filePath("invalid.motion3.json");
        QVERIFY(writeJson(path, {{"Version", 3}, {"Curves", QJsonArray{curve("Parameter", "A", segments)}}}));
        CMotionTimeline timeline;
        QVERIFY(timeline.AddPoint("Original", 0, 42));
        timeline.StartRecording();
        QString error;
        QVERIFY(!timeline.Load(path, &error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(timeline.PointCount(), 1);
        QCOMPARE(timeline.Evaluate("Original", 0), 42.0f);
        QVERIFY(timeline.IsRecording());
    }

    void preservesOfficialMotions()
    {
        QTemporaryDir temp;
        QDirIterator files(QString(TEST_RESOURCES), {"*.motion3.json"}, QDir::Files, QDirIterator::Subdirectories);
        int tested = 0;
        while (files.hasNext()) {
            const QString path = files.next();
            const QJsonObject original = readJson(path);
            CMotionTimeline timeline;
            QString error;
            QVERIFY2(timeline.Load(path, &error), qPrintable(path + ": " + error));
            const QString output = temp.filePath("roundtrip.motion3.json");
            QVERIFY2(timeline.Save(output, timeline.Fps(), timeline.Loop(), &error), qPrintable(error));
            const auto saved = readJson(output);
            const auto savedCurves = saved.value("Curves").toArray();
            QCOMPARE(savedCurves.size(), original.value("Curves").toArray().size());
            for (const auto& sourceCurve : original.value("Curves").toArray()) {
                const auto source = sourceCurve.toObject();
                bool matched = false;
                for (const auto& savedCurve : savedCurves) {
                    const auto target = savedCurve.toObject();
                    if (target.value("Id") == source.value("Id") && target.value("Target") == source.value("Target")) {
                        QCOMPARE(target, source);
                        matched = true;
                        break;
                    }
                }
                QVERIFY(matched);
            }
            CMotionTimeline reloaded;
            QVERIFY(reloaded.Load(output, &error));
            for (const auto& track : timeline.Tracks().keys()) {
                for (int sample = 0; sample <= 10; ++sample) {
                    const float time = timeline.Duration() * sample / 10.0f;
                    QVERIFY(qAbs(timeline.Evaluate(track, time) - reloaded.Evaluate(track, time)) < 0.0001f);
                }
            }
            ++tested;
        }
        QVERIFY(tested > 0);
    }

    void rejectsInvalidMetadataAndEvents()
    {
        const QJsonObject valid{{"Version", 3}, {"Curves", QJsonArray{curve("Parameter", "A", {0, 0, 0, 1, 1})}}};
        const QVector<QPair<QString, QJsonValue>> invalidFields{
            {"Version", 3.5}, {"Meta", QJsonArray{}},
            {"Meta", QJsonObject{{"Fps", 0}}}, {"Meta", QJsonObject{{"Duration", "bad"}}},
            {"Meta", QJsonObject{{"AreBeziersRestricted", 1}}}, {"Meta", QJsonObject{{"Loop", "true"}}},
            {"FadeInTime", "bad"}, {"FadeOutTime", 1e100}, {"UserData", QJsonObject{}},
            {"UserData", QJsonArray{QJsonObject{{"Time", "bad"}, {"Value", "text"}}}},
            {"UserData", QJsonArray{QJsonObject{{"Time", 0}, {"Value", 3}}}},
            {"Curves", QJsonArray{curve("InvalidTarget", "A", {0, 0, 0, 1, 1})}},
            {"Curves", QJsonArray{curve("Parameter", "A", {0, 0, 0, 1, 1}), curve("Parameter", "A", {0, 0, 0, 1, 1})}}
        };
        QTemporaryDir temp;
        const QString path = temp.filePath("input.json");
        CMotionTimeline timeline;
        QVERIFY(writeJson(path, valid));
        QVERIFY(timeline.Load(path));
        for (const auto& field : invalidFields) {
            auto root = valid;
            root.insert(field.first, field.second);
            QVERIFY(writeJson(path, root));
            QString error;
            QVERIFY2(!timeline.Load(path, &error), qPrintable(field.first));
            QVERIFY(!error.isEmpty());
            QCOMPARE(timeline.PointCount(), 2);
        }
    }

    void retargetsBezierAfterDeletingOrReorderingKeys()
    {
        QTemporaryDir temp;
        const QString path = temp.filePath("curve.json");
        const QJsonArray points{0, 0, 1, 0.2, 0, 0.6, 0.5, 1, 1, 1, 1.2, 1.5, 1.5, 2, 2, 2};
        QVERIFY(writeJson(path, {{"Version", 3}, {"Curves", QJsonArray{curve("Parameter", "A", points)}}}));
        for (bool remove : {true, false}) {
            CMotionTimeline timeline;
            QString error;
            QVERIFY(timeline.Load(path, &error));
            if (remove) QVERIFY(timeline.RemovePoint("A", 1));
            else QVERIFY(timeline.UpdatePoint("A", 2, 0.5, 2));
            const QString output = temp.filePath("saved.json");
            QVERIFY(timeline.Save(output, 30, false, &error));
            CMotionTimeline loaded;
            QVERIFY2(loaded.Load(output, &error), qPrintable(error));
            for (float time : {0.0f, 0.25f, 0.75f, 1.0f, 1.5f, 2.0f})
                QVERIFY(qAbs(loaded.Evaluate("A", time) - timeline.Evaluate("A", time)) < 0.0001f);
        }
    }

    void recordingResumesAfterExistingClip()
    {
        CMotionTimeline timeline;
        QVERIFY(timeline.AddPoint("A", 5, 0));
        timeline.StartRecording();
        QTest::qWait(20);
        timeline.Record("A", 1);
        const auto points = timeline.Curves().value("A");
        QCOMPARE(points.size(), 2);
        QVERIFY(points.last().x() >= 5.01);
        timeline.Clear();
        QVERIFY(!timeline.IsRecording());
        timeline.Record("A", 2);
        QCOMPARE(timeline.PointCount(), 0);
    }

    void snapshotPreservesCurvesAndStopsRecordingOnRestore()
    {
        const QString path = QString(TEST_RESOURCES) + "/Mark/motions/mark_m06.motion3.json";
        CMotionTimeline original;
        QVERIFY(original.Load(path));
        const QString id = original.Curves().firstKey();
        QVERIFY(original.UpdatePoint(id, 0, 0.0f, 0.25f));
        original.StartRecording();
        QJsonObject state;
        QVERIFY(original.SaveState(&state, true, original.Duration() * 0.5f));
        QVERIFY(original.IsRecording());
        CMotionTimeline restored;
        float time = 0;
        QVERIFY(restored.RestoreState(state, &time));
        QVERIFY(!restored.IsRecording());
        QCOMPARE(restored.Curves(), original.Curves());
        QJsonObject repeated;
        QVERIFY(restored.SaveState(&repeated, restored.Loop(), time));
        QCOMPARE(repeated, state);
        for (const auto& parameter : original.Curves().keys())
            QVERIFY(qAbs(original.Evaluate(parameter, time) - restored.Evaluate(parameter, time)) < 0.0001f);
    }

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

    void separatesImportedFractionalTimestamps()
    {
        QTemporaryDir temp;
        const QString path = temp.filePath("curve.json");
        QVERIFY(writeJson(path, {{"Version", 3}, {"Curves", QJsonArray{curve("Parameter", "A", {0, 0, 0, 0.1, 1})}}}));
        CMotionTimeline timeline;
        QVERIFY(timeline.Load(path));
        QVERIFY(!timeline.AddPoint("A", 0.1f, 1));
        QVERIFY(timeline.AddPoint("A", 0.1f, 2));
        const auto points = timeline.Curves().value("A");
        QVERIFY(static_cast<float>(points.last().x()) > static_cast<float>(points.at(1).x()));
        QVERIFY(timeline.Save(path, 30, false));
        CMotionTimeline loaded;
        QVERIFY(loaded.Load(path));
        QCOMPARE(loaded.PointCount(), 3);
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
