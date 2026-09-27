#include "geometry_object_pairs_document_fixture.h"
#include "deformer_order_fixture.h"
#include "live2dstruct/cruntimepackage.h"
#include "live2dstruct/cmocdeformerorder.h"
#include "std/cmodeljson.h"
#include "ui/para/ccidjson.h"
#include <QtTest>
#include <QTemporaryDir>
#include <QJsonDocument>
#include <QJsonArray>
#include <QFile>
#include <QDir>

using namespace GeometryPairsData;

namespace {
QByteArray read(const QString& path) { QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray(); }
bool write(const QString& path, const QByteArray& bytes) { QFile file(path); return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(); }
QByteArray digest(const QByteArray& bytes) { return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256); }
QByteArray entry(const CRuntimePackage::Prepared& package, const QString& path)
{
    for (const auto& value : package.entries) if (value.relativePath == path) return value.bytes;
    return {};
}
QJsonObject display(const CRuntimePackage::Prepared& package)
{
    return QJsonDocument::fromJson(entry(package, package.model["FileReferences"].toObject()["DisplayInfo"].toString())).object();
}
QJsonObject extension(const Pairs::State& state)
{
    QJsonObject json; Pairs::ToJson(state, json); return {{"Version", 1}, {"State", json}};
}
struct Fixture {
    QTemporaryDir directory;
    QJsonArray textures;
    bool ready = false;
    CModelDocument document;
    QString path, moc;
    Fixture() {
        CModelDocument source;
        if (!load(source)) return;
        path = directory.filePath("model.model3.json"); moc = directory.filePath("model.moc3");
        if (!write(moc, source.SourceBytes()) || !document.Load(moc)) return;
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
    bool save(bool hasDisplay) const {
        QJsonObject files{{"Moc", "model.moc3"}, {"Textures", textures}};
        if (hasDisplay) files["DisplayInfo"] = "names.cdi3.json";
        return write(path, QJsonDocument(QJsonObject{{"Version", 3}, {"FileReferences", files}}).toJson());
    }
    bool cdi(const QJsonObject& root) const { return save(true) && write(directory.filePath("names.cdi3.json"), QJsonDocument(root).toJson()); }
    CRuntimePackage::Request request() const {
        CRuntimePackage::Request result; result.modelJsonPath = path; result.modelSettingsDigest = digest(read(path));
        result.sourceMocDigest = document.SourceDigest(); document.BuildRuntimeMoc(result.mocBytes);
        result.geometryObjectPairs = document.GeometryObjectPairs(); return result;
    }
    bool edit() {
        auto positions = document.DrawableKeyformPositions("Mesh", 0); if (positions.isEmpty()) return false;
        positions[0] += .123f; return document.SetDrawableKeyformPositions("Mesh", 0, positions);
    }
};
}

class GeometryPairsExportTests : public QObject
{
    Q_OBJECT
private slots:
    void deformerNamesCreateDisplayInfoAndRoundTripAuthoritativeEmpty()
    {
        Fixture fixture; QVERIFY(fixture.ready); QString error;
        auto request = fixture.request(); request.deformerDisplayNames = QMap<QString, QString>{{"Root", QStringLiteral("头部 🌙")}};
        CRuntimePackage::Prepared package; QVERIFY2(CRuntimePackage::Prepare(request, package, nullptr, &error), qPrintable(error));
        const auto names = display(package)["Live2DToolsDeformerNames"].toObject();
        QCOMPARE(names["Version"].toInt(), 1); QCOMPARE(names["Names"].toObject()["Root"].toString(), QStringLiteral("头部 🌙"));
        const auto exportedMoc = entry(package, package.model["FileReferences"].toObject()["Moc"].toString());
        QCOMPARE(exportedMoc, request.mocBytes);
        const auto path = fixture.directory.filePath("readback.cdi3.json"); QVERIFY(write(path, QJsonDocument(display(package)).toJson()));
        CCidJson parser; QVERIFY(parser.Parse(path, &error)); QVERIFY(parser.HasDeformerDisplayNamesExtension());
        QCOMPARE(parser.DeformerDisplayNames(), *request.deformerDisplayNames);
        CModelDocument reopened; QVERIFY(reopened.LoadBytes(exportedMoc, "reopened", &error));
        QVERIFY(reopened.SetDeformerDisplayNames(parser.DeformerDisplayNames(), &error));
        QCOMPARE(reopened.DeformerDisplayName("Root"), QStringLiteral("头部 🌙"));
        QVERIFY(fixture.cdi({{"Version", 3}, {"Foreign", 17},
            {"Deformers", QJsonArray{QJsonObject{{"Id", "Root"}, {"Name", "old"}, {"Extra", 42}}, QJsonObject{{"Id", "Deleted"}, {"Name", "orphan"}}}},
            {"Live2DToolsDeformerNames", names}}));
        request = fixture.request(); request.deformerDisplayNames = QMap<QString, QString>{};
        QVERIFY2(CRuntimePackage::Prepare(request, package, nullptr, &error), qPrintable(error));
        const auto root = display(package); QCOMPARE(root["Foreign"].toInt(), 17);
        QVERIFY(root["Live2DToolsDeformerNames"].toObject()["Names"].toObject().isEmpty());
        const auto legacy = root["Deformers"].toArray(); QCOMPARE(legacy.size(), 1);
        QCOMPARE(legacy[0].toObject()["Name"].toString(), QString("Root")); QCOMPARE(legacy[0].toObject()["Extra"].toInt(), 42);
        QVERIFY(write(path, QJsonDocument(root).toJson())); QVERIFY(parser.Parse(path, &error));
        QVERIFY(parser.HasDeformerDisplayNamesExtension()); QVERIFY(parser.DeformerDisplayNames().isEmpty()); QVERIFY(parser.getDeformerName("Root").isEmpty());
    }
    void deformerNameExtensionOverridesLegacyAndMalformedInputIsAtomic_data()
    {
        QTest::addColumn<int>("fault"); for (int i = 0; i < 7; ++i) QTest::newRow(qPrintable(QString::number(i))) << i;
    }
    void deformerNameExtensionOverridesLegacyAndMalformedInputIsAtomic()
    {
        QFETCH(int, fault); QTemporaryDir directory; const auto path = directory.filePath("names.cdi3.json");
        QJsonObject names{{"Root", QStringLiteral("明确名称")}};
        QJsonObject root{{"Version", 3}, {"Deformers", QJsonArray{QJsonObject{{"Id", "Root"}, {"Name", "legacy"}}}},
            {"Live2DToolsDeformerNames", QJsonObject{{"Version", 1}, {"Names", names}}}};
        QVERIFY(write(path, QJsonDocument(root).toJson())); CCidJson parser; QString error; QVERIFY(parser.Parse(path, &error));
        const auto digest = parser.SourceDigest(); QCOMPARE(parser.getDeformerName("Root"), QStringLiteral("明确名称"));
        auto extension = root["Live2DToolsDeformerNames"].toObject();
        if (fault == 0) extension["Version"] = 2;
        if (fault == 1) extension["Names"] = QJsonArray{};
        if (fault == 2) extension["Names"] = QJsonObject{{"Root", 3}};
        if (fault == 3) extension["Names"] = QJsonObject{{"Root", " "}};
        if (fault == 4) extension["Names"] = QJsonObject{{"Root", QString("bad\nname")}};
        if (fault == 5) extension["Names"] = QJsonObject{{"Root", QString(1025, 'x')}};
        root["Live2DToolsDeformerNames"] = fault == 6 ? QJsonValue(false) : QJsonValue(extension);
        QVERIFY(write(path, QJsonDocument(root).toJson())); QVERIFY(!parser.Parse(path, &error)); QVERIFY(!error.isEmpty());
        QCOMPARE(parser.SourceDigest(), digest); QCOMPARE(parser.getDeformerName("Root"), QStringLiteral("明确名称"));
        root["Live2DToolsDeformerNames"] = QJsonObject{{"Version", 1}, {"Names", QJsonObject{}}};
        root["Deformers"] = "ignored legacy"; QVERIFY(write(path, QJsonDocument(root).toJson())); QVERIFY(parser.Parse(path, &error));
        QVERIFY(parser.HasDeformerDisplayNamesExtension()); QVERIFY(parser.DeformerDisplayNames().isEmpty());
        root.remove("Live2DToolsDeformerNames"); root["Deformers"] = QJsonArray{QJsonObject{{"Id", "Root"}, {"Name", "legacy"}}};
        QVERIFY(write(path, QJsonDocument(root).toJson())); QVERIFY(parser.Parse(path, &error));
        QVERIFY(!parser.HasDeformerDisplayNamesExtension()); QCOMPARE(parser.getDeformerName("Root"), QString("legacy"));
    }
    void runtimeRejectsUnknownDeformerNamesWithoutReplacingPreparedOutput_data()
    {
        QTest::addColumn<QString>("name"); QTest::newRow("named") << QString("name"); QTest::newRow("identity") << QString("missing");
    }
    void runtimeRejectsUnknownDeformerNamesWithoutReplacingPreparedOutput()
    {
        QFETCH(QString, name); Fixture fixture; QVERIFY(fixture.ready); auto request = fixture.request();
        request.deformerDisplayNames = QMap<QString, QString>{{"missing", name}};
        CRuntimePackage::Prepared output; output.modelFile = "keep"; QString error;
        QVERIFY(!CRuntimePackage::Prepare(request, output, nullptr, &error)); QVERIFY(!error.isEmpty()); QCOMPARE(output.modelFile, QString("keep"));
    }
    void changedRuntimeMocIsNormalizedAndPairDigestReanchored()
    {
        Fixture fixture; QVERIFY(fixture.ready); QString error; QVERIFY(fixture.document.IsLoaded());
        const auto original = paired(fixture.document); QVERIFY(fixture.document.SetGeometryObjectPairs(original)); QVERIFY(fixture.edit());
        auto input = fixture.request(); QVERIFY(digest(input.mocBytes) != input.sourceMocDigest);
        auto reader = CReader::FromBytes(input.mocBytes); CSectionTable table(reader); QVERIFY(table.IsValid());
        input.mocBytes = DeformerOrderFixture::permute(input.mocBytes, DeformerOrderFixture::reverseOrder(table)); QVERIFY(!input.mocBytes.isEmpty());
        QByteArray expected; QVERIFY(CMocDeformerOrder::Normalize(input.mocBytes, expected, &error)); QVERIFY(expected != input.mocBytes);
        CRuntimePackage::Prepared package; QVERIFY2(CRuntimePackage::Prepare(input, package, nullptr, &error), qPrintable(error));
        const auto files = package.model["FileReferences"].toObject(); const auto exportedMoc = entry(package, files["Moc"].toString());
        QCOMPARE(exportedMoc, expected); QVERIFY(digest(exportedMoc) != fixture.document.SourceDigest());
        Pairs::State exported; const auto ext = display(package)["Live2DToolsGeometryObjectPairs"].toObject();
        QCOMPARE(ext["Version"].toInt(), 1); QVERIFY(Pairs::FromJson(ext["State"].toObject(), exported, &error));
        QCOMPARE(exported.sourceDigest, digest(exportedMoc)); QCOMPARE(exported.pairs, original.pairs);
        CModelDocument compiled; QVERIFY2(compiled.LoadBytes(exportedMoc, "compiled", &error), qPrintable(error));
        QVERIFY(compiled.SetGeometryObjectPairs(exported, &error));
        QCOMPARE(compiled.DrawableKeyformPositions("Mesh", 0), fixture.document.DrawableKeyformPositions("Mesh", 0));
        QCOMPARE(compiled.DrawableKeyformPositions("Twin", 0), fixture.document.DrawableKeyformPositions("Twin", 0));
        Pairs::Endpoint first, second;
        QVERIFY(Pairs::DescribeEndpoint(compiled, exported.pairs[0].first.object, first)); QCOMPARE(first, exported.pairs[0].first);
        QVERIFY(Pairs::DescribeEndpoint(compiled, exported.pairs[0].second.object, second)); QCOMPARE(second, exported.pairs[0].second);
        CDirectoryExport::Options options; options.protectedPaths = package.protectedPaths; CDirectoryExport::Result published;
        const QString destination = fixture.directory.filePath("published");
        QVERIFY2(CDirectoryExport::Publish(destination, package.entries, options, published, nullptr, &error), qPrintable(error));
        CModelJson settings; settings.Parse(QDir(destination).filePath(package.modelFile)); QVERIFY(settings.IsOpen());
        CModelDocument reopened; QVERIFY(reopened.Load(settings.getMocFileName(), &error));
        CCidJson names; QVERIFY2(names.Parse(QDir(settings.m_parentPath).filePath(settings.m_DisplayInfo), &error), qPrintable(error));
        QVERIFY(names.GeometryObjectPairs()); QVERIFY(reopened.SetGeometryObjectPairs(*names.GeometryObjectPairs(), &error));
        QCOMPARE(reopened.GeometryObjectPairs(), exported); QCOMPARE(reopened.DrawableKeyformPositions("Mesh", 0), fixture.document.DrawableKeyformPositions("Mesh", 0));
        QCOMPARE(read(fixture.moc), fixture.document.SourceBytes());
    }

    void editedBindingKeysMatchTheExportedPairSignatures()
    {
        Fixture fixture; QVERIFY(fixture.ready); QString error; QVERIFY(fixture.document.SetGeometryObjectPairs(paired(fixture.document))); QVERIFY(fixture.edit());
        CMocParameterStructureBuilder::EditBindingKeysRequest keys;
        keys.owner = {CMocParameterStructureBuilder::Owner::ArtMesh, "Mesh"}; keys.axisIndex = 0;
        keys.parameterId = "P"; keys.expectedKeys = {0, 1}; keys.keys = {{.2f, 0}, {.8f, 1}};
        CModelEdit edit; QVERIFY2(fixture.document.MakeBindingKeysEdit(keys, edit, &error), qPrintable(error));
        QVERIFY(fixture.document.ExecuteEdit(edit)); const auto original = fixture.document.GeometryObjectPairs();
        QVERIFY(!original.pairs[0].requiresReview);
        CRuntimePackage::Prepared package; QVERIFY2(CRuntimePackage::Prepare(fixture.request(), package, nullptr, &error), qPrintable(error));
        const auto files = package.model["FileReferences"].toObject(); const auto bytes = entry(package, files["Moc"].toString());
        CModelDocument compiled; QVERIFY(compiled.LoadBytes(bytes, "compiled", &error));
        Pairs::State state; QVERIFY(Pairs::FromJson(display(package)["Live2DToolsGeometryObjectPairs"].toObject()["State"].toObject(), state, &error));
        QCOMPARE(state.sourceDigest, digest(bytes)); QCOMPARE(state.pairs, original.pairs);
        Pairs::Endpoint endpoint; QVERIFY(Pairs::DescribeEndpoint(compiled, state.pairs[0].first.object, endpoint));
        QCOMPARE(endpoint, state.pairs[0].first); QCOMPARE(endpoint.axes[0].keys, QVector<float>({.2f, .8f}));
        QVERIFY(Pairs::DescribeEndpoint(compiled, state.pairs[0].second.object, endpoint)); QCOMPARE(endpoint, state.pairs[0].second);
        QCOMPARE(compiled.DrawableKeyformPositions("Mesh", 0), fixture.document.DrawableKeyformPositions("Mesh", 0));
    }

    void unknownDisplayFieldsAndNamesRemainIntact()
    {
        Fixture fixture; QVERIFY(fixture.ready); const QJsonObject source{{"Version", 3}, {"RootExtra", QJsonObject{{"opaque", QJsonArray{1, 2, "three"}}}},
            {"Parts", QJsonArray{QJsonObject{{"Id", "ExternalPart"}, {"Name", "Part name"}, {"Extra", 7}}}},
            {"Deformers", QJsonArray{QJsonObject{{"Id", "Root"}, {"Name", "Friendly root"}, {"Extra", 8}}}},
            {"Parameters", QJsonArray{QJsonObject{{"Id", "P"}, {"Name", "Parameter name"}, {"GroupId", "face"}, {"Extra", 9}}}},
            {"ParameterGroups", QJsonArray{QJsonObject{{"Id", "face"}, {"Name", "Face"}, {"Extra", 10}}}},
            {"CombinedParameters", QJsonArray{QJsonArray{"P", "External"}}}};
        QVERIFY(fixture.cdi(source)); QVERIFY(fixture.document.SetGeometryObjectPairs(paired(fixture.document))); QVERIFY(fixture.edit());
        const auto original = read(fixture.directory.filePath("names.cdi3.json"));
        CRuntimePackage::Prepared package; QString error; QVERIFY2(CRuntimePackage::Prepare(fixture.request(), package, nullptr, &error), qPrintable(error));
        auto exported = display(package); QVERIFY(exported.contains("Live2DToolsGeometryObjectPairs"));
        exported.remove("Live2DToolsGeometryObjectPairs"); QCOMPARE(exported, source);
        QCOMPARE(read(fixture.directory.filePath("names.cdi3.json")), original);
    }

    void sourceAndStructureMismatchDoNotReplacePreparedOutput_data()
    {
        QTest::addColumn<int>("failure");
        for (int failure = 0; failure < 6; ++failure) QTest::newRow(qPrintable(QString::number(failure))) << failure;
    }
    void sourceAndStructureMismatchDoNotReplacePreparedOutput()
    {
        QFETCH(int, failure); Fixture fixture; QVERIFY(fixture.ready); QVERIFY(fixture.document.SetGeometryObjectPairs(paired(fixture.document))); QVERIFY(fixture.edit());
        auto input = fixture.request();
        if (failure == 0) input.geometryObjectPairs.sourceDigest[0] ^= 1;
        if (failure == 1) input.sourceMocDigest[0] ^= 1;
        if (failure == 2) input.geometryObjectPairs.pairs[0].firstVertexToSecondVertex = {0, 0, 2};
        if (failure == 3) {
            auto indices = fixture.document.DrawableIndices("Mesh"); std::swap(indices[0], indices[1]);
            QVERIFY(fixture.document.SetDrawableIndices("Mesh", indices)); QVERIFY(fixture.document.BuildRuntimeMoc(input.mocBytes));
        }
        if (failure == 4) input.geometryObjectPairs.pairs[0].first.object.id = "missing";
        if (failure == 5) input.mocBytes = "not a moc";
        CRuntimePackage::Prepared output; output.model = {{"held", true}}; output.totalBytes = 71;
        QString error; QVERIFY(!CRuntimePackage::Prepare(input, output, nullptr, &error)); QVERIFY(!error.isEmpty());
        QCOMPARE(output.model, QJsonObject({{"held", true}})); QCOMPARE(output.totalBytes, 71); QVERIFY(output.entries.isEmpty());
    }

    void reviewStateSurvivesChangedTopologyExport()
    {
        Fixture fixture; QVERIFY(fixture.ready); QVERIFY(fixture.document.SetGeometryObjectPairs(paired(fixture.document))); QVERIFY(fixture.edit());
        auto indices = fixture.document.DrawableIndices("Mesh"); std::swap(indices[0], indices[1]);
        QVERIFY(fixture.document.SetDrawableIndices("Mesh", indices));
        const auto before = fixture.document.GeometryObjectPairs(); QVERIFY(before.pairs[0].requiresReview);
        CRuntimePackage::Prepared package; QString error; QVERIFY2(CRuntimePackage::Prepare(fixture.request(), package, nullptr, &error), qPrintable(error));
        Pairs::State state; QVERIFY(Pairs::FromJson(display(package)["Live2DToolsGeometryObjectPairs"].toObject()["State"].toObject(), state, &error));
        QCOMPARE(state.pairs, before.pairs); QVERIFY(state.pairs[0].requiresReview);
        CModelDocument reopened; const auto bytes = entry(package, package.model["FileReferences"].toObject()["Moc"].toString());
        QVERIFY(reopened.LoadBytes(bytes, "exported", &error)); QVERIFY(reopened.SetGeometryObjectPairs(state, &error));
        QVERIFY(reopened.GeometryObjectPairs().pairs[0].requiresReview);
    }

    void removedAndDisabledMetadataDoesNotReviveFromSourceExtension_data()
    {
        QTest::addColumn<int>("mode");
        QTest::newRow("remove-extension") << 0; QTest::newRow("disabled-pair") << 1; QTest::newRow("explicit-empty") << 2;
    }
    void removedAndDisabledMetadataDoesNotReviveFromSourceExtension()
    {
        QFETCH(int, mode); Fixture fixture; QVERIFY(fixture.ready); const auto active = paired(fixture.document);
        const QJsonObject source{{"Version", 3}, {"Opaque", "kept"}, {"Live2DToolsGeometryObjectPairs", extension(active)}};
        QVERIFY(fixture.cdi(source)); auto input = fixture.request();
        if (mode == 1) { input.geometryObjectPairs = active; input.geometryObjectPairs.pairs[0].enabled = false; }
        if (mode == 2) input.geometryObjectPairs.sourceDigest = fixture.document.SourceDigest();
        CRuntimePackage::Prepared package; QString error; QVERIFY2(CRuntimePackage::Prepare(input, package, nullptr, &error), qPrintable(error));
        const auto root = display(package); QCOMPARE(root["Opaque"], source["Opaque"]);
        if (mode == 0) QVERIFY(!root.contains("Live2DToolsGeometryObjectPairs"));
        else {
            Pairs::State state; QVERIFY(Pairs::FromJson(root["Live2DToolsGeometryObjectPairs"].toObject()["State"].toObject(), state, &error));
            if (mode == 1) { QCOMPARE(state.pairs.size(), 1); QVERIFY(!state.pairs[0].enabled); }
            else QVERIFY(state.pairs.isEmpty());
        }
    }

    void parserFailureRetainsAllPriorState_data()
    {
        QTest::addColumn<int>("failure");
        for (int failure = 0; failure < 6; ++failure) QTest::newRow(qPrintable(QString::number(failure))) << failure;
    }
    void parserFailureRetainsAllPriorState()
    {
        QFETCH(int, failure); Fixture fixture; QVERIFY(fixture.ready); const auto state = paired(fixture.document);
        QJsonObject valid{{"Version", 3}, {"Parameters", QJsonArray{QJsonObject{{"Id", "P"}, {"Name", "Saved name"}, {"GroupId", "face"}}}},
            {"Parts", QJsonArray{QJsonObject{{"Id", "Part"}, {"Name", "Saved part"}}}},
            {"Deformers", QJsonArray{QJsonObject{{"Id", "Root"}, {"Name", "Saved deformer"}}}},
            {"ParameterGroups", QJsonArray{QJsonObject{{"Id", "face"}, {"Name", "Saved group"}}}},
            {"CombinedParameters", QJsonArray{QJsonArray{"P", "External"}}},
            {"Live2DToolsGeometryObjectPairs", extension(state)}};
        QVERIFY(fixture.cdi(valid)); CCidJson parser; QString error;
        const auto path = fixture.directory.filePath("names.cdi3.json"); QVERIFY(parser.Parse(path, &error));
        const auto previousPath = parser.SourcePath(); const auto previousDigest = parser.SourceDigest(); QVERIFY(parser.GeometryObjectPairs());
        auto invalid = valid; auto object = invalid["Live2DToolsGeometryObjectPairs"].toObject();
        if (failure == 0) object["Version"] = 2;
        if (failure == 1) object["State"] = QJsonArray{};
        if (failure == 2) { auto body = object["State"].toObject(); body["sourceDigest"] = "bad"; object["State"] = body; }
        if (failure == 3) { auto body = object["State"].toObject(); auto pairs = body["pairs"].toArray(); pairs.append(pairs[0]); body["pairs"] = pairs; object["State"] = body; }
        invalid["Live2DToolsGeometryObjectPairs"] = object;
        if (failure == 4) invalid["Parameters"] = 42;
        const QString next = fixture.directory.filePath("invalid.cdi3.json");
        QVERIFY(write(next, failure == 5 ? QByteArray("broken json") : QJsonDocument(invalid).toJson()));
        QVERIFY(!parser.Parse(next, &error)); QVERIFY(!error.isEmpty());
        QCOMPARE(parser.SourcePath(), previousPath); QCOMPARE(parser.SourceDigest(), previousDigest);
        QCOMPARE(parser.getParaName("P"), QString("Saved name")); QVERIFY(parser.GeometryObjectPairs()); QCOMPARE(*parser.GeometryObjectPairs(), state);
        QCOMPARE(parser.getPartName("Part"), QString("Saved part")); QCOMPARE(parser.getDeformerName("Root"), QString("Saved deformer"));
        QCOMPARE(parser.getGroupIdToName().value("face"), QString("Saved group")); QCOMPARE(parser.getGroups().size(), 1);
        QVERIFY(parser.isCombinedParameter("P")); QCOMPARE(parser.getCombinedParameters().size(), 1);
        QVERIFY(write(next, QJsonDocument(QJsonObject{{"Version", 3}, {"Parameters", QJsonArray{QJsonObject{{"Id", "P"}, {"Name", "New name"}}}}}).toJson()));
        QVERIFY(parser.Parse(next, &error)); QVERIFY(!parser.GeometryObjectPairs()); QCOMPARE(parser.getParaName("P"), QString("New name"));
        QCOMPARE(parser.SourcePath(), next); QVERIFY(parser.SourceDigest() != previousDigest);
        QVERIFY(parser.getPartName("Part").isEmpty()); QVERIFY(parser.getDeformerName("Root").isEmpty());
        QVERIFY(parser.getGroupIdToName().isEmpty()); QVERIFY(parser.getCombinedParameters().isEmpty());
    }
};

QTEST_GUILESS_MAIN(GeometryPairsExportTests)
#include "geometry_pairs_export_tests.moc"
