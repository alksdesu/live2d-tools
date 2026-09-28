#include "psd/cpsdreader.h"
#include "std/cexportcontrol.h"
#include <QtTest>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtEndian>
#include <cstring>
#include <limits>

namespace {
QByteArray be16(quint16 value) { char bytes[2]; qToBigEndian(value, bytes); return QByteArray(bytes, 2); }
QByteArray be32(quint32 value) { char bytes[4]; qToBigEndian(value, bytes); return QByteArray(bytes, 4); }
QByteArray block(const QByteArray& bytes) { return be32(bytes.size()) + bytes; }
QByteArray tag(const QByteArray& key, const QByteArray& data) { return "8BIM" + key + block(data); }
QByteArray unicode(const QString& name) {
    QByteArray result = be32(name.size()); for (const auto character : name) result += be16(character.unicode()); return result;
}
QByteArray number(double value) {
    quint64 bits; std::memcpy(&bits, &value, sizeof(bits)); char bytes[8]; qToBigEndian(bits, bytes); return QByteArray(bytes, 8);
}
QByteArray timestamp(double value = 1234567890.25, bool terminatedName = true) {
    QByteArray result = be32(16) + unicode(terminatedName ? QString(QChar(0)) : QString()) + block("metadata") + be32(1) +
        block("layerTime") + "doub" + number(value);
    result += QByteArray((4 - result.size() % 4) % 4, 0); return result;
}
QByteArray metadataItem(const QByteArray& key, const QByteArray& data, const QByteArray& signature = "8BIM", quint8 copy = 0) {
    return signature + key + char(copy) + QByteArray(3, 0) + block(data);
}
QByteArray metadata(const QByteArray& descriptor = timestamp()) { return be32(1) + metadataItem("cust", descriptor); }
QByteArray neutralTags() {
    return tag("clbl", QByteArray::fromHex("01000000")) + tag("infx", be32(0)) + tag("knko", be32(0)) +
        tag("lspf", be32(0)) + tag("lclr", QByteArray(8, 0)) + tag("fxrp", QByteArray(16, 0)) + tag("shmd", metadata());
}
QByteArray packed(const QByteArray& bytes, int width, int compression) {
    QByteArray result = be16(compression);
    if (!compression) return result + bytes;
    if (compression == 1) {
        const int rows = bytes.size() / width;
        for (int y = 0; y < rows; ++y) result += be16(width + 1);
        for (int y = 0; y < rows; ++y) result += QByteArray(1, char(width - 1)) + bytes.mid(y * width, width);
        return result;
    }
    QByteArray encoded = bytes;
    if (compression == 3) for (int at = 0; at < bytes.size(); ++at)
        if (at % width) encoded[at] = char(quint8(bytes[at]) - quint8(bytes[at - 1]));
    return result + qCompress(encoded).mid(4);
}
struct Layer {
    int kind = 0, compression = 0; QString name = QString::fromUtf8("左目😀");
    QByteArray extras, mask, ranges, blendMode = "norm"; QRect bounds{-1, -2, 2, 2}; quint8 flags = 0, opacity = 255, clipping = 0;
    QVector<QPair<int, QByteArray>> channels{{0, QByteArray::fromHex("10f030e0")}, {1, QByteArray::fromHex("20404080")},
        {2, QByteArray::fromHex("306020a0")}, {-1, QByteArray::fromHex("0080ff40")}};
};
QByteArray fixture(const QVector<Layer>& layers = {Layer()}, int compositeCompression = 0) {
    QByteArray records, images;
    for (const auto& layer : layers) {
        records += be32(layer.bounds.y()) + be32(layer.bounds.x()) + be32(layer.bounds.y() + layer.bounds.height()) + be32(layer.bounds.x() + layer.bounds.width());
        records += be16(layer.channels.size());
        for (const auto& channel : layer.channels) {
            const auto data = packed(channel.second, layer.bounds.width(), layer.compression);
            records += be16(quint16(channel.first)) + be32(data.size()); images += data;
        }
        records += "8BIM" + layer.blendMode; records += char(layer.opacity); records += char(layer.clipping); records += char(layer.flags); records += char(0);
        QByteArray extra = block(layer.mask) + block(layer.ranges) + QByteArray::fromHex("01610000");
        extra += tag("luni", unicode(layer.name)) + tag("lyid", be32(42));
        if (layer.kind) extra += tag("lsct", be32(layer.kind));
        extra += layer.extras; records += block(extra);
    }
    QByteArray info = be16(quint16(-layers.size())) + records + images; if (info.size() % 2) info += char(0);
    QByteArray layerMask = block(info) + be32(0);
    const QByteArray merged = QByteArray::fromHex("ffff7f10ff337f20ff33ff3000cc80ff");
    return QByteArray("8BPS") + be16(1) + QByteArray(6, 0) + be16(4) + be32(2) + be32(2) + be16(8) + be16(3) +
        be32(0) + be32(0) + block(layerMask) + packed(merged, 2, compositeCompression);
}
bool hasDiagnostic(const CPsdLayer& layer, const QString& code) {
    return std::any_of(layer.diagnostics.cbegin(), layer.diagnostics.cend(), [&](const auto& d) { return d.code == code; });
}
}

class PsdReaderTests : public QObject
{
    Q_OBJECT
private slots:
    void compressionAndStraightAlpha_data() {
        QTest::addColumn<int>("layerCompression"); QTest::addColumn<int>("compositeCompression");
        for (int a = 0; a < 4; ++a) for (int b = 0; b < 4; ++b) QTest::newRow(qPrintable(QString("%1/%2").arg(a).arg(b))) << a << b;
    }
    void compressionAndStraightAlpha() {
        QFETCH(int, layerCompression); QFETCH(int, compositeCompression); Layer layer; layer.compression = layerCompression;
        const auto bytes = fixture({layer}, compositeCompression); CPsdSnapshot result; QString error;
        QVERIFY2(CPsdReader::ReadBytes(bytes, result, &error), qPrintable(error)); QCOMPARE(result.layers.size(), 1);
        const auto& value = result.layers[0]; QCOMPARE(value.name, layer.name); QCOMPARE(value.bounds, layer.bounds);
        QVERIFY(value.layerId); QCOMPARE(*value.layerId, quint32(42)); QVERIFY(value.pixelEquivalent);
        QCOMPARE(value.image.format(), QImage::Format_ARGB32); QCOMPARE(value.image.size(), QSize(2, 2));
        QCOMPARE(value.image.pixel(0, 0), qRgba(16, 32, 48, 0)); QCOMPARE(value.image.pixel(1, 1), qRgba(224, 128, 160, 64));
        QCOMPARE(result.mergedImage.pixel(0, 0), qRgba(0, 0, 0, 0));
        QCOMPARE(result.mergedImage.pixel(1, 0), qRgba(255, 0, 0, 204));
        QCOMPARE(result.mergedImage.pixel(0, 1), qRgba(0, 0, 255, 128));
        QCOMPARE(result.mergedImage.pixel(1, 1), qRgba(16, 32, 48, 255));
        QCOMPARE(result.bytes, bytes);
        QCOMPARE(result.sha256, QCryptographicHash::hash(bytes, QCryptographicHash::Sha256)); QCOMPARE(result.displayOrder, QVector<int>{0});
    }
    void zeroLayersUseMergedTransparencyTag()
    {
        const QByteArray info = be16(0) + be16(0);
        const QByteArray masks = block(info) + be32(0) + tag("Mtrn", {});
        const QByteArray header = QByteArray("8BPS") + be16(1) + QByteArray(6, 0) + be16(4) + be32(1) + be32(2) +
            be16(8) + be16(3) + be32(0) + be32(0);
        const QByteArray merged = be16(0) + QByteArray::fromHex("ffffffffffff00ff");
        const auto bytes = header + block(masks) + merged;
        CPsdSnapshot pixels;
        CPsdDirectory directory;
        QString error;
        QVERIFY2(CPsdReader::ReadBytes(bytes, pixels, &error), qPrintable(error));
        QVERIFY2(CPsdReader::ReadDirectoryBytes(bytes, directory, &error), qPrintable(error));
        QVERIFY(pixels.layers.isEmpty());
        QVERIFY(pixels.hasMergedTransparency);
        QVERIFY(directory.hasMergedTransparency);
        QVERIFY(directory.hasMergedPreview);
        QCOMPARE(pixels.mergedImage.pixel(0, 0), qRgba(0, 0, 0, 0));
        QCOMPARE(pixels.mergedImage.pixel(1, 0), qRgba(255, 255, 255, 255));
        for (const auto& extra : {tag("Mtrn", QByteArray(4, 0)), tag("Mtrn", {}) + tag("Mtrn", {})}) {
            const auto malformed = header + block(block(info) + be32(0) + extra) + merged;
            auto unchanged = pixels;
            QVERIFY(!CPsdReader::ReadBytes(malformed, unchanged, &error));
            QCOMPARE(unchanged.bytes, pixels.bytes);
            QVERIFY(!CPsdReader::ReadDirectoryBytes(malformed, directory, &error));
            QCOMPARE(directory.bytes, pixels.bytes);
        }
        QByteArray missingAlpha = bytes;
        missingAlpha[13] = 3;
        missingAlpha.chop(2);
        QVERIFY(!CPsdReader::ReadBytes(missingAlpha, pixels, &error));
        QVERIFY(!CPsdReader::ReadDirectoryBytes(missingAlpha, directory, &error));
    }

    void adobeMergedRedHasStraightLayerAndWhiteMattePreview()
    {
        Layer layer;
        layer.bounds = QRect(0, 0, 2, 2);
        layer.channels = {{0, QByteArray(4, char(255))}, {1, QByteArray(4, 0)},
                          {2, QByteArray(4, 0)}, {-1, QByteArray(4, char(204))}};
        auto bytes = fixture({layer});
        bytes.chop(18);
        bytes += be16(0) + QByteArray(4, char(255)) + QByteArray(8, char(51)) + QByteArray(4, char(204));
        CPsdSnapshot result;
        QVERIFY(CPsdReader::ReadBytes(bytes, result));
        QCOMPARE(result.layers[0].image.pixel(0, 0), qRgba(255, 0, 0, 204));
        QCOMPARE(result.mergedImage.pixel(0, 0), qRgba(255, 0, 0, 204));
        QCOMPARE(result.mergedImage, result.layers[0].image);
    }

    void frozenInputOwnsBorrowedBytesBeforeCallbacks() {
        auto bytes = fixture(); const QByteArray original(bytes.constData(), bytes.size()); auto borrowed = QByteArray::fromRawData(bytes.constData(), bytes.size());
        CExportControl control; bool called = false;
        QVERIFY(control.SetHandler([&](const auto&, QString*) { called = true; bytes.fill('x'); return true; }));
        CPsdSnapshot result; QString error; QVERIFY2(CPsdReader::ReadBytes(borrowed, result, &error, &control), qPrintable(error));
        QVERIFY(called); QCOMPARE(result.bytes, original); QCOMPARE(result.layers[0].name, Layer().name);
    }
    void truncationsFailAtomically() {
        const auto bytes = fixture(); CPsdSnapshot baseline; QVERIFY(CPsdReader::ReadBytes(bytes, baseline));
        for (int length = 0; length < bytes.size(); ++length) {
            auto result = baseline; QString error;
            QVERIFY2(!CPsdReader::ReadBytes(bytes.left(length), result, &error), qPrintable(QString::number(length)));
            QVERIFY(!error.isEmpty()); QCOMPARE(result.bytes, baseline.bytes); QCOMPARE(result.mergedImage, baseline.mergedImage);
        }
    }
    void unsupportedHeadersAreExplicit() {
        for (const auto& field : QVector<QPair<int, quint16>>{{4, 2}, {22, 16}, {24, 4}}) {
            auto bytes = fixture(); const auto encoded = be16(field.second); bytes.replace(field.first, 2, encoded);
            CPsdSnapshot result; QString error; QVERIFY(!CPsdReader::ReadBytes(bytes, result, &error)); QVERIFY(error.contains(QStringLiteral("暂")));
        }
    }
    void duplicateStructuralTagsAreRejectedButUnknownDuplicatesArePreserved() {
        Layer layer; layer.extras = tag("lyid", be32(99)); CPsdSnapshot result; QString error;
        QVERIFY(!CPsdReader::ReadBytes(fixture({layer}), result, &error)); QVERIFY(error.contains(QStringLiteral("重复")));
        layer.extras = tag("zzzz", "one") + tag("zzzz", "two");
        QVERIFY2(CPsdReader::ReadBytes(fixture({layer}), result, &error), qPrintable(error));
        QCOMPARE(result.layers[0].additionalInfo.size(), 4); QVERIFY(!result.layers[0].pixelEquivalent);
        QCOMPARE(result.layers[0].additionalInfo[2].data, QByteArray("one")); QCOMPARE(result.layers[0].additionalInfo[3].data, QByteArray("two"));
    }
    void neutralMetadataEligibility_data() {
        QTest::addColumn<QByteArray>("extras"); QTest::addColumn<bool>("equivalent");
        const auto add = [](const QByteArray& name, const QByteArray& extras, bool equivalent) {
            QTest::newRow(name.constData()) << extras << equivalent;
        };
        const QVector<QPair<QByteArray, QByteArray>> defaults{{"clbl", QByteArray::fromHex("01000000")},
            {"infx", be32(0)}, {"knko", be32(0)}, {"lspf", be32(0)}, {"lclr", QByteArray(8, 0)},
            {"fxrp", QByteArray(16, 0)}, {"shmd", metadata()}, {"lyvr", be32(80)}};
        for (const auto& value : defaults) {
            const auto encoded = tag(value.first, value.second);
            add(value.first + "-default", encoded, true);
            add(value.first + "-duplicate", encoded + encoded, false);
            add(value.first + "-truncated", tag(value.first, value.second.left(3)), false);
            add(value.first + "-unknown-before", tag("zzzz", "preserved") + encoded, false);
            add(value.first + "-effect-after", encoded + tag("lfx2", "preserved"), false);
        }
        add("all-defaults", neutralTags(), true);
        for (const auto& key : {QByteArray("clbl"), QByteArray("infx"), QByteArray("knko")}) {
            for (int value = 0; value <= 2; ++value)
                add(key + "-value-" + QByteArray::number(value), tag(key, QByteArray(1, char(value)) + QByteArray(3, 0)), value == (key == "clbl" ? 1 : 0));
            const auto expected = key == "clbl" ? char(1) : char(0);
            add(key + "-padding", tag(key, QByteArray(1, expected) + QByteArray::fromHex("000001")), false);
            add(key + "-too-long", tag(key, QByteArray(1, expected) + QByteArray(4, 0)), false);
            add(key + "-conflicting-duplicate", tag(key, QByteArray(4, char(1))) + tag(key, QByteArray(1, expected) + QByteArray(3, 0)), false);
        }
        add("edit-locks", tag("lspf", be32(15)), true);
        add("unknown-locks", tag("lspf", be32(16)), false);
        add("color-label", tag("lclr", be16(7) + QByteArray(6, 0)), true);
        add("future-color-label", tag("lclr", be16(65535) + QByteArray(6, 0)), true);
        add("color-reserved-data", tag("lclr", QByteArray(7, 0) + char(1)), false);
        add("finite-reference", tag("fxrp", number(-12.5) + number(9000.25)), true);
        add("nan-reference", tag("fxrp", number(std::numeric_limits<double>::quiet_NaN()) + number(0)), false);
        add("infinite-reference", tag("fxrp", number(0) + number(std::numeric_limits<double>::infinity())), false);
        add("version-70", tag("lyvr", be32(70)), true);
        add("unknown-version", tag("lyvr", be32(900)), false);
        add("empty-metadata", tag("shmd", be32(0)), true);
        add("empty-descriptor-name", tag("shmd", metadata(timestamp(0, false))), true);
        add("negative-timestamp", tag("shmd", metadata(timestamp(-1.25))), true);
        add("nan-timestamp", tag("shmd", metadata(timestamp(std::numeric_limits<double>::quiet_NaN()))), false);
        add("infinite-timestamp", tag("shmd", metadata(timestamp(std::numeric_limits<double>::infinity()))), false);
        add("copy-on-duplication", tag("shmd", be32(1) + metadataItem("cust", timestamp(), "8BIM", 1)), true);
        add("invalid-copy-flag", tag("shmd", be32(1) + metadataItem("cust", timestamp(), "8BIM", 2)), false);
        add("unknown-signature", tag("shmd", be32(1) + metadataItem("cust", timestamp(), "8ELE")), false);
        add("unknown-subkey", tag("shmd", be32(1) + metadataItem("zzzz", timestamp())), false);
        add("duplicate-cust", tag("shmd", be32(2) + metadataItem("cust", timestamp()) + metadataItem("cust", timestamp())), false);
        add("mixed-known-and-unknown", tag("shmd", be32(2) + metadataItem("cust", timestamp()) + metadataItem("mlst", timestamp())), false);
        add("metadata-count-overflow", tag("shmd", be32(0xffffffff) + metadataItem("cust", timestamp())), false);
        add("metadata-trailing-data", tag("shmd", metadata() + QByteArray(4, 0)), false);
        for (const auto& change : QVector<QPair<QByteArray, QByteArray>>{{"metadata", "unknownx"}, {"layerTime", "otherTime"}, {"doub", "tdta"}}) {
            auto data = timestamp(); data.replace(change.first, change.second);
            add("timestamp-unknown-" + change.first, tag("shmd", metadata(data)), false);
        }
        auto data = metadata(); data[15] = char(1); add("metadata-nonzero-padding", tag("shmd", data), false);
        data = metadata(); data.replace(16, 4, be32(0xffffffff)); add("metadata-length-overflow", tag("shmd", data), false);
        data = timestamp(); data[3] = char(17); add("descriptor-version", tag("shmd", metadata(data)), false);
        data = timestamp(); data.replace(4, 4, be32(0xffffffff)); add("descriptor-name-overflow", tag("shmd", metadata(data)), false);
        data = timestamp(); data[9] = char('x'); add("descriptor-nonempty-name", tag("shmd", metadata(data)), false);
        data = timestamp(); data.replace(22, 4, be32(2)); add("descriptor-extra-item", tag("shmd", metadata(data)), false);
        data = timestamp(); data[data.size() - 1] = char(1); add("descriptor-nonzero-padding", tag("shmd", metadata(data)), false);
    }
    void neutralMetadataEligibility() {
        QFETCH(QByteArray, extras); QFETCH(bool, equivalent);
        Layer layer; layer.extras = extras; const auto bytes = fixture({layer});
        CPsdSnapshot baseline, eager; CPsdDirectory directory; CPsdReadDocument sealed; QString error;
        QVERIFY(CPsdReader::ReadBytes(fixture(), baseline));
        QVERIFY2(CPsdReader::ReadBytes(bytes, eager, &error), qPrintable(error));
        QVERIFY2(CPsdReader::ReadDirectoryBytes(bytes, directory, &error), qPrintable(error));
        QVERIFY2(CPsdReader::OpenBytes(bytes, sealed, &error), qPrintable(error));
        QCOMPARE(eager.layers[0].pixelEquivalent, equivalent);
        QCOMPARE(hasDiagnostic(eager.layers[0], QStringLiteral("unapplied-tag")), !equivalent);
        QCOMPARE(eager.layers[0].image, baseline.layers[0].image); QCOMPARE(eager.mergedImage, baseline.mergedImage);
        QCOMPARE(eager.bytes, bytes); QCOMPARE(directory.bytes, bytes); QCOMPARE(sealed.Directory()->bytes, bytes);
        for (const auto* actual : {static_cast<const CPsdDirectory*>(&directory), sealed.Directory()}) {
            QCOMPARE(actual->layers[0].pixelEquivalent, equivalent);
            QCOMPARE(actual->layers[0].diagnostics.size(), eager.layers[0].diagnostics.size());
            for (int index = 0; index < actual->layers[0].diagnostics.size(); ++index) {
                QCOMPARE(actual->layers[0].diagnostics[index].code, eager.layers[0].diagnostics[index].code);
                QCOMPARE(actual->layers[0].diagnostics[index].message, eager.layers[0].diagnostics[index].message);
            }
            QCOMPARE(actual->layers[0].additionalInfo.size(), eager.layers[0].additionalInfo.size());
            for (int index = 0; index < actual->layers[0].additionalInfo.size(); ++index) {
                QCOMPARE(actual->layers[0].additionalInfo[index].key, eager.layers[0].additionalInfo[index].key);
                QCOMPARE(actual->layers[0].additionalInfo[index].data, eager.layers[0].additionalInfo[index].data);
            }
        }
        QImage image; QVERIFY(sealed.ReadLayer(0, image)); QCOMPARE(image, baseline.layers[0].image);
        QByteArray retained;
        for (int index = 2; index < eager.layers[0].additionalInfo.size(); ++index)
            retained += tag(eager.layers[0].additionalInfo[index].key, eager.layers[0].additionalInfo[index].data);
        QCOMPARE(retained, extras);
    }
    void truncatedTimestampRemainsRecoverable() {
        const auto descriptor = timestamp();
        for (int size = 0; size < descriptor.size() - 1; ++size) {
            Layer layer; layer.extras = tag("shmd", metadata(descriptor.left(size))); const auto bytes = fixture({layer});
            CPsdSnapshot eager; CPsdDirectory directory; CPsdReadDocument sealed; QString error;
            QVERIFY2(CPsdReader::ReadBytes(bytes, eager, &error), qPrintable(error));
            QVERIFY2(CPsdReader::ReadDirectoryBytes(bytes, directory, &error), qPrintable(error));
            QVERIFY2(CPsdReader::OpenBytes(bytes, sealed, &error), qPrintable(error));
            QVERIFY(!eager.layers[0].pixelEquivalent); QVERIFY(!directory.layers[0].pixelEquivalent); QVERIFY(!sealed.Directory()->layers[0].pixelEquivalent);
            QCOMPARE(eager.bytes, bytes); QCOMPARE(directory.bytes, bytes); QCOMPARE(sealed.Directory()->bytes, bytes);
        }
    }
    void nestedMetadataRespectsBudgetAndCancellation() {
        const auto item = metadataItem("zzzz", {});
        Layer layer; layer.extras = tag("shmd", be32(65533) + item.repeated(65533));
        CPsdSnapshot eager; CPsdDirectory directory; CPsdReadDocument sealed; QString error;
        const auto accepted = fixture({layer});
        QVERIFY2(CPsdReader::ReadBytes(accepted, eager, &error), qPrintable(error));
        QVERIFY2(CPsdReader::ReadDirectoryBytes(accepted, directory, &error), qPrintable(error));
        QVERIFY2(CPsdReader::OpenBytes(accepted, sealed, &error), qPrintable(error));
        QVERIFY(!eager.layers[0].pixelEquivalent);
        layer.extras = tag("shmd", be32(65534) + item.repeated(65534)); const auto rejected = fixture({layer});
        QVERIFY(!CPsdReader::ReadBytes(rejected, eager, &error)); QVERIFY(error.contains(QStringLiteral("数量"))); QCOMPARE(eager.bytes, accepted);
        QVERIFY(!CPsdReader::ReadDirectoryBytes(rejected, directory, &error)); QCOMPARE(directory.bytes, accepted);
        QVERIFY(!CPsdReader::OpenBytes(rejected, sealed, &error)); QCOMPARE(sealed.Directory()->bytes, accepted);
        for (int path = 0; path < 3; ++path) {
            CExportControl control; int calls = 0;
            QVERIFY(control.SetHandler([&](const auto&, QString*) { if (++calls == 64) control.Cancel(); return true; }));
            const bool result = path == 0 ? CPsdReader::ReadBytes(accepted, eager, &error, &control) : path == 1
                ? CPsdReader::ReadDirectoryBytes(accepted, directory, &error, &control) : CPsdReader::OpenBytes(accepted, sealed, &error, &control);
            QVERIFY(!result); QVERIFY(control.WasCanceled()); QCOMPARE(calls, 64);
            QCOMPARE(eager.bytes, accepted); QCOMPARE(directory.bytes, accepted); QCOMPARE(sealed.Directory()->bytes, accepted);
        }
    }
    void neutralMetadataDoesNotOverrideCompositing() {
        for (int kind = 0; kind < 5; ++kind) {
            Layer layer; layer.extras = neutralTags();
            if (kind == 0) layer.blendMode = "mul ";
            if (kind == 1) layer.blendMode = "lddg";
            if (kind == 2) layer.clipping = 1;
            if (kind == 3) layer.extras = tag("lrFX", "effect") + layer.extras;
            if (kind == 4) layer.flags = 24;
            CPsdSnapshot eager; CPsdReadDocument sealed; QString error; const auto bytes = fixture({layer});
            QVERIFY2(CPsdReader::ReadBytes(bytes, eager, &error), qPrintable(error));
            QVERIFY2(CPsdReader::OpenBytes(bytes, sealed, &error), qPrintable(error));
            QVERIFY(!eager.layers[0].pixelEquivalent); QVERIFY(!sealed.Directory()->layers[0].pixelEquivalent);
            QImage image; QVERIFY(sealed.ReadLayer(0, image)); QCOMPARE(image, eager.layers[0].image);
        }
    }
    void malformedUnicodeAndRleAndZipAreRejected() {
        Layer layer; layer.name = QString(QChar(0xd800)); CPsdSnapshot result; QString error;
        QVERIFY(!CPsdReader::ReadBytes(fixture({layer}), result, &error));
        for (int compression : {1, 2, 3}) {
            layer = Layer(); layer.compression = compression; auto bytes = fixture({layer});
            const auto channel = packed(layer.channels[0].second, 2, compression); const auto at = bytes.indexOf(channel);
            QVERIFY(at >= 0); bytes[at + 2] = char(255);
            QVERIFY(!CPsdReader::ReadBytes(bytes, result, &error)); QVERIFY(!error.isEmpty());
        }
    }
    void groupsAndEmptyLayersRetainRecordIdentity() {
        Layer start, end, pixels; start.kind = 1; start.name = "Group"; start.bounds = {}; start.channels.clear();
        end = start; end.kind = 3; end.name = "Divider";
        CPsdSnapshot result; QString error;
        QVERIFY2(CPsdReader::ReadBytes(fixture({end, pixels, start}), result, &error), qPrintable(error));
        QCOMPARE(result.layers[1].parentIndex, 2); QCOMPARE(result.displayOrder, QVector<int>({2, 1}));
        QCOMPARE(result.layers[0].kind, CPsdLayer::Kind::GroupEnd); QVERIFY(result.layers[2].image.isNull());
        QVERIFY(!CPsdReader::ReadBytes(fixture({pixels, start}), result, &error)); QVERIFY(error.contains(QStringLiteral("结束")));
        QVERIFY(!CPsdReader::ReadBytes(fixture({end, pixels}), result, &error)); QVERIFY(error.contains(QStringLiteral("分组")));
        Layer empty; empty.bounds = {}; empty.channels.clear();
        QVERIFY(CPsdReader::ReadBytes(fixture({empty}), result, &error)); QVERIFY(result.layers[0].image.isNull());
    }
    void budgetsAndCancellationLeavePreviousSnapshotUntouched() {
        const auto bytes = fixture(); CPsdSnapshot result; QVERIFY(CPsdReader::ReadBytes(bytes, result));
        for (int setting = 0; setting < 4; ++setting) {
            CPsdReadLimits limits;
            if (setting == 0) { limits.maximumFileBytes = bytes.size() - 1; limits.maximumMetadataBytes = 1; }
            if (setting == 1) limits.maximumDecodedBytes = 47;
            if (setting == 2) limits.maximumLayerPixels = 3;
            if (setting == 3) limits.maximumMetadataBytes = 8;
            QString error; QVERIFY(!CPsdReader::ReadBytes(bytes, result, &error, nullptr, limits)); QVERIFY(!error.isEmpty()); QCOMPARE(result.bytes, bytes);
        }
        CExportControl control; QVERIFY(control.SetHandler([&](const auto&, QString*) { control.Cancel(); return true; }));
        QVERIFY(!CPsdReader::ReadBytes(bytes, result, nullptr, &control)); QVERIFY(control.WasCanceled()); QCOMPARE(result.bytes, bytes);
    }
    void fileReplacementDuringReadingIsRejected() {
        QTemporaryDir directory; QVERIFY(directory.isValid()); const auto path = directory.filePath("source.psd");
        QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); const auto bytes = fixture(); QCOMPARE(file.write(bytes), bytes.size()); file.close();
        CPsdSnapshot result; QString error; QVERIFY(CPsdReader::ReadFile(path, result, &error)); QCOMPARE(result.canonicalPath, QFileInfo(path).canonicalFilePath());
        CExportControl control; bool changed = false;
        QVERIFY(control.SetHandler([&](const auto& progress, QString*) {
            if (!changed && progress.detail == QStringLiteral("读取 PSD")) { changed = true; QFile replacement(path); if (replacement.open(QIODevice::WriteOnly)) replacement.write("bad"); }
            return true;
        }));
        QVERIFY(!CPsdReader::ReadFile(path, result, &error, &control)); QVERIFY(changed); QCOMPARE(result.bytes, bytes);
    }
    void zipBombsAndExtraDataNeverYieldPixels() {
        for (int compression : {0, 1, 2, 3}) {
            Layer layer; layer.compression = compression; layer.channels[0].second = QByteArray(100, 'x');
            CPsdSnapshot result; QString error;
            QVERIFY(!CPsdReader::ReadBytes(fixture({layer}), result, &error)); QVERIFY(!error.isEmpty()); QVERIFY(result.bytes.isEmpty());
        }
        Layer duplicate; duplicate.channels.append(duplicate.channels[0]); CPsdSnapshot result; QString error;
        QVERIFY(!CPsdReader::ReadBytes(fixture({duplicate}), result, &error)); QVERIFY(error.contains(QStringLiteral("通道")));
        for (int compression : {2, 3}) {
            Layer layer; layer.compression = compression; auto bytes = fixture({layer}, compression); bytes += char(0);
            QVERIFY(!CPsdReader::ReadBytes(bytes, result, &error)); QVERIFY(error.contains(QStringLiteral("尾随")));
        }
    }
    void masksEffectsAndCompositingHaveExplicitDiagnostics() {
        Layer layer; layer.opacity = 128; layer.clipping = 1; layer.flags = 2;
        layer.mask = be32(-2) + be32(-1) + be32(0) + be32(1) + QByteArray::fromHex("ff000000");
        layer.channels.append({-2, QByteArray::fromHex("ff00ff00")});
        layer.ranges = QByteArray::fromHex("0001ffff0000ffff"); layer.extras = tag("SoLd", "opaque-smart-object") + tag("lfx2", "opaque-effect");
        CPsdSnapshot result; QString error; QVERIFY2(CPsdReader::ReadBytes(fixture({layer}), result, &error), qPrintable(error));
        const auto& decoded = result.layers[0]; QVERIFY(!decoded.visible); QVERIFY(decoded.clipping); QCOMPARE(decoded.opacity, quint8(128));
        QVERIFY(!decoded.pixelEquivalent); QCOMPARE(decoded.image.pixel(1, 0), qRgba(240, 64, 96, 128));
        for (const auto& code : {"mask", "opacity", "clipping", "blend-ranges", "unapplied-tag", "extra-channel"}) QVERIFY(hasDiagnostic(decoded, QString::fromLatin1(code)));
        layer.mask = be32(0); QVERIFY(!CPsdReader::ReadBytes(fixture({layer}), result, &error));
    }
    void resourcesPreserveIccAndRespectMissingComposite() {
        auto bytes = fixture();
        const auto resource = [](int id, const QByteArray& data) { return QByteArray("8BIM") + be16(id) + QByteArray(2, 0) + block(data) + QByteArray(data.size() % 2, 0); };
        const QByteArray icc("frozen-profile");
        const auto resources = resource(1039, icc) + resource(1057, be32(1) + char(0));
        bytes.replace(30, 4, block(resources));
        CPsdSnapshot result; QString error; QVERIFY2(CPsdReader::ReadBytes(bytes, result, &error), qPrintable(error));
        QCOMPARE(result.iccProfile, icc); QVERIFY(result.mergedImage.isNull()); QVERIFY(!result.layers[0].image.isNull());
        auto duplicate = fixture(); duplicate.replace(30, 4, block(resource(1039, icc) + resource(1039, icc)));
        QVERIFY(!CPsdReader::ReadBytes(duplicate, result, &error)); QVERIFY(error.contains(QStringLiteral("重复")));
    }
    void cumulativeBudgetsAndGroupingAreChecked() {
        CPsdReadLimits limits; limits.maximumDecodedBytes = 95; CPsdSnapshot result; QString error;
        QVERIFY(!CPsdReader::ReadBytes(fixture({Layer(), Layer()}), result, &error, nullptr, limits)); QVERIFY(error.contains(QStringLiteral("总量")));
        Layer group, divider; group.kind = 1; group.bounds = {}; group.channels.clear(); divider = group; divider.kind = 3;
        limits = {}; limits.maximumDepth = 1;
        QVERIFY(!CPsdReader::ReadBytes(fixture({divider, divider, group, group}), result, &error, nullptr, limits)); QVERIFY(error.contains(QStringLiteral("层级")));
        limits = {}; limits.maximumLayers = 1;
        QVERIFY(!CPsdReader::ReadBytes(fixture({Layer(), Layer()}), result, &error, nullptr, limits)); QVERIFY(error.contains(QStringLiteral("数量")));
        auto invalid = fixture(); invalid.replace(44, 4, be32(0x80000000));
        QVERIFY(!CPsdReader::ReadBytes(invalid, result, &error));
    }
    void boundedByteMutationsNeverPublishPartialSnapshots() {
        const auto original = fixture(); CPsdSnapshot baseline; QVERIFY(CPsdReader::ReadBytes(original, baseline));
        CPsdReadLimits limits; limits.maximumFileBytes = 4096; limits.maximumMetadataBytes = 4096;
        limits.maximumDecodedBytes = 65536; limits.maximumLayerPixels = 4096; limits.maximumLayers = 16; limits.maximumDepth = 4;
        for (int index = 0; index < original.size(); ++index) for (int replacement : {0, 127, 255}) {
            auto bytes = original; bytes[index] = char(replacement); auto result = baseline; QString error;
            if (!CPsdReader::ReadBytes(bytes, result, &error, nullptr, limits)) {
                QVERIFY(!error.isEmpty()); QCOMPARE(result.bytes, original); QCOMPARE(result.layers[0].image, baseline.layers[0].image);
            } else {
                QCOMPARE(result.bytes, bytes);
                for (const auto& layer : result.layers) if (!layer.image.isNull()) QCOMPARE(layer.image.size(), layer.bounds.size());
            }
        }
    }
};
QTEST_GUILESS_MAIN(PsdReaderTests)
#include "psd_reader_tests.moc"
