#include "opengl/ctextureextractor.h"
#include "live2dstruct/csectiontable.h"
#include <QtTest>
#include <QTemporaryDir>
#include <algorithm>
#include <cmath>
#include <limits>

using namespace live2dstruct;
namespace {
QImage atlasImage(int width, int height, int seed = 0)
{
    QImage image(width, height, QImage::Format_ARGB32);
    for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x)
        image.setPixel(x, y, qRgba((x * 29 + seed + 47) % 256, (y * 43 + seed + 83) % 256,
                                 (x * 13 + y * 17 + seed + 121) % 256, (x + y) % 3 ? 1 + (x * 19 + y * 23) % 255 : 0));
    return image;
}
class Fixture {
public:
    Fixture() : reader(QString(TEST_RESOURCES) + "/Mark/Mark.moc3"), table(reader)
    {
        if (!table.IsValid()) return;
        for (auto* mesh : table.art_meshes) { mesh->vertex_counts = 0; mesh->position_index_sources_counts = 0; }
        table.uvs->uvs.clear(); table.position_indices->indices.clear();
    }
    bool valid() const { return temporary.isValid() && table.IsValid() && table.art_meshes.size() >= 3; }
    bool atlas(const QImage& image)
    {
        const QString path = temporary.filePath(QString::number(paths.size()) + ".png");
        if (!image.save(path, "PNG")) return false;
        paths.append(path); return extractor.LoadAtlases(paths);
    }
    CArt_meshes* mesh(int index, const std::vector<float>& uv, const std::vector<short>& indices = {0,1,2}, int texture = 0)
    {
        auto* object = table.art_meshes[index]; object->vertex_counts = int(uv.size() / 2);
        object->uv_sources_begin_indices = table.uvs->uvs.size(); object->texture_nos = texture;
        object->position_index_sources_begin_indices = table.position_indices->indices.size(); object->position_index_sources_counts = indices.size();
        table.uvs->uvs.insert(table.uvs->uvs.end(), uv.begin(), uv.end());
        table.position_indices->indices.insert(table.position_indices->indices.end(), indices.begin(), indices.end());
        return object;
    }
    CReader reader;
    CSectionTable table;
    CTextureExtractor extractor;
    QTemporaryDir temporary;
    QVector<QString> paths;
};
QVector<ExtractedMaterial> sentinel()
{
    return {{QStringLiteral("keep"), 12, 34, atlasImage(2,3), QRect(-2,4,2,3), QSize(7,8), false}};
}
void checkSentinel(const QVector<ExtractedMaterial>& result)
{
    QCOMPARE(result.size(), 1); QCOMPARE(result[0].name, QStringLiteral("keep")); QCOMPARE(result[0].meshIndex, 12);
    QCOMPARE(result[0].textureNo, 34); QCOMPARE(result[0].atlasRect, QRect(-2,4,2,3)); QCOMPARE(result[0].atlasSize, QSize(7,8));
    QCOMPARE(result[0].image, atlasImage(2,3)); QVERIFY(!result[0].visible);
}
int repeat(int value, int size) { const int remainder = value % size; return remainder < 0 ? remainder + size : remainder; }
QVector<double> bilinear(const QImage& image, double u, double v, bool repeating = false)
{
    const double px = u * image.width() - .5, py = v * image.height() - .5;
    const int x = int(std::floor(px)), y = int(std::floor(py)); const double dx = px - x, dy = py - y;
    QVector<double> result(4, 0);
    for (int iy = 0; iy < 2; ++iy) for (int ix = 0; ix < 2; ++ix) {
        const QRgb pixel = image.pixel(repeating ? repeat(x + ix, image.width()) : std::clamp(x + ix, 0, image.width() - 1),
                                       repeating ? repeat(y + iy, image.height()) : std::clamp(y + iy, 0, image.height() - 1));
        const double weight = (ix ? dx : 1 - dx) * (iy ? dy : 1 - dy);
        const int channels[]{qRed(pixel), qGreen(pixel), qBlue(pixel), qAlpha(pixel)};
        for (int channel = 0; channel < 4; ++channel) result[channel] += channels[channel] * weight;
    }
    return result;
}
}

class TextureMaterialTests : public QObject {
    Q_OBJECT
private slots:
    void allVerticesAndUncoveredPixelsRemainExact()
    {
        Fixture fixture; QVERIFY(fixture.valid()); const auto atlas = atlasImage(8,4); QVERIFY(fixture.atlas(atlas));
        auto* mesh = fixture.mesh(0, {.25f,.25f,.75f,.25f,.25f,.75f,.9375f,.875f}); mesh->is_visible = false;
        QVector<ExtractedMaterial> result; QString error;
        QVERIFY2(fixture.extractor.ExtractMaterials(&fixture.table, result, &error), qPrintable(error)); QCOMPARE(result.size(), 1);
        const auto& material = result.first(); QCOMPARE(material.atlasRect, QRect(1,0,8,5)); QCOMPARE(material.atlasSize, QSize(8,4));
        QCOMPARE(material.image.size(), material.atlasRect.size()); QCOMPARE(material.name, mesh->ids); QCOMPARE(material.meshIndex, 0); QVERIFY(!material.visible);
        QCOMPARE(material.image.format(), QImage::Format_ARGB32);
        for (int y = 0; y < material.image.height(); ++y) for (int x = 0; x < material.image.width(); ++x)
            QCOMPARE(material.image.pixel(x,y), atlas.pixel(repeat(x + 1, 8), repeat(y, 4)));
        fixture.table.position_indices->indices = {2,1,0}; QVector<ExtractedMaterial> reversed;
        QVERIFY(fixture.extractor.ExtractMaterials(&fixture.table, reversed, &error)); QCOMPARE(reversed.first().image, material.image);
        QCOMPARE(reversed.first().atlasRect, material.atlasRect);
    }
    void edgePaddingPreservesBilinearSamples_data()
    {
        QTest::addColumn<bool>("outside"); QTest::newRow("inside") << false; QTest::newRow("outside") << true;
    }
    void edgePaddingPreservesBilinearSamples()
    {
        QFETCH(bool, outside); Fixture fixture; QVERIFY(fixture.valid()); const auto atlas = atlasImage(4,3); QVERIFY(fixture.atlas(atlas));
        const float low = outside ? -.5f : .25f, high = outside ? 1.5f : .75f;
        fixture.mesh(0, {low,low,high,low,low,high,high,high}); QVector<ExtractedMaterial> result; QString error;
        QVERIFY2(fixture.extractor.ExtractMaterials(&fixture.table, result, &error), qPrintable(error)); const auto& material = result.first();
        for (int y = 0; y <= 16; ++y) for (int x = 0; x <= 16; ++x) {
            const double u = low + double(high - low) * x / 16, v = low + double(high - low) * y / 16;
            const auto expected = bilinear(atlas, u, v, true);
            const auto actual = bilinear(material.image, (u * atlas.width() - material.atlasRect.x()) / material.image.width(),
                                         (v * atlas.height() - material.atlasRect.y()) / material.image.height());
            for (int channel = 0; channel < 4; ++channel) QVERIFY(std::abs(actual[channel] - expected[channel]) < 1e-8);
        }
    }
    void entirelyOutsideCoordinatesRepeatAtlas_data()
    {
        QTest::addColumn<float>("uv"); QTest::newRow("negative") << -1000.25f; QTest::newRow("positive") << 1000.25f;
    }
    void entirelyOutsideCoordinatesRepeatAtlas()
    {
        QFETCH(float, uv); Fixture fixture; QVERIFY(fixture.valid()); const auto atlas = atlasImage(4,3); QVERIFY(fixture.atlas(atlas));
        fixture.mesh(0, {uv,uv}, {}); QVector<ExtractedMaterial> result; QString error;
        QVERIFY2(fixture.extractor.ExtractMaterials(&fixture.table, result, &error), qPrintable(error)); QCOMPARE(result.size(), 1);
        for (int y = 0; y < result[0].image.height(); ++y) for (int x = 0; x < result[0].image.width(); ++x)
            QCOMPARE(result[0].image.pixel(x,y), atlas.pixel(repeat(result[0].atlasRect.x() + x, atlas.width()), repeat(result[0].atlasRect.y() + y, atlas.height())));
    }
    void distinctAtlasesAndDuplicateRectsKeepIdentity()
    {
        Fixture fixture; QVERIFY(fixture.valid()); const auto first = atlasImage(4,3), second = atlasImage(4,3,93);
        QVERIFY(fixture.atlas(first)); QVERIFY(fixture.atlas(second));
        const std::vector<float> uv{.25f,.25f,.5f,.25f,.25f,.5f}; fixture.mesh(0, uv); fixture.mesh(1, uv); fixture.mesh(2, uv, {0,1,2}, 1);
        QVector<ExtractedMaterial> result; QString error; QVERIFY2(fixture.extractor.ExtractMaterials(&fixture.table, result, &error), qPrintable(error)); QCOMPARE(result.size(), 3);
        QCOMPARE(result[0].image.constBits(), result[1].image.constBits()); QVERIFY(result[0].image != result[2].image);
        QCOMPARE(result[0].textureNo, 0); QCOMPARE(result[2].textureNo, 1); QCOMPARE(result[0].meshIndex, 0); QCOMPARE(result[2].meshIndex, 2);
        result[0].image.setPixel(0,0, qRgba(1,2,3,4)); QVERIFY(result[0].image != result[1].image);
    }
    void emptyGeometryIsExplicitAndZeroIndicesKeepMaterial()
    {
        Fixture fixture; QVERIFY(fixture.valid()); auto result = sentinel(); QString error;
        QVERIFY(fixture.extractor.ExtractMaterials(&fixture.table, result, &error)); QVERIFY(result.isEmpty());
        QVERIFY(fixture.atlas(atlasImage(2,2))); fixture.mesh(1, {.5f,.5f}, {});
        QVERIFY(fixture.extractor.ExtractMaterials(&fixture.table, result, &error)); QCOMPARE(result.size(), 1); QCOMPARE(result[0].meshIndex, 1);
        QCOMPARE(result[0].atlasRect, QRect(0,0,2,2));
    }
    void invalidInputKeepsPreviousResult_data()
    {
        QTest::addColumn<int>("mutation"); for (int i = 0; i < 18; ++i) QTest::newRow(qPrintable(QString::number(i))) << i;
    }
    void invalidInputKeepsPreviousResult()
    {
        QFETCH(int, mutation); Fixture fixture; QVERIFY(fixture.valid()); QVERIFY(fixture.atlas(atlasImage(2,2)));
        auto* mesh = fixture.mesh(0, {0,0,1,0,0,1});
        switch (mutation) {
        case 0: fixture.table.count_info->art_meshes--; break;
        case 1: mesh->vertex_counts = -1; break;
        case 2: mesh->vertex_counts = 1000001; break;
        case 3: mesh->position_index_sources_counts = -1; break;
        case 4: mesh->position_index_sources_counts = 2; break;
        case 5: mesh->vertex_counts = 0; break;
        case 6: mesh->texture_nos = -1; break;
        case 7: mesh->texture_nos = 1; break;
        case 8: mesh->uv_sources_begin_indices = -1; break;
        case 9: mesh->uv_sources_begin_indices = 1; break;
        case 10: fixture.table.uvs->uvs[0] = std::numeric_limits<float>::quiet_NaN(); break;
        case 11: fixture.table.uvs->uvs[0] = std::numeric_limits<float>::infinity(); break;
        case 12: fixture.table.position_indices->indices[0] = -1; break;
        case 13: fixture.table.position_indices->indices[0] = 3; break;
        case 14: mesh->position_index_sources_begin_indices = -1; break;
        case 15: mesh->position_index_sources_begin_indices = 1; break;
        case 16: fixture.table.uvs->uvs[0] = std::numeric_limits<float>::max(); break;
        case 17: fixture.table.uvs->uvs[0] = -std::numeric_limits<float>::max(); break;
        }
        auto result = sentinel(); QString error; QVERIFY(!fixture.extractor.ExtractMaterials(&fixture.table, result, &error)); QVERIFY(!error.isEmpty()); checkSentinel(result);
        QVERIFY(!fixture.extractor.ExtractMaterials(nullptr, result, &error)); checkSentinel(result);
    }
    void unsigned16IndicesAndSharedOddUvHaveExactMaterials()
    {
        Fixture fixture; QVERIFY(fixture.valid()); const auto atlas = atlasImage(4, 3); QVERIFY(fixture.atlas(atlas));
        std::vector<float> uvs(65536 * 2, .25f); uvs[65534 * 2] = .75f; uvs[65535 * 2 + 1] = .75f;
        fixture.mesh(0, uvs, {short(quint16(32768)), short(quint16(65534)), short(quint16(65535))});
        auto* second = fixture.mesh(1, {.1f,.2f,.3f,.4f,.5f,.6f}); second->uv_sources_begin_indices = 1;
        QVector<ExtractedMaterial> result; QString error;
        QVERIFY2(fixture.extractor.ExtractMaterials(&fixture.table, result, &error), qPrintable(error)); QCOMPARE(result.size(), 2);
        QCOMPARE(result[0].authorUvs[1], .25f); QCOMPARE(result[0].authorUvs[2], .75f);
        QCOMPARE(result[1].authorUvs[0], .75f); QCOMPARE(result[1].authorUvs[1], .25f);
        for (const auto& material : result) for (int y = 0; y < material.image.height(); ++y) for (int x = 0; x < material.image.width(); ++x)
            QCOMPARE(material.image.pixel(x, y), atlas.pixel(repeat(material.atlasRect.x() + x, atlas.width()), repeat(material.atlasRect.y() + y, atlas.height())));
    }
    void pixelLimitsAreCheckedBeforeImagesAreAllocated()
    {
        Fixture fixture; QVERIFY(fixture.valid()); QVERIFY(fixture.atlas(atlasImage(2,2)));
        fixture.mesh(0, {0,0,8191,0,0,4095.5f}); auto result = sentinel(); QString error;
        QVERIFY(!fixture.extractor.ExtractMaterials(&fixture.table, result, &error)); QVERIFY(error.contains(QStringLiteral("单张"))); checkSentinel(result);
        fixture.table.uvs->uvs.clear(); fixture.table.position_indices->indices.clear();
        for (int index = 0; index < 3; ++index) { const float begin = float(index); fixture.mesh(index, {begin,0,begin+4999,0,begin,4999}); }
        QVERIFY(!fixture.extractor.ExtractMaterials(&fixture.table, result, &error)); QVERIFY(error.contains(QStringLiteral("总像素"))); checkSentinel(result);
    }
    void sharedSlicesAreCachedAndDistinctTraversalIsBounded()
    {
        Fixture fixture; QVERIFY(fixture.valid()); QVERIFY(fixture.atlas(atlasImage(2,2)));
        auto* first = fixture.mesh(0, std::vector<float>(20000,.5f));
        while (fixture.table.art_meshes.size() < 1800) fixture.table.art_meshes.append(new CArt_meshes(*first));
        for (int index = 0; index < fixture.table.art_meshes.size(); ++index) { *fixture.table.art_meshes[index] = *first; fixture.table.art_meshes[index]->ids = QString::number(index); }
        fixture.table.count_info->art_meshes = fixture.table.art_meshes.size();
        QVector<ExtractedMaterial> result; QString error;
        QVERIFY2(fixture.extractor.ExtractMaterials(&fixture.table, result, &error), qPrintable(error)); QCOMPARE(result.size(), 1800);
        QCOMPARE(result.first().image.constBits(), result.last().image.constBits());
        fixture.table.uvs->uvs.resize(23600,.5f);
        for (int i = 0; i < fixture.table.art_meshes.size(); ++i) fixture.table.art_meshes[i]->uv_sources_begin_indices = i * 2;
        result = sentinel(); QVERIFY(!fixture.extractor.ExtractMaterials(&fixture.table, result, &error)); QVERIFY(error.contains(QStringLiteral("检查总量"))); checkSentinel(result);
    }
};
QTEST_GUILESS_MAIN(TextureMaterialTests)
#include "texture_material_tests.moc"
