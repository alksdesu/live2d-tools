#include "live2dstruct/cruntimepackage.h"
#include "std/cmodeljson.h"
#include <QtTest>
#include <QTemporaryDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QCryptographicHash>
#include <QFile>
#include <QDir>

using namespace live2dstruct;
using Organization = CParameterOrganization;
namespace {
QByteArray read(const QString& path) { QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray(); }
bool write(const QString& path, const QByteArray& bytes) { QFile file(path); return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(); }
QByteArray digest(const QByteArray& bytes) { return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256); }
QByteArray entry(const CRuntimePackage::Prepared& package, const QString& path)
{
    for (const auto& value : package.entries) if (value.relativePath == path) return value.bytes;
    return {};
}
struct Fixture {
    QTemporaryDir directory;
    QJsonArray textures;
    bool ready = false;
    CModelDocument document;
    QString path;
    QStringList ids;
    Fixture() {
        document.Load(QString(TEST_RESOURCES) + "/Mark/Mark.moc3");
        for (const auto& parameter : document.Parameters()) ids.append(parameter.id);
        path = directory.filePath("model.model3.json");
        const QDir resources(QString(TEST_RESOURCES) + QStringLiteral("/Mark"));
        const auto originals = QJsonDocument::fromJson(read(resources.filePath("Mark.model3.json")))
            .object().value("FileReferences").toObject().value("Textures").toArray();
        if (originals.isEmpty()) return;
        for (int index = 0; index < originals.size(); ++index) {
            const auto name = QStringLiteral("texture_%1.png").arg(index);
            if (!QFile::copy(resources.filePath(originals[index].toString()), directory.filePath(name))) return;
            textures.append(name);
        }
        ready = save(false);
    }
    bool save(bool display) {
        QJsonObject files{{"Moc", document.SourcePath()}, {"Textures", textures}};
        if (display) files["DisplayInfo"] = "names.cdi3.json";
        return write(path, QJsonDocument(QJsonObject{{"Version", 3}, {"FileReferences", files}}).toJson());
    }
    bool cdi(const QJsonObject& root) { return save(true) && write(directory.filePath("names.cdi3.json"), QJsonDocument(root).toJson()); }
    CRuntimePackage::Request request() const {
        CRuntimePackage::Request result; result.modelJsonPath = path; result.modelSettingsDigest = digest(read(path));
        result.sourceMocDigest = document.SourceDigest(); document.BuildPreviewMoc(result.mocBytes);
        result.parameterOrganization = document.ParameterOrganization(); return result;
    }
    Organization::State state() const {
        Organization::State result; result.configured = true;
        result.nodes = {{Organization::Node::Folder, "body", {}, "Body", "#112233"},
            {Organization::Node::Parameter, ids[0], "face", {}, "#ABCDEF"},
            {Organization::Node::Parameter, ids[2], {}, {}, {}},
            {Organization::Node::Folder, "face", {}, "Face", "#223344"},
            {Organization::Node::Parameter, ids[1], "face", {}, {}},
            {Organization::Node::Parameter, ids[3], "body", {}, {}}};
        for (int index = 4; index < ids.size(); ++index) result.nodes.append({Organization::Node::Parameter, ids[index], {}, {}, {}});
        result.pairs = {{ids[0], ids[1], true, true}}; return result;
    }
};
}
class ParameterOrganizationExportTests : public QObject
{
    Q_OBJECT
private slots:
    void configuredOrganizationCreatesDisplayInfoAndReopensWithPairs()
    {
        Fixture fixture; QVERIFY(fixture.ready); QVERIFY(fixture.document.IsLoaded()); QVERIFY(fixture.document.SetParameterOrganization(fixture.state()));
        auto input = fixture.request(); CRuntimePackage::Prepared package; QString error;
        QVERIFY2(CRuntimePackage::Prepare(input, package, nullptr, &error), qPrintable(error));
        const auto files = package.model["FileReferences"].toObject(); const auto root = QJsonDocument::fromJson(entry(package, files["DisplayInfo"].toString())).object();
        const auto parameters = root["Parameters"].toArray(), groups = root["ParameterGroups"].toArray();
        QCOMPARE(parameters.size(), fixture.ids.size()); QCOMPARE(groups.size(), 2);
        QCOMPARE(parameters[0].toObject()["Id"].toString(), fixture.ids[3]); QCOMPARE(parameters[0].toObject()["GroupId"].toString(), QString("body"));
        QCOMPARE(parameters[1].toObject()["Id"].toString(), fixture.ids[2]); QCOMPARE(parameters[1].toObject()["GroupId"].toString(), QString());
        QCOMPARE(parameters[2].toObject()["Id"].toString(), fixture.ids[0]); QCOMPARE(parameters[3].toObject()["Id"].toString(), fixture.ids[1]);
        QCOMPARE(groups[0].toObject()["Id"].toString(), QString("body")); QCOMPARE(groups[1].toObject()["Name"].toString(), QString("Face"));
        const auto extension = root["Live2DToolsParameterOrganization"].toObject(); QCOMPARE(extension["Version"].toInt(), 1);
        Organization::State reopened; QVERIFY(Organization::FromJson(extension["State"].toObject(), reopened, &error)); QCOMPARE(reopened, input.parameterOrganization);
        QVERIFY(!root.contains("CombinedParameters"));
        CDirectoryExport::Options options; options.protectedPaths = package.protectedPaths; CDirectoryExport::Result published;
        QVERIFY(CDirectoryExport::Publish(fixture.directory.filePath("export"), package.entries, options, published, nullptr, &error));
        CModelJson model; model.Parse(fixture.directory.filePath("export/") + package.modelFile); QVERIFY(model.IsOpen());
        CModelDocument document; QVERIFY(document.Load(model.getMocFileName(), &error)); QVERIFY(document.SetParameterOrganization(reopened, &error));
        const auto first = fixture.ids[0], second = fixture.ids[1]; QMap<QString, float> values;
        QVERIFY(Organization::ExpandUserValues(document, {{first, document.ParameterMetadata(first).minimum}}, values, &error));
        QCOMPARE(values.value(second), document.ParameterMetadata(second).maximum);
    }

    void configuredExportPreservesNamesUnknownFieldsAndOrphanDisplayRecords()
    {
        Fixture fixture; QVERIFY(fixture.ready); const auto combined = QJsonArray{QJsonArray{fixture.ids[1], fixture.ids[2]}};
        const QJsonObject source{{"Version", 3}, {"RootExtra", QJsonObject{{"keep", true}}}, {"Parts", QJsonArray{QJsonObject{{"Id", "Part"}, {"Extra", 3}}}},
            {"Parameters", QJsonArray{QJsonObject{{"Id", fixture.ids[0]}, {"Name", "Friendly"}, {"GroupId", "face"}, {"RowExtra", 9}},
                QJsonObject{{"Id", fixture.ids[1]}, {"Name", "Old"}, {"RowExtra", 11}},
                QJsonObject{{"Id", "HostExtra"}, {"Name", "Host"}, {"GroupId", "external"}, {"Opaque", QJsonArray{1, 2}}}}},
            {"ParameterGroups", QJsonArray{QJsonObject{{"Id", "unused"}, {"Name", "Unused"}},
                QJsonObject{{"Id", "face"}, {"Name", "Old face"}, {"FolderExtra", 17}},
                QJsonObject{{"Id", "external"}, {"Name", "External"}, {"ExternalExtra", true}}}}, {"CombinedParameters", combined}};
        QVERIFY(fixture.cdi(source)); const auto original = read(fixture.directory.filePath("names.cdi3.json"));
        QVERIFY(fixture.document.SetParameterOrganization(fixture.state())); auto input = fixture.request();
        auto metadata = fixture.document.ParameterMetadata(fixture.ids[1]); metadata.displayName = "Edited"; input.parameterMetadata[fixture.ids[1]] = metadata;
        CRuntimePackage::Prepared package; QString error; QVERIFY2(CRuntimePackage::Prepare(input, package, nullptr, &error), qPrintable(error));
        const auto root = QJsonDocument::fromJson(entry(package, package.model["FileReferences"].toObject()["DisplayInfo"].toString())).object();
        QCOMPARE(root["RootExtra"], source["RootExtra"]); QCOMPARE(root["Parts"], source["Parts"]); QCOMPARE(root["CombinedParameters"].toArray(), combined);
        QMap<QString, QJsonObject> rows; for (const auto& value : root["Parameters"].toArray()) rows[value.toObject()["Id"].toString()] = value.toObject();
        QCOMPARE(rows[fixture.ids[0]]["Name"].toString(), QString("Friendly")); QCOMPARE(rows[fixture.ids[0]]["RowExtra"].toInt(), 9);
        QCOMPARE(rows[fixture.ids[1]]["Name"].toString(), QString("Edited")); QCOMPARE(rows[fixture.ids[1]]["RowExtra"].toInt(), 11);
        QCOMPARE(root["Parameters"].toArray().last().toObject(), source["Parameters"].toArray().last().toObject());
        const auto groups = root["ParameterGroups"].toArray(); QCOMPARE(groups.size(), 3);
        QCOMPARE(groups[1].toObject()["Name"].toString(), QString("Face")); QCOMPARE(groups[1].toObject()["FolderExtra"].toInt(), 17);
        QCOMPARE(groups.last().toObject(), source["ParameterGroups"].toArray().last().toObject());
        QCOMPARE(read(fixture.directory.filePath("names.cdi3.json")), original);
        QVERIFY(package.protectedPaths.contains(fixture.directory.filePath("names.cdi3.json")));
    }

    void deletedSourceParametersAndTheirCombinedRowsStayRemoved()
    {
        Fixture fixture; QVERIFY(fixture.ready); const auto deleted = fixture.ids[0];
        const auto combined = QJsonArray{QJsonArray{deleted, fixture.ids[1]}, QJsonArray{fixture.ids[1], fixture.ids[2]}};
        QVERIFY(fixture.cdi({{"Version", 3}, {"Parameters", QJsonArray{QJsonObject{{"Id", deleted}, {"Name", "Removed"}},
            QJsonObject{{"Id", "Host"}, {"Name", "Kept"}}}}, {"CombinedParameters", combined}}));
        QVERIFY(fixture.document.SetParameterOrganization(fixture.state())); CModelEdit edit; QString error;
        QVERIFY2(fixture.document.MakeDeleteParameterEdit({deleted, fixture.document.ParameterMetadata(deleted).defaultValue}, edit, &error), qPrintable(error));
        QVERIFY(fixture.document.ExecuteEdit(edit)); auto input = fixture.request(); CRuntimePackage::Prepared package;
        QVERIFY2(CRuntimePackage::Prepare(input, package, nullptr, &error), qPrintable(error));
        const auto root = QJsonDocument::fromJson(entry(package, package.model["FileReferences"].toObject()["DisplayInfo"].toString())).object();
        for (const auto& row : root["Parameters"].toArray()) QVERIFY(row.toObject()["Id"].toString() != deleted);
        QCOMPARE(root["CombinedParameters"].toArray(), QJsonArray({combined[1]}));
        Organization::State state; QVERIFY(Organization::FromJson(root["Live2DToolsParameterOrganization"].toObject()["State"].toObject(), state));
        QVERIFY(state.pairs.isEmpty()); QCOMPARE(state, fixture.document.ParameterOrganization());
    }

    void clearedOrganizationDoesNotReviveSourceExtension()
    {
        Fixture fixture; QVERIFY(fixture.ready); QJsonObject state; QVERIFY(Organization::ToJson(fixture.state(), state));
        QJsonObject source{{"Version", 3}, {"Parameters", QJsonArray{QJsonObject{{"Id", fixture.ids[0]}, {"Name", "Friendly"}}}},
            {"Opaque", "kept"}, {"Live2DToolsParameterOrganization", QJsonObject{{"Version", 1}, {"State", state}}}};
        QVERIFY(fixture.cdi(source)); CRuntimePackage::Prepared package; QString error;
        QVERIFY(CRuntimePackage::Prepare(fixture.request(), package, nullptr, &error));
        const auto root = QJsonDocument::fromJson(entry(package, package.model["FileReferences"].toObject()["DisplayInfo"].toString())).object();
        source.remove("Live2DToolsParameterOrganization"); QCOMPARE(root, source);
    }

    void malformedSourcesAndStateDoNotReplacePreparedOutput_data()
    {
        QTest::addColumn<int>("kind"); const char* names[]{"duplicate-parameter", "duplicate-folder", "missing-parameter", "unconfigured-with-nodes", "encoded-budget"};
        for (int kind = 0; kind < 5; ++kind) QTest::newRow(names[kind]) << kind;
    }
    void malformedSourcesAndStateDoNotReplacePreparedOutput()
    {
        QFETCH(int, kind); Fixture fixture; QVERIFY(fixture.ready); QJsonObject source{{"Version", 3}};
        if (kind == 0) source["Parameters"] = QJsonArray{QJsonObject{{"Id", fixture.ids[0]}}, QJsonObject{{"Id", fixture.ids[0]}}};
        if (kind == 1) source["ParameterGroups"] = QJsonArray{QJsonObject{{"Id", "face"}}, QJsonObject{{"Id", "face"}}};
        QVERIFY(fixture.cdi(source)); auto input = fixture.request(); input.parameterOrganization = fixture.state();
        if (kind == 2) input.parameterOrganization.nodes.removeLast(); if (kind == 3) input.parameterOrganization.configured = false;
        if (kind == 4) input.maximumFiles = 2;
        CRuntimePackage::Prepared prepared; prepared.model = {{"keep", true}}; prepared.totalBytes = 17;
        QString error; QVERIFY(!CRuntimePackage::Prepare(input, prepared, nullptr, &error)); QVERIFY(!error.isEmpty());
        QCOMPARE(prepared.model, QJsonObject({{"keep", true}})); QCOMPARE(prepared.totalBytes, 17); QVERIFY(prepared.entries.isEmpty());
    }
};
QTEST_GUILESS_MAIN(ParameterOrganizationExportTests)
#include "parameter_organization_export_tests.moc"
