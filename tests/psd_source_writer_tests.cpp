#include "psd/cpsdsourcewriter.h"
#include "psd/cpsdreader.h"
#include "psd/psdSamples/Exporter/PsdExportDocument.h"
#include "psd/psdSamples/Exporter/PsdExport.h"
#include "psd/psdSamples/Interfaces/PsdMallocAllocator.h"
#include "std/cexportcontrol.h"
#include <QtTest>
#include <QBuffer>
#include <QFile>
#include <QTemporaryDir>
#include <QtEndian>
#include <limits>

namespace {
class CountingAllocator : public psd::Allocator
{
public:
    qint64 bytes = 0;
    int untilFailure = -1;
private:
    void* DoAllocate(size_t size, size_t alignment) override
    {
        if (untilFailure == 0) return nullptr;
        if (untilFailure > 0) --untilFailure;
        void* pointer = backing.Allocate(size, alignment);
        if (pointer) { allocations.insert(pointer, size); bytes += size; }
        return pointer;
    }
    void DoFree(void* pointer) override
    {
        bytes -= allocations.take(pointer);
        backing.Free(pointer);
    }
    psd::MallocAllocator backing;
    QMap<void*, size_t> allocations;
};

QByteArray png(const QImage& image)
{
    QByteArray result;
    QBuffer buffer(&result);
    if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "PNG")) return {};
    return result;
}

QImage pixels()
{
    QImage result(3, 2, QImage::Format_RGBA8888);
    const uchar values[]{17, 23, 199, 0, 231, 11, 53, 127, 2, 253, 137, 255,
                         41, 223, 13, 1, 210, 59, 157, 249, 19, 7, 211, 0};
    for (int y = 0; y < 2; ++y) memcpy(result.scanLine(y), values + y * 12, 12);
    return result;
}

CPsdSourceDocument document()
{
    CPsdSourceDocument result;
    result.id = "source";
    result.name = QString::fromUtf8("源素材😀");
    result.canvasSize = QSize(8, 6);
    result.iccProfile = QByteArray("original-ICC\0bytes", 18);
    CPsdSourceLayer layer;
    layer.id = "pixels";
    layer.name = QString::fromUtf8("左目😀");
    layer.bounds = QRect(-2, 4, 3, 2);
    layer.png = png(pixels());
    layer.visible = false;
    layer.opacity = 117;
    layer.clipping = true;
    layer.blendMode = "mul ";
    layer.externalId = 0xfedcba98;
    result.layers.append(layer);
    return result;
}

QByteArray read(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

bool seed(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write("previous") == 8;
}

CPsdSourceLayer group(const QString& id, int parent, const QString& name)
{
    CPsdSourceLayer result;
    result.kind = CPsdSourceLayer::Kind::Group;
    result.id = id;
    result.parentIndex = parent;
    result.name = name;
    result.blendMode = "pass";
    return result;
}
}

class PsdSourceWriterTests : public QObject
{
    Q_OBJECT
private slots:
    void sourceChannelsAndAttributesArePreserved()
    {
        QTemporaryDir directory;
        auto source = document();
        QString error;
        QVERIFY2(CPsdSourceWriter::Validate(source, &error), qPrintable(error));
        const auto path = directory.filePath("source.psd");
        QVERIFY2(CPsdSourceWriter::Write(path, source, nullptr, &error), qPrintable(error));
        CPsdSnapshot decoded;
        QVERIFY2(CPsdReader::ReadFile(path, decoded, &error), qPrintable(error));
        QCOMPARE(decoded.canvasSize, source.canvasSize);
        QCOMPARE(decoded.iccProfile, source.iccProfile);
        QCOMPARE(decoded.layers.size(), 1);
        const auto& expected = source.layers[0];
        const auto& actual = decoded.layers[0];
        QCOMPARE(actual.name, expected.name);
        QCOMPARE(actual.bounds, expected.bounds);
        QCOMPARE(actual.visible, expected.visible);
        QCOMPARE(actual.opacity, expected.opacity);
        QCOMPARE(actual.clipping, expected.clipping);
        QCOMPARE(actual.blendMode, expected.blendMode);
        QCOMPARE(actual.layerId, expected.externalId);
        QCOMPARE(actual.image.convertToFormat(QImage::Format_RGBA8888), pixels());
        QVERIFY(read(path).contains(QByteArray::fromHex("3842494d04210000000000110000000100")));
    }

    void preorderedGroupsAndEmptyRecordsKeepOrder()
    {
        QTemporaryDir directory;
        auto source = document();
        const auto raster = source.layers[0];
        source.layers.clear();
        auto outer = group("outer", -1, "repeated");
        outer.bounds = QRect(-7, -5, 11, 13);
        outer.visible = false;
        outer.opacity = 61;
        outer.externalId = 19;
        source.layers.append(outer);
        auto first = raster;
        first.id = "first";
        first.parentIndex = 0;
        source.layers.append(first);
        auto inner = group("inner", 0, QString::fromUtf8("空群😀"));
        inner.opacity = 173;
        inner.blendMode = "norm";
        inner.clipping = true;
        source.layers.append(inner);
        CPsdSourceLayer empty;
        empty.id = "empty";
        empty.name = "repeated";
        empty.bounds = QRect(-25, 27, 0, 0);
        empty.parentIndex = 2;
        source.layers.append(empty);
        auto last = raster;
        last.id = "last";
        last.name = "repeated";
        source.layers.append(last);
        source.layers.append(group("empty-group", -1, "repeated"));
        QString error;
        const auto path = directory.filePath("tree.psd");
        QVERIFY2(CPsdSourceWriter::Write(path, source, nullptr, &error), qPrintable(error));
        CPsdSnapshot decoded;
        QVERIFY2(CPsdReader::ReadFile(path, decoded, &error), qPrintable(error));
        QCOMPARE(decoded.layers.size(), 9);
        QCOMPARE(decoded.displayOrder.size(), source.layers.size());
        for (int index = 0; index < source.layers.size(); ++index) {
            const auto& expected = source.layers[index];
            const auto& actual = decoded.layers[decoded.displayOrder[index]];
            QCOMPARE(actual.name, expected.name);
            QCOMPARE(actual.bounds, expected.bounds);
            QCOMPARE(actual.visible, expected.visible);
            QCOMPARE(actual.opacity, expected.opacity);
            QCOMPARE(actual.clipping, expected.clipping);
            QCOMPARE(actual.blendMode, expected.blendMode);
            QCOMPARE(actual.layerId, expected.externalId);
            const int expectedParent = expected.parentIndex < 0 ? -1 : decoded.displayOrder[expected.parentIndex];
            QCOMPARE(actual.parentIndex, expectedParent);
            if (expected.png.isEmpty()) QVERIFY(actual.image.isNull());
            else QCOMPARE(actual.image.convertToFormat(QImage::Format_RGBA8888), pixels());
        }
    }

    void rgbAndMissingIccDoNotInventAProfile()
    {
        auto source = document();
        source.iccProfile.clear();
        const QImage rgb = pixels().convertToFormat(QImage::Format_RGB888);
        source.layers[0].png = png(rgb);
        QTemporaryDir directory;
        const auto path = directory.filePath("rgb.psd");
        QString error;
        QVERIFY2(CPsdSourceWriter::Write(path, source, nullptr, &error), qPrintable(error));
        CPsdSnapshot decoded;
        QVERIFY2(CPsdReader::ReadFile(path, decoded, &error), qPrintable(error));
        QVERIFY(decoded.iccProfile.isEmpty());
        QCOMPARE(decoded.layers[0].image.convertToFormat(QImage::Format_RGB888), rgb);
        for (int y = 0; y < rgb.height(); ++y) for (int x = 0; x < rgb.width(); ++x)
            QCOMPARE(qAlpha(decoded.layers[0].image.pixel(x, y)), 255);
    }

    void twoForegroundGroupsKeepIndependentSiblingOrder()
    {
        CPsdSourceDocument source;
        source.id = "order";
        source.canvasSize = QSize(1, 1);
        const auto raster = [](const QString& name, int parent, QRgb color) {
            CPsdSourceLayer layer;
            layer.id = name;
            layer.name = name;
            layer.parentIndex = parent;
            layer.bounds = QRect(0, 0, 1, 1);
            QImage image(1, 1, QImage::Format_ARGB32);
            image.setPixel(0, 0, color);
            layer.png = png(image);
            return layer;
        };
        source.layers = {group("top", -1, "Top"), raster("Red", 0, qRgb(255, 0, 0)),
            raster("Green", 0, qRgb(0, 255, 0)), group("bottom", -1, "Bottom"),
            raster("Blue", 3, qRgb(0, 0, 255)), raster("Yellow", 3, qRgb(255, 255, 0))};
        QTemporaryDir directory;
        const auto path = directory.filePath("order.psd");
        QString error;
        QVERIFY2(CPsdSourceWriter::Write(path, source, nullptr, &error), qPrintable(error));
        CPsdSnapshot decoded;
        QVERIFY2(CPsdReader::ReadFile(path, decoded, &error), qPrintable(error));
        QStringList recordNames, foregroundNames;
        for (const auto& layer : decoded.layers) recordNames.append(layer.name);
        for (int index : decoded.displayOrder) foregroundNames.append(decoded.layers[index].name);
        QCOMPARE(recordNames, QStringList({"</Layer group>", "Yellow", "Blue", "Bottom",
                                          "</Layer group>", "Green", "Red", "Top"}));
        QCOMPARE(foregroundNames, QStringList({"Top", "Red", "Green", "Bottom", "Blue", "Yellow"}));
        QCOMPARE(decoded.layers[6].parentIndex, 7);
        QCOMPARE(decoded.layers[5].parentIndex, 7);
        QCOMPARE(decoded.layers[2].parentIndex, 3);
        QCOMPARE(decoded.layers[1].parentIndex, 3);
    }

    void endpointBoundsAreCheckedBeforeNarrowArithmetic_data()
    {
        QTest::addColumn<QRect>("bounds");
        const int minimum = std::numeric_limits<int>::min(), maximum = std::numeric_limits<int>::max();
        const auto add = [](const char* name, int left, int top, int right, int bottom) {
            QRect bounds;
            bounds.setCoords(left, top, right, bottom);
            QTest::newRow(name) << bounds;
        };
        add("horizontal-span", minimum, 0, maximum, 0);
        add("vertical-span", 0, minimum, 0, maximum);
        add("both-spans", minimum, minimum, maximum, maximum);
        add("horizontal-reversed", maximum, 0, minimum, 0);
        add("vertical-reversed", 0, maximum, 0, minimum);
        add("exclusive-right", maximum, 0, maximum, 0);
        add("exclusive-bottom", 0, maximum, 0, maximum);
    }

    void endpointBoundsAreCheckedBeforeNarrowArithmetic()
    {
        QFETCH(QRect, bounds);
        auto source = document();
        source.layers[0].bounds = bounds;
        QImage image(1, 1, QImage::Format_RGBA8888);
        image.fill(Qt::red);
        source.layers[0].png = png(image);
        QTemporaryDir directory;
        const auto path = directory.filePath("bounds.psd");
        QVERIFY(seed(path));
        QString error;
        QVERIFY(!CPsdSourceWriter::Validate(source, &error));
        QVERIFY(error.contains(QStringLiteral("边界")));
        CExportControl control;
        bool called = false;
        QVERIFY(control.SetHandler([&](const auto&, QString*) { called = true; return true; }));
        QVERIFY(!CPsdSourceWriter::Write(path, source, &control, &error));
        QVERIFY(!called);
        QCOMPARE(read(path), QByteArray("previous"));
    }

    void representableExtremePositionsAndEmptyBoundsRoundTrip()
    {
        const int minimum = std::numeric_limits<int>::min(), maximum = std::numeric_limits<int>::max();
        for (int coordinate : {minimum, maximum - 1}) {
            auto source = document();
            QImage image(1, 1, QImage::Format_RGBA8888);
            image.fill(Qt::red);
            source.layers[0].png = png(image);
            source.layers[0].bounds.setCoords(coordinate, coordinate, coordinate, coordinate);
            QTemporaryDir directory;
            const auto path = directory.filePath("extreme.psd");
            QString error;
            QVERIFY2(CPsdSourceWriter::Write(path, source, nullptr, &error), qPrintable(error));
            CPsdSnapshot decoded;
            QVERIFY2(CPsdReader::ReadFile(path, decoded, &error), qPrintable(error));
            QCOMPARE(decoded.layers[0].bounds, source.layers[0].bounds);
            QCOMPARE(decoded.layers[0].image.convertToFormat(QImage::Format_RGBA8888), image);
        }
        auto source = document();
        source.layers[0].bounds.setCoords(minimum + 1, minimum + 1, minimum, minimum);
        source.layers[0].png.clear();
        QTemporaryDir directory;
        const auto path = directory.filePath("empty-extreme.psd");
        QString error;
        QVERIFY2(CPsdSourceWriter::Write(path, source, nullptr, &error), qPrintable(error));
        CPsdSnapshot decoded;
        QVERIFY2(CPsdReader::ReadFile(path, decoded, &error), qPrintable(error));
        QCOMPARE(decoded.layers[0].bounds, source.layers[0].bounds);
        QVERIFY(decoded.layers[0].image.isNull());
    }

    void unknownAndMalformedDocumentsFailAtomically_data()
    {
        QTest::addColumn<int>("kind");
        for (int kind = 0; kind < 21; ++kind) QTest::newRow(qPrintable(QString::number(kind))) << kind;
    }

    void unknownAndMalformedDocumentsFailAtomically()
    {
        QFETCH(int, kind);
        auto source = document();
        switch (kind) {
        case 0: source.canvasSize = QSize(30001, 1); break;
        case 1: source.layers[0].bounds.setWidth(4); break;
        case 2: source.layers[0].blendMode = "xxxx"; break;
        case 3: source.layers[0].blendMode = "pass"; break;
        case 4: source.layers[0].parentIndex = 0; break;
        case 5: source.layers[0].png.clear(); break;
        case 6: source.layers[0].bounds = QRect(0, 0, 0, 2); source.layers[0].png.clear(); break;
        case 7: source.layers[0].bounds = QRect(0, 0, 0, 0); break;
        case 8: source.layers[0].name = QString(QChar(0xd800)); break;
        case 9: source.layers[0].name.append(QChar(0)); break;
        case 10: source.layers.append(source.layers[0]); break;
        case 11: source.layers[0].bounds.moveLeft(std::numeric_limits<int>::max() - 2); break;
        case 12: source.layers[0].png = source.layers[0].png.left(40); break;
        case 13: source.layers[0].png[24] = char(16); break;
        case 14: source.layers[0].png[25] = char(3); break;
        case 15: source.layers[0].kind = CPsdSourceLayer::Kind::Group; break;
        case 16: source.layers[0].kind = static_cast<CPsdSourceLayer::Kind>(42); break;
        case 17: source.diagnostics.append({CPsdSourceDiagnostic::Severity::Error, "missing", "missing pixels", "pixels"}); break;
        case 18: source.layers[0].name = QString(32769, QLatin1Char('a')); break;
        case 19: {
            source.layers = {group("a", -1, "a"), group("b", -1, "b"), source.layers[0]};
            source.layers[2].parentIndex = 0;
            break;
        }
        case 20: {
            source.layers.clear();
            for (int index = 0; index < 129; ++index) source.layers.append(group(QString::number(index), index - 1, "deep"));
            break;
        }
        }
        QTemporaryDir directory;
        const auto path = directory.filePath("existing.psd");
        QVERIFY(seed(path));
        QString error;
        QVERIFY(!CPsdSourceWriter::Validate(source, &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(!CPsdSourceWriter::Write(path, source, nullptr, &error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(read(path), QByteArray("previous"));
    }

    void knownBlendModesAreRetained_data()
    {
        QTest::addColumn<QByteArray>("mode");
        for (const auto& mode : {"norm", "diss", "dark", "mul ", "idiv", "lbrn", "dkCl", "lite", "scrn", "div ",
             "lddg", "lgCl", "over", "sLit", "hLit", "vLit", "lLit", "pLit", "hMix", "diff", "smud", "fsub", "fdiv",
             "hue ", "sat ", "colr", "lum "}) QTest::newRow(mode) << QByteArray(mode);
    }

    void knownBlendModesAreRetained()
    {
        QFETCH(QByteArray, mode);
        auto source = document();
        source.layers[0].blendMode = mode;
        QTemporaryDir directory;
        const auto path = directory.filePath("blend.psd");
        QString error;
        QVERIFY2(CPsdSourceWriter::Write(path, source, nullptr, &error), qPrintable(error));
        CPsdSnapshot decoded;
        QVERIFY2(CPsdReader::ReadFile(path, decoded, &error), qPrintable(error));
        QCOMPARE(decoded.layers[0].blendMode, mode);
    }

    void borrowedInputsAreOwnedBeforeTheFirstCallback()
    {
        QTemporaryDir directory;
        QString path = directory.filePath("frozen.psd");
        const QString expectedPath = path;
        auto source = document();
        QString name = source.layers[0].name;
        QByteArray data = source.layers[0].png;
        QByteArray icc = source.iccProfile;
        QByteArray blend = source.layers[0].blendMode;
        source.layers[0].name = QString::fromRawData(name.constData(), name.size());
        source.layers[0].png = QByteArray::fromRawData(data.constData(), data.size());
        source.iccProfile = QByteArray::fromRawData(icc.constData(), icc.size());
        source.layers[0].blendMode = QByteArray::fromRawData(blend.constData(), blend.size());
        const QString borrowedPath = QString::fromRawData(path.constData(), path.size());
        CExportControl control;
        bool called = false;
        QVERIFY(control.SetHandler([&](const auto&, QString*) {
            if (!called) {
                called = true;
                name.fill(QLatin1Char('x')); data.fill('x'); icc.fill('x'); blend.fill('x'); path.fill(QLatin1Char('x'));
                source.layers.clear(); source.canvasSize = QSize();
            }
            return true;
        }));
        QString error;
        QVERIFY2(CPsdSourceWriter::Write(borrowedPath, source, &control, &error), qPrintable(error));
        QVERIFY(called);
        QVERIFY(control.IsCommitted());
        CPsdSnapshot decoded;
        QVERIFY2(CPsdReader::ReadFile(expectedPath, decoded, &error), qPrintable(error));
        QCOMPARE(decoded.layers[0].name, document().layers[0].name);
        QCOMPARE(decoded.layers[0].image.convertToFormat(QImage::Format_RGBA8888), pixels());
        QCOMPARE(decoded.iccProfile, document().iccProfile);
        QCOMPARE(decoded.layers[0].blendMode, QByteArray("mul "));
    }

    void cancellationKeepsPreviousDestination_data()
    {
        QTest::addColumn<int>("phase");
        for (int value : {0, 3, 4, 5}) QTest::newRow(qPrintable(QString::number(value))) << value;
    }

    void cancellationKeepsPreviousDestination()
    {
        QFETCH(int, phase);
        QTemporaryDir directory;
        const auto path = directory.filePath("cancel.psd");
        QVERIFY(seed(path));
        CExportControl control;
        QVERIFY(control.SetHandler([&](const auto& progress, QString*) {
            if (int(progress.phase) == phase) control.Cancel();
            return true;
        }));
        QString error;
        QVERIFY(!CPsdSourceWriter::Write(path, document(), &control, &error));
        QVERIFY(control.WasCanceled());
        QVERIFY(!control.IsCommitted());
        QCOMPARE(read(path), QByteArray("previous"));
    }

    void sharedInputCannotBypassAggregatePixelBudget()
    {
        auto source = document();
        QImage large(2048, 2048, QImage::Format_RGBA8888);
        large.fill(Qt::transparent);
        const auto encoded = png(large);
        source.layers.clear();
        for (int index = 0; index < 65; ++index) {
            auto layer = document().layers[0];
            layer.id = QString::number(index);
            layer.bounds = QRect(0, 0, 2048, 2048);
            layer.png = encoded;
            source.layers.append(layer);
        }
        QString error;
        QVERIFY(!CPsdSourceWriter::Validate(source, &error));
        QVERIFY(error.contains(QStringLiteral("预算")));
    }

    void emptyDocumentAndEmptyNamesAreValid()
    {
        QTemporaryDir directory;
        for (bool empty : {false, true}) {
            auto source = document();
            if (empty) source.layers.clear();
            else source.layers[0].name.clear();
            const auto path = directory.filePath(empty ? "empty.psd" : "unnamed.psd");
            QString error;
            QVERIFY2(CPsdSourceWriter::Write(path, source, nullptr, &error), qPrintable(error));
            CPsdSnapshot decoded;
            QVERIFY2(CPsdReader::ReadFile(path, decoded, &error), qPrintable(error));
            QCOMPARE(decoded.layers.size(), source.layers.size());
            if (!empty) QVERIFY(decoded.layers[0].name.isEmpty());
        }
    }

    void sourceCompressionRetainsOnlyItsEncodedAllocation()
    {
        for (bool compact : {false, true}) {
            CountingAllocator allocator;
            auto* value = psd::CreateExportDocument(&allocator, 100, 100, 8, psd::exportColorMode::RGB);
            QVERIFY(value);
            if (compact) psd::SetSourceDocumentOptions(value);
            const auto index = psd::AddLayer(value, &allocator, "a");
            const qint64 baseline = allocator.bytes;
            const QByteArray plane(10000, char(197));
            QVERIFY(psd::UpdateLayer(value, &allocator, index, psd::exportChannel::RED, 0, 0, 100, 100,
                reinterpret_cast<const uint8_t*>(plane.constData()), psd::compressionType::RLE, {}));
            QCOMPARE(value->layers[index].channelSize[0], quint32(400));
            QCOMPARE(allocator.bytes - baseline, compact ? qint64(400) : qint64(20200));
            const qint64 accepted = allocator.bytes;
            void* original = value->layers[index].channelData[0];
            for (int failure = 0; failure < (compact ? 4 : 3); ++failure) {
                allocator.untilFailure = failure;
                QVERIFY(!psd::UpdateLayer(value, &allocator, index, psd::exportChannel::RED, 0, 0, 100, 100,
                    reinterpret_cast<const uint8_t*>(plane.constData()), psd::compressionType::RLE, {}));
                QCOMPARE(value->layers[index].channelData[0], original);
                QCOMPARE(allocator.bytes, accepted);
            }
            allocator.untilFailure = -1;
            psd::DestroyExportDocument(value, &allocator);
            QCOMPARE(allocator.bytes, qint64(0));
        }
    }

    void groupEndRecordsCountAgainstCapacityBeforeWriting()
    {
        auto source = document();
        source.layers.clear();
        for (int index = 0; index < 3501; ++index) source.layers.append(group(QString::number(index), -1, "group"));
        QTemporaryDir directory;
        const auto path = directory.filePath("capacity.psd");
        QVERIFY(seed(path));
        CExportControl control;
        bool called = false;
        QVERIFY(control.SetHandler([&](const auto&, QString*) { called = true; return true; }));
        QString error;
        QVERIFY(!CPsdSourceWriter::Write(path, source, &control, &error));
        QVERIFY(!called);
        QVERIFY(error.contains(QStringLiteral("结束记录")));
        QCOMPARE(read(path), QByteArray("previous"));
    }
};

QTEST_GUILESS_MAIN(PsdSourceWriterTests)
#include "psd_source_writer_tests.moc"
