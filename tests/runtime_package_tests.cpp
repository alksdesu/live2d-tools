#include "live2dstruct/cruntimepackage.h"
#include "live2dstruct/cparameterresourceedits.h"
#include "std/cmodeljson.h"
#include "std/cmodelresourcepath.h"
#include "std/cexportcontrol.h"
#include <QtTest>
#include <QCryptographicHash>
#include <QTemporaryDir>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>

using namespace live2dstruct;
namespace {
QByteArray read(const QString& path) { QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray(); }
bool write(const QString& path, const QByteArray& bytes) { QFile file(path); return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(); }
QByteArray digest(const QByteArray& bytes) { return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256); }
QByteArray json(const QJsonObject& root) { return QJsonDocument(root).toJson(QJsonDocument::Indented); }
QByteArray entry(const CRuntimePackage::Prepared& prepared, const QString& name)
{
    for (const auto& file : prepared.entries) if (file.relativePath == name) return file.bytes;
    return {};
}
CRuntimePackage::Request request(const QString& path)
{
    CRuntimePackage::Request result; result.modelJsonPath = path;
    const auto bytes = read(path); result.modelSettingsDigest = digest(bytes);
    const auto files = QJsonDocument::fromJson(bytes).object()["FileReferences"].toObject();
    result.mocBytes = read(CModelResourcePath::Resolve(QFileInfo(path).absolutePath(), files["Moc"].toString()));
    result.sourceMocDigest = digest(result.mocBytes); return result;
}
struct Fixture {
    QTemporaryDir directory;
    QString path;
    QJsonObject root;
    CModelDocument document;
    Fixture()
    {
        const auto home = QString(TEST_RESOURCES) + "/Mark";
        const auto files = QJsonDocument::fromJson(read(home + "/Mark.model3.json")).object()["FileReferences"].toObject();
        path = directory.filePath(QStringLiteral("模型.model3.json"));
        root = {{"Version", 3}, {"Layout", QJsonObject{{"CenterX", 0}}}, {"Custom", "kept"},
            {"FileReferences", QJsonObject{{"Moc", home + "/" + files["Moc"].toString()},
                {"Textures", QJsonArray{home + "/" + files["Textures"].toArray().first().toString()}}}}};
        document.LoadBytes(read(home + "/" + files["Moc"].toString()), {});
        save();
    }
    bool save() const { return write(path, json(root)); }
    void resource(const QString& field, const QString& name, const QByteArray& bytes)
    {
        write(directory.filePath(name), bytes); auto files = root["FileReferences"].toObject(); files[field] = name; root["FileReferences"] = files; save();
    }
};
}
class RuntimePackageTests : public QObject
{
    Q_OBJECT
private slots:
    void newlySavedUserDataRegistrationKeepsOtherSourceChecks_data()
    {
        QTest::addColumn<int>("index");
        QTest::newRow("registered") << 0; QTest::newRow("unrelated-change") << 1;
        QTest::newRow("missing-loaded-digest") << 2; QTest::newRow("wrong-loaded-digest") << 3;
        QTest::newRow("wrong-registration-path") << 4;
    }
    void newlySavedUserDataRegistrationKeepsOtherSourceChecks()
    {
        QFETCH(int, index); Fixture fixture; auto input = request(fixture.path);
        input.loadedModelSettingsBytes = read(fixture.path);
        const auto data = json({{"Version", 3}, {"UserData", QJsonArray{QJsonObject{{"Target", "ArtMesh"}, {"Id", fixture.document.Drawables().first().id}, {"Value", "saved"}}}}});
        fixture.resource("UserData", "created.userdata3.json", data);
        input.registeredUserDataPath = fixture.directory.filePath("created.userdata3.json");
        input.expectedSourceDigests[input.registeredUserDataPath] = digest(data); input.userData = data;
        if (index == 1) { fixture.root["Custom"] = "external change"; QVERIFY(fixture.save()); }
        if (index == 2) input.expectedSourceDigests.clear();
        if (index == 3) input.expectedSourceDigests[input.registeredUserDataPath] = QByteArray(32, 'x');
        if (index == 4) input.registeredUserDataPath = fixture.directory.filePath("other.json");
        CRuntimePackage::Prepared prepared; QString error;
        QCOMPARE(CRuntimePackage::Prepare(input, prepared, nullptr, &error), index == 0);
        if (index != 0) { QVERIFY(!error.isEmpty()); QVERIFY(prepared.entries.isEmpty()); }
        else QCOMPARE(entry(prepared, prepared.model["FileReferences"].toObject()["UserData"].toString()), data);
    }
    void updatedLoadedManifestCanPackageOldReferenceStateWithoutChangingHistory()
    {
        Fixture fixture; const QString id = "ParamAngleX";
        fixture.root["Groups"] = QJsonArray{QJsonObject{{"Name", "EyeBlink"}, {"Target", "Parameter"}, {"Ids", QJsonArray{id}}}};
        QVERIFY(fixture.save());
        CParameterResourceEdits::Prepared references; QString error;
        QVERIFY(CParameterResourceEdits::Prepare(fixture.path, {}, {id}, references, &error));
        const auto oldSource = references.state.modelSource;
        const auto data = json({{"Version", 3}, {"UserData", QJsonArray{QJsonObject{{"Target", "ArtMesh"},
            {"Id", fixture.document.Drawables().first().id}, {"Value", "saved"}}}}});
        fixture.resource("UserData", "created.userdata3.json", data);
        auto input = request(fixture.path); input.loadedModelSettingsBytes = read(fixture.path); input.references = references.state;
        CMocParameterStructureBuilder::Result deleted;
        QVERIFY(CMocParameterStructureBuilder::Delete(input.mocBytes, {id, 0}, deleted, &error)); input.mocBytes = deleted.bytes;
        CRuntimePackage::Prepared prepared;
        QVERIFY2(CRuntimePackage::Prepare(input, prepared, nullptr, &error), qPrintable(error));
        QCOMPARE(input.references.modelSource, oldSource);
        QCOMPARE(prepared.model["Groups"].toArray().first().toObject()["Ids"].toArray(), QJsonArray{});
        QCOMPARE(entry(prepared, prepared.model["FileReferences"].toObject()["UserData"].toString()), data);
        const auto acceptedModel = prepared.model;
        QMap<QString, QByteArray> acceptedEntries;
        for (const auto& file : prepared.entries) acceptedEntries.insert(file.relativePath, file.bytes);
        fixture.root["Custom"] = "external change"; QVERIFY(fixture.save());
        input.loadedModelSettingsBytes = read(fixture.path); input.modelSettingsDigest = digest(input.loadedModelSettingsBytes);
        QVERIFY(!CRuntimePackage::Prepare(input, prepared, nullptr, &error));
        QVERIFY(error.contains(QStringLiteral("引用状态"))); QCOMPARE(prepared.model, acceptedModel); QCOMPARE(prepared.entries.size(), acceptedEntries.size());
        for (const auto& file : prepared.entries) QCOMPARE(file.bytes, acceptedEntries.value(file.relativePath));
    }

    void optionalEmptyResourcesRemainPortable_data()
    {
        QTest::addColumn<int>("index");
        QTest::newRow("omitted") << 0; QTest::newRow("empty-list") << 1; QTest::newRow("empty-slot") << 2;
    }
    void optionalEmptyResourcesRemainPortable()
    {
        QFETCH(int, index); Fixture fixture;
        auto files = fixture.root["FileReferences"].toObject();
        if (index == 0) files.remove("Textures");
        else files["Textures"] = index == 1 ? QJsonArray{} : QJsonArray{""};
        files["Expressions"] = QJsonArray{QJsonObject{{"Name", "empty"}, {"File", ""}}};
        files["Motions"] = QJsonObject{{"Empty", QJsonArray{QJsonObject{{"File", ""}, {"Sound", ""}}}}};
        fixture.root["FileReferences"] = files; QVERIFY(fixture.save());
        CRuntimePackage::Prepared prepared; QString error;
        QVERIFY2(CRuntimePackage::Prepare(request(fixture.path), prepared, nullptr, &error), qPrintable(error));
        const auto result = prepared.model["FileReferences"].toObject();
        QCOMPARE(result.value("Textures"), files.value("Textures")); QCOMPARE(result["Expressions"], files["Expressions"]); QCOMPARE(result["Motions"], files["Motions"]);
    }
    void deletedParametersAreRemovedFromLinkedResources()
    {
        Fixture fixture; const QString id = "ParamAngleX";
        fixture.resource("DisplayInfo", "names.json", json({{"Version", 3}, {"Parameters", QJsonArray{QJsonObject{{"Id", id}, {"Name", "removed"}}}},
            {"Parts", QJsonArray{QJsonObject{{"Id", id}, {"Name", "part retained"}}}}, {"CombinedParameters", QJsonArray{QJsonArray{id, "ParamAngleY"}}}}));
        const auto expression = json({{"Type", "Live2D Expression"}, {"Parameters", QJsonArray{QJsonObject{{"Id", id}, {"Value", 1}, {"Blend", "Add"}}, QJsonObject{{"Id", "HostVirtual"}, {"Value", 2}, {"Blend", "Add"}}}}});
        QVERIFY(write(fixture.directory.filePath("face.exp3.json"), expression));
        QVERIFY(write(fixture.directory.filePath("move.motion3.json"), read(QString(TEST_RESOURCES) + "/Mark/motions/mark_m01.motion3.json")));
        auto files = fixture.root["FileReferences"].toObject();
        files["Expressions"] = QJsonArray{QJsonObject{{"Name", "face"}, {"File", "face.exp3.json"}}};
        files["Motions"] = QJsonObject{{"Idle", QJsonArray{QJsonObject{{"File", "move.motion3.json"}}}}};
        fixture.root["FileReferences"] = files; fixture.root["Groups"] = QJsonArray{QJsonObject{{"Target", "Parameter"}, {"Name", "LipSync"}, {"Ids", QJsonArray{id}}}}; QVERIFY(fixture.save());
        auto input = request(fixture.path); CMocParameterStructureBuilder::Result deleted; QString error;
        QVERIFY2(CMocParameterStructureBuilder::Delete(input.mocBytes, {id, 0}, deleted, &error), qPrintable(error)); input.mocBytes = deleted.bytes;
        CParameterResourceEdits::Prepared references;
        QVERIFY2(CParameterResourceEdits::Prepare(fixture.path, {}, {id}, references, &error), qPrintable(error)); input.references = references.state;
        CRuntimePackage::Prepared prepared;
        QVERIFY2(CRuntimePackage::Prepare(input, prepared, nullptr, &error), qPrintable(error));
        const auto result = prepared.model["FileReferences"].toObject();
        const auto names = QJsonDocument::fromJson(entry(prepared, result["DisplayInfo"].toString())).object();
        QVERIFY(names["Parameters"].toArray().isEmpty()); QVERIFY(names["CombinedParameters"].toArray().isEmpty()); QCOMPARE(names["Parts"].toArray().size(), 1);
        const auto face = QJsonDocument::fromJson(entry(prepared, result["Expressions"].toArray().first().toObject()["File"].toString())).object();
        QCOMPARE(face["Parameters"].toArray().size(), 1); QCOMPARE(face["Parameters"].toArray().first().toObject()["Id"].toString(), "HostVirtual");
        const auto motion = QJsonDocument::fromJson(entry(prepared, result["Motions"].toObject()["Idle"].toArray().first().toObject()["File"].toString())).object();
        for (const auto& curve : motion["Curves"].toArray()) QVERIFY(curve.toObject()["Target"] != "Parameter" || curve.toObject()["Id"] != id);
        QCOMPARE(read(fixture.directory.filePath("face.exp3.json")), expression);
    }
    void motionSyncChecksModelParameterNamespace_data()
    {
        QTest::addColumn<int>("index");
        QTest::newRow("cubism-parameter") << 0; QTest::newRow("mapping-target") << 1; QTest::newRow("audio-namespace") << 2;
    }
    void motionSyncChecksModelParameterNamespace()
    {
        QFETCH(int, index); Fixture fixture;
        QJsonObject setting{{"Id", "Setting1"}, {"CubismParameters", QJsonArray{}}, {"Mappings", QJsonArray{}}, {"AudioParameters", QJsonArray{QJsonObject{{"Id", "ParamAngleX"}}}}};
        if (index == 0) setting["CubismParameters"] = QJsonArray{QJsonObject{{"Id", "ParamAngleX"}}};
        if (index == 1) setting["Mappings"] = QJsonArray{QJsonObject{{"Id", "A"}, {"Targets", QJsonArray{QJsonObject{{"Id", "ParamAngleX"}, {"Value", 1}}}}}};
        if (index == 2) setting["Mappings"] = QJsonArray{QJsonObject{{"Id", "ParamAngleX"}, {"Targets", QJsonArray{QJsonObject{{"Id", "ParamAngleY"}, {"Value", 1}}}}}};
        fixture.resource("MotionSync", "sync.json", json({{"Version", 1}, {"Settings", QJsonArray{setting}}}));
        auto input = request(fixture.path); CMocParameterStructureBuilder::Result deleted; QString error;
        QVERIFY2(CMocParameterStructureBuilder::Delete(input.mocBytes, {"ParamAngleX", 0}, deleted, &error), qPrintable(error)); input.mocBytes = deleted.bytes;
        CRuntimePackage::Prepared prepared;
        QCOMPARE(CRuntimePackage::Prepare(input, prepared, nullptr, &error), index == 2);
        if (index != 2) { QVERIFY(error.contains("MotionSync")); QVERIFY(prepared.entries.isEmpty()); }
    }
    void originalModelsCanMoveWithoutSourcePaths_data()
    {
        QTest::addColumn<QString>("name");
        for (const auto& name : {"Mark", "Natori", "Rice", "Wanko"}) QTest::newRow(name) << QString::fromLatin1(name);
    }
    void originalModelsCanMoveWithoutSourcePaths()
    {
        QFETCH(QString, name); const auto source = QString(TEST_RESOURCES) + "/" + name + "/" + name + ".model3.json";
        auto input = request(source); CRuntimePackage::Prepared prepared; QString error;
        QVERIFY2(CRuntimePackage::Prepare(input, prepared, nullptr, &error), qPrintable(error));
        QVERIFY(prepared.entries.size() > 3); QTemporaryDir temporary;
        CDirectoryExport::Options options; options.protectedPaths = prepared.protectedPaths;
        CDirectoryExport::Result published;
        QVERIFY2(CDirectoryExport::Publish(temporary.filePath("package"), prepared.entries, options, published, nullptr, &error), qPrintable(error));
        QVERIFY(published.committed); QVERIFY(QDir().rename(temporary.filePath("package"), temporary.filePath(QStringLiteral("搬移后的 模型"))));
        CModelJson model; model.Parse(temporary.filePath(QStringLiteral("搬移后的 模型/")) + prepared.modelFile); QVERIFY(model.IsOpen());
        for (const auto& path : model.getAllZipFiles()) { QVERIFY(QFileInfo::exists(path)); QVERIFY(path.startsWith(temporary.path())); }
        CModelDocument loaded; QVERIFY2(loaded.LoadBytes(read(model.getMocFileName()), {}, &error), qPrintable(error));
        for (const auto& captured : prepared.sources) QCOMPARE(digest(read(captured.path)), captured.digest);
        QCOMPARE(loaded.SourceBytes(), input.mocBytes);
    }
    void collectsExtensionsAudioAndDraftsWithoutWritingSource()
    {
        Fixture fixture; QVERIFY(fixture.document.IsLoaded());
        const auto part = fixture.document.Parts().first().id; const auto mesh = fixture.document.Drawables().first().id;
        fixture.resource("Pose", QStringLiteral("姿势.pose3.json"), json({{"Version", 3}, {"Groups", QJsonArray{QJsonArray{QJsonObject{{"Id", part}, {"Custom", 7}}}}}}));
        fixture.resource("UserData", "tags.userdata3.json", json({{"Version", 3}, {"UserData", QJsonArray{QJsonObject{{"Target", "ArtMesh"}, {"Id", mesh}, {"Value", "tag"}}}}}));
        fixture.resource("Physics", "draft.physics3.json", json({{"Version", 3}, {"Meta", QJsonObject{{"VertexCount", 900}, {"Custom", true}}}, {"PhysicsSettings", QJsonArray{}}}));
        fixture.resource("MotionSync", "speech.motionsync3.json", json({{"Version", 1}, {"Settings", QJsonArray{}}, {"Custom", "sync"}}));
        const auto motion = read(QString(TEST_RESOURCES) + "/Mark/motions/mark_m01.motion3.json");
        QVERIFY(!motion.isEmpty()); QVERIFY(write(fixture.directory.filePath("move.motion3.json"), motion));
        QVERIFY(write(fixture.directory.filePath(QStringLiteral("音声.音")), "RIFFfixture"));
        auto files = fixture.root["FileReferences"].toObject();
        files["Motions"] = QJsonObject{{"Edited", QJsonArray{QJsonObject{{"File", "move.motion3.json"}, {"Sound", QStringLiteral("音声.音")}, {"MotionSync", "SettingsName"}, {"FadeInTime", 0.25}}}}};
        files["Expressions"] = QJsonArray{QJsonObject{{"Name", "smile"}, {"File", "smile.exp3.json"}}};
        QVERIFY(write(fixture.directory.filePath("smile.exp3.json"), json({{"Type", "Live2D Expression"}, {"Parameters", QJsonArray{}}})));
        fixture.root["FileReferences"] = files; QVERIFY(fixture.save());
        auto input = request(fixture.path); input.timeline = motion; input.timelineSoundPath = fixture.directory.filePath(QStringLiteral("音声.音"));
        input.physics = json({{"Version", 3}, {"Meta", QJsonObject{{"TotalInputCount", -1}, {"Custom", "draft"}}}, {"PhysicsSettings", QJsonArray{}}});
        CRuntimePackage::Prepared prepared; QString error;
        QVERIFY2(CRuntimePackage::Prepare(input, prepared, nullptr, &error), qPrintable(error));
        const auto result = prepared.model["FileReferences"].toObject(); const auto motions = result["Motions"].toObject();
        QVERIFY(motions.contains("Edited_2")); const auto first = motions["Edited"].toArray().first().toObject();
        QCOMPARE(first["MotionSync"].toString(), "SettingsName"); QCOMPARE(first["FadeInTime"].toDouble(), .25);
        QCOMPARE(first["File"], motions["Edited_2"].toArray().first().toObject()["File"]);
        QVERIFY(first["Sound"].toString().endsWith(".bin")); QCOMPARE(entry(prepared, first["Sound"].toString()), "RIFFfixture");
        QCOMPARE(QJsonDocument::fromJson(entry(prepared, result["Physics"].toString())).object()["Meta"].toObject()["TotalInputCount"].toInt(), 0);
        QCOMPARE(QJsonDocument::fromJson(entry(prepared, result["Physics"].toString())).object()["Meta"].toObject()["Custom"].toString(), "draft");
        QCOMPARE(entry(prepared, result["Pose"].toString()), read(fixture.directory.filePath(QStringLiteral("姿势.pose3.json"))));
        QCOMPARE(prepared.model["Custom"].toString(), "kept");
        for (const auto& source : prepared.sources) QCOMPARE(digest(read(source.path)), source.digest);
    }
    void sparseDisplayInfoPreservesOriginalBytesAndFriendlyNames()
    {
        Fixture fixture; const auto parameter = fixture.document.Parameters().first();
        const auto original = QByteArray("  ") + json({{"Version", 3}, {"Parameters", QJsonArray{QJsonObject{{"Id", parameter.id}, {"Name", "Friendly name"}, {"Custom", true}}}}, {"Other", 1}});
        fixture.resource("DisplayInfo", "names.cdi3.json", original);
        auto input = request(fixture.path); CRuntimePackage::Prepared prepared; QString error;
        QVERIFY2(CRuntimePackage::Prepare(input, prepared, nullptr, &error), qPrintable(error));
        QCOMPARE(entry(prepared, prepared.model["FileReferences"].toObject()["DisplayInfo"].toString()), original);
        CModelParameterMetadata metadata; metadata.displayName = parameter.id; input.parameterMetadata.insert(parameter.id, metadata);
        QVERIFY2(CRuntimePackage::Prepare(input, prepared, nullptr, &error), qPrintable(error));
        QCOMPARE(entry(prepared, prepared.model["FileReferences"].toObject()["DisplayInfo"].toString()), original);
        metadata.displayName = QStringLiteral("新名称"); input.parameterMetadata[parameter.id] = metadata;
        QVERIFY2(CRuntimePackage::Prepare(input, prepared, nullptr, &error), qPrintable(error));
        const auto root = QJsonDocument::fromJson(entry(prepared, prepared.model["FileReferences"].toObject()["DisplayInfo"].toString())).object();
        QCOMPARE(root["Parameters"].toArray().size(), 1); QCOMPARE(root["Parameters"].toArray().first().toObject()["Name"].toString(), QStringLiteral("新名称")); QCOMPARE(root["Other"].toInt(), 1);
    }
    void rejectsInvalidSourcesWithoutChangingOutput_data()
    {
        QTest::addColumn<int>("index"); for (int i = 0; i < 10; ++i) QTest::newRow(qPrintable(QString::number(i))) << i;
    }
    void rejectsInvalidSourcesWithoutChangingOutput()
    {
        QFETCH(int, index); Fixture fixture; auto files = fixture.root["FileReferences"].toObject();
        if (index == 0) files["FutureFile"] = "external.bin";
        if (index == 1) files["Pose"] = "missing.json";
        if (index == 2) { fixture.resource("Pose", "pose.json", json({{"Groups", QJsonArray{QJsonArray{QJsonObject{{"Id", "missing"}}}}}})); files = fixture.root["FileReferences"].toObject(); }
        if (index == 3) { fixture.resource("UserData", "tag.json", json({{"Version", 3}, {"UserData", QJsonArray{QJsonObject{{"Target", "ArtMesh"}, {"Id", "missing"}, {"Value", "tag"}}}}})); files = fixture.root["FileReferences"].toObject(); }
        fixture.root["FileReferences"] = files; QVERIFY(fixture.save()); auto input = request(fixture.path);
        if (index == 4) input.modelSettingsDigest = QByteArray(32, 'x');
        if (index == 5) input.sourceMocDigest = QByteArray(32, 'x');
        if (index == 6) input.expectedSourceDigests[files["Textures"].toArray().first().toString()] = QByteArray(32, 'x');
        if (index == 7) input.expectedSourceDigests[fixture.directory.filePath("not-linked")] = QByteArray(32, 'x');
        if (index == 8) input.maximumFileBytes = 16;
        if (index == 9) input.maximumFiles = 1;
        CRuntimePackage::Prepared prepared; prepared.modelFile = "unchanged"; prepared.totalBytes = 47; QString error;
        QVERIFY(!CRuntimePackage::Prepare(input, prepared, nullptr, &error)); QVERIFY(!error.isEmpty()); QCOMPARE(prepared.modelFile, "unchanged"); QCOMPARE(prepared.totalBytes, 47);
    }
    void cancellationLeavesNoPreparedResultOrWrites()
    {
        Fixture fixture; auto input = request(fixture.path); CExportControl control;
        control.SetHandler([](const CExportControl::Progress&, QString*) { return false; });
        CRuntimePackage::Prepared prepared; QString error;
        QVERIFY(!CRuntimePackage::Prepare(input, prepared, &control, &error)); QVERIFY(!error.isEmpty()); QVERIFY(prepared.entries.isEmpty()); QCOMPARE(read(fixture.path), json(fixture.root));
    }
    void detectsSourceMutationDuringPreparation()
    {
        Fixture fixture; auto input = request(fixture.path); bool changed = false;
        CExportControl control; control.SetHandler([&](const CExportControl::Progress& progress, QString*) {
            if (!changed && progress.detail.startsWith(QStringLiteral("准备 model/"))) { changed = true; write(fixture.path, json(fixture.root) + " "); }
            return true;
        });
        CRuntimePackage::Prepared prepared; QString error;
        QVERIFY(!CRuntimePackage::Prepare(input, prepared, &control, &error)); QVERIFY(changed); QVERIFY(!error.isEmpty()); QVERIFY(prepared.entries.isEmpty());
    }
};
QTEST_GUILESS_MAIN(RuntimePackageTests)
#include "runtime_package_tests.moc"
