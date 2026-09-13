#include "ui/cphysicspanel.h"
#include "ui/cphysicsgraph.h"
#include <QtTest>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QFile>
#include <QTemporaryDir>
#include <QSignalSpy>
#include <QPushButton>

class PhysicsPanelTests : public QObject
{
    Q_OBJECT
private slots:
    void rejectsInvalidJson()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString path = temp.filePath(QStringLiteral("broken.physics3.json"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QVERIFY(file.write("{broken") > 0);
        file.close();

        CPhysicsPanel panel;
        QString error;
        QVERIFY(!panel.LoadPhysics(path, &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(!panel.HasUnsavedChanges());
        auto* groups = panel.findChild<QTableWidget*>(QStringLiteral("physicsGroupTable"));
        QVERIFY(groups);
        QVERIFY(panel.findChild<CPhysicsGraphWidget*>(QStringLiteral("physicsGraph")));
        QCOMPARE(groups->rowCount(), 0);
    }

    void editsAndSavesPhysics()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString path = temp.filePath(QStringLiteral("model.physics3.json"));
        const QByteArray source = R"({
            "Meta": {"Fps": 30, "EffectiveForces": {"Gravity": {"X": 0, "Y": -1}, "Wind": {"X": 0, "Y": 0}},
                     "PhysicsDictionary": [{"Id": "ParamAngleX", "Name": "角度"}]},
            "PhysicsSettings": [{"Id": "hair", "Input": [{"Source": {"Id": "ParamAngleX"}, "Type": "X", "Weight": 0.5, "Reflect": false}],
                                  "Output": [{"Destination": {"Id": "ParamAngleY"}, "VertexIndex": 1, "Type": "Y", "Weight": 0.7, "Scale": 1.2, "Reflect": false}],
                                  "Vertices": [{"Position": {"X": 0, "Y": 0}}, {"Position": {"X": 1, "Y": 0}, "Radius": 1, "Mobility": 0.5, "Delay": 0.2, "Acceleration": 0.3}]}]
        })";
        QJsonObject sourceRoot = QJsonDocument::fromJson(source).object();
        QJsonArray settings = sourceRoot.value(QStringLiteral("PhysicsSettings")).toArray();
        QJsonObject second = settings.first().toObject();
        second.insert(QStringLiteral("Id"), QStringLiteral("tail"));
        QJsonArray secondOutputs = second.value(QStringLiteral("Output")).toArray();
        QJsonObject secondOutput = secondOutputs.first().toObject();
        secondOutput.insert(QStringLiteral("Scale"), 3.4);
        secondOutputs[0] = secondOutput;
        second.insert(QStringLiteral("Output"), secondOutputs);
        settings.append(second);
        sourceRoot.insert(QStringLiteral("PhysicsSettings"), settings);
        const QByteArray sourceWithSecondGroup = QJsonDocument(sourceRoot).toJson(QJsonDocument::Compact);
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QVERIFY(file.write(sourceWithSecondGroup) == sourceWithSecondGroup.size());
        file.close();

        CPhysicsPanel panel;
        panel.LoadPhysics(path);
        QVERIFY(!panel.HasUnsavedChanges());
        auto* input = panel.findChild<QTableWidget*>(QStringLiteral("physicsInputTable"));
        auto* output = panel.findChild<QTableWidget*>(QStringLiteral("physicsOutputTable"));
        auto* vertex = panel.findChild<QTableWidget*>(QStringLiteral("physicsVertexTable"));
        auto* groups = panel.findChild<QTableWidget*>(QStringLiteral("physicsGroupTable"));
        QVERIFY(input && output && vertex && groups);
        auto* graph = panel.findChild<CPhysicsGraphWidget*>(QStringLiteral("physicsGraph"));
        QVERIFY(graph);
        QCOMPARE(groups->rowCount(), 2);
        QCOMPARE(input->rowCount(), 1);
        QCOMPARE(output->rowCount(), 1);
        QCOMPARE(vertex->rowCount(), 1);
        QCOMPARE(graph->InputCount(), 1);
        QCOMPARE(graph->OutputCount(), 1);
        QCOMPARE(graph->VertexCount(), 2);
        auto* addVertex = panel.findChild<QPushButton*>(QStringLiteral("addPhysicsVertex"));
        auto* removeVertex = panel.findChild<QPushButton*>(QStringLiteral("removePhysicsVertex"));
        QVERIFY(addVertex && removeVertex && addVertex->isEnabled());
        addVertex->click();
        QCOMPARE(vertex->rowCount(), 2);
        vertex->selectRow(1);
        QVERIFY(removeVertex->isEnabled());
        removeVertex->click();
        QCOMPARE(vertex->rowCount(), 1);
        QSignalSpy configChanges(&panel, &CPhysicsPanel::PhysicsConfigChanged);
        panel.LoadPhysics(path);
        QCOMPARE(configChanges.count(), 0);

        groups->selectRow(1);
        output->item(0, 3)->setText(QStringLiteral("bad"));
        QCOMPARE(output->item(0, 3)->text(), QStringLiteral("3.40"));
        groups->selectRow(0);

        input->item(0, 2)->setText(QStringLiteral("0.8"));
        QCOMPARE(input->item(0, 2)->text(), QStringLiteral("0.8"));
        QVERIFY(panel.HasUnsavedChanges());
        input->item(0, 2)->setText(QStringLiteral("101.0"));
        QVERIFY(input->item(0, 2)->text() != QStringLiteral("101.0"));
        output->item(0, 3)->setText(QStringLiteral("2.5"));
        vertex->item(0, 2)->setText(QStringLiteral("0.75"));
        QVERIFY(panel.SavePhysics());
        QVERIFY(!panel.HasUnsavedChanges());

        QFile saved(path);
        QVERIFY(saved.open(QIODevice::ReadOnly));
        const QJsonObject root = QJsonDocument::fromJson(saved.readAll()).object();
        const QJsonObject setting = root.value(QStringLiteral("PhysicsSettings")).toArray().first().toObject();
        const QJsonObject meta = root.value(QStringLiteral("Meta")).toObject();
        QCOMPARE(meta.value(QStringLiteral("PhysicsSettingCount")).toInt(), 2);
        QCOMPARE(meta.value(QStringLiteral("TotalInputCount")).toInt(), 2);
        QCOMPARE(meta.value(QStringLiteral("TotalOutputCount")).toInt(), 2);
        QCOMPARE(meta.value(QStringLiteral("VertexCount")).toInt(), 4);
        QCOMPARE(setting.value(QStringLiteral("Input")).toArray().first().toObject().value(QStringLiteral("Weight")).toDouble(), 0.8);
        QCOMPARE(setting.value(QStringLiteral("Output")).toArray().first().toObject().value(QStringLiteral("Scale")).toDouble(), 2.5);
        QCOMPARE(setting.value(QStringLiteral("Vertices")).toArray().at(1).toObject().value(QStringLiteral("Mobility")).toDouble(), 0.75);

        const QString brokenPath = temp.filePath(QStringLiteral("broken.physics3.json"));
        QFile broken(brokenPath);
        QVERIFY(broken.open(QIODevice::WriteOnly));
        QVERIFY(broken.write("{broken") > 0);
        broken.close();
        QString error;
        QVERIFY(!panel.LoadPhysics(brokenPath, &error));
        QCOMPARE(groups->rowCount(), 2);
        QVERIFY(!panel.HasUnsavedChanges());

        const QString malformedPath = temp.filePath(QStringLiteral("malformed.physics3.json"));
        QFile malformed(malformedPath);
        QVERIFY(malformed.open(QIODevice::WriteOnly));
        QVERIFY(malformed.write(R"({"PhysicsSettings":[{"Input":"bad"}]})") > 0);
        malformed.close();
        QVERIFY(!panel.LoadPhysics(malformedPath, &error));
        QVERIFY(error.contains(QStringLiteral("Input 无效")));
        QCOMPARE(groups->rowCount(), 2);
        auto* removeGroup = panel.findChild<QPushButton*>(QStringLiteral("removePhysicsGroup"));
        QVERIFY(removeGroup);
        groups->selectRow(1);
        removeGroup->click();
        QCOMPARE(groups->rowCount(), 1);
        QVERIFY(panel.HasUnsavedChanges());
    }

    void loadsCubismPercentageWeights()
    {
        CPhysicsPanel panel;
        QString error;
        const QString path = QString(TEST_RESOURCES) + QStringLiteral("/Mark/Mark.physics3.json");
        QVERIFY2(panel.LoadPhysics(path, &error), qPrintable(error));
        auto* groups = panel.findChild<QTableWidget*>(QStringLiteral("physicsGroupTable"));
        auto* input = panel.findChild<QTableWidget*>(QStringLiteral("physicsInputTable"));
        auto* output = panel.findChild<QTableWidget*>(QStringLiteral("physicsOutputTable"));
        QVERIFY(groups && input && output);
        QVERIFY(groups->rowCount() > 0);
        QVERIFY(input->rowCount() > 0);
        QVERIFY(output->rowCount() > 0);
        QVERIFY(input->item(0, 2)->text().toDouble() > 1.0);
        QVERIFY(output->item(0, 2)->text().toDouble() > 1.0);
        QVERIFY(!panel.HasUnsavedChanges());
    }

    void loadsAllBuiltinPhysicsFiles()
    {
        for (const QString& model : {QStringLiteral("Mark"), QStringLiteral("Natori"),
                                     QStringLiteral("Rice"), QStringLiteral("Wanko")}) {
            CPhysicsPanel panel;
            QString error;
            const QString path = QString(TEST_RESOURCES) + QLatin1Char('/') + model + QLatin1Char('/') + model + QStringLiteral(".physics3.json");
            QVERIFY2(panel.LoadPhysics(path, &error), qPrintable(model + QStringLiteral(": ") + error));
            QVERIFY(panel.findChild<QTableWidget*>(QStringLiteral("physicsGroupTable"))->rowCount() > 0);
        }
    }

    void rejectsInvalidPhysicsReferences()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString path = temp.filePath(QStringLiteral("invalid.physics3.json"));
        const QByteArray source = R"({
            "PhysicsSettings": [{
                "Id": "group",
                "Input": [{"Source": {"Id": ""}, "Type": "Bad", "Weight": 50, "Reflect": false}],
                "Output": [{"Destination": {"Id": "Param"}, "VertexIndex": 2, "Scale": 1, "Weight": 100, "Type": "X", "Reflect": false}],
                "Vertices": [{"Position": {"X": 0, "Y": 0}, "Mobility": 1, "Delay": 1, "Acceleration": 1, "Radius": 0}]
            }]
        })";
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QVERIFY(file.write(source) == source.size());
        file.close();
        CPhysicsPanel panel;
        QString error;
        QVERIFY(!panel.LoadPhysics(path, &error));
        QVERIFY(error.contains(QStringLiteral("输入权重无效")) || error.contains(QStringLiteral("输出数值无效")));
    }
};

QTEST_MAIN(PhysicsPanelTests)
#include "physics_panel_tests.moc"
