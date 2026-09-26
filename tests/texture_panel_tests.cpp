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
#include <QScrollBar>
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

QByteArray meshBytes(QString& id, const QVector<float>& uvs, const QVector<quint16>& indices)
{
    CReader reader(QString(TEST_RESOURCES) + QStringLiteral("/Mark/Mark.moc3"));
    CSectionTable table(reader);
    if (!table.IsValid() || table.art_meshes.isEmpty()) return {};
    const auto* mesh = table.art_meshes[0];
    const auto* offsets = table.sectionOffsetTable->art_meshes;
    id = mesh->ids;
    QByteArray bytes = reader.m_byte;
    writeInt(bytes, table.sectionOffsetTable->count_info->count_info_offset + 4 * 4, 1);
    writeInt(bytes, offsets->vertex_counts, uvs.size() / 2);
    writeInt(bytes, offsets->texture_nos, 0);
    writeInt(bytes, offsets->position_index_sources_counts, indices.size());
    for (int i = 0; i < uvs.size(); ++i)
        writeFloat(bytes, table.sectionOffsetTable->uvs->uvs + (mesh->uv_sources_begin_indices + i) * 4, uvs[i]);
    for (int i = 0; i < indices.size(); ++i)
        qToLittleEndian(indices[i], reinterpret_cast<uchar*>(bytes.data() + table.sectionOffsetTable->position_indices->indices +
                                                           (mesh->position_index_sources_begin_indices + i) * 2));
    return bytes;
}

QGraphicsPixmapItem* atlasItem(QGraphicsScene* scene)
{
    for (auto* item : scene->items()) if (auto* pixmap = qgraphicsitem_cast<QGraphicsPixmapItem*>(item)) return pixmap;
    return nullptr;
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

    void effectiveTopologyUpdatesHitTestWithoutResettingView()
    {
        QTemporaryDir directory;
        const auto path = directory.filePath("atlas.png");
        QImage atlas(32, 32, QImage::Format_ARGB32); atlas.fill(Qt::white); QVERIFY(atlas.save(path));
        QString id;
        const QVector<float> uv{.1f,.1f,.9f,.1f,.1f,.9f,.9f,.9f};
        const auto quad = meshBytes(id, uv, {0,1,2,1,3,2});
        const auto triangle = meshBytes(id, uv, {0,1,2});
        CTexturePanel panel; panel.resize(300,250); panel.LoadTextures({{0,path}});
        QString error; QVERIFY2(panel.UpdateUvMeshes(quad,&error),qPrintable(error));
        panel.HighlightMesh(id);
        auto* view = panel.findChild<CAtlasView*>(); auto* wire = panel.findChild<QCheckBox*>();
        wire->setChecked(true); panel.show(); QVERIFY(QTest::qWaitForWindowExposed(&panel));
        view->scale(4,4); view->centerOn(20,19);
        const auto transform = view->transform(); const auto center = view->mapToScene(view->viewport()->rect().center());
        const int horizontal = view->horizontalScrollBar()->value(), vertical = view->verticalScrollBar()->value();
        auto* pixmap = atlasItem(view->scene()); QVERIFY(pixmap);
        const auto originalPixmap = pixmap->pixmap().cacheKey();
        const auto oldImage = renderScene(view->scene());
        QSignalSpy picked(&panel,&CTexturePanel::AtlasMeshPicked);
        view->ScenePosClicked(QPointF(25,25)); QCOMPARE(picked.size(),1); picked.clear();
        QVERIFY2(panel.UpdateUvMeshes(triangle,&error),qPrintable(error));
        QCOMPARE(atlasItem(view->scene()),pixmap); QCOMPARE(pixmap->pixmap().cacheKey(),originalPixmap);
        QCOMPARE(view->transform(),transform); QCOMPARE(view->horizontalScrollBar()->value(),horizontal);
        QCOMPARE(view->verticalScrollBar()->value(),vertical); QCOMPARE(view->mapToScene(view->viewport()->rect().center()),center);
        QVERIFY(wire->isChecked());
        view->ScenePosClicked(QPointF(25,25)); QCOMPARE(picked.size(),0);
        view->ScenePosClicked(QPointF(8,8)); QCOMPARE(picked.size(),1); QCOMPARE(picked.takeFirst()[0].toString(),id);
        QVERIFY(renderScene(view->scene()) != oldImage);
        QVERIFY2(panel.UpdateUvMeshes(quad,&error),qPrintable(error));
        QCOMPARE(renderScene(view->scene()),oldImage);
        view->ScenePosClicked(QPointF(25,25)); QCOMPARE(picked.size(),1);
    }

    void uvAndVertexCountChangesUsePassedBytes()
    {
        QTemporaryDir directory; const auto path=directory.filePath("atlas.png");
        QImage atlas(32,32,QImage::Format_ARGB32); atlas.fill(Qt::white); QVERIFY(atlas.save(path));
        QString id;
        const auto original=meshBytes(id,{.1f,.1f,.9f,.1f,.1f,.9f,.9f,.9f},{0,1,2,1,3,2});
        const auto moved=meshBytes(id,{.1f,.1f,.4f,.1f,.1f,.4f,.4f,.4f,.25f,.25f},{0,1,4,1,3,4,3,2,4,2,0,4});
        CTexturePanel panel;panel.LoadTextures({{0,path}});QString error;
        QVERIFY(panel.UpdateUvMeshes(original,&error));auto* view=panel.findChild<CAtlasView*>();
        QSignalSpy picked(&panel,&CTexturePanel::AtlasMeshPicked);
        view->ScenePosClicked(QPointF(25,25));QCOMPARE(picked.size(),1);picked.clear();
        QVERIFY(panel.UpdateUvMeshes(moved,&error));
        view->ScenePosClicked(QPointF(25,25));QCOMPARE(picked.size(),0);
        view->ScenePosClicked(QPointF(8,8));QCOMPARE(picked.size(),1);QCOMPARE(picked.takeFirst()[0].toString(),id);
        QVERIFY(panel.UpdateUvMeshes(original,&error));
        view->ScenePosClicked(QPointF(25,25));QCOMPARE(picked.size(),1);
    }

    void invalidGeometryPreservesEverything_data()
    {
        QTest::addColumn<int>("kind");
        for(int kind=0;kind<8;++kind)QTest::newRow(qPrintable(QString::number(kind)))<<kind;
    }
    void invalidGeometryPreservesEverything()
    {
        QFETCH(int,kind);
        QTemporaryDir directory;const auto path=directory.filePath("atlas.png");
        QImage atlas(32,32,QImage::Format_ARGB32);atlas.fill(Qt::white);QVERIFY(atlas.save(path));
        QString id;const auto original=meshBytes(id,{.1f,.1f,.9f,.1f,.1f,.9f},{0,1,2});
        CTexturePanel panel;panel.LoadTextures({{0,path}});QVERIFY(panel.UpdateUvMeshes(original));panel.HighlightMesh(id);
        auto* view=panel.findChild<CAtlasView*>();panel.findChild<QCheckBox*>()->setChecked(true);
        const auto items=view->scene()->items();const auto image=renderScene(view->scene());const auto transform=view->transform();
        QByteArray invalid=original;auto reader=CReader::FromBytes(original);CSectionTable table(reader);
        const auto* mesh=table.art_meshes[0];const auto* offsets=table.sectionOffsetTable->art_meshes;
        if(kind==0)invalid.clear();
        if(kind==1)invalid.truncate(17);
        if(kind==2)writeFloat(invalid,table.sectionOffsetTable->uvs->uvs+mesh->uv_sources_begin_indices*4,std::numeric_limits<float>::quiet_NaN());
        if(kind==3)writeFloat(invalid,table.sectionOffsetTable->uvs->uvs+mesh->uv_sources_begin_indices*4,std::numeric_limits<float>::infinity());
        if(kind==4)qToLittleEndian<quint16>(3,reinterpret_cast<uchar*>(invalid.data()+table.sectionOffsetTable->position_indices->indices+mesh->position_index_sources_begin_indices*2));
        if(kind==5)writeInt(invalid,offsets->position_index_sources_counts,4);
        if(kind==6)writeInt(invalid,offsets->texture_nos,7);
        if(kind==7)writeInt(invalid,offsets->uv_sources_begin_indices,INT_MAX);
        QString error;QVERIFY(!panel.UpdateUvMeshes(invalid,&error));QVERIFY(!error.isEmpty());
        QCOMPARE(view->scene()->items(),items);QCOMPARE(view->transform(),transform);QCOMPARE(renderScene(view->scene()),image);
        QSignalSpy picked(&panel,&CTexturePanel::AtlasMeshPicked);view->ScenePosClicked(QPointF(8,8));QCOMPARE(picked.size(),1);
    }

    void unchangedGeometryKeepsSceneAndCachedAtlas()
    {
        QTemporaryDir directory;const auto path=directory.filePath("atlas.png");
        QImage atlas(32,32,QImage::Format_ARGB32);atlas.fill(Qt::white);QVERIFY(atlas.save(path));
        QString id;const auto original=meshBytes(id,{.1f,.1f,.9f,.1f,.1f,.9f},{0,1,2});
        CTexturePanel panel;panel.LoadTextures({{0,path}});QVERIFY(panel.UpdateUvMeshes(original));panel.HighlightMesh(id);
        auto* view=panel.findChild<CAtlasView*>();const auto items=view->scene()->items();
        auto reader=CReader::FromBytes(original);CSectionTable table(reader);auto unrelated=original;
        writeFloat(unrelated,table.sectionOffsetTable->art_mesh_keyforms->opacities+table.art_meshes[0]->keyform_sources_begin_indices*4,.25f);
        QVERIFY(QFile::remove(path));
        QVERIFY(panel.UpdateUvMeshes(unrelated));QCOMPARE(view->scene()->items(),items);
        const auto changed=meshBytes(id,{.2f,.2f,.8f,.2f,.2f,.8f},{0,1,2});
        auto* pixmap=atlasItem(view->scene());QVERIFY(panel.UpdateUvMeshes(changed));QCOMPARE(atlasItem(view->scene()),pixmap);
        QVERIFY(!pixmap->pixmap().isNull());
    }

    void activeAtlasAndRuntimeOrderSurviveUpdates()
    {
        QTemporaryDir directory;const auto path=directory.filePath("atlas.png");const auto otherPath=directory.filePath("other.png");
        QImage atlas(32,32,QImage::Format_ARGB32);atlas.fill(Qt::white);QVERIFY(atlas.save(path));
        QImage other(48,16,QImage::Format_ARGB32);other.fill(Qt::green);QVERIFY(other.save(otherPath));
        CReader source(QString(TEST_RESOURCES)+"/Mark/Mark.moc3");CSectionTable table(source);QVERIFY(table.IsValid());QVERIFY(table.art_meshes.size()>=2);
        auto original=source.m_byte;writeInt(original,table.sectionOffsetTable->count_info->count_info_offset+4*4,2);
        QStringList ids;
        for(int row=0;row<2;++row){
            const auto* mesh=table.art_meshes[row];ids.append(mesh->ids);const auto* offsets=table.sectionOffsetTable->art_meshes;
            writeInt(original,offsets->vertex_counts+row*4,3);writeInt(original,offsets->position_index_sources_counts+row*4,3);writeInt(original,offsets->texture_nos+row*4,0);
            const QVector<float> uv{.1f,.1f,.9f,.1f,.1f,.9f};
            for(int i=0;i<uv.size();++i)writeFloat(original,table.sectionOffsetTable->uvs->uvs+(mesh->uv_sources_begin_indices+i)*4,uv[i]);
            for(quint16 i=0;i<3;++i)qToLittleEndian(i,reinterpret_cast<uchar*>(original.data()+table.sectionOffsetTable->position_indices->indices+(mesh->position_index_sources_begin_indices+i)*2));
        }
        CTexturePanel panel;panel.LoadTextures({{0,path},{1,otherPath}});QVERIFY(panel.UpdateUvMeshes(original));
        panel.HighlightMesh(ids[0]);panel.SetRenderOrder({ids[1],ids[0]});
        auto* selector=panel.findChild<QComboBox*>();auto* view=panel.findChild<CAtlasView*>();selector->setCurrentIndex(1);
        QCOMPARE(selector->currentData().toInt(),1);auto* pixmap=atlasItem(view->scene());const auto rect=view->sceneRect();const auto transform=view->transform();
        auto changed=original;
        for(int row=0;row<2;++row){
            const auto* mesh=table.art_meshes[row];
            writeFloat(changed,table.sectionOffsetTable->uvs->uvs+mesh->uv_sources_begin_indices*4,.2f);
            writeFloat(changed,table.sectionOffsetTable->art_mesh_keyforms->draw_orders+mesh->keyform_sources_begin_indices*4,float(100+row*100));
        }
        QVERIFY(panel.UpdateUvMeshes(changed));QCOMPARE(selector->currentData().toInt(),1);QCOMPARE(atlasItem(view->scene()),pixmap);QCOMPARE(view->sceneRect(),rect);QCOMPARE(view->transform(),transform);
        selector->setCurrentIndex(0);QSignalSpy picked(&panel,&CTexturePanel::AtlasMeshPicked);view->ScenePosClicked(QPointF(8,8));
        QCOMPARE(picked.size(),1);QCOMPARE(picked.first()[0].toString(),ids[0]);
    }

    void originalModelGeometryIsStable_data()
    {
        QTest::addColumn<QString>("model");
        for(const char* name:{"Mark","Natori","Rice","Wanko"})QTest::newRow(name)<<QString::fromLatin1(name);
    }
    void originalModelGeometryIsStable()
    {
        QFETCH(QString,model);const QString path=QString(TEST_RESOURCES)+'/'+model+'/'+model+".moc3";
        QFile file(path);QVERIFY(file.open(QIODevice::ReadOnly));const auto bytes=file.readAll();
        CTexturePanel panel;panel.LoadTextures(texturesFor(model),path);auto* view=panel.findChild<CAtlasView*>();
        const auto items=view->scene()->items();QVERIFY(items.size()>1);QString error;
        QVERIFY2(panel.UpdateUvMeshes(bytes,&error),qPrintable(error));QCOMPARE(view->scene()->items(),items);
    }

    void emptyTopologyClearsOnlyOverlayAndRestoresSelection()
    {
        QTemporaryDir directory;const auto path=directory.filePath("atlas.png");QImage atlas(32,32,QImage::Format_ARGB32);atlas.fill(Qt::white);QVERIFY(atlas.save(path));
        QString id;const auto original=meshBytes(id,{.1f,.1f,.9f,.1f,.1f,.9f},{0,1,2});
        CTexturePanel panel;panel.LoadTextures({{0,path}});QVERIFY(panel.UpdateUvMeshes(original));panel.HighlightMesh(id);
        auto* view=panel.findChild<CAtlasView*>();auto* wire=panel.findChild<QCheckBox*>();wire->setChecked(true);const auto selected=renderScene(view->scene());auto* pixmap=atlasItem(view->scene());
        QVERIFY(panel.UpdateUvMeshes(meshBytes(id,{},{})));QCOMPARE(view->scene()->items().size(),1);QCOMPARE(atlasItem(view->scene()),pixmap);QVERIFY(!wire->isEnabled());
        QVERIFY(panel.UpdateUvMeshes(original));QVERIFY(wire->isEnabled());QVERIFY(wire->isChecked());QCOMPARE(renderScene(view->scene()),selected);
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
