#include "cmo3/ccmo3builder.h"
#include "cmo3/ccaffarchive.h"
#include "live2dstruct/cphysicsdocument.h"
#include <QtTest>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QXmlStreamReader>
#include <array>

namespace {
struct XmlNode
{
    QString tag, field, text;
    QMap<QString, QString> attributes;
    QVector<XmlNode> children;
    const XmlNode* child(const QString& name) const
    {
        for (const auto& value : children) if (value.field == name) return &value;
        return nullptr;
    }
};

XmlNode readElement(QXmlStreamReader& reader)
{
    XmlNode node;
    node.tag = reader.name().toString();
    for (const auto& attribute : reader.attributes())
        node.attributes.insert(attribute.name().toString(), attribute.value().toString());
    node.field = node.attributes.value("xs.n");
    while (!reader.atEnd()) {
        reader.readNext();
        if (reader.isStartElement()) node.children.append(readElement(reader));
        else if (reader.isCharacters()) node.text += reader.text();
        else if (reader.isEndElement()) break;
    }
    return node;
}

bool readPhysics(const QString& path, XmlNode& result, QString* error)
{
    cmo3::CCaffArchive archive;
    if (!archive.Load(path, error)) return false;
    const auto* main = archive.FindByPath("main.xml");
    if (!main) { *error = "missing main.xml"; return false; }
    QXmlStreamReader reader(main->content);
    int found = 0;
    while (!reader.atEnd()) {
        reader.readNext();
        if (reader.isStartElement() && reader.name() == QLatin1String("CPhysicsSettingsSourceSet")) {
            result = readElement(reader);
            ++found;
        }
    }
    if (reader.hasError()) { *error = reader.errorString(); return false; }
    if (found != 1) { *error = "expected one physics settings object"; return false; }
    return true;
}

bool write(const QString& path, const QByteArray& bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(bytes) == bytes.size();
}

constexpr qint64 MaximumBytes = 16 * 1024 * 1024;
const std::array<const char*, 6> NormalizationFields{
    "normalizedPositionValueMin", "normalizedPositionValueMax", "normalizedPositionDefaultValue",
    "normalizedAngleValueMin", "normalizedAngleValueMax", "normalizedAngleDefaultValue"
};
}

class Cmo3PhysicsTests : public QObject
{
    Q_OBJECT
    QString m_moc;
    QString m_input, m_output;

    QJsonObject group(const QString& id = "Group") const
    {
        return {{"Id", id},
            {"Input", QJsonArray{QJsonObject{{"Source", QJsonObject{{"Target", "Parameter"}, {"Id", m_input}}},
                {"Weight", 100}, {"Type", "Angle"}, {"Reflect", false}}}},
            {"Output", QJsonArray{QJsonObject{{"Destination", QJsonObject{{"Target", "Parameter"}, {"Id", m_output}}},
                {"Weight", 100}, {"Scale", 1}, {"Type", "Angle"}, {"Reflect", false}, {"VertexIndex", 1}}}},
            {"Vertices", QJsonArray{
                QJsonObject{{"Position", QJsonObject{{"X", 0}, {"Y", 0}}}, {"Mobility", 0}, {"Delay", 0}, {"Acceleration", 0}, {"Radius", 0}},
                QJsonObject{{"Position", QJsonObject{{"X", 0}, {"Y", 1}}}, {"Mobility", .5}, {"Delay", .2}, {"Acceleration", .75}, {"Radius", 1}}}}};
    }

    QJsonObject document(const QJsonArray& groups) const
    {
        return {{"Version", 3}, {"PhysicsSettings", groups}};
    }

    bool build(const QByteArray& bytes, bool fileInput, const QString& directory, const QString& output, QString* error,
               bool allowConversion = false) const
    {
        cmo3::CCmo3Builder::Options options;
        options.mocPath = m_moc;
        options.outPath = output;
        options.allowPhysicsConversion = allowConversion;
        if (fileInput) {
            options.physicsPath = directory + "/input.physics3.json";
            if (!write(options.physicsPath, bytes)) { *error = "fixture write failed"; return false; }
        } else options.physicsBytes = bytes;
        return cmo3::CCmo3Builder::Build(options, error);
    }

    void verifyNormalization(const XmlNode& group, const std::array<double, 6>& expected)
    {
        for (size_t i = 0; i < expected.size(); ++i) {
            const QString field = QString::fromLatin1(NormalizationFields[i]);
            int count = 0;
            for (const auto& child : group.children) if (child.field == field) ++count;
            QCOMPARE(count, 1);
            const auto* value = group.child(field); QVERIFY(value); QCOMPARE(value->tag, QString("f"));
            bool ok = false; const double number = value->text.toDouble(&ok); QVERIFY(ok); QCOMPARE(number, expected[i]);
        }
    }

private slots:
    void initTestCase()
    {
        m_moc = QFINDTESTDATA("../Resources/Mark/Mark.moc3"); QVERIFY(!m_moc.isEmpty());
        live2dstruct::CModelDocument model; QString error;
        QVERIFY2(model.Load(m_moc, &error), qPrintable(error)); QVERIFY(model.Parameters().size() >= 2);
        m_input = model.Parameters()[0].id; m_output = model.Parameters()[1].id;
    }

    void normalizationValuesAreDirectFieldsOfTheCorrectGroup_data()
    {
        QTest::addColumn<bool>("fileInput");
        QTest::newRow("bytes") << false; QTest::newRow("file") << true;
    }

    void normalizationValuesAreDirectFieldsOfTheCorrectGroup()
    {
        QFETCH(bool, fileInput);
        auto first = group("First"); auto second = group("Second");
        first["Normalization"] = QJsonObject{
            {"Position", QJsonObject{{"Minimum", -2.5}, {"Maximum", 3.75}, {"Default", .5}}},
            {"Angle", QJsonObject{{"Minimum", -42}, {"Maximum", 27}, {"Default", -3}}}};
        second["Normalization"] = QJsonObject{
            {"Position", QJsonObject{{"Minimum", -8}, {"Maximum", 16}, {"Default", 4}}},
            {"Angle", QJsonObject{{"Minimum", -15.5}, {"Maximum", 61.25}, {"Default", 7.5}}}};
        const auto source = document({first, second}); QVERIFY(live2dstruct::CPhysicsDocument::Validate(source));
        const auto bytes = QJsonDocument(source).toJson();
        QTemporaryDir directory; QVERIFY(directory.isValid()); const auto output = directory.filePath("normalization.cmo3");
        QString error; QVERIFY2(build(bytes, fileInput, directory.path(), output, &error), qPrintable(error));
        XmlNode physics; QVERIFY2(readPhysics(output, physics, &error), qPrintable(error));
        const auto* groups = physics.child("_sourceCubismPhysics"); QVERIFY(groups);
        QCOMPARE(groups->tag, QString("carray_list")); QCOMPARE(groups->attributes.value("count"), QString("2")); QCOMPARE(groups->children.size(), 2);
        QCOMPARE(groups->children[0].tag, QString("CPhysicsSettingsSource")); QCOMPARE(groups->children[1].tag, QString("CPhysicsSettingsSource"));
        QVERIFY(groups->children[0].child("id")); QVERIFY(groups->children[1].child("id"));
        QCOMPARE(groups->children[0].child("id")->attributes.value("idstr"), QString("First"));
        QCOMPARE(groups->children[1].child("id")->attributes.value("idstr"), QString("Second"));
        verifyNormalization(groups->children[0], {-2.5, 3.75, .5, -42, 27, -3});
        verifyNormalization(groups->children[1], {-8, 16, 4, -15.5, 61.25, 7.5});
        if (fileInput) { QFile file(directory.filePath("input.physics3.json")); QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), bytes); }
    }

    void legacyDictionaryNamesRemainAndMetaNamesTakePrecedence_data()
    {
        QTest::addColumn<bool>("modern");
        QTest::newRow("legacy") << false; QTest::newRow("meta-precedence") << true;
    }

    void legacyDictionaryNamesRemainAndMetaNamesTakePrecedence()
    {
        QFETCH(bool, modern);
        auto source = document({group("named")});
        source["PhysicsDictionary"] = QJsonArray{QJsonObject{{"Id", "named"}, {"Name", QStringLiteral("旧字典 · 摆动 & 髪")}}};
        if (modern) source["Meta"] = QJsonObject{{"PhysicsDictionary", QJsonArray{QJsonObject{{"Id", "named"}, {"Name", "Meta name"}}}}};
        QTemporaryDir directory; const auto output = directory.filePath("names.cmo3"); QString error;
        QVERIFY2(build(QJsonDocument(source).toJson(), false, directory.path(), output, &error), qPrintable(error));
        XmlNode physics; QVERIFY2(readPhysics(output, physics, &error), qPrintable(error));
        const auto* groups = physics.child("_sourceCubismPhysics"); QVERIFY(groups); QCOMPARE(groups->children.size(), 1);
        const auto* name = groups->children[0].child("name"); QVERIFY(name);
        QCOMPARE(name->text, modern ? QString("Meta name") : QStringLiteral("旧字典 · 摆动 & 髪"));
    }

    void optionalArraysAndNumbersUseTheSharedZeroDefaults_data()
    {
        QTest::addColumn<bool>("rows");
        QTest::newRow("omitted-arrays") << false; QTest::newRow("omitted-row-values") << true;
    }

    void optionalArraysAndNumbersUseTheSharedZeroDefaults()
    {
        QFETCH(bool, rows);
        QJsonObject minimal{{"Id", "minimal"}, {"Vertices", QJsonArray{QJsonObject{}}}};
        if (rows) {
            minimal["Input"] = QJsonArray{QJsonObject{{"Source", QJsonObject{{"Id", m_input}}}, {"Type", "Angle"}}};
            minimal["Output"] = QJsonArray{QJsonObject{{"Destination", QJsonObject{{"Id", m_output}}}, {"Type", "Angle"}, {"VertexIndex", 0}}};
        }
        const auto source = document({minimal}); QString error;
        QVERIFY2(live2dstruct::CPhysicsDocument::Validate(source, &error), qPrintable(error));
        QTemporaryDir directory; const auto output = directory.filePath("defaults.cmo3");
        QVERIFY2(build(QJsonDocument(source).toJson(), false, directory.path(), output, &error), qPrintable(error));
        XmlNode physics; QVERIFY2(readPhysics(output, physics, &error), qPrintable(error));
        const auto* groups = physics.child("_sourceCubismPhysics"); QVERIFY(groups); QCOMPARE(groups->children.size(), 1);
        const auto& generated = groups->children[0]; verifyNormalization(generated, {0, 0, 0, 0, 0, 0});
        const auto* inputs = generated.child("inputs"); const auto* outputs = generated.child("outputs"); const auto* vertices = generated.child("vertices");
        QVERIFY(inputs && outputs && vertices); QCOMPARE(inputs->children.size(), rows ? 1 : 0); QCOMPARE(outputs->children.size(), rows ? 1 : 0);
        QCOMPARE(vertices->children.size(), 1);
        for (const char* field : {"mobility", "delay", "acceleration", "radius"}) {
            const auto* value = vertices->children[0].child(field); QVERIFY(value); QCOMPARE(value->text.toDouble(), 0.0);
        }
        const auto* position = vertices->children[0].child("position"); QVERIFY(position);
        QVERIFY(position->child("x") && position->child("y"));
        QCOMPARE(position->child("x")->text.toDouble(), 0.0); QCOMPARE(position->child("y")->text.toDouble(), 0.0);
        if (rows) {
            for (const auto* row : {&inputs->children[0], &outputs->children[0]}) {
                QVERIFY(row->child("weight")); QCOMPARE(row->child("weight")->text.toDouble(), 0.0);
                QVERIFY(row->child("isReverse")); QCOMPARE(row->child("isReverse")->text, QString("false"));
            }
            QVERIFY(outputs->children[0].child("angleScale")); QCOMPARE(outputs->children[0].child("angleScale")->text.toDouble(), 0.0);
            QVERIFY(inputs->children[0].child("source")); QVERIFY(outputs->children[0].child("destination"));
            QCOMPARE(inputs->children[0].child("source")->tag, QString("CParameterGuid"));
            QCOMPARE(outputs->children[0].child("destination")->tag, QString("CParameterGuid"));
        }
    }

    void invalidInputsPreserveExistingAndAbsentDestinations_data()
    {
        QTest::addColumn<QString>("mutation"); QTest::addColumn<bool>("fileInput");
        for (const char* mutation : {"meta", "normalization", "range-type", "range-number", "range-order", "range-default",
             "input-reflect", "output-reflect", "empty-vertices", "missing-vertices", "vertex-object", "position", "mobility",
             "delay", "radius", "weight", "index", "empty-id", "duplicate-id", "unknown-input", "unknown-output", "wrong-target"})
            for (bool fileInput : {false, true})
                QTest::newRow(qPrintable(QStringLiteral("%1-%2").arg(mutation).arg(fileInput ? "file" : "bytes"))) << QString::fromLatin1(mutation) << fileInput;
    }

    void invalidInputsPreserveExistingAndAbsentDestinations()
    {
        QFETCH(QString, mutation); QFETCH(bool, fileInput);
        auto value = group(); auto source = document({value});
        if (mutation == "meta") source["Meta"] = QJsonArray{};
        else if (mutation == "normalization") value["Normalization"] = false;
        else if (mutation.startsWith("range-")) {
            QJsonObject range{{"Minimum", -2}, {"Maximum", 2}, {"Default", 0}};
            if (mutation == "range-number") range["Minimum"] = "-2";
            if (mutation == "range-order") range["Minimum"] = 3;
            if (mutation == "range-default") range["Default"] = 3;
            value["Normalization"] = QJsonObject{{"Position", mutation == "range-type" ? QJsonValue(false) : QJsonValue(range)}};
        } else if (mutation == "empty-vertices") { value["Input"] = QJsonArray{}; value["Output"] = QJsonArray{}; value["Vertices"] = QJsonArray{}; }
        else if (mutation == "missing-vertices") { value["Input"] = QJsonArray{}; value["Output"] = QJsonArray{}; value.remove("Vertices"); }
        else if (mutation == "empty-id") value["Id"] = "";
        else if (mutation == "vertex-object" || mutation == "position" || mutation == "mobility" || mutation == "delay" || mutation == "radius") {
            auto vertices = value["Vertices"].toArray(); auto vertex = vertices[1].toObject();
            if (mutation == "position") vertex["Position"] = QJsonObject{{"X", "bad"}, {"Y", 1}};
            if (mutation == "mobility") vertex["Mobility"] = 1.01;
            if (mutation == "delay") vertex["Delay"] = -1;
            if (mutation == "radius") vertex["Radius"] = -1;
            vertices[1] = mutation == "vertex-object" ? QJsonValue(false) : QJsonValue(vertex); value["Vertices"] = vertices;
        } else if (mutation != "duplicate-id") {
            const bool output = mutation == "output-reflect" || mutation == "unknown-output" || mutation == "index";
            const QString field = output ? "Output" : "Input", endpoint = output ? "Destination" : "Source";
            auto entries = value[field].toArray(); auto entry = entries[0].toObject();
            if (mutation.endsWith("reflect")) entry["Reflect"] = "true";
            else if (mutation == "weight") entry["Weight"] = 101;
            else if (mutation == "index") entry["VertexIndex"] = .5;
            else { auto reference = entry[endpoint].toObject(); reference[mutation == "wrong-target" ? "Target" : "Id"] = mutation == "wrong-target" ? "Part" : "UnknownActualParameter"; entry[endpoint] = reference; }
            entries[0] = entry; value[field] = entries;
        }
        QJsonArray groups{value}; if (mutation == "duplicate-id") groups.append(value); source["PhysicsSettings"] = groups;
        const auto bytes = QJsonDocument(source).toJson(); QTemporaryDir directory; QVERIFY(directory.isValid());
        const auto output = directory.filePath("existing.cmo3"); const QByteArray sentinel("unchanged destination"); QVERIFY(write(output, sentinel));
        QString error; QVERIFY(!build(bytes, fileInput, directory.path(), output, &error)); QVERIFY(!error.isEmpty());
        QFile file(output); QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), sentinel); file.close();
        const auto absent = directory.filePath("absent.cmo3"); QVERIFY(!build(bytes, fileInput, directory.path(), absent, &error));
        QVERIFY(!QFileInfo::exists(absent));
    }

    void byteLimitIsTheSameForSnapshotAndFile_data()
    {
        QTest::addColumn<bool>("fileInput");
        QTest::newRow("bytes") << false; QTest::newRow("file") << true;
    }

    void byteLimitIsTheSameForSnapshotAndFile()
    {
        QFETCH(bool, fileInput);
        QByteArray bytes = QJsonDocument(document({})).toJson(QJsonDocument::Compact);
        bytes.append(QByteArray(MaximumBytes - bytes.size(), ' ')); QCOMPARE(bytes.size(), MaximumBytes);
        QTemporaryDir directory; const auto output = directory.filePath("limit.cmo3"); QString error;
        QVERIFY2(build(bytes, fileInput, directory.path(), output, &error), qPrintable(error));
        QFile file(output); QVERIFY(file.open(QIODevice::ReadOnly)); const auto original = file.readAll(); file.close(); QVERIFY(!original.isEmpty());
        bytes.append(' '); QVERIFY(!build(bytes, fileInput, directory.path(), output, &error)); QVERIFY(!error.isEmpty());
        QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), original);
        const auto absent = directory.filePath("over-limit.cmo3"); QVERIFY(!build(bytes, fileInput, directory.path(), absent, &error)); QVERIFY(!QFileInfo::exists(absent));
    }

    void prepareWithoutPhysicsDoesNotReadMocAndClearsOldResult()
    {
        cmo3::CCmo3Builder::Options options; options.mocPath = "missing-model.moc3";
        cmo3::CCmo3Builder::PhysicsExport result{true, QJsonObject{{"old", true}}, {"old"}};
        QString error("old"); QVERIFY(cmo3::CCmo3Builder::PreparePhysicsExport(options, result, &error));
        QVERIFY(error.isEmpty()); QVERIFY(!result.present); QVERIFY(result.document.isEmpty()); QVERIFY(result.conversions.isEmpty());
        QVERIFY(!options.physicsBytes); QVERIFY(options.physicsPath.isEmpty()); QVERIFY(!options.allowPhysicsConversion);
    }

    void prepareSnapshotHasPriorityAndRetainsUnconvertedValues_data()
    {
        QTest::addColumn<int>("pathKind");
        QTest::newRow("other-valid-file") << 0; QTest::newRow("missing-file") << 1;
        QTest::newRow("invalid-file") << 2; QTest::newRow("directory") << 3;
    }

    void prepareSnapshotHasPriorityAndRetainsUnconvertedValues()
    {
        QFETCH(int, pathKind);
        auto value = group("Snapshot"); auto outputs = value["Output"].toArray(); auto output = outputs[0].toObject();
        output["Type"] = "X"; output["Scale"] = -2.5; outputs[0] = output; value["Output"] = outputs;
        auto source = document({value});
        source["Meta"] = QJsonObject{{"Fps", 29.97}, {"TotalInputCount", 99},
            {"EffectiveForces", QJsonObject{{"Gravity", QJsonObject{{"X", .5}}}, {"Wind", QJsonObject{{"Y", .25}}}}}};
        source["Extension"] = QJsonObject{{"preserve", QJsonArray{1, "two"}}};
        const auto bytes = QJsonDocument(source).toJson(); const auto before = source;
        QTemporaryDir directory;
        cmo3::CCmo3Builder::Options options; options.mocPath = directory.filePath("missing-model.moc3"); options.physicsBytes = bytes;
        options.physicsPath = pathKind == 3 ? directory.path() : directory.filePath("source.physics3.json");
        if (pathKind == 0) QVERIFY(write(options.physicsPath, QJsonDocument(document({group("Disk")})).toJson()));
        if (pathKind == 2) QVERIFY(write(options.physicsPath, "{broken"));
        cmo3::CCmo3Builder::PhysicsExport result; QString error;
        QVERIFY2(cmo3::CCmo3Builder::PreparePhysicsExport(options, result, &error), qPrintable(error));
        QVERIFY(result.present); QCOMPARE(result.conversions.size(), 3);
        const auto notes = result.conversions.join('\n'); QVERIFY(notes.contains("FPS")); QVERIFY(notes.contains(QStringLiteral("重力")));
        QVERIFY(notes.contains(QStringLiteral("风力"))); QVERIFY(notes.contains("X/Y")); QVERIFY(notes.contains("Angle"));
        QCOMPARE(result.document["PhysicsSettings"], source["PhysicsSettings"]); QCOMPARE(result.document["Extension"], source["Extension"]);
        const auto meta = result.document["Meta"].toObject(); QCOMPARE(meta["Fps"].toDouble(), 29.97);
        QCOMPARE(meta["EffectiveForces"], source["Meta"].toObject()["EffectiveForces"]);
        QCOMPARE(meta["PhysicsSettingCount"].toInt(), 1); QCOMPARE(meta["TotalInputCount"].toInt(), 1);
        QCOMPARE(meta["TotalOutputCount"].toInt(), 1); QCOMPARE(meta["VertexCount"].toInt(), 2);
        QCOMPARE(*options.physicsBytes, bytes); QCOMPARE(source, before); QVERIFY(!options.allowPhysicsConversion);
    }

    void fpsUsesExplicitIntegerConversionAndKeepsMissingDistinct_data()
    {
        QTest::addColumn<bool>("missing"); QTest::addColumn<double>("fps"); QTest::addColumn<int>("encoded");
        QTest::addColumn<bool>("conversion");
        QTest::newRow("missing") << true << 0.0 << 0 << false;
        QTest::newRow("zero") << false << 0.0 << 0 << false;
        QTest::newRow("one") << false << 1.0 << 1 << false;
        QTest::newRow("thirty") << false << 30.0 << 30 << false;
        QTest::newRow("upper-bound") << false << 1000.0 << 1000 << false;
        QTest::newRow("rounds-to-thirty") << false << 29.999999999 << 30 << false;
        QTest::newRow("rounds-to-one") << false << .999999999 << 1 << false;
        QTest::newRow("fraction") << false << 29.97 << 29 << true;
        QTest::newRow("sub-one") << false << .5 << 0 << true;
    }

    void fpsUsesExplicitIntegerConversionAndKeepsMissingDistinct()
    {
        QFETCH(bool, missing); QFETCH(double, fps); QFETCH(int, encoded); QFETCH(bool, conversion);
        auto source = document({group()}); if (!missing) source["Meta"] = QJsonObject{{"Fps", fps}};
        const auto bytes = QJsonDocument(source).toJson();
        cmo3::CCmo3Builder::Options options; options.physicsBytes = bytes;
        cmo3::CCmo3Builder::PhysicsExport prepared; QString error;
        QVERIFY2(cmo3::CCmo3Builder::PreparePhysicsExport(options, prepared, &error), qPrintable(error));
        QCOMPARE(prepared.conversions.size(), conversion ? 1 : 0);
        if (missing) QVERIFY(!prepared.document["Meta"].toObject().contains("Fps"));
        else QCOMPARE(prepared.document["Meta"].toObject()["Fps"].toDouble(), fps);
        QTemporaryDir directory; const auto path = directory.filePath("fps.cmo3");
        if (conversion) {
            QVERIFY(prepared.conversions[0].contains("FPS")); QVERIFY(prepared.conversions[0].contains(QString::number(encoded)));
            QVERIFY(write(path, "preserved")); QVERIFY(!build(bytes, false, directory.path(), path, &error));
            QFile file(path); QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), QByteArray("preserved"));
        }
        QVERIFY2(build(bytes, false, directory.path(), path, &error, conversion), qPrintable(error));
        XmlNode physics; QVERIFY2(readPhysics(path, physics, &error), qPrintable(error));
        const auto* setting = physics.child("settingFPS"); QVERIFY(setting);
        QCOMPARE(setting->tag, missing ? QString("null") : QString("i"));
        if (missing) QVERIFY(setting->text.isEmpty());
        else { bool ok = false; QCOMPARE(setting->text.toInt(&ok), encoded); QVERIFY(ok); }
        const auto* groups = physics.child("_sourceCubismPhysics"); QVERIFY(groups); QCOMPARE(groups->children.size(), 1);
        QVERIFY(!groups->children[0].child("settingFPS")); QCOMPARE(*options.physicsBytes, bytes);
    }

    void forceDefaultsNeedNoPermissionAndNonDefaultsRequireIt_data()
    {
        QTest::addColumn<QJsonObject>("forces"); QTest::addColumn<bool>("conversion");
        QTest::newRow("missing") << QJsonObject{} << false;
        QTest::newRow("gravity-x-default") << QJsonObject{{"Gravity", QJsonObject{{"X", 0}}}} << false;
        QTest::newRow("gravity-y-default") << QJsonObject{{"Gravity", QJsonObject{{"Y", -1}}}} << false;
        QTest::newRow("wind-partial-default") << QJsonObject{{"Wind", QJsonObject{{"X", 0}}}} << false;
        QTest::newRow("gravity-rounds-to-default") << QJsonObject{{"Gravity", QJsonObject{{"Y", -.999999999}}}} << false;
        QTest::newRow("wind-rounds-to-zero") << QJsonObject{{"Wind", QJsonObject{{"X", 1.0e-50}}}} << false;
        QTest::newRow("explicit-defaults") << QJsonObject{{"Gravity", QJsonObject{{"X", 0}, {"Y", -1}}}, {"Wind", QJsonObject{{"X", 0}, {"Y", 0}}}} << false;
        QTest::newRow("gravity-x") << QJsonObject{{"Gravity", QJsonObject{{"X", .25}}}} << true;
        QTest::newRow("gravity-y-zero") << QJsonObject{{"Gravity", QJsonObject{{"Y", 0}}}} << true;
        QTest::newRow("wind-x") << QJsonObject{{"Wind", QJsonObject{{"X", -.125}}}} << true;
        QTest::newRow("wind-y") << QJsonObject{{"Wind", QJsonObject{{"Y", .5}}}} << true;
    }

    void forceDefaultsNeedNoPermissionAndNonDefaultsRequireIt()
    {
        QFETCH(QJsonObject, forces); QFETCH(bool, conversion);
        auto source = document({group()});
        if (!forces.isEmpty()) source["Meta"] = QJsonObject{{"EffectiveForces", forces}};
        const auto bytes = QJsonDocument(source).toJson();
        cmo3::CCmo3Builder::Options options; options.physicsBytes = bytes;
        cmo3::CCmo3Builder::PhysicsExport prepared; QString error;
        QVERIFY2(cmo3::CCmo3Builder::PreparePhysicsExport(options, prepared, &error), qPrintable(error));
        QCOMPARE(prepared.conversions.size(), conversion ? 1 : 0);
        QCOMPARE(prepared.document["Meta"].toObject()["EffectiveForces"].toObject(), forces);
        QTemporaryDir directory; const auto path = directory.filePath("forces.cmo3");
        if (conversion) {
            QVERIFY(prepared.conversions[0].contains(QStringLiteral("重力"))); QVERIFY(prepared.conversions[0].contains(QStringLiteral("风力")));
            QVERIFY(prepared.conversions[0].contains(QStringLiteral("不保存")));
            QVERIFY(write(path, "preserved")); QVERIFY(!build(bytes, true, directory.path(), path, &error));
            QFile file(path); QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), QByteArray("preserved"));
        }
        QVERIFY2(build(bytes, true, directory.path(), path, &error, conversion), qPrintable(error));
        XmlNode physics; QVERIFY2(readPhysics(path, physics, &error), qPrintable(error));
        const auto* groups = physics.child("_sourceCubismPhysics"); QVERIFY(groups); QCOMPARE(groups->children.size(), 1);
        QVERIFY(!physics.child("gravity")); QVERIFY(!physics.child("wind"));
        QCOMPARE(*options.physicsBytes, bytes);
    }

    void outputAxesRequirePermissionAndKeepTheirScale_data()
    {
        QTest::addColumn<QString>("type"); QTest::addColumn<double>("scale");
        for (const char* type : {"X", "Y"}) for (double scale : {-2.5, 0.0, 1.25})
            QTest::newRow(qPrintable(QStringLiteral("%1-%2").arg(type).arg(scale))) << QString::fromLatin1(type) << scale;
    }

    void outputAxesRequirePermissionAndKeepTheirScale()
    {
        QFETCH(QString, type); QFETCH(double, scale);
        auto value = group(); auto outputs = value["Output"].toArray(); auto output = outputs[0].toObject();
        output["Type"] = type; output["Scale"] = scale; output["Weight"] = 45; output["Reflect"] = true;
        outputs[0] = output; value["Output"] = outputs;
        auto inputs = value["Input"].toArray(); auto input = inputs[0].toObject(); input["Type"] = type; inputs[0] = input; value["Input"] = inputs;
        const auto bytes = QJsonDocument(document({value})).toJson();
        cmo3::CCmo3Builder::Options options; options.physicsBytes = bytes;
        cmo3::CCmo3Builder::PhysicsExport prepared; QString error;
        QVERIFY2(cmo3::CCmo3Builder::PreparePhysicsExport(options, prepared, &error), qPrintable(error));
        QCOMPARE(prepared.conversions.size(), 1); QVERIFY(prepared.conversions[0].contains("X/Y")); QVERIFY(prepared.conversions[0].contains("Angle"));
        const QJsonObject originalOutput = prepared.document["PhysicsSettings"].toArray()[0].toObject()["Output"].toArray()[0].toObject();
        QCOMPARE(originalOutput["Type"].toString(), type); QCOMPARE(originalOutput["Scale"].toDouble(), scale);
        QTemporaryDir directory; const auto path = directory.filePath("axes.cmo3"); QVERIFY(write(path, "preserved"));
        QVERIFY(!build(bytes, false, directory.path(), path, &error));
        QFile file(path); QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), QByteArray("preserved")); file.close();
        QVERIFY2(build(bytes, false, directory.path(), path, &error, true), qPrintable(error));
        XmlNode physics; QVERIFY2(readPhysics(path, physics, &error), qPrintable(error));
        const auto* groups = physics.child("_sourceCubismPhysics"); QVERIFY(groups); QCOMPARE(groups->children.size(), 1);
        const auto* emittedOutputs = groups->children[0].child("outputs"); const auto* emittedInputs = groups->children[0].child("inputs");
        QVERIFY(emittedOutputs && emittedInputs); QCOMPARE(emittedOutputs->children.size(), 1); QCOMPARE(emittedInputs->children.size(), 1);
        const auto& result = emittedOutputs->children[0];
        QVERIFY(result.child("type")); QCOMPARE(result.child("type")->attributes.value("v"), QString("SRC_TO_G_ANGLE"));
        QVERIFY(result.child("angleScale")); QCOMPARE(result.child("angleScale")->text.toDouble(), scale);
        QVERIFY(result.child("weight")); QCOMPARE(result.child("weight")->text.toDouble(), 45.0);
        QVERIFY(result.child("isReverse")); QCOMPARE(result.child("isReverse")->text, QString("true"));
        const auto* translation = result.child("translationScale"); QVERIFY(translation); QVERIFY(translation->child("x") && translation->child("y"));
        QCOMPARE(translation->child("x")->text.toDouble(), 0.0); QCOMPARE(translation->child("y")->text.toDouble(), 0.0);
        QVERIFY(emittedInputs->children[0].child("type"));
        QCOMPARE(emittedInputs->children[0].child("type")->attributes.value("v"), type == "X" ? QString("SRC_TO_X") : QString("SRC_TO_Y"));
        QCOMPARE(*options.physicsBytes, bytes);
    }

    void combinedConversionsAreReportedTogetherAndRemainExplicit()
    {
        auto value = group(); auto first = value["Output"].toArray()[0].toObject(); auto second = first;
        first["Type"] = "X"; first["Scale"] = -3; second["Type"] = "Y"; second["Scale"] = 0;
        value["Output"] = QJsonArray{first, second};
        auto source = document({value}); source["Meta"] = QJsonObject{{"Fps", 29.97},
            {"EffectiveForces", QJsonObject{{"Wind", QJsonObject{{"X", .125}}}}}};
        const auto bytes = QJsonDocument(source).toJson();
        cmo3::CCmo3Builder::Options options; options.physicsBytes = bytes;
        cmo3::CCmo3Builder::PhysicsExport prepared; QString error;
        QVERIFY(cmo3::CCmo3Builder::PreparePhysicsExport(options, prepared, &error)); QCOMPARE(prepared.conversions.size(), 3);
        const auto notes = prepared.conversions.join('\n'); QVERIFY(notes.contains("FPS")); QVERIFY(notes.contains(QStringLiteral("风力"))); QVERIFY(notes.contains("X/Y"));
        QTemporaryDir directory; const auto path = directory.filePath("combined.cmo3");
        QVERIFY(!build(bytes, false, directory.path(), path, &error)); QVERIFY(!QFileInfo::exists(path));
        QVERIFY2(build(bytes, false, directory.path(), path, &error, true), qPrintable(error));
        XmlNode physics; QVERIFY2(readPhysics(path, physics, &error), qPrintable(error));
        QVERIFY(physics.child("settingFPS")); QCOMPARE(physics.child("settingFPS")->text, QString("29"));
        const auto* groups = physics.child("_sourceCubismPhysics"); QVERIFY(groups); QCOMPARE(groups->children.size(), 1);
        const auto* outputs = groups->children[0].child("outputs"); QVERIFY(outputs); QCOMPARE(outputs->children.size(), 2);
        for (const auto& output : outputs->children) {
            QVERIFY(output.child("type")); QCOMPARE(output.child("type")->attributes.value("v"), QString("SRC_TO_G_ANGLE"));
        }
        QVERIFY(outputs->children[0].child("angleScale") && outputs->children[1].child("angleScale"));
        QCOMPARE(outputs->children[0].child("angleScale")->text.toDouble(), -3.0); QCOMPARE(outputs->children[1].child("angleScale")->text.toDouble(), 0.0);
        QCOMPARE(prepared.document["PhysicsSettings"], source["PhysicsSettings"]);
        QCOMPARE(prepared.document["Meta"].toObject()["Fps"].toDouble(), 29.97); QCOMPARE(*options.physicsBytes, bytes);
    }

    void preparationFailuresKeepThePreviousReport_data()
    {
        QTest::addColumn<QString>("failure");
        for (const char* failure : {"missing-file", "directory", "broken-file", "empty-bytes", "broken-bytes", "large-bytes", "large-file",
             "normalization", "reflect", "duplicate-id"}) QTest::newRow(failure) << QString::fromLatin1(failure);
    }

    void preparationFailuresKeepThePreviousReport()
    {
        QFETCH(QString, failure); QTemporaryDir directory;
        cmo3::CCmo3Builder::Options options; options.physicsPath = directory.filePath("source.physics3.json"); options.mocPath = "not-read.moc3";
        const auto original = QJsonDocument(document({group()})).toJson(); QVERIFY(write(options.physicsPath, original));
        if (failure == "missing-file") options.physicsPath = directory.filePath("missing");
        else if (failure == "directory") options.physicsPath = directory.path();
        else if (failure == "broken-file") QVERIFY(write(options.physicsPath, "{"));
        else if (failure == "empty-bytes") options.physicsBytes = QByteArray();
        else if (failure == "broken-bytes") options.physicsBytes = QByteArray("{");
        else if (failure == "large-bytes" || failure == "large-file") {
            QByteArray bytes = original; bytes.append(QByteArray(MaximumBytes + 1 - bytes.size(), ' '));
            if (failure == "large-bytes") options.physicsBytes = bytes; else QVERIFY(write(options.physicsPath, bytes));
        } else {
            auto value = group();
            if (failure == "normalization") value["Normalization"] = QJsonObject{{"Angle", QJsonObject{{"Minimum", 3}, {"Maximum", 1}}}};
            if (failure == "reflect") { auto inputs = value["Input"].toArray(); auto input = inputs[0].toObject(); input["Reflect"] = 1; inputs[0] = input; value["Input"] = inputs; }
            QJsonArray groups{value}; if (failure == "duplicate-id") groups.append(value);
            options.physicsBytes = QJsonDocument(document(groups)).toJson();
        }
        cmo3::CCmo3Builder::PhysicsExport result{true, QJsonObject{{"untouched", 7}}, {"old conversion"}};
        const auto before = result; QString error;
        QVERIFY(!cmo3::CCmo3Builder::PreparePhysicsExport(options, result, &error)); QVERIFY(!error.isEmpty());
        QCOMPARE(result.present, before.present); QCOMPARE(result.document, before.document); QCOMPARE(result.conversions, before.conversions);
    }

    void prepareDoesNotCheckMocMembershipButBuildAlwaysDoes_data()
    {
        QTest::addColumn<bool>("input"); QTest::newRow("input") << true; QTest::newRow("output") << false;
    }

    void prepareDoesNotCheckMocMembershipButBuildAlwaysDoes()
    {
        QFETCH(bool, input);
        auto value = group(); auto outputs = value["Output"].toArray(); auto output = outputs[0].toObject(); output["Type"] = "X";
        outputs[0] = output; value["Output"] = outputs;
        const QString field = input ? "Input" : "Output", endpoint = input ? "Source" : "Destination";
        auto rows = value[field].toArray(); auto row = rows[0].toObject(); auto reference = row[endpoint].toObject();
        reference["Id"] = "UnknownActualParameter"; row[endpoint] = reference; rows[0] = row; value[field] = rows;
        const auto bytes = QJsonDocument(document({value})).toJson();
        cmo3::CCmo3Builder::Options options; options.mocPath = "missing.moc3"; options.physicsBytes = bytes;
        cmo3::CCmo3Builder::PhysicsExport prepared; QString error;
        QVERIFY2(cmo3::CCmo3Builder::PreparePhysicsExport(options, prepared, &error), qPrintable(error));
        QVERIFY(prepared.present); QCOMPARE(prepared.conversions.size(), 1);
        QTemporaryDir directory; const auto path = directory.filePath("unknown.cmo3"); QVERIFY(write(path, "preserved"));
        QVERIFY(!build(bytes, false, directory.path(), path, &error, true)); QVERIFY(error.contains("UnknownActualParameter"));
        QFile file(path); QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), QByteArray("preserved"));
        const auto absent = directory.filePath("absent.cmo3"); QVERIFY(!build(bytes, true, directory.path(), absent, &error, true));
        QVERIFY(!QFileInfo::exists(absent));
    }
};

QTEST_GUILESS_MAIN(Cmo3PhysicsTests)
#include "cmo3_physics_tests.moc"
