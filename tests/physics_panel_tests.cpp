#include "ui/cphysicspanel.h"
#include <QtTest>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QFile>
#include <QTemporaryDir>

class PhysicsPanelTests : public QObject
{
    Q_OBJECT
private slots:
    void editsAndSavesPhysics()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString path = temp.filePath(QStringLiteral("model.physics3.json"));
        const QByteArray source = R"({
            "Meta": {"Fps": 30, "EffectiveForces": {"Gravity": {"X": 0, "Y": -1}, "Wind": {"X": 0, "Y": 0}},
                     "PhysicsDictionary": [{"Id": "ParamAngleX", "Name": "角度"}]},
            "PhysicsSettings": [{"Id": "hair", "Input": [{"Source": {"Id": "ParamAngleX"}, "Type": "X", "Weight": 0.5, "Reflect": false}],
                                  "Output": [{"Destination": {"Id": "ParamAngleY"}, "Type": "Y", "Weight": 0.7, "Scale": 1.2, "Reflect": false}],
                                  "Vertices": [{"Position": {"X": 0, "Y": 0}}, {"Position": {"X": 1, "Y": 0}, "Radius": 1, "Mobility": 0.5, "Delay": 0.2, "Acceleration": 0.3}]}]
        })";
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QVERIFY(file.write(source) == source.size());
        file.close();

        CPhysicsPanel panel;
        panel.LoadPhysics(path);
        auto* input = panel.findChild<QTableWidget*>(QStringLiteral("physicsInputTable"));
        auto* output = panel.findChild<QTableWidget*>(QStringLiteral("physicsOutputTable"));
        auto* vertex = panel.findChild<QTableWidget*>(QStringLiteral("physicsVertexTable"));
        QVERIFY(input && output && vertex);
        QCOMPARE(input->rowCount(), 1);
        QCOMPARE(output->rowCount(), 1);
        QCOMPARE(vertex->rowCount(), 1);

        input->item(0, 2)->setText(QStringLiteral("0.8"));
        QCOMPARE(input->item(0, 2)->text(), QStringLiteral("0.8"));
        input->item(0, 2)->setText(QStringLiteral("2.0"));
        QVERIFY(input->item(0, 2)->text() != QStringLiteral("2.0"));
        output->item(0, 3)->setText(QStringLiteral("2.5"));
        vertex->item(0, 2)->setText(QStringLiteral("0.75"));
        QVERIFY(panel.SavePhysics());

        QFile saved(path);
        QVERIFY(saved.open(QIODevice::ReadOnly));
        const QJsonObject root = QJsonDocument::fromJson(saved.readAll()).object();
        const QJsonObject setting = root.value(QStringLiteral("PhysicsSettings")).toArray().first().toObject();
        QCOMPARE(setting.value(QStringLiteral("Input")).toArray().first().toObject().value(QStringLiteral("Weight")).toDouble(), 0.8);
        QCOMPARE(setting.value(QStringLiteral("Output")).toArray().first().toObject().value(QStringLiteral("Scale")).toDouble(), 2.5);
        QCOMPARE(setting.value(QStringLiteral("Vertices")).toArray().at(1).toObject().value(QStringLiteral("Mobility")).toDouble(), 0.75);
    }
};

QTEST_MAIN(PhysicsPanelTests)
#include "physics_panel_tests.moc"
