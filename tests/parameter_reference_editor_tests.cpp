#include "live2dstruct/cparameterreferenceeditor.h"
#include "live2dstruct/cmotiontimeline.h"
#include <QCryptographicHash>
#include <QDirIterator>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QtTest>

using Editor = live2dstruct::CParameterReferenceEditor;

class ParameterReferenceEditorTests : public QObject
{
    Q_OBJECT
    static QByteArray bytes(const QJsonObject& value) { return QJsonDocument(value).toJson(QJsonDocument::Compact); }
    static QJsonObject curve(const QString& target, const QString& id)
    {
        return {{"Target", target}, {"Id", id}, {"Segments", QJsonArray{0, 0, 0, 1, 1}},
                {"FadeInTime", .25}, {"FadeOutTime", .75}, {"Extension", "kept"}};
    }
    static QJsonObject motion()
    {
        return {{"Version", 3}, {"Meta", QJsonObject{{"Duration", 5}, {"Fps", 30}, {"Loop", true},
            {"CurveCount", 99}, {"TotalSegmentCount", 99}, {"TotalPointCount", 99}, {"Extension", "kept"}}},
            {"FadeInTime", .3}, {"Sound", "track.wav"}, {"Extension", QJsonArray{"kept"}},
            {"Curves", QJsonArray{curve("Parameter", "A"), curve("PartOpacity", "A"), curve("Model", "EyeBlink"), curve("Parameter", "AB")}},
            {"UserData", QJsonArray{QJsonObject{{"Time", 2}, {"Value", QStringLiteral("雪😀")}}}}};
    }
    static QJsonObject physicsGroup(const QString& first, const QString& second)
    {
        const auto input = [](const QString& id) { return QJsonObject{{"Source", QJsonObject{{"Target", "Parameter"}, {"Id", id}}}, {"Weight", 37}}; };
        const auto output = [](const QString& id) { return QJsonObject{{"Destination", QJsonObject{{"Target", "Parameter"}, {"Id", id}}}, {"Weight", 63}}; };
        return {{"Id", "Hair"}, {"Input", QJsonArray{input(first), input(second)}}, {"Output", QJsonArray{output(second), output(first)}},
            {"Vertices", QJsonArray{QJsonObject{{"Position", QJsonObject{{"X", 0}, {"Y", 1}}}}}},
            {"Normalization", QJsonObject{{"Extension", "kept"}}}};
    }

private slots:
    void motionKeepsOtherNamespacesAndRawCurveData()
    {
        const auto source = motion();
        Editor::Prepared result;
        QString error;
        QVERIFY2(Editor::Prepare(Editor::Kind::Motion, bytes(source), {"A"}, result, &error), qPrintable(error));
        const auto curves = source["Curves"].toArray();
        QCOMPARE(result.effective["Curves"].toArray(), QJsonArray({curves[1], curves[2], curves[3]}));
        QCOMPARE(result.parameterRows, (QMap<int, QString>{{0, "A"}}));
        auto expected = source;
        expected["Curves"] = result.effective["Curves"];
        auto meta = source["Meta"].toObject();
        meta["CurveCount"] = 3;
        meta["TotalSegmentCount"] = 3;
        meta["TotalPointCount"] = 6;
        meta["UserDataCount"] = 1;
        meta["TotalUserDataSize"] = QStringLiteral("雪😀").toUtf8().size();
        expected["Meta"] = meta;
        QCOMPARE(result.effective, expected);
        QCOMPARE(result.source, source);
        QCOMPARE(result.sourceDigest, QCryptographicHash::hash(bytes(source), QCryptographicHash::Sha256));
    }

    void deletingLastMotionCurvePreservesDuration()
    {
        auto source = motion();
        source.remove("UserData");
        source["Curves"] = QJsonArray{curve("Parameter", "A")};
        Editor::Prepared result;
        QString error;
        QVERIFY2(Editor::Prepare(Editor::Kind::Motion, bytes(source), {"A"}, result, &error), qPrintable(error));
        QVERIFY(result.effective["Curves"].toArray().isEmpty());
        QCOMPARE(result.effective["Meta"].toObject()["Duration"].toDouble(), 5.0);
        QCOMPARE(result.effective["Meta"].toObject()["TotalPointCount"].toInt(), 0);
        Editor::Prepared again;
        QVERIFY(Editor::Prepare(Editor::Kind::Motion, result.effectiveBytes, {"A"}, again, &error));
        QCOMPARE(again.effectiveBytes, result.effectiveBytes);
    }

    void recountHandlesBezierStepAndInverseStepWithoutReserializingCurves()
    {
        auto source = motion();
        auto mixed = curve("Parameter", "B");
        mixed["Segments"] = QJsonArray{0, 0, 1, .2, .1, .8, .9, 1, 1, 2, 2, .5, 3, 3, 0};
        source["Curves"] = QJsonArray{curve("Parameter", "A"), mixed};
        Editor::Prepared result;
        QString error;
        QVERIFY2(Editor::Prepare(Editor::Kind::Motion, bytes(source), {"A"}, result, &error), qPrintable(error));
        QCOMPARE(result.effective["Curves"].toArray(), QJsonArray{mixed});
        const auto meta = result.effective["Meta"].toObject();
        QCOMPARE(meta["TotalSegmentCount"].toInt(), 3);
        QCOMPARE(meta["TotalPointCount"].toInt(), 6);
    }

    void timelineRetainsPlayheadAndSinglePointDraft()
    {
        auto draft = motion();
        auto single = curve("Parameter", "A");
        single["Segments"] = QJsonArray{0, .5};
        draft["Curves"] = QJsonArray{single, curve("PartOpacity", "A")};
        QJsonObject state{{"version", 1}, {"playhead", 2.5}, {"motion", draft}, {"Extension", "kept"}};
        Editor::Prepared result;
        QString error;
        QVERIFY2(Editor::Prepare(Editor::Kind::Timeline, bytes(state), {"A"}, result, &error), qPrintable(error));
        QCOMPARE(result.effective["playhead"], state["playhead"]);
        QCOMPARE(result.effective["Extension"], state["Extension"]);
        CMotionTimeline restored;
        float time = 0;
        QVERIFY2(restored.RestoreState(result.effective, &time, &error), qPrintable(error));
        QCOMPARE(time, 2.5f);
        QCOMPARE(restored.Tracks().size(), 1);
    }

    void deletingDraftsLastCurveKeepsInferredDuration()
    {
        QJsonObject draft{{"Version", 3}, {"Curves", QJsonArray{QJsonObject{{"Target", "Parameter"}, {"Id", "A"},
            {"Segments", QJsonArray{0, 0, 0, 10, 1}}}}}};
        const QJsonObject state{{"version", 1}, {"playhead", 5}, {"motion", draft}};
        Editor::Prepared result;
        QString error;
        QVERIFY2(Editor::Prepare(Editor::Kind::Timeline, bytes(state), {"A"}, result, &error), qPrintable(error));
        CMotionTimeline restored;
        float time = 0;
        QVERIFY2(restored.RestoreState(result.effective, &time, &error), qPrintable(error));
        QCOMPARE(time, 5.0f);
        QCOMPARE(restored.Timing().duration, 10.0);
        QVERIFY(restored.Tracks().isEmpty());
    }

    void physicsUsesOriginalFlattenedIndicesAndPreservesWeights()
    {
        const auto a = physicsGroup("A", "B"), b = physicsGroup("B", "A");
        const QJsonObject source{{"PhysicsSettings", QJsonArray{a, b}}, {"Meta", QJsonObject{{"PhysicsSettingCount", 2}, {"VertexCount", 2}}}};
        Editor::Prepared result;
        QVERIFY(Editor::Prepare(Editor::Kind::Physics, bytes(source), {"A"}, result));
        QCOMPARE(result.inputRows, (QMap<int, QString>{{0, "A"}, {3, "A"}}));
        QCOMPARE(result.outputRows, (QMap<int, QString>{{1, "A"}, {2, "A"}}));
        const auto groups = result.effective["PhysicsSettings"].toArray();
        for (int i = 0; i < groups.size(); ++i) {
            const auto original = source["PhysicsSettings"].toArray()[i].toObject();
            const auto group = groups[i].toObject();
            QCOMPARE(group["Vertices"], original["Vertices"]);
            QCOMPARE(group["Normalization"], original["Normalization"]);
            QCOMPARE(group["Input"].toArray()[0].toObject()["Weight"].toInt(), 37);
            QCOMPARE(group["Output"].toArray()[0].toObject()["Weight"].toInt(), 63);
        }
        QCOMPARE(result.effective["Meta"].toObject()["TotalInputCount"].toInt(), 2);
        QCOMPARE(result.effective["Meta"].toObject()["TotalOutputCount"].toInt(), 2);
    }

    void groupsAndExpressionsKeepUnrelatedFields()
    {
        const QJsonObject parameter{{"Name", "EyeBlink"}, {"Target", "Parameter"}, {"Ids", QJsonArray{"A", "B"}}};
        const QJsonObject part{{"Name", "Parts"}, {"Target", "Part"}, {"Ids", QJsonArray{"A"}}};
        const QJsonObject source{{"FileReferences", QJsonObject{}}, {"Groups", QJsonArray{parameter, part}}, {"HitAreas", QJsonArray{"A"}}};
        Editor::Prepared result;
        QVERIFY(Editor::Prepare(Editor::Kind::ModelSettings, bytes(source), {"A"}, result));
        QCOMPARE(result.eyeBlink, QSet<QString>{"A"});
        QCOMPARE(result.effective["Groups"].toArray()[1].toObject(), part);
        QCOMPARE(result.effective["HitAreas"], source["HitAreas"]);
        const QJsonObject kept{{"Id", "B"}, {"Value", .7}, {"Blend", "Multiply"}, {"Extension", "kept"}};
        const QJsonObject expression{{"Parameters", QJsonArray{QJsonObject{{"Id", "A"}, {"Value", 1}}, kept,
            QJsonObject{{"Id", "A"}, {"Value", .5}}}}, {"FadeInTime", 2}};
        QVERIFY(Editor::Prepare(Editor::Kind::Expression, bytes(expression), {"A"}, result));
        QCOMPARE(result.parameterRows, (QMap<int, QString>{{0, "A"}, {2, "A"}}));
        QCOMPARE(result.effective["Parameters"].toArray(), QJsonArray{kept});
        QCOMPARE(result.effective["FadeInTime"], expression["FadeInTime"]);
    }

    void noOpPreservesExactSourceBytes()
    {
        const QByteArray source = "{ \"Parameters\" : [ {\"Id\":\"A\",\"Value\":1} ] }\n";
        Editor::Prepared result;
        QVERIFY(Editor::Prepare(Editor::Kind::Expression, source, {"AB"}, result));
        QCOMPARE(result.effectiveBytes, source);
    }

    void sdkGroupsFollowNameAndOnlyFirstGroupDrivesRuntime()
    {
        const QJsonObject custom{{"Name", "CustomGroup"}, {"Target", QJsonObject{{"vendor", true}}}, {"Ids", QJsonArray{"", "Other"}}};
        const QJsonObject part{{"Name", "Parts"}, {"Target", "Part"}, {"Ids", QJsonArray{"A"}}};
        const QJsonObject source{{"FileReferences", QJsonObject{}}, {"Groups", QJsonArray{
            QJsonObject{{"Name", "EyeBlink"}, {"Ids", QJsonArray{"A"}}},
            QJsonObject{{"Name", "EyeBlink"}, {"Target", "Part"}, {"Ids", QJsonArray{"B"}}},
            QJsonObject{{"Name", "LipSync"}, {"Target", "Part"}, {"Ids", QJsonArray{"B"}}}, custom, part}}};
        Editor::Prepared result;
        QString error;
        QVERIFY2(Editor::Prepare(Editor::Kind::ModelSettings, bytes(source), {"A", "B"}, result, &error), qPrintable(error));
        QCOMPARE(result.eyeBlink, QSet<QString>{"A"});
        QCOMPARE(result.lipSync, QSet<QString>{"B"});
        QCOMPARE(result.references.size(), 3);
        const auto groups = result.effective["Groups"].toArray();
        QCOMPARE(groups[3].toObject(), custom);
        QCOMPARE(groups[4].toObject(), part);
        for (int i = 0; i < 3; ++i) QVERIFY(groups[i].toObject()["Ids"].toArray().isEmpty());
        const auto previous = result.effectiveBytes;
        QVERIFY(!Editor::Prepare(Editor::Kind::ModelSettings, bytes(source), {"Other"}, result, &error));
        QCOMPARE(result.effectiveBytes, previous);
    }

    void invalidSourcesNeverPublishPartialOutput()
    {
        const QVector<QPair<Editor::Kind, QByteArray>> invalid{
            {Editor::Kind::Motion, "{}"}, {Editor::Kind::Expression, "[]"},
            {Editor::Kind::Expression, "{\"Parameters\":[{\"Id\":\"A\",\"Value\":1e100}]}"},
            {Editor::Kind::Physics, "{\"PhysicsSettings\":[{\"Input\":[],\"Output\":[{}]}]}"},
            {Editor::Kind::ModelSettings, "{\"FileReferences\":{},\"Groups\":[{\"Name\":\"Unknown\",\"Target\":\"Extension\",\"Ids\":[\"A\"]}]}"},
            {static_cast<Editor::Kind>(99), "{}"}, {Editor::Kind::Timeline, "{\"version\":2}"}};
        for (const auto& source : invalid) {
            Editor::Prepared result;
            result.effectiveBytes = "unchanged";
            QString error;
            QVERIFY(!Editor::Prepare(source.first, source.second, {"A"}, result, &error));
            QVERIFY(!error.isEmpty());
            QCOMPARE(result.effectiveBytes, QByteArray("unchanged"));
        }
        auto malformed = motion();
        malformed["Curves"] = QJsonArray{curve("Parameter", "A"), QJsonObject{{"Target", "Parameter"}, {"Id", "B"}}};
        const auto before = malformed;
        QString error;
        QVERIFY(!CMotionTimeline::RecountMetadata(malformed, &error));
        QCOMPARE(malformed, before);
    }

    void suppliedSourcesValidateAndKeepExactBytes()
    {
        QDirIterator files(QStringLiteral(TEST_RESOURCES), {"*.model3.json", "*.motion3.json", "*.exp3.json", "*.physics3.json"}, QDir::Files, QDirIterator::Subdirectories);
        int count = 0;
        while (files.hasNext()) {
            const auto path = files.next();
            const auto kind = path.endsWith(".model3.json") ? Editor::Kind::ModelSettings : path.endsWith(".motion3.json") ? Editor::Kind::Motion
                : path.endsWith(".exp3.json") ? Editor::Kind::Expression : Editor::Kind::Physics;
            QFile file(path);
            QVERIFY(file.open(QIODevice::ReadOnly));
            const auto source = file.readAll();
            Editor::Prepared result;
            QString error;
            QVERIFY2(Editor::Prepare(kind, source, {"NoSuchParameterForReferenceTest"}, result, &error), qPrintable(path + ": " + error));
            QCOMPARE(result.effectiveBytes, source);
            ++count;
        }
        QVERIFY(count > 20);
    }
};

QTEST_APPLESS_MAIN(ParameterReferenceEditorTests)
#include "parameter_reference_editor_tests.moc"
