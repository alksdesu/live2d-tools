#include "live2dstruct/csectiontable.h"
#include "opengl/ctextureextractor.h"

#include <QtTest>
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
    void Geometry(const std::vector<float>& uvs, const std::vector<short>& indices = {0, 1, 2})
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
    QVector<ExtractedLayer> Extract() { return extractor.ExtractAll(&table, true); }

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

    void crossingRightEdgeClipsScanAreaWithoutMovingVertices()
    {
        MeshFixture mesh;
        QVERIFY(mesh.IsValid());
        const QImage atlas = ColoredAtlas();
        QVERIFY(mesh.LoadAtlas(atlas));
        mesh.Geometry({0, 0, 2, 0, 0, 1});
        const auto layers = mesh.Extract();
        QCOMPARE(layers.size(), 1);
        QCOMPARE(layers[0].image.size(), QSize(4, 4));
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 4; ++x) {
                const bool covered = y + 0.5 <= 4.0 - (x + 0.5) / 2.0;
                QCOMPARE(layers[0].image.pixel(x, y), covered ? atlas.pixel(x, y) : QRgb(0));
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
        QCOMPARE(layers[0].image.size(), QSize(4, 4));
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 4; ++x) {
                const bool covered = y + 0.5 <= (x + 0.5) / 2.0 + 2.0;
                QCOMPARE(layers[0].image.pixel(x, y), covered ? atlas.pixel(x, y) : QRgb(0));
            }
        QCOMPARE(layers[0].image.pixel(0, 1), atlas.pixel(0, 1));
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

    void outsideAndNonfiniteUvDoNotProduceLayers()
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
        for (const auto& uv : cases) {
            mesh.Geometry(uv);
            QVERIFY(mesh.Extract().isEmpty());
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
        layers = mesh.Extract();
        QCOMPARE(layers.size(), 1);
        QCOMPARE(layers[0].image, atlas);
    }

    void hugeNarrowTriangleRetainsVisiblePixels()
    {
        MeshFixture mesh;
        QVERIFY(mesh.IsValid());
        const QImage atlas = ColoredAtlas();
        QVERIFY(mesh.LoadAtlas(atlas));
        const float maximum = std::numeric_limits<float>::max();
        for (bool reverse : {false, true}) {
            mesh.Geometry({-maximum, -maximum, maximum, maximum, 0, 1},
                          reverse ? std::vector<short>{2, 1, 0} : std::vector<short>{0, 1, 2});
            const auto layers = mesh.Extract();
            QCOMPARE(layers.size(), 1);
            QCOMPARE(layers[0].image.size(), atlas.size());
            for (int y = 0; y < 4; ++y)
                for (int x = 0; x < 4; ++x)
                    QCOMPARE(layers[0].image.pixel(x, y), y >= x ? atlas.pixel(x, y) : QRgb(0));
            QCOMPARE(layers[0].image.pixel(0, 1), atlas.pixel(0, 1));
        }
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
