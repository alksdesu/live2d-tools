#include "geometry_object_pairs_document_fixture.h"
#include "live2dstruct/calphameshsource.h"
#include "std/cexportcontrol.h"
#include <QtTest>
#include <QBuffer>
#include <QImage>

using namespace live2dstruct;
using Source = CAlphaMeshSource;
using Artwork = CModelArtwork;
namespace {
QByteArray png(QSize size, int alpha = 173)
{
    QImage image(size, QImage::Format_RGBA8888);
    for (int y = 0; y < size.height(); ++y) for (int x = 0; x < size.width(); ++x)
        image.setPixelColor(x, y, QColor((17 * x) % 256, (23 * y) % 256, 59, alpha));
    QBuffer output; output.open(QIODevice::WriteOnly); image.save(&output, "PNG"); return output.data();
}
bool load(CModelDocument& document, QString* error)
{
    if (!GeometryPairsData::load(document, false, error)) return false;
    CTextureAssets::Source source;
    if (!CTextureAssets::MakeSource("/tmp/missing-alpha-source.png", png({16, 16}), source, error) ||
        !document.InitializeTextureWorkspace({{0, source}}, {}, error)) return false;
    CModelDrawableMaterial material;
    if (!document.CaptureDrawableMaterial("Mesh", material, error)) return false;
    material.uvs = {.25f, .25f, .75f, .25f, .25f, .75f};
    return document.SetDrawableMaterials({material}, error);
}
bool associate(CModelDocument& document, const QTransform& transform, bool opacity, bool repeat, QString* error)
{
    Artwork::State state; const QByteArray bytes = png({8, 6});
    const auto digest = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
    state.images.insert(digest, {{8, 6}, bytes});
    state.families.insert("family", {"family", "原稿", Artwork::SourceKind::Psd, "new"});
    Artwork::Revision revision; revision.id = "old"; revision.familyId = "family"; revision.fileName = "old.psd";
    revision.sourceBytes = "owned missing PSD"; revision.sourceDigest = QCryptographicHash::hash(revision.sourceBytes, QCryptographicHash::Sha256);
    revision.canvasSize = {1024, 768}; revision.documentToFamily = transform;
    Artwork::Layer layer; layer.id = "layer"; layer.name = "眼睛"; layer.bounds = {-3, 4, 8, 6}; layer.imageDigest = digest; layer.opacity = 128;
    revision.layers = {layer}; state.revisions.insert("old", revision);
    auto newer = revision; newer.id = "new"; newer.fileName = "new.psd"; newer.layers[0].name = "重命名"; state.revisions.insert("new", newer);
    state.materials.insert("material", {"material", "", "family", "old", "layer", repeat ? Artwork::Boundary::Repeat : Artwork::Boundary::Transparent, opacity, "new"});
    const auto toAtlas = transform.inverted() * QTransform::fromTranslate(3, -4) * QTransform::fromScale(2, 2) * QTransform::fromTranslate(2, 2);
    state.placements.insert("placement", {"placement", "material", document.TextureWorkspace().atlases[0].id, toAtlas,
        transform.mapRect(QRectF(layer.bounds)), 1, Artwork::Filter::ExactTexels});
    Artwork::Binding binding; binding.meshId = "Mesh"; binding.placementId = "placement";
    for (const QPointF point : {QPointF(1, 1), QPointF(5, 1), QPointF(1, 5)}) binding.familyCoordinates.append(transform.map(point + layer.bounds.topLeft()));
    if (!document.CaptureDrawableMaterial("Mesh", binding.sampledMaterial, error) ||
        !Artwork::TopologyDigest(3, document.DrawableIndices("Mesh"), binding.topologyDigest, error)) return false;
    state.bindings.insert("Mesh", binding); return document.SetArtwork(state, error);
}
}
class AlphaMeshSourceTests : public QObject
{
    Q_OBJECT
private slots:
    void effectiveAtlasUsesAuthorUvAndWarnsAboutCoverage()
    {
        CModelDocument document; QString error; QVERIFY2(load(document, &error), qPrintable(error));
        Source::Source result; QVERIFY2(Source::Capture(document, "Mesh", result, &error), qPrintable(error));
        QCOMPARE(result.input.pixelCoordinates, QVector<QPointF>({{4, 4}, {12, 4}, {4, 12}}));
        QCOMPARE(result.input.uvs, QVector<float>({.25f, .25f, .75f, .25f, .25f, .75f}));
        QCOMPARE(result.input.referenceSize, QSize(16, 16)); QCOMPARE(result.input.image.size(), QSize(16, 16));
        QVERIFY(result.input.repeat); QVERIFY(!result.linkedSource); QVERIFY(result.detail.contains("UV"));
        QCOMPARE(result.input.indices, document.DrawableIndices("Mesh")); QCOMPARE(result.input.sourceToken.size(), 32);
        QCOMPARE(result.input.image.pixelColor(3, 7), QColor(51, 161, 59, 173));
    }
    void pinnedLayerUsesInverseTransformAndOpacity_data()
    {
        QTest::addColumn<QTransform>("transform"); QTest::addColumn<bool>("opacity"); QTest::addColumn<bool>("repeat");
        QTransform rotated; rotated.rotate(90); rotated.scale(2, .5); rotated.translate(11, -7);
        for (const auto& pair : {qMakePair(QString("identity"), QTransform()), qMakePair(QString("rotated"), rotated),
                                qMakePair(QString("mirrored"), QTransform(-2, 0, 0, .5, 23, -19))})
            for (bool opacity : {false, true}) for (bool repeat : {false, true})
                QTest::newRow(qPrintable(pair.first + QString::number(opacity) + QString::number(repeat))) << pair.second << opacity << repeat;
    }
    void pinnedLayerUsesInverseTransformAndOpacity()
    {
        QFETCH(QTransform, transform); QFETCH(bool, opacity); QFETCH(bool, repeat);
        CModelDocument document; QString error; QVERIFY2(load(document, &error), qPrintable(error));
        QVERIFY2(associate(document, transform, opacity, repeat, &error), qPrintable(error));
        Source::Source result; QVERIFY2(Source::Capture(document, "Mesh", result, &error), qPrintable(error));
        QCOMPARE(result.input.pixelCoordinates, QVector<QPointF>({{1, 1}, {5, 1}, {1, 5}}));
        QCOMPARE(result.input.uvs, QVector<float>({.25f, .25f, .75f, .25f, .25f, .75f}));
        QCOMPARE(result.input.image.size(), QSize(8, 6)); QCOMPARE(result.input.referenceSize, QSize(1024, 768));
        QCOMPARE(result.input.image.pixelColor(3, 2), QColor(51, 46, 59, opacity ? 87 : 173));
        QCOMPARE(result.input.repeat, repeat); QVERIFY(result.linkedSource); QVERIFY(result.detail.contains("old.psd")); QVERIFY(!result.detail.contains("new.psd"));
        QCOMPARE(document.Artwork().images.first().png, png({8, 6}));
    }
    void detachedBindingFallsBackToCurrentPixels()
    {
        CModelDocument document; QString error; QVERIFY(load(document, &error)); QVERIFY(associate(document, {}, true, false, &error));
        auto state = document.TextureWorkspace(); state.atlases[0].png = png({16, 16}, 31);
        QVERIFY2(document.SetTextureWorkspace(state, &error), qPrintable(error));
        QCOMPARE(document.Artwork().bindings["Mesh"].status, Artwork::BindingStatus::Detached);
        Source::Source result; QVERIFY2(Source::Capture(document, "Mesh", result, &error), qPrintable(error));
        QVERIFY(!result.linkedSource); QVERIFY(result.detail.contains("分离")); QCOMPARE(result.input.image.pixelColor(0, 0).alpha(), 31);
    }
    void generatedAtlasUsesNewSlotAndSize()
    {
        CModelDocument document; QString error; QVERIFY(load(document, &error)); CModelAtlasState before; QVERIFY(document.CaptureAtlasState(before));
        auto after = before; after.workspace.atlases[1] = {"generated", "新图集", -1, {32, 8}, png({32, 8}, 67)};
        for (auto& material : after.materials) if (material.id == "Mesh") material.textureIndex = 1;
        CModelEdit edit; QVERIFY2(document.MakeAtlasEdit(before, after, edit, &error), qPrintable(error)); QVERIFY(document.ExecuteEdit(edit));
        Source::Source result; QVERIFY2(Source::Capture(document, "Mesh", result, &error), qPrintable(error));
        QCOMPARE(result.input.pixelCoordinates, QVector<QPointF>({{8, 2}, {24, 2}, {8, 6}}));
        QCOMPARE(result.input.image.size(), QSize(32, 8)); QCOMPARE(result.input.image.pixelColor(0, 0).alpha(), 67);
    }
    void progressCannotChangeFrozenSource()
    {
        CModelDocument document; QString error; QVERIFY(load(document, &error)); QVERIFY(associate(document, {}, true, false, &error));
        Source::Source expected; QVERIFY(Source::Capture(document, "Mesh", expected, &error));
        QChar raw[]{'M', 'e', 's', 'h'}; CExportControl control; bool modified = false;
        QVERIFY(control.SetHandler([&](const auto&, QString*) { document = {}; raw[0] = 'X'; modified = true; return true; }));
        Source::Source result; QVERIFY2(Source::Capture(document, QString::fromRawData(raw, 4), result, &error, &control), qPrintable(error));
        QVERIFY(modified); QCOMPARE(result.input.artMeshId, QString("Mesh")); QCOMPARE(result.input.sourceToken, expected.input.sourceToken);
        QCOMPARE(result.input.pixelCoordinates, expected.input.pixelCoordinates); QCOMPARE(result.input.image, expected.input.image);
    }
    void sourceAlphaDoesNotUseDrawableVisibilityOrOpacity()
    {
        CModelDocument document; QString error; QVERIFY(load(document, &error)); QVERIFY(associate(document, {}, false, false, &error));
        Source::Source before, after; QVERIFY(Source::Capture(document, "Mesh", before, &error));
        QVERIFY(document.SetDrawableVisible("Mesh", false)); QVERIFY(document.SetDrawableOpacity("Mesh", 0));
        QVERIFY(Source::Capture(document, "Mesh", after, &error)); QCOMPARE(after.input.image, before.input.image);
        QCOMPARE(after.input.pixelCoordinates, before.input.pixelCoordinates);
    }
    void emptyLayerAndZeroOpacityRemainTransparent_data()
    {
        QTest::addColumn<bool>("empty"); QTest::newRow("empty-layer") << true; QTest::newRow("zero-opacity") << false;
    }
    void emptyLayerAndZeroOpacityRemainTransparent()
    {
        QFETCH(bool, empty); CModelDocument document; QString error; QVERIFY(load(document, &error)); QVERIFY(associate(document, {}, true, false, &error));
        auto state = document.Artwork(); auto& layer = state.revisions["old"].layers[0];
        if (empty) { layer.bounds.setSize({0, 0}); layer.imageDigest.clear(); } else layer.opacity = 0;
        QVERIFY2(document.SetArtwork(state, &error), qPrintable(error)); Source::Source result;
        QVERIFY2(Source::Capture(document, "Mesh", result, &error), qPrintable(error));
        QCOMPARE(result.input.image.size(), empty ? QSize(1, 1) : QSize(8, 6));
        for (int y = 0; y < result.input.image.height(); ++y) for (int x = 0; x < result.input.image.width(); ++x)
            QCOMPARE(result.input.image.pixelColor(x, y).alpha(), 0);
    }
    void tokenTracksAcceptedSourceAndMeshChanges()
    {
        CModelDocument document; QString error; QVERIFY(load(document, &error)); Source::Source first, same, changed;
        QVERIFY(Source::Capture(document, "Mesh", first, &error)); QVERIFY(Source::Capture(document, "Mesh", same, &error));
        QCOMPARE(first.input.sourceToken, same.input.sourceToken);
        CModelDrawableMaterial material; QVERIFY(document.CaptureDrawableMaterial("Mesh", material)); material.uvs[0] += .125f;
        QVERIFY(document.SetDrawableMaterials({material}, &error)); QVERIFY(Source::Capture(document, "Mesh", changed, &error));
        QVERIFY(first.input.sourceToken != changed.input.sourceToken);
        const auto uvToken = changed.input.sourceToken; auto state = document.TextureWorkspace(); state.atlases[0].png = png({16, 16}, 31);
        QVERIFY(document.SetTextureWorkspace(state, &error)); QVERIFY(Source::Capture(document, "Mesh", changed, &error)); QVERIFY(uvToken != changed.input.sourceToken);
    }
    void failuresPreserveOutput_data()
    {
        QTest::addColumn<int>("mode"); for (int mode = 0; mode < 4; ++mode) QTest::newRow(qPrintable(QString::number(mode))) << mode;
    }
    void failuresPreserveOutput()
    {
        QFETCH(int, mode); CModelDocument document; QString error; QVERIFY(load(document, &error));
        Source::Source result; result.label = "sentinel"; result.input.image = QImage(2, 3, QImage::Format_RGBA8888); result.input.image.fill(Qt::red);
        const auto expected = result.input.image; CExportControl control;
        if (mode == 0) control.Cancel();
        if (mode == 1) QVERIFY(control.SetHandler([](const auto&, QString*) { return false; }));
        if (mode == 2) document = {};
        QVERIFY(!Source::Capture(document, mode == 3 ? "missing" : "Mesh", result, &error, &control));
        QVERIFY(!error.isEmpty()); QCOMPARE(result.label, QString("sentinel")); QCOMPARE(result.input.image, expected);
    }
    void glueEndpointsArePinnedFromCurrentModel()
    {
        CModelDocument document; QString error; QVERIFY(document.Load(DeformerSnapshotData::sample("Natori"), &error));
        CTextureAssets::Source source; QVERIFY(CTextureAssets::MakeSource("/tmp/natori.png", png({16, 16}), source)); QVERIFY(document.InitializeTextureWorkspace({{0, source}}));
        auto reader = CReader::FromBytes(document.SourceBytes()); CSectionTable table(reader); QVERIFY(!table.glue.isEmpty());
        const auto* glue = table.glue.first(); const int index = glue->art_mesh_indices_a; const auto id = table.art_meshes[index]->ids;
        QSet<int> expected;
        for (const auto* item : table.glue) for (int row = 0; row < item->glue_info_sources_counts; ++row)
            if ((row % 2 ? item->art_mesh_indices_b : item->art_mesh_indices_a) == index)
                expected.insert(table.glue_info[item->glue_info_sources_begin_indices + row]->position_indices);
        Source::Source result; QVERIFY2(Source::Capture(document, id, result, &error), qPrintable(error));
        QCOMPARE(QSet<int>(result.input.pinnedVertices.cbegin(), result.input.pinnedVertices.cend()), expected); QVERIFY(!expected.isEmpty());
    }
};
QTEST_GUILESS_MAIN(AlphaMeshSourceTests)
#include "alpha_mesh_source_tests.moc"
