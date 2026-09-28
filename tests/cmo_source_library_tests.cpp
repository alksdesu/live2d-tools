#include "live2dstruct/cartworksourcelibraryplanner.h"
#include "live2dstruct/cartworkpsdrecovery.h"
#include "live2dstruct/cmodeldocument.h"
#include "cmo3/ccmo3project.h"
#include "std/cexportcontrol.h"
#include "std/cpngimage.h"
#include "cmo_artwork_source_fixture.h"
#include <QtTest>
#include <QCryptographicHash>

using namespace live2dstruct;
using Artwork = CModelArtwork;
using Library = CArtworkSourceLibraryPlanner;
using Import = CArtworkSourceImport;
namespace {
Library::Snapshot snapshot()
{
    Library::Snapshot result; result.sourceDigest = QCryptographicHash::hash("model", QCryptographicHash::Sha256);
    QString error;
    if (!CTextureWorkspace::Initialize({}, {}, result.baseline.workspace, &error)) qFatal("%s", qPrintable(error));
    return result;
}
cmo3::CCmo3Project project()
{
    cmo3::CCmo3Project result; QString error;
    if (!cmo3::CCmo3Project::OpenBytes(cmo_artwork_source_fixture::archive(), "/missing/source.cmo3", result, &error)) qFatal("%s", qPrintable(error));
    return result;
}
Import::SourceCandidate source(const cmo3::CCmo3Project& project, int index)
{
    Import::SourceCandidate result; QString error;
    if (!Import::Prepare(project, QString("d%1-guid").arg(index), QString("family%1").arg(index), QString("revision%1").arg(index),
                         Artwork::ColorPolicy::PreserveValues, {}, result, &error)) qFatal("%s", qPrintable(error));
    return result;
}
}
class CmoSourceLibraryTests : public QObject
{
    Q_OBJECT
private slots:
    void documentsPreserveGuidHierarchyEmptyLayersAndOriginalBytes()
    {
        const auto p = project(); const auto first = source(p, 0), second = source(p, 1);
        for (const auto& item : {first, second}) {
            const auto& revision = item.revision;
            QCOMPARE(item.kind, Artwork::SourceKind::Cmo3); QCOMPARE(revision.sourceBytes, p.Bytes());
            QCOMPARE(revision.sourceDigest, p.SourceDigest()); QCOMPARE(revision.layers.size(), 3);
            QCOMPARE(revision.layers[0].kind, Artwork::LayerKind::Group);
            QCOMPARE(revision.layers[1].parentIndex, 0); QCOMPARE(revision.layers[2].parentIndex, 0);
            QVERIFY(revision.layers[2].imageDigest.isEmpty()); QCOMPARE(revision.layers[2].bounds, QRect(-3, 4, 0, 0));
            QCOMPARE(item.images.size(), 1); QImage image;
            QVERIFY(CPngImage::Decode(item.images.first().png, image));
            QCOMPARE(image.pixelColor(0, 0), QColor(17, 29, 43, 0)); QCOMPARE(image.pixelColor(1, 0), QColor(83, 109, 151, 123));
        }
        QVERIFY(first.revision.sourceDocumentId != second.revision.sourceDocumentId);
        QVERIFY(first.revision.layers[1].id != second.revision.layers[1].id);
    }
    void unbakedBlendAndClippingRemainStoredButCannotMasqueradeAsPlainPixels_data()
    {
        QTest::addColumn<QByteArray>("before"); QTest::addColumn<QByteArray>("after"); QTest::addColumn<QByteArray>("blend");
        QTest::newRow("multiply") << QByteArray("CBlend_Normal") << QByteArray("CBlend_Multiply") << QByteArray("mul ");
        QTest::newRow("clipping") << QByteArray("<b xs.n=\"isClipping\">false</b>") << QByteArray("<b xs.n=\"isClipping\">true</b>") << QByteArray("norm");
    }
    void unbakedBlendAndClippingRemainStoredButCannotMasqueradeAsPlainPixels()
    {
        QFETCH(QByteArray, before); QFETCH(QByteArray, after); QFETCH(QByteArray, blend);
        cmo3::CCaffArchive archive, changed; QString error;
        QVERIFY(archive.LoadBytes(cmo_artwork_source_fixture::archive(), &error));
        for (const auto& entry : archive.Entries()) {
            auto bytes = entry.content; if (entry.tag == "main_xml") bytes.replace(before, after);
            changed.AddEntry(entry.path, bytes, entry.tag, entry.compress);
        }
        QTemporaryDir folder; QVERIFY(folder.isValid()); const auto path = folder.filePath("effects.cmo3");
        QVERIFY(changed.Save(path, &error)); cmo3::CCmo3Project p; QVERIFY2(cmo3::CCmo3Project::OpenFile(path, p, &error), qPrintable(error));
        const auto prepared = source(p, 0); const auto& layer = prepared.revision.layers[1];
        QVERIFY(!layer.pixelEquivalent); QVERIFY(!layer.diagnostics.isEmpty()); QCOMPARE(layer.blendMode, blend); QVERIFY(!layer.imageDigest.isEmpty());
        Library::Plan plan; QVERIFY2(Library::Build(snapshot(), {{prepared, "effects"}}, plan, &error), qPrintable(error));
        QCOMPARE(plan.state.artwork.revisions.first().sourceBytes, p.Bytes());
    }
    void revisionIdentityIncludesDocumentPolicyTransformAndFamily()
    {
        const auto p = project(); const auto policy = Artwork::ColorPolicy::PreserveValues;
        const auto base = Import::RevisionIdentity(p, "d0-guid", "family", policy, {});
        QCOMPARE(base, Import::RevisionIdentity(p, "d0-guid", "family", policy, {}));
        QVERIFY(base != Import::RevisionIdentity(p, "d1-guid", "family", policy, {}));
        QVERIFY(base != Import::RevisionIdentity(p, "d0-guid", "other", policy, {}));
        QVERIFY(base != Import::RevisionIdentity(p, "d0-guid", "family", Artwork::ColorPolicy::ConvertToSRgb, {}));
        QVERIFY(base != Import::RevisionIdentity(p, "d0-guid", "family", policy, QTransform::fromTranslate(1, 2)));
    }
    void missingDocumentAndCancellationKeepOutput()
    {
        const auto p = project(); const auto before = source(p, 0); auto candidate = before; QString error;
        QVERIFY(!Import::Prepare(p, "missing", "family", "next", Artwork::ColorPolicy::PreserveValues, {}, candidate, &error));
        QCOMPARE(candidate.revision, before.revision); QCOMPARE(candidate.images, before.images);
        CExportControl control; control.Cancel();
        QVERIFY(!Import::Prepare(p, "d0-guid", "family", "next", Artwork::ColorPolicy::PreserveValues, {}, candidate, &error, &control));
        QCOMPARE(candidate.revision, before.revision); QCOMPARE(candidate.images, before.images);
    }
    void callbacksCannotReplaceBorrowedProjectOrIdentity()
    {
        auto p = project(); const auto bytes = p.Bytes();
        QChar documentData[]{'d','0','-','g','u','i','d'}, familyData[]{'f','a','m'}, revisionData[]{'r','e','v'};
        const auto document = QString::fromRawData(documentData, 7), family = QString::fromRawData(familyData, 3), revision = QString::fromRawData(revisionData, 3);
        CExportControl control; bool called = false; QString error;
        QVERIFY(control.SetHandler([&](const auto&, QString*) { called = true; p = {}; documentData[0] = 'X'; familyData[0] = 'X'; revisionData[0] = 'X'; return true; }));
        Import::SourceCandidate result;
        QVERIFY2(Import::Prepare(p, document, family, revision, Artwork::ColorPolicy::PreserveValues, {}, result, &error, &control), qPrintable(error));
        QVERIFY(called); QCOMPARE(result.revision.sourceBytes, bytes); QCOMPARE(result.revision.sourceDocumentId, QString("d0-guid"));
        QCOMPARE(result.revision.familyId, QString("fam")); QCOMPARE(result.revision.id, QString("rev"));
    }
    void batchIsAtomicAndDoesNotChangeMaterialsOrWorkspace()
    {
        const auto p = project(); const auto before = snapshot();
        QVector<Library::Source> sources{{source(p, 0), "同名文档"}, {source(p, 1), "同名文档"}};
        Library::Plan result; QString error;
        QVERIFY2(Library::Build(before, sources, result, &error), qPrintable(error));
        QVERIFY2(Library::Validate(before, result, &error), qPrintable(error));
        QCOMPARE(result.documentCount, 2); QCOMPARE(result.layerCount, 6); QCOMPARE(result.state.artwork.families.size(), 2);
        QCOMPARE(result.state.artwork.revisions.size(), 2); QCOMPARE(result.state.artwork.images.size(), 1);
        QCOMPARE(result.state.workspace, before.baseline.workspace); QCOMPARE(result.state.materials, before.baseline.materials);
        QVERIFY(result.state.artwork.bindings.isEmpty()); QVERIFY(before.baseline.artwork.IsEmpty());
        const auto held = result;
        sources[1].incoming.revision.sourceDigest.fill('x');
        QVERIFY(!Library::Build(before, sources, result, &error)); QCOMPARE(result.state, held.state); QCOMPARE(result.token, held.token);
        sources[1] = sources[0]; QVERIFY(!Library::Build(before, sources, result, &error)); QCOMPARE(result.state, held.state);
    }
    void sealsRejectAllPublicMutationsAndStaleSnapshot()
    {
        const auto p = project(); const auto before = snapshot(); Library::Plan result; QString error;
        QVERIFY(Library::Build(before, {{source(p, 0), "原稿"}}, result, &error));
        auto next = before; ++next.documentRevision; QVERIFY(!Library::Validate(next, result, &error));
        auto changed = result; ++changed.documentCount; QVERIFY(!Library::Validate(before, changed));
        changed = result; ++changed.layerCount; QVERIFY(!Library::Validate(before, changed));
        changed = result; changed.token.fill('x'); QVERIFY(!Library::Validate(before, changed));
        changed = result; changed.state.artwork.families.first().name = "altered"; QVERIFY(!Library::Validate(before, changed));
        next = before; ++next.limits.maximumOperations; QVERIFY(!Library::Validate(next, result));
        QVERIFY(Library::Validate(before, result));
    }
    void callbackMutationAndCancelCannotPublishPartialBatch()
    {
        const auto p = project(); auto before = snapshot(); const auto original = before;
        QVector<Library::Source> sources{{source(p, 0), "A"}, {source(p, 1), "B"}};
        CExportControl control; bool called = false; QString error;
        QVERIFY(control.SetHandler([&](const auto&, QString*) { called = true; sources.clear(); ++before.documentRevision; return true; }));
        Library::Plan result;
        QVERIFY2(Library::Build(before, sources, result, &error, &control), qPrintable(error));
        QVERIFY(called); QVERIFY(Library::Validate(original, result)); QVERIFY(!Library::Validate(before, result)); QCOMPARE(result.documentCount, 2);
        const auto held = result; CExportControl canceled; int calls = 0;
        QVERIFY(canceled.SetHandler([&](const auto&, QString*) { return ++calls < 2; }));
        QVERIFY(!Library::Build(original, {{source(p, 0), "A"}, {source(p, 1), "B"}}, result, &error, &canceled));
        QCOMPARE(result.state, held.state); QCOMPARE(result.token, held.token);
    }
    void originalHistoryCannotBeOverwritten()
    {
        const auto p = project(); auto before = snapshot(); Library::Plan first, result; QString error;
        auto incoming = source(p, 0); QVERIFY(Library::Build(before, {{incoming, "A"}}, first, &error));
        before.baseline = first.state; incoming.revision.layers[1].name = "changed";
        QVERIFY(!Library::Build(before, {{incoming, "A"}}, result, &error)); QVERIFY(error.contains("不能改写历史来源"));
        incoming.revision.id = "second"; QVERIFY(Library::Build(before, {{incoming, "A"}}, result, &error));
        QCOMPARE(result.state.artwork.revisions.size(), 2); QCOMPARE(result.state.artwork.revisions["revision0"], first.state.artwork.revisions["revision0"]);
    }
    void officialNativeSampleKeepsUnmeshedArtwork()
    {
        const auto path = qEnvironmentVariable("LIVE2D_TEST_NATIVE_CMO"); if (path.isEmpty()) QSKIP("native sample not configured");
        cmo3::CCmo3Project p; QString error; QVERIFY2(cmo3::CCmo3Project::OpenFile(path, p, &error), qPrintable(error));
        QVERIFY(p.Artwork()); QCOMPARE(p.Artwork()->documents.size(), 1); const auto* document = p.Artwork()->documents[0].Reconstructed(); QVERIFY(document);
        Import::SourceCandidate incoming;
        QVERIFY2(Import::Prepare(p, document->id, "official", "revision", Artwork::ColorPolicy::PreserveValues, {}, incoming, &error), qPrintable(error));
        int pixels = 0, groups = 0; QStringList names;
        for (const auto& layer : incoming.revision.layers) QVERIFY2(layer.pixelEquivalent, qPrintable(layer.diagnostics.join('\n')));
        for (const auto& layer : incoming.revision.layers) { if (layer.kind == Artwork::LayerKind::Group) ++groups; else { ++pixels; names.append(layer.name); } }
        QCOMPARE(pixels, 58); QCOMPARE(groups, 15); QVERIFY(names.contains(QStringLiteral("ハイライト"))); QVERIFY(names.contains(QStringLiteral("唇")));
        Library::Plan result; const auto before = snapshot();
        QVERIFY2(Library::Build(before, {{incoming, document->name}}, result, &error), qPrintable(error));
        CPsdSourceCatalog recovered; QVERIFY2(CArtworkPsdRecovery::Prepare(result.state.artwork, recovered, &error), qPrintable(error));
        QCOMPARE(recovered.documents.size(), 1); QVERIFY(recovered.documents[0].Reconstructed());
        QCOMPARE(recovered.documents[0].Reconstructed()->layers.size(), 73);
    }
};
QTEST_APPLESS_MAIN(CmoSourceLibraryTests)
#include "cmo_source_library_tests.moc"
