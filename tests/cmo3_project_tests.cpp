#include "cmo3/ccmo3project.h"
#include "cmo3/ccmo3xml_p.h"
#include "cmo3/ccaffarchive.h"
#include "cmo3/ccmo3artworkreader.h"
#include "cmo3/ccmo3builder.h"
#include "std/cexportcontrol.h"
#include <QBuffer>
#include <QCryptographicHash>
#include <QFile>
#include <QImage>
#include <QLineF>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QtTest>

namespace {
QByteArray emptyProject()
{
    return QByteArrayLiteral("<root fileFormatVersion=\"500000000\"><shared><CPartSource xs.id=\"root-part\">"
        "<ACParameterControllableSource xs.n=\"super\"><s xs.n=\"localName\">Root</s><null xs.n=\"parentGuid\"/>"
        "</ACParameterControllableSource><CPartId xs.n=\"id\" idstr=\"__RootPart__\"/>"
        "<CPartGuid xs.n=\"guid\" uuid=\"root-part-guid\"/></CPartSource></shared><main><CModelSource>"
        "<CImageCanvas xs.n=\"canvas\"><i xs.n=\"pixelWidth\">20</i><i xs.n=\"pixelHeight\">30</i></CImageCanvas>"
        "<CPartSource xs.n=\"rootPart\" xs.ref=\"root-part\"/><CPartSourceSet xs.n=\"partSourceSet\">"
        "<carray_list xs.n=\"_sources\" count=\"1\"><CPartSource xs.ref=\"root-part\"/></carray_list></CPartSourceSet>"
        "<CDrawableSourceSet xs.n=\"drawableSourceSet\"><carray_list xs.n=\"_sources\" count=\"0\"/></CDrawableSourceSet>"
        "<CDeformerSourceSet xs.n=\"deformerSourceSet\"><carray_list xs.n=\"_sources\" count=\"0\"/></CDeformerSourceSet>"
        "<CTextureManager xs.n=\"textureManager\"><carray_list xs.n=\"_rawImages\" count=\"0\"/>"
        "<carray_list xs.n=\"_modelImageGroups\" count=\"0\"/></CTextureManager></CModelSource></main></root>");
}
QByteArray archive(const QByteArray& xml)
{
    cmo3::CCaffArchive archive;
    archive.AddEntry("main.xml", xml, "main_xml", cmo3::CCaffArchive::CompressSmall);
    QTemporaryDir directory;
    const auto path = directory.filePath("project.cmo3");
    if (!archive.Save(path)) qFatal("cannot save fixture");
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) qFatal("cannot read fixture");
    return file.readAll();
}
QByteArray replaceXml(const QByteArray& bytes, const std::function<void(QByteArray&)>& edit)
{
    cmo3::CCaffArchive source, target;
    if (!source.LoadBytes(bytes)) qFatal("cannot read archive");
    for (const auto& entry : source.Entries()) {
        auto content = entry.content;
        if (entry.tag == QStringLiteral("main_xml")) edit(content);
        target.AddEntry(entry.path, content, entry.tag, entry.compress);
    }
    QTemporaryDir directory;
    const auto path = directory.filePath("project.cmo3");
    if (!target.Save(path)) qFatal("cannot save fixture");
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) qFatal("cannot read fixture");
    return file.readAll();
}
}

class Cmo3ProjectTests : public QObject
{
    Q_OBJECT
    QByteArray m_generated;
private slots:
    void initTestCase()
    {
        QTemporaryDir directory;
        cmo3::CCmo3Builder::Options options;
        options.mocPath = QStringLiteral(TEST_RESOURCES "/Mark/Mark.moc3");
        options.outPath = directory.filePath("model.cmo3");
        QImage image(16, 16, QImage::Format_RGBA8888);
        image.fill(QColor(33, 77, 129, 83));
        QByteArray png; QBuffer buffer(&png); QVERIFY(buffer.open(QIODevice::WriteOnly)); QVERIFY(image.save(&buffer, "PNG"));
        options.texturePngs = QMap<int, QByteArray>{{0, png}};
        QString error;
        QVERIFY2(cmo3::CCmo3Builder::Build(options, &error), qPrintable(error));
        QFile file(options.outPath); QVERIFY(file.open(QIODevice::ReadOnly)); m_generated = file.readAll();
    }
    void independentCapabilities_data()
    {
        QTest::addColumn<QByteArray>("xml");
        QTest::addColumn<bool>("artwork"); QTest::addColumn<bool>("names"); QTest::addColumn<bool>("materials");
        const auto valid = emptyProject();
        QTest::newRow("all") << valid << true << true << true;
        auto xml = valid; xml.replace("xs.n=\"canvas\"", "xs.n=\"absentCanvas\"");
        QTest::newRow("names-unavailable") << xml << true << false << true;
        xml = valid; xml.replace("xs.n=\"_rawImages\"", "xs.n=\"absentImages\"");
        QTest::newRow("names-only") << xml << false << true << false;
        xml = valid; xml.replace("xs.n=\"_modelImageGroups\"", "xs.n=\"absentGroups\"");
        QTest::newRow("materials-unavailable") << xml << true << true << false;
    }
    void independentCapabilities()
    {
        QFETCH(QByteArray, xml); QFETCH(bool, artwork); QFETCH(bool, names); QFETCH(bool, materials);
        cmo3::CCmo3Project project; QString error;
        QVERIFY2(cmo3::CCmo3Project::OpenBytes(archive(xml), "offline.cmo3", project, &error), qPrintable(error));
        QVERIFY(project.IsValid()); QVERIFY(error.isEmpty());
        QCOMPARE(project.Artwork() != nullptr, artwork); QCOMPARE(project.Names() != nullptr, names); QCOMPARE(project.Materials() != nullptr, materials);
        QCOMPARE(project.ArtworkError().isEmpty(), artwork); QCOMPARE(project.NamesError().isEmpty(), names); QCOMPARE(project.MaterialsError().isEmpty(), materials);
        cmo3::CCmo3NameReader::Catalog catalog;
        QCOMPARE(cmo3::CCmo3NameReader::ReadBytes(archive(xml), {}, catalog, &error), names);
        CPsdSourceCatalog source;
        QCOMPARE(cmo3::CCmo3ArtworkReader::ReadBytes(archive(xml), {}, source, &error), artwork);
    }
    void legacyProcessingVersionsAndBlend()
    {
        const auto bytes = replaceXml(m_generated, [](QByteArray& xml) {
            const QRegularExpression version(QStringLiteral(" fileFormatVersion=\"[^\"]*\""));
            QString text = QString::fromUtf8(xml); text.remove(version);
            xml = text.toUtf8(); xml.replace("CBlend_Normal", "CBlend_Normal_Original");
        });
        cmo3::CCmo3Project project; QString error;
        QVERIFY2(cmo3::CCmo3Project::OpenBytes(bytes, {}, project, &error), qPrintable(error));
        QVERIFY2(project.Artwork(), qPrintable(project.ArtworkError()));
        QVERIFY(project.SourceVersion().startsWith(QStringLiteral("SerializeFormatVersion:")));
        bool legacy = false;
        for (const auto& entry : project.Artwork()->documents) {
            const auto* document = entry.Reconstructed(); QVERIFY(document);
            for (const auto& diagnostic : document->diagnostics) legacy |= diagnostic.code == QStringLiteral("legacy-normal-blend");
            for (const auto& layer : document->layers) QCOMPARE(layer.blendMode, QByteArray("norm"));
        }
        QVERIFY(legacy);
    }
    void duplicateProcessingVersionsRejected()
    {
        auto xml = emptyProject();
        xml.prepend("<?version SerializeFormatVersion:2?><?version SerializeFormatVersion:2?>");
        cmo3::CCmo3Project result; QString error;
        QVERIFY(!cmo3::CCmo3Project::OpenBytes(archive(xml), {}, result, &error));
        QVERIFY(!result.IsValid()); QVERIFY(error.contains(QStringLiteral("版本")));
    }
    void borrowedInputAndPathAreFrozenBeforeCallbacks()
    {
        auto backing = archive(emptyProject()); const auto expected = backing;
        auto bytes = QByteArray::fromRawData(backing.constData(), backing.size());
        QString path = QStringLiteral("original.cmo3"); auto borrowedPath = QString::fromRawData(path.constData(), path.size());
        CExportControl control; bool changed = false;
        QVERIFY(control.SetHandler([&](const auto&, QString*) {
            if (!changed) { changed = true; backing.fill('x'); path.fill(QChar('x')); }
            return true;
        }));
        cmo3::CCmo3Project project; QString error;
        QVERIFY2(cmo3::CCmo3Project::OpenBytes(bytes, borrowedPath, project, &error, &control), qPrintable(error));
        QVERIFY(changed); QCOMPARE(project.Bytes(), expected); QCOMPARE(project.SourcePath(), QStringLiteral("original.cmo3"));
        QCOMPARE(project.SourceDigest(), QCryptographicHash::hash(expected, QCryptographicHash::Sha256));
    }
    void canceledReadDoesNotPublishPartialCapabilities()
    {
        cmo3::CCmo3Project project; QString error;
        QVERIFY(cmo3::CCmo3Project::OpenBytes(archive(emptyProject()), "existing.cmo3", project, &error));
        const auto original = project; CExportControl control;
        QVERIFY(control.SetHandler([&](const auto& progress, QString*) {
            if (progress.phase == CExportControl::Phase::Extracting) control.Cancel();
            return true;
        }));
        QVERIFY(!cmo3::CCmo3Project::OpenBytes(m_generated, "new.cmo3", project, &error, &control));
        QCOMPARE(project.Bytes(), original.Bytes()); QCOMPARE(project.SourcePath(), original.SourcePath());
        QVERIFY(control.WasCanceled()); QVERIFY(project.Names());
    }
    void handlerFailureDoesNotBecomeCapabilityError()
    {
        CExportControl control;
        QVERIFY(control.SetHandler([](const auto&, QString* error) { *error = QStringLiteral("stop reading"); return false; }));
        cmo3::CCmo3Project project; QString error;
        QVERIFY(!cmo3::CCmo3Project::OpenBytes(m_generated, {}, project, &error, &control));
        QVERIFY(!project.IsValid()); QCOMPARE(error, QStringLiteral("stop reading"));
    }
    void generatedMaterialsPreserveSourceCoordinates()
    {
        cmo3::CCmo3Project project; QString error;
        QVERIFY2(cmo3::CCmo3Project::OpenBytes(m_generated, {}, project, &error), qPrintable(error));
        QVERIFY2(project.Artwork(), qPrintable(project.ArtworkError())); QVERIFY2(project.Names(), qPrintable(project.NamesError()));
        QVERIFY2(project.Materials(), qPrintable(project.MaterialsError()));
        const auto& graph = *project.Materials(); QVERIFY(!graph.meshes.isEmpty()); QCOMPARE(graph.images.size(), graph.meshes.size());
        QMap<QString, const cmo3::CCmo3MaterialGraph::ModelImage*> images;
        for (const auto& image : graph.images) {
            QVERIFY(image.directRaster); QCOMPARE(image.candidates.size(), 1); QVERIFY(image.candidates.first().current);
            QCOMPARE(image.candidates.first().layers.size(), 1); QVERIFY(image.candidates.first().layers.first().affine.isIdentity());
            QVERIFY(!image.imageDigest.isEmpty()); images.insert(image.guid, &image);
        }
        for (const auto& mesh : graph.meshes) {
            QCOMPARE(mesh.inputs.size(), 1); QVERIFY(mesh.inputs.first().current); QVERIFY(mesh.inputs.first().optionalTransformOnCanvas.isIdentity());
            const auto* image = images.value(mesh.inputs.first().modelImageGuid); QVERIFY(image);
            QCOMPARE(mesh.materialPositions.size(), mesh.materialUvs.size()); QVERIFY(!mesh.indices.isEmpty());
            const QSize envelope((image->imageSize.width() + 63) / 64 * 64, (image->imageSize.height() + 63) / 64 * 64);
            for (int i = 0; i < mesh.materialPositions.size(); ++i) {
                const auto actual = image->materialLocalToCanvas.map(QPointF(mesh.materialUvs[i].x() * envelope.width(), mesh.materialUvs[i].y() * envelope.height()));
                QVERIFY(QLineF(actual, mesh.materialPositions[i]).length() < 0.001);
            }
        }
    }
    void emptyContainerMetadataIsStrictlyTyped_data()
    {
        QTest::addColumn<QByteArray>("field"); QTest::addColumn<bool>("empty");
        QTest::newRow("hash-string") << QByteArray("<hash_map count=\"0\" keyType=\"string\"/>") << true;
        QTest::newRow("linked-string") << QByteArray("<linked_map count=\"0\" keyType=\"string\"/>") << true;
        QTest::newRow("serialization-index") << QByteArray("<hash_map count=\"0\" keyType=\"string\" xs.idx=\"106\"/>") << true;
        QTest::newRow("invalid-serialization-index") << QByteArray("<hash_map count=\"0\" keyType=\"string\" xs.idx=\"-1\"/>") << false;
        QTest::newRow("unknown-serialization-attribute") << QByteArray("<hash_map count=\"0\" keyType=\"string\" xs.future=\"106\"/>") << false;
        QTest::newRow("nonempty-hash") << QByteArray("<hash_map count=\"1\" keyType=\"string\"><s>effect</s></hash_map>") << false;
        QTest::newRow("unknown-key-type") << QByteArray("<hash_map count=\"0\" keyType=\"future\"/>") << false;
        QTest::newRow("unknown-map-type") << QByteArray("<future_map count=\"0\" keyType=\"string\"/>") << false;
        QTest::newRow("missing-count") << QByteArray("<hash_map keyType=\"string\"/>") << false;
        QTest::newRow("unknown-attribute") << QByteArray("<hash_map count=\"0\" keyType=\"string\" mode=\"enabled\"/>") << false;
        QTest::newRow("text-payload") << QByteArray("<hash_map count=\"0\" keyType=\"string\">effect</hash_map>") << false;
    }
    void emptyContainerMetadataIsStrictlyTyped()
    {
        QFETCH(QByteArray, field); QFETCH(bool, empty);
        cmo3::detail::Xml xml;
        xml.parse(QByteArray("<root>") + field + "</root>", nullptr);
        QCOMPARE(xml.empty(1), empty);
    }
    void attributedUnknownFieldsPreventAutomaticSampling_data()
    {
        QTest::addColumn<QByteArray>("closingTag");
        QTest::addColumn<QByteArray>("field");
        QTest::newRow("model-image") << QByteArray("</CModelImage>") << QByteArray("<CUnmodeled xs.n=\"futureEffect\" mode=\"enabled\"/>");
        QTest::newRow("layer-input") << QByteArray("</CLayerInputData>") << QByteArray("<CUnmodeled xs.n=\"futureEffect\" mode=\"enabled\"/>");
    }
    void attributedUnknownFieldsPreventAutomaticSampling()
    {
        QFETCH(QByteArray, closingTag); QFETCH(QByteArray, field);
        const auto bytes = replaceXml(m_generated, [&](QByteArray& xml) {
            QVERIFY(xml.contains(closingTag));
            xml.replace(closingTag, field + closingTag);
        });
        cmo3::CCmo3Project project; QString error;
        QVERIFY2(cmo3::CCmo3Project::OpenBytes(bytes, {}, project, &error), qPrintable(error));
        QVERIFY(project.Artwork()); QVERIFY(project.Names()); QVERIFY2(project.Materials(), qPrintable(project.MaterialsError()));
        for (const auto& image : project.Materials()->images) {
            QVERIFY(!image.directRaster); QVERIFY(!image.candidates.isEmpty()); QVERIFY(!image.diagnostics.isEmpty());
        }
    }
    void attributedUnknownSourceFieldsPreventLossyRecovery()
    {
        const auto bytes = replaceXml(m_generated, [](QByteArray& xml) {
            QVERIFY(xml.contains("</CLayer>"));
            xml.replace("</CLayer>", "<CUnmodeled xs.n=\"futureEffect\" mode=\"enabled\"/></CLayer>");
        });
        cmo3::CCmo3Project project; QString error;
        QVERIFY2(cmo3::CCmo3Project::OpenBytes(bytes, {}, project, &error), qPrintable(error));
        QVERIFY2(project.Artwork(), qPrintable(project.ArtworkError()));
        bool unsupported = false;
        for (const auto& entry : project.Artwork()->documents) {
            const auto* document = entry.Reconstructed(); QVERIFY(document);
            for (const auto& diagnostic : document->diagnostics)
                unsupported |= diagnostic.code == QStringLiteral("unsupported-source-field");
        }
        QVERIFY(unsupported);
    }
    void unsupportedFilterRetainsProvenance()
    {
        const auto bytes = replaceXml(m_generated, [](QByteArray& xml) {
            xml.replace("5e9fe1ea-0ec3-4d68-a5fa-018fc7abe301", "unknown-filter-definition");
        });
        cmo3::CCmo3Project project; QString error;
        QVERIFY2(cmo3::CCmo3Project::OpenBytes(bytes, {}, project, &error), qPrintable(error));
        QVERIFY(project.Artwork()); QVERIFY(project.Names()); QVERIFY2(project.Materials(), qPrintable(project.MaterialsError()));
        for (const auto& image : project.Materials()->images) {
            QVERIFY(!image.directRaster); QVERIFY(!image.candidates.isEmpty()); QVERIFY(!image.diagnostics.isEmpty());
        }
    }
    void brokenCurrentInputDoesNotHideArtworkOrNames()
    {
        const auto bytes = replaceXml(m_generated, [](QByteArray& xml) {
            auto text = QString::fromUtf8(xml);
            const QRegularExpression reference(QStringLiteral("(xs.n=\"currentTextureInputData\" xs.ref=\")[^\"]+"));
            const auto match = reference.match(text); if (!match.hasMatch()) qFatal("missing texture input fixture");
            text.replace(match.capturedStart(), match.capturedLength(), match.captured(1) + QStringLiteral("missing-input"));
            xml = text.toUtf8();
        });
        cmo3::CCmo3Project project; QString error;
        QVERIFY2(cmo3::CCmo3Project::OpenBytes(bytes, {}, project, &error), qPrintable(error));
        QVERIFY(project.Artwork()); QVERIFY(project.Names()); QVERIFY(!project.Materials()); QVERIFY(!project.MaterialsError().isEmpty());
    }
    void sourceReferencesRequireTheExactOwner_data()
    {
        QTest::addColumn<QString>("type"); QTest::addColumn<QString>("field");
        QTest::newRow("extension-owner") << QStringLiteral("CArtMeshSource") << QStringLiteral("_owner");
        QTest::newRow("input-owner") << QStringLiteral("CTextureInputExtension") << QStringLiteral("_owner");
        QTest::newRow("current-input") << QStringLiteral("CTextureInput_ModelImage") << QStringLiteral("currentTextureInputData");
    }
    void sourceReferencesRequireTheExactOwner()
    {
        QFETCH(QString, type); QFETCH(QString, field);
        const auto bytes = replaceXml(m_generated, [&](QByteArray& xml) {
            auto text = QString::fromUtf8(xml);
            const QRegularExpression reference(QStringLiteral("<%1 xs.n=\"%2\" xs.ref=\"([^\"]+)\"").arg(type, field));
            auto matches = reference.globalMatch(text);
            if (!matches.hasNext()) qFatal("missing owner fixture");
            const auto first = matches.next();
            if (!matches.hasNext()) qFatal("missing second owner fixture");
            const auto second = matches.next();
            text.replace(first.capturedStart(1), first.capturedLength(1), second.captured(1));
            xml = text.toUtf8();
        });
        cmo3::CCmo3Project project; QString error;
        QVERIFY2(cmo3::CCmo3Project::OpenBytes(bytes, {}, project, &error), qPrintable(error));
        QVERIFY(project.Artwork()); QVERIFY(project.Names()); QVERIFY(!project.Materials()); QVERIFY(!project.MaterialsError().isEmpty());
    }
    void numericGeometryIsValidated_data()
    {
        QTest::addColumn<QString>("field"); QTest::addColumn<QString>("value");
        QTest::newRow("nonfinite-uv") << QStringLiteral("uvs") << QStringLiteral("nan");
        QTest::newRow("negative-index") << QStringLiteral("indices") << QStringLiteral("-1");
        QTest::newRow("fractional-index") << QStringLiteral("indices") << QStringLiteral("0.5");
        QTest::newRow("overflow-index") << QStringLiteral("indices") << QStringLiteral("2147483648");
    }
    void numericGeometryIsValidated()
    {
        QFETCH(QString, field); QFETCH(QString, value);
        const auto bytes = replaceXml(m_generated, [&](QByteArray& xml) {
            auto text = QString::fromUtf8(xml);
            const QRegularExpression array(QStringLiteral("<[^>]+xs.n=\"%1\"[^>]*>\\s*([^\\s<]+)").arg(field));
            const auto match = array.match(text);
            if (!match.hasMatch()) qFatal("missing numeric geometry fixture");
            text.replace(match.capturedStart(1), match.capturedLength(1), value);
            xml = text.toUtf8();
        });
        cmo3::CCmo3Project project; QString error;
        QVERIFY2(cmo3::CCmo3Project::OpenBytes(bytes, {}, project, &error), qPrintable(error));
        QVERIFY(project.Artwork()); QVERIFY(!project.Materials()); QVERIFY(!project.MaterialsError().isEmpty());
    }
    void nonIdentityTransformsKeepProvenance_data()
    {
        QTest::addColumn<QString>("field");
        QTest::newRow("layer-input") << QStringLiteral("affine");
        QTest::newRow("canvas-input") << QStringLiteral("optionalTransformOnCanvas");
    }
    void nonIdentityTransformsKeepProvenance()
    {
        QFETCH(QString, field);
        const auto bytes = replaceXml(m_generated, [&](QByteArray& xml) {
            auto text = QString::fromUtf8(xml);
            const QRegularExpression transform(QStringLiteral("<CAffine xs.n=\"%1\"[^>]*m02=\"([^\"]+)\"").arg(field));
            const auto match = transform.match(text);
            if (!match.hasMatch()) qFatal("missing affine fixture");
            text.replace(match.capturedStart(1), match.capturedLength(1), QStringLiteral("12.5"));
            xml = text.toUtf8();
        });
        cmo3::CCmo3Project project; QString error;
        QVERIFY2(cmo3::CCmo3Project::OpenBytes(bytes, {}, project, &error), qPrintable(error));
        QVERIFY2(project.Materials(), qPrintable(project.MaterialsError()));
        int changed = 0;
        if (field == QStringLiteral("affine")) {
            for (const auto& image : project.Materials()->images) for (const auto& candidate : image.candidates)
                for (const auto& layer : candidate.layers) if (!layer.affine.isIdentity()) {
                    QCOMPARE(layer.affine.dx(), 12.5); QVERIFY(!layer.diagnostics.isEmpty()); QVERIFY(!image.directRaster); ++changed;
                }
        } else {
            for (const auto& mesh : project.Materials()->meshes) for (const auto& input : mesh.inputs)
                if (!input.optionalTransformOnCanvas.isIdentity()) {
                    QCOMPARE(input.optionalTransformOnCanvas.dx(), 12.5); QVERIFY(!input.diagnostics.isEmpty()); ++changed;
                }
        }
        QCOMPARE(changed, 1);
    }
    void nativeFileCapabilityEvidence()
    {
        const QString path = qEnvironmentVariable("LIVE2D_TEST_NATIVE_CMO");
        if (path.isEmpty()) QSKIP("LIVE2D_TEST_NATIVE_CMO is not set");
        cmo3::CCmo3Project project; QString error;
        QVERIFY2(cmo3::CCmo3Project::OpenFile(path, project, &error), qPrintable(error));
        QVERIFY2(project.Artwork(), qPrintable(project.ArtworkError())); QVERIFY2(project.Names(), qPrintable(project.NamesError()));
        QVERIFY2(project.Materials(), qPrintable(project.MaterialsError()));
        QCOMPARE(project.Artwork()->documents.size(), 1);
        const auto* source = project.Artwork()->documents.first().Reconstructed(); QVERIFY(source); QCOMPARE(source->layers.size(), 73);
        QStringList sourceErrors;
        for (const auto& diagnostic : source->diagnostics)
            if (diagnostic.severity == CPsdSourceDiagnostic::Severity::Error)
                sourceErrors.append(diagnostic.code + QStringLiteral(": ") + diagnostic.message);
        QVERIFY2(sourceErrors.isEmpty(), qPrintable(sourceErrors.join(QChar('\n'))));
        QCOMPARE(project.Materials()->images.size(), 58); QCOMPARE(project.Materials()->meshes.size(), 56);
        QCOMPARE(project.SourceVersion(), QStringLiteral("SerializeFormatVersion:2"));
        int rasters = 0, groups = 0;
        for (const auto& layer : source->layers) {
            if (layer.kind == CPsdSourceLayer::Kind::Group) ++groups;
            else ++rasters;
        }
        QCOMPARE(rasters, 58); QCOMPARE(groups, 15);
        QMap<QString, const cmo3::CCmo3MaterialGraph::ModelImage*> images;
        for (const auto& image : project.Materials()->images) {
            QVERIFY(image.directRaster); images.insert(image.guid, &image);
        }
        QSet<QString> referencedImages;
        int vertices = 0;
        for (const auto& mesh : project.Materials()->meshes) {
            QCOMPARE(mesh.inputs.size(), 1); QVERIFY(mesh.inputs.first().current);
            const auto* image = images.value(mesh.inputs.first().modelImageGuid); QVERIFY(image);
            referencedImages.insert(image->guid);
            QCOMPARE(mesh.materialPositions.size(), mesh.materialUvs.size());
            vertices += mesh.materialPositions.size();
            for (int index = 0; index < mesh.materialPositions.size(); ++index) {
                const QPointF pixels(mesh.materialUvs[index].x() * image->imageSize.width(),
                                     mesh.materialUvs[index].y() * image->imageSize.height());
                QVERIFY(QLineF(image->materialLocalToCanvas.map(pixels), mesh.materialPositions[index]).length() < 0.001);
            }
        }
        QCOMPARE(vertices, 1130); QCOMPARE(referencedImages.size(), 56);
        QStringList unmeshed;
        for (const auto& image : project.Materials()->images)
            if (!referencedImages.contains(image.guid)) unmeshed.append(image.name);
        unmeshed.sort();
        QStringList expected {QStringLiteral("ハイライト"), QStringLiteral("唇")}; expected.sort();
        QCOMPARE(unmeshed, expected);
    }
    void officialMaoNullMaterialTransformsRemainDiagnosed()
    {
        const QString path = qEnvironmentVariable("LIVE2D_TEST_MAO_CMO");
        if (path.isEmpty()) QSKIP("LIVE2D_TEST_MAO_CMO is not configured");
        cmo3::CCmo3Project project; QString error;
        QVERIFY2(cmo3::CCmo3Project::OpenFile(path, project, &error), qPrintable(error));
        QVERIFY2(project.Materials(), qPrintable(project.MaterialsError()));
        QCOMPARE(project.Materials()->images.size(), 187);
        QCOMPARE(project.Materials()->meshes.size(), 261);
        int missing = 0, direct = 0;
        for (const auto& image : project.Materials()->images) {
            if (!image.materialTransformPresent) {
                ++missing;
                QVERIFY(!image.directRaster);
                QVERIFY(image.diagnostics.join('\n').contains(QStringLiteral("按单位变换读取")));
            }
            if (image.directRaster) ++direct;
        }
        QCOMPARE(missing, 13);
        QCOMPARE(direct, 173);
    }
    void malformedMaterialTransformDoesNotBecomeAnIdentity_data()
    {
        QTest::addColumn<QString>("replacement");
        QTest::newRow("attribute") << QStringLiteral("<null xs.n=\"_materialLocalToCanvasTransform\" effect=\"enabled\"/>");
        QTest::newRow("text") << QStringLiteral("<null xs.n=\"_materialLocalToCanvasTransform\">payload</null>");
        QTest::newRow("child") << QStringLiteral("<null xs.n=\"_materialLocalToCanvasTransform\"><null/></null>");
    }
    void malformedMaterialTransformDoesNotBecomeAnIdentity()
    {
        QFETCH(QString, replacement);
        const auto bytes = replaceXml(m_generated, [&](QByteArray& xml) {
            auto text = QString::fromUtf8(xml);
            const QRegularExpression pattern(QStringLiteral("<CAffine[^>]*xs.n=\"_materialLocalToCanvasTransform\"[^>]*/>"));
            const auto match = pattern.match(text); QVERIFY(match.hasMatch());
            text.replace(match.capturedStart(), match.capturedLength(), replacement); xml = text.toUtf8();
        });
        cmo3::CCmo3Project project; QString error;
        QVERIFY2(cmo3::CCmo3Project::OpenBytes(bytes, {}, project, &error), qPrintable(error));
        QVERIFY(project.Artwork()); QVERIFY(project.Names()); QVERIFY(!project.Materials());
        QVERIFY(!project.MaterialsError().isEmpty());
    }
};
QTEST_APPLESS_MAIN(Cmo3ProjectTests)
#include "cmo3_project_tests.moc"
