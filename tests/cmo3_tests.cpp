#include "cmo3/ccaffarchive.h"
#include "cmo3/ccmo3builder.h"
#include "cmo3/cxsxmlwriter.h"
#include "live2dstruct/cmodeldocument.h"
#include "live2dstruct/csectiontable.h"
#include "live2dstruct/util/cfilereader.h"
#include <QtTest>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <algorithm>

class Cmo3Tests : public QObject
{
    Q_OBJECT
private slots:
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
            expected = (123.456f + table.canvas_info->origin_x) * table.canvas_info->pixels_per_unit;
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
