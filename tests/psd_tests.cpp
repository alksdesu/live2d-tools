#include "psd/cpsdlist.h"
#include "psd/psdSamples/Exporter/PsdExportDocument.h"
#include "psd/psdSamples/Util/PsdSyncFileWriter.h"
#include "psd_binary_reader.h"
#include "std/cexportcontrol.h"
#include <QtTest>
#include <QTemporaryDir>
#include <QScopeGuard>
#include <cstring>
#include <limits>
#include <QSet>
#include <QCryptographicHash>
#include <QtEndian>
#include <type_traits>
#include <vector>

namespace {
class TrackingAllocator : public psd::Allocator
{
public:
    QSet<void*> active;
    int untilFailure = -1;
private:
    void* DoAllocate(size_t size, size_t alignment) override
    {
        if (untilFailure == 0) return nullptr;
        if (untilFailure > 0) --untilFailure;
        void* result = backing.Allocate(size, alignment);
        if (result) active.insert(result);
        return result;
    }
    void DoFree(void* pointer) override
    {
        active.remove(pointer);
        backing.Free(pointer);
    }
    psd::MallocAllocator backing;
};

class FaultFile : public psd::File
{
public:
    FaultFile(psd::Allocator* allocator, int failAt, bool nullOperation = false)
        : File(allocator), m_failAt(failAt), m_nullOperation(nullOperation) {}
    int writes = 0;
    int waits = 0;
    int outstanding = 0;
    QVector<quint64> positions;
    QByteArray bytes;
private:
    bool DoOpenRead(const wchar_t*) override { return false; }
    bool DoOpenWrite(const wchar_t*) override { return true; }
    bool DoClose() override { return true; }
    ReadOperation DoRead(void*, uint32_t, uint64_t) override { return nullptr; }
    bool DoWaitForRead(ReadOperation&) override { return false; }
    WriteOperation DoWrite(const void* data, uint32_t count, uint64_t position) override
    {
        positions.append(position);
        const bool success = ++writes != m_failAt;
        if (!success && m_nullOperation) return nullptr;
        if (success) {
            if (position + count > static_cast<quint64>(bytes.size())) bytes.resize(position + count);
            memcpy(bytes.data() + position, data, count);
        }
        ++outstanding;
        return new bool(success);
    }
    bool DoWaitForWrite(WriteOperation& operation) override
    {
        ++waits;
        const bool success = *static_cast<bool*>(operation);
        delete static_cast<bool*>(operation);
        operation = nullptr;
        --outstanding;
        return success;
    }
    uint64_t DoGetSize() const override { return bytes.size(); }
    int m_failAt;
    bool m_nullOperation;
};

QByteArray ReadFile(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return file.readAll();
}

bool SeedDestination(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write("original") == 8;
}

QImage RawImage(QImage::Format format, QRgb pixel)
{
    QImage image(1, 1, format);
    if (format == QImage::Format_RGBA8888 || format == QImage::Format_RGBA8888_Premultiplied) {
        uchar* data = image.bits();
        data[0] = qRed(pixel);
        data[1] = qGreen(pixel);
        data[2] = qBlue(pixel);
        data[3] = qAlpha(pixel);
    } else *reinterpret_cast<QRgb*>(image.bits()) = pixel;
    return image;
}
}

class PsdTests : public QObject
{
    Q_OBJECT
private slots:
    void cancelableChannelsKeepOldDataOnAbort_data()
    {
        QTest::addColumn<int>("depth");
        QTest::addColumn<bool>("rle");
        for (int depth : {8, 16, 32}) for (bool rle : {false, true})
            QTest::newRow(qPrintable(QStringLiteral("%1-%2").arg(depth).arg(rle ? "rle" : "raw"))) << depth << rle;
    }

    void cancelableChannelsKeepOldDataOnAbort()
    {
        QFETCH(int, depth);
        QFETCH(bool, rle);
        TrackingAllocator allocator;
        auto* document = psd::CreateExportDocument(&allocator, 4, 6, depth, psd::exportColorMode::RGB);
        const auto cleanup = qScopeGuard([&] { psd::DestroyExportDocument(document, &allocator); });
        const auto layerIndex = psd::AddLayer(document, &allocator, "candidate");
        const auto compression = rle ? psd::compressionType::RLE : psd::compressionType::RAW;
        const auto exercise = [&](auto sample) {
            using T = decltype(sample);
            std::vector<T> original(24, sample), changed(24);
            for (int i = 0; i < 24; ++i) changed[i] = static_cast<T>(i + 1);
            QVERIFY(psd::UpdateLayer(document, &allocator, layerIndex, psd::exportChannel::RED,
                                    0, 0, 4, 6, original.data(), psd::compressionType::RAW, {}));
            auto& layer = document->layers[layerIndex];
            void* old = layer.channelData[0];
            const uint32_t oldSize = layer.channelSize[0];
            const QByteArray oldBytes(static_cast<const char*>(old), oldSize);
            const auto allocated = allocator.active;
            for (int stop : {0, 3, 6}) {
                const psd::ExportCheckpoint checkpoint = [stop](uint32_t current, uint32_t) { return current != uint32_t(stop); };
                QVERIFY(!psd::UpdateLayer(document, &allocator, layerIndex, psd::exportChannel::RED,
                                         5, 7, 9, 13, changed.data(), compression, checkpoint));
                QCOMPARE(layer.channelData[0], old);
                QCOMPARE(layer.channelSize[0], oldSize);
                QCOMPARE(layer.channelCompression[0], uint16_t(psd::compressionType::RAW));
                QCOMPARE(layer.left, 0);
                QCOMPARE(layer.top, 0);
                QCOMPARE(QByteArray(static_cast<const char*>(old), oldSize), oldBytes);
                QVERIFY(allocator.active == allocated);
            }
            QVERIFY(!psd::UpdateLayer(document, &allocator, layerIndex, psd::exportChannel::RED,
                5, 7, 9, 13, changed.data(), compression, [](uint32_t row, uint32_t) {
                    if (row == 3) throw std::runtime_error("injected callback failure");
                    return true;
                }));
            QVERIFY(allocator.active == allocated);
            QCOMPARE(layer.channelData[0], old);
            for (int failAt = 0; failAt < (rle ? 3 : 1); ++failAt) {
                allocator.untilFailure = failAt;
                QVERIFY(!psd::UpdateLayer(document, &allocator, layerIndex, psd::exportChannel::RED,
                    5, 7, 9, 13, changed.data(), compression, {}));
                allocator.untilFailure = -1;
                QVERIFY(allocator.active == allocated);
                QCOMPARE(layer.channelData[0], old);
            }
            for (auto unsupported : {psd::compressionType::ZIP, psd::compressionType::ZIP_WITH_PREDICTION}) {
                QVERIFY(!psd::UpdateLayer(document, &allocator, layerIndex, psd::exportChannel::RED,
                    5, 7, 9, 13, changed.data(), unsupported, {}));
                psd::UpdateLayer(document, &allocator, layerIndex, psd::exportChannel::RED,
                    5, 7, 9, 13, changed.data(), unsupported);
                QCOMPARE(layer.channelData[0], old);
                QVERIFY(allocator.active == allocated);
            }
            int rowsChecked = 0;
            bool validTotal = true;
            QVERIFY(psd::UpdateLayer(document, &allocator, layerIndex, psd::exportChannel::RED,
                0, 0, 4, 6, changed.data(), compression, [&](uint32_t, uint32_t total) {
                    validTotal = validTotal && total == 6;
                    ++rowsChecked;
                    return true;
                }));
            QVERIFY(rowsChecked >= 7);
            QVERIFY(validTotal);
            QByteArray expected;
            for (T value : changed) {
                quint32 bits = 0;
                if constexpr (std::is_same_v<T, float>) memcpy(&bits, &value, sizeof(value));
                else bits = value;
                for (int byte = int(sizeof(T)) - 1; byte >= 0; --byte) expected.append(char(bits >> (byte * 8)));
            }
            const QByteArray bytes(static_cast<const char*>(layer.channelData[0]), layer.channelSize[0]);
            if (!rle) QCOMPARE(bytes, expected);
            else {
                int offset = 12;
                QByteArray decoded;
                for (int row = 0; row < 6; ++row) {
                    const quint16 size = qFromBigEndian<quint16>(reinterpret_cast<const uchar*>(bytes.constData() + row * 2));
                    decoded += PsdBinary::DecodePackBits(bytes.mid(offset, size), 4 * sizeof(T));
                    offset += size;
                }
                QCOMPARE(offset, bytes.size());
                QCOMPARE(decoded, expected);
            }
        };
        if (depth == 8) exercise(uint8_t(42));
        else if (depth == 16) exercise(uint16_t(0x1234));
        else exercise(float(0.5f));
    }

    void canceledExportNeverPublishesPartialDestination_data()
    {
        QTest::addColumn<QString>("point");
        QTest::addColumn<bool>("existing");
        for (const auto& point : {QStringLiteral("pixels"), QStringLiteral("rle"), QStringLiteral("writing"), QStringLiteral("commit"), QStringLiteral("exception")})
            for (bool existing : {false, true})
                QTest::newRow(qPrintable(point + (existing ? "-existing" : "-new"))) << point << existing;
    }

    void canceledExportNeverPublishesPartialDestination()
    {
        QFETCH(QString, point);
        QFETCH(bool, existing);
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString path = temp.filePath(QStringLiteral("output.psd"));
        if (existing) QVERIFY(SeedDestination(path));
        const int side = point == QStringLiteral("writing") ? 1024 : 32;
        QImage image(side, side, QImage::Format_ARGB32);
        image.fill(qRgba(43, 77, 101, 255));
        CExportControl control;
        bool reached = false;
        QVERIFY(control.SetHandler([&](const CExportControl::Progress& progress, QString*) {
            const bool cancel = (point == QStringLiteral("pixels") && progress.phase == CExportControl::Phase::Encoding && progress.current == side / 2) ||
                (point == QStringLiteral("rle") && progress.phase == CExportControl::Phase::Encoding && progress.current == side + side / 2) ||
                (point == QStringLiteral("writing") && progress.phase == CExportControl::Phase::Writing && progress.current >= 1024 * 1024) ||
                (point == QStringLiteral("commit") && progress.phase == CExportControl::Phase::Committing) ||
                (point == QStringLiteral("exception") && progress.phase == CExportControl::Phase::Writing && progress.current > 0);
            if (cancel) {
                reached = true;
                if (point == QStringLiteral("exception")) throw std::runtime_error("injected write checkpoint failure");
                control.Cancel();
            }
            return true;
        }));
        CPsdList writer;
        writer.SetControl(&control);
        QCOMPARE(writer.Control(), &control);
        QVERIFY(writer.SetPsd(path, side, side));
        const bool layer = writer.AddLayer(QStringLiteral("cancelable"), image, QRect(0, 0, side, side));
        if (point == QStringLiteral("pixels") || point == QStringLiteral("rle")) QVERIFY(!layer);
        else QVERIFY(layer);
        QVERIFY(!writer.CreatePsd());
        QVERIFY(reached);
        QVERIFY(!control.IsCommitted());
        QCOMPARE(control.WasCanceled(), point != QStringLiteral("exception"));
        QVERIFY(!control.Error().isEmpty());
        if (existing) QCOMPARE(ReadFile(path), QByteArray("original"));
        else QVERIFY(!QFileInfo::exists(path));
        QCOMPARE(QDir(temp.path()).entryList(QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot).size(), existing ? 1 : 0);
    }

    void successfulControlKeepsOutputBytesAndLateCancelCannotRevokeCommit()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QImage image(3, 2, QImage::Format_ARGB32);
        image.fill(qRgba(100, 50, 20, 127));
        const QString plain = temp.filePath(QStringLiteral("plain.psd"));
        const QString controlled = temp.filePath(QStringLiteral("controlled.psd"));
        CPsdList baseline;
        QVERIFY(baseline.SetPsd(plain, 3, 2));
        QVERIFY(baseline.AddGroupStart());
        QVERIFY(baseline.AddLayer(QStringLiteral("内容"), image, QRect(0, 0, 3, 2)));
        QVERIFY(baseline.AddGroupEnd("group"));
        QVERIFY(baseline.CreatePsd());
        CExportControl control;
        int checkpoints = 0;
        QVERIFY(control.SetHandler([&](const CExportControl::Progress&, QString*) { ++checkpoints; return true; }));
        CPsdList writer;
        writer.SetControl(&control);
        QVERIFY(writer.SetPsd(controlled, 3, 2));
        QVERIFY(writer.AddGroupStart());
        QVERIFY(writer.AddLayer(QStringLiteral("内容"), image, QRect(0, 0, 3, 2)));
        QVERIFY(writer.AddGroupEnd("group"));
        QVERIFY(writer.CreatePsd());
        QVERIFY(checkpoints > 20);
        QCOMPARE(ReadFile(controlled), ReadFile(plain));
        QCOMPARE(QCryptographicHash::hash(ReadFile(controlled), QCryptographicHash::Sha256).toHex(),
                 QByteArray("7da358fac4b14ecd2cede9e3d260e4196e6a1a9bd2e8ebefb5a093d7e7f0ef00"));
        QVERIFY(control.IsCommitted());
        control.Cancel();
        QVERIFY(control.Checkpoint());
        QVERIFY(control.IsCommitted());
        QVERIFY(!control.WasCanceled());
        QCOMPARE(ReadFile(controlled), ReadFile(plain));
    }

    void independentReaderRejectsMalformedData()
    {
        const QByteArray fixture = QByteArray::fromHex(
            "3842505300010000000000000003000000010000000100080003000000000000"
            "0000000000660000005e00010000000000000000000000010000000100010000"
            "000000033842494d6e6f726dff00000000000030000000000000000001410000"
            "3842494d6c73637400000004000000003842494d6c756e690000000800000001"
            "0041000000006400000000000000643218");
        PsdBinary::Document parsed;
        QString error;
        QVERIFY2(PsdBinary::Parse(fixture, parsed, &error), qPrintable(error));
        QCOMPARE(parsed.canvas, QSize(1, 1));
        QCOMPARE(parsed.layers.size(), 1);
        QCOMPARE(parsed.layers[0].name, QString("A"));
        QCOMPARE(parsed.layers[0].Plane(0), QByteArray::fromHex("64"));
        for (qsizetype length = 0; length < fixture.size(); ++length) {
            QVERIFY2(!PsdBinary::Parse(fixture.left(length), parsed, &error), qPrintable(QString::number(length)));
            QVERIFY(!error.isEmpty());
            QCOMPARE(parsed.layers[0].name, QString("A"));
        }
        QByteArray oversized = fixture;
        oversized.replace(34, 4, QByteArray(4, '\xff'));
        QVERIFY(!PsdBinary::Parse(oversized, parsed, &error));
        QByteArray badUnicode = fixture;
        const qsizetype unicodeCount = badUnicode.indexOf("luni") + 8;
        badUnicode.replace(unicodeCount, 4, QByteArray(4, '\xff'));
        QVERIFY(!PsdBinary::Parse(badUnicode, parsed, &error));
        QByteArray emptyExtreme = fixture;
        emptyExtreme.replace(48, 4, QByteArray::fromHex("80000000"));
        emptyExtreme.replace(56, 4, QByteArray::fromHex("80000000"));
        QVERIFY(!PsdBinary::Parse(emptyExtreme, parsed, &error));
        QVERIFY(!PsdBinary::Parse(fixture + '\0', parsed, &error));
    }

    void independentPackBitsDecoder()
    {
        QCOMPARE(PsdBinary::DecodePackBits(QByteArray::fromHex("0241424380fe44"), 6), QByteArray("ABCDDD"));
        QCOMPARE(PsdBinary::DecodePackBits(QByteArray::fromHex("8158"), 128), QByteArray(128, 'X'));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, PsdBinary::DecodePackBits(QByteArray::fromHex("02aabb"), 3));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, PsdBinary::DecodePackBits(QByteArray::fromHex("fe"), 3));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, PsdBinary::DecodePackBits(QByteArray::fromHex("fe00"), 2));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, PsdBinary::DecodePackBits(QByteArray::fromHex("0001"), 2));
    }

    void alphaFormatsExportStraightPixels_data()
    {
        QTest::addColumn<int>("format");
        QTest::addColumn<bool>("premultiplied");
        QTest::addColumn<quint32>("raw");
        QTest::addColumn<QByteArray>("expected");
        for (QImage::Format format : {QImage::Format_ARGB32, QImage::Format_RGBA8888,
                                      QImage::Format_ARGB32_Premultiplied, QImage::Format_RGBA8888_Premultiplied}) {
            const bool tagged = format == QImage::Format_ARGB32_Premultiplied || format == QImage::Format_RGBA8888_Premultiplied;
            for (bool flag : {false, true}) {
                const QByteArray suffix = QByteArray::number(format) + (flag ? "-flagged" : "-unflagged");
                QTest::newRow(("translucent-" + suffix).constData()) << int(format) << flag << quint32(qRgba(50, 25, 12, 128))
                    << QByteArray::fromHex(tagged || flag ? "64321880" : "32190c80");
                QTest::newRow(("transparent-" + suffix).constData()) << int(format) << flag << quint32(qRgba(50, 25, 12, 0))
                    << QByteArray::fromHex("00000000");
                QTest::newRow(("opaque-" + suffix).constData()) << int(format) << flag << quint32(qRgba(100, 50, 24, 255))
                    << QByteArray::fromHex("643218ff");
            }
        }
    }

    void alphaFormatsExportStraightPixels()
    {
        QFETCH(int, format);
        QFETCH(bool, premultiplied);
        QFETCH(quint32, raw);
        QFETCH(QByteArray, expected);
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString path = temp.filePath("alpha.psd");
        CPsdList writer;
        writer.SetPsd(path, 1, 1);
        writer.AddLayer("pixel", RawImage(static_cast<QImage::Format>(format), raw), QRect(0, 0, 1, 1),
                        255, 0x6e6f726d, false, premultiplied);
        QVERIFY(writer.CreatePsd());
        PsdBinary::Document parsed;
        QString error;
        QVERIFY2(PsdBinary::Parse(ReadFile(path), parsed, &error), qPrintable(error));
        QCOMPARE(parsed.layers.size(), 1);
        const auto& layer = parsed.layers[0];
        QCOMPARE(layer.Plane(0) + layer.Plane(1) + layer.Plane(2) + layer.Plane(-1), expected);
        for (const auto& channel : layer.channels) QCOMPARE(channel.compression, quint16(1));
    }

    void layerMetadataGroupsNamesAndProfile()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString path = temp.filePath("metadata.psd");
        const QString layerName = QString::fromUtf8("左眼・高光 ✨😀");
        const QByteArray groupName = QString::fromUtf8("表情分组 🎨").toUtf8();
        QImage image(3, 2, QImage::Format_ARGB32);
        const QRgb pixels[] = {qRgba(100, 50, 24, 128), qRgba(20, 40, 60, 255), qRgba(10, 20, 30, 0),
                              qRgba(70, 70, 70, 255), qRgba(70, 70, 70, 255), qRgba(70, 70, 70, 255)};
        for (int y = 0; y < 2; ++y)
            for (int x = 0; x < 3; ++x) image.setPixel(x, y, pixels[y * 3 + x]);
        CPsdList writer;
        writer.SetPsd(path, 4, 3, true);
        writer.AddGroupStart();
        writer.AddLayer(layerName, image, QRect(-2, -1, 3, 2), 127, 0x6d756c20, true);
        writer.AddGroupEnd(groupName.constData());
        QVERIFY(writer.CreatePsd());
        PsdBinary::Document parsed;
        QString error;
        QVERIFY2(PsdBinary::Parse(ReadFile(path), parsed, &error), qPrintable(error));
        QCOMPARE(parsed.canvas, QSize(4, 3));
        QCOMPARE(parsed.channels, quint16(3));
        QCOMPARE(parsed.layers.size(), 3);
        QCOMPARE(parsed.layers[0].sectionType, quint32(3));
        QCOMPARE(parsed.layers[2].sectionType, quint32(1));
        QCOMPARE(parsed.layers[2].name, QString::fromUtf8(groupName));
        const auto& layer = parsed.layers[1];
        QCOMPARE(layer.name, layerName);
        QCOMPARE(layer.sectionType, quint32(0));
        QCOMPARE(layer.bounds, QRect(-2, -1, 3, 2));
        QCOMPARE(layer.opacity, quint8(127));
        QCOMPARE(layer.blend, QByteArray("mul "));
        QCOMPARE(layer.clipping, quint8(1));
        QCOMPARE(layer.Plane(0), QByteArray::fromHex("641400464646"));
        QCOMPARE(layer.Plane(1), QByteArray::fromHex("322800464646"));
        QCOMPARE(layer.Plane(2), QByteArray::fromHex("183c00464646"));
        QCOMPARE(layer.Plane(-1), QByteArray::fromHex("80ff00ffffff"));
        const QByteArray profile = ReadFile(":/icc/sRGB.icc");
        QVERIFY(!profile.isEmpty());
        QCOMPARE(parsed.resources.value(1039), profile);
        for (int index : {0, 2})
            for (const auto& channel : parsed.layers[index].channels) {
                QCOMPARE(channel.length, quint32(2));
                QVERIFY(channel.pixels.isEmpty());
            }
    }

    void invalidGeometryPreservesDestination_data()
    {
        QTest::addColumn<QSize>("canvas");
        QTest::addColumn<QRect>("bounds");
        QTest::addColumn<bool>("nullImage");
        QTest::newRow("empty-canvas") << QSize(0, 2) << QRect(0, 0, 1, 1) << false;
        QTest::newRow("negative-canvas") << QSize(-1, 2) << QRect(0, 0, 1, 1) << false;
        QTest::newRow("dimension-limit") << QSize(30001, 1) << QRect(0, 0, 1, 1) << false;
        QTest::newRow("pixel-limit") << QSize(10001, 10000) << QRect(0, 0, 1, 1) << false;
        QTest::newRow("empty-layer") << QSize(2, 2) << QRect(0, 0, 0, 1) << false;
        QTest::newRow("layer-dimension-limit") << QSize(2, 2) << QRect(0, 0, 30001, 1) << false;
        QTest::newRow("layer-pixel-limit") << QSize(2, 2) << QRect(0, 0, 10001, 10000) << false;
        const QPoint extreme(std::numeric_limits<int>::max(), 0);
        QTest::newRow("layer-coordinate-overflow") << QSize(2, 2) << QRect(extreme, extreme) << false;
        QTest::newRow("null-image") << QSize(2, 2) << QRect(0, 0, 1, 1) << true;
    }

    void invalidGeometryPreservesDestination()
    {
        QFETCH(QSize, canvas);
        QFETCH(QRect, bounds);
        QFETCH(bool, nullImage);
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString path = temp.filePath("existing.psd");
        QVERIFY(SeedDestination(path));
        CPsdList writer;
        writer.SetPsd(path, canvas.width(), canvas.height());
        const QImage image = nullImage ? QImage() : RawImage(QImage::Format_ARGB32, qRgb(1, 2, 3));
        writer.AddLayer("invalid", image, bounds);
        QVERIFY(!writer.CreatePsd());
        QCOMPARE(ReadFile(path), QByteArray("original"));
    }

    void openFailureRetainsDocumentForRetry()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString path = temp.filePath("missing/retry.psd");
        CPsdList writer;
        QVERIFY(writer.SetPsd(path, 1, 1));
        writer.AddLayer("retained", RawImage(QImage::Format_ARGB32, qRgb(9, 8, 7)), QRect(0, 0, 1, 1));
        QVERIFY(!writer.CreatePsd());
        QVERIFY(QDir().mkpath(temp.filePath("missing")));
        QVERIFY(writer.CreatePsd());
        PsdBinary::Document parsed;
        QString error;
        QVERIFY2(PsdBinary::Parse(ReadFile(path), parsed, &error), qPrintable(error));
        QCOMPARE(parsed.layers.size(), 1);
        QCOMPARE(parsed.layers[0].name, QString("retained"));
        QCOMPARE(parsed.layers[0].Plane(0), QByteArray(1, char(9)));
        QVERIFY(!writer.CreatePsd());
    }

    void missingColorProfilePreservesDestination()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString path = temp.filePath("existing.psd");
        QVERIFY(SeedDestination(path));
        Q_CLEANUP_RESOURCE(icc);
        const auto restoreResource = qScopeGuard([] { Q_INIT_RESOURCE(icc); });
        QVERIFY(!QFile::exists(":/icc/sRGB.icc"));
        CPsdList writer;
        QVERIFY(!writer.SetPsd(path, 1, 1, true));
        QVERIFY(!writer.CreatePsd());
        QCOMPARE(ReadFile(path), QByteArray("original"));
    }

    void synchronousWriterStopsAtFirstFailure_data()
    {
        QTest::addColumn<bool>("nullOperation");
        QTest::newRow("failed-wait") << false;
        QTest::newRow("null-operation") << true;
    }

    void synchronousWriterStopsAtFirstFailure()
    {
        QFETCH(bool, nullOperation);
        psd::MallocAllocator allocator;
        FaultFile file(&allocator, 2, nullOperation);
        psd::SyncFileWriter writer(&file);
        writer.Write("abc", 3);
        QVERIFY(!writer.HasError());
        QCOMPARE(writer.GetPosition(), uint64_t(3));
        writer.Write("defg", 4);
        QVERIFY(writer.HasError());
        QCOMPARE(writer.GetPosition(), uint64_t(3));
        writer.Write("ignored", 7);
        QCOMPARE(file.writes, 2);
        QCOMPARE(file.waits, nullOperation ? 1 : 2);
        QCOMPARE(file.outstanding, 0);
        QCOMPARE(file.positions, QVector<quint64>({0, 3}));
        QCOMPARE(file.bytes, QByteArray("abc"));
        psd::SyncFileWriter missing(nullptr);
        QVERIFY(missing.HasError());
        missing.Write("x", 1);
        QCOMPARE(missing.GetPosition(), uint64_t(0));
        FaultFile valid(&allocator, 0);
        psd::SyncFileWriter invalidBuffer(&valid);
        invalidBuffer.Write(nullptr, 0);
        QVERIFY(!invalidBuffer.HasError());
        invalidBuffer.Write(nullptr, 1);
        QVERIFY(invalidBuffer.HasError());
        QCOMPARE(valid.writes, 0);
    }

    void documentReportsWriteFailureAtEveryBoundary()
    {
        psd::MallocAllocator allocator;
        auto* document = psd::CreateExportDocument(&allocator, 1, 1, 8, psd::exportColorMode::RGB);
        const auto cleanup = qScopeGuard([&] { psd::DestroyExportDocument(document, &allocator); });
        const unsigned int layer = psd::AddLayer(document, &allocator, "fault-boundaries");
        const uint8_t pixel = 42;
        psd::UpdateLayer(document, &allocator, layer, psd::exportChannel::RED, 0, 0, 1, 1,
                         &pixel, psd::compressionType::RLE);
        QByteArray profile("icc");
        psd::SetICCProfile(document, &allocator, profile.data(), profile.size());
        FaultFile success(&allocator, 0);
        QVERIFY(psd::WriteDocument(document, &allocator, &success));
        QVERIFY(success.writes > 10);
        QCOMPARE(success.outstanding, 0);
        for (bool nullOperation : {false, true}) {
            for (int failure = 1; failure <= success.writes; ++failure) {
                FaultFile file(&allocator, failure, nullOperation);
                QVERIFY(!psd::WriteDocument(document, &allocator, &file));
                QCOMPARE(file.writes, failure);
                QCOMPARE(file.outstanding, 0);
                QCOMPARE(file.waits, failure - int(nullOperation));
            }
        }
        QVERIFY(!psd::WriteDocument(nullptr, &allocator, &success));
        QVERIFY(!psd::WriteDocument(document, nullptr, &success));
        QVERIFY(!psd::WriteDocument(document, &allocator, nullptr));
    }

    void highDepthLayerSections_data()
    {
        QTest::addColumn<int>("depth");
        QTest::newRow("16-bit") << 16;
        QTest::newRow("32-bit") << 32;
    }

    void highDepthLayerSections()
    {
        QFETCH(int, depth);
        psd::MallocAllocator allocator;
        auto* document = psd::CreateExportDocument(&allocator, 1, 1, depth, psd::exportColorMode::RGB);
        const auto cleanup = qScopeGuard([&] { psd::DestroyExportDocument(document, &allocator); });
        const unsigned int index = psd::AddLayer(document, &allocator, "high-depth");
        QByteArray expectedLayer, expectedMerged;
        if (depth == 16) {
            const uint16_t red = 0x1234, green = 0x5678, blue = 0x9abc;
            psd::UpdateLayer(document, &allocator, index, psd::exportChannel::RED, 0, 0, 1, 1,
                             &red, psd::compressionType::RAW);
            psd::UpdateMergedImage(document, &allocator, &red, &green, &blue);
            expectedLayer = QByteArray::fromHex("1234");
            expectedMerged = QByteArray::fromHex("123456789abc");
        } else {
            const float layer = 0.25f, red = 1.0f, green = 0.5f, blue = -0.25f;
            psd::UpdateLayer(document, &allocator, index, psd::exportChannel::RED, 0, 0, 1, 1,
                             &layer, psd::compressionType::RAW);
            psd::UpdateMergedImage(document, &allocator, &red, &green, &blue);
            expectedLayer = QByteArray::fromHex("3e800000");
            expectedMerged = QByteArray::fromHex("3f8000003f000000be800000");
        }
        FaultFile file(&allocator, 0);
        QVERIFY(psd::WriteDocument(document, &allocator, &file));
        PsdBinary::Reader input(file.bytes);
        input.Signature("8BPS");
        QCOMPARE(input.U16(), quint16(1));
        QCOMPARE(input.Take(6), QByteArray(6, '\0'));
        QCOMPARE(input.U16(), quint16(3));
        QCOMPARE(input.U32(), quint32(1));
        QCOMPARE(input.U32(), quint32(1));
        QCOMPARE(input.U16(), quint16(depth));
        QCOMPARE(input.U16(), quint16(3));
        input.Section();
        QCOMPARE(input.Section().Remaining(), qsizetype(0));
        PsdBinary::Reader mask = input.Section();
        QCOMPARE(mask.U32(), quint32(0));
        QCOMPARE(mask.U32(), quint32(0));
        mask.Signature("8BIM");
        QCOMPARE(mask.Take(4), depth == 16 ? QByteArray("Lr16") : QByteArray("Lr32"));
        PsdBinary::Reader info = mask.Section();
        QCOMPARE(info.I16(), qint16(1));
        const auto layer = PsdBinary::ReadLayer(info);
        QCOMPARE(layer.name, QString("high-depth"));
        QCOMPARE(layer.bounds, QRect(0, 0, 1, 1));
        QCOMPARE(layer.channels.size(), 1);
        QCOMPARE(layer.channels[0].id, qint16(0));
        PsdBinary::Reader channel(info.Take(layer.channels[0].length));
        QCOMPARE(channel.U16(), quint16(0));
        QCOMPARE(channel.Take(depth / 8), expectedLayer);
        QCOMPARE(channel.Remaining(), qsizetype(0));
        QVERIFY(info.Remaining() <= 3);
        info.ZeroPadding();
        QCOMPARE(mask.Remaining(), qsizetype(0));
        QCOMPARE(input.U16(), quint16(0));
        QCOMPARE(input.Take(3 * depth / 8), expectedMerged);
        QCOMPARE(input.Remaining(), qsizetype(0));
    }

    void capacityFailurePreservesDestination_data()
    {
        QTest::addColumn<int>("type");
        QTest::newRow("layer") << 0;
        QTest::newRow("group-start") << 1;
        QTest::newRow("group-end") << 2;
    }

    void capacityFailurePreservesDestination()
    {
        QFETCH(int, type);
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString path = temp.filePath("existing.psd");
        QFile existing(path);
        QVERIFY(existing.open(QIODevice::WriteOnly));
        QCOMPARE(existing.write("original"), 8);
        existing.close();

        CPsdList writer;
        writer.SetPsd(path, 2, 2);
        for (unsigned int i = 0; i < psd::ExportDocument::MAX_LAYER_COUNT / 2; ++i) {
            writer.AddGroupStart();
            writer.AddGroupEnd("group");
        }
        QImage image(2, 2, QImage::Format_ARGB32);
        image.fill(Qt::red);
        if (type == 0) writer.AddLayer("overflow", image, QRect(0, 0, 2, 2));
        else if (type == 1) writer.AddGroupStart();
        else writer.AddGroupEnd("overflow");
        QVERIFY(!writer.CreatePsd());
        QVERIFY(existing.open(QIODevice::ReadOnly));
        QCOMPARE(existing.readAll(), QByteArray("original"));
        existing.close();

        writer.SetPsd(path, 2, 2);
        writer.AddLayer(QString::fromUtf8("图层"), image, QRect(0, 0, 2, 2));
        QVERIFY(writer.CreatePsd());
        QVERIFY(existing.open(QIODevice::ReadOnly));
        QCOMPARE(existing.read(6), QByteArray::fromHex("384250530001"));
    }

    void invalidResetCannotWritePreviousDocument()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        CPsdList writer;
        QVERIFY(writer.SetPsd(temp.filePath("first.psd"), 2, 2));
        const QString invalidPath = temp.filePath("invalid.psd");
        QVERIFY(!writer.SetPsd(invalidPath, 0, 2));
        QVERIFY(!writer.CreatePsd());
        QVERIFY(!QFile::exists(invalidPath));
    }

    void creationReportsPixelBudgetFailureBeforeExport()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString path = temp.filePath("existing.psd");
        QVERIFY(SeedDestination(path));
        CPsdList writer;
        for (const QSize size : {QSize(16384, 16384), QSize(10001, 10000), QSize(30001, 1)}) {
            QVERIFY(!writer.SetPsd(path, size.width(), size.height()));
            QCOMPARE(ReadFile(path), QByteArray("original"));
            QVERIFY(!writer.CreatePsd());
            QCOMPARE(ReadFile(path), QByteArray("original"));
        }
        QVERIFY(writer.SetPsd(path, 1, 1));
        writer.AddLayer("pixel", RawImage(QImage::Format_ARGB32, qRgb(19, 23, 31)), QRect(0, 0, 1, 1));
        QVERIFY(writer.CreatePsd());
        PsdBinary::Document parsed;
        QString error;
        QVERIFY2(PsdBinary::Parse(ReadFile(path), parsed, &error), qPrintable(error));
        QCOMPARE(parsed.canvas, QSize(1, 1));
        QCOMPARE(parsed.layers.size(), 1);
    }
};

QTEST_GUILESS_MAIN(PsdTests)
#include "psd_tests.moc"
