#include "psd/cpsdcomposite.h"
#include "psd/cpsdlist.h"
#include "psd/cpsdreader.h"
#include "psd/psdSamples/Exporter/PsdExportDocument.h"
#include "std/cexportcontrol.h"
#include <QtTest>
#include <QTemporaryDir>
#include <QFile>
#include <QScopeGuard>
#include <array>
#include <vector>

namespace {
struct Fixture
{
    psd::MallocAllocator allocator;
    psd::ExportDocument* document;
    explicit Fixture(int width = 2, int height = 2)
        : document(psd::CreateExportDocument(&allocator, width, height, 8, psd::exportColorMode::RGB)) {}
    ~Fixture() { psd::DestroyExportDocument(document, &allocator); }
    unsigned int Layer(const QRect& rect, std::array<uint8_t, 4> color, uint32_t blend = 0x6e6f726d,
                       uint8_t opacity = 255, psd::compressionType::Enum compression = psd::compressionType::RLE)
    {
        const auto index = psd::AddLayer(document, &allocator, "layer");
        psd::SetLayerBlendMode(document, index, blend); psd::SetLayerOpacity(document, index, opacity);
        const psd::exportChannel::Enum channels[]{psd::exportChannel::RED, psd::exportChannel::GREEN,
                                                 psd::exportChannel::BLUE, psd::exportChannel::ALPHA};
        for (int channel = 0; channel < 4; ++channel) {
            std::vector<uint8_t> values(size_t(rect.width()) * rect.height(), color[size_t(channel)]);
            if (!psd::UpdateLayer(document, &allocator, index, channels[channel], rect.x(), rect.y(),
                                 rect.x() + rect.width(), rect.y() + rect.height(), values.data(), compression, {}))
                qFatal("Cannot create composite fixture");
        }
        return index;
    }
    void BeginGroup() { psd::AddLayerGroupStart(document, &allocator, "end"); }
    void EndGroup(bool pass, bool visible = true)
    {
        const auto index = psd::AddLayerGroupEnd(document, &allocator, "folder");
        psd::ExportLayerAttributes attributes;
        attributes.blendModeKey = pass ? 0x70617373 : 0x6e6f726d; attributes.visible = visible;
        if (!psd::SetLayerAttributes(document, index, attributes)) qFatal("Invalid group fixture");
    }
};
std::array<QByteArray, 4> Collect(CPsdComposite& composite, int width, int height, QString* error)
{
    std::array<QByteArray, 4> result;
    for (auto& plane : result) plane.resize(width * height);
    int next = 0;
    const bool ok = composite.Write([&](uint32_t row, uint32_t count, const uint8_t* red, const uint8_t* green,
                                       const uint8_t* blue, const uint8_t* alpha) {
        if (int(row) != next || count == 0 || row + count > uint32_t(height)) return false;
        const uint8_t* data[]{red, green, blue, alpha};
        for (int channel = 0; channel < 4; ++channel)
            std::memcpy(result[size_t(channel)].data() + row * width, data[channel], size_t(count) * width);
        next += int(count); return true;
    }, error);
    if (!ok || next != height) return {};
    return result;
}
QByteArray Read(const QString& path)
{
    QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}
}

class PsdCompositeTests : public QObject
{
    Q_OBJECT
private slots:
    void canonicalBlend_data()
    {
        QTest::addColumn<quint32>("blend"); QTest::addColumn<QByteArray>("rgb");
        QTest::newRow("normal") << quint32(0x6e6f726d) << QByteArray::fromHex("804020");
        QTest::newRow("multiply") << quint32(0x6d756c20) << QByteArray::fromHex("402010");
        QTest::newRow("additive") << quint32(0x6c646467) << QByteArray::fromHex("ffc0a0");
    }
    void canonicalBlend()
    {
        QFETCH(quint32, blend); QFETCH(QByteArray, rgb);
        Fixture fixture; fixture.Layer({0, 0, 2, 2}, {128, 128, 128, 255});
        fixture.BeginGroup(); fixture.Layer({0, 0, 2, 2}, {128, 64, 32, 255}, blend); fixture.EndGroup(true);
        CPsdComposite composite; QString error;
        QVERIFY2(composite.Prepare(*fixture.document, nullptr, &error), qPrintable(error)); QVERIFY(composite.IsSupported());
        const auto planes = Collect(composite, 2, 2, &error);
        QVERIFY2(!planes[0].isEmpty(), qPrintable(error));
        for (int channel = 0; channel < 3; ++channel) QCOMPARE(planes[size_t(channel)], QByteArray(4, rgb[channel]));
        QCOMPARE(planes[3], QByteArray(4, char(255)));
        QVERIFY(!composite.Write([](auto, auto, auto, auto, auto, auto) { return true; }, &error));
    }
    void whiteMatteAndEmptyCanvas()
    {
        Fixture fixture; fixture.Layer({0, 0, 1, 1}, {255, 0, 0, 204});
        CPsdComposite composite; QString error; QVERIFY(composite.Prepare(*fixture.document, nullptr, &error));
        const auto pixels = Collect(composite, 2, 2, &error); QVERIFY2(!pixels[0].isEmpty(), qPrintable(error));
        QCOMPARE(quint8(pixels[0][0]), quint8(255)); QCOMPARE(quint8(pixels[1][0]), quint8(51));
        QCOMPARE(quint8(pixels[2][0]), quint8(51)); QCOMPARE(quint8(pixels[3][0]), quint8(204));
        for (int i = 1; i < 4; ++i) {
            for (int c = 0; c < 3; ++c) QCOMPARE(quint8(pixels[size_t(c)][i]), quint8(255));
            QCOMPARE(quint8(pixels[3][i]), quint8(0));
        }
        Fixture empty; CPsdComposite transparent; QVERIFY(transparent.Prepare(*empty.document, nullptr, &error));
        const auto zero = Collect(transparent, 2, 2, &error); QCOMPARE(zero[3], QByteArray(4, '\0'));
        QCOMPARE(zero[0], QByteArray(4, char(255)));
    }
    void rowBandsAndOffCanvasBounds_data()
    {
        QTest::addColumn<bool>("raw"); QTest::newRow("rle") << false; QTest::newRow("raw") << true;
    }
    void rowBandsAndOffCanvasBounds()
    {
        QFETCH(bool, raw); Fixture fixture(3, 137);
        fixture.Layer({-2, -3, 4, 142}, {17, 31, 83, 255}, 0x6e6f726d, 255,
                      raw ? psd::compressionType::RAW : psd::compressionType::RLE);
        fixture.Layer({20, 20, 1, 1}, {255, 0, 0, 255});
        CPsdComposite composite; QString error; QVERIFY(composite.Prepare(*fixture.document, nullptr, &error));
        const auto output = Collect(composite, 3, 137, &error); QVERIFY2(!output[0].isEmpty(), qPrintable(error));
        for (int y = 0; y < 137; ++y) for (int x = 0; x < 3; ++x) {
            QCOMPARE(quint8(output[0][y * 3 + x]), quint8(x < 2 ? 17 : 255));
            QCOMPARE(quint8(output[3][y * 3 + x]), quint8(x < 2 ? 255 : 0));
        }
    }
    void unsupportedFeaturesAndHiddenPassGroup()
    {
        Fixture isolated; isolated.BeginGroup(); isolated.Layer({0, 0, 2, 2}, {255, 0, 0, 255}); isolated.EndGroup(false);
        CPsdComposite composite; QString error; QVERIFY(composite.Prepare(*isolated.document, nullptr, &error)); QVERIFY(!composite.IsSupported());
        Fixture clipped; const auto index = clipped.Layer({0, 0, 2, 2}, {255, 0, 0, 255}); clipped.document->layers[index].clipping = 1;
        QVERIFY(composite.Prepare(*clipped.document, nullptr, &error)); QVERIFY(!composite.IsSupported());
        Fixture hidden; hidden.BeginGroup(); hidden.Layer({0, 0, 2, 2}, {255, 0, 0, 255}); hidden.EndGroup(true, false);
        QVERIFY(composite.Prepare(*hidden.document, nullptr, &error)); QVERIFY(composite.IsSupported());
        QCOMPARE(Collect(composite, 2, 2, &error)[3], QByteArray(4, '\0'));
    }
    void invalidEncodingAndGroupStructureFail()
    {
        Fixture fixture; const auto index = fixture.Layer({0, 0, 2, 2}, {17, 31, 83, 255});
        auto& plane = fixture.document->layers[index]; const auto size = plane.channelSize[0]; --plane.channelSize[0];
        CPsdComposite composite; QString error; QVERIFY(!composite.Prepare(*fixture.document, nullptr, &error)); QVERIFY(!error.isEmpty());
        plane.channelSize[0] = size; fixture.BeginGroup();
        QVERIFY(!composite.Prepare(*fixture.document, nullptr, &error)); QVERIFY(!error.isEmpty());
    }
    void cancellationAndSinkFailureAreNotSuccess()
    {
        Fixture fixture(3, 137); fixture.Layer({0, 0, 3, 137}, {17, 31, 83, 255});
        CPsdComposite composite; CExportControl control; QString error;
        QVERIFY(composite.Prepare(*fixture.document, &control, &error)); int bands = 0;
        QVERIFY(!composite.Write([&](auto, auto, auto, auto, auto, auto) { ++bands; control.Cancel(); return true; }, &error));
        QCOMPARE(bands, 1); QVERIFY(control.WasCanceled()); QVERIFY(!error.isEmpty());
        CPsdComposite failed; QVERIFY(failed.Prepare(*fixture.document, nullptr, &error));
        QVERIFY(!failed.Write([](auto, auto, auto, auto, auto, auto) { return false; }, &error)); QVERIFY(!error.isEmpty());
    }
    void compositeRejectsReentrantPreparation()
    {
        Fixture original(2, 137), replacement;
        original.Layer({0, 0, 2, 137}, {17, 31, 83, 255});
        CPsdComposite composite; QString error;
        QVERIFY(composite.Prepare(*original.document, nullptr, &error));
        bool accepted = false; int rows = 0;
        const bool written = composite.Write([&](auto, auto count, auto, auto, auto, auto) {
            if (!rows) accepted = composite.Prepare(*replacement.document, nullptr, &error);
            rows += int(count); return true;
        }, &error);
        QVERIFY(!accepted); QVERIFY(written); QCOMPARE(rows, 137);
        CExportControl control; bool attempted = false;
        QVERIFY(control.SetHandler([&](const auto&, QString*) {
            if (!attempted) { attempted = true; accepted = composite.Prepare(*replacement.document, nullptr, &error); }
            return true;
        }));
        QVERIFY(composite.Prepare(*original.document, &control, &error));
        QVERIFY(attempted); QVERIFY(!accepted);
        QCOMPARE(Collect(composite, 2, 137, &error)[0], QByteArray(274, char(17)));
    }
    void sinkLifetimeAndPreparationFailure()
    {
        Fixture fixture(2, 137); CPsdComposite composite; QString error;
        QVERIFY(composite.Prepare(*fixture.document, nullptr, &error));
        int rows = 0; psd::MergedImageSink sink;
        sink = [&](auto, auto count, auto, auto, auto, auto) { rows += int(count); sink = {}; return true; };
        QVERIFY(composite.Write(sink, &error)); QCOMPARE(rows, 137);
        fixture.document->width = 0;
        QVERIFY(!composite.Prepare(*fixture.document, nullptr, &error)); QVERIFY(!composite.IsSupported());
        QVERIFY(!composite.Write([](auto, auto, auto, auto, auto, auto) { return true; }, &error));
        fixture.document->width = 2;
        QVERIFY(composite.Prepare(*fixture.document, nullptr, &error));
        QVERIFY(!composite.Write([](auto, auto, auto, auto, auto, auto) -> bool { throw 7; }, &error));
        QVERIFY(!error.isEmpty());
    }
    void expensivePreviewFallsBackWithoutFailingDocument()
    {
        Fixture fixture(1000, 1000);
        std::vector<uint8_t> plane(1000000, 255);
        auto restore = qScopeGuard([&] {
            for (int i = 0; i < fixture.document->layerCount; ++i)
                for (int channel = 0; channel < 4; ++channel) fixture.document->layers[i].channelData[channel] = nullptr;
        });
        for (int i = 0; i < 1100; ++i) {
            const auto index = psd::AddLayer(fixture.document, &fixture.allocator, "layer");
            fixture.document->layers[index].right = 1000; fixture.document->layers[index].bottom = 1000;
            for (int channel = 0; channel < 4; ++channel) {
                fixture.document->layers[index].channelData[channel] = plane.data();
                fixture.document->layers[index].channelSize[channel] = uint32_t(plane.size());
            }
        }
        CPsdComposite composite; CExportControl control; QString error;
        QVERIFY(composite.Prepare(*fixture.document, &control, &error));
        QVERIFY(!composite.IsSupported()); QVERIFY(control.Checkpoint()); QVERIFY(error.isEmpty());
    }
    void writerRejectsMutationFromProgress_data()
    {
        QTest::addColumn<QString>("outer"); QTest::addColumn<QString>("inner");
        QTest::addColumn<bool>("cancelFirst");
        for (const QString& outer : {QString("setup"), QString("encoding"), QString("writing")})
            for (const QString& inner : {QString("setup"), QString("layer"), QString("empty"), QString("group-start"),
                                        QString("group-end"), QString("write"), QString("control")})
                for (bool cancelFirst : {false, true})
                    QTest::newRow(qPrintable(outer + "-" + inner + (cancelFirst ? "-cancel" : ""))) << outer << inner << cancelFirst;
    }
    void writerRejectsMutationFromProgress()
    {
        QFETCH(QString, outer); QFETCH(QString, inner);
        QFETCH(bool, cancelFirst);
        QTemporaryDir folder; const auto path = folder.filePath("existing.psd");
        QFile original(path); QVERIFY(original.open(QIODevice::WriteOnly)); QCOMPARE(original.write("original"), qint64(8)); original.close();
        CPsdList writer; CExportControl control; QImage image(2, 2, QImage::Format_ARGB32); image.fill(Qt::red);
        if (outer != "setup") QVERIFY(writer.SetPsd(path, 2, 2));
        if (outer == "writing") QVERIFY(writer.AddLayer("original", image, {0, 0, 2, 2}));
        writer.SetControl(&control); bool attempted = false, accepted = false;
        QVERIFY(control.SetHandler([&](const auto&, QString*) {
            if (attempted) return true;
            attempted = true;
            if (cancelFirst) control.Cancel();
            if (inner == "setup") accepted = writer.SetPsd(folder.filePath("replacement.psd"), 2, 2);
            else if (inner == "layer") accepted = writer.AddLayer("nested", image, {0, 0, 2, 2});
            else if (inner == "empty") accepted = writer.AddEmptyLayer("nested");
            else if (inner == "group-start") accepted = writer.AddGroupStart();
            else if (inner == "group-end") accepted = writer.AddGroupEnd("nested");
            else if (inner == "write") accepted = writer.CreatePsd();
            else { writer.SetControl(nullptr); accepted = writer.Control() != &control; }
            return true;
        }));
        const bool result = outer == "setup" ? writer.SetPsd(path, 2, 2) : outer == "encoding"
            ? writer.AddLayer("original", image, {0, 0, 2, 2}) : writer.CreatePsd();
        QVERIFY(attempted); QVERIFY(!accepted); QVERIFY(!result); QVERIFY(!control.IsCommitted());
        QCOMPARE(control.WasCanceled(), cancelFirst);
        QVERIFY(!writer.CreatePsd()); QCOMPARE(writer.Preview(), CPsdPreviewKind::Unknown);
        QCOMPARE(Read(path), QByteArray("original")); QVERIFY(!QFile::exists(folder.filePath("replacement.psd")));
    }
    void ordinaryWriterPublishesTransparentPreviewAndPassGroups()
    {
        QTemporaryDir folder; QVERIFY(folder.isValid()); const auto path = folder.filePath("preview.psd");
        CPsdList writer; QVERIFY(writer.SetPsd(path, 2, 2)); QVERIFY(writer.AddGroupStart());
        QImage image(1, 1, QImage::Format_ARGB32); image.fill(qRgba(255, 0, 0, 204));
        QVERIFY(writer.AddLayer("red", image, QRect(0, 0, 1, 1))); QVERIFY(writer.AddEmptyLayer("empty"));
        QVERIFY(writer.AddGroupEnd("folder")); QVERIFY(writer.CreatePsd()); QCOMPARE(writer.Preview(), CPsdPreviewKind::Composite);
        CPsdSnapshot snapshot; QString error;
        QVERIFY2(CPsdReader::ReadBytes(Read(path), snapshot, &error), qPrintable(error));
        QCOMPARE(snapshot.channels, 4); QVERIFY(snapshot.hasMergedTransparency);
        QCOMPARE(snapshot.mergedImage.pixelColor(0, 0), QColor(255, 0, 0, 204)); QCOMPARE(snapshot.mergedImage.pixelColor(1, 1).alpha(), 0);
        bool group = false; for (const auto& layer : snapshot.layers) if (layer.kind == CPsdLayer::Kind::Group) {
            group = true; QCOMPARE(layer.blendMode, QByteArray("pass"));
        }
        QVERIFY(group);
    }
    void unsupportedWriterKeepsLayersAndDeclaresMissingPreview()
    {
        QTemporaryDir folder; const auto path = folder.filePath("clipped.psd"); CPsdList writer;
        QVERIFY(writer.SetPsd(path, 2, 2)); QImage image(2, 2, QImage::Format_ARGB32); image.fill(Qt::red);
        QVERIFY(writer.AddLayer("clipped", image, {0, 0, 2, 2}, 127, 0x6e6f726d, true));
        QVERIFY(writer.CreatePsd()); QCOMPARE(writer.Preview(), CPsdPreviewKind::RebuildFromLayers);
        CPsdSnapshot snapshot; QString error; QVERIFY2(CPsdReader::ReadBytes(Read(path), snapshot, &error), qPrintable(error));
        QVERIFY(snapshot.mergedImage.isNull()); QCOMPARE(snapshot.layers.size(), 1);
        QCOMPARE(snapshot.layers[0].opacity, quint8(127)); QVERIFY(snapshot.layers[0].clipping);
        QCOMPARE(snapshot.layers[0].image.pixelColor(0, 0), QColor(Qt::red));
    }
};
QTEST_GUILESS_MAIN(PsdCompositeTests)
#include "psd_composite_tests.moc"
