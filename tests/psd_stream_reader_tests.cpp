#include "psd/cpsdreader.h"
#include "std/cexportcontrol.h"
#include "psd_original_fixture.h"
#include <QtTest>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <limits>

namespace {
using namespace psd_original_fixture;

struct Layer
{
    QRect bounds{0, 0, 2, 2};
    QString name = QStringLiteral("图层");
    int kind = 0;
    quint8 flags = 0, opacity = 255, clipping = 0;
    QVector<QPair<int, QByteArray>> channels{{0, QByteArray::fromHex("10f030e0")},
        {1, QByteArray::fromHex("20404080")}, {2, QByteArray::fromHex("306020a0")},
        {-1, QByteArray::fromHex("0080ff40")}};
    QMap<int, QByteArray> encoded;
};

QByteArray document(const QVector<Layer>& layers)
{
    QByteArray records, pixels;
    for (int index = 0; index < layers.size(); ++index) {
        const auto& layer = layers[index];
        records += u32(layer.bounds.y()) + u32(layer.bounds.x()) +
            u32(layer.bounds.y() + layer.bounds.height()) + u32(layer.bounds.x() + layer.bounds.width());
        records += u16(layer.channels.size());
        for (const auto& plane : layer.channels) {
            const auto encoded = layer.encoded.value(plane.first, u16(0) + plane.second);
            records += u16(quint16(plane.first)) + u32(encoded.size()); pixels += encoded;
        }
        const QByteArray mode = layer.kind == 1 ? "pass" : "norm";
        records += "8BIM" + mode;
        records += char(layer.opacity); records += char(layer.clipping); records += char(layer.flags); records += char(0);
        QByteArray extra = u32(0) + u32(0) + QByteArray(4, 0) + tag("luni", unicode(layer.name)) + tag("lyid", u32(index + 42));
        if (layer.kind) extra += tag("lsct", u32(layer.kind) + "8BIM" + mode);
        records += block(extra);
    }
    QByteArray info = u16(quint16(-layers.size())) + records + pixels;
    if (info.size() % 2) info += char(0);
    return QByteArray("8BPS") + u16(1) + QByteArray(6, 0) + u16(4) + u32(2) + u32(2) + u16(8) + u16(3) +
        u32(0) + u32(0) + block(block(info) + u32(0)) + u16(0) + QByteArray(12, char(255)) + QByteArray(4, 0);
}

QImage sentinel()
{
    QImage image(1, 1, QImage::Format_ARGB32); image.fill(qRgba(19, 37, 71, 127)); return image;
}

void compareDiagnostics(const QVector<CPsdDiagnostic>& actual, const QVector<CPsdDiagnostic>& expected)
{
    QCOMPARE(actual.size(), expected.size());
    for (int index = 0; index < actual.size(); ++index) {
        QCOMPARE(actual[index].code, expected[index].code); QCOMPARE(actual[index].message, expected[index].message);
    }
}

void compareDirectory(const CPsdDirectory& actual, const CPsdSnapshot& expected)
{
    QCOMPARE(actual.bytes, expected.bytes); QCOMPARE(actual.sha256, expected.sha256); QCOMPARE(actual.iccProfile, expected.iccProfile);
    QCOMPARE(actual.canonicalPath, expected.canonicalPath); QCOMPARE(actual.canvasSize, expected.canvasSize);
    QCOMPARE(actual.version, expected.version); QCOMPARE(actual.depth, expected.depth); QCOMPARE(actual.colorMode, expected.colorMode);
    QCOMPARE(actual.channels, expected.channels); QCOMPARE(actual.hasMergedTransparency, expected.hasMergedTransparency);
    QCOMPARE(actual.hasMergedPreview, !expected.mergedImage.isNull()); QCOMPARE(actual.displayOrder, expected.displayOrder);
    compareDiagnostics(actual.diagnostics, expected.diagnostics); QCOMPARE(actual.layers.size(), expected.layers.size());
    for (int index = 0; index < actual.layers.size(); ++index) {
        const auto& a = actual.layers[index]; const auto& e = expected.layers[index];
        QCOMPARE(a.recordIndex, e.recordIndex); QCOMPARE(a.parentIndex, e.parentIndex); QCOMPARE(a.kind, e.kind);
        QCOMPARE(a.layerId, e.layerId); QCOMPARE(a.name, e.name); QCOMPARE(a.bounds, e.bounds); QCOMPARE(a.opacity, e.opacity);
        QCOMPARE(a.blendMode, e.blendMode); QCOMPARE(a.visible, e.visible); QCOMPARE(a.clipping, e.clipping);
        QCOMPARE(a.pixelEquivalent, e.pixelEquivalent); compareDiagnostics(a.diagnostics, e.diagnostics);
        QCOMPARE(a.additionalInfo.size(), e.additionalInfo.size());
        for (int info = 0; info < a.additionalInfo.size(); ++info) {
            QCOMPARE(a.additionalInfo[info].key, e.additionalInfo[info].key);
            QCOMPARE(a.additionalInfo[info].data, e.additionalInfo[info].data);
        }
    }
}

bool writeFile(const QString& path, const QByteArray& bytes)
{
    QFile file(path); return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
}

class PsdStreamReaderTests : public QObject
{
    Q_OBJECT
private slots:
    void compressionMetadataAndPixelsMatchEager_data()
    {
        QTest::addColumn<int>("compression"); QTest::addColumn<bool>("effects"); QTest::addColumn<bool>("grouped");
        for (int compression = 0; compression < 4; ++compression) for (bool effects : {false, true}) for (bool grouped : {false, true})
            QTest::newRow(qPrintable(QStringLiteral("%1/%2/%3").arg(compression).arg(effects).arg(grouped))) << compression << effects << grouped;
    }
    void compressionMetadataAndPixelsMatchEager()
    {
        QFETCH(int, compression); QFETCH(bool, effects); QFETCH(bool, grouped);
        const auto bytes = fixture(compression, effects, !effects, QStringLiteral("原始图层😀"), grouped);
        CPsdSnapshot eager; CPsdReadDocument source; QString error;
        QVERIFY2(CPsdReader::ReadBytes(bytes, eager, &error), qPrintable(error));
        QVERIFY2(CPsdReader::OpenBytes(bytes, source, &error), qPrintable(error)); QVERIFY(source.IsValid());
        compareDirectory(*source.Directory(), eager);
        for (int index = 0; index < eager.layers.size(); ++index) {
            QImage image = sentinel(); QVERIFY2(source.ReadLayer(index, image, &error), qPrintable(error));
            QCOMPARE(image, eager.layers[index].image);
        }
        const int raster = grouped ? 1 : 0;
        QImage image; QVERIFY(source.ReadLayer(raster, image)); QCOMPARE(image.pixel(0, 0), qRgba(1, 17, 255, 0));
        QCOMPARE(source.Directory()->layers[raster].pixelEquivalent, !effects);
    }
    void missingRgbEmptyGroupsAndExtraChannels()
    {
        Layer divider; divider.kind = 3; divider.bounds = {}; divider.channels.clear(); divider.name = "end";
        Layer empty = divider; empty.kind = 0; empty.name = "empty";
        Layer missing; missing.channels.removeAt(1); missing.name = "missing";
        Layer extra; extra.channels.append({7, QByteArray(4, 0)}); extra.flags = 2; extra.opacity = 91; extra.clipping = 1;
        Layer group = divider; group.kind = 1; group.name = "folder";
        const auto bytes = document({divider, empty, missing, extra, group});
        CPsdSnapshot eager; CPsdReadDocument source; QString error;
        QVERIFY2(CPsdReader::ReadBytes(bytes, eager, &error), qPrintable(error));
        QVERIFY2(CPsdReader::OpenBytes(bytes, source, &error), qPrintable(error)); compareDirectory(*source.Directory(), eager);
        for (int index = 0; index < eager.layers.size(); ++index) {
            QImage image = sentinel(); QVERIFY2(source.ReadLayer(index, image, &error), qPrintable(error));
            QCOMPARE(image, eager.layers[index].image); QCOMPARE(image.isNull(), index != 3);
        }
        QVERIFY(!source.Directory()->layers[2].pixelEquivalent); QVERIFY(!source.Directory()->layers[3].pixelEquivalent);
    }
    void realMaskChannelsStayUnapplied()
    {
        const auto base = u32(0) + u32(0) + u32(2) + u32(2) + QByteArray::fromHex("ff10");
        const auto real = QByteArray::fromHex("00ff") + u32(0) + u32(0) + u32(1) + u32(2);
        const auto mask = base + real + QByteArray::fromHex("01cc");
        const auto bytes = maskFixture(mask); CPsdSnapshot eager; CPsdReadDocument source; QString error;
        QVERIFY2(CPsdReader::ReadBytes(bytes, eager, &error), qPrintable(error));
        QVERIFY2(CPsdReader::OpenBytes(bytes, source, &error), qPrintable(error)); compareDirectory(*source.Directory(), eager);
        QImage image; QVERIFY(source.ReadLayer(0, image)); QCOMPARE(image, eager.layers[0].image);
        QCOMPARE(image.pixel(0, 0), qRgba(127, 127, 127, 127)); QVERIFY(!source.Directory()->layers[0].pixelEquivalent);
    }
    void opaqueRgbAndRepeatedImagesRemainIndependent()
    {
        Layer layer; layer.channels.removeLast(); layer.opacity = 63;
        CPsdReadDocument source; QVERIFY(CPsdReader::OpenBytes(document({layer}), source));
        QImage first, second; QVERIFY(source.ReadLayer(0, first)); QVERIFY(source.ReadLayer(0, second));
        QCOMPARE(first.pixel(0, 0), qRgba(16, 32, 48, 255)); QCOMPARE(first, second);
        source = {}; first.fill(Qt::blue); QCOMPARE(second.pixel(0, 0), qRgba(16, 32, 48, 255));
    }
    void borrowedInputAndLimitsAreFrozenBeforeCallbacks()
    {
        auto bytes = fixture(3); const QByteArray expected(bytes.constData(), bytes.size());
        const auto borrowed = QByteArray::fromRawData(bytes.constData(), bytes.size());
        CPsdStreamLimits limits; CPsdReadDocument source; CExportControl control; bool called = false;
        QVERIFY(control.SetHandler([&](const auto&, QString*) {
            if (!called) { called = true; bytes.fill('x'); limits.maximumLayerWorkingBytes = 1; limits.document.maximumDecodedBytes = 1; }
            return true;
        }));
        QString error; QVERIFY2(CPsdReader::OpenBytes(borrowed, source, &error, &control, limits), qPrintable(error));
        QVERIFY(called); QCOMPARE(source.Directory()->bytes, expected);
        QCOMPARE(source.Directory()->sha256, QCryptographicHash::hash(expected, QCryptographicHash::Sha256));
        auto retained = source; source = {}; QImage image;
        QVERIFY(retained.ReadLayer(0, image)); QCOMPARE(image.pixel(0, 0), qRgba(1, 17, 255, 0));
    }
    void sourceReplacementDuringReadKeepsActiveData_data()
    {
        QTest::addColumn<bool>("replace"); QTest::newRow("clear") << false; QTest::newRow("replace") << true;
    }
    void sourceReplacementDuringReadKeepsActiveData()
    {
        QFETCH(bool, replace); CPsdReadDocument source; QVERIFY(CPsdReader::OpenBytes(fixture(3), source));
        QImage expected; QVERIFY(source.ReadLayer(0, expected)); CExportControl control; bool called = false, replaced = false;
        const auto replacement = fixture(0, false, true, QStringLiteral("replacement"));
        QVERIFY(control.SetHandler([&](const auto&, QString*) {
            if (!called) { called = true; source = {}; if (replace) replaced = CPsdReader::OpenBytes(replacement, source); }
            return true;
        }));
        QImage actual = sentinel(); QString error; QVERIFY2(source.ReadLayer(0, actual, &error, &control), qPrintable(error));
        QVERIFY(called); QCOMPARE(actual, expected); QCOMPARE(source.IsValid(), replace); QCOMPARE(replaced, replace);
        if (replace) QCOMPARE(source.Directory()->layers[0].name, QStringLiteral("replacement"));
    }
    void invalidIndicesAndCancellationKeepResults()
    {
        CPsdReadDocument source; QVERIFY(CPsdReader::OpenBytes(largeSolid(128, 130), source));
        const auto original = source; QString error;
        for (int index : {-1, 1, std::numeric_limits<int>::max()}) {
            QImage image = sentinel(); QVERIFY(!source.ReadLayer(index, image, &error)); QCOMPARE(image, sentinel()); QVERIFY(!error.isEmpty());
        }
        CPsdReadDocument invalid; QImage unchanged = sentinel();
        QVERIFY(!invalid.ReadLayer(0, unchanged, &error)); QCOMPARE(unchanged, sentinel());
        CExportControl readControl; int reads = 0;
        QVERIFY(readControl.SetHandler([&](const auto&, QString*) { if (++reads == 20) readControl.Cancel(); return true; }));
        QVERIFY(!source.ReadLayer(0, unchanged, &error, &readControl)); QVERIFY(readControl.WasCanceled());
        QCOMPARE(reads, 20); QCOMPARE(unchanged, sentinel()); QCOMPARE(source.Directory(), original.Directory());
        CExportControl openControl; int opens = 0;
        QVERIFY(openControl.SetHandler([&](const auto&, QString*) { if (++opens == 10) openControl.Cancel(); return true; }));
        QVERIFY(!CPsdReader::OpenBytes(fixture(2), source, &error, &openControl)); QVERIFY(openControl.WasCanceled());
        QCOMPARE(source.Directory(), original.Directory());
    }
    void badLateDataIsRejectedBeforePublishing_data()
    {
        QTest::addColumn<int>("compression"); QTest::addColumn<bool>("preview");
        for (int compression = 0; compression < 4; ++compression) for (bool preview : {false, true})
            QTest::newRow(qPrintable(QStringLiteral("%1/%2").arg(compression).arg(preview))) << compression << preview;
    }
    void badLateDataIsRejectedBeforePublishing()
    {
        QFETCH(int, compression); QFETCH(bool, preview);
        CPsdReadDocument source; QVERIFY(CPsdReader::OpenBytes(fixture(), source)); const auto* before = source.Directory();
        auto bytes = fixture(compression, false, preview); bytes.chop(1); QString error;
        QVERIFY(!CPsdReader::OpenBytes(bytes, source, &error)); QVERIFY(!error.isEmpty()); QCOMPARE(source.Directory(), before);
        Layer first, hidden; hidden.flags = 2; hidden.channels.append({7, QByteArray(4, 0)});
        hidden.encoded.insert(7, u16(1) + u16(2) + u16(2) + QByteArray::fromHex("fd00fd00"));
        QVERIFY(!CPsdReader::OpenBytes(document({first, hidden}), source, &error)); QCOMPARE(source.Directory(), before);
    }
    void workingAndCumulativeBudgetsHaveIndependentBoundaries()
    {
        const auto bytes = fixture(); CPsdReadDocument source; QString error; CPsdStreamLimits limits;
        limits.maximumLayerWorkingBytes = 65536 + 2 * 2 * 5; limits.document.maximumDecodedBytes = 2 * 2 * 8;
        QVERIFY2(CPsdReader::OpenBytes(bytes, source, &error, nullptr, limits), qPrintable(error));
        QImage image; QVERIFY(source.ReadLayer(0, image)); QCOMPARE(image.size(), QSize(2, 2)); const auto* before = source.Directory();
        --limits.maximumLayerWorkingBytes;
        QVERIFY(!CPsdReader::OpenBytes(bytes, source, &error, nullptr, limits)); QVERIFY(error.contains(QStringLiteral("工作"))); QCOMPARE(source.Directory(), before);
        ++limits.maximumLayerWorkingBytes; --limits.document.maximumDecodedBytes;
        QVERIFY(!CPsdReader::OpenBytes(bytes, source, &error, nullptr, limits)); QVERIFY(error.contains(QStringLiteral("总量"))); QCOMPARE(source.Directory(), before);
        CPsdReadLimits eagerLimits; eagerLimits.maximumDecodedBytes = 2 * 2 * 16;
        CPsdSnapshot snapshot; QVERIFY(CPsdReader::ReadBytes(bytes, snapshot, &error, nullptr, eagerLimits));
        --eagerLimits.maximumDecodedBytes; QVERIFY(!CPsdReader::ReadBytes(bytes, snapshot, &error, nullptr, eagerLimits));
        QCOMPARE(snapshot.bytes, bytes);
    }
    void directoryPolicyAdmitsMoreValidationWithoutRgba()
    {
        const CPsdReadLimits eagerDefault; const auto directoryDefault = CPsdReadLimits::Directory();
        QVERIFY(directoryDefault.maximumDecodedBytes > eagerDefault.maximumDecodedBytes);
        QCOMPARE(directoryDefault.maximumLayerPixels, eagerDefault.maximumLayerPixels);
        const auto bytes = largeSolid(16, 16); CPsdDirectory directory; QString error;
        CPsdReadLimits small = eagerDefault; small.maximumDecodedBytes = 1024;
        QVERIFY(!CPsdReader::ReadDirectoryBytes(bytes, directory, &error, nullptr, small));
        small.maximumDecodedBytes = 4096;
        QVERIFY2(CPsdReader::ReadDirectoryBytes(bytes, directory, &error, nullptr, small), qPrintable(error));
        CPsdStreamLimits stream; stream.document = small; CPsdReadDocument source;
        QVERIFY(CPsdReader::OpenBytes(bytes, source, &error, nullptr, stream));
        QCOMPARE(source.Directory()->bytes, directory.bytes); QImage image; QVERIFY(source.ReadLayer(0, image));
        QCOMPARE(image.size(), QSize(16, 16)); QCOMPARE(image.pixel(15, 15), qRgba(91, 91, 91, 91));
    }
    void emptyLayerSectionsPreflightMergedBudget_data()
    {
        QTest::addColumn<int>("form"); QTest::newRow("no-layer-section") << 0;
        QTest::newRow("empty-layer-info") << 1; QTest::newRow("zero-layer-count") << 2;
    }
    void emptyLayerSectionsPreflightMergedBudget()
    {
        QFETCH(int, form);
        const auto section = form == 0 ? QByteArray() : form == 1 ? u32(0) + u32(0) : block(u16(0)) + u32(0);
        const auto bytes = QByteArray("8BPS") + u16(1) + QByteArray(6, 0) + u16(4) + u32(100) + u32(2) + u16(8) + u16(3) +
            u32(0) + u32(0) + block(section) + repeatedChannel(2, 100, 4, 73);
        CPsdSnapshot snapshot; QVERIFY(CPsdReader::ReadBytes(fixture(), snapshot)); const auto previous = snapshot;
        CPsdReadLimits limits; limits.maximumDecodedBytes = 800; CExportControl control; int checkpoints = 0;
        QVERIFY(control.SetHandler([&](const auto&, QString*) { ++checkpoints; return true; }));
        QString error; QVERIFY(!CPsdReader::ReadBytes(bytes, snapshot, &error, &control, limits));
        QVERIFY(error.contains(QStringLiteral("总量"))); QVERIFY(checkpoints < 100);
        QCOMPARE(snapshot.bytes, previous.bytes); QCOMPARE(snapshot.sha256, previous.sha256);
        QCOMPARE(snapshot.mergedImage, previous.mergedImage); QCOMPARE(snapshot.layers.size(), previous.layers.size());
        QCOMPARE(snapshot.layers[0].image, previous.layers[0].image);
        CPsdDirectory directory;
        QVERIFY2(CPsdReader::ReadDirectoryBytes(bytes, directory, &error, nullptr, limits), qPrintable(error));
        QVERIFY(directory.layers.isEmpty()); QCOMPARE(directory.canvasSize, QSize(2, 100));
        CPsdStreamLimits stream; stream.document = limits; CPsdReadDocument source;
        QVERIFY2(CPsdReader::OpenBytes(bytes, source, &error, nullptr, stream), qPrintable(error));
        QVERIFY(source.Directory()->layers.isEmpty()); QCOMPARE(source.Directory()->bytes, bytes);
        const auto* before = source.Directory(); const auto truncated = bytes.left(bytes.size() - 1);
        QVERIFY(!CPsdReader::ReadDirectoryBytes(truncated, directory, &error, nullptr, limits));
        QCOMPARE(directory.bytes, bytes);
        QVERIFY(!CPsdReader::OpenBytes(truncated, source, &error, nullptr, stream)); QCOMPARE(source.Directory(), before);
    }
    void invalidWorkingLimitsRejectBeforeFileRead_data()
    {
        QTest::addColumn<qint64>("working"); QTest::newRow("negative") << qint64(-1);
        QTest::newRow("zero") << qint64(0); QTest::newRow("over-cap") << qint64(2LL * 1024 * 1024 * 1024 + 1);
    }
    void invalidWorkingLimitsRejectBeforeFileRead()
    {
        QFETCH(qint64, working); CPsdReadDocument source; QVERIFY(CPsdReader::OpenBytes(fixture(), source));
        const auto* before = source.Directory(); CPsdStreamLimits limits; limits.maximumLayerWorkingBytes = working;
        QString error; QVERIFY(!CPsdReader::OpenBytes(fixture(), source, &error, nullptr, limits));
        QVERIFY(error.contains(QStringLiteral("工作预算"))); QCOMPARE(source.Directory(), before);
        QTemporaryDir directory; QVERIFY(directory.isValid()); const auto path = directory.filePath("valid.psd");
        QVERIFY(writeFile(path, fixture())); CExportControl control; bool reading = false;
        QVERIFY(control.SetHandler([&](const auto& progress, QString*) { reading |= progress.detail == QStringLiteral("读取 PSD"); return true; }));
        QVERIFY(!CPsdReader::OpenFile(path, source, &error, &control, limits));
        QVERIFY(!reading); QVERIFY(error.contains(QStringLiteral("工作预算"))); QCOMPARE(source.Directory(), before);
    }
    void filePathAndContentHaveIndependentLifetimes()
    {
        QTemporaryDir directory; QVERIFY(directory.isValid()); QString path = directory.filePath("source.psd");
        const auto original = path; const auto bytes = fixture(3); QVERIFY(writeFile(path, bytes));
        CExportControl control; bool called = false;
        QVERIFY(control.SetHandler([&](const auto&, QString*) { if (!called) { called = true; path = directory.filePath("missing.psd"); } return true; }));
        CPsdReadDocument source; QString error; QVERIFY2(CPsdReader::OpenFile(path, source, &error, &control), qPrintable(error));
        QVERIFY(called); QCOMPARE(source.Directory()->canonicalPath, QFileInfo(original).canonicalFilePath());
        QCOMPARE(source.Directory()->bytes, bytes); QVERIFY(QFile::remove(original));
        QImage image; QVERIFY(source.ReadLayer(0, image)); QCOMPARE(image.pixel(0, 0), qRgba(1, 17, 255, 0));
    }
    void fileChangesDuringReadKeepExistingDocument()
    {
        QTemporaryDir directory; QVERIFY(directory.isValid()); const auto path = directory.filePath("changing.psd");
        QVERIFY(writeFile(path, largeSolid(64, 1300, 0))); CPsdReadDocument source;
        QVERIFY(CPsdReader::OpenBytes(fixture(), source)); const auto* before = source.Directory();
        CExportControl control; bool changed = false, appended = false;
        QVERIFY(control.SetHandler([&](const auto& progress, QString*) {
            if (!changed && progress.detail == QStringLiteral("读取 PSD") && progress.current >= 256 * 1024) {
                changed = true; QFile file(path); appended = file.open(QIODevice::WriteOnly | QIODevice::Append) && file.write("x", 1) == 1;
            }
            return true;
        }));
        QString error; QVERIFY(!CPsdReader::OpenFile(path, source, &error, &control));
        QVERIFY(changed); QVERIFY(appended); QVERIFY(error.contains(QStringLiteral("变化"))); QCOMPARE(source.Directory(), before);
    }
};

QTEST_GUILESS_MAIN(PsdStreamReaderTests)
#include "psd_stream_reader_tests.moc"
