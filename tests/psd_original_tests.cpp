#include "psd/cpsdoriginal.h"
#include "std/cexportcontrol.h"
#include "psd_original_fixture.h"
#include <QCryptographicHash>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

namespace {
QByteArray digest(const QByteArray& bytes) { return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256); }
QByteArray read(const QString& path) { QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray(); }
bool write(const QString& path, const QByteArray& bytes) { QFile file(path); return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(); }
}

class PsdOriginalTests : public QObject
{
    Q_OBJECT
private slots:
    void directoryMatchesPixelsAndRestoresAllOriginalBytes_data()
    {
        QTest::addColumn<int>("compression"); QTest::addColumn<bool>("effects"); QTest::addColumn<bool>("preview");
        for (int compression = 0; compression < 4; ++compression) for (bool effects : {false, true}) for (bool preview : {false, true})
            QTest::newRow(qPrintable(QString("%1/%2/%3").arg(compression).arg(effects).arg(preview))) << compression << effects << preview;
    }
    void directoryMatchesPixelsAndRestoresAllOriginalBytes()
    {
        QFETCH(int, compression); QFETCH(bool, effects); QFETCH(bool, preview);
        const auto bytes = psd_original_fixture::fixture(compression, effects, preview);
        CPsdSnapshot pixels; CPsdDirectory directory; QString error;
        QVERIFY2(CPsdReader::ReadBytes(bytes, pixels, &error), qPrintable(error));
        QVERIFY2(CPsdReader::ReadDirectoryBytes(bytes, directory, &error), qPrintable(error));
        QCOMPARE(directory.bytes, bytes); QCOMPARE(directory.sha256, pixels.sha256); QCOMPARE(directory.canvasSize, pixels.canvasSize);
        QCOMPARE(directory.iccProfile, pixels.iccProfile); QCOMPARE(directory.displayOrder, pixels.displayOrder);
        QCOMPARE(directory.hasMergedTransparency, pixels.hasMergedTransparency); QCOMPARE(directory.hasMergedPreview, preview);
        QCOMPARE(directory.layers.size(), pixels.layers.size());
        for (int index = 0; index < directory.layers.size(); ++index) {
            const auto& a = directory.layers[index]; const auto& b = pixels.layers[index];
            QCOMPARE(a.recordIndex, b.recordIndex); QCOMPARE(a.parentIndex, b.parentIndex); QCOMPARE(a.kind, b.kind);
            QCOMPARE(a.layerId, b.layerId); QCOMPARE(a.name, b.name); QCOMPARE(a.bounds, b.bounds);
            QCOMPARE(a.opacity, b.opacity); QCOMPARE(a.blendMode, b.blendMode); QCOMPARE(a.visible, b.visible); QCOMPARE(a.clipping, b.clipping);
            QCOMPARE(a.additionalInfo.size(), b.additionalInfo.size());
            for (int tag = 0; tag < a.additionalInfo.size(); ++tag) {
                QCOMPARE(a.additionalInfo[tag].key, b.additionalInfo[tag].key); QCOMPARE(a.additionalInfo[tag].data, b.additionalInfo[tag].data);
            }
        }
        CPsdOriginal original;
        QVERIFY2(CPsdOriginal::Prepare(bytes, digest(bytes), original, &error), qPrintable(error));
        QTemporaryDir dir; const auto path = dir.filePath("recovered.psd");
        CExportControl control;
        QVERIFY2(original.Write(path, &control, &error), qPrintable(error)); QVERIFY(control.IsCommitted());
        QCOMPARE(read(path), bytes); QCOMPARE(digest(read(path)), digest(bytes));
    }
    void directoryBudgetCountsInflatedBytesWithoutAllocatingRgba()
    {
        const auto bytes = psd_original_fixture::fixture(); QString error; CPsdReadLimits limits; limits.maximumDecodedBytes = 32;
        CPsdDirectory directory;
        QVERIFY2(CPsdReader::ReadDirectoryBytes(bytes, directory, &error, nullptr, limits), qPrintable(error));
        CPsdSnapshot pixels; QVERIFY(!CPsdReader::ReadBytes(bytes, pixels, &error, nullptr, limits));
        limits.maximumDecodedBytes = 31;
        QVERIFY(!CPsdReader::ReadDirectoryBytes(bytes, directory, &error, nullptr, limits)); QCOMPARE(directory.bytes, bytes);
    }
    void groupedDirectoryRetainsPhysicalIdentityAndDisplayOrder()
    {
        const auto bytes = psd_original_fixture::fixture(3, true, true, QStringLiteral("层"), true);
        CPsdOriginal original; QString error;
        QVERIFY2(CPsdOriginal::Prepare(bytes, digest(bytes), original, &error), qPrintable(error));
        const auto& directory = *original.Directory();
        QCOMPARE(directory.displayOrder, QVector<int>({2, 1}));
        QCOMPARE(directory.layers[0].kind, CPsdLayer::Kind::GroupEnd);
        QCOMPARE(directory.layers[1].parentIndex, 2);
        QCOMPARE(directory.layers[2].kind, CPsdLayer::Kind::Group);
        QCOMPARE(directory.layers[2].name, QStringLiteral("源文件夹"));
        QCOMPARE(directory.layers[2].blendMode, QByteArray("pass"));
        QCOMPARE(directory.bytes, bytes);
    }
    void zipSinksValidateMultipleOutputBlocks_data()
    {
        QTest::addColumn<int>("compression");
        QTest::newRow("zip") << 2; QTest::newRow("prediction") << 3;
    }
    void zipSinksValidateMultipleOutputBlocks()
    {
        QFETCH(int, compression);
        const auto bytes = psd_original_fixture::largeSolid(512, 512, compression);
        CPsdDirectory directory; QString error; CPsdReadLimits limits;
        limits.maximumDecodedBytes = 8LL * 512 * 512;
        QVERIFY2(CPsdReader::ReadDirectoryBytes(bytes, directory, &error, nullptr, limits), qPrintable(error));
        QCOMPARE(directory.canvasSize, QSize(512, 512));
        CExportControl control; int count = 0;
        QVERIFY(control.SetHandler([&](const auto&, QString*) { if (++count == 20) control.Cancel(); return true; }));
        QVERIFY(!CPsdReader::ReadDirectoryBytes(bytes, directory, &error, &control));
        QVERIFY(control.WasCanceled()); QCOMPARE(directory.bytes, bytes);
        --limits.maximumDecodedBytes;
        QVERIFY(!CPsdReader::ReadDirectoryBytes(bytes, directory, &error, nullptr, limits));
    }
    void maskParameterLayoutsRequireCompleteUnambiguousFields_data()
    {
        QTest::addColumn<QByteArray>("mask"); QTest::addColumn<bool>("includeReal");
        using namespace psd_original_fixture;
        const auto base = u32(0) + u32(0) + u32(2) + u32(2) + QByteArray::fromHex("ff10");
        const auto real = QByteArray::fromHex("00ff") + u32(0) + u32(0) + u32(1) + u32(2);
        const auto parameters = QByteArray::fromHex("0fcc4004cccccccccccdfa3fd3333333333333");
        QTest::newRow("real-before-parameters") << base + real + parameters + QByteArray(1, 0) << true;
        QTest::newRow("parameters-before-real") << base + parameters + real + QByteArray(1, 0) << true;
        auto realFlagged = real; realFlagged[0] = char(16);
        auto baseUnflagged = base; baseUnflagged[17] = char(0);
        QTest::newRow("real-flag-activates-parameters") << baseUnflagged + realFlagged + parameters + QByteArray(1, 0) << true;
        QTest::newRow("single-vector-density") << base + QByteArray::fromHex("04cc") << false;
        QTest::newRow("single-user-density-feather") << base + QByteArray::fromHex("03e64018000000000000") << false;
    }
    void maskParameterLayoutsRequireCompleteUnambiguousFields()
    {
        QFETCH(QByteArray, mask); QFETCH(bool, includeReal);
        const auto bytes = psd_original_fixture::maskFixture(mask, 2, 1, includeReal);
        CPsdSnapshot pixels; CPsdDirectory directory; CPsdOriginal original; QString error;
        QVERIFY2(CPsdReader::ReadBytes(bytes, pixels, &error), qPrintable(error));
        QVERIFY2(CPsdReader::ReadDirectoryBytes(bytes, directory, &error), qPrintable(error));
        QVERIFY2(CPsdOriginal::Prepare(bytes, digest(bytes), original, &error), qPrintable(error));
        QCOMPARE(directory.layers.size(), 1); QCOMPARE(directory.layers[0].bounds, QRect(0, 0, 2, 2));
        QCOMPARE(pixels.layers[0].image.size(), QSize(2, 2)); QCOMPARE(original.Directory()->bytes, bytes);
        QTemporaryDir dir; const auto path = dir.filePath("mask.psd");
        QVERIFY(original.Write(path, nullptr, &error)); QCOMPARE(read(path), bytes);
    }
    void malformedAndAmbiguousMaskTailsRemainRejected_data()
    {
        QTest::addColumn<QByteArray>("mask"); QTest::addColumn<int>("realWidth"); QTest::addColumn<int>("realHeight");
        using namespace psd_original_fixture;
        const auto base = u32(0) + u32(0) + u32(2) + u32(2) + QByteArray::fromHex("ff10");
        const auto real = QByteArray::fromHex("00ff") + u32(0) + u32(0) + u32(1) + u32(2);
        const auto parameters = QByteArray::fromHex("0fcc4004cccccccccccdfa3fd3333333333333");
        QTest::newRow("unknown-parameter-flags") << base + real + QByteArray::fromHex("10") + QByteArray(3, 0) << 2 << 1;
        QTest::newRow("nonzero-padding") << base + real + parameters + QByteArray(1, 1) << 2 << 1;
        QTest::newRow("missing-parameters") << base + real << 2 << 1;
        QTest::newRow("truncated-real-bounds") << base + real.left(16) << 2 << 1;
        QTest::newRow("excess-zero-padding") << base + real + parameters + QByteArray(5, 0) << 2 << 1;
        const auto emptyReal = QByteArray::fromHex("0100") + QByteArray(16, 0);
        QTest::newRow("same-bounds-different-parameters") << base + emptyReal + QByteArray(4, 0) << 0 << 0;
        const auto densityZeroReal = QByteArray(18, 0);
        QTest::newRow("different-real-bounds") << base + densityZeroReal + QByteArray::fromHex("04000000") << 0 << 0;
    }
    void malformedAndAmbiguousMaskTailsRemainRejected()
    {
        QFETCH(QByteArray, mask); QFETCH(int, realWidth); QFETCH(int, realHeight);
        const auto bytes = psd_original_fixture::maskFixture(mask, realWidth, realHeight);
        CPsdSnapshot pixels; CPsdDirectory directory; QString error;
        QVERIFY(!CPsdReader::ReadBytes(bytes, pixels, &error)); QVERIFY(!error.isEmpty());
        QVERIFY(!CPsdReader::ReadDirectoryBytes(bytes, directory, &error)); QVERIFY(!error.isEmpty());
        if (QByteArray(QTest::currentDataTag()).contains("different")) QVERIFY(error.contains(QStringLiteral("歧义")));
    }
    void largeDirectoryChecksAllChannelsWithABoundedOutputBuffer()
    {
        const auto bytes = psd_original_fixture::largeSolid(4096, 4096);
        QVERIFY(bytes.size() < 3 * 1024 * 1024);
        CPsdReadLimits limits;
        limits.maximumDecodedBytes = 8LL * 4096 * 4096;
        CPsdDirectory directory;
        QString error;
        QVERIFY2(CPsdReader::ReadDirectoryBytes(bytes, directory, &error, nullptr, limits), qPrintable(error));
        QCOMPARE(directory.layers.size(), 1);
        QCOMPARE(directory.layers[0].bounds, QRect(0, 0, 4096, 4096));
        QCOMPARE(directory.bytes, bytes);
        --limits.maximumDecodedBytes;
        QVERIFY(!CPsdReader::ReadDirectoryBytes(bytes, directory, &error, nullptr, limits));
        QCOMPARE(directory.bytes, bytes);
        CExportControl control;
        int checkpoints = 0;
        QVERIFY(control.SetHandler([&](const auto&, QString*) { if (++checkpoints == 100) control.Cancel(); return true; }));
        QVERIFY(!CPsdReader::ReadDirectoryBytes(bytes, directory, &error, &control));
        QVERIFY(control.WasCanceled()); QCOMPARE(directory.bytes, bytes);
    }
    void malformedStreamsAndDigestDoNotReplacePreviousArtifact_data()
    {
        QTest::addColumn<int>("compression");
        for (int compression = 0; compression < 4; ++compression) QTest::newRow(qPrintable(QString::number(compression))) << compression;
    }
    void malformedStreamsAndDigestDoNotReplacePreviousArtifact()
    {
        QFETCH(int, compression);
        const auto bytes = psd_original_fixture::fixture(compression, true);
        CPsdOriginal original; QString error;
        QVERIFY(CPsdOriginal::Prepare(bytes, digest(bytes), original, &error)); const auto* retained = original.Directory();
        for (int length = 0; length < bytes.size(); ++length) {
            const auto broken = bytes.left(length);
            QVERIFY(!CPsdOriginal::Prepare(broken, digest(broken), original, &error)); QCOMPARE(original.Directory(), retained);
        }
        auto trailing = bytes + 'x'; QVERIFY(!CPsdOriginal::Prepare(trailing, digest(trailing), original, &error));
        QCOMPARE(original.Directory(), retained);
        QVERIFY(!CPsdOriginal::Prepare(bytes, QByteArray(32, 'x'), original, &error)); QCOMPARE(original.Directory(), retained);
        auto corrupt = bytes;
        const auto channel = psd_original_fixture::channel(QByteArray::fromHex("01c840e0"), 2, compression);
        const auto at = corrupt.indexOf(channel); QVERIFY(at >= 0);
        if (compression == 0) corrupt[at + 1] = 4;
        else corrupt[at + 2] = char(255);
        QVERIFY(!CPsdOriginal::Prepare(corrupt, digest(corrupt), original, &error)); QCOMPARE(original.Directory(), retained);
    }
    void borrowedBytesAndExpectedDigestAreOwnedBeforeCallbacks()
    {
        auto source = psd_original_fixture::fixture(3, true); const auto expected = source;
        auto hash = digest(source); const auto borrowed = QByteArray::fromRawData(source.constData(), source.size());
        const auto borrowedHash = QByteArray::fromRawData(hash.constData(), hash.size());
        CExportControl control; bool called = false;
        QVERIFY(control.SetHandler([&](const auto&, QString*) { called = true; source.fill('x'); hash.fill('x'); return true; }));
        CPsdOriginal result; QString error;
        QVERIFY2(CPsdOriginal::Prepare(borrowed, borrowedHash, result, &error, &control), qPrintable(error));
        QVERIFY(called); QCOMPARE(result.Directory()->bytes, expected); QCOMPARE(result.Directory()->sha256, digest(expected));
    }
    void writingOwnsReceiverAndPathBeforeCallbacks()
    {
        const auto source = psd_original_fixture::fixture(2, true); CPsdOriginal original; QVERIFY(CPsdOriginal::Prepare(source, digest(source), original));
        QTemporaryDir dir; QString path = dir.filePath("owned.psd"); const QString expectedPath = path;
        const auto borrowedPath = QString::fromRawData(path.constData(), path.size());
        CExportControl control; bool called = false;
        QVERIFY(control.SetHandler([&](const auto&, QString*) { called = true; original = {}; path.fill(QLatin1Char('x')); return true; }));
        QString error; QVERIFY2(original.Write(borrowedPath, &control, &error), qPrintable(error));
        QVERIFY(called); QVERIFY(!original.IsValid()); QCOMPARE(read(expectedPath), source);
    }
    void cancellationAndFailuresPreserveDestination_data()
    {
        QTest::addColumn<int>("phase");
        for (auto phase : {CExportControl::Phase::Preparing, CExportControl::Phase::Writing, CExportControl::Phase::Committing})
            QTest::newRow(qPrintable(QString::number(int(phase)))) << int(phase);
    }
    void cancellationAndFailuresPreserveDestination()
    {
        QFETCH(int, phase); const auto bytes = psd_original_fixture::fixture(); CPsdOriginal original;
        QVERIFY(CPsdOriginal::Prepare(bytes, digest(bytes), original)); QTemporaryDir dir; const auto path = dir.filePath("output.psd");
        QVERIFY(write(path, "before")); CExportControl control;
        QVERIFY(control.SetHandler([&](const auto& progress, QString*) { if (int(progress.phase) == phase) control.Cancel(); return true; }));
        QString error; QVERIFY(!original.Write(path, &control, &error)); QVERIFY(control.WasCanceled()); QCOMPARE(read(path), QByteArray("before"));
    }
    void symbolicLinksAndInvalidArtifactsCannotWrite()
    {
        QTemporaryDir dir; const auto source = dir.filePath("source.psd"), target = dir.filePath("target.psd");
        QVERIFY(write(source, "source")); QVERIFY(QFile::link(source, target)); const auto bytes = psd_original_fixture::fixture();
        CPsdOriginal original; QVERIFY(CPsdOriginal::Prepare(bytes, digest(bytes), original)); QString error;
        QVERIFY(!original.Write(target, nullptr, &error)); QCOMPARE(read(source), QByteArray("source"));
        QVERIFY(!CPsdOriginal().Write(source, nullptr, &error)); QCOMPARE(read(source), QByteArray("source"));
    }
};
QTEST_GUILESS_MAIN(PsdOriginalTests)
#include "psd_original_tests.moc"
