#include "live2dstruct/cphysicsdocument.h"
#include <QtTest>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>

using live2dstruct::CPhysicsDocument;
namespace {
QJsonObject source()
{
    QFile file(QString(TEST_RESOURCES) + "/Mark/Mark.physics3.json");
    if (!file.open(QIODevice::ReadOnly)) return {};
    return QJsonDocument::fromJson(file.readAll()).object();
}
}
class PhysicsDocumentTests : public QObject
{
    Q_OBJECT
private slots:
    void normalizesCountsAndLegacyDictionaryWithoutMutatingInput()
    {
        auto document = source(); QVERIFY(!document.isEmpty());
        auto meta = document["Meta"].toObject(); const auto dictionary = meta.take("PhysicsDictionary");
        for (const auto& field : {"PhysicsSettingCount", "TotalInputCount", "TotalOutputCount", "VertexCount"}) meta[field] = -23;
        meta["Custom"] = "preserved"; document["Meta"] = meta; document["PhysicsDictionary"] = dictionary;
        const auto before = document;
        QJsonObject normalized; QString error;
        QVERIFY2(CPhysicsDocument::Normalize(document, normalized, &error), qPrintable(error));
        QCOMPARE(document, before); QVERIFY(!normalized.contains("PhysicsDictionary"));
        const auto result = normalized["Meta"].toObject(); QCOMPARE(result["PhysicsDictionary"], dictionary); QCOMPARE(result["Custom"].toString(), "preserved");
        int inputs = 0, outputs = 0, vertices = 0;
        for (const auto& row : document["PhysicsSettings"].toArray()) {
            inputs += row.toObject()["Input"].toArray().size(); outputs += row.toObject()["Output"].toArray().size(); vertices += row.toObject()["Vertices"].toArray().size();
        }
        QCOMPARE(result["PhysicsSettingCount"].toInt(), document["PhysicsSettings"].toArray().size());
        QCOMPARE(result["TotalInputCount"].toInt(), inputs); QCOMPARE(result["TotalOutputCount"].toInt(), outputs); QCOMPARE(result["VertexCount"].toInt(), vertices);
        QJsonObject second; QVERIFY(CPhysicsDocument::Normalize(normalized, second)); QCOMPARE(second, normalized);
    }
    void invalidValuesAreTransactional_data()
    {
        QTest::addColumn<int>("index");
        for (int i = 0; i < 13; ++i) QTest::newRow(qPrintable(QString::number(i))) << i;
    }
    void invalidValuesAreTransactional()
    {
        QFETCH(int, index); auto document = source(); auto groups = document["PhysicsSettings"].toArray(); auto group = groups.first().toObject();
        auto input = group["Input"].toArray().first().toObject(); auto output = group["Output"].toArray().first().toObject();
        switch (index) {
        case 0: document["Meta"] = "bad"; break;
        case 1: { auto meta = document["Meta"].toObject(); meta["EffectiveForces"] = false; document["Meta"] = meta; break; }
        case 2: group["Normalization"] = QJsonObject{{"Position", QJsonObject{{"Minimum", 4}, {"Maximum", 2}}}}; break;
        case 3: input["Reflect"] = 1; break;
        case 4: input["Source"] = QJsonObject{{"Id", QString("bad") + QChar::Null}, {"Target", "Parameter"}}; break;
        case 5: input["Source"] = QJsonObject{{"Id", "ParamAngleX"}, {"Target", "PartOpacity"}}; break;
        case 6: input["Weight"] = 101; break;
        case 7: output["VertexIndex"] = 0.5; break;
        case 8: output["VertexIndex"] = 999999; break;
        case 9: output["Scale"] = "bad"; break;
        case 10: group["Vertices"] = QJsonArray{}; break;
        case 11: { auto vertices = group["Vertices"].toArray(); auto vertex = vertices.first().toObject(); vertex["Position"] = QJsonObject{{"X", "bad"}}; vertices[0] = vertex; group["Vertices"] = vertices; break; }
        case 12: group["Input"] = false; break;
        }
        if (index >= 3 && index <= 6) group["Input"] = QJsonArray{input};
        if (index >= 7 && index <= 9) group["Output"] = QJsonArray{output};
        groups[0] = group; document["PhysicsSettings"] = groups;
        QJsonObject result{{"preserve", true}}; const auto before = result; QString error;
        QVERIFY(!CPhysicsDocument::Normalize(document, result, &error)); QVERIFY(!error.isEmpty()); QCOMPARE(result, before);
    }
    void originalModels_data()
    {
        QTest::addColumn<QString>("name");
        for (const auto& name : {"Mark", "Natori", "Rice", "Wanko"}) QTest::newRow(name) << QString::fromLatin1(name);
    }
    void originalModels()
    {
        QFETCH(QString, name); QFile file(QString(TEST_RESOURCES) + "/" + name + "/" + name + ".physics3.json");
        QVERIFY(file.open(QIODevice::ReadOnly)); const auto document = QJsonDocument::fromJson(file.readAll()).object();
        QJsonObject normalized; QString error; QVERIFY2(CPhysicsDocument::Normalize(document, normalized, &error), qPrintable(error)); QCOMPARE(normalized, document);
    }
};
QTEST_GUILESS_MAIN(PhysicsDocumentTests)
#include "physics_document_tests.moc"
