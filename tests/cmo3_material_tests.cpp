#include "live2dstruct/cmocmaterialbuilder.h"
#include "deformer_snapshot_fixture.h"
#include "cmo3/ccaffarchive.h"
#include "cmo3/ccmo3builder.h"
#include "cmo3/ccmo3entities.h"
#include <cstring>
#include "live2dstruct/csectiontable.h"
#include <QtTest>
#include <QFile>
#include <QBuffer>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QXmlStreamReader>
#include <QRegularExpression>
#include <QtEndian>
#include <algorithm>
#include <cmath>

using namespace live2dstruct;
namespace {
struct Node {
    QString tag, field, text;
    QMap<QString, QString> attributes;
    QVector<Node> children;
    const Node* child(const QString& fieldName) const {
        for (const auto& item : children) if (item.field == fieldName) return &item;
        return nullptr;
    }
};
Node readNode(QXmlStreamReader& xml)
{
    Node node; node.tag = xml.name().toString();
    for (const auto& attribute : xml.attributes()) node.attributes.insert(attribute.name().toString(), attribute.value().toString());
    node.field = node.attributes.value("xs.n");
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement()) node.children.append(readNode(xml));
        else if (xml.isCharacters()) node.text += xml.text();
        else if (xml.isEndElement()) break;
    }
    return node;
}
QVector<float> floats(const Node* node)
{
    QVector<float> result;
    if (!node) return result;
    for (const auto& number : node->text.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts)) {
        bool okay = false; const float value = number.toFloat(&okay);
        if (!okay || !std::isfinite(value)) return {};
        result.append(value);
    }
    return result;
}
struct Archive {
    cmo3::CCaffArchive archive;
    Node root;
    QMap<QString, const Node*> objects;
    QVector<const Node*> all;
    bool load(const QString& path, QString& error) {
        if (!archive.Load(path, &error)) return false;
        const auto* main = archive.FindByPath("main.xml"); if (!main) { error = "missing main.xml"; return false; }
        QXmlStreamReader xml(main->content); xml.setNamespaceProcessing(false);
        while (!xml.atEnd()) { xml.readNext(); if (xml.isStartElement()) { root = readNode(xml); break; } }
        if (xml.hasError()) { error = xml.errorString(); return false; }
        QVector<const Node*> pending{&root};
        while (!pending.isEmpty()) {
            const auto* node = pending.takeLast(); all.append(node);
            if (node->attributes.contains("xs.id")) {
                const auto id = node->attributes.value("xs.id");
                if (objects.contains(id)) { error = "duplicate object ID"; return false; }
                objects.insert(id, node);
            }
            for (const auto& child : node->children) pending.append(&child);
        }
        return true;
    }
    const Node* resolve(const Node* node) const {
        if (!node) return nullptr;
        return node->attributes.contains("xs.ref") ? objects.value(node->attributes.value("xs.ref"), nullptr) : node;
    }
    const Node* descendant(const Node* node, const QString& tag) const {
        if (!node) return nullptr;
        QVector<const Node*> pending{node};
        while (!pending.isEmpty()) {
            const auto* current = pending.takeLast();
            if (current->tag == tag) return current;
            for (const auto& child : current->children) pending.append(&child);
        }
        return nullptr;
    }
    const Node* source(const QString& id) const {
        for (const auto* node : all) if (node->tag == "CArtMeshSource" && node->attributes.contains("xs.id")) {
            const auto* objectId = descendant(node, "CDrawableId");
            if (objectId && objectId->attributes.value("idstr") == id) return node;
        }
        return nullptr;
    }
    const Node* material(const Node* source) const {
        const auto* extension = resolve(descendant(source, "CTextureInputExtension"));
        const auto* input = extension ? resolve(extension->child("currentTextureInputData")) : nullptr;
        const auto* guid = input ? resolve(input->child("_modelImageGuid")) : nullptr;
        if (!guid) return nullptr;
        for (const auto* node : all) if (node->tag == "CModelImage" && resolve(node->child("guid")) == guid) return node;
        return nullptr;
    }
    QImage image(const Node* resource) const {
        resource = resolve(resource); if (!resource) return {};
        const auto* file = resource->child("imageFileBuf"); if (!file) return {};
        const auto* entry = archive.FindByPath(file->attributes.value("path"));
        return entry ? QImage::fromData(entry->content, "PNG").convertToFormat(QImage::Format_ARGB32) : QImage();
    }
};
bool write(const QString& path, const QByteArray& bytes)
{
    QFile file(path); return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
QByteArray png(const QImage& image)
{
    QByteArray bytes; QBuffer buffer(&bytes);
    return buffer.open(QIODevice::WriteOnly) && image.save(&buffer, "PNG") ? bytes : QByteArray();
}
QRect sourceRect(const CSectionTable& table, const CArt_meshes* mesh, QSize size)
{
    CModelMaterialOverrides materials; CMocMaterialBuilder::Capture(table, materials); const auto values = materials.value(mesh->ids).uvs;
    double left = 1e100, top = left, right = -left, bottom = -left;
    for (int index = 0; index < mesh->vertex_counts; ++index) {
        const double x = double(values[index * 2]) * size.width();
        const double y = double(values[index * 2 + 1]) * size.height();
        left = std::min(left,x); right = std::max(right,x); top = std::min(top,y); bottom = std::max(bottom,y);
    }
    const int x = int(std::floor(left)) - 1, y = int(std::floor(top)) - 1;
    return QRect(x,y,int(std::ceil(right))+1-x,int(std::ceil(bottom))+1-y);
}
int repeat(int value, int size) { const int remainder = value % size; return remainder < 0 ? remainder + size : remainder; }
QVector<double> sample(const QImage& image, double x, double y, bool repeating = false)
{
    x -= .5; y -= .5; const int ix = int(std::floor(x)), iy = int(std::floor(y)); const double dx = x-ix, dy = y-iy;
    QVector<double> values(4,0);
    for (int row = 0; row < 2; ++row) for (int column = 0; column < 2; ++column) {
        const auto p = image.pixel(repeating ? repeat(ix+column,image.width()) : std::clamp(ix+column,0,image.width()-1),
                                   repeating ? repeat(iy+row,image.height()) : std::clamp(iy+row,0,image.height()-1));
        const double weight = (column ? dx : 1-dx)*(row ? dy : 1-dy);
        const int channels[]{qRed(p),qGreen(p),qBlue(p),qAlpha(p)};
        for (int channel = 0; channel < 4; ++channel) values[channel] += channels[channel]*weight;
    }
    return values;
}
}

class Cmo3MaterialTests : public QObject {
    Q_OBJECT
    QByteArray m_bytes;
    QString m_sourcePath;
    QImage m_atlas;
    void checkAffine(const Node* node, double x, double y) {
        QVERIFY(node); for(const QString& key:{QString("m00"),QString("m01"),QString("m02"),QString("m10"),QString("m11"),QString("m12")}) QVERIFY(node->attributes.contains(key)); QCOMPARE(node->attributes.value("m00").toDouble(),1.0); QCOMPARE(node->attributes.value("m11").toDouble(),1.0);
        QCOMPARE(node->attributes.value("m01").toDouble(),0.0); QCOMPARE(node->attributes.value("m10").toDouble(),0.0);
        QCOMPARE(node->attributes.value("m02").toDouble(),x); QCOMPARE(node->attributes.value("m12").toDouble(),y);
    }
private slots:
    void initTestCase() {
        m_sourcePath = QString(TEST_RESOURCES) + "/Mark/Mark.moc3"; QFile file(m_sourcePath); QVERIFY(file.open(QIODevice::ReadOnly)); m_bytes = file.readAll();
        auto reader = CReader::FromBytes(m_bytes); CSectionTable table(reader); QVERIFY(table.IsValid()); QVERIFY(table.art_meshes.size()>1);
        qToLittleEndian(qint32(-1),m_bytes.data()+table.sectionOffsetTable->art_meshes->parent_deformer_indices);
        m_atlas = QImage(32,20,QImage::Format_ARGB32);
        for (int y=0;y<m_atlas.height();++y) for (int x=0;x<m_atlas.width();++x)
            m_atlas.setPixel(x,y,qRgba((x*13+47)%256,(y*17+83)%256,(x*3+y*7+121)%256,(x+y)%3 ? 1+(x*19+y*23)%255 : 0));
    }
    void explicitSnapshotsIgnoreDiskTexturesAndKeepDistinctSparseSlots()
    {
        auto bytes = m_bytes; auto reader = CReader::FromBytes(bytes); CSectionTable table(reader); QVERIFY(table.IsValid());
        for (int i = 0; i < table.art_meshes.size(); ++i)
            qToLittleEndian(qint32(i == 1 ? 2 : 0), bytes.data() + table.sectionOffsetTable->art_meshes->texture_nos + i * 4);
        QImage first(32, 20, QImage::Format_ARGB32), second(first.size(), first.format());
        first.fill(qRgba(192, 41, 13, 77)); second.fill(qRgba(8, 114, 211, 39));
        QTemporaryDir temp; QVERIFY(temp.isValid());
        QVERIFY(write(temp.filePath("Mark.model3.json"), QJsonDocument(QJsonObject{{"FileReferences", QJsonObject{
            {"Moc", "Mark.moc3"}, {"Textures", QJsonArray{"missing.png"}}}}}).toJson()));
        cmo3::CCmo3Builder::Options options; options.mocPath = temp.filePath("Mark.moc3");
        options.mocBytes = bytes; options.outPath = temp.filePath("snapshot.cmo3");
        options.texturePngs = QMap<int, QByteArray>{{0, png(first)}, {2, png(second)}};
        QString error; QVERIFY2(cmo3::CCmo3Builder::Build(options, &error), qPrintable(error));
        Archive output; QVERIFY2(output.load(options.outPath, error), qPrintable(error));
        for (int i = 0; i < table.art_meshes.size(); ++i) {
            const auto* mesh = table.art_meshes[i]; if (!mesh->vertex_counts) continue;
            const auto* material = output.material(output.source(mesh->ids)); QVERIFY(material);
            const auto* input = output.descendant(material, "CLayerInputData"); QVERIFY(input);
            const auto* layer = output.resolve(input->child("layer")); QVERIFY(layer);
            const auto image = output.image(layer->child("imageResource")); QVERIFY(!image.isNull());
            const auto expected = (i == 1 ? second : first).pixel(0, 0);
            for (int y = 0; y < image.height(); ++y) for (int x = 0; x < image.width(); ++x) QCOMPARE(image.pixel(x, y), expected);
        }
        QVERIFY(!QFileInfo::exists(options.mocPath)); QVERIFY(!QFileInfo::exists(temp.filePath("missing.png")));
    }

    void invalidExplicitSnapshotsNeverFallBackOrReplaceOutput_data()
    {
        QTest::addColumn<int>("kind");
        QTest::newRow("empty") << 0; QTest::newRow("missing-used-slot") << 1; QTest::newRow("negative") << 2;
        QTest::newRow("too-large-slot") << 3; QTest::newRow("invalid-png") << 4; QTest::newRow("empty-png") << 5;
        QTest::newRow("unused-empty-png") << 6; QTest::newRow("unused-invalid-png") << 7;
    }
    void invalidExplicitSnapshotsNeverFallBackOrReplaceOutput()
    {
        QFETCH(int, kind); QTemporaryDir temp; QVERIFY(temp.isValid());
        const auto image = png(m_atlas); QVERIFY(write(temp.filePath("atlas.png"), image));
        QVERIFY(write(temp.filePath("Mark.model3.json"), QJsonDocument(QJsonObject{{"FileReferences", QJsonObject{
            {"Moc", "Mark.moc3"}, {"Textures", QJsonArray{"atlas.png"}}}}}).toJson()));
        cmo3::CCmo3Builder::Options options; options.mocPath = temp.filePath("Mark.moc3"); options.mocBytes = m_bytes;
        options.outPath = temp.filePath("keep.cmo3"); QVERIFY(write(options.outPath, "preserved"));
        QMap<int, QByteArray> textures{{0, image}};
        if (kind == 0) textures.clear(); if (kind == 1) { textures.clear(); textures[1] = image; }
        if (kind == 2) textures[-1] = image; if (kind == 3) textures[10000] = image;
        if (kind == 4) textures[0] = "invalid"; if (kind == 5) textures[0].clear();
        if (kind == 6) textures[2].clear(); if (kind == 7) textures[2] = "invalid";
        options.texturePngs = textures; QString error;
        QVERIFY(!cmo3::CCmo3Builder::Build(options, &error)); QVERIFY(!error.isEmpty());
        QFile output(options.outPath); QVERIFY(output.open(QIODevice::ReadOnly)); QCOMPARE(output.readAll(), QByteArray("preserved"));
    }

    void imageEncodingFailureKeepsArchiveAndPath() {
        cmo3::CCaffArchive archive; archive.AddEntry("keep.bin", "old");
        cmo3::CXsXmlWriter xml; xml.BeginDocument({}, {}, "3.0"); xml.BeginShared();
        cmo3::CCmo3Entities entities(xml, archive); QString path="keep.png", error;
        const auto before=xml.Result();
        QVERIFY(entities.WriteImageResource({}, &path, &error).isEmpty()); QVERIFY(!error.isEmpty());
        QCOMPARE(path,QString("keep.png")); QCOMPARE(archive.Entries().size(),1); QCOMPARE(archive.FindByPath("keep.bin")->content,QByteArray("old")); QCOMPARE(xml.Result(),before);
        error.clear(); const auto id=entities.WriteImageResource(m_atlas,&path,&error); QVERIFY(!id.isEmpty()); QVERIFY(error.isEmpty());
        const auto* entry=archive.FindByPath(path); QVERIFY(entry); QCOMPARE(QImage::fromData(entry->content,"PNG").convertToFormat(QImage::Format_ARGB32),m_atlas);
        QVector<cmo3::SLayerData> layers(1); layers[0].name="invalid";
        const auto failed=entities.WriteLayeredImage("invalid",32,20,layers,&error); QVERIFY(failed.layeredImage.isEmpty()); QVERIFY(failed.rootGroup.isEmpty()); QVERIFY(!error.isEmpty());
        QCOMPARE(archive.Entries().size(),2);
        xml.EndShared(); xml.EndDocument(); QVERIFY(!xml.HasError());
    }
    void unsigned16TriangleIndicesRoundTrip()
    {
        DeformerSnapshotData::Fixture source; const auto& offsets = *source.table.sectionOffsetTable;
        const auto positions = source.bytes.mid(offsets.keyform_positions->xys, 20 * 4);
        source.column(offsets.art_meshes->vertex_counts, 0, 65536);
        source.appendColumn(0x178, QVector<int>(65536 * 2, 0)); source.count(15, 65536 * 2);
        source.appendColumn(0x15c, QVector<int>(20 + 65536 * 4, 0)); source.count(10, 20 + 65536 * 4);
        const int positionStart = DeformerSnapshotData::integer(source.bytes, 0x15c);
        source.bytes.replace(positionStart, positions.size(), positions);
        source.column(offsets.art_mesh_keyforms->keyform_position_sources_begin_indices, 0, 20);
        source.column(offsets.art_mesh_keyforms->keyform_position_sources_begin_indices, 1, 20 + 65536 * 2);
        const int uvStart = DeformerSnapshotData::integer(source.bytes, 0x178);
        DeformerSnapshotData::real(source.bytes, uvStart + 65534 * 8, 1.0f);
        DeformerSnapshotData::real(source.bytes, uvStart + 65535 * 8 + 4, 1.0f);
        const QVector<quint16> expected{32768, 65534, 65535};
        for (int index = 0; index < 3; ++index) qToLittleEndian(expected[index], source.bytes.data() + offsets.position_indices->indices + index * 2);
        QTemporaryDir temp; QVERIFY(temp.isValid()); cmo3::CCmo3Builder::Options options;
        options.mocPath = temp.filePath("unsigned.moc3"); options.mocBytes = source.bytes;
        options.outPath = temp.filePath("unsigned.cmo3"); options.texturePngs = QMap<int, QByteArray>{{0, png(m_atlas)}};
        QString error; QVERIFY2(cmo3::CCmo3Builder::Build(options, &error), qPrintable(error));
        Archive output; QVERIFY2(output.load(options.outPath, error), qPrintable(error));
        const auto* mesh = output.source("Mesh"); QVERIFY(mesh);
        const QVector<float> indices{32768, 65534, 65535}; QCOMPARE(floats(mesh->child("indices")), indices);
        QCOMPARE(floats(mesh->child("uvs")).size(), 65536 * 2);
    }
    void repeatedMaterialEncodingBudgetPreservesExistingTarget() {
        auto bytes=m_bytes; auto reader=CReader::FromBytes(bytes); CSectionTable table(reader); QVERIFY(table.IsValid());
        constexpr int size=3072;
        QVERIFY(qint64(table.art_meshes.size())*(size+2)*(size+2)>256LL*1024*1024);
        for(int i=0;i<table.uvs->uvs.size();++i) {
            const float value=(i/2)%2 ? 1.0f : 0.0f; quint32 bits; std::memcpy(&bits,&value,4);
            qToLittleEndian(bits,bytes.data()+table.sectionOffsetTable->uvs->uvs+i*4);
        }
        QTemporaryDir temp; QVERIFY(temp.isValid()); const auto moc=temp.filePath("Mark.moc3"); QVERIFY(write(moc,bytes));
        QImage atlas(size,size,QImage::Format_ARGB32); atlas.fill(qRgba(93,121,217,127)); QVERIFY(atlas.save(temp.filePath("atlas.png"),"PNG"));
        QVERIFY(write(temp.filePath("Mark.model3.json"),QJsonDocument(QJsonObject{{"FileReferences",QJsonObject{{"Moc","Mark.moc3"},{"Textures",QJsonArray{"atlas.png"}}}}}).toJson()));
        cmo3::CCmo3Builder::Options options; options.mocPath=moc; options.outPath=temp.filePath("keep.cmo3"); QVERIFY(write(options.outPath,"original target"));
        QString error; QVERIFY(!cmo3::CCmo3Builder::Build(options,&error)); QVERIFY2(error.contains(QStringLiteral("编码总像素")),qPrintable(error));
        QFile preserved(options.outPath); QVERIFY(preserved.open(QIODevice::ReadOnly)); QCOMPARE(preserved.readAll(),QByteArray("original target"));
    }
    void materialGraphPreservesPixelsAndFormCoordinates_data() {
        QTest::addColumn<bool>("editForm"); QTest::addColumn<int>("uvMode");
        QTest::newRow("baseline") << false << 0; QTest::newRow("edited-local-form") << true << 0;
        QTest::newRow("outside-repeat") << false << 1; QTest::newRow("odd-shared-uv") << false << 2;
    }
    void materialGraphPreservesPixelsAndFormCoordinates() {
        QFETCH(bool, editForm); QFETCH(int, uvMode); auto bytes = m_bytes;
        auto sourceReader = CReader::FromBytes(bytes); CSectionTable sourceTable(sourceReader); QVERIFY(sourceTable.IsValid());
        if (uvMode == 1) for (int index = 0; index < sourceTable.uvs->uvs.size(); ++index) {
            const float value = sourceTable.uvs->uvs[index] + (index % 2 ? 1.0f : -2.0f); quint32 bits; std::memcpy(&bits, &value, 4);
            qToLittleEndian(bits, bytes.data() + sourceTable.sectionOffsetTable->uvs->uvs + index * 4);
        }
        if (uvMode == 2) qToLittleEndian(qint32(sourceTable.art_meshes[0]->uv_sources_begin_indices + 1),
            bytes.data() + sourceTable.sectionOffsetTable->art_meshes->uv_sources_begin_indices + 4);
        auto reader = CReader::FromBytes(bytes); CSectionTable table(reader); QVERIFY(table.IsValid());
        CModelMaterialOverrides materials; QVERIFY(CMocMaterialBuilder::Capture(table, materials));
        QTemporaryDir temp; QVERIFY(temp.isValid()); const auto moc=temp.filePath("Mark.moc3"); QVERIFY(write(moc,bytes)); QVERIFY(m_atlas.save(temp.filePath("atlas.png"),"PNG"));
        int maximumTexture=0, parented=-1;
        for(int i=0;i<table.art_meshes.size();++i) {maximumTexture=std::max(maximumTexture,table.art_meshes[i]->texture_nos); if(table.art_meshes[i]->parent_deformer_indices>=0) parented=i;}
        QVERIFY(parented>=0); QJsonArray textures; for(int i=0;i<=maximumTexture;++i) textures.append("atlas.png");
        QVERIFY(write(temp.filePath("Mark.model3.json"),QJsonDocument(QJsonObject{{"FileReferences",QJsonObject{{"Moc","Mark.moc3"},{"Textures",textures}}}}).toJson()));
        cmo3::CCmo3Builder::Options options; options.mocPath=moc; options.outPath=temp.filePath("material.cmo3");
        if(editForm) {
            const auto* mesh=table.art_meshes[parented]; const auto* form=table.art_mesh_keyforms[mesh->keyform_sources_begin_indices]; QVector<float> values;
            for(int i=0;i<mesh->vertex_counts*2;++i) values.append(table.keyform_positions->xys[form->keyform_position_sources_begin_indices+i]);
            values[0]+=.125f; options.drawableKeyformPositionOverrides.insert(mesh->ids,{values});
        }
        QString error; QVERIFY2(cmo3::CCmo3Builder::Build(options,&error),qPrintable(error)); Archive output; QVERIFY2(output.load(options.outPath,error),qPrintable(error));
        QSize sourceCanvas=m_atlas.size();
        for(const auto* mesh:table.art_meshes) if(mesh->vertex_counts) sourceCanvas=sourceCanvas.expandedTo(sourceRect(table,mesh,m_atlas.size()).size());
        const Node* imageDocument=nullptr; const Node* modelCanvas=nullptr;
        for(const auto* node:output.all) {
            if(node->tag=="CLayeredImage" && node->child("width") && node->child("height")) imageDocument=node;
            if(node->tag=="CImageCanvas" && node->field=="canvas") modelCanvas=node;
        }
        QVERIFY(imageDocument); QCOMPARE(imageDocument->child("width")->text.toInt(),sourceCanvas.width()); QCOMPARE(imageDocument->child("height")->text.toInt(),sourceCanvas.height());
        QVERIFY(modelCanvas); QVERIFY(modelCanvas->child("pixelWidth")); QVERIFY(modelCanvas->child("pixelHeight"));
        QCOMPARE(modelCanvas->child("pixelWidth")->text.toFloat(),table.canvas_info->width); QCOMPARE(modelCanvas->child("pixelHeight")->text.toFloat(),table.canvas_info->height);
        QVERIFY(sourceCanvas!=QSize(qRound(table.canvas_info->width),qRound(table.canvas_info->height)));
        bool sawRoot=false,sawParent=false; int checked=0;
        for(const auto* mesh:table.art_meshes) {
            if(mesh->vertex_counts==0) continue;
            const auto* source=output.source(mesh->ids); QVERIFY(source); const auto* material=output.material(source); QVERIFY(material);
            const auto* extension=output.resolve(output.descendant(source,"CTextureInputExtension")); QVERIFY(extension);
            const auto* inputs=extension->child("_textureInputs"); QVERIFY(inputs); QCOMPARE(inputs->children.size(),1);
            QCOMPARE(output.resolve(&inputs->children.first()),output.resolve(extension->child("currentTextureInputData")));

            const auto* input=output.descendant(material,"CLayerInputData"); QVERIFY(input); const auto* layer=output.resolve(input->child("layer")); QVERIFY(layer);
            const auto* bounds=layer->child("boundsOnImageDoc"); QVERIFY(bounds); QVERIFY(bounds->child("x")); QVERIFY(bounds->child("y")); QVERIFY(bounds->child("width")); QVERIFY(bounds->child("height"));
            const QPoint origin(bounds->child("x")->text.toInt(),bounds->child("y")->text.toInt());
            const auto* resource=output.resolve(layer->child("imageResource")); QVERIFY(resource); const auto image=output.image(resource); QVERIFY(!image.isNull());
            QCOMPARE(image.width(),bounds->child("width")->text.toInt()); QCOMPARE(image.height(),bounds->child("height")->text.toInt());
            QCOMPARE(image.width(),resource->attributes.value("width").toInt()); QCOMPARE(image.height(),resource->attributes.value("height").toInt());
            QCOMPARE(output.resolve(material->child("_filteredImage")),resource); checkAffine(input->child("affine"),0,0); checkAffine(material->child("_materialLocalToCanvasTransform"),origin.x(),origin.y());
            const auto* cache=material->child("cachedImageManager"); QVERIFY(cache); QVERIFY(cache->child("defaultCacheType")); QVERIFY(cache->child("requiredMipmapLevel"));
            QCOMPARE(cache->child("defaultCacheType")->attributes.value("v"),QString("SCALE_1")); QCOMPARE(cache->child("requiredMipmapLevel")->text.toInt(),64);
            QCOMPARE(output.resolve(cache->child("rawImage")),resource);
            const auto crop=sourceRect(table,mesh,m_atlas.size()); QCOMPARE(image.size(),crop.size());
            for(int y=0;y<image.height();++y) for(int x=0;x<image.width();++x)
                QCOMPARE(image.pixel(x,y),m_atlas.pixel(repeat(crop.x()+x,m_atlas.width()),repeat(crop.y()+y,m_atlas.height())));
            const auto authorUvs = materials.value(mesh->ids).uvs;
            const auto positions=floats(source->child("positions")), uvs=floats(source->child("uvs"));
            QCOMPARE(positions.size(),mesh->vertex_counts*2); QCOMPARE(uvs.size(),positions.size());
            const int envelope[]{(image.width()+63)/64*64,(image.height()+63)/64*64};
            for(int v=0;v<mesh->vertex_counts;++v) for(int axis=0;axis<2;++axis) {
                const double original=double(authorUvs[v*2+axis])*(axis?m_atlas.height():m_atlas.width());
                const int offset=axis?origin.y():origin.x(), cropOffset=axis?crop.y():crop.x();
                QCOMPARE(positions[v*2+axis],float(original-cropOffset+offset));
                QCOMPARE(uvs[v*2+axis],float((double(positions[v*2+axis])-offset)/envelope[axis]));
            }
            const auto* forms=source->child("keyforms"); QVERIFY(forms); QCOMPARE(forms->children.size(),mesh->keyform_sources_counts);
            const bool root=mesh->parent_deformer_indices<0; sawRoot|=root; sawParent|=!root;
            for(int k=0;k<mesh->keyform_sources_counts;++k) {
                const auto* old=table.art_mesh_keyforms[mesh->keyform_sources_begin_indices+k]; const auto actual=floats(forms->children[k].child("positions"));
                QCOMPARE(actual.size(),mesh->vertex_counts*2); QVERIFY(!forms->children[k].child("uvs"));
                for(int at=0;at<actual.size();++at) {
                    float value=table.keyform_positions->xys[old->keyform_position_sources_begin_indices+at];
                    if(editForm && mesh==table.art_meshes[parented] && k==0 && at==0) value+=.125f;
                    const float expected=root ? float(at%2 ? double(table.canvas_info->origin_y)+double(value)*table.canvas_info->pixels_per_unit : double(table.canvas_info->origin_x)+double(value)*table.canvas_info->pixels_per_unit) : value;
                    QCOMPARE(actual[at],expected);
                }
            }
            for(int tri=0;tri<mesh->position_index_sources_counts;tri+=3) {
                double oldX=0,oldY=0,newX=0,newY=0;
                for(int side=0;side<3;++side) {
                    const int vertex=static_cast<quint16>(table.position_indices->indices[mesh->position_index_sources_begin_indices+tri+side]);
                    oldX+=double(authorUvs[vertex*2])*m_atlas.width()/3;
                    oldY+=double(authorUvs[vertex*2+1])*m_atlas.height()/3;
                    newX+=double(uvs[vertex*2])*envelope[0]/3; newY+=double(uvs[vertex*2+1])*envelope[1]/3;
                }
                const auto expected=sample(m_atlas,oldX,oldY,true), actual=sample(image,newX,newY);
                for(int channel=0;channel<4;++channel) QVERIFY(std::abs(expected[channel]-actual[channel])<.002);
            }
            ++checked;
        }
        QVERIFY(sawRoot); QVERIFY(sawParent); QCOMPARE(checked,table.art_meshes.size());
    }
};
QTEST_GUILESS_MAIN(Cmo3MaterialTests)
#include "cmo3_material_tests.moc"
