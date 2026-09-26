#include "cmo3/ccaffarchive.h"
#include "cmo3/ccmo3builder.h"
#include "live2dstruct/ccanvasgeometry.h"
#include "live2dstruct/csectiontable.h"
#include "real_core_model_fixture.h"
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QXmlStreamReader>
#include <QtEndian>

using namespace live2dstruct;
namespace {
void writeInt(QByteArray& bytes, int offset, int value) { qToLittleEndian(qint32(value), bytes.data() + offset); }
void writeFloat(QByteArray& bytes, int offset, float value)
{
    quint32 bits; std::memcpy(&bits, &value, 4); qToLittleEndian(bits, bytes.data() + offset);
}
bool writeFile(const QString& path, const QByteArray& bytes)
{
    QFile file(path); return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
struct Node { QString tag, name, text; QMap<QString, QString> attributes; QVector<int> children; };
class Xml
{
public:
    QVector<Node> nodes;
    bool parse(const QByteArray& bytes)
    {
        QXmlStreamReader xml(bytes); xml.setNamespaceProcessing(false); QVector<int> stack;
        while (!xml.atEnd()) {
            xml.readNext();
            if (xml.isStartElement()) {
                Node node; node.tag = xml.name().toString();
                for (const auto& a : xml.attributes()) node.attributes.insert(a.qualifiedName().toString(), a.value().toString());
                node.name = node.attributes.value("xs.n"); const int index = nodes.size(); nodes.append(node);
                if (!stack.isEmpty()) nodes[stack.last()].children.append(index); stack.append(index);
            } else if (xml.isCharacters() && !stack.isEmpty()) nodes[stack.last()].text += xml.text();
            else if (xml.isEndElement()) stack.removeLast();
        }
        return !xml.hasError();
    }
    int field(int parent, const QString& name) const
    {
        if (parent < 0) return -1;
        for (int child : nodes[parent].children) if (nodes[child].name == name) return child;
        return -1;
    }
    int source(const QString& tag, const QString& id) const
    {
        for (int index = 0; index < nodes.size(); ++index) {
            if (nodes[index].tag != tag || nodes[index].attributes.value("xs.id").isEmpty()) continue;
            const int owner = field(field(index, "super"), "id");
            if (owner >= 0 && nodes[owner].attributes.value("idstr") == id) return index;
        }
        return -1;
    }
    QVector<float> positions(int parent) const
    {
        QVector<float> result; const int values = field(parent, "positions"); if (values < 0) return result;
        for (const auto& text : nodes[values].text.simplified().split(' ', Qt::SkipEmptyParts)) result.append(text.toFloat());
        return result;
    }
};
int drawable(csmModel* model, const QString& id)
{
    for (int i = 0; i < csmGetDrawableCount(model); ++i) if (QString::fromUtf8(csmGetDrawableIds(model)[i]) == id) return i;
    return -1;
}
}

class Cmo3CanvasCoreTests : public QObject
{
    Q_OBJECT
private slots:
    void rotationUnitsFollowTheEffectiveAncestry_data()
    {
        QTest::addColumn<int>("kind"); QTest::addColumn<float>("parentScale"); QTest::addColumn<float>("childScale");
        QTest::newRow("root") << 0 << 3.0f << 2.0f;
        QTest::newRow("warp-first") << 1 << 3.0f << 2.0f;
        QTest::newRow("nested-direct") << 2 << 3.0f << 2.0f;
        QTest::newRow("nested-through-warp") << 3 << 3.0f << 2.0f;
        QTest::newRow("override-detach") << 4 << 3.0f << 2.0f;
        QTest::newRow("override-nest-through-warp") << 5 << 3.0f << 2.0f;
        QTest::newRow("negative-ancestor") << 3 << -3.0f << 2.0f;
        QTest::newRow("negative-first-scale") << 1 << 3.0f << -2.0f;
    }
    void rotationUnitsFollowTheEffectiveAncestry()
    {
        QFETCH(int, kind); QFETCH(float, parentScale); QFETCH(float, childScale);
        const QByteArray original = read(QString(TEST_RESOURCES) + "/Mark/Mark.moc3");
        auto reader = CReader::FromBytes(original); CSectionTable table(reader); QVERIFY(table.IsValid());
        QVector<int> rotations; int warpIndex = -1;
        for (int i = 0; i < table.deformers.size(); ++i) {
            if (table.deformers[i]->types == 1) rotations.append(i);
            else if (warpIndex < 0) warpIndex = i;
        }
        QVERIFY(rotations.size() >= 2); QVERIFY(warpIndex >= 0);
        const int a = rotations[0];
        int b = -1;
        for (int index : rotations) if (index > warpIndex && index > a) { b = index; break; }
        QVERIFY(a < warpIndex); QVERIFY(b > warpIndex);
        const int warpParent = kind == 3 || kind == 5 ? a : -1;
        const int sourceParent = kind == 1 || kind == 3 ? warpIndex : kind == 2 || kind == 4 ? a : -1;
        const int effectiveParent = kind == 4 ? -1 : kind == 5 ? warpIndex : sourceParent;
        const bool inherited = effectiveParent == a || (effectiveParent == warpIndex && warpParent == a);
        QByteArray bytes = original;
        const auto setParent = [&](QByteArray& data, int child, int parent) { writeInt(data, table.sectionOffsetTable->deformers->parent_deformer_indices + child * 4, parent); };
        setParent(bytes, a, -1); setParent(bytes, warpIndex, warpParent); setParent(bytes, b, sourceParent);
        writeInt(bytes, table.sectionOffsetTable->art_meshes->parent_deformer_indices, b);
        const auto* mesh = table.art_meshes[0]; QVERIFY(mesh->vertex_counts >= 2);
        for (int form = 0; form < mesh->keyform_sources_counts; ++form) {
            const int start = table.art_mesh_keyforms[mesh->keyform_sources_begin_indices + form]->keyform_position_sources_begin_indices;
            for (int coordinate = 0; coordinate < 4; ++coordinate)
                writeFloat(bytes, table.sectionOffsetTable->keyform_positions->xys + (start + coordinate) * 4, coordinate == 2 ? 1 : 0);
        }
        for (int index : {a, b}) {
            const int specific = table.deformers[index]->specific_sources_indices; const auto* rotation = table.rotation_deformers[specific];
            writeFloat(bytes, table.sectionOffsetTable->rotation_deformers->base_angles + specific * 4, 0);
            for (int form = 0; form < rotation->keyform_sources_counts; ++form) {
                const int row = rotation->keyform_sources_begin_indices + form;
                writeFloat(bytes, table.sectionOffsetTable->rotation_deformer_keyforms->origin_x + row * 4, index == a ? .1f : .25f);
                writeFloat(bytes, table.sectionOffsetTable->rotation_deformer_keyforms->origin_y + row * 4, index == a ? -.2f : .4f);
                writeFloat(bytes, table.sectionOffsetTable->rotation_deformer_keyforms->scales + row * 4, index == a ? parentScale : childScale);
                writeFloat(bytes, table.sectionOffsetTable->rotation_deformer_keyforms->angles + row * 4, 0);
                writeInt(bytes, table.sectionOffsetTable->rotation_deformer_keyforms->is_reflect_x + row * 4, 0);
                writeInt(bytes, table.sectionOffsetTable->rotation_deformer_keyforms->is_reflect_y + row * 4, 0);
            }
        }
        const auto* warp = table.warp_deformers[table.deformers[warpIndex]->specific_sources_indices];
        QVERIFY(warp->columns > 0 && warp->rows > 0); QCOMPARE(warp->vertex_counts, (warp->columns + 1) * (warp->rows + 1));
        for (int form = 0; form < warp->keyform_sources_counts; ++form) {
            const int start = table.warp_deformer_keyforms[warp->keyform_sources_begin_indices + form]->keyform_position_sources_begin_indices;
            for (int y = 0; y <= warp->rows; ++y) for (int x = 0; x <= warp->columns; ++x) {
                const int vertex = y * (warp->columns + 1) + x;
                writeFloat(bytes, table.sectionOffsetTable->keyform_positions->xys + (start + vertex * 2) * 4, float(x) / warp->columns);
                writeFloat(bytes, table.sectionOffsetTable->keyform_positions->xys + (start + vertex * 2 + 1) * 4, float(y) / warp->rows);
            }
        }
        QByteArray effective = bytes; setParent(effective, b, effectiveParent); CoreModel core; QVERIFY(core.load(effective));
        core.pose(QVector<float>(csmGetParameterDefaultValues(core.value), csmGetParameterDefaultValues(core.value) + csmGetParameterCount(core.value)));
        QTemporaryDir temporary; QVERIFY(temporary.isValid()); const auto path = temporary.filePath("Mark.moc3"); QVERIFY(writeFile(path, bytes));
        QVERIFY(writeFile(temporary.filePath("Mark.model3.json"), QJsonDocument(QJsonObject{{"FileReferences", QJsonObject{{"Moc", "Mark.moc3"}, {"Textures", QJsonArray{"atlas.png"}}}}}).toJson()));
        QImage atlas(32, 24, QImage::Format_ARGB32); atlas.fill(Qt::white); QVERIFY(atlas.save(temporary.filePath("atlas.png")));
        cmo3::CCmo3Builder::Options options; options.mocPath = path; options.outPath = temporary.filePath("rotation.cmo3");
        if (effectiveParent != sourceParent) options.deformerParentOverrides.insert(table.deformers[b]->ids, effectiveParent);
        QString error; QVERIFY2(cmo3::CCmo3Builder::Build(options, &error), qPrintable(error));
        cmo3::CCaffArchive archive; QVERIFY(archive.Load(options.outPath, &error)); Xml xml; QVERIFY(xml.parse(archive.FindByPath("main.xml")->content));
        const auto form = [&](const QString& tag, const QString& id) { const int owner = xml.source(tag, id); return owner < 0 ? -1 : xml.nodes[xml.field(owner, "keyforms")].children.first(); };
        const int first = form("CRotationDeformerSource", table.deformers[a]->ids), second = form("CRotationDeformerSource", table.deformers[b]->ids);
        const int grid = form("CWarpDeformerSource", table.deformers[warpIndex]->ids); QVERIFY(first >= 0 && second >= 0 && grid >= 0);
        const auto originOf = [&](int index) { return QPointF(xml.nodes[index].attributes.value("originX").toFloat(), xml.nodes[index].attributes.value("originY").toFloat()); };
        const auto scaleOf = [&](int index) { return xml.nodes[index].attributes.value("scale").toFloat(); };
        const auto gridValues = xml.positions(grid);
        const auto warpMap = [&](QPointF point) {
            const QPointF p0(gridValues[0], gridValues[1]);
            const QPointF px(gridValues[warp->columns * 2], gridValues[warp->columns * 2 + 1]);
            const int lastRow = warp->rows * (warp->columns + 1) * 2;
            const QPointF py(gridValues[lastRow], gridValues[lastRow + 1]);
            const QPointF local = p0 + (px - p0) * point.x() + (py - p0) * point.y();
            return warpParent == a ? originOf(first) + local * scaleOf(first) : local;
        };
        const auto parentMap = [&](QPointF point) { return effectiveParent == a ? originOf(first) + point * scaleOf(first) : warpMap(point); };
        const QPointF originB = effectiveParent < 0 ? originOf(second) : parentMap(originOf(second));
        double angle = 0;
        if (effectiveParent >= 0) {
            const QPointF direction = parentMap(originOf(second) + QPointF(0, -.1)) - originB;
            angle = std::atan2(direction.y(), direction.x()) + 1.5707963267948966192;
        }
        const double totalScale = scaleOf(second) * (inherited ? scaleOf(first) : 1.0);
        csmVector2 canvas{}, origin{}; float ppu; csmReadCanvasInfo(core.value, &canvas, &origin, &ppu);
        QCOMPARE(scaleOf(first), parentScale * ppu); QCOMPARE(scaleOf(second), inherited ? childScale : childScale * ppu);
        const int target = drawable(core.value, mesh->ids); QVERIFY(target >= 0);
        for (int vertex = 0; vertex < 2; ++vertex) {
            const QPointF exported = originB + QPointF(std::cos(angle), std::sin(angle)) * (totalScale * vertex);
            const auto point = csmGetDrawableVertexPositions(core.value)[target][vertex];
            const QPointF expected(double(origin.X) + double(point.X) * ppu, double(origin.Y) - double(point.Y) * ppu);
            QVERIFY2(std::hypot(exported.x() - expected.x(), exported.y() - expected.y()) < .005,
                     qPrintable(QString("xml(%1,%2) core(%3,%4)").arg(exported.x()).arg(exported.y()).arg(expected.x()).arg(expected.y())));
        }
        QCOMPARE(read(path), bytes); QVERIFY(core.intact());
    }
    void rootAuthoringCoordinatesMatchCorePixels_data()
    {
        QTest::addColumn<int>("canvas"); QTest::newRow("original-origin") << 0;
        QTest::newRow("off-center-origin") << 1; QTest::newRow("negative-origin") << 2;
    }
    void rootAuthoringCoordinatesMatchCorePixels()
    {
        QFETCH(int, canvas);
        const QByteArray original = read(QString(TEST_RESOURCES) + "/Mark/Mark.moc3"); QVERIFY(!original.isEmpty());
        auto reader = CReader::FromBytes(original); CSectionTable table(reader); QVERIFY(table.IsValid());
        QVERIFY(table.art_meshes.size() >= 3); QVERIFY(table.glue.isEmpty());
        int warpIndex = -1, rotationIndex = -1;
        for (int i = 0; i < table.deformers.size(); ++i) {
            if (table.deformers[i]->types == 0 && warpIndex < 0) warpIndex = i;
            if (table.deformers[i]->types == 1 && rotationIndex < 0) rotationIndex = i;
        }
        QVERIFY(warpIndex >= 0); QVERIFY(rotationIndex >= 0); QByteArray bytes = original;
        const auto* warpOwner = table.deformers[warpIndex]; const auto* rotationOwner = table.deformers[rotationIndex];
        const auto* warp = table.warp_deformers[warpOwner->specific_sources_indices];
        const auto* rotation = table.rotation_deformers[rotationOwner->specific_sources_indices];
        for (int index : {warpIndex, rotationIndex}) writeInt(bytes, table.sectionOffsetTable->deformers->parent_deformer_indices + index * 4, -1);
        const int parents[]{-1, warpIndex, rotationIndex};
        for (int mesh = 0; mesh < 3; ++mesh) {
            const auto* source = table.art_meshes[mesh]; QVERIFY(source->vertex_counts > 0);
            writeInt(bytes, table.sectionOffsetTable->art_meshes->parent_deformer_indices + mesh * 4, parents[mesh]);
            for (int form = 0; form < source->keyform_sources_counts; ++form) {
                const int start = table.art_mesh_keyforms[source->keyform_sources_begin_indices + form]->keyform_position_sources_begin_indices;
                writeFloat(bytes, table.sectionOffsetTable->keyform_positions->xys + start * 4, mesh == 0 ? .25f : 0);
                writeFloat(bytes, table.sectionOffsetTable->keyform_positions->xys + (start + 1) * 4, mesh == 0 ? -.125f : 0);
                if (mesh == 2) {
                    QVERIFY(source->vertex_counts >= 2);
                    writeFloat(bytes, table.sectionOffsetTable->keyform_positions->xys + (start + 2) * 4, 1);
                    writeFloat(bytes, table.sectionOffsetTable->keyform_positions->xys + (start + 3) * 4, 0);
                }
            }
        }
        for (int form = 0; form < warp->keyform_sources_counts; ++form) {
            const auto* key = table.warp_deformer_keyforms[warp->keyform_sources_begin_indices + form];
            writeFloat(bytes, table.sectionOffsetTable->keyform_positions->xys + key->keyform_position_sources_begin_indices * 4, .2f);
            writeFloat(bytes, table.sectionOffsetTable->keyform_positions->xys + (key->keyform_position_sources_begin_indices + 1) * 4, -.35f);
        }
        for (int form = 0; form < rotation->keyform_sources_counts; ++form) {
            const int row = rotation->keyform_sources_begin_indices + form;
            writeFloat(bytes, table.sectionOffsetTable->rotation_deformer_keyforms->origin_x + row * 4, .3f);
            writeFloat(bytes, table.sectionOffsetTable->rotation_deformer_keyforms->origin_y + row * 4, -.4f);
            writeFloat(bytes, table.sectionOffsetTable->rotation_deformer_keyforms->angles + row * 4, 0);
            writeFloat(bytes, table.sectionOffsetTable->rotation_deformer_keyforms->scales + row * 4, 1);
            writeInt(bytes, table.sectionOffsetTable->rotation_deformer_keyforms->is_reflect_x + row * 4, 0);
            writeInt(bytes, table.sectionOffsetTable->rotation_deformer_keyforms->is_reflect_y + row * 4, 0);
        }
        writeFloat(bytes, table.sectionOffsetTable->rotation_deformers->base_angles + rotationOwner->specific_sources_indices * 4, 0);
        if (canvas) {
            const int offset = qFromLittleEndian<qint32>(bytes.constData() + 0x44);
            const float fields[]{10, canvas == 1 ? 30.0f : -20.0f, canvas == 1 ? 40.0f : -15.0f, 200, 300};
            for (int i = 0; i < 5; ++i) writeFloat(bytes, offset + i * 4, fields[i]);
        }
        CoreModel core; QVERIFY2(core.load(bytes), "The controlled root fixture must be accepted by real Core");
        core.pose(QVector<float>(csmGetParameterDefaultValues(core.value), csmGetParameterDefaultValues(core.value) + csmGetParameterCount(core.value)));
        csmVector2 size{}, origin{}; float ppu = 0; csmReadCanvasInfo(core.value, &size, &origin, &ppu);
        QVector<QPointF> expected;
        for (int mesh = 0; mesh < 3; ++mesh) {
            const int index = drawable(core.value, table.art_meshes[mesh]->ids); QVERIFY(index >= 0);
            const auto point = csmGetDrawableVertexPositions(core.value)[index][0];
            expected.append(QPointF(double(origin.X) + double(point.X) * ppu, double(origin.Y) - double(point.Y) * ppu));
        }
        QTemporaryDir temporary; QVERIFY(temporary.isValid()); const auto modelPath = temporary.filePath("Mark.moc3");
        QVERIFY(writeFile(modelPath, bytes));
        QVERIFY(writeFile(temporary.filePath("Mark.model3.json"), QJsonDocument(QJsonObject{{"FileReferences", QJsonObject{{"Moc", "Mark.moc3"}, {"Textures", QJsonArray{"atlas.png"}}}}}).toJson()));
        QImage atlas(32, 24, QImage::Format_ARGB32); atlas.fill(qRgba(113, 79, 201, 255)); QVERIFY(atlas.save(temporary.filePath("atlas.png")));
        cmo3::CCmo3Builder::Options options; options.mocPath = modelPath; options.outPath = temporary.filePath("roots.cmo3"); QString error;
        QVERIFY2(cmo3::CCmo3Builder::Build(options, &error), qPrintable(error));
        cmo3::CCaffArchive archive; QVERIFY(archive.Load(options.outPath, &error)); const auto* xmlFile = archive.FindByPath("main.xml"); QVERIFY(xmlFile);
        Xml xml; QVERIFY(xml.parse(xmlFile->content));
        const QString tags[]{"CArtMeshSource", "CWarpDeformerSource", "CRotationDeformerSource"};
        const QString ids[]{table.art_meshes[0]->ids, warpOwner->ids, rotationOwner->ids};
        for (int owner = 0; owner < 3; ++owner) {
            const int source = xml.source(tags[owner], ids[owner]); QVERIFY2(source >= 0, qPrintable(ids[owner]));
            const int forms = xml.field(source, "keyforms"); QVERIFY(forms >= 0); QVERIFY(!xml.nodes[forms].children.isEmpty());
            for (int form : xml.nodes[forms].children) {
                QPointF actual;
                if (owner == 2) actual = QPointF(xml.nodes[form].attributes.value("originX").toFloat(), xml.nodes[form].attributes.value("originY").toFloat());
                else { const auto values = xml.positions(form); QVERIFY(values.size() >= 2); actual = QPointF(values[0], values[1]); }
                QVERIFY2(std::abs(actual.x() - expected[owner].x()) <= .001 && std::abs(actual.y() - expected[owner].y()) <= .001,
                    qPrintable(QString("%1: xml(%2,%3) corePixels(%4,%5)").arg(ids[owner]).arg(actual.x(), 0, 'g', 12).arg(actual.y(), 0, 'g', 12).arg(expected[owner].x(), 0, 'g', 12).arg(expected[owner].y(), 0, 'g', 12)));
            }
        }
        for (int mesh : {1, 2}) {
            const int source = xml.source("CArtMeshSource", table.art_meshes[mesh]->ids); QVERIFY(source >= 0);
            for (int form : xml.nodes[xml.field(source, "keyforms")].children) {
                const auto local = xml.positions(form); QVERIFY(local.size() >= 2); QCOMPARE(local[0], 0.0f); QCOMPARE(local[1], 0.0f);
                if (mesh == 2) {
                    QVERIFY(local.size() >= 4); QCOMPARE(local[2], 1.0f); QCOMPARE(local[3], 0.0f);
                    const auto point = csmGetDrawableVertexPositions(core.value)[drawable(core.value, table.art_meshes[2]->ids)][1];
                    const int rotationSource = xml.source("CRotationDeformerSource", rotationOwner->ids);
                    const int rotationForm = xml.nodes[xml.field(rotationSource, "keyforms")].children.first();
                    const float exportedScale = xml.nodes[rotationForm].attributes.value("scale").toFloat();
                    QCOMPARE(exportedScale, ppu);
                    QVERIFY(std::abs(double(origin.X) + double(point.X) * ppu - (expected[2].x() + exportedScale * local[2])) <= .001);
                    QVERIFY(std::abs(double(origin.Y) - double(point.Y) * ppu - expected[2].y()) <= .001);
                }
            }
        }
        QCOMPARE(read(QString(TEST_RESOURCES) + "/Mark/Mark.moc3"), original); QVERIFY(core.intact());
    }
};
QTEST_GUILESS_MAIN(Cmo3CanvasCoreTests)
#include "cmo3_canvas_core_tests.moc"
