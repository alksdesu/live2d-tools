#include "live2dstruct/cparameterreferences.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

using live2dstruct::CParameterReferences;

class ParameterReferencesTests : public QObject
{
    Q_OBJECT
    static bool write(const QString& path, const QJsonObject& object)
    {
        QFile file(path);
        const auto bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
        return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
    }
    static QJsonObject physics(const QString& id)
    {
        return {{"PhysicsSettings", QJsonArray{QJsonObject{{"Id", "Hair"},
            {"Input", QJsonArray{QJsonObject{{"Source", QJsonObject{{"Target", "Parameter"}, {"Id", id}}}}}},
            {"Output", QJsonArray{QJsonObject{{"Destination", QJsonObject{{"Target", "Parameter"}, {"Id", id}}}}}}}}}};
    }

private slots:
    void emptyPhysicsHasNoReferences()
    {
        QStringList references{QStringLiteral("previous")};
        QVERIFY(CParameterReferences::ScanPhysics({{QStringLiteral("PhysicsSettings"), QJsonArray{}}},
                                                  QStringLiteral("ParamA"), references));
        QVERIFY(references.isEmpty());
    }

    void physicsReportsInputsAndOutputsExactly()
    {
        QStringList references;
        QString error;
        QVERIFY2(CParameterReferences::ScanPhysics(physics(QStringLiteral("参数雪")), QStringLiteral("参数雪"), references, &error), qPrintable(error));
        QCOMPARE(references.size(), 2);
        QVERIFY(references.join('\n').contains(QStringLiteral("Hair")));
        QVERIFY(references.join('\n').contains(QStringLiteral("输入")));
        QVERIFY(references.join('\n').contains(QStringLiteral("输出")));
        QVERIFY(CParameterReferences::ScanPhysics(physics(QStringLiteral("ParamAB")), QStringLiteral("ParamA"), references));
        QVERIFY(references.isEmpty());
    }

    void invalidPhysicsPreservesOutput_data()
    {
        QTest::addColumn<int>("kind");
        for (int i = 0; i < 8; ++i) QTest::newRow(qPrintable(QString::number(i))) << i;
    }

    void invalidPhysicsPreservesOutput()
    {
        QFETCH(int, kind);
        auto root = physics(QStringLiteral("ParamA"));
        auto groups = root["PhysicsSettings"].toArray();
        auto group = groups[0].toObject();
        if (kind == 0) root.remove("PhysicsSettings");
        if (kind == 1) root["PhysicsSettings"] = "bad";
        if (kind == 2) { groups[0] = 12; root["PhysicsSettings"] = groups; }
        if (kind >= 3) {
            if (kind == 3) group.remove("Output");
            if (kind == 4) group["Output"] = QJsonArray{false};
            if (kind >= 5) {
                QJsonObject destination{{"Id", "Other"}, {"Target", "Parameter"}};
                if (kind == 5) destination.remove("Id");
                if (kind == 6) destination["Target"] = "Part";
                if (kind == 7) destination["Id"] = 123;
                group["Output"] = QJsonArray{QJsonObject{{"Destination", destination}}};
            }
            groups[0] = group;
            root["PhysicsSettings"] = groups;
        }
        QStringList references{QStringLiteral("unchanged")};
        QString error;
        QVERIFY(!CParameterReferences::ScanPhysics(root, QStringLiteral("ParamA"), references, &error));
        QCOMPARE(references, QStringList({QStringLiteral("unchanged")}));
        QVERIFY(!error.isEmpty());
    }

    void modelReportsEveryParameterSourceWithoutPartFalsePositives()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const auto file = [&](const QString& name) { return temporary.filePath(name); };
        const QString id = QStringLiteral("参数雪");
        QVERIFY(write(file("物理.json"), physics(id)));
        QVERIFY(write(file("表情.json"), {{"Parameters", QJsonArray{QJsonObject{{"Id", id}}}}}));
        QVERIFY(write(file("动作.json"), {{"Curves", QJsonArray{
            QJsonObject{{"Target", "Parameter"}, {"Id", id}},
            QJsonObject{{"Target", "PartOpacity"}, {"Id", id}},
            QJsonObject{{"Target", "Model"}, {"Id", id}}}}}));
        const QJsonObject root{{"FileReferences", QJsonObject{{"Physics", "物理.json"}, {"Pose", "missing.pose3.json"},
            {"Expressions", QJsonArray{QJsonObject{{"Name", "笑顔"}, {"File", "表情.json"}}}},
            {"Motions", QJsonObject{{"Idle", QJsonArray{QJsonObject{{"File", "动作.json"}}}}}}}},
            {"Groups", QJsonArray{QJsonObject{{"Name", "EyeBlink"}, {"Target", "Parameter"}, {"Ids", QJsonArray{id}}},
                                  QJsonObject{{"Name", "Parts"}, {"Target", "Part"}, {"Ids", QJsonArray{id}}}}},
            {"HitAreas", QJsonArray{QJsonObject{{"Name", "Hit"}, {"Id", id}}}}};
        QVERIFY(write(file("模型.model3.json"), root));
        QStringList references;
        QString error;
        QVERIFY2(CParameterReferences::ScanModel(file("模型.model3.json"), id, references, &error), qPrintable(error));
        QCOMPARE(references.size(), 5);
        const QString text = references.join('\n');
        QVERIFY(text.contains(QStringLiteral("EyeBlink")));
        QVERIFY(text.contains(QStringLiteral("Hair")));
        QVERIFY(text.contains(QStringLiteral("笑顔")));
        QVERIFY(text.contains(QStringLiteral("Idle")));
        QVERIFY(text.contains(QStringLiteral("动作.json")));
        QVERIFY(!text.contains(QStringLiteral("Parts")));
        QVERIFY(!text.contains(QStringLiteral("missing.pose3.json")));
        QVERIFY(CParameterReferences::ScanModel(file("模型.model3.json"), QStringLiteral("Absent"), references));
        QVERIFY(references.isEmpty());
    }

    void malformedAndMissingFilesNeverPublishPartialMatches_data()
    {
        QTest::addColumn<int>("kind");
        for (int i = 0; i < 10; ++i) QTest::newRow(qPrintable(QString::number(i))) << i;
    }

    void malformedAndMissingFilesNeverPublishPartialMatches()
    {
        QFETCH(int, kind);
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        QJsonObject root{{"FileReferences", QJsonObject{}}, {"Groups", QJsonArray{
            QJsonObject{{"Name", "EyeBlink"}, {"Target", "Parameter"}, {"Ids", QJsonArray{"ParamA"}}}}}};
        QJsonObject files;
        if (kind == 0) files["Physics"] = "missing.json";
        if (kind == 1) files["Expressions"] = "bad";
        if (kind == 2) files["Motions"] = QJsonArray{};
        if (kind == 3) files["Expressions"] = QJsonArray{QJsonObject{{"File", 1}}};
        if (kind >= 4) {
            files["Motions"] = QJsonObject{{"Idle", QJsonArray{QJsonObject{{"File", "motion.json"}}}}};
            if (kind == 4) {
                QFile file(temporary.filePath("motion.json"));
                QVERIFY(file.open(QIODevice::WriteOnly));
                file.write("{broken");
            } else {
                QJsonObject motion{{"Curves", QJsonArray{QJsonObject{{"Id", "Other"}, {"Target", "Parameter"}}}}};
                if (kind == 5) motion.remove("Curves");
                if (kind == 6) motion["Curves"] = QJsonArray{QJsonObject{{"Target", "Parameter"}}};
                if (kind == 7) motion["Curves"] = QJsonArray{QJsonObject{{"Id", "Other"}, {"Target", "Unknown"}}};
                if (kind == 8) motion["Curves"] = false;
                if (kind == 9) motion["Curves"] = QJsonArray{false};
                QVERIFY(write(temporary.filePath("motion.json"), motion));
            }
        }
        root["FileReferences"] = files;
        QVERIFY(write(temporary.filePath("model.json"), root));
        QStringList references{QStringLiteral("previous")};
        QString error;
        QVERIFY(!CParameterReferences::ScanModel(temporary.filePath("model.json"), "ParamA", references, &error));
        QCOMPARE(references, QStringList({QStringLiteral("previous")}));
        QVERIFY(!error.isEmpty());
    }

    void budgetsRejectOversizedAndExcessiveFiles()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        QFile large(temporary.filePath("large.json"));
        QVERIFY(large.open(QIODevice::WriteOnly));
        QVERIFY(large.resize(16 * 1024 * 1024 + 1));
        large.close();
        QVERIFY(write(temporary.filePath("model.json"), {{"FileReferences", QJsonObject{{"Physics", "large.json"}}}}));
        QStringList references{QStringLiteral("previous")};
        QString error;
        QVERIFY(!CParameterReferences::ScanModel(temporary.filePath("model.json"), "ParamA", references, &error));
        QVERIFY(error.contains(QStringLiteral("限制")));
        QVERIFY(write(temporary.filePath("expression.json"), {{"Parameters", QJsonArray{}}}));
        QJsonArray expressions;
        for (int i = 0; i < 2049; ++i) expressions.append(QJsonObject{{"File", "expression.json"}});
        QVERIFY(write(temporary.filePath("model.json"), {{"FileReferences", QJsonObject{{"Expressions", expressions}}}}));
        QVERIFY(!CParameterReferences::ScanModel(temporary.filePath("model.json"), "ParamA", references, &error));
        QCOMPARE(references, QStringList({QStringLiteral("previous")}));
        QVERIFY(error.contains(QStringLiteral("限制")));
    }

    void sdkParameterGroupsCannotHideBehindAnIncorrectTarget()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        for (const auto& name : {QStringLiteral("EyeBlink"), QStringLiteral("LipSync")}) {
            QVERIFY(write(temporary.filePath("model.json"), {{"FileReferences", QJsonObject{}},
                {"Groups", QJsonArray{QJsonObject{{"Name", name}, {"Target", "Part"}, {"Ids", QJsonArray{"ParamA"}}}}}}));
            QStringList references{QStringLiteral("previous")};
            QString error;
            QVERIFY(CParameterReferences::ScanModel(temporary.filePath("model.json"), "ParamA", references, &error));
            QCOMPARE(references.size(), 1);
            QVERIFY(references.first().contains(name));
        }
    }

    void suppliedModelsCanBeScannedCompletely_data()
    {
        QTest::addColumn<QString>("name");
        for (const auto& name : {"Mark", "Natori", "Rice", "Wanko"}) QTest::newRow(name) << QString::fromLatin1(name);
    }

    void suppliedModelsCanBeScannedCompletely()
    {
        QFETCH(QString, name);
        const QString model = QStringLiteral(TEST_RESOURCES) + "/" + name + "/" + name + ".model3.json";
        QStringList references;
        QString error;
        QVERIFY2(CParameterReferences::ScanModel(model, QStringLiteral("DefinitelyNotAParameter"), references, &error), qPrintable(error));
        QVERIFY(references.isEmpty());
        const QString id = name == QStringLiteral("Wanko") ? QStringLiteral("PARAM_ANGLE_X") : QStringLiteral("ParamAngleX");
        QVERIFY2(CParameterReferences::ScanModel(model, id, references, &error), qPrintable(error));
        QVERIFY(!references.isEmpty());
    }
};

QTEST_GUILESS_MAIN(ParameterReferencesTests)
#include "parameter_references_tests.moc"
