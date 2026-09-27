#include "cmo3/ccmo3artworkreader.h"
#include "cmo3/ccaffarchive.h"
#include "std/cexportcontrol.h"
#include "std/cpngimage.h"
#include "psd/psdSamples/Zip/Psdminiz.h"
#include <QtEndian>
#include <QBuffer>
#include <QCryptographicHash>
#include <QFile>
#include <QImage>
#include <QTemporaryDir>
#include <QtTest>

namespace {
QByteArray png()
{
    QImage image(2, 1, QImage::Format_RGBA8888);
    const unsigned char pixels[] = {17, 29, 43, 0, 83, 109, 151, 123};
    memcpy(image.bits(), pixels, sizeof(pixels));
    QByteArray result;
    QBuffer buffer(&result);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return result;
}
QByteArray pngChunk(const QByteArray& type, const QByteArray& content)
{
    QByteArray bytes(4, '\0');
    qToBigEndian<quint32>(quint32(content.size()), reinterpret_cast<uchar*>(bytes.data()));
    bytes += type;
    bytes += content;
    const quint32 crc = quint32(mz_crc32(MZ_CRC32_INIT, reinterpret_cast<const unsigned char*>(bytes.constData() + 4), bytes.size() - 4));
    const auto old = bytes.size(); bytes.resize(old + 4);
    qToBigEndian<quint32>(crc, reinterpret_cast<uchar*>(bytes.data() + old));
    return bytes;
}
QByteArray withProfile(QByteArray image, const QByteArray& profile)
{
    image.insert(33, pngChunk("iCCP", QByteArray("profile\0\0", 9) + qCompress(profile).mid(4)));
    return image;
}

QString common(const QString& name, const QString& id, const QString& parent, const QString& extra = {})
{
    return QStringLiteral("<ACLayerEntry xs.n=\"super\"><s xs.n=\"name\">%1</s><s xs.n=\"memo\"/>"
        "<b xs.n=\"isVisible\">true</b><b xs.n=\"isClipping\">false</b>"
        "<CBlend_Normal xs.n=\"blend\"><ACBlend xs.n=\"super\"/></CBlend_Normal>"
        "<CLayerGuid xs.n=\"guid\" uuid=\"%2\"/>%3<i xs.n=\"opacity255\">255</i>"
        "<hash_map xs.n=\"_optionOfIOption\" count=\"0\"/><CLayeredImage xs.n=\"_layeredImage\" xs.ref=\"doc\"/>%4</ACLayerEntry>")
        .arg(name, id, parent.isEmpty() ? QStringLiteral("<null xs.n=\"group\"/>") :
             QStringLiteral("<CLayerGroup xs.n=\"group\" xs.ref=\"%1\"/>").arg(parent), extra);
}
QString project(const QByteArray& pixels)
{
    const QString root = QStringLiteral("<CLayerGroup xs.id=\"root\"><ACLayerGroup xs.n=\"super\">%1"
        "<carray_list xs.n=\"_children\" count=\"2\"><CLayer xs.ref=\"front\"/><CLayerGroup xs.ref=\"folder\"/></carray_list>"
        "</ACLayerGroup><null xs.n=\"layerIdentifier\"/></CLayerGroup>").arg(common("root", "root-guid", {}));
    const QString folder = QStringLiteral("<CLayerGroup xs.id=\"folder\"><ACLayerGroup xs.n=\"super\">%1"
        "<carray_list xs.n=\"_children\" count=\"1\"><CLayer xs.ref=\"back\"/></carray_list>"
        "</ACLayerGroup><null xs.n=\"layerIdentifier\"/></CLayerGroup>").arg(common("分组 🌙", "folder-guid", "root"));
    const auto leaf = [&pixels](const QString& id, const QString& parent, const QString& layerId) {
        return QStringLiteral("<CLayer xs.id=\"%1\"><ACImageLayer xs.n=\"super\">%2</ACImageLayer>"
            "<CRect xs.n=\"boundsOnImageDoc\"><i xs.n=\"x\">-3</i><i xs.n=\"y\">4</i><i xs.n=\"width\">2</i><i xs.n=\"height\">1</i></CRect>"
            "<CLayerIdentifier xs.n=\"layerIdentifier\"><s xs.n=\"layerName\">旧名</s><s xs.n=\"layerId\">%3</s></CLayerIdentifier>"
            "<CImageResource xs.n=\"imageResource\" width=\"2\" height=\"1\" type=\"INT_ARGB\" imageFileBuf_size=\"%4\">"
            "<file xs.n=\"imageFileBuf\" path=\"original.png\"/></CImageResource><linked_map xs.n=\"layerInfo\" count=\"0\"/>"
            "</CLayer>").arg(id, common("真实 &amp; 同名 😀", id + "-guid", parent), layerId).arg(pixels.size());
    };
    return QStringLiteral("<?xml version=\"1.0\"?><root fileFormatVersion=\"502020000\"><shared>"
        "<CLayeredImage xs.id=\"doc\"><s xs.n=\"name\">原画文档</s><i xs.n=\"width\">20</i><i xs.n=\"height\">30</i>"
        "<CLayeredImageGuid xs.n=\"guid\" uuid=\"doc-guid\"/><null xs.n=\"psdBytes\"/>"
        "<CLayerGroup xs.n=\"_rootLayer\" xs.ref=\"root\"/><LayerSet xs.n=\"layerSet\">"
        "<CLayeredImage xs.n=\"_layeredImage\" xs.ref=\"doc\"/><carray_list xs.n=\"_layerEntryList\" count=\"4\">"
        "<CLayerGroup xs.ref=\"root\"/><CLayer xs.ref=\"front\"/><CLayerGroup xs.ref=\"folder\"/><CLayer xs.ref=\"back\"/>"
        "</carray_list></LayerSet></CLayeredImage>%1%2%3%4</shared><main><CModelSource><CTextureManager xs.n=\"textureManager\">"
        "<carray_list xs.n=\"_rawImages\" count=\"1\"><LayeredImageWrapper><CLayeredImage xs.n=\"image\" xs.ref=\"doc\"/>"
        "<b xs.n=\"isReplaced\">true</b></LayeredImageWrapper></carray_list></CTextureManager></CModelSource></main></root>")
        .arg(root, folder, leaf("front", "root", "FF-FF-FF-FE"), leaf("back", "folder", "00-00-00-07"));
}
QByteArray archive(const QString& xml, const QByteArray& pixels, bool includePng = true)
{
    QTemporaryDir directory;
    cmo3::CCaffArchive archive;
    archive.AddEntry("main.xml", xml.toUtf8(), "main_xml", cmo3::CCaffArchive::CompressSmall);
    if (includePng) archive.AddEntry("original.png", pixels);
    const QString path = directory.filePath("model.cmo3");
    if (!archive.Save(path)) return {};
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return file.readAll();
}
bool hasDiagnostic(const CPsdSourceDocument& document, const QString& code, CPsdSourceDiagnostic::Severity severity)
{
    for (const auto& diagnostic : document.diagnostics) if (diagnostic.code == code && diagnostic.severity == severity) return true;
    return false;
}
}
class Cmo3ArtworkReaderTests : public QObject
{
    Q_OBJECT
private slots:
    void sourceTreeNamesBoundsAndStraightPixels()
    {
        const auto pixels = png();
        const auto bytes = archive(project(pixels), pixels);
        CPsdSourceCatalog result;
        QString error;
        QVERIFY2(cmo3::CCmo3ArtworkReader::ReadBytes(bytes, "source.cmo3", result, &error), qPrintable(error));
        QCOMPARE(result.sourceDigest, QCryptographicHash::hash(bytes, QCryptographicHash::Sha256));
        QCOMPARE(result.sourceVersion, QString("502020000"));
        QCOMPARE(result.documents.size(), 1);
        const auto& doc = result.documents.first();
        QCOMPARE(doc.name, QString("原画文档"));
        QCOMPARE(doc.canvasSize, QSize(20, 30));
        QCOMPARE(doc.layers.size(), 3);
        QCOMPARE(doc.layers[0].id, QString("front-guid"));
        QCOMPARE(doc.layers[1].id, QString("folder-guid"));
        QCOMPARE(doc.layers[2].id, QString("back-guid"));
        QCOMPARE(doc.layers[0].name, QString("真实 & 同名 😀"));
        QCOMPARE(doc.layers[0].name, doc.layers[2].name);
        QCOMPARE(doc.layers[0].bounds, QRect(-3, 4, 2, 1));
        QCOMPARE(doc.layers[2].parentIndex, 1);
        QCOMPARE(doc.layers[0].parentIndex, -1);
        QCOMPARE(doc.layers[0].externalId, std::optional<quint32>(0xfffffffe));
        QCOMPARE(doc.layers[0].png, pixels);
        QImage decoded;
        QVERIFY(CPngImage::Decode(doc.layers[0].png, decoded, &error));
        QCOMPARE(decoded.pixelColor(0, 0), QColor(17, 29, 43, 0));
        QVERIFY(hasDiagnostic(doc, "missing-icc", CPsdSourceDiagnostic::Severity::Warning));
    }
    void retainsNondefaultRootAndLayerAttributes()
    {
        const auto pixels = png();
        QString xml = project(pixels);
        xml.replace("<i xs.n=\"opacity255\">255</i>", "<i xs.n=\"opacity255\">123</i>");
        xml.replace("<b xs.n=\"isVisible\">true</b>", "<b xs.n=\"isVisible\">false</b>");
        xml.replace("<b xs.n=\"isClipping\">false</b>", "<b xs.n=\"isClipping\">true</b>");
        CPsdSourceCatalog result;
        QString error;
        QVERIFY2(cmo3::CCmo3ArtworkReader::ReadBytes(archive(xml, pixels), {}, result, &error), qPrintable(error));
        QCOMPARE(result.documents[0].layers.size(), 4);
        for (const auto& layer : result.documents[0].layers) {
            QCOMPARE(layer.opacity, quint8(123));
            QVERIFY(!layer.visible);
            QVERIFY(layer.clipping);
        }
        QCOMPARE(result.documents[0].layers[1].parentIndex, 0);
    }
    void unsupportedSourceMetadataBlocksOnlyItsDocument_data()
    {
        QTest::addColumn<QString>("before"); QTest::addColumn<QString>("after");
        QTest::newRow("stored-original") << "<null xs.n=\"psdBytes\"/>" << "<bytes xs.n=\"psdBytes\">0102</bytes>";
        QTest::newRow("layer-info") << "<linked_map xs.n=\"layerInfo\" count=\"0\"/>" << "<linked_map xs.n=\"layerInfo\" count=\"1\"><s>mask</s></linked_map>";
        QTest::newRow("options") << "<hash_map xs.n=\"_optionOfIOption\" count=\"0\"/>" << "<hash_map xs.n=\"_optionOfIOption\" count=\"1\"><s>effect</s></hash_map>";
    }
    void unsupportedSourceMetadataBlocksOnlyItsDocument()
    {
        QFETCH(QString, before); QFETCH(QString, after);
        const auto pixels = png(); QString xml = project(pixels); xml.replace(before, after);
        CPsdSourceCatalog result; QString error;
        QVERIFY2(cmo3::CCmo3ArtworkReader::ReadBytes(archive(xml, pixels), {}, result, &error), qPrintable(error));
        QVERIFY(hasDiagnostic(result.documents[0], "unsupported-source-field", CPsdSourceDiagnostic::Severity::Error));
        QCOMPARE(result.documents[0].layers[0].png, pixels);
    }
    void structuralFailureIsAtomic_data()
    {
        QTest::addColumn<QString>("before"); QTest::addColumn<QString>("after");
        QTest::newRow("undefined") << "xs.ref=\"front\"" << "xs.ref=\"missing\"";
        QTest::newRow("duplicate") << "xs.id=\"back\"" << "xs.id=\"front\"";
        QTest::newRow("cycle") << "<CLayer xs.ref=\"front\"/>" << "<CLayerGroup xs.ref=\"root\"/>";
        QTest::newRow("wrong-count") << "xs.n=\"_rawImages\" count=\"1\"" << "xs.n=\"_rawImages\" count=\"9\"";
        QTest::newRow("wrong-size") << "xs.n=\"width\">2</i>" << "xs.n=\"width\">3</i>";
        QTest::newRow("pixel-budget") << "xs.n=\"width\">2</i>" << "xs.n=\"width\">30001</i>";
        QTest::newRow("dtd") << "?><root" << "?><!DOCTYPE root [<!ENTITY custom 'expanded'>]><root";
        QTest::newRow("parent") << "xs.n=\"group\" xs.ref=\"folder\"" << "xs.n=\"group\" xs.ref=\"root\"";
        QTest::newRow("opacity") << "xs.n=\"opacity255\">255" << "xs.n=\"opacity255\">256";
        QTest::newRow("bool") << "xs.n=\"isVisible\">true" << "xs.n=\"isVisible\">maybe";
        QTest::newRow("id") << "FF-FF-FF-FE" << "GG-00-00-00";
    }
    void structuralFailureIsAtomic()
    {
        QFETCH(QString, before); QFETCH(QString, after);
        const auto pixels = png(); QString xml = project(pixels); QVERIFY(xml.contains(before)); xml.replace(before, after);
        CPsdSourceCatalog result; result.sourcePath = "retained"; result.documents.append(CPsdSourceDocument{}); QString error;
        QVERIFY(!cmo3::CCmo3ArtworkReader::ReadBytes(archive(xml, pixels), {}, result, &error));
        QVERIFY(!error.isEmpty()); QCOMPARE(result.sourcePath, QString("retained")); QCOMPARE(result.documents.size(), 1);
    }
    void derivedDiagnosticsStayWithinBudget()
    {
        const auto pixels = png(); QString xml = project(pixels);
        QString unknown;
        for (int index = 0; index < 4097; ++index) unknown += QString("<s xs.n=\"unknown-%1\">effect</s>").arg(index);
        const auto at = xml.indexOf("<s xs.n=\"memo\"/>"); xml.insert(at, unknown);
        CPsdSourceCatalog result; result.sourcePath = "retained"; QString error;
        QVERIFY(!cmo3::CCmo3ArtworkReader::ReadBytes(archive(xml, pixels), {}, result, &error));
        QVERIFY(error.contains("诊断数量")); QCOMPARE(result.sourcePath, QString("retained"));
        xml = project(pixels); xml.replace("<shared>", "<shared><" + QString(257, 'x') + "/>");
        QVERIFY(!cmo3::CCmo3ArtworkReader::ReadBytes(archive(xml, pixels), {}, result, &error));
        QVERIFY(error.contains("类型名称"));
    }

    void extremeBoundsAndPartialEmptyLayersAreRejected_data()
    {
        QTest::addColumn<QString>("x"); QTest::addColumn<QString>("y"); QTest::addColumn<QString>("width"); QTest::addColumn<QString>("height");
        QTest::newRow("x-qint64-max") << "9223372036854775807" << "4" << "2" << "1";
        QTest::newRow("y-qint64-max") << "-3" << "9223372036854775807" << "2" << "1";
        QTest::newRow("x-int-min-empty") << "-2147483648" << "4" << "0" << "0";
        QTest::newRow("y-int-min-empty") << "-3" << "-2147483648" << "0" << "0";
        QTest::newRow("zero-by-positive") << "-3" << "4" << "0" << "1";
        QTest::newRow("positive-by-zero") << "-3" << "4" << "2" << "0";
    }
    void extremeBoundsAndPartialEmptyLayersAreRejected()
    {
        QFETCH(QString, x); QFETCH(QString, y); QFETCH(QString, width); QFETCH(QString, height);
        const auto pixels = png(); QString xml = project(pixels);
        xml.replace("<i xs.n=\"x\">-3</i>", QString("<i xs.n=\"x\">%1</i>").arg(x));
        xml.replace("<i xs.n=\"y\">4</i>", QString("<i xs.n=\"y\">%1</i>").arg(y));
        xml.replace("<i xs.n=\"width\">2</i>", QString("<i xs.n=\"width\">%1</i>").arg(width));
        xml.replace("<i xs.n=\"height\">1</i>", QString("<i xs.n=\"height\">%1</i>").arg(height));
        if (width == "0" || height == "0") {
            const QString begin = "<CImageResource xs.n=\"imageResource\"";
            for (;;) {
                const auto at = xml.indexOf(begin); if (at < 0) break;
                const auto end = xml.indexOf("</CImageResource>", at) + 17;
                xml.replace(at, end - at, "<null xs.n=\"imageResource\"/>");
            }
        }
        CPsdSourceCatalog result; result.sourcePath = "retained"; QString error;
        QVERIFY(!cmo3::CCmo3ArtworkReader::ReadBytes(archive(xml, pixels), {}, result, &error));
        QVERIFY(!error.isEmpty()); QCOMPARE(result.sourcePath, QString("retained"));
    }

    void legitimateEmptySourceLayerKeepsItsOrigin()
    {
        const auto pixels = png(); QString xml = project(pixels);
        const auto begin = xml.indexOf("<CLayer xs.id=\"front\"");
        const auto end = xml.indexOf("</CLayer>", begin) + 9;
        QString layer = xml.mid(begin, end - begin);
        layer.replace("<i xs.n=\"width\">2</i>", "<i xs.n=\"width\">0</i>");
        layer.replace("<i xs.n=\"height\">1</i>", "<i xs.n=\"height\">0</i>");
        const auto resource = layer.indexOf("<CImageResource xs.n=\"imageResource\"");
        const auto resourceEnd = layer.indexOf("</CImageResource>", resource) + 17;
        layer.replace(resource, resourceEnd - resource, "<null xs.n=\"imageResource\"/>");
        xml.replace(begin, end - begin, layer);
        CPsdSourceCatalog result; QString error;
        QVERIFY2(cmo3::CCmo3ArtworkReader::ReadBytes(archive(xml, pixels), {}, result, &error), qPrintable(error));
        QCOMPARE(result.documents[0].layers[0].bounds, QRect(-3, 4, 0, 0));
        QVERIFY(result.documents[0].layers[0].png.isEmpty());
        QCOMPARE(result.documents[0].layers[2].png, pixels);
    }

    void resourcesNeverReadOutsideArchive()
    {
        const auto pixels = png(); CPsdSourceCatalog result; QString error;
        QTemporaryDir directory; const QString path = directory.filePath("original.png"); QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write(pixels), pixels.size()); file.close();
        QString xml = project(pixels); xml.replace("path=\"original.png\"", QString("path=\"%1\"").arg(path));
        QVERIFY(!cmo3::CCmo3ArtworkReader::ReadBytes(archive(xml, pixels, false), {}, result, &error));
        QVERIFY(error.contains("未找到"));
    }
    void independentDocumentsAndUnrelatedBrokenReferences()
    {
        const auto pixels = png(); QString xml = project(pixels);
        QString shared = xml.mid(xml.indexOf("<shared>") + 8, xml.indexOf("</shared>") - xml.indexOf("<shared>") - 8);
        for (const auto& id : {QString("doc"), QString("root"), QString("folder"), QString("front"), QString("back")}) {
            shared.replace(QString("xs.id=\"%1\"").arg(id), QString("xs.id=\"%1-2\"").arg(id));
            shared.replace(QString("xs.ref=\"%1\"").arg(id), QString("xs.ref=\"%1-2\"").arg(id));
            shared.replace(QString("uuid=\"%1-guid\"").arg(id), QString("uuid=\"%1-guid-2\"").arg(id));
        }
        xml.replace("</shared>", shared + "<UnrelatedPhysics xs.ref=\"lost-physics\"/></shared>");
        xml.replace("xs.n=\"_rawImages\" count=\"1\"", "xs.n=\"_rawImages\" count=\"2\"");
        xml.replace("</LayeredImageWrapper>", "</LayeredImageWrapper><LayeredImageWrapper><CLayeredImage xs.n=\"image\" xs.ref=\"doc-2\"/></LayeredImageWrapper>");
        CPsdSourceCatalog result; QString error;
        QVERIFY2(cmo3::CCmo3ArtworkReader::ReadBytes(archive(xml, pixels), {}, result, &error), qPrintable(error));
        QCOMPARE(result.documents.size(), 2);
        QCOMPARE(result.documents[0].id, QString("doc-guid")); QCOMPARE(result.documents[1].id, QString("doc-guid-2"));
        QCOMPARE(result.documents[0].layers[0].png, result.documents[1].layers[0].png);
        QCOMPARE(result.diagnostics.size(), 1); QCOMPARE(result.diagnostics[0].code, QString("unrelated-invalid-references"));
    }
    void knownAndUnknownBlends_data()
    {
        QTest::addColumn<QString>("type"); QTest::addColumn<QByteArray>("key");
        QTest::newRow("multiply") << "CBlend_Multiply" << QByteArray("mul ");
        QTest::newRow("screen") << "CBlend_Screen" << QByteArray("scrn");
        QTest::newRow("linear-dodge") << "CBlend_LinearDodge" << QByteArray("lddg");
        QTest::newRow("actual-overlay-spelling") << "CBlend_Overray" << QByteArray("over");
        QTest::newRow("unknown") << "CBlend_NotInvented" << QByteArray();
    }
    void knownAndUnknownBlends()
    {
        QFETCH(QString, type); QFETCH(QByteArray, key);
        const auto pixels = png(); QString xml = project(pixels); xml.replace("CBlend_Normal", type);
        CPsdSourceCatalog result; QString error;
        QVERIFY2(cmo3::CCmo3ArtworkReader::ReadBytes(archive(xml, pixels), {}, result, &error), qPrintable(error));
        QCOMPARE(result.documents[0].layers[0].blendMode, key);
        QCOMPARE(hasDiagnostic(result.documents[0], "unsupported-blend", CPsdSourceDiagnostic::Severity::Error), key.isEmpty());
    }
    void rgbTransparencyAndProfiles()
    {
        QImage image(2, 1, QImage::Format_RGB888);
        image.setPixelColor(0, 0, QColor(17, 29, 43)); image.setPixelColor(1, 0, QColor(83, 109, 151));
        QByteArray pixels; QBuffer buffer(&pixels); QVERIFY(buffer.open(QIODevice::WriteOnly)); QVERIFY(image.save(&buffer, "PNG"));
        QCOMPARE(quint8(pixels[25]), quint8(2));
        pixels.insert(33, pngChunk("tRNS", QByteArray::fromHex("0011001d002b")));
        const QByteArray profile("exact ICC bytes retained without conversion");
        pixels = withProfile(pixels, profile);
        CPsdSourceCatalog result; QString error;
        QVERIFY2(cmo3::CCmo3ArtworkReader::ReadBytes(archive(project(pixels), pixels), {}, result, &error), qPrintable(error));
        QCOMPARE(result.documents[0].iccProfile, profile);
        QVERIFY(result.documents[0].iccProfile.capacity() < 1024);
        QImage decoded; QVERIFY(CPngImage::Decode(result.documents[0].layers[0].png, decoded, &error));
        QCOMPARE(decoded.pixelColor(0, 0), QColor(17, 29, 43, 0));
        QCOMPARE(decoded.pixelColor(1, 0), QColor(83, 109, 151, 255));
        QVERIFY(!hasDiagnostic(result.documents[0], "missing-icc", CPsdSourceDiagnostic::Severity::Warning));
    }
    void partiallyProfiledDocumentIsBlocked()
    {
        const auto plain = png(); const auto profiled = withProfile(plain, QByteArray("profile"));
        QString xml = project(plain);
        const auto start = xml.indexOf("<CLayer xs.id=\"back\"");
        const auto end = xml.indexOf("</CLayer>", start) + 9;
        QString back = xml.mid(start, end - start);
        back.replace("path=\"original.png\"", "path=\"profiled.png\"");
        back.replace(QString("imageFileBuf_size=\"%1\"").arg(plain.size()), QString("imageFileBuf_size=\"%1\"").arg(profiled.size()));
        xml.replace(start, end - start, back);
        cmo3::CCaffArchive source; source.AddEntry("main.xml", xml.toUtf8(), "main_xml");
        source.AddEntry("original.png", plain); source.AddEntry("profiled.png", profiled);
        QTemporaryDir directory; const QString path = directory.filePath("model.cmo3"); QVERIFY(source.Save(path));
        CPsdSourceCatalog result; QString error;
        QVERIFY2(cmo3::CCmo3ArtworkReader::ReadFile(path, result, &error), qPrintable(error));
        QVERIFY(hasDiagnostic(result.documents[0], "partial-icc", CPsdSourceDiagnostic::Severity::Error));
    }
    void unsupportedBitDepthNeverSilentlyConverts()
    {
        auto pixels = png(); QByteArray header = pixels.mid(16, 13); header[8] = 16;
        pixels.replace(8, 25, pngChunk("IHDR", header));
        CPsdSourceCatalog result; QString error;
        QVERIFY(!cmo3::CCmo3ArtworkReader::ReadBytes(archive(project(pixels), pixels), {}, result, &error));
        QVERIFY(error.contains("8 位"));
    }
    void originalPassThroughKeyIsRetained()
    {
        const auto pixels = png(); QString xml = project(pixels);
        const auto start = xml.indexOf("<CLayerGroup xs.id=\"folder\"");
        const auto end = xml.indexOf("</CLayerGroup>", start) + 14;
        QString group = xml.mid(start, end - start);
        group.replace("<CBlend_Normal xs.n=\"blend\"><ACBlend xs.n=\"super\"/></CBlend_Normal>",
                      "<CBlend_NotSupported xs.n=\"blend\"><ACBlend xs.n=\"super\"/><s xs.n=\"blendKey\">pass</s></CBlend_NotSupported>");
        xml.replace(start, end - start, group);
        CPsdSourceCatalog result; QString error;
        QVERIFY2(cmo3::CCmo3ArtworkReader::ReadBytes(archive(xml, pixels), {}, result, &error), qPrintable(error));
        QCOMPARE(result.documents[0].layers[1].blendMode, QByteArray("pass"));
        QVERIFY(!hasDiagnostic(result.documents[0], "unsupported-blend", CPsdSourceDiagnostic::Severity::Error));
    }

    void corruptPngIsRejectedWithoutChangingOutput()
    {
        auto pixels = png(); const QString xml = project(pixels); pixels[29] ^= 1;
        CPsdSourceCatalog result; result.sourcePath = "retained"; QString error;
        QVERIFY(!cmo3::CCmo3ArtworkReader::ReadBytes(archive(xml, pixels), {}, result, &error));
        QCOMPARE(result.sourcePath, QString("retained")); QVERIFY(error.contains("校验和"));
    }

    void borrowedInputsAreFrozenBeforeCallbacks()
    {
        const auto pixels = png(); QByteArray storage = archive(project(pixels), pixels);
        const QByteArray expected = storage;
        QString pathStorage = "borrowed.cmo3";
        const auto input = QByteArray::fromRawData(storage.data(), storage.size());
        const auto path = QString::fromRawData(pathStorage.data(), pathStorage.size());
        CExportControl control; bool called = false;
        QVERIFY(control.SetHandler([&](const CExportControl::Progress&, QString*) {
            if (!called) { storage.fill('x'); pathStorage.fill('x'); called = true; }
            return true;
        }));
        CPsdSourceCatalog result; QString error;
        QVERIFY2(cmo3::CCmo3ArtworkReader::ReadBytes(input, path, result, &error, &control), qPrintable(error));
        QVERIFY(called); QCOMPARE(result.sourcePath, QString("borrowed.cmo3"));
        QCOMPARE(result.sourceDigest, QCryptographicHash::hash(expected, QCryptographicHash::Sha256));
        QCOMPARE(result.documents[0].layers[0].png, pixels);
    }
    void cancelAndFileReadAreAtomic()
    {
        const auto pixels = png(); const auto input = archive(project(pixels), pixels);
        CExportControl control; QVERIFY(control.SetHandler([](const CExportControl::Progress&, QString*) { return false; }));
        CPsdSourceCatalog result; result.sourcePath = "retained"; QString error;
        QVERIFY(!cmo3::CCmo3ArtworkReader::ReadBytes(input, {}, result, &error, &control));
        QCOMPARE(result.sourcePath, QString("retained"));
        QTemporaryDir directory; const QString path = directory.filePath("工程.cmo3");
        QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write(input), input.size()); file.close();
        QVERIFY2(cmo3::CCmo3ArtworkReader::ReadFile(path, result, &error), qPrintable(error));
        QCOMPARE(result.sourcePath, QFileInfo(path).canonicalFilePath());
        QCOMPARE(result.sourceDigest, QCryptographicHash::hash(input, QCryptographicHash::Sha256));
    }
};
QTEST_GUILESS_MAIN(Cmo3ArtworkReaderTests)
#include "cmo3_artwork_reader_tests.moc"
