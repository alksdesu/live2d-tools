#include "live2d/cparameterreferenceplan.h"
#include "live2dstruct/cmodelsession.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest>

using Plan = CParameterReferencePlan;
using Edits = live2dstruct::CParameterResourceEdits;
using Editor = live2dstruct::CParameterReferenceEditor;

namespace {
QByteArray encode(const QJsonObject& object) { return QJsonDocument(object).toJson(QJsonDocument::Indented); }
QByteArray digest(const QByteArray& bytes) { return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256); }
QByteArray read(const QString& path) { QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray(); }
bool write(const QString& path, const QByteArray& bytes)
{
    QFile file(path); return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
QJsonObject expression()
{
    return {{"Parameters", QJsonArray{QJsonObject{{"Id", "P"}, {"Value", .7}},
        QJsonObject{{"Id", "VirtualOnly"}, {"Value", .2}}, QJsonObject{{"Id", "Keep"}, {"Value", .3}}}}};
}
QJsonObject motion()
{
    return {{"Version", 3}, {"Meta", QJsonObject{{"Duration", 1}, {"Fps", 30}, {"Loop", false},
        {"AreBeziersRestricted", true}, {"CurveCount", 4}, {"TotalSegmentCount", 4}, {"TotalPointCount", 8},
        {"UserDataCount", 0}, {"TotalUserDataSize", 0}}},
        {"Curves", QJsonArray{QJsonObject{{"Target", "Model"}, {"Id", "Opacity"}, {"Segments", QJsonArray{0, 1, 0, 1, 1}}},
            QJsonObject{{"Target", "Parameter"}, {"Id", "P"}, {"Segments", QJsonArray{0, .1, 0, 1, .9}}},
            QJsonObject{{"Target", "Parameter"}, {"Id", "VirtualOnly"}, {"Segments", QJsonArray{0, .2, 0, 1, .8}}},
            QJsonObject{{"Target", "PartOpacity"}, {"Id", "P"}, {"Segments", QJsonArray{0, .3, 0, 1, .6}}}}}};
}
QJsonObject physics()
{
    const auto input = [](const char* id) { return QJsonObject{{"Source", QJsonObject{{"Target", "Parameter"}, {"Id", id}}}, {"Weight", 50}, {"Type", "X"}, {"Reflect", false}}; };
    const auto output = [](const char* id) { return QJsonObject{{"Destination", QJsonObject{{"Target", "Parameter"}, {"Id", id}}}, {"Weight", 100}, {"Type", "Angle"}, {"Scale", 1}, {"VertexIndex", 0}, {"Reflect", false}}; };
    return {{"Version", 3}, {"Meta", QJsonObject{{"TotalInputCount", 3}, {"TotalOutputCount", 2}}},
        {"PhysicsSettings", QJsonArray{QJsonObject{{"Id", "One"}, {"Input", QJsonArray{input("Keep"), input("P")}}, {"Output", QJsonArray{output("P")}}, {"Vertices", QJsonArray{}}},
            QJsonObject{{"Id", "Two"}, {"Input", QJsonArray{input("P")}}, {"Output", QJsonArray{output("Keep")}}, {"Vertices", QJsonArray{}}}}}};
}
QJsonObject model()
{
    return {{"Version", 3}, {"FileReferences", QJsonObject{{"Moc", "model.moc3"}, {"Physics", "rig.physics3.json"},
        {"Motions", QJsonObject{{"Idle", QJsonArray{QJsonObject{{"File", "motion.motion3.json"}}}}}},
        {"Expressions", QJsonArray{QJsonObject{{"Name", "Face"}, {"File", "face.exp3.json"}}}}}},
        {"Groups", QJsonArray{QJsonObject{{"Name", "EyeBlink"}, {"Target", "Parameter"}, {"Ids", QJsonArray{"P", "Keep"}}},
            QJsonObject{{"Name", "LipSync"}, {"Target", "Parameter"}, {"Ids", QJsonArray{"P"}}},
            QJsonObject{{"Name", "Parts"}, {"Target", "Part"}, {"Ids", QJsonArray{"P"}}}}}};
}
ParameterReferenceCatalog catalogFor(const QString& path)
{
    ParameterReferenceCatalog result;
    const auto bytes = read(path); result.modelSettingsDigest = digest(bytes);
    const auto root = QJsonDocument::fromJson(bytes).object();
    const auto files = root.value("FileReferences").toObject(); const QDir directory = QFileInfo(path).absoluteDir();
    const auto key = [&](const QString& file) { return QDir::cleanPath(directory.relativeFilePath(QFileInfo(directory.filePath(file)).absoluteFilePath())); };
    const auto motions = files.value("Motions").toObject();
    for (const auto& group : motions) for (const auto& row : group.toArray()) {
        const auto file = row.toObject().value("File").toString(); result.motions.insert(key(file), digest(read(directory.filePath(file))));
    }
    for (const auto& row : files.value("Expressions").toArray()) {
        const auto file = row.toObject().value("File").toString(); result.expressions.insert(key(file), digest(read(directory.filePath(file))));
    }
    const auto physics = files.value("Physics").toString();
    if (!physics.isEmpty()) { result.physicsPath = key(physics); result.physicsDigest = digest(read(directory.filePath(physics))); }
    for (const auto& value : root.value("Groups").toArray()) {
        const auto group = value.toObject();
        auto* ids = group.value("Name").toString() == "EyeBlink" ? &result.eyeBlink : group.value("Name").toString() == "LipSync" ? &result.lipSync : nullptr;
        if (ids) for (const auto& id : group.value("Ids").toArray()) ids->insert(id.toString());
    }
    return result;
}
bool fixture(QTemporaryDir& directory, QString& path, ParameterReferenceCatalog& catalog)
{
    path = directory.filePath("model.model3.json");
    if (!directory.isValid() || !write(path, encode(model())) || !write(directory.filePath("motion.motion3.json"), encode(motion())) ||
        !write(directory.filePath("face.exp3.json"), encode(expression())) || !write(directory.filePath("rig.physics3.json"), encode(physics()))) return false;
    catalog = catalogFor(path); catalog.breath = {"P", "BreathOnly"}; catalog.mouseFollow = {"P", "MouseOnly"};
    return true;
}
QJsonObject persistent(const Edits::State& state)
{
    QJsonObject json; QString error;
    if (!Edits::ToJson(state, json, &error)) qFatal("Invalid test state: %s", qPrintable(error));
    return json;
}
}

class ParameterReferencePlanTests : public QObject
{
    Q_OBJECT
private slots:
    void oldSessionStateAcceptsRegistrationOnlyWithTheNewLoadedCatalog()
    {
        QTemporaryDir directory; QString path; ParameterReferenceCatalog catalog; QVERIFY(fixture(directory, path, catalog));
        Plan::Prepared original; QString error; QVERIFY(Plan::Prepare(path, {}, {"P"}, catalog, original, &error));
        const auto originalState = persistent(original.resources.state);
        live2dstruct::CModelSession saved; saved.parameterResources = original.resources.state;
        const auto sessionPath = directory.filePath("old.live2d-session.json"); QVERIFY(saved.Save(sessionPath, &error));
        auto root = model(); auto files = root["FileReferences"].toObject(); files["UserData"] = "labels.json"; root["FileReferences"] = files;
        const auto updated = encode(root); QVERIFY(write(path, updated));
        live2dstruct::CModelSession loaded; QVERIFY(live2dstruct::CModelSession::Load(sessionPath, &loaded, &error));
        QCOMPARE(persistent(loaded.parameterResources), originalState);
        Plan::Prepared result = original;
        QVERIFY(!Plan::Prepare(path, loaded.parameterResources, {}, catalog, result, &error));
        QVERIFY(error.contains(QStringLiteral("模型设置与磁盘计划不是同一版本"))); QCOMPARE(persistent(result.resources.state), originalState);
        QVERIFY(!Plan::Prepare(path, {}, {}, catalog, result, &error)); QCOMPARE(persistent(result.resources.state), originalState);
        auto updatedCatalog = catalog; updatedCatalog.modelSettingsDigest = digest(updated);
        QVERIFY2(Plan::Prepare(path, loaded.parameterResources, {}, updatedCatalog, result, &error), qPrintable(error));
        QCOMPARE(result.resources.state.modelSource, updated); QCOMPARE(result.runtime.modelSettingsDigest, updatedCatalog.modelSettingsDigest);
        QCOMPARE(result.runtime.physics.suppressedInputs, original.runtime.physics.suppressedInputs);
        QCOMPARE(result.runtime.motions.first().suppressedParameters, original.runtime.motions.first().suppressedParameters);
        QCOMPARE(persistent(loaded.parameterResources), originalState);
        Plan::Prepared undo; QVERIFY(Plan::Prepare(path, {}, {}, updatedCatalog, undo, &error)); QVERIFY(undo.resources.state.IsEmpty());
        Plan::Prepared redo; QVERIFY(Plan::Prepare(path, loaded.parameterResources, {"P"}, updatedCatalog, redo, &error));
        QCOMPARE(persistent(redo.resources.state), persistent(result.resources.state));
        auto tampered = root; tampered["Layout"] = QJsonObject{{"width", 3}}; QVERIFY(write(path, encode(tampered)));
        updatedCatalog.modelSettingsDigest = digest(encode(tampered));
        const auto accepted = persistent(result.resources.state);
        QVERIFY(!Plan::Prepare(path, loaded.parameterResources, {}, updatedCatalog, result, &error)); QCOMPARE(persistent(result.resources.state), accepted);
    }

    void originalRowsAndTypedChannelsArePreserved()
    {
        QTemporaryDir directory; QString path; ParameterReferenceCatalog catalog;
        QVERIFY(fixture(directory, path, catalog));
        Plan::Prepared prepared; QString error;
        QVERIFY2(Plan::Prepare(path, {}, {"P"}, catalog, prepared, &error), qPrintable(error));
        QCOMPARE(prepared.runtime.motions.value("motion.motion3.json").suppressedParameters, (QMap<int, QString>{{1, "P"}}));
        QCOMPARE(prepared.runtime.expressions.value("face.exp3.json").suppressedParameters, (QMap<int, QString>{{0, "P"}}));
        QCOMPARE(prepared.runtime.physics.suppressedInputs, (QMap<int, QString>{{1, "P"}, {2, "P"}}));
        QCOMPARE(prepared.runtime.physics.suppressedOutputs, (QMap<int, QString>{{0, "P"}}));
        QCOMPARE(prepared.runtime.eyeBlink, QSet<QString>{"P"}); QCOMPARE(prepared.runtime.lipSync, QSet<QString>{"P"});
        QCOMPARE(prepared.runtime.breath, QSet<QString>{"P"}); QCOMPARE(prepared.runtime.mouseFollow, QSet<QString>{"P"});
        const auto curves = prepared.resources.resources.value("motion.motion3.json").effective.value("Curves").toArray();
        QCOMPARE(curves.last().toObject().value("Target").toString(), QString("PartOpacity"));
        QCOMPARE(prepared.resources.model.effective.value("Groups").toArray().last().toObject().value("Ids").toArray(), QJsonArray{"P"});
        QCOMPARE(read(path), encode(model())); QCOMPARE(read(directory.filePath("rig.physics3.json")), encode(physics()));
        QCOMPARE(read(directory.filePath("motion.motion3.json")), encode(motion())); QCOMPARE(read(directory.filePath("face.exp3.json")), encode(expression()));
    }

    void sameIdRecreationDoesNotReleaseExistingMasks()
    {
        QTemporaryDir directory; QString path; ParameterReferenceCatalog catalog; QVERIFY(fixture(directory, path, catalog));
        Plan::Prepared first, recreated, second; QString error;
        QVERIFY(Plan::Prepare(path, {}, {"P"}, catalog, first, &error));
        QVERIFY(Plan::Prepare(path, first.resources.state, {}, catalog, recreated, &error));
        QCOMPARE(persistent(first.resources.state), persistent(recreated.resources.state));
        QCOMPARE(recreated.runtime.motions.value("motion.motion3.json").suppressedParameters, (QMap<int, QString>{{1, "P"}}));
        QVERIFY(Plan::Prepare(path, recreated.resources.state, {"VirtualOnly"}, catalog, second, &error));
        QCOMPARE(second.runtime.motions.value("motion.motion3.json").suppressedParameters, (QMap<int, QString>{{1, "P"}, {2, "VirtualOnly"}}));
        QCOMPARE(second.runtime.expressions.value("face.exp3.json").suppressedParameters, (QMap<int, QString>{{0, "P"}, {1, "VirtualOnly"}}));
        QCOMPARE(second.runtime.physics.sourceDigest, catalog.physicsDigest);
    }

    void loadedSourceChangesRejectEvenWithoutRemainingMask_data()
    {
        QTest::addColumn<int>("kind"); QTest::newRow("motion") << 0; QTest::newRow("expression") << 1; QTest::newRow("physics") << 2;
    }
    void loadedSourceChangesRejectEvenWithoutRemainingMask()
    {
        QFETCH(int, kind);
        QTemporaryDir directory; QString path; ParameterReferenceCatalog catalog; QVERIFY(fixture(directory, path, catalog));
        const auto name = kind == 0 ? "motion.motion3.json" : kind == 1 ? "face.exp3.json" : "rig.physics3.json";
        const auto type = kind == 0 ? Editor::Kind::Motion : kind == 1 ? Editor::Kind::Expression : Editor::Kind::Physics;
        Editor::Prepared changed; QString error;
        QVERIFY(Editor::Prepare(type, read(directory.filePath(name)), {"P"}, changed, &error));
        QVERIFY(write(directory.filePath(name), changed.effectiveBytes));
        Editor::Prepared noMask; QVERIFY(Editor::Prepare(type, changed.effectiveBytes, {"P"}, noMask, &error));
        QVERIFY(noMask.references.isEmpty());
        Plan::Prepared output; output.runtime.modelSettingsDigest = "retained"; output.resources.model.sourceBytes = "retained-source";
        QVERIFY(!Plan::Prepare(path, {}, {"P"}, catalog, output, &error));
        QVERIFY2(error.contains(name), qPrintable(error));
        QCOMPARE(output.runtime.modelSettingsDigest, QByteArray("retained")); QCOMPARE(output.resources.model.sourceBytes, QByteArray("retained-source"));
    }

    void malformedCatalogAndUnknownPreviousCapabilitiesFailAtomically_data()
    {
        QTest::addColumn<int>("kind");
        for (int kind = 0; kind < 10; ++kind) QTest::newRow(qPrintable(QString::number(kind))) << kind;
    }
    void malformedCatalogAndUnknownPreviousCapabilitiesFailAtomically()
    {
        QFETCH(int, kind);
        QTemporaryDir directory; QString path; ParameterReferenceCatalog catalog; QVERIFY(fixture(directory, path, catalog));
        Plan::Prepared original; QString error; QVERIFY(Plan::Prepare(path, {}, {"P"}, catalog, original, &error));
        auto state = original.resources.state;
        if (kind == 0) catalog.modelSettingsDigest = QByteArray(32, 'x');
        if (kind == 1) catalog.motions.insert("undeclared.motion3.json", QByteArray(32, 'x'));
        if (kind == 2) catalog.expressions.insert("motion.motion3.json", catalog.motions.first());
        if (kind == 3) catalog.physicsPath = "undeclared.physics3.json";
        if (kind == 4) catalog.physicsDigest.clear();
        if (kind == 5) catalog.motions["./motion.motion3.json"] = catalog.motions.take("motion.motion3.json");
        if (kind == 6) state.breath.insert("unknown-breath");
        if (kind == 7) state.mouseFollow.insert("unknown-mouse");
        if (kind == 8) catalog.eyeBlink.clear();
        if (kind == 9) catalog.lipSync.clear();
        Plan::Prepared output = original;
        QVERIFY(!Plan::Prepare(path, state, {}, catalog, output, &error)); QVERIFY(!error.isEmpty());
        QCOMPARE(persistent(output.resources.state), persistent(original.resources.state));
        QCOMPARE(output.runtime.physics.suppressedInputs, original.runtime.physics.suppressedInputs);
    }

    void unrelatedAndEmptyCatalogCapabilitiesDoNotCreateMasks()
    {
        QTemporaryDir directory; QString path; ParameterReferenceCatalog catalog; QVERIFY(fixture(directory, path, catalog));
        for (auto* ids : {&catalog.eyeBlink, &catalog.lipSync, &catalog.breath, &catalog.mouseFollow}) {
            ids->insert(QString()); ids->insert("UnrelatedVirtualDriver");
        }
        Plan::Prepared prepared; QString error;
        QVERIFY2(Plan::Prepare(path, {}, {"P"}, catalog, prepared, &error), qPrintable(error));
        QCOMPARE(prepared.runtime.eyeBlink, QSet<QString>{"P"}); QCOMPARE(prepared.runtime.lipSync, QSet<QString>{"P"});
        QCOMPARE(prepared.runtime.breath, QSet<QString>{"P"}); QCOMPARE(prepared.runtime.mouseFollow, QSet<QString>{"P"});
        QVERIFY2(Plan::Prepare(path, {}, {"not-referenced"}, catalog, prepared, &error), qPrintable(error));
        QVERIFY(prepared.resources.state.IsEmpty());
        auto invalid = prepared.resources.state;
        Plan::Prepared withMasks; QVERIFY(Plan::Prepare(path, {}, {"P"}, catalog, withMasks, &error));
        invalid = withMasks.resources.state; invalid.breath.insert(QString());
        QVERIFY(!Plan::Prepare(path, invalid, {}, catalog, prepared, &error));
        QVERIFY(prepared.resources.state.IsEmpty());
    }

    void noOpStateIsEmptyWithoutDiscardingParsedSources()
    {
        QTemporaryDir directory; QString path; ParameterReferenceCatalog catalog; QVERIFY(fixture(directory, path, catalog));
        Plan::Prepared prepared; QString error;
        QVERIFY2(Plan::Prepare(path, {}, {"not-referenced"}, catalog, prepared, &error), qPrintable(error));
        QVERIFY(prepared.resources.state.IsEmpty()); QCOMPARE(prepared.resources.resources.size(), 3);
        QCOMPARE(prepared.resources.model.sourceBytes, encode(model()));
        QVERIFY(prepared.runtime.motions.isEmpty()); QVERIFY(prepared.runtime.expressions.isEmpty()); QVERIFY(prepared.runtime.physics.sourceDigest.isEmpty());
        QVERIFY(Plan::Prepare(path, {}, {"BreathOnly"}, catalog, prepared, &error));
        QVERIFY(!prepared.resources.state.IsEmpty()); QCOMPARE(prepared.resources.state.breath, QSet<QString>{"BreathOnly"});
        QCOMPARE(prepared.resources.removals, QStringList{QStringLiteral("呼吸驱动 · BreathOnly")});
        QVERIFY(prepared.runtime.motions.isEmpty()); QVERIFY(prepared.runtime.physics.sourceDigest.isEmpty());
    }

    void emptyPhysicsAndAbsoluteSiblingPathUseNormalizedIdentity()
    {
        QTemporaryDir directory; QVERIFY(directory.isValid()); QVERIFY(QDir(directory.path()).mkdir("nested"));
        const auto path = directory.filePath("nested/model.model3.json");
        const auto expressionPath = directory.filePath("face.exp3.json");
        const QJsonObject root{{"Version", 3}, {"FileReferences", QJsonObject{{"Moc", "model.moc3"}, {"Physics", ""},
            {"Expressions", QJsonArray{QJsonObject{{"Name", "Face"}, {"File", expressionPath}}}}}}};
        QVERIFY(write(path, encode(root))); QVERIFY(write(expressionPath, encode(expression())));
        const auto catalog = catalogFor(path); QCOMPARE(catalog.expressions.firstKey(), QString("../face.exp3.json"));
        Plan::Prepared prepared; QString error;
        QVERIFY2(Plan::Prepare(path, {}, {"P"}, catalog, prepared, &error), qPrintable(error));
        QCOMPARE(prepared.runtime.expressions.firstKey(), QString("../face.exp3.json"));
        QVERIFY(prepared.runtime.physics.sourcePath.isEmpty());
    }

    void originalModelResourcesRemainUnchanged_data()
    {
        QTest::addColumn<QString>("name"); for (const char* name : {"Mark", "Natori", "Rice", "Wanko"}) QTest::newRow(name) << QString::fromLatin1(name);
    }
    void originalModelResourcesRemainUnchanged()
    {
        QFETCH(QString, name);
        const auto path = QString(TEST_RESOURCES) + '/' + name + '/' + name + ".model3.json";
        const auto catalog = catalogFor(path);
        Plan::Prepared prepared; QString error;
        QVERIFY2(Plan::Prepare(path, {}, {"ParamAngleX"}, catalog, prepared, &error), qPrintable(error));
        QCOMPARE(prepared.resources.model.sourceDigest, catalog.modelSettingsDigest);
        QCOMPARE(digest(read(path)), catalog.modelSettingsDigest);
        const QDir directory = QFileInfo(path).absoluteDir();
        for (auto it = prepared.resources.resources.cbegin(); it != prepared.resources.resources.cend(); ++it) {
            QCOMPARE(digest(read(directory.filePath(it.key()))), it->sourceDigest);
            if (prepared.resources.state.resources.contains(it.key())) QVERIFY(!it->references.isEmpty());
        }
    }
};

QTEST_GUILESS_MAIN(ParameterReferencePlanTests)
#include "parameter_reference_plan_tests.moc"
