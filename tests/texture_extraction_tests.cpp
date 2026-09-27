#include "live2dstruct/csectiontable.h"
#include "opengl/ctextureextractor.h"
#include "std/cexportcontrol.h"
#include "wide_mesh_fixture.h"

#include <QtTest>
#include <QBuffer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <algorithm>
#include <limits>

using namespace live2dstruct;

namespace {
QImage ColoredAtlas(int size = 4)
{
    QImage image(size, size, QImage::Format_ARGB32);
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x)
            image.setPixel(x, y, qRgba(12 + x * 20, 18 + y * 18, 30 + x * 3 + y * 5, 70 + x * 4 + y * 6));
    return image;
}

QByteArray Png(const QImage& image)
{
    QByteArray bytes; QBuffer buffer(&bytes);
    return buffer.open(QIODevice::WriteOnly) && image.save(&buffer, "PNG") ? bytes : QByteArray();
}

bool HasVisiblePixel(const QImage& image)
{
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x)
            if (qAlpha(image.pixel(x, y))) return true;
    return false;
}

class MeshFixture
{
public:
    MeshFixture() : reader(QString(TEST_RESOURCES) + "/Mark/Mark.moc3"), table(reader)
    {
        if (!IsValid()) return;
        table.count_info->art_meshes = 1;
        auto* mesh = table.art_meshes[0];
        mesh->texture_nos = 0;
        mesh->uv_sources_begin_indices = 0;
        mesh->position_index_sources_begin_indices = 0;
        mesh->keyform_sources_begin_indices = -1;
    }

    bool IsValid() const { return temp.isValid() && table.IsValid() && !table.art_meshes.empty(); }
    bool LoadAtlas(const QImage& image)
    {
        const QString path = temp.filePath("atlas.png");
        return image.save(path, "PNG") && extractor.LoadAtlases({path});
    }
    void Geometry(const std::vector<float>& uvs, const std::vector<quint16>& indices = {0, 1, 2})
    {
        table.uvs->uvs = uvs;
        table.position_indices->indices = indices;
        table.art_meshes[0]->vertex_counts = static_cast<int>(uvs.size() / 2);
        table.art_meshes[0]->position_index_sources_counts = static_cast<int>(indices.size());
    }
    void CanvasPositions(const std::vector<float>& positions, float ppu, float x = 0, float y = 0)
    {
        table.keyform_positions->xys = positions;
        table.art_meshes[0]->keyform_sources_begin_indices = 0;
        table.art_mesh_keyforms[0]->keyform_position_sources_begin_indices = 0;
        table.canvas_info->pixels_per_unit = ppu;
        table.canvas_info->origin_x = x;
        table.canvas_info->origin_y = y;
    }
    void Masks(int count, bool inverted)
    {
        table.art_meshes[0]->drawable_mask_sources_begin_indices = 0;
        table.art_meshes[0]->drawable_mask_sources_counts = count;
        table.art_meshes[0]->drawable_flags = inverted ? 0x08 : 0;
        for (int index = 0; index < count; ++index)
            table.drawable_masks[index]->art_mesh_sources_indices = index + 1;
    }
    void MeshCount(int count)
    {
        table.count_info->art_meshes = count;
        for (int index = 1; index < count; ++index) {
            const QString id = table.art_meshes[index]->ids;
            *table.art_meshes[index] = *table.art_meshes[0];
            table.art_meshes[index]->ids = id;
        }
    }
    void AdditionalGeometry(const std::vector<float>& uvs)
    {
        MeshCount(2); auto* mesh = table.art_meshes[1];
        mesh->vertex_counts = uvs.size() / 2; mesh->uv_sources_begin_indices = table.uvs->uvs.size();
        mesh->position_index_sources_counts = 3;
        table.uvs->uvs.insert(table.uvs->uvs.end(), uvs.begin(), uvs.end());
    }
    void TextureSlot(int slot) { table.art_meshes[0]->texture_nos = slot; }
    bool LoadBytes(const QVector<QByteArray>& bytes, CExportControl* control = nullptr)
    {
        return extractor.LoadAtlasBytes(bytes, control);
    }
    const std::vector<float>& Uvs() const { return table.uvs->uvs; }
    const std::vector<quint16>& Indices() const { return table.position_indices->indices; }
    QString WriteAtlas(const QImage& image, const QString& name)
    {
        const QString path = temp.filePath(name);
        return image.save(path, "PNG") ? path : QString();
    }
    bool Reload(const QVector<QString>& paths, CExportControl* control = nullptr)
    {
        return extractor.LoadAtlases(paths, control);
    }
    QVector<ExtractedLayer> Extract(CExportControl* control = nullptr,
                                   const QHash<QString, QVector<quint16>>* overrides = nullptr,
                                   const QHash<QString, QVector<float>>* positions = nullptr)
    {
        return extractor.ExtractAll(&table, true, overrides, control, positions);
    }

    QString Id() const { return table.art_meshes[0]->ids; }

private:
    CReader reader;
    CSectionTable table;
    QTemporaryDir temp;
    CTextureExtractor extractor;
};
}

class TextureExtractionTests : public QObject
{
    Q_OBJECT
private slots:
    void byteSnapshotsPreserveSparseSlotsAndStraightAlpha()
    {
        MeshFixture mesh; QVERIFY(mesh.IsValid());
        const auto atlas = ColoredAtlas(); const auto png = Png(atlas); QVERIFY(!png.isEmpty());
        mesh.Geometry({0, 0, 1, 0, 1, 1, 0, 1}, {0, 1, 2, 0, 2, 3});
        mesh.TextureSlot(1); QVERIFY(mesh.LoadBytes({{}, png}));
        const auto layers = mesh.Extract(); QCOMPARE(layers.size(), 1); QCOMPARE(layers[0].image, atlas);
        mesh.TextureSlot(0); QVERIFY(mesh.Extract().isEmpty());
        QVERIFY(mesh.LoadBytes({png})); QCOMPARE(mesh.Extract()[0].image, atlas);
        QVERIFY(mesh.LoadAtlas(atlas)); QCOMPARE(mesh.Extract()[0].image, atlas);
    }

    void failedOrCanceledByteReloadIsAtomic()
    {
        MeshFixture mesh; QVERIFY(mesh.IsValid());
        const auto atlas = ColoredAtlas(); QVERIFY(mesh.LoadAtlas(atlas));
        mesh.Geometry({0, 0, 1, 0, 1, 1, 0, 1}, {0, 1, 2, 0, 2, 3});
        QImage replacement(4, 4, QImage::Format_ARGB32); replacement.fill(qRgba(190, 21, 101, 74));
        const auto png = Png(replacement);
        for (const auto& invalid : QVector<QVector<QByteArray>>{{}, {png, "invalid"}, {png, png.left(33)}}) {
            CExportControl control; QVERIFY(!mesh.LoadBytes(invalid, &control));
            QVERIFY(!control.Error().isEmpty()); QCOMPARE(mesh.Extract()[0].image, atlas);
        }
        CExportControl canceled; bool reached = false;
        QVERIFY(canceled.SetHandler([&](const CExportControl::Progress& progress, QString*) {
            if (progress.phase == CExportControl::Phase::Preparing && progress.current == 1 && progress.total == 2) {
                reached = true; canceled.Cancel();
            }
            return true;
        }));
        QVERIFY(!mesh.LoadBytes({png, png}, &canceled)); QVERIFY(reached); QVERIFY(canceled.WasCanceled());
        QCOMPARE(mesh.Extract()[0].image, atlas);
        QVERIFY(mesh.LoadBytes({png})); QCOMPARE(mesh.Extract()[0].image, replacement);
    }

    void byteReloadCapturesInputBeforeProgressCallbacks()
    {
        MeshFixture mesh; QVERIFY(mesh.IsValid());
        const auto atlas = ColoredAtlas(); QVector<QByteArray> bytes{Png(atlas)};
        mesh.Geometry({0, 0, 1, 0, 1, 1, 0, 1}, {0, 1, 2, 0, 2, 3});
        CExportControl control; bool called = false;
        QVERIFY(control.SetHandler([&](const CExportControl::Progress&, QString*) { called = true; bytes.clear(); return true; }));
        QVERIFY(mesh.LoadBytes(bytes, &control)); QVERIFY(called); QVERIFY(bytes.isEmpty());
        QCOMPARE(mesh.Extract()[0].image, atlas);
    }

    void frozenModelCoordinatesReplaceParentLocalPlacement()
    {
        MeshFixture mesh; QVERIFY(mesh.IsValid());
        QVERIFY(mesh.LoadAtlas(ColoredAtlas()));
        mesh.Geometry({0, 0, 1, 0, 0, 1});
        mesh.CanvasPositions({100, 100, 200, 100, 100, 300}, 100, 50, 50);
        const auto original = mesh.Extract(); QCOMPARE(original.size(), 1);
        QHash<QString, QVector<float>> positions{{mesh.Id(), {-0.5f, -0.5f, 0.5f, -0.5f, -0.5f, 0.5f}}};
        const auto saved = positions;
        CExportControl control;
        const auto frozen = mesh.Extract(&control, nullptr, &positions);
        QVERIFY(control.Checkpoint()); QCOMPARE(frozen.size(), 1);
        QCOMPARE(frozen[0].canvasRect, QRect(0, 0, 100, 100));
        QVERIFY(original[0].canvasRect != frozen[0].canvasRect);
        QCOMPARE(frozen[0].image.size(), frozen[0].canvasRect.size());
        QVERIFY(HasVisiblePixel(frozen[0].image));
        QCOMPARE(frozen[0].image.pixel(99, 0), QRgb(0));
        QCOMPARE(positions, saved);
        QCOMPARE(mesh.Extract()[0].canvasRect, original[0].canvasRect);
    }

    void incompleteOrInvalidFrozenCoordinatesNeverFallBackToLocalPlacement()
    {
        MeshFixture mesh; QVERIFY(mesh.IsValid());
        QVERIFY(mesh.LoadAtlas(ColoredAtlas()));
        mesh.Geometry({0, 0, 1, 0, 0, 1});
        mesh.CanvasPositions({0, 0, 1, 0, 0, 1}, 10);
        const auto baseline = mesh.Extract(); QCOMPARE(baseline.size(), 1);
        const float infinity = std::numeric_limits<float>::infinity();
        const float largest = std::numeric_limits<float>::max();
        const QVector<QHash<QString, QVector<float>>> inputs{
            {}, {{"unknown", {0, 0, 1, 0, 0, 1}}}, {{mesh.Id(), {0, 0}}},
            {{mesh.Id(), {0, 0, infinity, 0, 0, 1}}}, {{mesh.Id(), {0, 0, largest, 0, 0, 1}}},
            {{mesh.Id(), {0, 0, 0, 0, 0, 0}}}};
        for (const auto& input : inputs) {
            CExportControl control;
            QVERIFY(mesh.Extract(&control, nullptr, &input).isEmpty());
            QVERIFY(!control.Checkpoint()); QVERIFY(!control.Error().isEmpty());
            QCOMPARE(mesh.Extract()[0].image, baseline[0].image);
        }
        mesh.MeshCount(2);
        const QHash<QString, QVector<float>> partial{{mesh.Id(), {0, 0, 1, 0, 0, 1}}};
        CExportControl control;
        QVERIFY(mesh.Extract(&control, nullptr, &partial).isEmpty());
        QVERIFY(!control.Checkpoint());
    }

    void frozenSamplingIsInvariantUnderAtlasRotationResizeAndSlotMove()
    {
        MeshFixture mesh; QVERIFY(mesh.IsValid());
        QImage atlas(6, 4, QImage::Format_ARGB32);
        for (int y = 0; y < atlas.height(); ++y)
            for (int x = 0; x < atlas.width(); ++x)
                atlas.setPixel(x, y, qRgba(20 + x * 30, 40 + y * 40, x * 17 + y * 11, (x + y) % 3 ? 180 : 0));
        QVERIFY(mesh.LoadAtlas(atlas));
        const std::vector<float> uvs{0.125f, 0.125f, 0.875f, 0.125f, 0.875f, 0.875f, 0.125f, 0.875f};
        mesh.Geometry(uvs, {0, 1, 2, 0, 2, 3});
        mesh.CanvasPositions({}, 1);
        const QHash<QString, QVector<float>> positions{{mesh.Id(), {-3, 1, 8, 0, 10, -9, -1, -11}}};
        CExportControl beforeControl;
        const auto before = mesh.Extract(&beforeControl, nullptr, &positions);
        QVERIFY2(beforeControl.Checkpoint(), qPrintable(beforeControl.Error())); QCOMPARE(before.size(), 1);
        QCOMPARE(before[0].canvasRect, QRect(-3, -1, 13, 12));
        QCOMPARE(before[0].image.size(), before[0].canvasRect.size());
        for (bool rotated : {false, true}) {
            QImage packed(16, 16, QImage::Format_ARGB32);
            const auto repeat = [](int value, int size) { return (value % size + size) % size; };
            for (int y = 0; y < packed.height(); ++y)
                for (int x = 0; x < packed.width(); ++x) {
                    const int sourceX = rotated ? y - 4 : x - 4;
                    const int sourceY = rotated ? 3 - (x - 4) : y - 4;
                    packed.setPixel(x, y, atlas.pixel(repeat(sourceX, 6), repeat(sourceY, 4)));
                }
            std::vector<float> transformed;
            for (size_t index = 0; index < uvs.size(); index += 2) {
                transformed.push_back((4 + (rotated ? 4 - uvs[index + 1] * 4 : uvs[index] * 6)) / 16);
                transformed.push_back((4 + (rotated ? uvs[index] * 6 : uvs[index + 1] * 4)) / 16);
            }
            mesh.Geometry(transformed, {0, 1, 2, 0, 2, 3}); mesh.TextureSlot(2);
            QVERIFY(mesh.LoadBytes({Png(atlas), {}, Png(packed)}));
            CExportControl control; const auto after = mesh.Extract(&control, nullptr, &positions);
            QVERIFY2(control.Checkpoint(), qPrintable(control.Error())); QCOMPARE(after.size(), 1);
            QCOMPARE(after[0].canvasRect, before[0].canvasRect);
            QCOMPARE(after[0].image, before[0].image);
            QCOMPARE(after[0].textureNo, 2);
        }
    }

    void frozenSamplingUsesStraightBilinearRgbaAndRepeat()
    {
        MeshFixture mesh; QVERIFY(mesh.IsValid());
        QImage atlas(2, 2, QImage::Format_ARGB32);
        atlas.setPixel(0, 0, qRgba(200, 0, 0, 0)); atlas.setPixel(1, 0, qRgba(0, 100, 0, 64));
        atlas.setPixel(0, 1, qRgba(0, 0, 80, 128)); atlas.setPixel(1, 1, qRgba(0, 0, 0, 240));
        QVERIFY(mesh.LoadAtlas(atlas)); mesh.CanvasPositions({}, 1);
        const QHash<QString, QVector<float>> positions{{mesh.Id(), {0, 0, 1, 0, 1, -1, 0, -1}}};
        for (float coordinate : {0.5f, -0.5f, 10.5f}) {
            mesh.Geometry({coordinate, coordinate, coordinate, coordinate, coordinate, coordinate, coordinate, coordinate}, {0, 1, 2, 0, 2, 3});
            CExportControl control; const auto layers = mesh.Extract(&control, nullptr, &positions);
            QVERIFY2(control.Checkpoint(), qPrintable(control.Error())); QCOMPARE(layers.size(), 1);
            QCOMPARE(layers[0].image.format(), QImage::Format_ARGB32);
            QCOMPARE(layers[0].image.pixel(0, 0), qRgba(50, 25, 20, 108));
        }
        const float large = std::numeric_limits<float>::max();
        mesh.Geometry({large, -large, large, -large, large, -large, large, -large}, {0, 1, 2, 0, 2, 3});
        CExportControl control; const auto layers = mesh.Extract(&control, nullptr, &positions);
        QVERIFY2(control.Checkpoint(), qPrintable(control.Error())); QCOMPARE(layers.size(), 1);
        QCOMPARE(layers[0].image.pixel(0, 0), qRgba(50, 25, 20, 108));
        mesh.Geometry({0.25f, 0.25f, 0.25f, 0.25f, 0.25f, 0.25f, 0.25f, 0.25f}, {0, 1, 2, 0, 2, 3});
        QCOMPARE(mesh.Extract(nullptr, nullptr, &positions)[0].image.pixel(0, 0), qRgba(200, 0, 0, 0));
    }

    void frozenSamplingRejectsInvalidTopologyAndBudgetsBeforeRasterization()
    {
        MeshFixture mesh; QVERIFY(mesh.IsValid()); QVERIFY(mesh.LoadAtlas(ColoredAtlas()));
        mesh.Geometry({0, 0, 1, 0, 0, 1}); mesh.CanvasPositions({}, 1);
        const QHash<QString, QVector<float>> positions{{mesh.Id(), {0, 0, 1024, 0, 0, -1024}}};
        for (const QVector<quint16>& indices : QVector<QVector<quint16>>{{}, {0, 1}, {0, 1, 3}}) {
            const QHash<QString, QVector<quint16>> overrides{{mesh.Id(), indices}};
            CExportControl control; QVERIFY(mesh.Extract(&control, &overrides, &positions).isEmpty());
            QVERIFY(!control.Error().isEmpty());
        }
        std::vector<quint16> repeated;
        for (int triangle = 0; triangle < 513; ++triangle) repeated.insert(repeated.end(), {0, 1, 2});
        mesh.Geometry({0, 0, 1, 0, 0, 1}, repeated);
        CExportControl work; bool scanned = false;
        QVERIFY(work.SetHandler([&](const CExportControl::Progress& progress, QString*) {
            if (progress.detail == QStringLiteral("采样画布三角形")) scanned = true;
            return true;
        }));
        QVERIFY(mesh.Extract(&work, nullptr, &positions).isEmpty());
        QVERIFY(work.Error().contains(QStringLiteral("扫描总量"))); QVERIFY(!scanned);
        QImage largeAtlas(1024, 1024, QImage::Format_ARGB32); largeAtlas.fill(Qt::white);
        QVERIFY(mesh.LoadAtlas(largeAtlas));
        CExportControl rawWork; QVERIFY(mesh.Extract(&rawWork).isEmpty());
        QVERIFY(rawWork.Error().contains(QStringLiteral("扫描总量")));
        mesh.Geometry({0, 0, 1, 0, 0, 1});
        const QHash<QString, QVector<float>> large{{mesh.Id(), {0, 0, 16384, 0, 0, -16384}}};
        CExportControl pixels; QVERIFY(mesh.Extract(&pixels, nullptr, &large).isEmpty());
        QVERIFY(pixels.Error().contains(QStringLiteral("像素总量")));
    }

    void frozenSamplingSkipsValidEmptyDrawables()
    {
        MeshFixture mesh; QVERIFY(mesh.IsValid()); QVERIFY(mesh.LoadAtlas(ColoredAtlas()));
        mesh.CanvasPositions({}, 1);
        const QHash<QString, QVector<float>> positions;
        for (const auto& uvs : std::vector<std::vector<float>>{{}, {0, 0, 1, 0, 0, 1}}) {
            mesh.Geometry(uvs, {});
            CExportControl control; QVERIFY(mesh.Extract(&control, nullptr, &positions).isEmpty());
            QVERIFY2(control.Checkpoint(), qPrintable(control.Error()));
        }
        mesh.Geometry({}, {0, 0, 0});
        CExportControl invalid; QVERIFY(mesh.Extract(&invalid, nullptr, &positions).isEmpty());
        QVERIFY(!invalid.Error().isEmpty());
    }

    void frozenSamplingCancellationDiscardsLayersAndAllowsRetry()
    {
        MeshFixture mesh; QVERIFY(mesh.IsValid()); QVERIFY(mesh.LoadAtlas(ColoredAtlas()));
        mesh.Geometry({0, 0, 1, 0, 0, 1}); mesh.CanvasPositions({}, 1);
        const QHash<QString, QVector<float>> positions{{mesh.Id(), {0, 0, 16, 0, 0, -16}}};
        CExportControl control; bool reached = false;
        QVERIFY(control.SetHandler([&](const CExportControl::Progress& progress, QString*) {
            if (progress.detail == QStringLiteral("采样画布三角形") && progress.current == 8) {
                reached = true; control.Cancel();
            }
            return true;
        }));
        QVERIFY(mesh.Extract(&control, nullptr, &positions).isEmpty()); QVERIFY(reached); QVERIFY(control.WasCanceled());
        CExportControl retry; const auto layers = mesh.Extract(&retry, nullptr, &positions);
        QVERIFY(retry.Checkpoint()); QCOMPARE(layers.size(), 1); QCOMPARE(layers[0].image.size(), QSize(16, 16));
        QVERIFY(HasVisiblePixel(layers[0].image));
    }

    void frozenSamplingSupportsUnsignedIndicesAndDegenerateTriangles()
    {
        MeshFixture mesh; QVERIFY(mesh.IsValid()); QVERIFY(mesh.LoadAtlas(ColoredAtlas())); mesh.CanvasPositions({}, 1);
        mesh.Geometry({0, 0, 1, 0, 0, 1});
        QHash<QString, QVector<float>> positions{{mesh.Id(), {0, 0, 4, 0, 0, -4}}};
        const auto expected = mesh.Extract(nullptr, nullptr, &positions); QCOMPARE(expected.size(), 1);
        std::vector<float> uvs(65536 * 2, 0.0f);
        uvs[65534 * 2] = 1; uvs[65535 * 2 + 1] = 1;
        mesh.Geometry(uvs, {65533, 65534, 65535});
        QVector<float> points(65536 * 2, 0.0f); points[65534 * 2] = 4; points[65535 * 2 + 1] = -4;
        positions[mesh.Id()] = points;
        CExportControl control; const auto actual = mesh.Extract(&control, nullptr, &positions);
        QVERIFY2(control.Checkpoint(), qPrintable(control.Error())); QCOMPARE(actual.size(), 1);
        QCOMPARE(actual[0].image, expected[0].image); QCOMPARE(actual[0].canvasRect, expected[0].canvasRect);
        mesh.Geometry({0, 0, 1, 0, 0, 1}); positions[mesh.Id()] = {0, 0, 2, -2, 4, -4};
        const auto degenerate = mesh.Extract(nullptr, nullptr, &positions); QCOMPARE(degenerate.size(), 1);
        QCOMPARE(degenerate[0].image.size(), QSize(4, 4)); QVERIFY(!HasVisiblePixel(degenerate[0].image));
    }

    void cancellationInsideSingleTriangleDiscardsEverythingAndAllowsRetry()
    {
        MeshFixture mesh;
        QVERIFY(mesh.IsValid());
        const QImage atlas = ColoredAtlas(512);
        QVERIFY(mesh.LoadAtlas(atlas));
        mesh.Geometry({0, 0, 1, 0, 0, 1});
        const auto expected = mesh.Extract();
        QCOMPARE(expected.size(), 1);
        const auto originalUvs = mesh.Uvs();
        const auto originalIndices = mesh.Indices();
        CExportControl control;
        bool reachedMiddle = false;
        QVERIFY(control.SetHandler([&](const CExportControl::Progress& progress, QString*) {
            if (progress.phase == CExportControl::Phase::Extracting &&
                progress.total == 512 && progress.current == 256) {
                reachedMiddle = true;
                control.Cancel();
            }
            return true;
        }));
        QVERIFY(mesh.Extract(&control).isEmpty());
        QVERIFY(reachedMiddle);
        QVERIFY(control.WasCanceled());
        QVERIFY(!control.Error().isEmpty());
        QVERIFY(mesh.Uvs() == originalUvs);
        QVERIFY(mesh.Indices() == originalIndices);
        const auto retry = mesh.Extract();
        QCOMPARE(retry.size(), 1);
        QCOMPARE(retry[0].image, expected[0].image);
        QCOMPARE(retry[0].canvasRect, expected[0].canvasRect);
        QCOMPARE(retry[0].image.pixel(16, 32), atlas.pixel(16, 32));
        QCOMPARE(retry[0].image.pixel(500, 500), QRgb(0));
    }

    void cancellationBetweenMeshesDoesNotReturnCompletedLayers_data()
    {
        QTest::addColumn<int>("completed");
        QTest::newRow("middle") << 3;
        QTest::newRow("after-last") << 6;
    }

    void cancellationBetweenMeshesDoesNotReturnCompletedLayers()
    {
        QFETCH(int, completed);
        MeshFixture mesh;
        QVERIFY(mesh.IsValid());
        QVERIFY(mesh.LoadAtlas(ColoredAtlas(8)));
        mesh.Geometry({0, 0, 1, 0, 1, 1, 0, 1}, {0, 1, 2, 0, 2, 3});
        mesh.MeshCount(6);
        const auto expected = mesh.Extract();
        QCOMPARE(expected.size(), 6);
        CExportControl control;
        bool canceledAtBoundary = false;
        QVERIFY(control.SetHandler([&](const CExportControl::Progress& progress, QString*) {
            if (progress.phase == CExportControl::Phase::Extracting &&
                progress.total == 6 && progress.current == completed) {
                canceledAtBoundary = true;
                control.Cancel();
            }
            return true;
        }));
        QVERIFY(mesh.Extract(&control).isEmpty());
        QVERIFY(canceledAtBoundary);
        QVERIFY(control.WasCanceled());
        const auto retry = mesh.Extract();
        QCOMPARE(retry.size(), expected.size());
        for (qsizetype index = 0; index < retry.size(); ++index) {
            QCOMPARE(retry[index].image, expected[index].image);
            QCOMPARE(retry[index].name, expected[index].name);
        }
    }

    void canceledOrFailedAtlasReloadPreservesPreviousAtlases()
    {
        MeshFixture mesh;
        QVERIFY(mesh.IsValid());
        const QImage original = ColoredAtlas();
        QVERIFY(mesh.LoadAtlas(original));
        mesh.Geometry({0, 0, 1, 0, 1, 1, 0, 1}, {0, 1, 2, 0, 2, 3});
        QImage replacement(4, 4, QImage::Format_ARGB32);
        replacement.fill(qRgba(200, 40, 20, 110));
        const QString first = mesh.WriteAtlas(replacement, "new-first.png");
        const QString second = mesh.WriteAtlas(replacement, "new-second.png");
        QVERIFY(!first.isEmpty() && !second.isEmpty());
        CExportControl canceled;
        bool firstLoaded = false;
        QVERIFY(canceled.SetHandler([&](const CExportControl::Progress& progress, QString*) {
            if (progress.phase == CExportControl::Phase::Preparing && progress.current == 1 && progress.total == 2) {
                firstLoaded = true;
                canceled.Cancel();
            }
            return true;
        }));
        QVERIFY(!mesh.Reload({first, second}, &canceled));
        QVERIFY(firstLoaded);
        QVERIFY(canceled.WasCanceled());
        auto layers = mesh.Extract();
        QCOMPARE(layers.size(), 1);
        QCOMPARE(layers[0].image, original);
        CExportControl failed;
        QVERIFY(!mesh.Reload({first, second + ".missing"}, &failed));
        QVERIFY(!failed.WasCanceled());
        QVERIFY(failed.Error().contains(second + ".missing"));
        layers = mesh.Extract();
        QCOMPARE(layers.size(), 1);
        QCOMPARE(layers[0].image, original);
        QVERIFY(!mesh.Reload({}));
        QCOMPARE(mesh.Extract()[0].image, original);
        QVERIFY(mesh.Reload({first, second}));
        QCOMPARE(mesh.Extract()[0].image, replacement);
    }

    void nonCancelingControlKeepsAllPixelsAndMetadata_data()
    {
        QTest::addColumn<QVector<quint16>>("indices");
        QTest::newRow("source") << QVector<quint16>{};
        QTest::newRow("override") << QVector<quint16>{0, 1, 2};
        QTest::newRow("invalid-override-fallback") << QVector<quint16>{0, 1, 9};
    }

    void nonCancelingControlKeepsAllPixelsAndMetadata()
    {
        QFETCH(QVector<quint16>, indices);
        MeshFixture mesh;
        QVERIFY(mesh.IsValid());
        const QImage atlas = ColoredAtlas(16);
        QVERIFY(mesh.LoadAtlas(atlas));
        mesh.Geometry({-.2f, .1f, .9f, .1f, .9f, 1.2f, -.2f, 1.2f}, {0, 1, 2, 0, 2, 3});
        mesh.Masks(2, true);
        mesh.MeshCount(3);
        const auto source = mesh.Extract();
        QCOMPARE(source.size(), 3);
        QHash<QString, QVector<quint16>> overrides;
        if (!indices.isEmpty()) for (const auto& layer : source) overrides.insert(layer.name, indices);
        const auto expected = mesh.Extract(nullptr, &overrides);
        CExportControl control;
        int checkpoints = 0;
        QVERIFY(control.SetHandler([&](const CExportControl::Progress&, QString*) { ++checkpoints; return true; }));
        const auto actual = mesh.Extract(&control, &overrides);
        QVERIFY(checkpoints > 3);
        QVERIFY(!control.WasCanceled());
        QVERIFY(control.Error().isEmpty());
        QCOMPARE(actual.size(), expected.size());
        for (qsizetype index = 0; index < actual.size(); ++index) {
            const auto& a = actual[index];
            const auto& b = expected[index];
            QCOMPARE(a.image, b.image);
            QCOMPARE(QByteArray(reinterpret_cast<const char*>(a.image.constBits()), a.image.sizeInBytes()),
                     QByteArray(reinterpret_cast<const char*>(b.image.constBits()), b.image.sizeInBytes()));
            QCOMPARE(a.canvasRect, b.canvasRect);
            QCOMPARE(a.name, b.name);
            QCOMPARE(a.meshIndex, b.meshIndex);
            QCOMPARE(a.textureNo, b.textureNo);
            QCOMPARE(a.drawableFlags, b.drawableFlags);
            QCOMPARE(a.opacity, b.opacity);
            QCOMPARE(a.isVisible, b.isVisible);
            QCOMPARE(a.isEnabled, b.isEnabled);
            QCOMPARE(a.parentPartIndex, b.parentPartIndex);
            QCOMPARE(a.maskIndices, b.maskIndices);
        }
    }

    void failedCheckpointDiscardsLayersWithoutMasqueradingAsCancellation()
    {
        MeshFixture mesh;
        QVERIFY(mesh.IsValid());
        QVERIFY(mesh.LoadAtlas(ColoredAtlas(16)));
        mesh.Geometry({0, 0, 1, 0, 0, 1});
        CExportControl control;
        QVERIFY(control.SetHandler([](const CExportControl::Progress& progress, QString* error) {
            if (progress.phase == CExportControl::Phase::Extracting && progress.total == 16 && progress.current == 8) {
                *error = QStringLiteral("controlled extraction failure");
                return false;
            }
            return true;
        }));
        QVERIFY(mesh.Extract(&control).isEmpty());
        QVERIFY(!control.WasCanceled());
        QCOMPARE(control.Error(), QStringLiteral("controlled extraction failure"));
        QCOMPARE(mesh.Extract().size(), 1);
    }

    void originalTexturePixelsDoNotBakeClipping_data()
    {
        QTest::addColumn<int>("count");
        QTest::addColumn<bool>("inverted");
        QTest::newRow("empty-inverted") << 0 << true;
        QTest::newRow("single-mask") << 1 << false;
        QTest::newRow("multiple-masks") << 2 << false;
        QTest::newRow("multiple-inverted") << 2 << true;
    }

    void originalTexturePixelsDoNotBakeClipping()
    {
        QFETCH(int, count);
        QFETCH(bool, inverted);
        MeshFixture mesh;
        QVERIFY(mesh.IsValid());
        const QImage atlas = ColoredAtlas();
        QVERIFY(mesh.LoadAtlas(atlas));
        mesh.Geometry({0, 0, 1, 0, 1, 1, 0, 1}, {0, 1, 2, 0, 2, 3});
        mesh.Masks(count, inverted);
        const auto layers = mesh.Extract();
        QCOMPARE(layers.size(), 1);
        QCOMPARE(layers[0].image, atlas);
        QCOMPARE(layers[0].canvasRect, QRect(0, 0, 4, 4));
        QVector<int> expected;
        for (int index = 0; index < count; ++index) expected.append(index + 1);
        QCOMPARE(layers[0].maskIndices, expected);
        QCOMPARE(layers[0].drawableFlags, inverted ? 0x08 : 0);
    }

    void inBoundsTextureKeepsEveryColorAndAlpha()
    {
        MeshFixture mesh;
        QVERIFY(mesh.IsValid());
        const QImage atlas = ColoredAtlas();
        QVERIFY(mesh.LoadAtlas(atlas));
        mesh.Geometry({0, 0, 1, 0, 1, 1, 0, 1}, {0, 1, 2, 0, 2, 3});
        const auto layers = mesh.Extract();
        QCOMPARE(layers.size(), 1);
        QCOMPARE(layers[0].image, atlas);
        QCOMPARE(layers[0].canvasRect, QRect(0, 0, 4, 4));
    }

    void crossingRightEdgeRepeatsWithoutMovingVertices()
    {
        MeshFixture mesh;
        QVERIFY(mesh.IsValid());
        const QImage atlas = ColoredAtlas();
        QVERIFY(mesh.LoadAtlas(atlas));
        mesh.Geometry({0, 0, 2, 0, 0, 1});
        const auto layers = mesh.Extract();
        QCOMPARE(layers.size(), 1);
        QCOMPARE(layers[0].image.size(), QSize(9, 4));
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 9; ++x) {
                const bool covered = y + 0.5 <= 4.0 - (x + 0.5) / 2.0;
                QCOMPARE(layers[0].image.pixel(x, y), covered ? atlas.pixel(x % 4, y) : QRgb(0));
            }
        QCOMPARE(layers[0].image.pixel(3, 1), atlas.pixel(3, 1));
    }

    void negativeUvPreservesVisibleTriangleAndOriginalSampling()
    {
        MeshFixture mesh;
        QVERIFY(mesh.IsValid());
        const QImage atlas = ColoredAtlas();
        QVERIFY(mesh.LoadAtlas(atlas));
        mesh.Geometry({-1, 0, 1, 0, 1, 1});
        const auto layers = mesh.Extract();
        QCOMPARE(layers.size(), 1);
        QCOMPARE(layers[0].image.size(), QSize(9, 4)); QCOMPARE(layers[0].canvasRect, QRect(-5, 0, 9, 4));
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 9; ++x) {
                const int sourceX = x - 5;
                const bool covered = sourceX >= -4 && y + 0.5 <= (sourceX + 0.5) / 2.0 + 2.0;
                QCOMPARE(layers[0].image.pixel(x, y), covered ? atlas.pixel((sourceX % 4 + 4) % 4, y) : QRgb(0));
            }
        QCOMPARE(layers[0].image.pixel(5, 1), atlas.pixel(0, 1));
    }

    void canonicalWideSourceAndOverridesKeepCanvasAndUvPixels()
    {
        WideMeshData::Fixture wide; QString error; QVERIFY2(WideMeshData::make(65536, WideMeshData::Layout::Sparse, wide, &error), qPrintable(error));
        auto reader = CReader::FromBytes(wide.bytes); CSectionTable table(reader); QVERIFY(table.IsValid());
        CArt_meshes* target = nullptr; int atlasCount = 0;
        for (auto* mesh : table.art_meshes) { mesh->is_visible = mesh->ids == wide.ids.first(); if (mesh->is_visible) target = mesh; atlasCount = std::max(atlasCount, mesh->texture_nos + 1); }
        QVERIFY(target); QCOMPARE(target->vertex_counts, 65536); const int first = target->position_index_sources_begin_indices;
        for (int corner = 0; corner < 3; ++corner) QCOMPARE(table.position_indices->indices[first + corner], wide.indices[corner]);
        auto positions = wide.positions;
        for (int corner = 0; corner < 3; ++corner) for (int axis = 0; axis < 2; ++axis) {
            table.uvs->uvs[target->uv_sources_begin_indices + corner * 2 + axis] = table.uvs->uvs[target->uv_sources_begin_indices + wide.indices[corner] * 2 + axis];
            positions[corner * 2 + axis] = positions[wide.indices[corner] * 2 + axis];
        }
        table.canvas_info->pixels_per_unit = 10; table.canvas_info->origin_x = table.canvas_info->origin_y = 0;
        const auto atlas = ColoredAtlas(8); CTextureExtractor extractor; QVERIFY(extractor.LoadAtlasBytes(QVector<QByteArray>(atlasCount, Png(atlas))));
        const QHash<QString, QVector<quint16>> low{{target->ids,{0,1,2}}}, reverse{{target->ids,{65535,65534,32768}}};
        const QHash<QString,QVector<float>> frozen{{target->ids,positions}};
        for (bool canvas : {false,true}) {
            const auto* pose = canvas ? &frozen : nullptr;
            CExportControl originalControl, lowControl, reversedControl;
            const auto original = extractor.ExtractAll(&table,false,nullptr,&originalControl,pose);
            const auto control = extractor.ExtractAll(&table,false,&low,&lowControl,pose);
            const auto reversed = extractor.ExtractAll(&table,false,&reverse,&reversedControl,pose);
            QVERIFY2(originalControl.Checkpoint(),qPrintable(originalControl.Error())); QVERIFY(lowControl.Checkpoint()); QVERIFY(reversedControl.Checkpoint());
            QCOMPARE(original.size(),1); QCOMPARE(control.size(),1); QCOMPARE(reversed.size(),1); QVERIFY(HasVisiblePixel(original[0].image));
            QCOMPARE(original[0].image,control[0].image); QCOMPARE(original[0].image,reversed[0].image); QCOMPARE(original[0].canvasRect,control[0].canvasRect); QCOMPARE(original[0].opacity,control[0].opacity);
        }
    }
    void unsigned16IndicesReachTheLastVertex()
    {
        MeshFixture mesh; QVERIFY(mesh.IsValid()); const auto atlas = ColoredAtlas(); QVERIFY(mesh.LoadAtlas(atlas));
        std::vector<float> uvs(65536 * 2, 0.0f);
        uvs[65534 * 2] = 1; uvs[65535 * 2 + 1] = 1;
        mesh.Geometry(uvs, {32768, 65534, 65535});
        const auto layers = mesh.Extract(); QCOMPARE(layers.size(), 1);
        QCOMPARE(layers[0].image.pixel(0, 0), atlas.pixel(0, 0)); QCOMPARE(layers[0].image.pixel(3, 3), QRgb(0));
        const QHash<QString, QVector<quint16>> overrides{{mesh.Id(), {65535, 65534, 32768}}};
        const auto reversed = mesh.Extract(nullptr, &overrides); QCOMPARE(reversed.size(), 1); QCOMPARE(reversed[0].image, layers[0].image);
    }
    void croppedTextureSamplesWithItsAtlasOrigin()
    {
        MeshFixture mesh;
        QVERIFY(mesh.IsValid());
        const QImage atlas = ColoredAtlas(8);
        QVERIFY(mesh.LoadAtlas(atlas));
        mesh.Geometry({0.25f, 0.25f, 0.75f, 0.25f, 0.75f, 0.75f, 0.25f, 0.75f}, {0, 1, 2, 0, 2, 3});
        const auto layers = mesh.Extract();
        QCOMPARE(layers.size(), 1);
        QCOMPARE(layers[0].canvasRect, QRect(1, 1, 6, 6));
        QCOMPARE(layers[0].image.size(), QSize(6, 6));
        for (int y = 0; y < 6; ++y)
            for (int x = 0; x < 6; ++x) {
                const bool covered = x >= 1 && x <= 4 && y >= 1 && y <= 4;
                QCOMPARE(layers[0].image.pixel(x, y), covered ? atlas.pixel(x + 1, y + 1) : QRgb(0));
            }
    }

    void outOfRangeUvRepeatsAndNonfiniteFails()
    {
        MeshFixture mesh;
        QVERIFY(mesh.IsValid());
        QVERIFY(mesh.LoadAtlas(ColoredAtlas()));
        const float infinity = std::numeric_limits<float>::infinity();
        const float nan = std::numeric_limits<float>::quiet_NaN();
        const float maximum = std::numeric_limits<float>::max();
        const std::vector<std::vector<float>> cases{
            {2, 0, 3, 0, 2, 1}, {-2, 0, -1, 0, -1, 1},
            {0, 2, 1, 2, 0, 3}, {0, -2, 1, -1, 0, -1},
            {maximum, 0, maximum / 2, 0, maximum, 1},
            {0, 0, 1, 0, nan, 1}, {0, 0, 1, infinity, 0, 1}
        };
        for (qsizetype index = 0; index < cases.size(); ++index) {
            mesh.Geometry(cases[index]); CExportControl control; const auto layers = mesh.Extract(&control);
            if (index < 4) { QCOMPARE(layers.size(), 1); QVERIFY(HasVisiblePixel(layers[0].image)); QVERIFY(control.Error().isEmpty()); }
            else { QVERIFY(layers.isEmpty()); QVERIFY(!control.Error().isEmpty()); }
        }
    }

    void degenerateAndExtremeTrianglesStayWithinPixelBounds()
    {
        MeshFixture mesh;
        QVERIFY(mesh.IsValid());
        const QImage atlas = ColoredAtlas();
        QVERIFY(mesh.LoadAtlas(atlas));
        mesh.Geometry({0, 0, 0.5f, 0.5f, 1, 1});
        auto layers = mesh.Extract();
        QCOMPARE(layers.size(), 1);
        QVERIFY(!HasVisiblePixel(layers[0].image));
        const float tiny = std::numeric_limits<float>::denorm_min();
        mesh.Geometry({0, 0, tiny, 0, 0, tiny});
        layers = mesh.Extract();
        QCOMPARE(layers.size(), 1);
        QVERIFY(!HasVisiblePixel(layers[0].image));
        const float maximum = std::numeric_limits<float>::max();
        mesh.Geometry({-maximum, -maximum, maximum, -maximum, 0, maximum});
        CExportControl control; layers = mesh.Extract(&control);
        QVERIFY(layers.isEmpty()); QVERIFY(!control.Error().isEmpty());
    }

    void unboundedRepeatExtentFailsAtomically()
    {
        MeshFixture mesh;
        QVERIFY(mesh.IsValid());
        const QImage atlas = ColoredAtlas();
        QVERIFY(mesh.LoadAtlas(atlas));
        const float maximum = std::numeric_limits<float>::max();
        for (bool reverse : {false, true}) {
            mesh.Geometry({-maximum, -maximum, maximum, maximum, 0, 1},
                          reverse ? std::vector<quint16>{2, 1, 0} : std::vector<quint16>{0, 1, 2});
            CExportControl control; const auto layers = mesh.Extract(&control);
            QVERIFY(layers.isEmpty()); QVERIFY(control.Error().contains(QStringLiteral("限制")));
        }
    }

    void oversizedLaterLayerCannotReturnPartialExtraction()
    {
        MeshFixture mesh; QVERIFY(mesh.IsValid()); QVERIFY(mesh.LoadAtlas(ColoredAtlas()));
        mesh.Geometry({0,0,1,0,0,1}); mesh.AdditionalGeometry({0,0,100000,0,0,100000});
        CExportControl control; QVERIFY(mesh.Extract(&control).isEmpty()); QVERIFY(control.Error().contains(QStringLiteral("限制")));
        QVERIFY(mesh.Extract().isEmpty());
    }
    void canvasBoundsUseDoubleAndRejectUnrepresentableRectangles()
    {
        MeshFixture mesh;
        QVERIFY(mesh.IsValid());
        QVERIFY(mesh.LoadAtlas(ColoredAtlas()));
        mesh.Geometry({0, 0, 1, 0, 1, 1, 0, 1}, {0, 1, 2, 0, 2, 3});
        mesh.CanvasPositions({-1, -2, 1, -2, 1, 2, -1, 2}, 10, 3, 4);
        auto layers = mesh.Extract();
        QCOMPARE(layers.size(), 1);
        QCOMPARE(layers[0].canvasRect, QRect(-7, -16, 20, 40));
        mesh.CanvasPositions({2147483392.0f, 0, 2147483520.0f, 0, 2147483520.0f, 1, 2147483392.0f, 1}, 1);
        layers = mesh.Extract();
        QCOMPARE(layers.size(), 1);
        QCOMPARE(layers[0].canvasRect, QRect(2147483392, -1, 128, 1));
        const float maximum = std::numeric_limits<float>::max();
        const float infinity = std::numeric_limits<float>::infinity();
        struct Invalid { std::vector<float> positions; float ppu; float x = 0; float y = 0; };
        const std::vector<Invalid> invalid{
            {{-maximum, -1, maximum, -1, maximum, 1, -maximum, 1}, maximum},
            {{-2000000000.0f, -1, 2000000000.0f, -1, 2000000000.0f, 1, -2000000000.0f, 1}, 1},
            {{-2147483648.0f, 0, -2147483648.0f, 0, -2147483648.0f, 1, -2147483648.0f, 1}, 1},
            {{0, 0, 1, 0, 1, infinity, 0, 1}, 1},
            {{0, 0, 1, 0, 1, 1, 0, 1}, 0},
            {{0, 0, 1, 0, 1, 1, 0, 1}, 1, infinity},
            {{0, 0, 1, 0, 1, 1, 0, 1}, maximum, maximum}
        };
        for (const auto& input : invalid) {
            mesh.CanvasPositions(input.positions, input.ppu, input.x, input.y);
            layers = mesh.Extract();
            QCOMPARE(layers.size(), 1);
            QCOMPARE(layers[0].canvasRect, QRect(0, 0, 4, 4));
            QCOMPARE(layers[0].image, ColoredAtlas());
        }
    }

    void riceBoundaryMeshSurvivesSmallAndOriginalAtlases()
    {
        const QString directory = QString(TEST_RESOURCES) + "/Rice/";
        CReader reader(directory + "Rice.moc3");
        CSectionTable table(reader);
        QVERIFY(table.IsValid());
        const auto target = std::find_if(table.art_meshes.begin(), table.art_meshes.end(),
            [](const CArt_meshes* mesh) { return mesh->ids == QLatin1String("ArtMesh183"); });
        QVERIFY(target != table.art_meshes.end());
        auto* mesh = *target;
        QCOMPARE(mesh->vertex_counts, 25);
        QCOMPARE(table.uvs->uvs[mesh->uv_sources_begin_indices + 8], 1.0021253824234009f);
        std::iter_swap(table.art_meshes.begin(), target);
        table.count_info->art_meshes = 1;
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QImage small(16, 16, QImage::Format_ARGB32);
        const QRgb color = qRgba(120, 80, 40, 180);
        small.fill(color);
        const QString smallPath = temp.filePath("small.png");
        QVERIFY(small.save(smallPath, "PNG"));
        CTextureExtractor extractor;
        QVERIFY(extractor.LoadAtlases({smallPath, smallPath}));
        auto layers = extractor.ExtractAll(&table, true);
        QCOMPARE(layers.size(), 1);
        QCOMPARE(layers[0].name, QString("ArtMesh183"));
        QVERIFY(HasVisiblePixel(layers[0].image));
        for (int y = 0; y < layers[0].image.height(); ++y)
            for (int x = 0; x < layers[0].image.width(); ++x)
                if (qAlpha(layers[0].image.pixel(x, y))) QCOMPARE(layers[0].image.pixel(x, y), color);
        QFile model(directory + "Rice.model3.json");
        QVERIFY(model.open(QIODevice::ReadOnly));
        const auto textures = QJsonDocument::fromJson(model.readAll()).object()
            .value("FileReferences").toObject().value("Textures").toArray();
        QVector<QString> paths;
        for (const auto& texture : textures) paths.append(directory + texture.toString());
        QCOMPARE(paths.size(), 2);
        QVERIFY(extractor.LoadAtlases(paths));
        layers = extractor.ExtractAll(&table, true);
        QCOMPARE(layers.size(), 1);
        QCOMPARE(layers[0].name, QString("ArtMesh183"));
        QVERIFY(HasVisiblePixel(layers[0].image));
        QVERIFY(layers[0].image.width() <= 2048 && layers[0].image.height() <= 2048);
    }
};

QTEST_GUILESS_MAIN(TextureExtractionTests)
#include "texture_extraction_tests.moc"
