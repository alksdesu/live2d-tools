#include "live2dstruct/cparameterresourceedits.h"
#include "live2dstruct/cmodelsession.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest>

using Edits = live2dstruct::CParameterResourceEdits;

class ParameterResourceEditsTests : public QObject
{
    Q_OBJECT
    static QByteArray bytes(const QJsonObject& value) { return QJsonDocument(value).toJson(QJsonDocument::Indented); }
    static bool write(const QString& path, const QByteArray& value)
    {
        QFile file(path);
        return file.open(QIODevice::WriteOnly) && file.write(value) == value.size();
    }
    static QByteArray read(const QString& path)
    {
        QFile file(path);
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    }
    static QByteArray model()
    {
        return bytes({{"Version", 3}, {"FileReferences", QJsonObject{{"Moc", "model.moc3"},
            {"Expressions", QJsonArray{QJsonObject{{"Name", "Face"}, {"File", "../face.exp3.json"}},
                                       QJsonObject{{"Name", "Alias"}, {"File", "./../face.exp3.json"}}}}}},
            {"Groups", QJsonArray{QJsonObject{{"Name", "EyeBlink"}, {"Target", "Parameter"}, {"Ids", QJsonArray{"A", "B"}}}}}});
    }
    static QByteArray expression()
    {
        return bytes({{"Parameters", QJsonArray{QJsonObject{{"Id", "A"}, {"Value", 1}}, QJsonObject{{"Id", "B"}, {"Value", .5}}}}, {"FadeInTime", 2}});
    }
    static bool fixture(QTemporaryDir& directory, QString& path)
    {
        path = directory.filePath("nested/model.model3.json");
        return directory.isValid() && QDir(directory.path()).mkdir("nested") && write(path, model()) &&
               write(directory.filePath("face.exp3.json"), expression());
    }
    static QJsonObject json(const Edits::State& state)
    {
        QJsonObject result;
        Edits::ToJson(state, result);
        return result;
    }

private slots:
    void deletionIsPreparedWithoutWritingSources()
    {
        QTemporaryDir directory;
        QString path;
        QVERIFY(fixture(directory, path));
        Edits::Prepared result;
        QString error;
        QVERIFY2(Edits::Prepare(path, {}, {"A"}, result, &error), qPrintable(error));
        QCOMPARE(result.state.modelPath, path);
        QCOMPARE(result.state.modelSuppressedIds, QSet<QString>{"A"});
        QCOMPARE(result.state.resources.size(), 1);
        QCOMPARE(result.state.resources.firstKey(), QStringLiteral("../face.exp3.json"));
        QCOMPARE(result.resources.first().parameterRows, (QMap<int, QString>{{0, "A"}}));
        QCOMPARE(result.removals.size(), 2);
        QCOMPARE(read(path), model());
        QCOMPARE(read(directory.filePath("face.exp3.json")), expression());
        Edits::Prepared repeated;
        QVERIFY(Edits::Prepare(path, result.state, {"A"}, repeated, &error));
        QCOMPARE(json(repeated.state), json(result.state));
        QVERIFY(repeated.removals.isEmpty());
    }

    void successiveDeletesKeepOriginalRowsAndRoundTrip()
    {
        QTemporaryDir directory;
        QString path;
        QVERIFY(fixture(directory, path));
        Edits::Prepared first, second;
        QVERIFY(Edits::Prepare(path, {}, {"A"}, first));
        first.state.breath.insert("A");
        first.state.mouseFollow.insert("A");
        QVERIFY(Edits::Prepare(path, first.state, {"B"}, second));
        QCOMPARE(second.resources.first().parameterRows, (QMap<int, QString>{{0, "A"}, {1, "B"}}));
        QCOMPARE(second.state.resources.first().source, expression());
        QCOMPARE(second.state.breath, first.state.breath);
        Edits::State restored;
        QString error;
        const auto encoded = json(second.state);
        QVERIFY2(Edits::FromJson(encoded, restored, &error), qPrintable(error));
        QCOMPARE(json(restored), encoded);
        Edits::Prepared prepared;
        QVERIFY(Edits::Prepare(path, restored, {}, prepared, &error));
        QCOMPARE(prepared.resources.first().effectiveBytes, second.resources.first().effectiveBytes);
        Edits::Prepared undone;
        QVERIFY(Edits::Prepare(path, first.state, {}, undone, &error));
        QCOMPARE(undone.resources.first().parameterRows, (QMap<int, QString>{{0, "A"}}));
        Edits::Prepared cleared;
        QVERIFY(Edits::Prepare(path, {}, {}, cleared, &error));
        QVERIFY(cleared.state.resources.isEmpty());
        QCOMPARE(cleared.resources.first().effectiveBytes, expression());
    }

    void registeredUserDataUpdatesOnlyThePreparedModelSource()
    {
        QTemporaryDir directory; QString path; QVERIFY(fixture(directory, path));
        Edits::Prepared original; QVERIFY(Edits::Prepare(path, {}, {"A"}, original));
        const auto preserved = json(original.state);
        auto root = QJsonDocument::fromJson(model()).object(); auto files = root["FileReferences"].toObject();
        files["UserData"] = "../labels.userdata3.json"; root["FileReferences"] = files;
        const auto registered = bytes(root); QVERIFY(write(path, registered));
        Edits::Prepared result; QString error;
        QVERIFY2(Edits::Prepare(path, original.state, {"B"}, result, &error), qPrintable(error));
        QCOMPARE(result.state.modelSource, registered); QCOMPARE(json(original.state), preserved);
        QCOMPARE(result.state.resources.first().source, expression());
        QCOMPARE(result.resources.first().parameterRows, (QMap<int, QString>{{0, "A"}, {1, "B"}}));
        const auto accepted = json(result.state);
        root["UnknownExtension"] = "external"; QVERIFY(write(path, bytes(root)));
        QVERIFY(!Edits::Prepare(path, original.state, {}, result, &error)); QCOMPARE(json(result.state), accepted);
        QVERIFY(write(path, registered)); QVERIFY(write(directory.filePath("face.exp3.json"), expression() + " "));
        QVERIFY(!Edits::Prepare(path, original.state, {}, result, &error)); QCOMPARE(json(result.state), accepted);
        QVERIFY(error.contains(QStringLiteral("关联资源已改变")));
        QVERIFY(write(directory.filePath("face.exp3.json"), expression()));
        auto changedFiles = files; changedFiles["UserData"] = "../retarget.json";
        root.remove("UnknownExtension"); root["FileReferences"] = changedFiles; QVERIFY(write(path, bytes(root)));
        QVERIFY(!Edits::Prepare(path, result.state, {}, result, &error)); QCOMPARE(json(result.state), accepted);
    }

    void sourceDriftAndMissingResourcesRejectAtomically_data()
    {
        QTest::addColumn<int>("kind");
        for (int i = 0; i < 4; ++i) QTest::newRow(qPrintable(QString::number(i))) << i;
    }
    void sourceDriftAndMissingResourcesRejectAtomically()
    {
        QFETCH(int, kind);
        QTemporaryDir directory;
        QString path;
        QVERIFY(fixture(directory, path));
        Edits::Prepared original;
        QVERIFY(Edits::Prepare(path, {}, {"A"}, original));
        if (kind == 0) QVERIFY(write(path, model() + " "));
        if (kind == 1) QVERIFY(write(directory.filePath("face.exp3.json"), expression() + " "));
        if (kind == 2) QVERIFY(QFile::remove(directory.filePath("face.exp3.json")));
        if (kind == 3) {
            QFile file(directory.filePath("face.exp3.json"));
            QVERIFY(file.open(QIODevice::WriteOnly));
            QVERIFY(file.resize(16 * 1024 * 1024 + 1));
        }
        Edits::Prepared result = original;
        QString error;
        QVERIFY(!Edits::Prepare(path, original.state, {"B"}, result, &error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(json(result.state), json(original.state));
        QCOMPARE(result.resources.first().effectiveBytes, original.resources.first().effectiveBytes);
    }

    void malformedPersistenceNeverPublishesPartialState_data()
    {
        QTest::addColumn<int>("kind");
        for (int i = 0; i < 12; ++i) QTest::newRow(qPrintable(QString::number(i))) << i;
    }
    void malformedPersistenceNeverPublishesPartialState()
    {
        QFETCH(int, kind);
        QTemporaryDir directory;
        QString path;
        QVERIFY(fixture(directory, path));
        Edits::Prepared prepared;
        QVERIFY(Edits::Prepare(path, {}, {"A"}, prepared));
        auto bad = json(prepared.state);
        auto resources = bad["resources"].toObject();
        auto entry = resources["../face.exp3.json"].toObject();
        if (kind == 0) bad["version"] = 2;
        if (kind == 1) bad["modelSource"] = "not base64";
        if (kind == 2) bad["modelPath"] = "../model.model3.json";
        if (kind == 3) bad["modelSuppressedIds"] = QJsonArray{"A", "A"};
        if (kind == 4) bad["modelSuppressedIds"] = QJsonArray{"Absent"};
        if (kind == 5) bad["breath"] = QJsonArray{QString(QChar::Null)};
        if (kind >= 6) {
            if (kind == 6) entry["kind"] = "timeline";
            if (kind == 7) entry["source"] = QString::fromLatin1(QByteArray("{}").toBase64());
            if (kind == 8) entry["suppressedIds"] = QJsonArray{"Absent"};
            if (kind == 9) entry["suppressedIds"] = QJsonArray{};
            resources["../face.exp3.json"] = entry;
            if (kind == 10 || kind == 11) {
                resources = QJsonObject{{kind == 10 ? "/tmp/face.exp3.json" : "./../face.exp3.json", entry}};
            }
            bad["resources"] = resources;
        }
        Edits::State output = prepared.state;
        QString error;
        QVERIFY(!Edits::FromJson(bad, output, &error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(json(output), json(prepared.state));
    }

    void unrelatedIdsDoNotCreateResourceMasks()
    {
        QTemporaryDir directory;
        QString path;
        QVERIFY(fixture(directory, path));
        Edits::Prepared result;
        QVERIFY(Edits::Prepare(path, {}, {"AB"}, result));
        QVERIFY(result.state.resources.isEmpty());
        QVERIFY(result.state.modelSuppressedIds.isEmpty());
        QVERIFY(result.removals.isEmpty());
    }

    void emptyPhysicsAndAbsoluteResourcePathsMatchRuntime()
    {
        QTemporaryDir directory;
        QString path;
        QVERIFY(fixture(directory, path));
        auto source = QJsonDocument::fromJson(model()).object();
        auto files = source["FileReferences"].toObject();
        files["Physics"] = "";
        auto expressions = files["Expressions"].toArray();
        auto expression = expressions[0].toObject();
        expression["File"] = directory.filePath("face.exp3.json");
        expressions[0] = expression;
        files["Expressions"] = expressions;
        source["FileReferences"] = files;
        QVERIFY(write(path, bytes(source)));
        Edits::Prepared prepared;
        QString error;
        QVERIFY2(Edits::Prepare(path, {}, {"A"}, prepared, &error), qPrintable(error));
        QCOMPARE(prepared.state.resources.size(), 1);
        QCOMPARE(prepared.state.resources.firstKey(), QStringLiteral("../face.exp3.json"));
        Edits::State restored;
        QVERIFY2(Edits::FromJson(json(prepared.state), restored, &error), qPrintable(error));
        QVERIFY(Edits::Prepare(path, restored, {}, prepared, &error));
    }

    void sessionStoresReferenceSourcesAndPhysicsDraftWithAnExplicitVersion()
    {
        QTemporaryDir directory;
        QString path;
        QVERIFY(fixture(directory, path));
        Edits::Prepared prepared;
        QVERIFY(Edits::Prepare(path, {}, {"A"}, prepared));
        live2dstruct::CModelSession session;
        session.parameterResources = prepared.state;
        session.physicsDraft = live2dstruct::CModelSession::PhysicsDraft{directory.filePath("physics.json"),
            QJsonObject{{"PhysicsSettings", QJsonArray{}}}, true};
        const auto encoded = session.ToJson();
        QCOMPARE(QJsonDocument::fromJson(encoded).object()["version"].toInt(), 3);
        live2dstruct::CModelSession restored;
        QString error;
        QVERIFY2(live2dstruct::CModelSession::FromJson(encoded, &restored, &error), qPrintable(error));
        QCOMPARE(restored.version, 3);
        QCOMPARE(json(restored.parameterResources), json(session.parameterResources));
        QVERIFY(restored.physicsDraft);
        QVERIFY(restored.physicsDraft->dirty);
        QCOMPARE(restored.physicsDraft->document, session.physicsDraft->document);
        QCOMPARE(restored.physicsDraft->sourcePath, session.physicsDraft->sourcePath);
        const auto saved = restored.ToJson();
        auto invalid = QJsonDocument::fromJson(encoded).object();
        invalid["version"] = 2;
        QVERIFY(!live2dstruct::CModelSession::FromJson(bytes(invalid), &restored, &error));
        QCOMPARE(restored.ToJson(), saved);
        invalid["version"] = 3;
        invalid["physicsDraft"] = QJsonObject{{"sourcePath", "physics.json"}, {"document", QJsonObject{}}, {"dirty", true}};
        QVERIFY(!live2dstruct::CModelSession::FromJson(bytes(invalid), &restored, &error));
        QCOMPARE(restored.ToJson(), saved);
        for (int version : {1, 2}) {
            QVERIFY(live2dstruct::CModelSession::FromJson(bytes({{"version", version}}), &restored, &error));
            QVERIFY(restored.parameterResources.IsEmpty());
            QVERIFY(!restored.physicsDraft);
            QCOMPARE(restored.version, version);
        }
    }

    void restoredResourcesCannotRetainDeletedSourceParameter_data()
    {
        QTest::addColumn<int>("channel");
        QTest::newRow("parameter-group") << 0;
        QTest::newRow("sdk-group") << 1;
        QTest::newRow("expression") << 2;
        QTest::newRow("motion") << 3;
        QTest::newRow("physics-input") << 4;
        QTest::newRow("physics-output") << 5;
    }

    void restoredResourcesCannotRetainDeletedSourceParameter()
    {
        QFETCH(int, channel);
        using namespace live2dstruct;
        CModelDocument original;
        QString error;
        QVERIFY2(original.Load(QString(TEST_RESOURCES) + "/Mark/Mark.moc3", &error), qPrintable(error));
        CModelDocument working = original;
        CModelEdit deletion;
        QVERIFY2(working.MakeDeleteParameterEdit({QStringLiteral("ParamAngleX"), 0}, deletion, &error), qPrintable(error));
        QVERIFY(working.ExecuteEdit(deletion));
        CModelSession session;
        session.version = 3;
        session.sourceDigest = original.SourceDigest();
        session.structure = working.ParameterStructure();
        auto& resources = session.parameterResources;
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        resources.modelPath = directory.filePath("model.model3.json");
        resources.modelSuppressedIds = {QStringLiteral("VirtualRemoved")};
        QJsonObject files{{"Moc", "model.moc3"}};
        QJsonArray ids{"VirtualRemoved"};
        if (channel < 2) ids.append("ParamAngleX");
        QJsonObject group{{"Name", channel == 1 ? "EyeBlink" : "Parameters"}, {"Ids", ids}};
        if (channel != 1) group.insert("Target", "Parameter");
        const QJsonArray groups{group, QJsonObject{{"Name", "Parts"}, {"Target", "Part"}, {"Ids", QJsonArray{"ParamAngleX"}}}};
        QString resourcePath;
        if (channel >= 2) {
            resourcePath = channel == 2 ? "face.exp3.json" : channel == 3 ? "walk.motion3.json" : "rig.physics3.json";
            Edits::Resource resource;
            resource.suppressedIds = {QStringLiteral("VirtualRemoved")};
            if (channel == 2) {
                resource.kind = Edits::Kind::Expression;
                files.insert("Expressions", QJsonArray{QJsonObject{{"Name", "Face"}, {"File", resourcePath}}});
                resource.source = bytes({{"Parameters", QJsonArray{
                    QJsonObject{{"Id", "VirtualRemoved"}, {"Value", .2}},
                    QJsonObject{{"Id", "ParamAngleX"}, {"Value", .5}},
                    QJsonObject{{"Id", "VirtualKept"}, {"Value", .7}}}}});
            } else if (channel == 3) {
                resource.kind = Edits::Kind::Motion;
                files.insert("Motions", QJsonObject{{"Idle", QJsonArray{QJsonObject{{"File", resourcePath}}}}});
                QJsonArray curves;
                for (const char* id : {"VirtualRemoved", "ParamAngleX", "VirtualKept"})
                    curves.append(QJsonObject{{"Target", "Parameter"}, {"Id", id}, {"Segments", QJsonArray{0, 0, 0, 1, 1}}});
                curves.append(QJsonObject{{"Target", "PartOpacity"}, {"Id", "ParamAngleX"}, {"Segments", QJsonArray{0, .5, 0, 1, .5}}});
                resource.source = bytes({{"Version", 3}, {"Meta", QJsonObject{{"Duration", 1}, {"Fps", 30}}}, {"Curves", curves}});
            } else {
                resource.kind = Edits::Kind::Physics;
                files.insert("Physics", resourcePath);
                QJsonArray inputs, outputs;
                for (const char* id : {"VirtualRemoved", "ParamAngleX", "VirtualKept"}) {
                    const QJsonObject endpoint{{"Target", "Parameter"}, {"Id", id}};
                    if (channel == 4) inputs.append(QJsonObject{{"Source", endpoint}});
                    else outputs.append(QJsonObject{{"Destination", endpoint}});
                }
                resource.source = bytes({{"PhysicsSettings", QJsonArray{QJsonObject{{"Input", inputs}, {"Output", outputs}}}}});
            }
            resources.resources.insert(resourcePath, resource);
        }
        resources.modelSource = bytes({{"Version", 3}, {"FileReferences", files}, {"Groups", groups}});
        QVERIFY2(Edits::Validate(resources, &error), qPrintable(error));
        CModelSession decoded;
        QVERIFY2(CModelSession::FromJson(session.ToJson(), &decoded, &error), qPrintable(error));
        CModelDocument result = original;
        const auto mesh = result.Drawables().first();
        QVERIFY(result.ExecuteEdit({mesh.id, "artmesh", CModelEdit::Opacity, result.DrawableOpacity(mesh), .3f, -1, false}));
        QVERIFY(result.ExecuteEdit({mesh.id, "artmesh", CModelEdit::Opacity, .3f, .4f, -1, false}));
        QVERIFY(result.UndoEdit());
        QVERIFY(result.CanUndoEdit());
        QVERIFY(result.CanRedoEdit());
        const auto revision = result.Revision();
        const auto previous = result.DrawableOpacityOverrides();
        QVERIFY(!decoded.RestoreDocument(working, result, &error));
        QVERIFY2(error.contains(QStringLiteral("ParamAngleX")), qPrintable(error));
        QVERIFY2(error.contains(channel < 2 ? QStringLiteral("modelSource") : resourcePath), qPrintable(error));
        QCOMPARE(result.Revision(), revision);
        QCOMPARE(result.DrawableOpacityOverrides(), previous);
        QCOMPARE(result.SourceBytes(), original.SourceBytes());
        QVERIFY(result.CanUndoEdit());
        QVERIFY(result.CanRedoEdit());
        QVERIFY(result.RedoEdit());
        QCOMPARE(result.DrawableOpacity(mesh), .4f);
        if (channel < 2) session.parameterResources.modelSuppressedIds.insert(QStringLiteral("ParamAngleX"));
        else session.parameterResources.resources[resourcePath].suppressedIds.insert(QStringLiteral("ParamAngleX"));
        QVERIFY2(CModelSession::FromJson(session.ToJson(), &decoded, &error), qPrintable(error));
        QVERIFY2(decoded.RestoreDocument(working, result, &error), qPrintable(error));
        QVERIFY(result.ParameterStructure() == working.ParameterStructure());
        QVERIFY(!result.CanUndoEdit());
        QVERIFY(!result.CanRedoEdit());
        QVERIFY(!QFileInfo::exists(resources.modelPath));
        if (!resourcePath.isEmpty()) QVERIFY(!QFileInfo::exists(directory.filePath(resourcePath)));
    }

    void archivedVirtualReferenceRemainsValidAfterWorkingParameterRemoval()
    {
        using namespace live2dstruct;
        CModelDocument original;
        QString error;
        QVERIFY(original.Load(QString(TEST_RESOURCES) + "/Mark/Mark.moc3", &error));
        CModelDocument working = original;
        CModelEdit deletion;
        QVERIFY2(working.MakeDeleteParameterEdit({QStringLiteral("ParamAngleX"), 0}, deletion, &error), qPrintable(error));
        QVERIFY(working.ExecuteEdit(deletion));
        CModelSession session;
        session.version = 3;
        session.structure = working.ParameterStructure();
        CMocParameterStructureBuilder::CreateRequest create;
        create.parameter.id = QStringLiteral("SessionOnlyParameter");
        CModelEdit addition;
        QVERIFY2(working.MakeCreateParameterEdit(create, {}, addition, &error), qPrintable(error));
        QVERIFY(working.ExecuteEdit(addition));
        auto& resources = session.parameterResources;
        resources.modelPath = QStringLiteral("/unread/model.model3.json");
        resources.modelSource = bytes({{"FileReferences", QJsonObject{
            {"Expressions", QJsonArray{QJsonObject{{"Name", "Face"}, {"File", "face.exp3.json"}}}}}},
            {"Groups", QJsonArray{QJsonObject{{"Name", "Parameters"}, {"Target", "Parameter"}, {"Ids", QJsonArray{create.parameter.id}}}}}});
        resources.resources.insert(QStringLiteral("face.exp3.json"), {Edits::Kind::Expression,
            bytes({{"Parameters", QJsonArray{QJsonObject{{"Id", "VirtualRemoved"}, {"Value", .2}},
                QJsonObject{{"Id", create.parameter.id}, {"Value", .7}}}}}), {QStringLiteral("VirtualRemoved")}});
        CModelSession decoded;
        QVERIFY2(CModelSession::FromJson(session.ToJson(), &decoded, &error), qPrintable(error));
        CModelDocument result;
        QVERIFY2(decoded.RestoreDocument(working, result, &error), qPrintable(error));
        QCOMPARE(result.Parameters().size(), original.Parameters().size() - 1);
        for (const auto& parameter : result.Parameters()) QVERIFY(parameter.id != create.parameter.id);
        QCOMPARE(result.SourceBytes(), original.SourceBytes());
    }

    void suppliedModelsPrepareWithoutChangingAnyBytes_data()
    {
        QTest::addColumn<QString>("name");
        for (const auto& name : {"Mark", "Natori", "Rice", "Wanko"}) QTest::newRow(name) << QString::fromLatin1(name);
    }
    void suppliedModelsPrepareWithoutChangingAnyBytes()
    {
        QFETCH(QString, name);
        const auto path = QStringLiteral(TEST_RESOURCES) + '/' + name + '/' + name + ".model3.json";
        Edits::Prepared result;
        QString error;
        QVERIFY2(Edits::Prepare(path, {}, {"ParamAngleX"}, result, &error), qPrintable(error));
        QVERIFY(!result.resources.isEmpty());
        QVERIFY(Edits::Validate(result.state, &error));
        QCOMPARE(read(path), result.state.modelSource);
        for (auto it = result.resources.cbegin(); it != result.resources.cend(); ++it)
            QCOMPARE(read(QFileInfo(path).dir().filePath(it.key())), it->sourceBytes);
    }
};

QTEST_APPLESS_MAIN(ParameterResourceEditsTests)
#include "parameter_resource_edits_tests.moc"
