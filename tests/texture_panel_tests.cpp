#include "ui/ctexturepanel.h"
#include "live2dstruct/csectiontable.h"

#include <QtTest>
#include <QDir>
#include <QFile>
#include <QGraphicsPixmapItem>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QTemporaryDir>
#include <QtEndian>
#include <cmath>
#include <cstring>
#include <limits>

using namespace live2dstruct;

namespace {
QMap<int, QString> texturesFor(const QString& model)
{
    const QDir directory(QString(TEST_RESOURCES) + '/' + model);
    QFile file(directory.filePath(model + ".model3.json"));
    if (!file.open(QIODevice::ReadOnly)) return {};
    const auto textures = QJsonDocument::fromJson(file.readAll()).object()
        .value("FileReferences").toObject().value("Textures").toArray();
    QMap<int, QString> paths;
    for (int index = 0; index < textures.size(); ++index)
        paths.insert(index, directory.filePath(textures[index].toString()));
    return paths;
}

QToolButton* exportButton(CTexturePanel& panel)
{
    for (auto* button : panel.findChildren<QToolButton*>())
        if (button->text() == QStringLiteral("导出 PNG")) return button;
    return nullptr;
}

QImage renderScene(QGraphicsScene* scene, int margin = 8, QSize target = {})
{
    const QRectF source = scene->sceneRect().adjusted(-margin, -margin, margin, margin);
    if (target.isEmpty()) target = source.size().toSize();
    QImage image(target, QImage::Format_ARGB32);
    image.fill(qRgb(173, 11, 119));
    QPainter painter(&image);
    scene->render(&painter, QRectF(image.rect()), source, Qt::IgnoreAspectRatio);
    return image;
}

void writeInt(QByteArray& bytes, int offset, qint32 value)
{
    Q_ASSERT(offset >= 0 && offset <= bytes.size() - 4);
    qToLittleEndian<qint32>(value, reinterpret_cast<uchar*>(bytes.data() + offset));
}

void writeFloat(QByteArray& bytes, int offset, float value)
{
    quint32 bits;
    std::memcpy(&bits, &value, sizeof(bits));
    qToLittleEndian<quint32>(bits, reinterpret_cast<uchar*>(bytes.data() + offset));
}

bool makeSingleTriangle(const QString& path, const std::vector<float>& uvs, QString& name)
{
    CReader reader(QString(TEST_RESOURCES) + QStringLiteral("/Mark/Mark.moc3"));
    CSectionTable table(reader);
    if (!table.IsValid() || table.art_meshes.isEmpty() || uvs.size() != 6) return false;
    const auto* mesh = table.art_meshes[0];
    const auto* offsets = table.sectionOffsetTable->art_meshes;
    name = mesh->ids;
    QByteArray bytes = reader.m_byte;
    writeInt(bytes, table.sectionOffsetTable->count_info->count_info_offset + 4 * 4, 1);
    writeInt(bytes, offsets->vertex_counts, 3);
    writeInt(bytes, offsets->texture_nos, 0);
    writeInt(bytes, offsets->position_index_sources_counts, 3);
    for (int index = 0; index < 6; ++index)
        writeFloat(bytes, table.sectionOffsetTable->uvs->uvs + (mesh->uv_sources_begin_indices + index) * 4, uvs[index]);
    for (int index = 0; index < 3; ++index)
        qToLittleEndian<quint16>(index, reinterpret_cast<uchar*>(bytes.data() +
            table.sectionOffsetTable->position_indices->indices + (mesh->position_index_sources_begin_indices + index) * 2));
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
}

class TexturePanelTests : public QObject
{
    Q_OBJECT
private slots:
    void riceBoundaryMeshCanBeHighlightedPickedAndRendered()
    {
        const QString mocPath = QString(TEST_RESOURCES) + QStringLiteral("/Rice/Rice.moc3");
        CReader reader(mocPath);
        CSectionTable table(reader);
        QVERIFY(table.IsValid());
        CArt_meshes* target = nullptr;
        QStringList order;
        for (auto* mesh : table.art_meshes) {
            order.append(mesh->ids);
            if (mesh->ids == QLatin1String("ArtMesh183")) target = mesh;
        }
        QVERIFY(target);
        QCOMPARE(target->texture_nos, 1);
        const auto textures = texturesFor("Rice");
        QCOMPARE(textures.size(), 2);
        const QImage atlas(textures.value(1));
        QVERIFY(!atlas.isNull());
        QPointF inside;
        bool found = false;
        for (int triangle = 0; triangle < target->position_index_sources_counts && !found; triangle += 3) {
            QPointF points[3];
            for (int vertex = 0; vertex < 3; ++vertex) {
                const int index = table.position_indices->indices[target->position_index_sources_begin_indices + triangle + vertex];
                points[vertex] = QPointF(double(table.uvs->uvs[target->uv_sources_begin_indices + index * 2]) * atlas.width(),
                                        double(table.uvs->uvs[target->uv_sources_begin_indices + index * 2 + 1]) * atlas.height());
            }
            const QPointF candidate = (points[0] + points[1] + points[2]) / 3;
            const QPointF a = points[1] - points[0], b = points[2] - points[0];
            if (a.x() * b.y() - a.y() * b.x() != 0 && QRectF(atlas.rect()).contains(candidate)) {
                inside = candidate;
                found = true;
            }
        }
        QVERIFY(found);
        CTexturePanel panel;
        panel.resize(900, 700);
        panel.LoadTextures(textures, mocPath);
        auto* selector = panel.findChild<QComboBox*>();
        auto* view = panel.findChild<CAtlasView*>();
        auto* wireframe = panel.findChild<QCheckBox*>();
        QVERIFY(selector && view && wireframe);
        panel.HighlightMesh(target->ids);
        QCOMPARE(selector->currentData().toInt(), 1);
        QVERIFY(wireframe->isEnabled());
        QCOMPARE(view->scene()->sceneRect(), QRectF(atlas.rect()));
        order.removeAll(target->ids);
        order.append(target->ids);
        panel.SetRenderOrder(order);
        QSignalSpy picked(&panel, &CTexturePanel::AtlasMeshPicked);
        view->ScenePosClicked(inside);
        QCOMPARE(picked.size(), 1);
        QCOMPARE(picked.takeFirst()[0].toString(), target->ids);
        for (const QPointF& point : {QPointF(-1, inside.y()), QPointF(atlas.width() + 1, inside.y()),
                                    QPointF(inside.x(), -1), QPointF(inside.x(), atlas.height() + 1),
                                    QPointF(std::numeric_limits<double>::infinity(), inside.y()),
                                    QPointF(inside.x(), std::numeric_limits<double>::quiet_NaN())})
            view->ScenePosClicked(point);
        QCOMPARE(picked.size(), 0);
        wireframe->setChecked(false);
        const QImage base = renderScene(view->scene(), 16, QSize(600, 600));
        wireframe->setChecked(true);
        const QImage overlay = renderScene(view->scene(), 16, QSize(600, 600));
        QVERIFY(overlay != base);
        const QString artifactDirectory = qEnvironmentVariable("TEXTURE_PANEL_TEST_ARTIFACT_DIR");
        if (!artifactDirectory.isEmpty()) {
            QVERIFY(QDir().mkpath(artifactDirectory));
            QVERIFY(overlay.save(QDir(artifactDirectory).filePath("rice-boundary-uv.png")));
        }
    }

    void clippedWireframeKeepsOnlyOriginalEdges_data()
    {
        QTest::addColumn<bool>("extreme");
        QTest::newRow("negative-and-beyond-atlas") << false;
        QTest::newRow("finite-extreme") << true;
    }

    void clippedWireframeKeepsOnlyOriginalEdges()
    {
        QFETCH(bool, extreme);
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString mocPath = temporary.filePath("mesh.moc3");
        const QString texturePath = temporary.filePath("atlas.png");
        QString name;
        const float maximum = std::numeric_limits<float>::max();
        const std::vector<float> uv = extreme ? std::vector<float>{-maximum, 0.25f, maximum, 0.25f, 0, maximum} :
                                               std::vector<float>{-1, 0.25f, 2, 0.25f, 0.5f, 2};
        QVERIFY(makeSingleTriangle(mocPath, uv, name));
        QImage atlas(32, 32, QImage::Format_ARGB32);
        atlas.fill(Qt::white);
        QVERIFY(atlas.save(texturePath));
        CTexturePanel panel;
        panel.LoadTextures({{0, texturePath}}, mocPath);
        auto* view = panel.findChild<CAtlasView*>();
        auto* wireframe = panel.findChild<QCheckBox*>();
        QVERIFY(view && wireframe && wireframe->isEnabled());
        wireframe->setChecked(true);
        QCOMPARE(view->scene()->sceneRect(), QRectF(0, 0, 32, 32));
        const QImage lines = renderScene(view->scene());
        int painted = 0;
        for (int y = 0; y < lines.height(); ++y) {
            for (int x = 0; x < lines.width(); ++x) {
                const bool inAtlas = x >= 8 && x < 40 && y >= 8 && y < 40;
                const QRgb expected = inAtlas ? qRgb(255, 255, 255) : qRgb(173, 11, 119);
                if (lines.pixel(x, y) == expected) continue;
                QVERIFY(inAtlas);
                QVERIFY(std::abs(y - 16) <= 2);
                ++painted;
            }
        }
        QVERIFY(painted > 20);
        panel.HighlightMesh(name);
        const QImage selected = renderScene(view->scene());
        QVERIFY(selected.pixel(24, 32) != qRgb(255, 255, 255));
        QCOMPARE(selected.pixel(24, 11), qRgb(255, 255, 255));
        for (int y = 0; y < selected.height(); ++y)
            for (int x = 0; x < selected.width(); ++x)
                if (x < 8 || x >= 40 || y < 8 || y >= 40) QCOMPARE(selected.pixel(x, y), qRgb(173, 11, 119));
        QSignalSpy picked(&panel, &CTexturePanel::AtlasMeshPicked);
        view->ScenePosClicked(QPointF(16, 24));
        QCOMPARE(picked.size(), 1);
        QCOMPARE(picked.takeFirst()[0].toString(), name);
        view->ScenePosClicked(QPointF(16, 4));
        view->ScenePosClicked(QPointF(-1, 24));
        QCOMPARE(picked.size(), 0);
    }

    void missingTexturesAndReloadsClearOldSelections()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString mocPath = temporary.filePath("mesh.moc3");
        const QString texturePath = temporary.filePath("atlas.png");
        QString name;
        QVERIFY(makeSingleTriangle(mocPath, {-1, 0.25f, 2, 0.25f, 0.5f, 2}, name));
        QImage atlas(32, 32, QImage::Format_ARGB32);
        atlas.fill(Qt::white);
        QVERIFY(atlas.save(texturePath));
        CTexturePanel panel;
        auto* selector = panel.findChild<QComboBox*>();
        auto* view = panel.findChild<CAtlasView*>();
        auto* wireframe = panel.findChild<QCheckBox*>();
        auto* exportPng = exportButton(panel);
        QVERIFY(selector && view && wireframe && exportPng);
        QVERIFY(!wireframe->isEnabled() && !exportPng->isEnabled());
        QSignalSpy picked(&panel, &CTexturePanel::AtlasMeshPicked);
        for (int reload = 0; reload < 4; ++reload) {
            panel.LoadTextures({{0, texturePath}, {1, temporary.filePath("missing.png")}}, mocPath);
            panel.HighlightMesh(name);
            wireframe->setChecked(true);
            QVERIFY(exportPng->isEnabled());
            QVERIFY(!renderScene(view->scene()).isNull());
            view->ScenePosClicked(QPointF(16, 24));
            QCOMPARE(picked.size(), 1);
            QCOMPARE(picked.takeFirst()[0].toString(), name);
            selector->setCurrentIndex(1);
            QVERIFY(view->scene()->items().isEmpty());
            QVERIFY(view->scene()->sceneRect().isEmpty());
            QVERIFY(!wireframe->isEnabled() && !exportPng->isEnabled());
            view->ScenePosClicked(QPointF(16, 24));
            QCOMPARE(picked.size(), 0);
            selector->setCurrentIndex(0);
            QVERIFY(wireframe->isEnabled() && exportPng->isEnabled());
            view->ScenePosClicked(QPointF(16, 24));
            QCOMPARE(picked.size(), 1);
            picked.clear();
            panel.LoadTextures({});
            QCOMPARE(selector->count(), 0);
            QVERIFY(view->scene()->items().isEmpty());
            QVERIFY(view->scene()->sceneRect().isEmpty());
            QVERIFY(!wireframe->isEnabled() && !exportPng->isEnabled());
            view->ScenePosClicked(QPointF(16, 24));
            QCOMPARE(picked.size(), 0);
        }
    }

    void nonfiniteMeshUvDoesNotEnterTheScene_data()
    {
        QTest::addColumn<float>("invalid");
        QTest::newRow("nan") << std::numeric_limits<float>::quiet_NaN();
        QTest::newRow("infinity") << std::numeric_limits<float>::infinity();
    }

    void nonfiniteMeshUvDoesNotEnterTheScene()
    {
        QFETCH(float, invalid);
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString mocPath = temporary.filePath("mesh.moc3");
        const QString texturePath = temporary.filePath("atlas.png");
        QString name;
        QVERIFY(makeSingleTriangle(mocPath, {invalid, 0.25f, 2, 0.25f, 0.5f, 2}, name));
        QImage atlas(32, 32, QImage::Format_ARGB32);
        atlas.fill(Qt::white);
        QVERIFY(atlas.save(texturePath));
        CTexturePanel panel;
        panel.LoadTextures({{0, texturePath}}, mocPath);
        auto* view = panel.findChild<CAtlasView*>();
        auto* wireframe = panel.findChild<QCheckBox*>();
        QVERIFY(view && wireframe);
        QVERIFY(!wireframe->isEnabled());
        QCOMPARE(view->scene()->items().size(), 1);
        QSignalSpy picked(&panel, &CTexturePanel::AtlasMeshPicked);
        view->ScenePosClicked(QPointF(16, 24));
        QCOMPARE(picked.size(), 0);
    }
};

QTEST_MAIN(TexturePanelTests)
#include "texture_panel_tests.moc"
