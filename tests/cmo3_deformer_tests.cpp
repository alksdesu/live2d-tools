#include "cmo3/ccaffarchive.h"
#include "cmo3/ccmo3builder.h"
#include "live2dstruct/csectiontable.h"
#include <QtTest>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QImage>
#include <QTemporaryDir>
#include <QXmlStreamReader>
#include <QtEndian>
#include <array>
#include <cstring>
#include <limits>

namespace {
struct XmlNode
{
    QString tag;
    QString text;
    QMap<QString, QString> attributes;
    QVector<int> children;
};

struct ExportXml
{
    QVector<XmlNode> nodes;
    QHash<QString, int> definitions;

    bool parse(const QByteArray& bytes, QString* error)
    {
        QXmlStreamReader xml(bytes);
        xml.setNamespaceProcessing(false);
        QVector<int> parents;
        while (!xml.atEnd()) {
            xml.readNext();
            if (xml.isStartElement()) {
                XmlNode node;
                node.tag = xml.name().toString();
                for (const auto& attribute : xml.attributes())
                    node.attributes.insert(attribute.qualifiedName().toString(), attribute.value().toString());
                const int index = nodes.size();
                if (!parents.isEmpty()) nodes[parents.last()].children.append(index);
                const QString id = node.attributes.value("xs.id");
                if (!id.isEmpty()) {
                    if (definitions.contains(id)) { *error = "duplicate xs.id: " + id; return false; }
                    definitions.insert(id, index);
                }
                nodes.append(node);
                parents.append(index);
            } else if (xml.isCharacters() && !parents.isEmpty()) {
                nodes[parents.last()].text += xml.text();
            } else if (xml.isEndElement()) {
                parents.removeLast();
            }
        }
        *error = xml.errorString();
        return !xml.hasError();
    }

    int field(int parent, const QString& name) const
    {
        if (parent < 0 || parent >= nodes.size()) return -1;
        for (int child : nodes[parent].children)
            if (nodes[child].attributes.value("xs.n") == name) return child;
        return -1;
    }

    QString attribute(int index, const QString& name) const
    {
        return index >= 0 && index < nodes.size() ? nodes[index].attributes.value(name) : QString();
    }

    QString text(int index) const
    {
        return index >= 0 && index < nodes.size() ? nodes[index].text.trimmed() : QString();
    }

    int resolve(int index) const { return definitions.value(attribute(index, "xs.ref"), -1); }
};

bool copyInputs(const QString& model, const QDir& destination)
{
    const QDir source(QString(TEST_RESOURCES) + '/' + model);
    const QString jsonName = model + ".model3.json";
    if (!QFile::copy(source.filePath(jsonName), destination.filePath(jsonName))) return false;
    QFile jsonFile(source.filePath(jsonName));
    if (!jsonFile.open(QIODevice::ReadOnly)) return false;
    const auto textures = QJsonDocument::fromJson(jsonFile.readAll()).object()
        .value("FileReferences").toObject().value("Textures").toArray();
    for (const auto& texture : textures) {
        const QString path = destination.filePath(texture.toString());
        if (!QDir().mkpath(QFileInfo(path).absolutePath()) ||
            !QFile::copy(source.filePath(texture.toString()), path)) return false;
    }
    return true;
}

bool saveBytes(const QString& path, const QByteArray& bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

void writeInt(QByteArray& bytes, int offset, int value)
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

QMap<QString, bool> sourceVisibility(const live2dstruct::CSectionTable& table)
{
    QMap<QString, bool> result;
    for (const auto* part : table.parts) result.insert("part|" + part->ids, part->is_visible);
    for (const auto* mesh : table.art_meshes) result.insert("artmesh|" + mesh->ids, mesh->is_visible);
    for (const auto* deformer : table.deformers)
        result.insert((deformer->types == 0 ? QStringLiteral("warp|") : QStringLiteral("rotation|")) +
                      deformer->ids, deformer->is_visible);
    return result;
}

QMap<QString, bool> exportedVisibility(const ExportXml& xml)
{
    QMap<QString, bool> result;
    for (int index = 0; index < xml.nodes.size(); ++index) {
        const QString tag = xml.nodes[index].tag;
        const QString type = tag == "CPartSource" ? QStringLiteral("part") :
            tag == "CArtMeshSource" ? QStringLiteral("artmesh") :
            tag == "CWarpDeformerSource" ? QStringLiteral("warp") :
            tag == "CRotationDeformerSource" ? QStringLiteral("rotation") : QString();
        if (type.isEmpty() || xml.attribute(index, "xs.id").isEmpty()) continue;
        const int idParent = type == QLatin1String("part") ? index : xml.field(index, "super");
        const QString id = xml.attribute(xml.field(idParent, "id"), "idstr");
        const QString visible = xml.text(xml.field(xml.field(idParent, "super"), "isVisible"));
        if (!id.isEmpty() && (visible == "true" || visible == "false"))
            result.insert(type + '|' + id, visible == "true");
    }
    return result;
}

bool visibilityInputs(const QDir& directory, const QByteArray& bytes)
{
    auto reader = live2dstruct::CReader::FromBytes(bytes);
    live2dstruct::CSectionTable table(reader);
    if (!table.IsValid() || !saveBytes(directory.filePath("fixture.moc3"), bytes)) return false;
    int atlases = 1;
    for (const auto* mesh : table.art_meshes) atlases = qMax(atlases, mesh->texture_nos + 1);
    QJsonArray textures;
    for (int index = 0; index < atlases; ++index) textures.append("atlas.png");
    const QJsonObject references{{"Moc", "fixture.moc3"}, {"Textures", textures}};
    if (!saveBytes(directory.filePath("fixture.model3.json"), QJsonDocument(QJsonObject{{"FileReferences", references}}).toJson())) return false;
    QImage atlas(32, 32, QImage::Format_ARGB32);
    atlas.fill(qRgba(80, 130, 180, 255));
    return atlas.save(directory.filePath("atlas.png"));
}
}

class Cmo3DeformerTests : public QObject
{
    Q_OBJECT
private slots:
    void originalVisibilitySurvivesCaffExport_data()
    {
        QTest::addColumn<QString>("model");
        for (const char* model : {"Mark", "Natori", "Rice", "Wanko"}) QTest::newRow(model) << QString::fromLatin1(model);
    }

    void originalVisibilitySurvivesCaffExport()
    {
        QFETCH(QString, model);
        live2dstruct::CReader reader(QString(TEST_RESOURCES) + '/' + model + '/' + model + ".moc3");
        live2dstruct::CSectionTable table(reader);
        QVERIFY(table.IsValid());
        const auto expected = sourceVisibility(table);
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        QVERIFY(visibilityInputs(QDir(temporary.path()), reader.m_byte));
        cmo3::CCmo3Builder::Options options;
        options.mocPath = temporary.filePath("fixture.moc3");
        options.outPath = temporary.filePath("original.cmo3");
        QString error;
        QVERIFY2(cmo3::CCmo3Builder::Build(options, &error), qPrintable(error));
        cmo3::CCaffArchive archive;
        QVERIFY2(archive.Load(options.outPath, &error), qPrintable(error));
        const auto* main = archive.FindByPath("main.xml");
        QVERIFY(main);
        ExportXml xml;
        QVERIFY2(xml.parse(main->content, &error), qPrintable(error));
        const auto visible = exportedVisibility(xml);
        for (auto it = expected.cbegin(); it != expected.cend(); ++it) {
            QVERIFY2(visible.contains(it.key()), qPrintable(it.key()));
            QCOMPARE(visible.value(it.key()), it.value());
        }
    }

    void typedVisibilityOverridesSourceAndLegacyIds_data()
    {
        QTest::addColumn<QString>("type");
        QTest::addColumn<int>("mode");
        for (const char* type : {"part", "artmesh", "warp", "rotation"})
            for (int mode = 0; mode < 3; ++mode)
                QTest::newRow(qPrintable(QStringLiteral("%1-%2").arg(type).arg(mode))) << QString::fromLatin1(type) << mode;
    }

    void typedVisibilityOverridesSourceAndLegacyIds()
    {
        QFETCH(QString, type);
        QFETCH(int, mode);
        live2dstruct::CReader reader(QString(TEST_RESOURCES) + "/Mark/Mark.moc3");
        live2dstruct::CSectionTable table(reader);
        QVERIFY(table.IsValid());
        const auto& offsets = *table.sectionOffsetTable;
        int warp = -1, rotation = -1;
        for (int index = 0; index < table.deformers.size(); ++index) {
            if (table.deformers[index]->types == 0 && warp == -1) warp = index;
            if (table.deformers[index]->types == 1 && rotation == -1) rotation = index;
        }
        QVERIFY(warp >= 0 && rotation >= 0 && !table.parts.isEmpty() && !table.art_meshes.isEmpty());
        QByteArray bytes = reader.m_byte;
        const QByteArray shared("SharedVisibility");
        const auto rename = [&](int offset, int row, const QByteArray& id) {
            bytes.replace(offset + row * 64, 64, id.leftJustified(64, '\0'));
        };
        rename(offsets.parts->ids, 0, shared);
        rename(offsets.art_meshes->ids, 0, shared);
        rename(offsets.deformers->ids, type == "rotation" ? rotation : warp, shared);
        const bool sourceVisible = mode != 0;
        writeInt(bytes, offsets.parts->is_visible, sourceVisible);
        writeInt(bytes, offsets.art_meshes->is_visible, sourceVisible);
        writeInt(bytes, offsets.deformers->is_visible + warp * 4, sourceVisible);
        writeInt(bytes, offsets.deformers->is_visible + rotation * 4, sourceVisible);
        auto patchedReader = live2dstruct::CReader::FromBytes(bytes);
        live2dstruct::CSectionTable patched(patchedReader);
        QVERIFY(patched.IsValid());
        auto expected = sourceVisibility(patched);
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        QVERIFY(visibilityInputs(QDir(temporary.path()), bytes));
        cmo3::CCmo3Builder::Options options;
        options.mocPath = temporary.filePath("fixture.moc3");
        options.outPath = temporary.filePath("visibility.cmo3");
        if (mode == 2) options.hiddenIds.insert(QString::fromUtf8(shared));
        for (auto it = expected.begin(); it != expected.end(); ++it)
            it.value() = it.value() && !options.hiddenIds.contains(it.key().mid(it.key().indexOf('|') + 1));
        const QString key = type + '|' + QString::fromUtf8(shared);
        options.objectVisibility.insert(key, mode != 1);
        options.objectVisibility.insert("wrong-type|SharedVisibility", false);
        expected[key] = mode != 1;
        QString error;
        QVERIFY2(cmo3::CCmo3Builder::Build(options, &error), qPrintable(error));
        cmo3::CCaffArchive archive;
        QVERIFY2(archive.Load(options.outPath, &error), qPrintable(error));
        const auto* main = archive.FindByPath("main.xml");
        QVERIFY(main);
        ExportXml xml;
        QVERIFY2(xml.parse(main->content, &error), qPrintable(error));
        const auto visible = exportedVisibility(xml);
        for (auto it = expected.cbegin(); it != expected.cend(); ++it) {
            QVERIFY2(visible.contains(it.key()), qPrintable(it.key()));
            QCOMPARE(visible.value(it.key()), it.value());
        }
        QFile original(options.mocPath);
        QVERIFY(original.open(QIODevice::ReadOnly));
        QCOMPARE(original.readAll(), bytes);
    }

    void invalidCanvasPreservesExistingOutput_data()
    {
        QTest::addColumn<int>("field");
        QTest::addColumn<float>("value");
        const float nan = std::numeric_limits<float>::quiet_NaN();
        const float infinity = std::numeric_limits<float>::infinity();
        for (int field : {3, 4}) {
            QTest::newRow(qPrintable(QString("dimension-%1-nan").arg(field))) << field << nan;
            QTest::newRow(qPrintable(QString("dimension-%1-inf").arg(field))) << field << infinity;
            QTest::newRow(qPrintable(QString("dimension-%1-negative").arg(field))) << field << -1.0f;
            QTest::newRow(qPrintable(QString("dimension-%1-zero").arg(field))) << field << 0.0f;
            QTest::newRow(qPrintable(QString("dimension-%1-overflow").arg(field))) << field << 2147483648.0f;
            QTest::newRow(qPrintable(QString("dimension-%1-subpixel").arg(field))) << field << 0.25f;
        }
        QTest::newRow("ppu-nan") << 0 << nan;
        QTest::newRow("ppu-inf") << 0 << infinity;
        QTest::newRow("ppu-zero") << 0 << 0.0f;
        QTest::newRow("ppu-negative") << 0 << -1.0f;
        for (int field : {1, 2}) {
            QTest::newRow(qPrintable(QString("origin-%1-nan").arg(field))) << field << nan;
            QTest::newRow(qPrintable(QString("origin-%1-inf").arg(field))) << field << infinity;
            QTest::newRow(qPrintable(QString("origin-%1-negative-inf").arg(field))) << field << -infinity;
        }
    }

    void invalidCanvasPreservesExistingOutput()
    {
        QFETCH(int, field);
        QFETCH(float, value);
        const QString model = QStringLiteral("Mark");
        QFile source(QString(TEST_RESOURCES) + '/' + model + '/' + model + ".moc3");
        QVERIFY(source.open(QIODevice::ReadOnly));
        QByteArray bytes = source.readAll();
        QVERIFY(bytes.size() >= 0x48);
        const int canvasOffset = qFromLittleEndian<qint32>(bytes.constData() + 0x44);
        QVERIFY(canvasOffset >= 0 && canvasOffset <= bytes.size() - 20);
        writeFloat(bytes, canvasOffset + field * 4, value);
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QDir directory(temporary.path());
        QVERIFY(copyInputs(model, directory));
        cmo3::CCmo3Builder::Options options;
        options.mocPath = directory.filePath(model + ".moc3");
        options.outPath = directory.filePath("existing.cmo3");
        QVERIFY(saveBytes(options.mocPath, bytes));
        const QByteArray original("previous-project");
        QVERIFY(saveBytes(options.outPath, original));
        QString error;
        QVERIFY(!cmo3::CCmo3Builder::Build(options, &error));
        QVERIFY2(error.contains(QStringLiteral("画布")) || error.contains("canvas", Qt::CaseInsensitive), qPrintable(error));
        QFile target(options.outPath);
        QVERIFY(target.open(QIODevice::ReadOnly));
        QCOMPARE(target.readAll(), original);
    }

    void finitePixelConversionOverflowPreservesExistingOutput_data()
    {
        QTest::addColumn<int>("kind");
        QTest::newRow("mesh") << 0;
        QTest::newRow("warp") << 1;
        QTest::newRow("rotation") << 2;
    }

    void finitePixelConversionOverflowPreservesExistingOutput()
    {
        QFETCH(int, kind);
        const QString mocPath = QString(TEST_RESOURCES) + QStringLiteral("/Mark/Mark.moc3");
        live2dstruct::CModelDocument document;
        QString error;
        QVERIFY2(document.Load(mocPath, &error), qPrintable(error));
        cmo3::CCmo3Builder::Options options;
        options.mocPath = mocPath;
        const float maximum = std::numeric_limits<float>::max();
        bool changed = false;
        if (kind == 0) {
            live2dstruct::CReader reader(mocPath);
            live2dstruct::CSectionTable table(reader);
            QVERIFY(table.IsValid());
            options.mocBytes = reader.m_byte;
            writeInt(options.mocBytes, table.sectionOffsetTable->art_meshes->parent_deformer_indices, -1);
            const auto& mesh = document.Drawables().first();
            QVERIFY(!mesh.keyforms.isEmpty() && !mesh.keyforms.first().positions.isEmpty());
            auto positions = mesh.keyforms.first().positions;
            positions[0] = maximum;
            options.drawableKeyformPositionOverrides.insert(mesh.id, {positions});
            changed = true;
        } else {
            for (const auto& deformer : document.Deformers()) {
                if (kind == 1 && deformer.type == live2dstruct::CModelDeformer::Warp) {
                    auto forms = document.DeformerWarpKeyforms(deformer.id);
                    if (forms.isEmpty() || forms[0].positions.isEmpty()) continue;
                    forms[0].positions[0] = maximum;
                    options.deformerWarpKeyformOverrides.insert(deformer.id, forms);
                } else if (kind == 2 && deformer.type == live2dstruct::CModelDeformer::Rotation) {
                    auto forms = document.DeformerRotationKeyforms(deformer.id);
                    if (forms.isEmpty()) continue;
                    forms[0].originX = maximum;
                    options.deformerRotationKeyformOverrides.insert(deformer.id, forms);
                } else continue;
                options.deformerParentOverrides.insert(deformer.id, -1);
                changed = true;
                break;
            }
        }
        QVERIFY(changed);
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        options.outPath = temporary.filePath("existing.cmo3");
        const QByteArray original("previous-project");
        QVERIFY(saveBytes(options.outPath, original));
        QVERIFY(!cmo3::CCmo3Builder::Build(options, &error));
        QVERIFY2(error.contains("float") && error.contains(QStringLiteral("坐标")), qPrintable(error));
        QFile target(options.outPath);
        QVERIFY(target.open(QIODevice::ReadOnly));
        QCOMPARE(target.readAll(), original);
    }

    void rootCoordinatesUsePixelOrigins_data()
    {
        QTest::addColumn<QString>("model");
        QTest::addColumn<QPointF>("center");
        QTest::addColumn<QPointF>("shifted");
        QTest::addColumn<bool>("customCanvas");
        QTest::newRow("Mark") << QString("Mark") << QPointF(1200, 1500) << QPointF(6000, -5700) << false;
        QTest::newRow("Rice") << QString("Rice") << QPointF(2250, 1500) << QPointF(8250, -7500) << false;
        QTest::newRow("Natori") << QString("Natori") << QPointF(2000, 3500) << QPointF(10000, -8500) << false;
        QTest::newRow("Wanko") << QString("Wanko") << QPointF(600, 600) << QPointF(3000, -3000) << false;
        QTest::newRow("independent-canvas-origin") << QString("Mark") << QPointF(30, 40) << QPointF(50, 10) << true;
    }

    void rotationUnitOverflowPreservesExistingOutput_data()
    {
        QTest::addColumn<float>("scale");
        QTest::newRow("positive") << std::numeric_limits<float>::max();
        QTest::newRow("negative") << -std::numeric_limits<float>::max();
    }
    void rotationUnitOverflowPreservesExistingOutput()
    {
        QFETCH(float, scale);
        const QString source = QString(TEST_RESOURCES) + "/Mark/Mark.moc3";
        live2dstruct::CModelDocument document; QString error; QVERIFY(document.Load(source, &error));
        cmo3::CCmo3Builder::Options options; options.mocPath = source;
        QString id;
        for (const auto& deformer : document.Deformers()) if (deformer.type == live2dstruct::CModelDeformer::Rotation) {
            auto forms = document.DeformerRotationKeyforms(deformer.id); if (forms.isEmpty()) continue;
            forms[0].scale = scale; options.deformerRotationKeyformOverrides.insert(deformer.id, forms);
            options.deformerParentOverrides.insert(deformer.id, -1); id = deformer.id; break;
        }
        QVERIFY(!id.isEmpty()); QTemporaryDir temporary; QVERIFY(temporary.isValid()); options.outPath = temporary.filePath("keep.cmo3");
        QVERIFY(saveBytes(options.outPath, "original target")); QVERIFY(!cmo3::CCmo3Builder::Build(options, &error));
        QVERIFY2(error.contains(QStringLiteral("缩放")), qPrintable(error)); QFile preserved(options.outPath); QVERIFY(preserved.open(QIODevice::ReadOnly)); QCOMPARE(preserved.readAll(), QByteArray("original target"));
    }
    void cyclicEffectiveParentsPreserveExistingOutput()
    {
        const QString source = QString(TEST_RESOURCES) + "/Mark/Mark.moc3";
        live2dstruct::CReader reader(source); live2dstruct::CSectionTable table(reader); QVERIFY(table.IsValid()); QVERIFY(table.deformers.size() >= 2);
        cmo3::CCmo3Builder::Options options; options.mocPath = source;
        options.deformerParentOverrides.insert(table.deformers[0]->ids, 1);
        options.deformerParentOverrides.insert(table.deformers[1]->ids, 0);
        QTemporaryDir temporary; QVERIFY(temporary.isValid()); options.outPath = temporary.filePath("keep.cmo3");
        QVERIFY(saveBytes(options.outPath, "original target")); QString error; QVERIFY(!cmo3::CCmo3Builder::Build(options, &error));
        QVERIFY2(error.contains(QStringLiteral("父链")), qPrintable(error)); QFile preserved(options.outPath); QVERIFY(preserved.open(QIODevice::ReadOnly)); QCOMPARE(preserved.readAll(), QByteArray("original target"));
    }

    void rootCoordinatesUsePixelOrigins()
    {
        QFETCH(QString, model);
        QFETCH(QPointF, center);
        QFETCH(QPointF, shifted);
        QFETCH(bool, customCanvas);
        live2dstruct::CReader reader(QString(TEST_RESOURCES) + '/' + model + '/' + model + ".moc3");
        live2dstruct::CSectionTable table(reader);
        QVERIFY(table.IsValid());
        QVERIFY(!table.art_meshes.isEmpty());
        QByteArray bytes = reader.m_byte;
        writeInt(bytes, table.sectionOffsetTable->art_meshes->parent_deformer_indices, -1);
        if (customCanvas) {
            const int canvas = qFromLittleEndian<qint32>(bytes.constData() + 0x44);
            const std::array<float, 5> fields{10, 30, 40, 200, 300};
            for (int field = 0; field < 5; ++field) writeFloat(bytes, canvas + field * 4, fields[field]);
        }
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QDir directory(temporary.path());
        QVERIFY(copyInputs(model, directory));
        const QString mocPath = directory.filePath(model + ".moc3");
        QVERIFY(saveBytes(mocPath, bytes));
        live2dstruct::CModelDocument document;
        QString error;
        QVERIFY2(document.Load(mocPath, &error), qPrintable(error));
        const auto& mesh = document.Drawables().first();
        QVERIFY(mesh.vertexCount >= 2 && !mesh.keyforms.isEmpty());
        cmo3::CCmo3Builder::Options options;
        options.mocPath = mocPath;
        options.outPath = directory.filePath("canvas.cmo3");
        auto meshPositions = mesh.keyforms.first().positions;
        meshPositions[0] = 0;
        meshPositions[1] = 0;
        meshPositions[2] = 2;
        meshPositions[3] = -3;
        options.drawableKeyformPositionOverrides.insert(mesh.id, {meshPositions});
        QString warpId, rotationId;
        for (const auto& deformer : document.Deformers()) {
            if (warpId.isEmpty() && deformer.type == live2dstruct::CModelDeformer::Warp) {
                auto forms = document.DeformerWarpKeyforms(deformer.id);
                if (forms.isEmpty() || forms[0].positions.size() < 4) continue;
                forms[0].positions[0] = 0;
                forms[0].positions[1] = 0;
                forms[0].positions[2] = 2;
                forms[0].positions[3] = -3;
                options.deformerWarpKeyformOverrides.insert(deformer.id, forms);
                options.deformerParentOverrides.insert(deformer.id, -1);
                warpId = deformer.id;
            }
            if (rotationId.isEmpty() && deformer.type == live2dstruct::CModelDeformer::Rotation) {
                auto forms = document.DeformerRotationKeyforms(deformer.id);
                if (forms.size() < 2) continue;
                forms[0].originX = 0;
                forms[0].originY = 0;
                forms[1].originX = 2;
                forms[1].originY = -3;
                options.deformerRotationKeyformOverrides.insert(deformer.id, forms);
                options.deformerParentOverrides.insert(deformer.id, -1);
                rotationId = deformer.id;
            }
        }
        QVERIFY(!warpId.isEmpty() && !rotationId.isEmpty());
        QVERIFY2(cmo3::CCmo3Builder::Build(options, &error), qPrintable(error));
        cmo3::CCaffArchive archive;
        QVERIFY2(archive.Load(options.outPath, &error), qPrintable(error));
        const auto* main = archive.FindByPath("main.xml");
        QVERIFY(main);
        ExportXml xml;
        QVERIFY2(xml.parse(main->content, &error), qPrintable(error));
        const auto findSource = [&](const QString& tag, const QString& id) {
            for (int index = 0; index < xml.nodes.size(); ++index)
                if (xml.nodes[index].tag == tag && !xml.attribute(index, "xs.id").isEmpty() &&
                    xml.attribute(xml.field(xml.field(index, "super"), "id"), "idstr") == id) return index;
            return -1;
        };
        const int meshSource = findSource("CArtMeshSource", mesh.id);
        const int warpSource = findSource("CWarpDeformerSource", warpId);
        const int rotationSource = findSource("CRotationDeformerSource", rotationId);
        QVERIFY(meshSource >= 0 && warpSource >= 0 && rotationSource >= 0);
        for (int source : {meshSource, warpSource}) {
            const int forms = xml.field(source, "keyforms");
            QVERIFY(forms >= 0 && !xml.nodes[forms].children.isEmpty());
            const auto values = xml.text(xml.field(xml.nodes[forms].children[0], "positions")).split(' ', Qt::SkipEmptyParts);
            QVERIFY(values.size() >= 4);
            QCOMPARE(QPointF(values[0].toDouble(), values[1].toDouble()), center);
            QCOMPARE(QPointF(values[2].toDouble(), values[3].toDouble()), shifted);
        }
        const int forms = xml.field(rotationSource, "keyforms");
        QVERIFY(forms >= 0 && xml.nodes[forms].children.size() >= 2);
        QCOMPARE(QPointF(xml.attribute(xml.nodes[forms].children[0], "originX").toDouble(),
                         xml.attribute(xml.nodes[forms].children[0], "originY").toDouble()), center);
        QCOMPARE(QPointF(xml.attribute(xml.nodes[forms].children[1], "originX").toDouble(),
                         xml.attribute(xml.nodes[forms].children[1], "originY").toDouble()), shifted);
    }

    void sourceRelationships_data()
    {
        QTest::addColumn<QString>("model");
        QTest::addColumn<int>("permutedType");
        QTest::addColumn<bool>("editKeyforms");
        for (const QString& model : {QStringLiteral("Mark"), QStringLiteral("Wanko"),
                                    QStringLiteral("Rice"), QStringLiteral("Natori")})
            QTest::newRow(qPrintable(model)) << model << -1 << false;
        QTest::newRow("warp-permuted") << QStringLiteral("Mark") << 0 << false;
        QTest::newRow("rotation-permuted") << QStringLiteral("Mark") << 1 << false;
        QTest::newRow("warp-edited-permuted") << QStringLiteral("Mark") << 0 << true;
        QTest::newRow("rotation-edited-permuted") << QStringLiteral("Mark") << 1 << true;
    }

    void sourceRelationships()
    {
        QFETCH(QString, model);
        QFETCH(int, permutedType);
        QFETCH(bool, editKeyforms);
        live2dstruct::CReader original(QString(TEST_RESOURCES) + '/' + model + '/' + model + ".moc3");
        live2dstruct::CSectionTable originalTable(original);
        QVERIFY(originalTable.IsValid());
        QByteArray bytes = original.m_byte;
        if (permutedType >= 0) {
            QVector<int> candidates;
            for (int i = 0; i < originalTable.deformers.size(); ++i)
                if (originalTable.deformers[i]->types == permutedType) candidates.append(i);
            QVERIFY(candidates.size() >= 2);
            const int first = candidates.first();
            const int second = candidates.last();
            const auto* offsets = originalTable.sectionOffsetTable->deformers;
            writeInt(bytes, offsets->specific_sources_indices + first * 4,
                     originalTable.deformers[second]->specific_sources_indices);
            writeInt(bytes, offsets->specific_sources_indices + second * 4,
                     originalTable.deformers[first]->specific_sources_indices);
            writeInt(bytes, offsets->keyform_binding_sources_indices + first * 4,
                     originalTable.deformers[second]->keyform_binding_sources_indices);
            writeInt(bytes, offsets->keyform_binding_sources_indices + second * 4,
                     originalTable.deformers[first]->keyform_binding_sources_indices);
        }

        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QDir dir(temp.path());
        QVERIFY(copyInputs(model, dir));
        const QString mocPath = dir.filePath(model + ".moc3");
        QVERIFY(saveBytes(mocPath, bytes));
        live2dstruct::CReader reader(mocPath);
        live2dstruct::CSectionTable table(reader);
        QVERIFY(table.IsValid());
        live2dstruct::CModelDocument document;
        QString error;
        QVERIFY2(document.Load(mocPath, &error), qPrintable(error));
        if (editKeyforms) {
            for (const auto& deformer : document.Deformers()) {
                if (deformer.type == live2dstruct::CModelDeformer::Warp) {
                    auto keyforms = document.DeformerWarpKeyforms(deformer.id);
                    QVERIFY(!keyforms.isEmpty() && !keyforms.first().positions.isEmpty());
                    keyforms[0].positions[0] += 0.375f;
                    QVERIFY(document.SetDeformerWarpKeyforms(deformer.id, keyforms));
                } else {
                    auto keyforms = document.DeformerRotationKeyforms(deformer.id);
                    QVERIFY(!keyforms.isEmpty());
                    keyforms[0].angle += 13.25f;
                    keyforms[0].reflectY = !keyforms[0].reflectY;
                    QVERIFY(document.SetDeformerRotationKeyforms(deformer.id, keyforms));
                }
            }
        }
        cmo3::CCmo3Builder::Options options;
        options.mocPath = mocPath;
        options.outPath = dir.filePath("result.cmo3");
        options.deformerWarpKeyformOverrides = document.DeformerWarpKeyformOverrides();
        options.deformerRotationKeyformOverrides = document.DeformerRotationKeyformOverrides();
        QVERIFY2(cmo3::CCmo3Builder::Build(options, &error), qPrintable(error));
        cmo3::CCaffArchive archive;
        QVERIFY2(archive.Load(options.outPath, &error), qPrintable(error));
        const auto* main = archive.FindByPath("main.xml");
        QVERIFY(main);
        ExportXml xml;
        QVERIFY2(xml.parse(main->content, &error), qPrintable(error));
        QHash<QString, int> sources;
        int sourceSet = -1;
        for (int i = 0; i < xml.nodes.size(); ++i) {
            const auto& node = xml.nodes[i];
            if (node.tag == "CDeformerSourceSet") sourceSet = i;
            if ((node.tag != "CWarpDeformerSource" && node.tag != "CRotationDeformerSource") ||
                xml.attribute(i, "xs.id").isEmpty()) continue;
            const int super = xml.field(i, "super");
            const QString id = xml.attribute(xml.field(super, "id"), "idstr");
            QVERIFY2(!id.isEmpty() && !sources.contains(id), qPrintable(id));
            sources.insert(id, i);
        }
        QCOMPARE(sources.size(), table.deformers.size());
        QVERIFY(sourceSet >= 0);
        const int sourceList = xml.field(sourceSet, "_sources");
        QVERIFY(sourceList >= 0);
        const auto& orderedSources = xml.nodes[sourceList].children;
        QCOMPARE(orderedSources.size(), table.deformers.size());
        QCOMPARE(xml.attribute(sourceList, "count").toInt(), table.deformers.size());

        for (int d = 0; d < table.deformers.size(); ++d) {
            const auto* deformer = table.deformers[d];
            const int source = sources.value(deformer->ids, -1);
            QVERIFY2(source >= 0, qPrintable(deformer->ids));
            QCOMPARE(xml.resolve(orderedSources[d]), source);
            const int super = xml.field(source, "super");
            QCOMPARE(xml.attribute(xml.resolve(xml.field(super, "guid")), "note"), deformer->ids);
            const int parent = deformer->parent_deformer_indices;
            const int targetGuid = xml.resolve(xml.field(super, "targetDeformerGuid"));
            QVERIFY(targetGuid >= 0);
            if (parent >= 0) QCOMPARE(xml.attribute(targetGuid, "note"), table.deformers[parent]->ids);
            const int keyformList = xml.field(source, "keyforms");
            QVERIFY(keyformList >= 0);
            const auto& forms = xml.nodes[keyformList].children;
            const int common = xml.field(super, "super");
            const int grid = xml.resolve(xml.field(common, "keyformGridSource"));
            QVERIFY(grid >= 0);
            const int gridForms = xml.field(grid, "keyformsOnGrid");
            QVERIFY(gridForms >= 0);
            QCOMPARE(xml.nodes[gridForms].children.size(), forms.size());
            for (int k = 0; k < forms.size(); ++k) {
                const int formBase = xml.field(xml.field(forms[k], "super"), "super");
                QCOMPARE(xml.resolve(xml.field(formBase, "_source")), source);
                QCOMPARE(xml.resolve(xml.field(formBase, "guid")),
                         xml.resolve(xml.field(xml.nodes[gridForms].children[k], "keyformGuid")));
            }
            if (deformer->types == 0) {
                const auto* warp = table.warp_deformers[deformer->specific_sources_indices];
                QCOMPARE(xml.text(xml.field(source, "row")).toInt(), warp->rows);
                QCOMPARE(xml.text(xml.field(source, "col")).toInt(), warp->columns);
                QCOMPARE(xml.text(xml.field(source, "isQuadTransform")), warp->is_quad_source ? QString("true") : QString("false"));
                const auto expected = document.DeformerWarpKeyforms(deformer->ids);
                QCOMPARE(forms.size(), expected.size());
                for (int k = 0; k < forms.size(); ++k) {
                    const int positionNode = xml.field(forms[k], "positions");
                    const auto values = xml.text(positionNode).split(' ', Qt::SkipEmptyParts);
                    QCOMPARE(values.size(), expected[k].positions.size());
                    QCOMPARE(xml.attribute(positionNode, "count").toInt(), values.size());
                    for (int v = 0; v < values.size(); ++v) {
                        float expectedValue = expected[k].positions[v];
                        if (parent < 0) expectedValue = v % 2 == 0
                            ? static_cast<float>(double(expectedValue) * table.canvas_info->pixels_per_unit + table.canvas_info->origin_x)
                            : static_cast<float>(table.canvas_info->origin_y + double(expectedValue) * table.canvas_info->pixels_per_unit);
                        QCOMPARE(values[v].toFloat(), expectedValue);
                    }
                    QCOMPARE(xml.text(xml.field(xml.field(forms[k], "super"), "opacity")).toFloat(), expected[k].opacity);
                }
            } else {
                const auto* rotation = table.rotation_deformers[deformer->specific_sources_indices];
                QCOMPARE(xml.text(xml.field(source, "baseAngle")).toFloat(), rotation->base_angles);
                const auto expected = document.DeformerRotationKeyforms(deformer->ids);
                QCOMPARE(forms.size(), expected.size());
                for (int k = 0; k < forms.size(); ++k) {
                    const auto& form = expected[k];
                    QCOMPARE(xml.attribute(forms[k], "angle").toFloat(), form.angle);
                    QCOMPARE(xml.attribute(forms[k], "originX").toFloat(), parent >= 0 ? form.originX :
                             static_cast<float>(double(form.originX) * table.canvas_info->pixels_per_unit + table.canvas_info->origin_x));
                    QCOMPARE(xml.attribute(forms[k], "originY").toFloat(), parent >= 0 ? form.originY :
                             static_cast<float>(table.canvas_info->origin_y + double(form.originY) * table.canvas_info->pixels_per_unit));
                    bool rotationAncestor = false;
                    for (int ancestor = parent; ancestor >= 0; ancestor = table.deformers[ancestor]->parent_deformer_indices)
                        rotationAncestor = rotationAncestor || table.deformers[ancestor]->types == 1;
                    QCOMPARE(xml.attribute(forms[k], "scale").toFloat(), rotationAncestor ? form.scale :
                             static_cast<float>(double(form.scale) * table.canvas_info->pixels_per_unit));
                    QCOMPARE(xml.attribute(forms[k], "isReflectX"), form.reflectX ? QString("true") : QString("false"));
                    QCOMPARE(xml.attribute(forms[k], "isReflectY"), form.reflectY ? QString("true") : QString("false"));
                    QCOMPARE(xml.text(xml.field(xml.field(forms[k], "super"), "opacity")).toFloat(), form.opacity);
                }
            }
        }
    }

    void drawableFlagsFollowSourceBytesAndExplicitOverrides_data()
    {
        QTest::addColumn<QString>("model");
        QTest::addColumn<bool>("overrideMasks");
        QTest::addColumn<int>("firstFlagOverride");
        for (const QString& model : {QStringLiteral("Mark"), QStringLiteral("Wanko"),
                                    QStringLiteral("Rice"), QStringLiteral("Natori")})
            QTest::newRow(qPrintable(model)) << model << false << -1;
        QTest::newRow("mask-overrides") << QStringLiteral("Rice") << true << -1;
        QTest::newRow("additive-precedes-multiply") << QStringLiteral("Mark") << false << 0x07;
    }

    void drawableFlagsFollowSourceBytesAndExplicitOverrides()
    {
        QFETCH(QString, model);
        QFETCH(bool, overrideMasks);
        QFETCH(int, firstFlagOverride);
        live2dstruct::CReader reader(QString(TEST_RESOURCES) + '/' + model + '/' + model + ".moc3");
        live2dstruct::CSectionTable table(reader);
        QVERIFY(table.IsValid());
        const int count = table.art_meshes.size();
        QVERIFY(count > 0);
        const auto* offsets = table.sectionOffsetTable->art_meshes;
        QByteArray bytes = reader.m_byte;
        QVERIFY(offsets->drawable_flags > 0 && offsets->drawable_flags <= bytes.size() - count);
        QVERIFY(offsets->ids > 0 && offsets->ids <= bytes.size() - count * 64);
        QVERIFY(offsets->drawable_mask_sources_begin_indices > 0 &&
                offsets->drawable_mask_sources_begin_indices <= bytes.size() - count * 4);
        QVERIFY(offsets->drawable_mask_sources_counts > 0 &&
                offsets->drawable_mask_sources_counts <= bytes.size() - count * 4);
        if (firstFlagOverride >= 0) bytes[offsets->drawable_flags] = static_cast<char>(firstFlagOverride);
        QStringList ids;
        QVector<quint8> flags;
        QVector<QVector<int>> sourceMasks;
        QVector<int> invertedMasked;
        QVector<int> normalMasked;
        int additiveCount = 0;
        int invertedCount = 0;
        for (int index = 0; index < count; ++index) {
            const QByteArray rawId = bytes.mid(offsets->ids + index * 64, 64);
            ids.append(QString::fromUtf8(rawId.constData(), rawId.indexOf('\0') < 0 ? rawId.size() : rawId.indexOf('\0')));
            QVERIFY(!ids.last().isEmpty());
            const quint8 flag = static_cast<quint8>(bytes[offsets->drawable_flags + index]);
            flags.append(flag);
            if (flag & 0x01) ++additiveCount;
            if (flag & 0x08) ++invertedCount;
            const qint32 begin = qFromLittleEndian<qint32>(bytes.constData() + offsets->drawable_mask_sources_begin_indices + index * 4);
            const qint32 maskCount = qFromLittleEndian<qint32>(bytes.constData() + offsets->drawable_mask_sources_counts + index * 4);
            QVERIFY(begin >= 0 && maskCount >= 0 && begin <= table.drawable_masks.size() &&
                    maskCount <= table.drawable_masks.size() - begin);
            QVector<int> masks;
            for (int mask = 0; mask < maskCount; ++mask) {
                const qint64 position = static_cast<qint64>(table.sectionOffsetTable->drawable_masks->art_mesh_sources_indices) +
                                        static_cast<qint64>(begin + mask) * 4;
                QVERIFY(position > 0 && position <= bytes.size() - 4);
                const qint32 meshIndex = qFromLittleEndian<qint32>(bytes.constData() + position);
                QVERIFY(meshIndex >= 0 && meshIndex < count);
                masks.append(meshIndex);
            }
            sourceMasks.append(masks);
            if (!masks.isEmpty()) ((flag & 0x08) ? invertedMasked : normalMasked).append(index);
        }
        if (model == QLatin1String("Rice")) {
            QCOMPARE(additiveCount, 21);
            QCOMPARE(invertedCount, 7);
        } else if (model == QLatin1String("Natori")) {
            QCOMPARE(ids[150], QStringLiteral("ArtMesh7"));
            QCOMPARE(flags[150], quint8(0x06));
        } else if (model == QLatin1String("Wanko")) {
            QVERIFY(ids.contains(QStringLiteral("HitArea")));
            QCOMPARE(flags[ids.indexOf(QStringLiteral("HitArea"))], quint8(0x04));
        } else {
            for (quint8 flag : flags) QVERIFY(flag & 0x04);
        }

        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QDir directory(temporary.path());
        QVERIFY(copyInputs(model, directory));
        const QString mocPath = directory.filePath(model + ".moc3");
        QVERIFY(saveBytes(mocPath, bytes));
        cmo3::CCmo3Builder::Options options;
        options.mocPath = mocPath;
        options.outPath = directory.filePath("drawable-flags.cmo3");
        if (overrideMasks) {
            QVERIFY(invertedMasked.size() >= 2 && !normalMasked.isEmpty());
            options.drawableMaskInverted.insert(ids[invertedMasked[0]], false);
            options.drawableMaskInverted.insert(ids[normalMasked[0]], true);
            options.drawableMaskIndexOverrides.insert(ids[invertedMasked[1]], {});
            options.drawableMaskInverted.insert(ids[invertedMasked[1]], true);
        }
        QString error;
        QVERIFY2(cmo3::CCmo3Builder::Build(options, &error), qPrintable(error));
        cmo3::CCaffArchive archive;
        QVERIFY2(archive.Load(options.outPath, &error), qPrintable(error));
        const auto* main = archive.FindByPath("main.xml");
        QVERIFY(main);
        ExportXml xml;
        QVERIFY2(xml.parse(main->content, &error), qPrintable(error));
        QHash<QString, int> sources;
        for (int index = 0; index < xml.nodes.size(); ++index) {
            if (xml.nodes[index].tag != QLatin1String("CArtMeshSource") || xml.attribute(index, "xs.id").isEmpty()) continue;
            const QString id = xml.attribute(xml.field(xml.field(index, "super"), "id"), "idstr");
            QVERIFY2(!id.isEmpty() && !sources.contains(id), qPrintable(id));
            sources.insert(id, index);
        }
        QCOMPARE(sources.size(), count);
        for (int index = 0; index < count; ++index) {
            const QString& id = ids[index];
            const int source = sources.value(id, -1);
            QVERIFY2(source >= 0, qPrintable(id));
            const quint8 flag = flags[index];
            const QString composition = (flag & 0x01) ? QStringLiteral("ADD") :
                                        (flag & 0x02) ? QStringLiteral("MULTIPLY") : QStringLiteral("NORMAL");
            QCOMPARE(xml.attribute(xml.field(source, "colorComposition"), "v"), composition);
            QCOMPARE(xml.text(xml.field(source, "culling")), (flag & 0x04) ? QStringLiteral("false") : QStringLiteral("true"));
            const int base = xml.field(source, "super");
            const int maskList = xml.field(base, "clipGuidList");
            QVERIFY(maskList >= 0);
            const QVector<int> masks = options.drawableMaskIndexOverrides.value(id, sourceMasks[index]);
            QCOMPARE(xml.nodes[maskList].children.size(), masks.size());
            QCOMPARE(xml.attribute(maskList, "count").toInt(), masks.size());
            for (int mask = 0; mask < masks.size(); ++mask) {
                const int guid = xml.resolve(xml.nodes[maskList].children[mask]);
                QVERIFY(guid >= 0);
                QCOMPARE(xml.attribute(guid, "note"), ids[masks[mask]]);
            }
            const bool inverted = options.drawableMaskInverted.value(id, (flag & 0x08) != 0) && !masks.isEmpty();
            QCOMPARE(xml.text(xml.field(base, "invertClippingMask")), inverted ? QStringLiteral("true") : QStringLiteral("false"));
        }
    }

    void rejectInvalidSource_data()
    {
        QTest::addColumn<int>("mode");
        QTest::newRow("unknown-type") << 0;
        QTest::newRow("out-of-range-index") << 1;
    }

    void rejectInvalidSource()
    {
        QFETCH(int, mode);
        const QString model = QStringLiteral("Mark");
        const QString sourcePath = QString(TEST_RESOURCES) + '/' + model + '/' + model + ".moc3";
        live2dstruct::CReader original(sourcePath);
        live2dstruct::CSectionTable table(original);
        QVERIFY(table.IsValid());
        QVERIFY(!table.deformers.isEmpty());
        QByteArray bytes = original.m_byte;
        const auto* offsets = table.sectionOffsetTable->deformers;
        int firstWarp = -1;
        for (int i = 0; i < table.deformers.size(); ++i) {
            if (table.deformers[i]->types == 0) { firstWarp = i; break; }
        }
        QVERIFY(firstWarp >= 0);
        if (mode == 0) {
            writeInt(bytes, offsets->types + firstWarp * 4, 99);
        } else {
            writeInt(bytes, offsets->specific_sources_indices + firstWarp * 4,
                     table.warp_deformers.size());
        }

        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString mocPath = QDir(temp.path()).filePath(model + ".moc3");
        QVERIFY(saveBytes(mocPath, bytes));
        const QString outPath = QDir(temp.path()).filePath("result.cmo3");
        const QByteArray sentinel("existing output");
        QVERIFY(saveBytes(outPath, sentinel));

        cmo3::CCmo3Builder::Options options;
        options.mocPath = mocPath;
        options.outPath = outPath;
        QString error;
        QVERIFY(!cmo3::CCmo3Builder::Build(options, &error));
        QVERIFY2(error.contains(QStringLiteral("专用数据索引无效")), qPrintable(error));
        QFile output(outPath);
        QVERIFY(output.open(QIODevice::ReadOnly));
        QCOMPARE(output.readAll(), sentinel);
    }
};

QTEST_GUILESS_MAIN(Cmo3DeformerTests)
#include "cmo3_deformer_tests.moc"
