#include "cmo3/ccmo3namereader.h"
#include "cmo3/ccaffarchive.h"
#include "cmo3/ccmo3ids.h"
#include "live2dstruct/cmodeldocument.h"
#include "live2dstruct/csectiontable.h"
#include "std/cexportcontrol.h"
#include <QCryptographicHash>
#include <QFile>
#include <QTemporaryDir>
#include <QXmlStreamWriter>
#include <QtTest>

namespace {
using Names = cmo3::CCmo3NameReader;
QByteArray mocBytes()
{
    QFile file(QStringLiteral(TEST_RESOURCES "/Mark/Mark.moc3"));
    if (!file.open(QIODevice::ReadOnly)) qFatal("missing Mark fixture");
    return file.readAll();
}
QByteArray archive(const QByteArray& xml)
{
    cmo3::CCaffArchive archive;
    archive.AddEntry(QStringLiteral("main.xml"), xml, QStringLiteral("main_xml"), cmo3::CCaffArchive::CompressSmall);
    QTemporaryDir directory;
    const auto path = directory.filePath(QStringLiteral("names.cmo3"));
    if (!archive.Save(path)) qFatal("cannot create fixture");
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) qFatal("cannot read fixture");
    return file.readAll();
}
QByteArray project(const QByteArray& moc)
{
    live2dstruct::CModelDocument document;
    if (!document.LoadBytes(moc, {})) qFatal("invalid fixture");
    auto reader = live2dstruct::CReader::FromBytes(moc);
    live2dstruct::CSectionTable table(reader);
    QByteArray bytes;
    QXmlStreamWriter xml(&bytes);
    const auto open = [&](const QString& type, const QString& name = {}) {
        xml.writeStartElement(type);
        if (!name.isEmpty()) xml.writeAttribute(QStringLiteral("xs.n"), name);
    };
    const auto text = [&](const QString& type, const QString& name, const QString& value) {
        open(type, name); xml.writeCharacters(value); xml.writeEndElement();
    };
    const auto attribute = [&](const QString& type, const QString& name, const QString& attr, const QString& value) {
        open(type, name); xml.writeAttribute(attr, value); xml.writeEndElement();
    };
    const auto partGuid = [&](int index) { return index < 0 ? QStringLiteral("root-guid") : "part-" + document.Parts()[index].id; };
    const auto deformerGuid = [&](int index) { return index < 0 ? cmo3::WellKnown::kDeformerRoot : "deformer-" + document.Deformers()[index].id; };
    const auto common = [&](const QString& name, const QString& parent) {
        open(QStringLiteral("ACParameterControllableSource"), QStringLiteral("super"));
        text("s", "localName", name);
        attribute("CPartGuid", "parentGuid", "uuid", parent);
        xml.writeEndElement();
    };
    const auto set = [&](const QString& type, const QString& name, int count) {
        open(type, name); open("carray_list", "_sources"); xml.writeAttribute("count", QString::number(count));
    };
    const auto array = [&](const QString& type, const QString& name, const auto& values) {
        open(type, name); xml.writeAttribute("count", QString::number(values.size()));
        QStringList strings;
        for (auto value : values) strings.append(QString::number(value));
        xml.writeCharacters(strings.join(' ')); xml.writeEndElement();
    };
    open("root"); xml.writeAttribute("fileFormatVersion", "500000000");
    open("shared"); open("CPartSource"); xml.writeAttribute("xs.id", "root-part");
    common("Root", {}); attribute("CPartId", "id", "idstr", "__RootPart__"); attribute("CPartGuid", "guid", "uuid", "root-guid");
    xml.writeEndElement(); xml.writeEndElement();
    open("main"); open("CModelSource");
    open("CImageCanvas", "canvas");
    text("i", "pixelWidth", QString::number(table.canvas_info->width));
    text("i", "pixelHeight", QString::number(table.canvas_info->height)); xml.writeEndElement();
    attribute("CPartSource", "rootPart", "xs.ref", "root-part");
    set("CPartSourceSet", "partSourceSet", document.Parts().size() + 1);
    attribute("CPartSource", {}, "xs.ref", "root-part");
    for (const auto& part : document.Parts()) {
        open("CPartSource"); common("部件 " + part.id, partGuid(part.parentIndex));
        attribute("CPartId", "id", "idstr", part.id); attribute("CPartGuid", "guid", "uuid", "part-" + part.id); xml.writeEndElement();
    }
    xml.writeEndElement(); xml.writeEndElement();
    set("CDrawableSourceSet", "drawableSourceSet", document.Drawables().size());
    for (const auto& mesh : document.Drawables()) {
        open("CArtMeshSource"); open("ACDrawableSource", "super"); common("网格 " + mesh.id, partGuid(mesh.parentPartIndex));
        attribute("CDrawableId", "id", "idstr", mesh.id); attribute("CDrawableGuid", "guid", "uuid", "mesh-" + mesh.id);
        attribute("CDeformerGuid", "targetDeformerGuid", "uuid", deformerGuid(mesh.parentDeformerIndex)); xml.writeEndElement();
        array("int-array", "indices", mesh.indices);
        open("carray_list", "keyforms"); xml.writeAttribute("count", "1"); open("CArtMeshForm");
        array("float-array", "positions", QVector<int>(mesh.vertexCount * 2)); xml.writeEndElement(); xml.writeEndElement(); xml.writeEndElement();
    }
    xml.writeEndElement(); xml.writeEndElement();
    set("CDeformerSourceSet", "deformerSourceSet", document.Deformers().size());
    for (const auto& deformer : document.Deformers()) {
        const bool warp = deformer.type == live2dstruct::CModelDeformer::Warp;
        open(warp ? "CWarpDeformerSource" : "CRotationDeformerSource"); open("ACDeformerSource", "super");
        common("变形器 " + deformer.id, partGuid(deformer.parentPartIndex)); attribute("CDeformerId", "id", "idstr", deformer.id);
        attribute("CDeformerGuid", "guid", "uuid", "deformer-" + deformer.id);
        attribute("CDeformerGuid", "targetDeformerGuid", "uuid", deformerGuid(deformer.parentDeformerIndex)); xml.writeEndElement();
        if (warp) {
            text("i", "row", QString::number(deformer.rows)); text("i", "col", QString::number(deformer.columns));
            text("b", "isQuadTransform", deformer.quadTransform ? "true" : "false");
        }
        xml.writeEndElement();
    }
    xml.writeEndElement(); xml.writeEndElement(); xml.writeEndElement(); xml.writeEndElement(); xml.writeEndElement();
    return bytes;
}
const Names::Match* match(const Names::Proposal& proposal, const QString& id)
{
    for (const auto& item : proposal.matches) if (item.id == id) return &item;
    return nullptr;
}
}

class Cmo3NameReaderTests : public QObject
{
    Q_OBJECT
private slots:
    void matchingCompleteActiveTypedStructure()
    {
        const auto moc = mocBytes();
        Names::Catalog catalog; Names::Proposal proposal; QString error;
        QVERIFY2(Names::ReadBytes(archive(project(moc)), "missing.cmo3", catalog, &error), qPrintable(error));
        QVERIFY2(Names::BuildProposal(catalog, moc, proposal, &error), qPrintable(error));
        QCOMPARE(proposal.compatibleCount, catalog.Objects().size());
        QVERIFY(!proposal.partial);
        QCOMPARE(proposal.sourceMocDigest, QCryptographicHash::hash(moc, QCryptographicHash::Sha256));
        QCOMPARE(match(proposal, "Hair2")->name, QStringLiteral("网格 Hair2"));
        QCOMPARE(match(proposal, "Hair2")->type, Names::ObjectType::ArtMesh);
        QCOMPARE(catalog.SourcePath(), QStringLiteral("missing.cmo3"));
        for (const auto& object : catalog.Objects()) QVERIFY(object.id != "__RootPart__");
    }
    void unusedPoolObjectsAreNotImported()
    {
        auto xml = project(mocBytes());
        xml.replace("<shared>", "<shared><CArtMeshSource xs.id=\"orphan\"><broken/></CArtMeshSource>");
        Names::Catalog catalog; QString error;
        QVERIFY2(Names::ReadBytes(archive(xml), {}, catalog, &error), qPrintable(error));
        for (const auto& object : catalog.Objects()) QVERIFY(object.guid != "orphan");
    }
    void conflictsAndMissingPreserveOtherCandidates_data()
    {
        QTest::addColumn<QByteArray>("before"); QTest::addColumn<QByteArray>("after"); QTest::addColumn<QString>("id");
        QTest::addColumn<int>("status");
        QTest::newRow("renamed-id") << QByteArray("idstr=\"Hair2\"") << QByteArray("idstr=\"ExternalHair\"") << QString("ExternalHair") << int(Names::MatchStatus::MissingTarget);
        QTest::newRow("duplicate-id") << QByteArray("idstr=\"Hair1\"") << QByteArray("idstr=\"Hair2\"") << QString("Hair2") << int(Names::MatchStatus::Conflict);
        QTest::newRow("invalid-name") << QString("网格 Hair2").toUtf8() << QByteArray(" ") << QString("Hair2") << int(Names::MatchStatus::InvalidName);
        QTest::newRow("parent") << QByteArray("xs.n=\"targetDeformerGuid\" uuid=\"deformer-Warp12\"") << QByteArray("xs.n=\"targetDeformerGuid\" uuid=\"absent\"") << QString("Hair2") << int(Names::MatchStatus::Conflict);
    }
    void conflictsAndMissingPreserveOtherCandidates()
    {
        QFETCH(QByteArray, before); QFETCH(QByteArray, after); QFETCH(QString, id); QFETCH(int, status);
        const auto moc = mocBytes(); auto xml = project(moc); QVERIFY(xml.contains(before)); xml.replace(before, after);
        Names::Catalog catalog; Names::Proposal proposal; QString error;
        QVERIFY2(Names::ReadBytes(archive(xml), {}, catalog, &error), qPrintable(error));
        QVERIFY2(Names::BuildProposal(catalog, moc, proposal, &error), qPrintable(error));
        QVERIFY(match(proposal, id)); QCOMPARE(int(match(proposal, id)->status), status);
        QVERIFY(proposal.partial); QVERIFY(proposal.compatibleCount > 0);
    }
    void unrelatedCanvasCannotMatchCommonIds()
    {
        const auto moc = mocBytes(); auto xml = project(moc);
        xml.replace("<i xs.n=\"pixelWidth\">2400</i>", "<i xs.n=\"pixelWidth\">2401</i>");
        Names::Catalog catalog; Names::Proposal proposal; QString error;
        QVERIFY2(Names::ReadBytes(archive(xml), {}, catalog, &error), qPrintable(error));
        QVERIFY2(Names::BuildProposal(catalog, moc, proposal, &error), qPrintable(error));
        QCOMPARE(proposal.compatibleCount, 0);
    }
    void topologyMismatchIsNotOverriddenByMatchingId()
    {
        const auto moc = mocBytes(); auto xml = project(moc);
        const int begin = xml.indexOf("<int-array xs.n=\"indices\""); const int values = xml.indexOf('>', begin) + 1;
        const int end = xml.indexOf(' ', values); QVERIFY(begin >= 0 && end > values);
        xml.replace(values, end - values, xml.mid(values, end-values) == "0" ? "1" : "0");
        Names::Catalog catalog; Names::Proposal proposal; QString error;
        QVERIFY2(Names::ReadBytes(archive(xml), {}, catalog, &error), qPrintable(error));
        QVERIFY2(Names::BuildProposal(catalog, moc, proposal, &error), qPrintable(error));
        QCOMPARE(match(proposal, "Hair2")->status, Names::MatchStatus::Conflict);
        QVERIFY(proposal.compatibleCount > 0);
    }
    void malformedReferencesAndArraysAreAtomic_data()
    {
        QTest::addColumn<QByteArray>("before"); QTest::addColumn<QByteArray>("after");
        QTest::newRow("missing-ref") << QByteArray("xs.ref=\"root-part\"") << QByteArray("xs.ref=\"absent\"");
        QTest::newRow("wrong-type") << QByteArray("<CPartSource xs.n=\"rootPart\"") << QByteArray("<CWarpDeformerSource xs.n=\"rootPart\"");
        QTest::newRow("bad-number") << QByteArray("<i xs.n=\"row\">5</i>") << QByteArray("<i xs.n=\"row\">nan</i>");
        QTest::newRow("duplicate-field") << QByteArray("<i xs.n=\"pixelWidth\">2400</i>") << QByteArray("<i xs.n=\"pixelWidth\">2400</i><i xs.n=\"pixelWidth\">2400</i>");
    }
    void malformedReferencesAndArraysAreAtomic()
    {
        QFETCH(QByteArray, before); QFETCH(QByteArray, after);
        auto xml = project(mocBytes()); Names::Catalog catalog; QString error;
        QVERIFY(Names::ReadBytes(archive(xml), {}, catalog, &error)); const auto original = catalog.SourceDigest();
        QVERIFY(xml.contains(before)); xml.replace(before, after);
        QVERIFY(!Names::ReadBytes(archive(xml), {}, catalog, &error)); QVERIFY(!error.isEmpty()); QCOMPARE(catalog.SourceDigest(), original);
    }
    void wrongWarpShapeConflictsWithDescendantMesh()
    {
        const auto moc = mocBytes(); auto xml = project(moc);
        const int id = xml.indexOf("idstr=\"Warp12\"");
        const int row = xml.indexOf("<i xs.n=\"row\">", id);
        const int value = xml.indexOf('>', row) + 1;
        const int end = xml.indexOf('<', value);
        QVERIFY(id >= 0 && row > id && end > value);
        xml.replace(value, end - value, "7");
        Names::Catalog catalog; Names::Proposal proposal; QString error;
        QVERIFY2(Names::ReadBytes(archive(xml), {}, catalog, &error), qPrintable(error));
        QVERIFY2(Names::BuildProposal(catalog, moc, proposal, &error), qPrintable(error));
        QCOMPARE(match(proposal, "Warp12")->status, Names::MatchStatus::Conflict);
        QCOMPARE(match(proposal, "Hair2")->status, Names::MatchStatus::Conflict);
        QVERIFY(match(proposal, "Hair2")->diagnostics.contains(QStringLiteral("父级结构无法与原始模型核对")));
        QVERIFY(proposal.compatibleCount > 0);
    }
    void matchingIdsWithoutAnyMeshEvidenceRemainBlocked()
    {
        const auto moc = mocBytes(); auto xml = project(moc);
        int from = 0;
        while ((from = xml.indexOf("<int-array xs.n=\"indices\"", from)) >= 0) {
            const int values = xml.indexOf('>', from) + 1, end = xml.indexOf('<', values);
            auto data = xml.mid(values, end - values);
            for (auto& c : data) if (c >= '0' && c <= '9') c = '0';
            xml.replace(values, end - values, data);
            from = end;
        }
        Names::Catalog catalog; Names::Proposal proposal; QString error;
        QVERIFY2(Names::ReadBytes(archive(xml), {}, catalog, &error), qPrintable(error));
        QVERIFY2(Names::BuildProposal(catalog, moc, proposal, &error), qPrintable(error));
        QCOMPARE(proposal.compatibleCount, 0);
        for (const auto& candidate : proposal.matches) QVERIFY(candidate.status != Names::MatchStatus::Compatible);
    }
    void typedIdentityCannotFallBackToPartWithSameId()
    {
        const auto moc = mocBytes(); auto xml = project(moc);
        xml.replace("<CDrawableId xs.n=\"id\" idstr=\"Hair2\"", "<CDrawableId xs.n=\"id\" idstr=\"PartHead\"");
        Names::Catalog catalog; Names::Proposal proposal; QString error;
        QVERIFY2(Names::ReadBytes(archive(xml), {}, catalog, &error), qPrintable(error));
        QVERIFY2(Names::BuildProposal(catalog, moc, proposal, &error), qPrintable(error));
        int found = 0;
        for (const auto& candidate : proposal.matches) if (candidate.id == "PartHead") {
            ++found;
            QCOMPARE(candidate.status, candidate.type == Names::ObjectType::Part ? Names::MatchStatus::Compatible : Names::MatchStatus::MissingTarget);
        }
        QCOMPARE(found, 2);
    }
    void duplicateGuidIsNotDisambiguatedByName()
    {
        const auto moc = mocBytes(); auto xml = project(moc);
        xml.replace("uuid=\"mesh-Hair1\"", "uuid=\"mesh-Hair2\"");
        Names::Catalog catalog; Names::Proposal proposal; QString error;
        QVERIFY2(Names::ReadBytes(archive(xml), {}, catalog, &error), qPrintable(error));
        QVERIFY2(Names::BuildProposal(catalog, moc, proposal, &error), qPrintable(error));
        QCOMPARE(match(proposal, "Hair2")->status, Names::MatchStatus::Conflict);
        QCOMPARE(match(proposal, "Hair1")->status, Names::MatchStatus::Conflict);
    }
    void originalBytesRemainTheAnchorAfterMeshEditing()
    {
        const auto moc = mocBytes(); Names::Catalog catalog; Names::Proposal proposal; QString error;
        QVERIFY(Names::ReadBytes(archive(project(moc)), {}, catalog, &error));
        live2dstruct::CModelDocument document; QVERIFY(document.LoadBytes(moc, {}, &error));
        auto indices = document.Drawables().first().indices;
        indices.removeLast(); indices.removeLast(); indices.removeLast();
        QVERIFY(document.SetDrawableIndices("Hair2", indices));
        QVERIFY2(Names::BuildProposal(catalog, document.SourceBytes(), proposal, &error), qPrintable(error));
        QCOMPARE(match(proposal, "Hair2")->status, Names::MatchStatus::Compatible);
        QByteArray runtime; QVERIFY2(document.BuildRuntimeMoc(runtime, &error), qPrintable(error));
        QVERIFY2(Names::BuildProposal(catalog, runtime, proposal, &error), qPrintable(error));
        QCOMPARE(match(proposal, "Hair2")->status, Names::MatchStatus::Conflict);
    }
    void graphCyclesAndInvalidNamesDoNotEnableDependents()
    {
        const auto moc = mocBytes(); auto xml = project(moc);
        const int name = xml.indexOf(QStringLiteral("部件 PartHairFront").toUtf8());
        const int parent = xml.indexOf("xs.n=\"parentGuid\" uuid=\"", name);
        const int begin = xml.indexOf("uuid=\"", parent) + 6, end = xml.indexOf('"', begin);
        QVERIFY(name >= 0 && parent > name && end > begin);
        xml.replace(begin, end - begin, "part-PartHairFront");
        Names::Catalog catalog; Names::Proposal proposal; QString error;
        QVERIFY2(Names::ReadBytes(archive(xml), {}, catalog, &error), qPrintable(error));
        QVERIFY2(Names::BuildProposal(catalog, moc, proposal, &error), qPrintable(error));
        QCOMPARE(match(proposal, "PartHairFront")->status, Names::MatchStatus::Conflict);
        QCOMPARE(match(proposal, "Hair2")->status, Names::MatchStatus::Conflict);
    }
    void cancellationAndCallbackBorrowedMutationAreAtomic()
    {
        auto bytes = archive(project(mocBytes())); const auto expected = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
        QByteArray borrowed = QByteArray::fromRawData(bytes.data(), bytes.size());
        QString path = "original.cmo3"; QString borrowedPath = QString::fromRawData(path.data(), path.size());
        CExportControl control; int calls = 0;
        control.SetHandler([&](const auto&, QString*) { if (++calls == 1) { bytes.fill('x'); path.fill('x'); } return true; });
        Names::Catalog catalog; QString error;
        QVERIFY2(Names::ReadBytes(borrowed, borrowedPath, catalog, &error, &control), qPrintable(error));
        QCOMPARE(catalog.SourceDigest(), expected); QCOMPARE(catalog.SourcePath(), QString("original.cmo3"));
        CExportControl canceled; canceled.Cancel();
        QVERIFY(!Names::ReadBytes(archive(project(mocBytes())), {}, catalog, &error, &canceled)); QCOMPARE(catalog.SourceDigest(), expected);
        auto moc = mocBytes(); const auto mocDigest = QCryptographicHash::hash(moc, QCryptographicHash::Sha256);
        const QByteArray rawMoc = QByteArray::fromRawData(moc.data(), moc.size());
        CExportControl matching; matching.SetHandler([&](const auto&, QString*) { moc.fill('x'); catalog = {}; return true; });
        Names::Proposal proposal;
        QVERIFY2(Names::BuildProposal(catalog, rawMoc, proposal, &error, &matching), qPrintable(error));
        QCOMPARE(proposal.sourceMocDigest, mocDigest); QCOMPARE(proposal.sourceCmoDigest, expected);
    }
    void readFileUsesFrozenArchiveAndProposalFailureRetainsOutput()
    {
        QTemporaryDir directory; const auto path = directory.filePath("source.cmo3"); QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly)); const auto bytes = archive(project(mocBytes())); QCOMPARE(file.write(bytes), bytes.size()); file.close();
        Names::Catalog catalog; QString error; QVERIFY2(Names::ReadFile(path, catalog, &error), qPrintable(error)); QFile::remove(path);
        Names::Proposal proposal; QVERIFY(Names::BuildProposal(catalog, mocBytes(), proposal, &error));
        const auto digest = proposal.sourceMocDigest; const int count = proposal.compatibleCount;
        QVERIFY(!Names::BuildProposal(catalog, "invalid", proposal, &error)); QCOMPARE(proposal.sourceMocDigest, digest); QCOMPARE(proposal.compatibleCount, count);
    }
};
QTEST_GUILESS_MAIN(Cmo3NameReaderTests)
#include "cmo3_name_reader_tests.moc"
