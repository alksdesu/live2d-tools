#include "ui/ctexturepanel.h"
#include "ui/theme.h"
#include <QScopeGuard>
#include "live2dstruct/csectiontable.h"
#include "deformer_replay_fixture.h"
#include "wide_mesh_fixture.h"
#include <QComboBox>

#include <QtTest>
#include <QAction>
#include <QBuffer>
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
    void artworkWarningIsIndependentOfSnapshotAndEditingState()
    {
        QImage image(32, 24, QImage::Format_RGBA8888); image.fill(Qt::white); QBuffer buffer; QVERIFY(buffer.open(QIODevice::WriteOnly)); QVERIFY(image.save(&buffer, "PNG"));
        CTextureAssets::Source source; QVERIFY(CTextureAssets::MakeSource("/frozen/atlas.png", buffer.data(), source)); const CTextureAssets::Catalog sources{{0, source}};
        CTextureWorkspace::State workspace; QVERIFY(CTextureWorkspace::Initialize(sources, {}, workspace)); QString id;
        const auto moc = meshBytes(id, {.1f, .1f, .9f, .1f, .1f, .9f}, {0, 1, 2});
        CTexturePanel panel; QVERIFY(panel.LoadWorkspaceSnapshot(sources, workspace, moc)); panel.SetEditingEnabled(true); panel.SetEditHandler([](int, const QByteArray&, QString*) { return true; });
        auto* warning = panel.findChild<QLabel*>("textureArtworkWarning"); auto* error = panel.findChild<QLabel*>("textureError"); QVERIFY(warning && warning->isHidden());
        auto* view = panel.findChild<CAtlasView*>(); const auto scene = view->scene(); const auto pixels = atlasItem(scene)->pixmap().cacheKey();
        const QString reason = QStringLiteral("<b>附件校验失败</b>\n保留正常模型，来源未恢复"); panel.SetArtworkLoadWarning(reason);
        QCOMPARE(warning->text(), QStringLiteral("来源附件未载入：") + reason); QCOMPARE(warning->textFormat(), Qt::PlainText); QVERIFY(warning->wordWrap()); QVERIFY(!warning->isHidden());
        QVERIFY(warning->textInteractionFlags().testFlag(Qt::TextSelectableByMouse)); QVERIFY(warning->textInteractionFlags().testFlag(Qt::TextSelectableByKeyboard));
        for (const char* name : {"textureReplace", "textureEditUv", "textureEditAtlas", "textureExport"}) QVERIFY2(panel.findChild<QToolButton*>(name)->isEnabled(), name);
        QVERIFY(error->isHidden()); QCOMPARE(view->scene(), scene); QCOMPARE(atlasItem(scene)->pixmap().cacheKey(), pixels);
        QVERIFY(panel.LoadWorkspaceSnapshot(sources, workspace, moc)); panel.HighlightMesh(id); QCOMPARE(warning->text(), QStringLiteral("来源附件未载入：") + reason);
        panel.SetSnapshotAvailable(false, "broken snapshot"); QVERIFY(!panel.findChild<QToolButton*>("textureExport")->isEnabled()); QVERIFY(!error->isHidden());
        panel.SetArtworkLoadWarning({}); QVERIFY(warning->isHidden()); QVERIFY(warning->text().isEmpty()); QVERIFY(!error->isHidden()); QVERIFY(!panel.findChild<QToolButton*>("textureExport")->isEnabled());
        panel.SetArtworkLoadWarning("来源读取已取消"); panel.SetSnapshotAvailable(true); QVERIFY(error->isHidden()); QVERIFY(!warning->isHidden()); QVERIFY(panel.findChild<QToolButton*>("textureExport")->isEnabled());
        panel.SetPreviewActive(true); panel.SetArtworkLoadWarning({}); QVERIFY(!panel.findChild<QToolButton*>("textureExport")->isEnabled());
        panel.SetPreviewActive(false); QVERIFY(panel.findChild<QToolButton*>("textureExport")->isEnabled()); QCOMPARE(view->scene(), scene);
    }
    void artworkWarningWrapsInThemedNarrowPanels_data()
    {
        QTest::addColumn<bool>("dark"); QTest::addColumn<int>("width");
        for (bool dark : {false, true}) for (int width : {360, 640}) QTest::newRow(qPrintable(QString("%1-%2").arg(dark).arg(width))) << dark << width;
    }
    void artworkWarningWrapsInThemedNarrowPanels()
    {
        QFETCH(bool, dark); QFETCH(int, width); const auto before = qApp->styleSheet(); qApp->setStyleSheet(Theme::globalStyleSheet(dark)); const auto restore = qScopeGuard([before] { qApp->setStyleSheet(before); });
        CTexturePanel panel; panel.resize(width, 460); panel.SetArtworkLoadWarning(QStringLiteral("附件内容不完整，未恢复原稿版本与图层关联。当前模型图像仍可正常使用；请检查来源文件后重新打开。")); panel.show(); QTest::qWait(100);
        auto* warning = panel.findChild<QLabel*>("textureArtworkWarning"); QVERIFY(warning->isVisible());
        QVERIFY(warning->geometry().right() < panel.widget()->width()); QVERIFY(warning->height() >= warning->heightForWidth(warning->width()));
        QVERIFY(panel.widget()->rect().contains(warning->geometry())); QCOMPARE(panel.width(), width);
        const auto path = qEnvironmentVariable("ARTWORK_WARNING_QA_DIR"); if (!path.isEmpty()) { QVERIFY(QDir().mkpath(path)); QVERIFY(panel.grab().save(QDir(path).filePath(QString("warning-%1-%2.png").arg(dark ? "dark" : "light").arg(width)))); }
        panel.SetArtworkLoadWarning({}); QTest::qWait(20); QVERIFY(warning->isHidden());
    }
    void workspaceFastPathPreservesViewAndFailedResizeKeepsOldPixelsAndUv()
    {
        QTemporaryDir directory; const auto path = directory.filePath("atlas.png");
        QImage image(32, 24, QImage::Format_RGBA8888); image.fill(Qt::white); QVERIFY(image.save(path));
        QFile file(path); QVERIFY(file.open(QIODevice::ReadOnly)); CTextureAssets::Source source;
        QVERIFY(CTextureAssets::MakeSource(path, file.readAll(), source)); const CTextureAssets::Catalog sources{{0, source}};
        CTextureWorkspace::State workspace; QVERIFY(CTextureWorkspace::Initialize(sources, {}, workspace));
        QString id; const auto original = meshBytes(id, {.1f, .1f, .9f, .1f, .1f, .9f}, {0, 1, 2});
        const auto moved = meshBytes(id, {.2f, .2f, .5f, .2f, .2f, .5f}, {0, 1, 2});
        CTexturePanel panel; QString error; QVERIFY(!panel.LoadWorkspaceSnapshot({}, {}, {}, &error));
        QVERIFY(panel.LoadWorkspaceSnapshot(sources, workspace, original, &error)); panel.resize(500, 360); panel.show();
        auto* view = panel.findChild<CAtlasView*>(); view->ZoomReset(); view->scale(3, 3);
        const auto transform = view->transform(); auto* scene = view->scene(); auto* pixmap = atlasItem(scene); const auto key = pixmap->pixmap().cacheKey();
        QVERIFY(panel.LoadWorkspaceSnapshot(sources, workspace, moved, &error));
        QCOMPARE(view->scene(), scene); QCOMPARE(atlasItem(scene), pixmap); QCOMPARE(pixmap->pixmap().cacheKey(), key); QCOMPARE(view->transform(), transform);
        QVERIFY(panel.LoadWorkspaceSnapshot(sources, workspace, original, &error)); const auto before = renderScene(scene);
        auto failed = workspace; failed.atlases[0].size = {64, 48}; failed.atlases[0].png = QByteArray("invalid png");
        QVERIFY(!panel.LoadWorkspaceSnapshot(sources, failed, moved, &error)); panel.SetSnapshotAvailable(false, error);
        QVERIFY(!panel.findChild<QToolButton*>("textureExport")->isEnabled());
        QVERIFY(panel.findChild<QLabel*>("textureError")->text().contains(QStringLiteral("仅供参考")));
        QCOMPARE(view->scene(), scene); QCOMPARE(renderScene(scene), before); QCOMPARE(view->transform(), transform);
        QVERIFY(!panel.LoadWorkspaceSnapshot(sources, failed, moved, &error)); QVERIFY(!panel.UpdateUvMeshes(moved, &error));
        QCOMPARE(renderScene(scene), before); QCOMPARE(atlasItem(scene), pixmap); QCOMPARE(view->sceneRect().size(), QSizeF(32, 24));
        QSignalSpy picked(&panel, &CTexturePanel::AtlasMeshPicked); view->ScenePosClicked({8, 6}); QCOMPARE(picked.size(), 0);
        QVERIFY(panel.LoadWorkspaceSnapshot(sources, workspace, original, &error)); panel.SetSnapshotAvailable(true);
        QVERIFY(panel.findChild<QToolButton*>("textureExport")->isEnabled()); QVERIFY(panel.findChild<QLabel*>("textureError")->isHidden());
        view->ScenePosClicked({8, 6}); QCOMPARE(picked.size(), 1); QCOMPARE(picked.first()[0].toString(), id);
    }
    void workspaceMocAndNonSquareAtlasPublishTogether()
    {
        QTemporaryDir directory; const auto path = directory.filePath("atlas.png");
        QImage originalImage(64, 32, QImage::Format_RGBA8888); originalImage.fill(Qt::white); QVERIFY(originalImage.save(path));
        QFile originalFile(path); QVERIFY(originalFile.open(QIODevice::ReadOnly)); CTextureAssets::Source source;
        QVERIFY(CTextureAssets::MakeSource(path, originalFile.readAll(), source)); const CTextureAssets::Catalog sources{{0, source}};
        CTextureWorkspace::State workspace; QVERIFY(CTextureWorkspace::Initialize(sources, {}, workspace));
        QString id; const auto original = meshBytes(id, {.1f, .1f, .9f, .1f, .1f, .9f}, {0, 1, 2});
        CTexturePanel panel; QString error; QVERIFY2(panel.LoadWorkspaceSnapshot(sources, workspace, original, &error), qPrintable(error));
        panel.HighlightMesh(id); auto* view = panel.findChild<CAtlasView*>(); QSignalSpy picked(&panel, &CTexturePanel::AtlasMeshPicked);
        view->ScenePosClicked({12, 6}); QCOMPARE(picked.size(), 1); picked.clear();
        QImage newImage(120, 60, QImage::Format_RGBA8888); newImage.fill(Qt::blue); QBuffer png;
        QVERIFY(png.open(QIODevice::WriteOnly)); QVERIFY(newImage.save(&png, "PNG"));
        workspace.atlases.insert(1, {"generated-atlas", "新图集", -1, newImage.size(), png.data()});
        auto moved = meshBytes(id, {.2f, .2f, .6f, .2f, .2f, .6f}, {0, 1, 2});
        auto reader = CReader::FromBytes(moved); CSectionTable table(reader); QVERIFY(table.IsValid());
        writeInt(moved, table.sectionOffsetTable->art_meshes->texture_nos, 1);
        QVERIFY2(panel.LoadWorkspaceSnapshot(sources, workspace, moved, &error), qPrintable(error)); QCOMPARE(panel.CurrentSlot(), 1);
        QCOMPARE(view->scene()->sceneRect().size(), QSizeF(120, 60));
        QCOMPARE(atlasItem(view->scene())->pixmap().toImage().pixelColor(0, 0), QColor(Qt::blue));
        view->ScenePosClicked({12, 6}); QCOMPARE(picked.size(), 0);
        view->ScenePosClicked({48, 15}); QCOMPARE(picked.size(), 1); QCOMPARE(picked.first()[0].toString(), id); picked.clear();
        panel.SetEditingEnabled(true); auto* edit = panel.findChild<QToolButton*>("textureEditAtlas"); QVERIFY(edit && edit->isEnabled());
        QSignalSpy requested(&panel, &CTexturePanel::EditAtlasRequested); edit->defaultAction()->trigger(); QCOMPARE(requested.size(), 1); QCOMPARE(requested.first()[0].toString(), id);
        const auto scene = view->scene(); const auto image = renderScene(scene); const auto transform = view->transform();
        auto invalid = moved; writeInt(invalid, table.sectionOffsetTable->art_meshes->texture_nos, 2);
        QVERIFY(!panel.LoadWorkspaceSnapshot(sources, workspace, invalid, &error)); QCOMPARE(view->scene(), scene);
        QCOMPARE(view->transform(), transform); QCOMPARE(renderScene(view->scene()), image); QCOMPARE(panel.CurrentSlot(), 1);
        auto deleted = workspace; deleted.atlases.remove(1);
        QVERIFY(!panel.LoadWorkspaceSnapshot(sources, deleted, {}, &error)); QCOMPARE(view->scene(), scene); QCOMPARE(panel.CurrentSlot(), 1);
        QVERIFY2(panel.LoadWorkspaceSnapshot(sources, deleted, original, &error), qPrintable(error)); QCOMPARE(panel.CurrentSlot(), 0);
        QCOMPARE(view->scene()->sceneRect().size(), QSizeF(64, 32));
        QByteArray external(original.constData(), original.size());
        QVERIFY(panel.LoadWorkspaceSnapshot(sources, deleted, QByteArray::fromRawData(external.constData(), external.size()), &error));
        external[0] = 'x';
        QImage resized(128, 96, QImage::Format_RGBA8888); resized.fill(Qt::green); QBuffer resizedPng;
        QVERIFY(resizedPng.open(QIODevice::WriteOnly)); QVERIFY(resized.save(&resizedPng, "PNG"));
        deleted.atlases[0].size = resized.size(); deleted.atlases[0].png = resizedPng.data();
        QVERIFY2(panel.LoadWorkspaceSnapshot(sources, deleted, {}, &error), qPrintable(error)); QCOMPARE(view->scene()->sceneRect().size(), QSizeF(128, 96));
        view->ScenePosClicked({32, 48}); QCOMPARE(picked.size(), 1); QCOMPARE(picked.first()[0].toString(), id);
    }
    void canonicalWideMeshIsPickedThroughViewportClick()
    {
        WideMeshData::Fixture wide; QString error; QVERIFY2(WideMeshData::make(65536,WideMeshData::Layout::Sparse,wide,&error),qPrintable(error));
        auto reader = CReader::FromBytes(wide.bytes); CSectionTable table(reader); QVERIFY(table.IsValid());
        const auto target = std::find_if(table.art_meshes.cbegin(),table.art_meshes.cend(),[&](const auto* mesh){return mesh->ids == wide.ids.first();}); QVERIFY(target != table.art_meshes.cend());
        CTexturePanel panel; panel.LoadTextures(texturesFor("Mark")); panel.SetEditingEnabled(true); QVERIFY2(panel.UpdateUvMeshes(wide.bytes,&error),qPrintable(error));
        auto* selector = panel.findChild<QComboBox*>("textureSlot"); QVERIFY(selector); const int slot = selector->findData((*target)->texture_nos); QVERIFY(slot >= 0); selector->setCurrentIndex(slot);
        QStringList order; for (const auto* mesh : table.art_meshes) if (mesh->ids != wide.ids.first()) order.append(mesh->ids); order.append(wide.ids.first()); panel.SetRenderOrder(order);
        panel.resize(600,500); panel.show(); QVERIFY(QTest::qWaitForWindowExposed(&panel)); QTest::qWait(40);
        auto* view = panel.findChild<CAtlasView*>(); QVERIFY(view); QSignalSpy picked(&panel,&CTexturePanel::AtlasMeshPicked);
        const QPoint click = view->mapFromScene(view->scene()->sceneRect().center()); QVERIFY(view->viewport()->rect().contains(click)); QTest::mouseClick(view->viewport(),Qt::LeftButton,{},click);
        QCOMPARE(picked.size(),1); QCOMPARE(picked.first().first().toString(),wide.ids.first());
        panel.HighlightMesh(wide.ids.first()); QSignalSpy requested(&panel,&CTexturePanel::EditUvRequested); auto* edit = panel.findChild<QToolButton*>("textureEditUv"); QVERIFY(edit && edit->isEnabled()); edit->defaultAction()->trigger(); QCOMPARE(requested.size(),1); QCOMPARE(requested.first().first().toString(),wide.ids.first());
    }
    void unsignedTriangleIndicesKeepHighVerticesSelectable()
    {
        const auto source = DeformerReplayFixture::read("Mark");
        auto reader = CReader::FromBytes(source); CSectionTable table(reader); QVERIFY(table.IsValid());
        DeformerReplayFixture::RawColumns raw(source);
        auto uvs = raw.column("uvs.uvs"); const int uvStart = uvs.size() / 4;
        constexpr int count = 32769;
        uvs.append(QByteArray(count * 8, '\0'));
        writeFloat(uvs, uvStart * 4, .1f); writeFloat(uvs, uvStart * 4 + 4, .1f);
        writeFloat(uvs, uvStart * 4 + 8, .9f); writeFloat(uvs, uvStart * 4 + 12, .1f);
        writeFloat(uvs, (uvStart + (count - 1) * 2) * 4, .1f);
        writeFloat(uvs, (uvStart + (count - 1) * 2 + 1) * 4, .9f);
        const auto uvSpan = raw.spans.value("uvs.uvs");
        raw.state.columns[uvSpan.slot] = uvs; raw.state.counts[uvSpan.countIndex] = uvs.size() / 4;
        auto indices = raw.column("position_indices.indices"); const int indexStart = indices.size() / 2;
        indices.append(QByteArray(6, '\0'));
        qToLittleEndian<quint16>(1, indices.data() + indexStart * 2 + 2);
        qToLittleEndian<quint16>(count - 1, indices.data() + indexStart * 2 + 4);
        const auto indexSpan = raw.spans.value("position_indices.indices");
        raw.state.columns[indexSpan.slot] = indices; raw.state.counts[indexSpan.countIndex] = indices.size() / 2;
        raw.set("art_meshes.vertex_counts", 0, count); raw.set("art_meshes.uv_sources_begin_indices", 0, uvStart);
        raw.set("art_meshes.position_index_sources_begin_indices", 0, indexStart);
        raw.set("art_meshes.position_index_sources_counts", 0, 3);
        QString error; const auto bytes = raw.encode(&error); QVERIFY2(!bytes.isEmpty(), qPrintable(error));
        CTexturePanel panel; panel.LoadTextures(texturesFor("Mark")); panel.SetEditingEnabled(true);
        auto* edit = panel.findChild<QToolButton*>("textureEditUv"); QVERIFY(edit); QVERIFY(!edit->isEnabled());
        QVERIFY2(panel.UpdateUvMeshes(bytes, &error), qPrintable(error)); QVERIFY(edit->isEnabled());
        const QString target = table.art_meshes[0]->ids;
        QStringList order; for (const auto* drawable : table.art_meshes) if (drawable->ids != target) order.append(drawable->ids);
        order.append(target); panel.SetRenderOrder(order);
        QSignalSpy picked(&panel, &CTexturePanel::AtlasMeshPicked); auto* view = panel.findChild<CAtlasView*>();
        const auto size = view->scene()->sceneRect().size(); view->ScenePosClicked({size.width() * .2, size.height() * .2});
        QCOMPARE(picked.size(), 1); QCOMPARE(picked.first().first().toString(), target);
        panel.HighlightMesh(target); QSignalSpy requested(&panel, &CTexturePanel::EditUvRequested);
        edit->defaultAction()->trigger(); QCOMPARE(requested.size(), 1); QCOMPARE(requested.first().first().toString(), target);
    }
    void sharedUvUsesCoreSamplingCoordinates()
    {
        const auto source = DeformerReplayFixture::read("Mark");
        auto reader = CReader::FromBytes(source); CSectionTable table(reader); QVERIFY(table.IsValid());
        auto bytes = source; const auto* offsets = table.sectionOffsetTable->art_meshes;
        const auto* mesh = table.art_meshes[0];
        writeInt(bytes, table.sectionOffsetTable->count_info->count_info_offset + 4 * 4, 2);
        for (int index = 0; index < 2; ++index) {
            writeInt(bytes, offsets->vertex_counts + index * 4, 3);
            writeInt(bytes, offsets->texture_nos + index * 4, 0);
            writeInt(bytes, offsets->uv_sources_begin_indices + index * 4, mesh->uv_sources_begin_indices);
            writeInt(bytes, offsets->position_index_sources_begin_indices + index * 4, mesh->position_index_sources_begin_indices);
            writeInt(bytes, offsets->position_index_sources_counts + index * 4, 3);
        }
        const QVector<float> uvs{.1f, .1f, .4f, .1f, .1f, .4f};
        for (int index = 0; index < uvs.size(); ++index)
            writeFloat(bytes, table.sectionOffsetTable->uvs->uvs + (mesh->uv_sources_begin_indices + index) * 4, uvs[index]);
        for (int index = 0; index < 3; ++index)
            qToLittleEndian<quint16>(index, bytes.data() + table.sectionOffsetTable->position_indices->indices + (mesh->position_index_sources_begin_indices + index) * 2);
        CTexturePanel panel; panel.LoadTextures(texturesFor("Mark")); QString error;
        QVERIFY2(panel.UpdateUvMeshes(bytes, &error), qPrintable(error));
        const QString target = table.art_meshes[1]->ids;
        panel.SetRenderOrder({table.art_meshes[0]->ids, target});
        QSignalSpy picked(&panel, &CTexturePanel::AtlasMeshPicked); auto* view = panel.findChild<CAtlasView*>();
        const auto size = view->scene()->sceneRect().size();
        view->ScenePosClicked({size.width() * .15, size.height() * .15}); QCOMPARE(picked.size(), 0);
        view->ScenePosClicked({size.width() * .15, size.height() * .85}); QCOMPARE(picked.size(), 1);
        QCOMPARE(picked.first().first().toString(), target);
    }
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

    void failedTextureReloadPreservesSnapshotAndEmptyReloadClearsSelections()
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
            panel.LoadTextures({{0, texturePath}}, mocPath);
            panel.HighlightMesh(name);
            wireframe->setChecked(true);
            QVERIFY(exportPng->isEnabled());
            QVERIFY(!renderScene(view->scene()).isNull());
            view->ScenePosClicked(QPointF(16, 24));
            QCOMPARE(picked.size(), 1);
            QCOMPARE(picked.takeFirst()[0].toString(), name);
            const auto items = view->scene()->items();
            const auto sceneRect = view->scene()->sceneRect();
            panel.LoadTextures({{0, texturePath}, {1, temporary.filePath("missing.png")}}, mocPath);
            QCOMPARE(selector->count(), 1);
            QCOMPARE(view->scene()->items(), items);
            QCOMPARE(view->scene()->sceneRect(), sceneRect);
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

    void samePixelVerticesUseCurrentAtlasBounds()
    {
        CTexturePanel panel;
        QString id, error;
        for (int size : {32, 64, 32}) {
            QImage image(size, size, QImage::Format_RGBA8888);
            image.fill(Qt::white);
            QBuffer png;
            QVERIFY(png.open(QIODevice::WriteOnly));
            QVERIFY(image.save(&png, "PNG"));
            CTextureAssets::Source source;
            QVERIFY(CTextureAssets::MakeSource("/frozen/atlas.png", png.data(), source));
            const CTextureAssets::Catalog sources{{0, source}};
            CTextureWorkspace::State workspace;
            QVERIFY(CTextureWorkspace::Initialize(sources, {}, workspace));
            const auto moc = meshBytes(id, {8.f / size, 8.f / size, 56.f / size, 8.f / size,
                8.f / size, 56.f / size}, {0, 1, 2});
            QVERIFY2(panel.LoadWorkspaceSnapshot(sources, workspace, moc, &error), qPrintable(error));
            panel.HighlightMesh(id);
            panel.findChild<QCheckBox*>()->setChecked(true);
            CTexturePanel fresh;
            QVERIFY2(fresh.LoadWorkspaceSnapshot(sources, workspace, moc, &error), qPrintable(error));
            fresh.HighlightMesh(id);
            fresh.findChild<QCheckBox*>()->setChecked(true);
            auto* view = panel.findChild<CAtlasView*>();
            QCOMPARE(view->sceneRect().size(), QSizeF(size, size));
            QCOMPARE(renderScene(view->scene()), renderScene(fresh.findChild<CAtlasView*>()->scene()));
            QSignalSpy picked(&panel, &CTexturePanel::AtlasMeshPicked);
            view->ScenePosClicked(QPointF(40, 12));
            QCOMPARE(picked.size(), size == 64 ? 1 : 0);
        }
    }

    void borrowedMocCannotMutateThePreviousSnapshot()
    {
        QTemporaryDir directory;
        const auto path = directory.filePath("atlas.png");
        QImage image(32, 32, QImage::Format_RGBA8888);
        image.fill(Qt::white);
        QVERIFY(image.save(path));
        QString id, error;
        auto moc = meshBytes(id, {.1f, .1f, .9f, .1f, .1f, .9f}, {0, 1, 2});
        auto reader = CReader::FromBytes(moc);
        CSectionTable table(reader);
        QVERIFY(table.IsValid());
        const int offset = table.sectionOffsetTable->uvs->uvs + table.art_meshes[0]->uv_sources_begin_indices * 4;
        QByteArray storage(moc.constData(), moc.size());
        const auto borrowed = QByteArray::fromRawData(storage.constData(), storage.size());
        CTexturePanel panel;
        panel.LoadTextures({{0, path}});
        QVERIFY2(panel.UpdateUvMeshes(borrowed, &error), qPrintable(error));
        auto* view = panel.findChild<CAtlasView*>();
        panel.findChild<QCheckBox*>()->setChecked(true);
        const auto original = renderScene(view->scene());
        const auto items = view->scene()->items();
        QVERIFY(panel.UpdateUvMeshes(borrowed, &error));
        QCOMPARE(view->scene()->items(), items);
        writeFloat(storage, offset, .5f);
        QVERIFY(panel.UpdateUvMeshes(borrowed, &error));
        QVERIFY(renderScene(view->scene()) != original);
        CTexturePanel fresh;
        fresh.LoadTextures({{0, path}});
        QVERIFY(fresh.UpdateUvMeshes(storage, &error));
        fresh.findChild<QCheckBox*>()->setChecked(true);
        QCOMPARE(renderScene(view->scene()), renderScene(fresh.findChild<CAtlasView*>()->scene()));
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
        panel.LoadTextures({{0, texturePath}});
        const auto originalItems = panel.findChild<CAtlasView*>()->scene()->items();
        QSignalSpy rejected(&panel, &CTexturePanel::EditRejected);
        panel.LoadTextures({{0, texturePath}}, mocPath);
        auto* view = panel.findChild<CAtlasView*>();
        auto* wireframe = panel.findChild<QCheckBox*>();
        QVERIFY(view && wireframe);
        QVERIFY(!wireframe->isEnabled());
        QCOMPARE(view->scene()->items().size(), 1);
        QCOMPARE(view->scene()->items(), originalItems);
        QCOMPARE(rejected.size(), 1);
        QSignalSpy picked(&panel, &CTexturePanel::AtlasMeshPicked);
        view->ScenePosClicked(QPointF(16, 24));
        QCOMPARE(picked.size(), 0);
    }
};

QTEST_MAIN(TexturePanelTests)
#include "texture_panel_tests.moc"
