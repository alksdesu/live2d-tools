#include "cmo3/ccaffarchive.h"
#include "cmo3_userdata.h"
#include "live2dstruct/cmodeluserdata.h"
#include "cmo3/ccmo3builder.h"
#include "cmo3/cxsxmlwriter.h"
#include "live2dstruct/cmodeldocument.h"
#include "live2dstruct/csectiontable.h"
#include "live2dstruct/util/cfilereader.h"
#include "opengl/ctextureextractor.h"
#include <QtTest>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QXmlStreamReader>
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <limits>
#include <locale>
#include <sstream>
#include <cstring>

namespace {
QMap<QString, QMap<QString, QString>> parameterFields(const QByteArray& bytes, QString* error)
{
    QXmlStreamReader xml(bytes);
    xml.setNamespaceProcessing(false);
    QMap<QString, QMap<QString, QString>> result;
    while (!xml.atEnd()) {
        xml.readNext();
        if (!xml.isStartElement() || xml.name() != QLatin1String("CParameterSource")) continue;
        QString id;
        QMap<QString, QString> fields;
        while (!xml.atEnd()) {
            xml.readNext();
            if (xml.isEndElement() && xml.name() == QLatin1String("CParameterSource")) break;
            if (!xml.isStartElement()) continue;
            if (xml.name() == QLatin1String("CParameterId")) id = xml.attributes().value("idstr").toString();
            else if (xml.name() == QLatin1String("f") || xml.name() == QLatin1String("i") ||
                     xml.name() == QLatin1String("b") || xml.name() == QLatin1String("s")) {
                const QString name = xml.attributes().value("xs.n").toString();
                fields.insert(name, xml.readElementText());
            }
        }
        if (id.isEmpty() || result.contains(id)) {
            if (error) *error = QStringLiteral("missing or duplicate parameter definition");
            return {};
        }
        result.insert(id, fields);
    }
    if (xml.hasError()) {
        if (error) *error = xml.errorString();
        return {};
    }
    if (error) error->clear();
    return result;
}

QMap<QString, QString> objectDisplayNames(const QByteArray& bytes, QString* error)
{
    QXmlStreamReader xml(bytes);
    xml.setNamespaceProcessing(false);
    const QSet<QString> types{"CArtMeshSource", "CWarpDeformerSource", "CRotationDeformerSource", "CPartSource"};
    QMap<QString, QString> names;
    int depth = 0;
    QString type, id, name;
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement()) {
            if (!depth) {
                if (!types.contains(xml.name().toString()) || !xml.attributes().hasAttribute("xs.id")) continue;
                type = xml.name().toString(); id.clear(); name.clear(); depth = 1;
                continue;
            }
            ++depth;
            if (xml.name() == QLatin1String("CDrawableId") || xml.name() == QLatin1String("CDeformerId") || xml.name() == QLatin1String("CPartId"))
                id = xml.attributes().value("idstr").toString();
            if (xml.name() == QLatin1String("s") && xml.attributes().value("xs.n") == QLatin1String("localName")) {
                name = xml.readElementText();
                --depth;
            }
        } else if (xml.isEndElement() && depth && --depth == 0 && !id.isEmpty()) names.insert(type + '/' + id, name);
    }
    if (error) *error = xml.hasError() ? xml.errorString() : QString();
    return names;
}
}

class Cmo3Tests : public QObject
{
    Q_OBJECT
private slots:
    void parameterNamesPreserveCdiAndDoNotRenameOtherObjectTypes_data()
    {
        QTest::addColumn<QString>("nameMode");
        QTest::addColumn<bool>("withCdi");
        for (bool withCdi : {false, true})
            for (const QString& mode : {QStringLiteral("none"), QStringLiteral("empty"), QStringLiteral("id"), QStringLiteral("custom")})
                QTest::newRow(qPrintable((withCdi ? "cdi-" : "fallback-") + mode)) << mode << withCdi;
    }

    void parameterNamesPreserveCdiAndDoNotRenameOtherObjectTypes()
    {
        QFETCH(QString, nameMode);
        QFETCH(bool, withCdi);
        const QString base = QString(TEST_RESOURCES) + "/Wanko/";
        live2dstruct::CModelDocument document;
        QVERIFY(document.Load(base + "Wanko.moc3"));
        QVERIFY(!document.Parameters().isEmpty());
        QVERIFY(!document.Drawables().isEmpty());
        QVERIFY(!document.Deformers().isEmpty());
        QVERIFY(!document.Parts().isEmpty());
        const QString id = document.Parameters().first().id;
        const QByteArray utf8 = id.toUtf8();
        QVERIFY(utf8.size() < 64);
        QByteArray moc = document.SourceBytes();
        for (int slot : {0xc4, 0x6c, 0x4c}) {
            const quint32 ids = qFromLittleEndian<quint32>(moc.constData() + slot);
            QVERIFY(ids <= quint32(moc.size() - 64));
            std::memset(moc.data() + ids, 0, 64);
            std::memcpy(moc.data() + ids, utf8.constData(), utf8.size());
        }
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QFile model(temp.filePath("Wanko.moc3"));
        QVERIFY(model.open(QIODevice::WriteOnly));
        QCOMPARE(model.write(moc), moc.size());
        model.close();
        QFile config(temp.filePath("Wanko.model3.json"));
        QVERIFY(config.open(QIODevice::WriteOnly));
        const QJsonObject modelJson{{"FileReferences", QJsonObject{{"Moc", "Wanko.moc3"},
            {"Textures", QJsonArray{base + "Wanko.1024/texture_00.png"}}}}};
        const QByteArray configBytes = QJsonDocument(modelJson).toJson();
        QCOMPARE(config.write(configBytes), configBytes.size());
        config.close();
        QFile cdi(temp.filePath("Wanko.cdi3.json"));
        QVERIFY(cdi.open(QIODevice::WriteOnly));
        const QJsonObject names{{"Version", 3},
            {"Parameters", QJsonArray{QJsonObject{{"Id", id}, {"Name", "CDI 参数"}}}},
            {"Parts", QJsonArray{QJsonObject{{"Id", id}, {"Name", "CDI 非参数对象"}}}}};
        const QByteArray cdiBytes = QJsonDocument(names).toJson();
        QCOMPARE(cdi.write(cdiBytes), cdiBytes.size());
        cdi.close();
        cmo3::CCmo3Builder::Options options;
        options.mocPath = model.fileName();
        options.outPath = temp.filePath("names.cmo3");
        options.parameterDefaultMode = cmo3::CCmo3Builder::ParameterDefaultMode::ModelDefinition;
        if (withCdi) options.cdiPath = cdi.fileName();
        auto metadata = document.ParameterMetadata(id);
        metadata.decimalPlaces = 7;
        if (nameMode == "empty") metadata.displayName.clear();
        if (nameMode == "custom") metadata.displayName = QStringLiteral("自定义参数 <literal>");
        if (nameMode != "none") options.parameterMetadata.insert(id, metadata);
        QString error;
        QVERIFY2(cmo3::CCmo3Builder::Build(options, &error), qPrintable(error));
        cmo3::CCaffArchive archive;
        QVERIFY2(archive.Load(options.outPath, &error), qPrintable(error));
        const auto* main = archive.FindByPath("main.xml");
        QVERIFY(main);
        const auto parameters = parameterFields(main->content, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        const QString expected = nameMode == "custom" ? metadata.displayName : withCdi ? QStringLiteral("CDI 参数") : id;
        QCOMPARE(parameters.value(id).value("name"), expected);
        if (nameMode != "none") QCOMPARE(parameters.value(id).value("decimalPlaces"), QStringLiteral("7"));
        const auto objects = objectDisplayNames(main->content, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        const QString deformerType = document.Deformers().first().type == live2dstruct::CModelDeformer::Warp
            ? QStringLiteral("CWarpDeformerSource") : QStringLiteral("CRotationDeformerSource");
        for (const QString& type : {QStringLiteral("CArtMeshSource"), deformerType, QStringLiteral("CPartSource")}) {
            QVERIFY2(objects.contains(type + '/' + id), qPrintable(type));
            QCOMPARE(objects.value(type + '/' + id), withCdi ? QStringLiteral("CDI 非参数对象") : id);
        }
    }

    void exportsParameterStructureFromEditedSnapshot()
    {
        QTemporaryDir directory;
        live2dstruct::CModelDocument document;
        QString error;
        const auto path = QString(TEST_RESOURCES) + "/Wanko/Wanko.moc3";
        QVERIFY2(document.Load(path, &error), qPrintable(error));
        const auto original = document.SourceBytes();
        const auto mesh = document.Drawables().front();
        using Builder = live2dstruct::CMocParameterStructureBuilder;
        Builder::CreateRequest request;
        request.parameter.id = "ParamNewAxis";
        request.bindings.append({{Builder::Owner::ArtMesh, mesh.id}, {-1, 0, 1}});
        live2dstruct::CModelEdit edit;
        QVERIFY2(document.MakeCreateParameterEdit(request, QStringLiteral("新增轴"), edit, &error), qPrintable(error));
        QVERIFY(document.ExecuteEdit(edit));
        cmo3::CCmo3Builder::Options options;
        options.mocPath = path;
        options.outPath = directory.filePath("edited.cmo3");
        options.parameterDefaultMode = cmo3::CCmo3Builder::ParameterDefaultMode::ModelDefinition;
        options.parameterMetadata = document.ParameterMetadataOverrides();
        QVERIFY2(document.BuildPreviewMoc(options.mocBytes, &error), qPrintable(error));
        QVERIFY2(cmo3::CCmo3Builder::Build(options, &error), qPrintable(error));
        cmo3::CCaffArchive archive;
        QVERIFY2(archive.Load(options.outPath, &error), qPrintable(error));
        const auto* main = archive.FindByPath("main.xml");
        QVERIFY(main);
        const auto parameters = parameterFields(main->content, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(parameters.size(), document.Parameters().size());
        QCOMPARE(parameters.value("ParamNewAxis").value("name"), QStringLiteral("新增轴"));
        int forms = 0;
        for (const auto& drawable : document.Drawables()) forms += drawable.keyforms.size();
        QXmlStreamReader xml(main->content);
        int exportedForms = 0;
        while (!xml.atEnd()) {
            xml.readNext();
            if (xml.isStartElement() && xml.name() == QLatin1String("CArtMeshForm")) ++exportedForms;
        }
        QVERIFY2(!xml.hasError(), qPrintable(xml.errorString()));
        QCOMPARE(exportedForms, forms);
        QCOMPARE(document.SourceBytes(), original);
        QFile source(path);
        QVERIFY(source.open(QIODevice::ReadOnly));
        QCOMPARE(source.readAll(), original);
    }

    void invalidEditedSnapshotCannotFallBackToOriginal()
    {
        QTemporaryDir directory;
        cmo3::CCmo3Builder::Options options;
        options.mocPath = QString(TEST_RESOURCES) + "/Wanko/Wanko.moc3";
        options.outPath = directory.filePath("invalid.cmo3");
        options.mocBytes = "broken snapshot";
        QString error;
        QVERIFY(!cmo3::CCmo3Builder::Build(options, &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(!QFileInfo::exists(options.outPath));
    }

    void explicitParameterDefaultModes_data()
    {
        QTest::addColumn<bool>("definition");
        QTest::addColumn<QString>("scenario");
        for (bool definition : {false, true})
            for (const QString& scenario : {QStringLiteral("source"), QStringLiteral("source-fallback"),
                    QStringLiteral("metadata"), QStringLiteral("metadata-fallback"), QStringLiteral("expanded"),
                    QStringLiteral("narrowed"), QStringLiteral("narrowed-fallback")})
                QTest::newRow(qPrintable((definition ? "definition-" : "current-") + scenario)) << definition << scenario;
    }

    void explicitParameterDefaultModes()
    {
        QFETCH(bool, definition);
        QFETCH(QString, scenario);
        const QString mocPath = QString(TEST_RESOURCES) + "/Wanko/Wanko.moc3";
        live2dstruct::CModelDocument document;
        QString error;
        QVERIFY2(document.Load(mocPath, &error), qPrintable(error));
        QVERIFY(document.Parameters().size() > 1);
        const auto parameter = document.Parameters().first();
        auto metadata = document.ParameterMetadata(parameter.id);
        float current = -2.5f;
        const bool overridden = scenario == "metadata" || scenario == "metadata-fallback" || scenario == "expanded" || scenario.startsWith("narrowed");
        if (overridden) metadata.defaultValue = 1.25f;
        if (scenario == "expanded") { metadata.maximum = 60; current = 45; }
        if (scenario.startsWith("narrowed")) { metadata.minimum = 10; metadata.maximum = 20; metadata.defaultValue = 12.5f; current = 15; }
        const bool hasCurrent = !scenario.endsWith("-fallback");
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        cmo3::CCmo3Builder::Options options;
        QCOMPARE(options.parameterDefaultMode, cmo3::CCmo3Builder::ParameterDefaultMode::CurrentValues);
        options.mocPath = mocPath;
        options.outPath = temp.filePath("mode.cmo3");
        options.parameterDefaultMode = definition ? cmo3::CCmo3Builder::ParameterDefaultMode::ModelDefinition
                                                : cmo3::CCmo3Builder::ParameterDefaultMode::CurrentValues;
        if (hasCurrent) options.parameterValues.insert(parameter.id, current);
        if (overridden) options.parameterMetadata.insert(parameter.id, metadata);
        const auto originalValues = options.parameterValues;
        QVERIFY2(cmo3::CCmo3Builder::Build(options, &error), qPrintable(error));
        QCOMPARE(options.parameterValues, originalValues);
        cmo3::CCaffArchive archive;
        QVERIFY2(archive.Load(options.outPath, &error), qPrintable(error));
        const auto* main = archive.FindByPath("main.xml");
        QVERIFY(main);
        const auto fields = parameterFields(main->content, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(fields.size(), document.Parameters().size());
        const float expected = definition || !hasCurrent ? metadata.defaultValue : current;
        QCOMPARE(fields.value(parameter.id).value("defaultValue"), cmo3::CXsXmlWriter::FormatFloat(expected));
        QCOMPARE(fields.value(parameter.id).value("minValue"), cmo3::CXsXmlWriter::FormatFloat(metadata.minimum));
        QCOMPARE(fields.value(parameter.id).value("maxValue"), cmo3::CXsXmlWriter::FormatFloat(metadata.maximum));
        const auto other = document.Parameters()[1];
        QCOMPARE(fields.value(other.id).value("defaultValue"), cmo3::CXsXmlWriter::FormatFloat(other.defaultValue));
        QVERIFY(!fields.value(parameter.id).contains("currentValue"));
    }

    void invalidParameterModesAndValuesPreserveExistingOutput_data()
    {
        QTest::addColumn<bool>("definition");
        QTest::addColumn<QString>("invalid");
        for (bool definition : {false, true})
            for (const QString& invalid : {QStringLiteral("mode"), QStringLiteral("unknown-value"), QStringLiteral("unknown-metadata"),
                    QStringLiteral("nan-value"), QStringLiteral("infinite-value"), QStringLiteral("outside-range"),
                    QStringLiteral("narrowed-range"), QStringLiteral("bad-default"), QStringLiteral("reversed-range"), QStringLiteral("bad-precision")})
                QTest::newRow(qPrintable((definition ? "definition-" : "current-") + invalid)) << definition << invalid;
    }

    void invalidParameterModesAndValuesPreserveExistingOutput()
    {
        QFETCH(bool, definition);
        QFETCH(QString, invalid);
        const QString mocPath = QString(TEST_RESOURCES) + "/Wanko/Wanko.moc3";
        live2dstruct::CModelDocument document;
        QVERIFY(document.Load(mocPath));
        const auto parameter = document.Parameters().first();
        auto metadata = document.ParameterMetadata(parameter.id);
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        cmo3::CCmo3Builder::Options options;
        options.mocPath = mocPath;
        options.outPath = temp.filePath("retained.cmo3");
        options.parameterDefaultMode = definition ? cmo3::CCmo3Builder::ParameterDefaultMode::ModelDefinition
                                                : cmo3::CCmo3Builder::ParameterDefaultMode::CurrentValues;
        if (invalid == "mode") options.parameterDefaultMode = static_cast<cmo3::CCmo3Builder::ParameterDefaultMode>(99);
        if (invalid == "unknown-value") options.parameterValues.insert("missing", 0);
        if (invalid == "unknown-metadata") options.parameterMetadata.insert("missing", metadata);
        if (invalid == "nan-value") options.parameterValues.insert(parameter.id, std::numeric_limits<float>::quiet_NaN());
        if (invalid == "infinite-value") options.parameterValues.insert(parameter.id, std::numeric_limits<float>::infinity());
        if (invalid == "outside-range") options.parameterValues.insert(parameter.id, parameter.maximum + 1);
        if (invalid == "narrowed-range") {
            metadata.maximum = 1;
            options.parameterMetadata.insert(parameter.id, metadata);
            options.parameterValues.insert(parameter.id, 2);
        }
        if (invalid == "bad-default") { metadata.defaultValue = metadata.maximum + 1; options.parameterMetadata.insert(parameter.id, metadata); }
        if (invalid == "reversed-range") { metadata.minimum = metadata.maximum + 1; options.parameterMetadata.insert(parameter.id, metadata); }
        if (invalid == "bad-precision") { metadata.decimalPlaces = 10; options.parameterMetadata.insert(parameter.id, metadata); }
        QFile output(options.outPath);
        QVERIFY(output.open(QIODevice::WriteOnly));
        QCOMPARE(output.write("existing artifact"), 17);
        output.close();
        QString error;
        QVERIFY(!cmo3::CCmo3Builder::Build(options, &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(output.open(QIODevice::ReadOnly));
        QCOMPARE(output.readAll(), QByteArray("existing artifact"));
    }

    void xmlFloatWritersRejectNonfiniteValues_data()
    {
        QTest::addColumn<int>("kind");
        QTest::addColumn<float>("value");
        for (int kind = 0; kind < 3; ++kind) {
            QTest::newRow(qPrintable(QStringLiteral("field-%1-nan").arg(kind))) << kind << std::numeric_limits<float>::quiet_NaN();
            QTest::newRow(qPrintable(QStringLiteral("field-%1-positive-infinity").arg(kind))) << kind << std::numeric_limits<float>::infinity();
            QTest::newRow(qPrintable(QStringLiteral("field-%1-negative-infinity").arg(kind))) << kind << -std::numeric_limits<float>::infinity();
        }
    }

    void xmlFloatWritersRejectNonfiniteValues()
    {
        QFETCH(int, kind);
        QFETCH(float, value);
        cmo3::CXsXmlWriter writer;
        writer.BeginDocument({}, {}, "1");
        writer.BeginObject("Test");
        if (kind == 0) writer.F("first-number", value);
        else if (kind == 1) writer.FloatArray("first-number", {1, value, 3});
        else writer.FloatAttribute("first-number", value);
        QVERIFY(writer.HasError());
        QVERIFY(writer.NumberError().contains("first-number"));
        const QString firstError = writer.NumberError();
        writer.F("second-number", std::numeric_limits<float>::infinity());
        QCOMPARE(writer.NumberError(), firstError);
        writer.EndObject();
        writer.EndDocument();
        QVERIFY(writer.HasError());
        QVERIFY(!writer.Result().contains("nan"));
        QVERIFY(!writer.Result().contains("inf"));
    }

    void finiteFloatWriterFormattingRemainsRoundTrippable()
    {
        const QVector<float> values{0, -0.0f, 1, -2.5f, 0.14434664f,
            std::numeric_limits<float>::max(), std::numeric_limits<float>::lowest(),
            std::numeric_limits<float>::denorm_min()};
        for (float value : values) {
            const QString formatted = cmo3::CXsXmlWriter::FormatFloat(value);
            std::istringstream input(formatted.toStdString());
            input.imbue(std::locale::classic());
            float converted = 0;
            input >> converted;
            QVERIFY(input.eof() && !input.bad());
            const bool representedSubnormal = std::fpclassify(converted) == FP_SUBNORMAL && converted == value;
            QVERIFY(!input.fail() || representedSubnormal);
            QCOMPARE(converted, value);
            cmo3::CXsXmlWriter writer;
            writer.BeginDocument({}, {}, "1");
            writer.BeginObject("Test");
            writer.FloatAttribute("attribute", value);
            writer.F("number", value);
            writer.FloatArray("array", {value});
            writer.EndObject();
            writer.EndDocument();
            QVERIFY(!writer.HasError());
            QXmlStreamReader xml(writer.Result());
            xml.setNamespaceProcessing(false);
            QString attribute, number, array;
            while (!xml.atEnd()) {
                xml.readNext();
                if (!xml.isStartElement()) continue;
                if (xml.name() == QLatin1String("Test")) attribute = xml.attributes().value("attribute").toString();
                else if (xml.name() == QLatin1String("f")) number = xml.readElementText();
                else if (xml.name() == QLatin1String("float-array")) array = xml.readElementText();
            }
            QVERIFY(!xml.hasError());
            QCOMPARE(attribute, formatted);
            QCOMPARE(number, formatted);
            QCOMPARE(array, formatted);
        }
        QCOMPARE(cmo3::CXsXmlWriter::FormatFloat(1), QStringLiteral("1.0"));
        QCOMPARE(cmo3::CXsXmlWriter::FormatFloat(-2.5f), QStringLiteral("-2.5"));
    }

    void textureExtractionUsesValidatedTopologyOverrides()
    {
        live2dstruct::CReader reader(QString(TEST_RESOURCES) + "/Wanko/Wanko.moc3");
        live2dstruct::CSectionTable table(reader);
        QVERIFY(table.IsValid());
        QVERIFY(!table.art_meshes.isEmpty());
        table.count_info->art_meshes = 1;
        auto* mesh = table.art_meshes[0];
        mesh->vertex_counts = 4;
        mesh->texture_nos = 0;
        mesh->uv_sources_begin_indices = 0;
        mesh->position_index_sources_begin_indices = 0;
        mesh->position_index_sources_counts = 6;
        mesh->keyform_sources_begin_indices = -1;
        table.uvs->uvs = {0, 0, 1, 0, 1, 1, 0, 1};
        table.position_indices->indices = {0, 1, 2, 0, 2, 3};

        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QImage atlas(16, 16, QImage::Format_ARGB32);
        atlas.fill(QColor(80, 120, 200, 255));
        const QString atlasPath = temp.filePath(QStringLiteral("atlas.png"));
        QVERIFY(atlas.save(atlasPath));
        CTextureExtractor extractor;
        QVERIFY(extractor.LoadAtlases({atlasPath}));
        const auto original = extractor.ExtractAll(&table, true);
        QCOMPARE(original.size(), 1);
        const auto opaquePixels = [](const QImage& image) {
            int count = 0;
            for (int y = 0; y < image.height(); ++y)
                for (int x = 0; x < image.width(); ++x)
                    if (qAlpha(image.pixel(x, y)) != 0) ++count;
            return count;
        };
        const int fullPixels = opaquePixels(original[0].image);
        QVERIFY(fullPixels > 0);
        QHash<QString, QVector<quint16>> overrides{{mesh->ids, {0, 1, 2}}};
        const auto edited = extractor.ExtractAll(&table, true, &overrides);
        QCOMPARE(edited.size(), 1);
        const int editedPixels = opaquePixels(edited[0].image);
        QVERIFY(editedPixels > 0 && editedPixels < fullPixels);
        QCOMPARE(edited[0].canvasRect, original[0].canvasRect);
        overrides[mesh->ids] = {2, 1, 0};
        QCOMPARE(extractor.ExtractAll(&table, true, &overrides)[0].image, edited[0].image);
        for (const auto& invalid : {QVector<quint16>{}, QVector<quint16>{0, 1},
                                    QVector<quint16>{0, 1, 4}, QVector<quint16>{0, 1, 32768}}) {
            overrides[mesh->ids] = invalid;
            const auto fallback = extractor.ExtractAll(&table, true, &overrides);
            QCOMPARE(fallback.size(), 1);
            QCOMPARE(fallback[0].image, original[0].image);
        }
        QCOMPARE(extractor.ExtractAll(&table, true)[0].image, original[0].image);
    }

    void sparseKeyformOverridesExportAllForms()
    {
        const QString mocPath = QString(TEST_RESOURCES) + "/Mark/Mark.moc3";
        live2dstruct::CModelDocument document;
        QString error;
        QVERIFY2(document.Load(mocPath, &error), qPrintable(error));
        const auto mesh = std::find_if(document.Drawables().cbegin(), document.Drawables().cend(),
            [](const auto& drawable) { return drawable.vertexCount > 0 && drawable.keyforms.size() >= 3; });
        QVERIFY(mesh != document.Drawables().cend());
        const int editedIndex = mesh->keyforms.size() / 2;
        auto edited = mesh->keyforms[editedIndex].positions;
        QVERIFY(!edited.isEmpty());
        edited[0] += 0.25f;
        QVERIFY(document.SetDrawableKeyformPositions(mesh->id, editedIndex, edited));
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        cmo3::CCmo3Builder::Options options;
        options.mocPath = mocPath;
        options.outPath = temp.filePath("keyforms.cmo3");
        options.drawableKeyformPositionOverrides = document.DrawableKeyformPositionOverrides();
        QVERIFY2(cmo3::CCmo3Builder::Build(options, &error), qPrintable(error));
        cmo3::CCaffArchive archive;
        QVERIFY2(archive.Load(options.outPath, &error), qPrintable(error));
        const auto* main = archive.FindByPath("main.xml");
        QVERIFY(main);
        QXmlStreamReader xml(main->content);
        xml.setNamespaceProcessing(false);
        int depth = 0;
        int meshDepth = -1;
        int formDepth = -1;
        QString meshId;
        QStringList exportedPositions;
        while (!xml.atEnd()) {
            xml.readNext();
            if (xml.isStartElement()) {
                ++depth;
                if (xml.name() == QLatin1String("CArtMeshSource") && xml.attributes().hasAttribute("xs.id")) {
                    meshDepth = depth;
                    meshId.clear();
                }
                if (meshDepth < 0) continue;
                if (xml.name() == QLatin1String("CDrawableId") && xml.attributes().value("xs.n") == QLatin1String("id"))
                    meshId = xml.attributes().value("idstr").toString();
                if (xml.name() == QLatin1String("CArtMeshForm")) formDepth = depth;
                if (meshId == mesh->id && formDepth > 0 && depth == formDepth + 1 &&
                    xml.name() == QLatin1String("float-array") &&
                    xml.attributes().value("xs.n") == QLatin1String("positions")) {
                    exportedPositions.append(xml.readElementText());
                    --depth;
                }
            } else if (xml.isEndElement()) {
                if (depth == formDepth) formDepth = -1;
                if (depth == meshDepth) meshDepth = -1;
                --depth;
            }
        }
        QVERIFY2(!xml.hasError(), qPrintable(xml.errorString()));
        QCOMPARE(exportedPositions.size(), mesh->keyforms.size());
        live2dstruct::CReader reader(mocPath);
        live2dstruct::CSectionTable table(reader);
        QVERIFY(table.IsValid());
        for (int index = 0; index < mesh->keyforms.size(); ++index) {
            auto positions = index == editedIndex ? edited : mesh->keyforms[index].positions;
            QCOMPARE(document.DrawableKeyformPositions(mesh->id, index), positions);
            if (mesh->parentDeformerIndex < 0) {
                for (int vertex = 0; vertex < positions.size(); vertex += 2) {
                    positions[vertex] = static_cast<float>(double(positions[vertex]) * table.canvas_info->pixels_per_unit + table.canvas_info->origin_x);
                    positions[vertex + 1] = static_cast<float>(table.canvas_info->origin_y + double(positions[vertex + 1]) * table.canvas_info->pixels_per_unit);
                }
            }
            QStringList expected;
            for (float position : positions) expected.append(cmo3::CXsXmlWriter::FormatFloat(position));
            QCOMPARE(exportedPositions[index], expected.join(QLatin1Char(' ')));
        }
    }

    void exportsUserDataByMeshId()
    {
        QTemporaryDir temp;
        live2dstruct::CModelUserData data;
        QString error;
        QVERIFY(data.Load(QString(TEST_RESOURCES) + "/Mark/Mark.userdata3.json", &error));
        auto entries = data.Entries();
        entries[0].value = QStringLiteral("头发 🌸 <hair> & \"tag\"\n第二行\t尾部");
        entries[1].value.clear();
        data.SetEntries(entries);
        cmo3::CCmo3Builder::Options options;
        options.mocPath = QString(TEST_RESOURCES) + "/Mark/Mark.moc3";
        options.userDataPath = temp.filePath("edited.userdata3.json");
        options.outPath = temp.filePath("edited.cmo3");
        live2dstruct::CModelDocument model;
        QVERIFY(model.Load(options.mocPath, &error));
        for (bool cleared : {false, true}) {
            if (cleared) data.SetEntries({});
            QVERIFY(data.Save(options.userDataPath, &error));
            QVERIFY2(cmo3::CCmo3Builder::Build(options, &error), qPrintable(error));
            cmo3::CCaffArchive archive;
            QVERIFY2(archive.Load(options.outPath, &error), qPrintable(error));
            const auto* main = archive.FindByPath("main.xml");
            QVERIFY(main);
            QMap<QString, QString> tags;
            QVERIFY2(readMeshUserData(main->content, &tags, &error), qPrintable(error));
            QCOMPARE(tags.size(), model.Drawables().size());
            QMap<QString, QString> expected;
            for (const auto& drawable : model.Drawables()) expected.insert(drawable.id, QString());
            for (const auto& entry : data.Entries()) expected[entry.id] = entry.value;
            QCOMPARE(tags, expected);
        }
    }

    void rejectsInvalidUserDataBeforeReplacingOutput_data()
    {
        QTest::addColumn<QString>("kind");
        for (const QString& kind : {"missing", "invalid-json", "unknown-id", "duplicate", "invalid-xml", "invalid-xml-name"})
            QTest::newRow(qPrintable(kind)) << kind;
    }

    void rejectsInvalidUserDataBeforeReplacingOutput()
    {
        QFETCH(QString, kind);
        QTemporaryDir temp;
        cmo3::CCmo3Builder::Options options;
        options.mocPath = QString(TEST_RESOURCES) + "/Mark/Mark.moc3";
        options.outPath = temp.filePath("existing.cmo3");
        options.userDataPath = temp.filePath("labels.userdata3.json");
        if (kind == QLatin1String("invalid-xml-name")) options.modelName = QString(QChar(1));
        if (kind != QLatin1String("missing")) {
            QByteArray bytes = "{broken";
            if (kind != QLatin1String("invalid-json")) {
                live2dstruct::CModelUserData data;
                QString id = kind == QLatin1String("unknown-id") ? "MissingMesh" : "Hair1";
                QString value = kind == QLatin1String("invalid-xml") ? QString(QChar(1)) : "tag";
                data.SetEntries({{"ArtMesh", id, value, {}}});
                if (kind == QLatin1String("duplicate")) data.SetEntries({data.Entries().first(), data.Entries().first()});
                bytes = data.ToJson();
            }
            QFile source(options.userDataPath);
            QVERIFY(source.open(QIODevice::WriteOnly));
            QCOMPARE(source.write(bytes), bytes.size());
        }
        QFile target(options.outPath);
        QVERIFY(target.open(QIODevice::WriteOnly));
        QCOMPARE(target.write("previous-project"), 16);
        target.close();
        QString error;
        QVERIFY(!cmo3::CCmo3Builder::Build(options, &error));
        QVERIFY2(!error.isEmpty(), qPrintable(kind));
        QVERIFY(target.open(QIODevice::ReadOnly));
        QCOMPARE(target.readAll(), QByteArray("previous-project"));
    }

    void buildUsesPhysicsDisplayNames()
    {
        QFile source(QString(TEST_RESOURCES) + "/Wanko/Wanko.physics3.json");
        QVERIFY(source.open(QIODevice::ReadOnly));
        QJsonObject root = QJsonDocument::fromJson(source.readAll()).object();
        QJsonArray settings = root.value("PhysicsSettings").toArray();
        QVERIFY(!settings.isEmpty());
        const QString id = settings.first().toObject().value("Id").toString();
        QJsonObject fallback = settings.first().toObject();
        fallback.insert("Id", "UnmappedGroup");
        settings = QJsonArray{settings.first(), fallback};
        root.insert("PhysicsSettings", settings);
        QJsonObject meta = root.value("Meta").toObject();
        meta.insert("PhysicsDictionary", QJsonArray{QJsonObject{{"Id", id}, {"Name", QStringLiteral("发梢 & 耳朵")}}});
        root.insert("Meta", meta);
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QFile edited(temp.filePath("edited.physics3.json"));
        QVERIFY(edited.open(QIODevice::WriteOnly));
        const QByteArray bytes = QJsonDocument(root).toJson();
        QCOMPARE(edited.write(bytes), bytes.size());
        edited.close();
        cmo3::CCmo3Builder::Options options;
        options.mocPath = QString(TEST_RESOURCES) + "/Wanko/Wanko.moc3";
        options.physicsPath = edited.fileName();
        options.outPath = temp.filePath("named.cmo3");
        QString error;
        QVERIFY2(cmo3::CCmo3Builder::Build(options, &error), qPrintable(error));
        cmo3::CCaffArchive archive;
        QVERIFY2(archive.Load(options.outPath, &error), qPrintable(error));
        const auto* entry = archive.FindByPath("main.xml");
        QVERIFY(entry);
        QXmlStreamReader xml(entry->content);
        QStringList names;
        QStringList ids;
        bool inPhysicsGroup = false;
        while (!xml.atEnd()) {
            xml.readNext();
            if (xml.isStartElement() && xml.name() == QLatin1String("CPhysicsSettingsSource")) inPhysicsGroup = true;
            if (xml.isEndElement() && xml.name() == QLatin1String("CPhysicsSettingsSource")) inPhysicsGroup = false;
            if (!inPhysicsGroup || !xml.isStartElement()) continue;
            if (xml.name() == QLatin1String("s") && xml.attributes().value("xs.n") == QLatin1String("name"))
                names.append(xml.readElementText());
            if (xml.name() == QLatin1String("CPhysicsSettingId")) ids.append(xml.attributes().value("idstr").toString());
        }
        QVERIFY2(!xml.hasError(), qPrintable(xml.errorString()));
        QCOMPARE(names, (QStringList{QStringLiteral("发梢 & 耳朵"), "UnmappedGroup"}));
        QCOMPARE(ids, (QStringList{id, "UnmappedGroup"}));
    }

    void matchesTextureConfigToMoc()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QDir source(QString(TEST_RESOURCES) + "/Wanko");
        const QDir destination(temp.path());
        QVERIFY(QFile::copy(source.filePath("Wanko.moc3"), destination.filePath("Wanko.moc3")));
        const QString modelJson = source.filePath("Wanko.model3.json");
        QFile input(modelJson);
        QVERIFY(input.open(QIODevice::ReadOnly));
        const QJsonObject root = QJsonDocument::fromJson(input.readAll()).object();
        input.close();
        QVERIFY(QFile::copy(modelJson, destination.filePath("Wanko.model3.json")));
        for (const QJsonValue& texture : root.value("FileReferences").toObject().value("Textures").toArray()) {
            QVERIFY(destination.mkpath(QFileInfo(texture.toString()).path()));
            QVERIFY(QFile::copy(source.filePath(texture.toString()), destination.filePath(texture.toString())));
        }
        QFile unrelated(destination.filePath("A-other.model3.json"));
        QVERIFY(unrelated.open(QIODevice::WriteOnly));
        QVERIFY(unrelated.write(R"({"FileReferences":{"Moc":"Other.moc3","Textures":["missing.png"]}})") > 0);
        unrelated.close();

        cmo3::CCmo3Builder::Options options;
        options.mocPath = destination.filePath("Wanko.moc3");
        options.outPath = destination.filePath("result.cmo3");
        QString error;
        QVERIFY2(cmo3::CCmo3Builder::Build(options, &error), qPrintable(error));
        QVERIFY(QFileInfo::exists(options.outPath));
    }

    void physicsByteSnapshotTakesPrecedence_data()
    {
        QTest::addColumn<int>("pathKind");
        QTest::newRow("existing-source") << 0;
        QTest::newRow("missing-source") << 1;
        QTest::newRow("no-source") << 2;
        QTest::newRow("invalid-source") << 3;
    }

    void physicsByteSnapshotTakesPrecedence()
    {
        QFETCH(int, pathKind);
        QFile source(QString(TEST_RESOURCES) + "/Wanko/Wanko.physics3.json");
        QVERIFY(source.open(QIODevice::ReadOnly));
        const auto original = source.readAll();
        auto root = QJsonDocument::fromJson(original).object();
        auto group = root.value("PhysicsSettings").toArray().first().toObject();
        group["Id"] = QStringLiteral("MemoryOnly");
        group["Input"] = QJsonArray{};
        group["Output"] = QJsonArray{};
        root["PhysicsSettings"] = QJsonArray{group};
        const auto snapshot = QJsonDocument(root).toJson();
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        cmo3::CCmo3Builder::Options options;
        options.mocPath = QString(TEST_RESOURCES) + "/Wanko/Wanko.moc3";
        options.outPath = temporary.filePath("snapshot.cmo3");
        if (pathKind == 0) options.physicsPath = source.fileName();
        if (pathKind == 1) options.physicsPath = temporary.filePath("missing.physics3.json");
        if (pathKind == 3) {
            options.physicsPath = temporary.filePath("invalid.physics3.json");
            QFile invalid(options.physicsPath); QVERIFY(invalid.open(QIODevice::WriteOnly));
            QCOMPARE(invalid.write("{invalid"), qint64(8));
        }
        options.physicsBytes = snapshot;
        QString error;
        QVERIFY2(cmo3::CCmo3Builder::Build(options, &error), qPrintable(error));
        cmo3::CCaffArchive archive;
        QVERIFY2(archive.Load(options.outPath, &error), qPrintable(error));
        const auto* main = archive.FindByPath("main.xml"); QVERIFY(main);
        QXmlStreamReader xml(main->content);
        QStringList ids;
        int inputs = 0, outputs = 0;
        while (!xml.atEnd()) {
            xml.readNext();
            if (!xml.isStartElement()) continue;
            if (xml.name() == QLatin1String("CPhysicsSettingId")) ids.append(xml.attributes().value("idstr").toString());
            if (xml.name() == QLatin1String("CPhysicsInput")) ++inputs;
            if (xml.name() == QLatin1String("CPhysicsOutput")) ++outputs;
        }
        QVERIFY2(!xml.hasError(), qPrintable(xml.errorString()));
        QCOMPARE(ids, QStringList{"MemoryOnly"});
        QCOMPARE(inputs, 0); QCOMPARE(outputs, 0);
        QCOMPARE(*options.physicsBytes, snapshot);
        QVERIFY(source.seek(0)); QCOMPARE(source.readAll(), original);
    }

    void invalidPhysicsByteSnapshotsPreserveDestination_data()
    {
        QTest::addColumn<int>("kind");
        QTest::newRow("explicit-empty") << 0;
        QTest::newRow("broken-json") << 1;
        QTest::newRow("missing-settings") << 2;
        QTest::newRow("unknown-parameter") << 3;
        QTest::newRow("invalid-vertex") << 4;
        QTest::newRow("wrong-target") << 5;
    }

    void invalidPhysicsByteSnapshotsPreserveDestination()
    {
        QFETCH(int, kind);
        QFile source(QString(TEST_RESOURCES) + "/Wanko/Wanko.physics3.json");
        QVERIFY(source.open(QIODevice::ReadOnly));
        auto root = QJsonDocument::fromJson(source.readAll()).object();
        QByteArray bytes;
        if (kind == 1) bytes = "{broken";
        if (kind == 2) bytes = "{}";
        if (kind >= 3) {
            auto settings = root.value("PhysicsSettings").toArray();
            auto group = settings.first().toObject();
            if (kind == 4) {
                auto outputs = group.value("Output").toArray();
                auto output = outputs.first().toObject(); output["VertexIndex"] = 1000000;
                outputs[0] = output; group["Output"] = outputs;
            } else {
                auto inputs = group.value("Input").toArray();
                auto input = inputs.first().toObject(); auto reference = input.value("Source").toObject();
                reference[kind == 3 ? "Id" : "Target"] = kind == 3 ? "MissingParameter" : "Part";
                input["Source"] = reference; inputs[0] = input; group["Input"] = inputs;
            }
            settings[0] = group; root["PhysicsSettings"] = settings;
            bytes = QJsonDocument(root).toJson();
        }
        QTemporaryDir temporary; QVERIFY(temporary.isValid());
        cmo3::CCmo3Builder::Options options;
        options.mocPath = QString(TEST_RESOURCES) + "/Wanko/Wanko.moc3";
        options.physicsPath = source.fileName();
        options.physicsBytes = bytes;
        options.outPath = temporary.filePath("preserved.cmo3");
        QFile destination(options.outPath); QVERIFY(destination.open(QIODevice::WriteOnly));
        QCOMPARE(destination.write("preserved-output"), qint64(16)); destination.close();
        QString error;
        QVERIFY(!cmo3::CCmo3Builder::Build(options, &error));
        QVERIFY2(error.contains("physics3"), qPrintable(error));
        QVERIFY(destination.open(QIODevice::ReadOnly));
        QCOMPARE(destination.readAll(), QByteArray("preserved-output"));
    }

    void rejectsInvalidPhysicsJson()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString physicsPath = temp.filePath(QStringLiteral("broken.physics3.json"));
        QFile file(physicsPath);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QVERIFY(file.write("{broken") > 0);
        file.close();

        cmo3::CCmo3Builder::Options options;
        options.mocPath = QString(TEST_RESOURCES) + "/Wanko/Wanko.moc3";
        options.outPath = temp.filePath(QStringLiteral("broken.cmo3"));
        options.physicsPath = physicsPath;
        QString error;
        QVERIFY(!cmo3::CCmo3Builder::Build(options, &error));
        QVERIFY(error.contains(QStringLiteral("physics3 JSON")));
        QVERIFY(!QFileInfo::exists(options.outPath));
    }

    void rejectsInvalidPhysicsStructure()
    {
        QFile source(QString(TEST_RESOURCES) + "/Wanko/Wanko.physics3.json");
        QVERIFY(source.open(QIODevice::ReadOnly));
        const QJsonObject original = QJsonDocument::fromJson(source.readAll()).object();
        for (const QString& kind : {QStringLiteral("vertex"), QStringLiteral("duplicate"),
                                   QStringLiteral("missing-input"), QStringLiteral("missing-output"),
                                   QStringLiteral("target-input"), QStringLiteral("target-output")}) {
            QTemporaryDir temp;
            QVERIFY(temp.isValid());
            QJsonObject root = original;
            QJsonArray settings = root.value(QStringLiteral("PhysicsSettings")).toArray();
            QJsonObject first = settings.first().toObject();
            if (kind == QStringLiteral("vertex")) {
                QJsonArray outputs = first.value(QStringLiteral("Output")).toArray();
                QJsonObject output = outputs.first().toObject();
                output.insert(QStringLiteral("VertexIndex"), 99999);
                outputs[0] = output;
                first.insert(QStringLiteral("Output"), outputs);
            } else if (kind.startsWith(QLatin1String("missing-")) || kind.startsWith(QLatin1String("target-"))) {
                const bool input = kind.endsWith(QLatin1String("input"));
                const QString field = input ? QStringLiteral("Input") : QStringLiteral("Output");
                const QString target = input ? QStringLiteral("Source") : QStringLiteral("Destination");
                QJsonArray entries = first.value(field).toArray();
                QJsonObject entry = entries.first().toObject();
                auto reference = entry.value(target).toObject();
                if (kind.startsWith(QLatin1String("missing-"))) reference.insert("Id", "MissingParameter");
                else reference.insert("Target", "Part");
                entry.insert(target, reference);
                entries[0] = entry;
                first.insert(field, entries);
            } else {
                settings.append(first);
            }
            settings[0] = first;
            root.insert(QStringLiteral("PhysicsSettings"), settings);
            const QString physicsPath = temp.filePath(kind + QStringLiteral(".physics3.json"));
            QFile file(physicsPath);
            QVERIFY(file.open(QIODevice::WriteOnly));
            const QByteArray data = QJsonDocument(root).toJson();
            QCOMPARE(file.write(data), data.size());
            file.close();
            cmo3::CCmo3Builder::Options options;
            options.mocPath = QString(TEST_RESOURCES) + "/Wanko/Wanko.moc3";
            options.physicsPath = physicsPath;
            options.outPath = temp.filePath(kind + QStringLiteral(".cmo3"));
            QString error;
            QVERIFY(!cmo3::CCmo3Builder::Build(options, &error));
            QVERIFY(error.contains(QStringLiteral("physics3")));
            QVERIFY(!QFileInfo::exists(options.outPath));
        }
    }

    void buildAndReadArchive()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        cmo3::CCmo3Builder::Options options;
        options.mocPath = QString(TEST_RESOURCES) + "/Wanko/Wanko.moc3";
        options.outPath = temp.filePath(QStringLiteral("Wanko.cmo3"));
        options.cdiPath = QString(TEST_RESOURCES) + "/Wanko/Wanko.cdi3.json";
        options.physicsPath = QString(TEST_RESOURCES) + "/Wanko/Wanko.physics3.json";
        QString error;
        QVERIFY2(cmo3::CCmo3Builder::Build(options, &error), qPrintable(error));
        QVERIFY(QFileInfo::exists(options.outPath));

        cmo3::CCaffArchive archive;
        QVERIFY2(archive.Load(options.outPath, &error), qPrintable(error));
        const cmo3::CCaffArchive::Entry* mainXml = archive.FindByPath(QStringLiteral("main.xml"));
        QVERIFY(mainXml);
        QVERIFY(mainXml->content.contains("CModel"));
        int pngCount = 0;
        for (const auto& entry : archive.Entries()) if (entry.path.endsWith(QStringLiteral(".png"))) ++pngCount;
        QVERIFY(pngCount > 0);
    }

    void buildUsesKeyformOverride()
    {
        const QString mocPath = QString(TEST_RESOURCES) + "/Wanko/Wanko.moc3";
        live2dstruct::CModelDocument document;
        QString error;
        QVERIFY2(document.Load(mocPath, &error), qPrintable(error));
        const auto meshIt = std::find_if(document.Drawables().cbegin(), document.Drawables().cend(),
                                         [](const live2dstruct::CModelDrawable& mesh){ return !mesh.keyforms.isEmpty(); });
        QVERIFY(meshIt != document.Drawables().cend());
        const QString meshId = meshIt->id;
        const QVector<float> original = document.DrawableKeyformPositions(meshId, 0);
        QVERIFY(!original.isEmpty());
        QVector<float> edited = original;

        live2dstruct::CReader reader(mocPath);
        live2dstruct::CSectionTable table(reader);
        QVERIFY(table.IsValid());
        float expected = 123.456f;
        if (meshIt->parentDeformerIndex < 0) {
            expected = static_cast<float>(double(123.456f) * table.canvas_info->pixels_per_unit + table.canvas_info->origin_x);
            edited[0] = 123.456f;
        } else {
            edited[0] = expected;
        }
        QVERIFY(document.SetDrawableKeyformPositions(meshId, 0, edited));

        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        cmo3::CCmo3Builder::Options options;
        options.mocPath = mocPath;
        options.outPath = temp.filePath(QStringLiteral("Wanko-override.cmo3"));
        options.drawableKeyformPositionOverrides = document.DrawableKeyformPositionOverrides();
        QVERIFY2(cmo3::CCmo3Builder::Build(options, &error), qPrintable(error));
        cmo3::CCaffArchive archive;
        QVERIFY2(archive.Load(options.outPath, &error), qPrintable(error));
        const auto* mainXml = archive.FindByPath(QStringLiteral("main.xml"));
        QVERIFY(mainXml);
        QVERIFY(mainXml->content.contains(cmo3::CXsXmlWriter::FormatFloat(expected).toUtf8()));
    }

    void buildUsesParameterOverride()
    {
        const QString mocPath = QString(TEST_RESOURCES) + "/Wanko/Wanko.moc3";
        live2dstruct::CModelDocument document;
        QString error;
        QVERIFY2(document.Load(mocPath, &error), qPrintable(error));
        QVERIFY(!document.Parameters().isEmpty());
        const auto& parameter = document.Parameters().first();
        float value = parameter.minimum;
        if (qFuzzyCompare(value + 1.0f, parameter.defaultValue + 1.0f)) value = parameter.maximum;
        QVERIFY(value >= parameter.minimum && value <= parameter.maximum);

        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        cmo3::CCmo3Builder::Options options;
        options.mocPath = mocPath;
        options.outPath = temp.filePath(QStringLiteral("Wanko-parameter.cmo3"));
        options.parameterValues.insert(parameter.id, value);
        QVERIFY2(cmo3::CCmo3Builder::Build(options, &error), qPrintable(error));
        cmo3::CCaffArchive archive;
        QVERIFY2(archive.Load(options.outPath, &error), qPrintable(error));
        const auto* mainXml = archive.FindByPath(QStringLiteral("main.xml"));
        QVERIFY(mainXml);
        QVERIFY(mainXml->content.contains(cmo3::CXsXmlWriter::FormatFloat(value).toUtf8()));
    }

    void buildUsesParameterMetadataOverride()
    {
        const QString mocPath = QString(TEST_RESOURCES) + "/Wanko/Wanko.moc3";
        live2dstruct::CModelDocument document;
        QString error;
        QVERIFY2(document.Load(mocPath, &error), qPrintable(error));
        QVERIFY(!document.Parameters().isEmpty());
        const auto& parameter = document.Parameters().first();
        auto metadata = document.ParameterMetadata(parameter.id);
        metadata.displayName = QStringLiteral("测试参数");
        QVERIFY(document.SetParameterMetadata(parameter.id, metadata));

        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        cmo3::CCmo3Builder::Options options;
        options.mocPath = mocPath;
        options.outPath = temp.filePath(QStringLiteral("Wanko-metadata.cmo3"));
        options.parameterMetadata = document.ParameterMetadataOverrides();
        QVERIFY2(cmo3::CCmo3Builder::Build(options, &error), qPrintable(error));
        cmo3::CCaffArchive archive;
        QVERIFY2(archive.Load(options.outPath, &error), qPrintable(error));
        const auto* mainXml = archive.FindByPath(QStringLiteral("main.xml"));
        QVERIFY(mainXml);
        QVERIFY(mainXml->content.contains(QStringLiteral("测试参数").toUtf8()));
    }

    void buildUsesRotationKeyformOverride()
    {
        const QString mocPath = QString(TEST_RESOURCES) + "/Mark/Mark.moc3";
        live2dstruct::CModelDocument document;
        QString error;
        QVERIFY2(document.Load(mocPath, &error), qPrintable(error));
        const auto deformerIt = std::find_if(document.Deformers().cbegin(), document.Deformers().cend(),
            [](const live2dstruct::CModelDeformer& deformer){
                return deformer.type == live2dstruct::CModelDeformer::Rotation && !deformer.rotationKeyforms.isEmpty();
            });
        QVERIFY(deformerIt != document.Deformers().cend());
        QVector<live2dstruct::CModelRotationKeyform> keyforms = document.DeformerRotationKeyforms(deformerIt->id);
        keyforms[0].angle += 7.5f;
        QVERIFY(document.SetDeformerRotationKeyforms(deformerIt->id, keyforms));

        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        cmo3::CCmo3Builder::Options options;
        options.mocPath = mocPath;
        options.outPath = temp.filePath(QStringLiteral("Mark-rotation.cmo3"));
        options.deformerRotationKeyformOverrides = document.DeformerRotationKeyformOverrides();
        QVERIFY2(cmo3::CCmo3Builder::Build(options, &error), qPrintable(error));
        cmo3::CCaffArchive archive;
        QVERIFY2(archive.Load(options.outPath, &error), qPrintable(error));
        const auto* mainXml = archive.FindByPath(QStringLiteral("main.xml"));
        QVERIFY(mainXml);
        QVERIFY(mainXml->content.contains(cmo3::CXsXmlWriter::FormatFloat(keyforms[0].angle).toUtf8()));
    }

    void buildUsesWarpKeyformOverride()
    {
        const QString mocPath = QString(TEST_RESOURCES) + "/Mark/Mark.moc3";
        live2dstruct::CModelDocument document;
        QString error;
        QVERIFY2(document.Load(mocPath, &error), qPrintable(error));
        const auto deformerIt = std::find_if(document.Deformers().cbegin(), document.Deformers().cend(),
            [](const live2dstruct::CModelDeformer& deformer){
                return deformer.type == live2dstruct::CModelDeformer::Warp &&
                       deformer.parentDeformerIndex >= 0 && !deformer.warpKeyforms.isEmpty() &&
                       !deformer.warpKeyforms.first().positions.isEmpty();
            });
        QVERIFY(deformerIt != document.Deformers().cend());
        QVector<live2dstruct::CModelWarpKeyform> keyforms = document.DeformerWarpKeyforms(deformerIt->id);
        keyforms[0].positions[0] += 4.25f;
        QVERIFY(document.SetDeformerWarpKeyforms(deformerIt->id, keyforms));

        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        cmo3::CCmo3Builder::Options options;
        options.mocPath = mocPath;
        options.outPath = temp.filePath(QStringLiteral("Mark-warp.cmo3"));
        options.deformerWarpKeyformOverrides = document.DeformerWarpKeyformOverrides();
        QVERIFY2(cmo3::CCmo3Builder::Build(options, &error), qPrintable(error));
        cmo3::CCaffArchive archive;
        QVERIFY2(archive.Load(options.outPath, &error), qPrintable(error));
        const auto* mainXml = archive.FindByPath(QStringLiteral("main.xml"));
        QVERIFY(mainXml);
        QVERIFY(mainXml->content.contains(cmo3::CXsXmlWriter::FormatFloat(keyforms[0].positions[0]).toUtf8()));
    }

    void buildUsesIndexOverride()
    {
        const QString mocPath = QString(TEST_RESOURCES) + "/Wanko/Wanko.moc3";
        live2dstruct::CModelDocument document;
        QString error;
        QVERIFY2(document.Load(mocPath, &error), qPrintable(error));
        const auto meshIt = std::find_if(document.Drawables().cbegin(), document.Drawables().cend(),
                                         [](const live2dstruct::CModelDrawable& mesh){ return mesh.indices.size() >= 3; });
        QVERIFY(meshIt != document.Drawables().cend());
        const QString meshId = meshIt->id;
        QVector<quint16> indices = meshIt->indices;
        std::swap(indices[0], indices[1]);
        QVERIFY(document.SetDrawableIndices(meshId, indices));

        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        cmo3::CCmo3Builder::Options options;
        options.mocPath = mocPath;
        options.outPath = temp.filePath(QStringLiteral("Wanko-indices.cmo3"));
        options.drawableIndexOverrides = document.DrawableIndexOverrides();
        QVERIFY2(cmo3::CCmo3Builder::Build(options, &error), qPrintable(error));
        cmo3::CCaffArchive archive;
        QVERIFY2(archive.Load(options.outPath, &error), qPrintable(error));
        const auto* mainXml = archive.FindByPath(QStringLiteral("main.xml"));
        QVERIFY(mainXml);
        QVERIFY(mainXml->content.contains(QStringLiteral("<int-array xs.n=\"indices\"").toUtf8()));
        const QByteArray expectedPrefix = QStringLiteral(">%1 %2 %3 ").arg(indices[0]).arg(indices[1]).arg(indices[2]).toUtf8();
        QVERIFY(mainXml->content.contains(expectedPrefix));
    }

    void buildUsesMaskOverride()
    {
        const QString mocPath = QString(TEST_RESOURCES) + "/Wanko/Wanko.moc3";
        live2dstruct::CModelDocument document;
        QString error;
        QVERIFY2(document.Load(mocPath, &error), qPrintable(error));
        QVERIFY(document.Drawables().size() > 1);
        const QString meshId = document.Drawables().at(1).id;
        QVERIFY(document.SetDrawableMaskIndices(meshId, {0}));
        QVERIFY(document.SetDrawableMaskInverted(meshId, true));

        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        cmo3::CCmo3Builder::Options options;
        options.mocPath = mocPath;
        options.outPath = temp.filePath(QStringLiteral("Wanko-masks.cmo3"));
        options.drawableMaskIndexOverrides = document.DrawableMaskIndexOverrides();
        options.drawableMaskInverted = document.DrawableMaskInvertedOverrides();
        QVERIFY2(cmo3::CCmo3Builder::Build(options, &error), qPrintable(error));
        cmo3::CCaffArchive archive;
        QVERIFY2(archive.Load(options.outPath, &error), qPrintable(error));
        const auto* mainXml = archive.FindByPath(QStringLiteral("main.xml"));
        QVERIFY(mainXml);
        QVERIFY(mainXml->content.contains(QStringLiteral("clipGuidList").toUtf8()));
    }

    void buildUsesDeformerParentOverride()
    {
        const QString mocPath = QString(TEST_RESOURCES) + "/Mark/Mark.moc3";
        live2dstruct::CModelDocument document;
        QString error;
        QVERIFY2(document.Load(mocPath, &error), qPrintable(error));
        int child = -1;
        int parent = -1;
        for (int i = 0; i < document.Deformers().size(); ++i) {
            for (int j = 0; j < document.Deformers().size(); ++j) {
                if (i != j && document.Deformers()[i].parentPartIndex == document.Deformers()[j].parentPartIndex &&
                    document.DeformerParentIndex(document.Deformers()[i].id) != j) {
                    child = i;
                    parent = j;
                    break;
                }
            }
            if (child >= 0) break;
        }
        QVERIFY(child >= 0);
        QVERIFY(document.SetDeformerParentIndex(document.Deformers()[child].id, parent));
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        cmo3::CCmo3Builder::Options options;
        options.mocPath = mocPath;
        options.outPath = temp.filePath(QStringLiteral("Mark-deformer-parent.cmo3"));
        options.deformerParentOverrides = document.DeformerParentOverrides();
        QVERIFY2(cmo3::CCmo3Builder::Build(options, &error), qPrintable(error));
        QVERIFY(QFileInfo::exists(options.outPath));
    }
};

QTEST_GUILESS_MAIN(Cmo3Tests)
#include "cmo3_tests.moc"
