#include "live2dstruct/cmocpreviewbuilder.h"
#include "live2dstruct/cmodeldocument.h"
#include "live2dstruct/csectiontable.h"

#include <QCryptographicHash>
#include <QFile>
#include <QtEndian>
#include <QtTest>
#include <cmath>
#include <cstring>
#include <limits>

using namespace live2dstruct;

namespace {
using Overrides = QHash<QString, QVector<QVector<float>>>;

QByteArray Source(const QString& name)
{
    QFile file(QString(TEST_RESOURCES) + '/' + name + '/' + name + ".moc3");
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

int Word(const QByteArray& bytes, int offset)
{
    return qFromLittleEndian<qint32>(bytes.constData() + offset);
}

void Put(QByteArray& bytes, int offset, int value)
{
    qToLittleEndian(qint32(value), bytes.data() + offset);
}

void PutFloat(QByteArray& bytes, int offset, float value)
{
    quint32 bits;
    std::memcpy(&bits, &value, 4);
    qToLittleEndian(bits, bytes.data() + offset);
}

float Float(const QByteArray& bytes, int offset)
{
    const quint32 bits = qFromLittleEndian<quint32>(bytes.constData() + offset);
    float value;
    std::memcpy(&value, &bits, 4);
    return value;
}

QVector<float> Row(const QByteArray& bytes, int row, int vertices = 3)
{
    const int begin = Word(bytes, Word(bytes, 0x158) + row * 4);
    const int pool = Word(bytes, 0x15c);
    QVector<float> result;
    for (int index = 0; index < vertices * 2; ++index) result.append(Float(bytes, pool + (begin + index) * 4));
    return result;
}

QVector<float> Changed(QVector<float> values)
{
    for (int index = 0; index < values.size(); ++index) values[index] += index % 2 ? -0.25f : 0.125f;
    return values;
}

void SetCount(QByteArray& bytes, int index, int value)
{
    Put(bytes, Word(bytes, 0x40) + index * 4, value);
}

void SetColumn(QByteArray& bytes, int slot, int index, int value)
{
    Put(bytes, Word(bytes, slot) + index * 4, value);
}

int AppendIntegers(QByteArray& bytes, std::initializer_list<int> values)
{
    const int originalSize = bytes.size();
    const int offset = int((bytes.size() + 63) & ~qsizetype(63));
    bytes.resize(offset + values.size() * 4);
    std::memset(bytes.data() + originalSize, 0, bytes.size() - originalSize);
    int index = 0;
    for (int value : values) Put(bytes, offset + index++ * 4, value);
    return offset;
}

QByteArray Fixture()
{
    QByteArray bytes = Source("Mark");
    if (bytes.isEmpty()) return {};
    for (int index = 0; index < 32; ++index) SetCount(bytes, index, 0);
    SetCount(bytes, 4, 2);
    SetCount(bytes, 9, 2);
    SetCount(bytes, 10, 12);
    SetCount(bytes, 15, 12);
    SetCount(bytes, 16, 6);
    for (int mesh = 0; mesh < 2; ++mesh) {
        const int id = Word(bytes, 0xc4) + mesh * 64;
        std::memset(bytes.data() + id, 0, 64);
        const QByteArray name = mesh ? QByteArray("MeshB") : QByteArray("MeshA");
        std::memcpy(bytes.data() + id, name.constData(), name.size());
        SetColumn(bytes, 0xc8, mesh, -1);
        SetColumn(bytes, 0xcc, mesh, mesh);
        SetColumn(bytes, 0xd0, mesh, 1);
        SetColumn(bytes, 0xd4, mesh, 1);
        SetColumn(bytes, 0xd8, mesh, 1);
        SetColumn(bytes, 0xdc, mesh, -1);
        SetColumn(bytes, 0xe0, mesh, -1);
        SetColumn(bytes, 0xe4, mesh, 0);
        bytes[Word(bytes, 0xe8) + mesh] = 4;
        SetColumn(bytes, 0xec, mesh, 3);
        SetColumn(bytes, 0xf0, mesh, mesh * 6);
        SetColumn(bytes, 0xf4, mesh, mesh * 3);
        SetColumn(bytes, 0xf8, mesh, 3);
        SetColumn(bytes, 0xfc, mesh, 0);
        SetColumn(bytes, 0x100, mesh, 0);
        PutFloat(bytes, Word(bytes, 0x150) + mesh * 4, 1.0f);
        PutFloat(bytes, Word(bytes, 0x154) + mesh * 4, float(mesh));
        SetColumn(bytes, 0x158, mesh, mesh * 6);
    }
    for (int index = 0; index < 12; ++index) {
        PutFloat(bytes, Word(bytes, 0x15c) + index * 4, float(index) / 8.0f);
        PutFloat(bytes, Word(bytes, 0x178) + index * 4, float(index % 3) / 2.0f);
    }
    for (int index = 0; index < 6; ++index)
        qToLittleEndian(quint16(index % 3), bytes.data() + Word(bytes, 0x17c) + index * 2);
    return bytes;
}

void AddArtBlendShape(QByteArray& bytes, int target, int row)
{
    SetCount(bytes, 26, 1);
    SetCount(bytes, 28, 1);
    for (const auto& value : {std::pair{0x220, -1}, {0x224, row}, {0x228, 1},
                              {0x22c, 0}, {0x230, 0}, {0x240, target}, {0x244, 0}, {0x248, 1}})
        Put(bytes, value.first, AppendIntegers(bytes, {value.second}));
}

void AddWarp(QByteArray& bytes, int begin, bool blendShape = false)
{
    SetCount(bytes, 2, 1);
    SetCount(bytes, 7, 1);
    for (const auto& value : {std::pair{0x8c, -1}, {0x90, 0}, {0x94, blendShape ? 0 : 1},
                              {0x98, 3}, {0x9c, 0}, {0xa0, 2}, {0x130, begin}})
        Put(bytes, value.first, AppendIntegers(bytes, {value.second}));
    if (!blendShape) return;
    SetCount(bytes, 26, 1);
    SetCount(bytes, 27, 1);
    for (const auto& value : {std::pair{0x220, -1}, {0x224, 0}, {0x228, 1},
                              {0x22c, 0}, {0x230, 0}, {0x234, 0}, {0x238, 0}, {0x23c, 1}})
        Put(bytes, value.first, AppendIntegers(bytes, {value.second}));
}

void Models()
{
    QTest::addColumn<QString>("model");
    for (const char* name : {"Mark", "Natori", "Rice", "Wanko"})
        QTest::newRow(name) << QString::fromLatin1(name);
}
}

class MocPreviewBuilderTests : public QObject
{
    Q_OBJECT
private slots:
    void realModelsEditEveryKeyform_data() { Models(); }
    void realModelsEditEveryKeyform()
    {
        QFETCH(QString, model);
        const QByteArray source = Source(model);
        QVERIFY(!source.isEmpty());
        CReader reader = CReader::FromBytes(source);
        CSectionTable table(reader);
        QVERIFY2(table.IsValid(), qPrintable(reader.Error()));
        QCOMPARE(Word(source, 0x158), table.sectionOffsetTable->art_mesh_keyforms->keyform_position_sources_begin_indices);
        QCOMPARE(Word(source, 0x15c), table.sectionOffsetTable->keyform_positions->xys);
        QCOMPARE(Word(source, Word(source, 0x40) + 40), table.count_info->keyform_positions);

        Overrides overrides;
        QByteArray expected = source;
        const int meshCount = Word(source, Word(source, 0x40) + 16);
        const int ids = Word(source, 0xc4);
        for (int mesh = 0; mesh < meshCount; ++mesh) {
            const QString id = QString::fromUtf8(source.constData() + ids + mesh * 64);
            const int first = Word(source, Word(source, 0xcc) + mesh * 4);
            const int count = Word(source, Word(source, 0xd0) + mesh * 4);
            const int vertices = Word(source, Word(source, 0xec) + mesh * 4);
            QVector<QVector<float>> forms;
            for (int local = 0; local < count; ++local) {
                const auto positions = Changed(Row(source, first + local, vertices));
                forms.append(positions);
                const int begin = Word(source, Word(source, 0x158) + (first + local) * 4);
                for (int index = 0; index < positions.size(); ++index)
                    PutFloat(expected, Word(source, 0x15c) + (begin + index) * 4, positions[index]);
            }
            overrides.insert(id, forms);
        }
        QVERIFY(!overrides.isEmpty());
        CMocPreviewBuilder builder;
        QString error;
        QVERIFY2(builder.Initialize(source, &error), qPrintable(error));
        QCOMPARE(builder.SourceDigest(), QCryptographicHash::hash(source, QCryptographicHash::Sha256));
        QByteArray output("unchanged");
        QVERIFY2(builder.Build(overrides, output, &error), qPrintable(error));
        QCOMPARE(output, expected);
        QCOMPARE(output.size(), source.size());
        CModelDocument document;
        QVERIFY2(document.LoadBytes(output, "memory.moc3", &error), qPrintable(error));
        for (auto it = overrides.cbegin(); it != overrides.cend(); ++it)
            for (int local = 0; local < it->size(); ++local)
                QCOMPARE(document.DrawableKeyformPositions(it.key(), local), (*it)[local]);
        CReader rebuiltReader = CReader::FromBytes(output);
        CSectionTable rebuilt(rebuiltReader);
        QVERIFY(rebuilt.IsValid());
        QCOMPARE(rebuilt.art_meshes.size(), table.art_meshes.size());
        QCOMPARE(rebuilt.warp_deformer_keyforms.size(), table.warp_deformer_keyforms.size());
        for (int row = 0; row < table.warp_deformer_keyforms.size(); ++row)
            QCOMPARE(rebuilt.warp_deformer_keyforms[row]->keyform_position_sources_begin_indices,
                     table.warp_deformer_keyforms[row]->keyform_position_sources_begin_indices);
        QVERIFY(builder.Build({}, output, &error));
        QCOMPARE(output, source);
    }

    void sharedPositionSlicesAreIsolated_data()
    {
        QTest::addColumn<int>("secondBegin");
        QTest::newRow("identical") << 0;
        QTest::newRow("partial") << 2;
    }
    void sharedPositionSlicesAreIsolated()
    {
        QFETCH(int, secondBegin);
        QByteArray source = Fixture();
        QVERIFY(!source.isEmpty());
        SetColumn(source, 0x158, 1, secondBegin);
        const auto beforeA = Row(source, 0);
        const auto beforeB = Row(source, 1);
        const auto afterA = Changed(beforeA);
        CMocPreviewBuilder builder;
        QString error;
        QVERIFY2(builder.Initialize(source, &error), qPrintable(error));
        QByteArray result;
        QVERIFY2(builder.Build({{"MeshA", {afterA}}}, result, &error), qPrintable(error));
        QVERIFY(result.size() > source.size());
        QCOMPARE(Word(result, 0x15c) % 64, 0);
        QCOMPARE(Word(result, 0x158) % 64, 0);
        QCOMPARE(result.size() % 64, 0);
        QCOMPARE(Row(result, 0), afterA);
        QCOMPARE(Row(result, 1), beforeB);
        QCOMPARE(result.mid(Word(result, 0x15c), 48), source.mid(Word(source, 0x15c), 48));
        QCOMPARE(Word(result, Word(result, 0x40) + 40), 18);
        QCOMPARE(Word(result, Word(result, 0x158)), 12);
        QCOMPARE(Word(result, Word(result, 0x158) + 4), secondBegin);
        QByteArray restored = result.left(source.size());
        Put(restored, 0x15c, Word(source, 0x15c));
        Put(restored, 0x158, Word(source, 0x158));
        SetCount(restored, 10, 12);
        QCOMPARE(restored, source);
        CModelDocument document;
        QVERIFY2(document.LoadBytes(result, "alias.moc3", &error), qPrintable(error));
        QCOMPARE(document.DrawableKeyformPositions("MeshA", 0), afterA);
        QCOMPARE(document.DrawableKeyformPositions("MeshB", 0), beforeB);
        const QByteArray firstResult = result;
        QVERIFY(builder.Build({{"MeshA", {afterA}}}, result));
        QCOMPARE(result, firstResult);
        QVERIFY(builder.Build({{"MeshA", {beforeA}}}, result));
        QCOMPARE(result, source);
        QVERIFY(builder.Build({}, result));
        QCOMPARE(result, source);
    }

    void physicalColumnsRemainUnchanged()
    {
        QByteArray source = Fixture();
        QVERIFY(!source.isEmpty());
        Put(source, 0x178, Word(source, 0x15c));
        Put(source, 0x174, Word(source, 0x158));
        SetCount(source, 14, 2);
        const QByteArray sourceUv = source.mid(Word(source, 0x178), 48);
        const QByteArray sourceKeys = source.mid(Word(source, 0x174), 8);
        CMocPreviewBuilder builder;
        QString error;
        QVERIFY2(builder.Initialize(source, &error), qPrintable(error));
        QByteArray result;
        const auto edited = Changed(Row(source, 0));
        QVERIFY2(builder.Build({{"MeshA", {edited}}}, result, &error), qPrintable(error));
        QCOMPARE(result.mid(Word(result, 0x178), 48), sourceUv);
        QCOMPARE(result.mid(Word(result, 0x174), 8), sourceKeys);
        QCOMPARE(Row(result, 0), edited);
        QCOMPARE(Row(result, 1), Row(source, 1));
        QVERIFY(Word(result, 0x15c) != Word(source, 0x15c));
        QVERIFY(Word(result, 0x158) != Word(source, 0x158));
    }

    void sharedGlobalRowsCanBeEditedIndependently()
    {
        QByteArray source = Fixture();
        QVERIFY(!source.isEmpty());
        SetColumn(source, 0xcc, 1, 0);
        CMocPreviewBuilder builder;
        QString error;
        QVERIFY2(builder.Initialize(source, &error), qPrintable(error));
        QByteArray result("sentinel");
        const auto original = Row(source, 0);
        const auto edited = Changed(original);
        QVERIFY2(builder.Build({{"MeshA", {edited}}}, result, &error), qPrintable(error));
        QCOMPARE(Row(result, Word(result, Word(result, 0xcc))), edited);
        QCOMPARE(Row(result, Word(result, Word(result, 0xcc) + 4)), original);
        QVERIFY2(builder.Build({{"MeshA", {edited}}, {"MeshB", {original}}}, result, &error), qPrintable(error));
        QCOMPARE(Row(result, Word(result, Word(result, 0xcc))), edited);
        QCOMPARE(Row(result, Word(result, Word(result, 0xcc) + 4)), original);
        QVERIFY2(builder.Build({{"MeshA", {edited}}, {"MeshB", {edited}}}, result, &error), qPrintable(error));
        CModelDocument document;
        QVERIFY2(document.LoadBytes(result, "shared-row.moc3", &error), qPrintable(error));
        QCOMPARE(document.DrawableKeyformPositions("MeshA", 0), edited);
        QCOMPARE(document.DrawableKeyformPositions("MeshB", 0), edited);
        QVERIFY(builder.Build({{"MeshA", {original}}}, result));
        QCOMPARE(result, source);
        SetColumn(source, 0xec, 1, 2);
        qToLittleEndian(quint16(1), source.data() + Word(source, 0x17c) + 5 * 2);
        QVERIFY(builder.Initialize(source));
        result = "sentinel";
        QVERIFY2(builder.Build({{"MeshA", {edited}}, {"MeshB", {edited.mid(0, 4)}}}, result, &error), qPrintable(error));
        QCOMPARE(Row(result, Word(result, Word(result, 0xcc))), edited);
        QCOMPARE(Row(result, Word(result, Word(result, 0xcc) + 4), 2), edited.mid(0, 4));
    }

    void blendShapeReferencesProtectOnlyUsedRows()
    {
        QByteArray source = Fixture();
        QVERIFY(!source.isEmpty());
        AddArtBlendShape(source, 1, 1);
        CMocPreviewBuilder builder;
        QString error;
        QVERIFY2(builder.Initialize(source, &error), qPrintable(error));
        QByteArray result;
        const auto a = Changed(Row(source, 0));
        const auto b = Changed(Row(source, 1));
        QVERIFY2(builder.Build({{"MeshA", {a}}}, result, &error), qPrintable(error));
        QCOMPARE(Row(result, 0), a);
        QCOMPARE(Row(result, 1), Row(source, 1));
        QVERIFY2(builder.Build({{"MeshB", {b}}}, result, &error), qPrintable(error));
        QCOMPARE(Row(result, Word(result, Word(result, 0xcc) + 4)), b);
        QCOMPARE(Row(result, 1), Row(source, 1));
        QCOMPARE(Word(result, Word(result, 0x224)), 1);
        QVERIFY(builder.Build({{"MeshB", {Row(source, 1)}}}, result));
        QCOMPARE(result, source);
        SetColumn(source, 0x158, 1, 0);
        QVERIFY(builder.Initialize(source));
        QVERIFY(builder.Build({{"MeshA", {a}}}, result));
        QVERIFY(result.size() > source.size());
        QCOMPARE(Row(result, 1), Row(source, 1));
    }

    void warpReferencesPreserveSharedCoordinates_data()
    {
        QTest::addColumn<bool>("blendShape");
        QTest::newRow("normal-warp") << false;
        QTest::newRow("blend-shape-warp") << true;
    }
    void warpReferencesPreserveSharedCoordinates()
    {
        QFETCH(bool, blendShape);
        QByteArray source = Fixture();
        QVERIFY(!source.isEmpty());
        AddWarp(source, 0, blendShape);
        CMocPreviewBuilder builder;
        QString error;
        QVERIFY2(builder.Initialize(source, &error), qPrintable(error));
        QByteArray result;
        QVERIFY2(builder.Build({{"MeshA", {Changed(Row(source, 0))}}}, result, &error), qPrintable(error));
        QVERIFY(result.size() > source.size());
        QCOMPARE(Word(result, Word(result, 0x130)), 0);
        QCOMPARE(result.mid(Word(result, 0x15c), 24), source.mid(Word(source, 0x15c), 24));
    }

    void stableOrderingAndSnapshotOwnership()
    {
        QByteArray input = Fixture();
        QVERIFY(!input.isEmpty());
        SetColumn(input, 0x158, 1, 0);
        const QByteArray source = input;
        CMocPreviewBuilder builder;
        QVERIFY(builder.Initialize(input));
        const auto digest = builder.SourceDigest();
        input.fill('x');
        QCOMPARE(builder.SourceDigest(), QCryptographicHash::hash(source, QCryptographicHash::Sha256));
        Overrides forward;
        forward.insert("MeshA", {Changed(Row(source, 0))});
        forward.insert("MeshB", {Changed(Changed(Row(source, 1)))});
        Overrides reverse;
        reverse.insert("MeshB", forward["MeshB"]);
        reverse.insert("MeshA", forward["MeshA"]);
        QByteArray first, second;
        QVERIFY(builder.Build(forward, first));
        QVERIFY(builder.Build(reverse, second));
        QCOMPARE(first, second);
        QVERIFY(builder.Build(forward, first));
        QCOMPARE(first, second);
        CMocPreviewBuilder copy = builder;
        QVERIFY(!builder.Initialize("invalid"));
        QCOMPARE(builder.SourceDigest(), digest);
        QVERIFY(builder.Build(reverse, second));
        QCOMPARE(first, second);
        QVERIFY(builder.Initialize(Fixture()));
        QVERIFY(copy.Build(reverse, second));
        QCOMPARE(first, second);
        QVERIFY(copy.Build({}, second));
        QCOMPARE(second, source);
    }

    void externallyOwnedBaselineIsCopied()
    {
        QByteArray storage = Fixture();
        QVERIFY(!storage.isEmpty());
        const QByteArray expected = storage;
        storage.detach();
        const auto borrowed = QByteArray::fromRawData(storage.constData(), storage.size());
        CMocPreviewBuilder builder;
        QVERIFY(builder.Initialize(borrowed));
        std::memset(storage.data(), 0, storage.size());
        QByteArray result;
        QVERIFY(builder.Build({}, result));
        QCOMPARE(result, expected);
        QVERIFY(builder.Build({{"MeshA", {Changed(Row(expected, 0))}}}, result));
        QCOMPARE(Row(result, 0), Changed(Row(expected, 0)));
        QCOMPARE(builder.SourceDigest(), QCryptographicHash::hash(expected, QCryptographicHash::Sha256));
    }

    void sparseLocalKeyformsUseTheirOriginalRows()
    {
        QByteArray source = Fixture();
        QVERIFY(!source.isEmpty());
        SetCount(source, 4, 1);
        SetColumn(source, 0xd0, 0, 2);
        CMocPreviewBuilder builder;
        QVERIFY(builder.Initialize(source));
        QByteArray result;
        const auto edited = Changed(Row(source, 1));
        QVERIFY(builder.Build({{"MeshA", {QVector<float>(), edited}}}, result));
        QCOMPARE(Row(result, 0), Row(source, 0));
        QCOMPARE(Row(result, 1), edited);
        QCOMPARE(result.size(), source.size());
    }

    void invalidOverridesPreserveOutput()
    {
        const QByteArray source = Fixture();
        QVERIFY(!source.isEmpty());
        CMocPreviewBuilder builder;
        QByteArray result("sentinel");
        QString error;
        QVERIFY(!builder.Build({}, result, &error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(result, QByteArray("sentinel"));
        QVERIFY(builder.SourceDigest().isEmpty());
        QVERIFY(builder.Initialize(source));
        const auto original = Row(source, 0);
        QVector<float> nan = original, infinity = original;
        nan[1] = std::numeric_limits<float>::quiet_NaN();
        infinity[0] = std::numeric_limits<float>::infinity();
        const QVector<Overrides> invalid{
            {{"Missing", {original}}}, {{"Missing", {}}}, {{"MeshA", {original, original}}},
            {{"MeshA", {original.mid(0, 4)}}}, {{"MeshA", {nan}}}, {{"MeshA", {infinity}}}
        };
        for (const auto& overrides : invalid) {
            QVERIFY(!builder.Build(overrides, result, &error));
            QVERIFY(!error.isEmpty());
            QCOMPARE(result, QByteArray("sentinel"));
        }
        QVERIFY(builder.Build({{"MeshA", {QVector<float>()}}}, result, &error));
        QVERIFY(error.isEmpty());
        QCOMPARE(result, source);
    }

    void invalidSourceLayouts_data()
    {
        QTest::addColumn<QString>("corruption");
        for (const char* value : {"magic", "version-zero", "version-six", "big-endian", "truncated", "offset",
                                  "negative-count", "too-many-positions", "vertices", "negative-vertices", "keyform-begin",
                                  "keyform-count", "negative-position", "position-overrun", "duplicate-id", "empty-id",
                                  "nan", "infinity", "blend-target", "blend-binding", "blend-row", "warp-position"})
            QTest::newRow(value) << QString::fromLatin1(value);
    }
    void invalidSourceLayouts()
    {
        QFETCH(QString, corruption);
        QByteArray bytes = Fixture();
        QVERIFY(!bytes.isEmpty());
        if (corruption == "magic") bytes[0] = 'X';
        else if (corruption == "version-zero") bytes[4] = 0;
        else if (corruption == "version-six") bytes[4] = 6;
        else if (corruption == "big-endian") bytes[5] = 1;
        else if (corruption == "truncated") bytes.truncate(0x200);
        else if (corruption == "offset") Put(bytes, 0x15c, std::numeric_limits<int>::max());
        else if (corruption == "negative-count") SetCount(bytes, 9, -1);
        else if (corruption == "too-many-positions") SetCount(bytes, 10, 1000001);
        else if (corruption == "vertices") SetColumn(bytes, 0xec, 0, std::numeric_limits<int>::max());
        else if (corruption == "negative-vertices") SetColumn(bytes, 0xec, 0, -1);
        else if (corruption == "keyform-begin") SetColumn(bytes, 0xcc, 0, std::numeric_limits<int>::max());
        else if (corruption == "keyform-count") SetColumn(bytes, 0xd0, 0, std::numeric_limits<int>::max());
        else if (corruption == "negative-position") SetColumn(bytes, 0x158, 0, -1);
        else if (corruption == "position-overrun") SetColumn(bytes, 0x158, 0, 10);
        else if (corruption == "duplicate-id") std::memcpy(bytes.data() + Word(bytes, 0xc4) + 64, bytes.constData() + Word(bytes, 0xc4), 64);
        else if (corruption == "empty-id") bytes[Word(bytes, 0xc4)] = '\0';
        else if (corruption == "nan") PutFloat(bytes, Word(bytes, 0x15c), std::numeric_limits<float>::quiet_NaN());
        else if (corruption == "infinity") PutFloat(bytes, Word(bytes, 0x15c), std::numeric_limits<float>::infinity());
        else if (corruption == "blend-target") AddArtBlendShape(bytes, 2, 1);
        else if (corruption == "blend-binding") { AddArtBlendShape(bytes, 1, 1); SetColumn(bytes, 0x244, 0, std::numeric_limits<int>::max()); }
        else if (corruption == "blend-row") AddArtBlendShape(bytes, 1, 2);
        else if (corruption == "warp-position") AddWarp(bytes, 10);
        CMocPreviewBuilder builder;
        QString error;
        QVERIFY(!builder.Initialize(bytes, &error));
        QVERIFY2(!error.isEmpty(), qPrintable(corruption));
        QVERIFY(builder.SourceDigest().isEmpty());
    }

    void relocationRespectsPositionBudget()
    {
        QByteArray bytes = Fixture();
        QVERIFY(!bytes.isEmpty());
        const int pool = int((bytes.size() + 63) & ~qsizetype(63));
        const auto original = bytes.mid(Word(bytes, 0x15c), 48);
        bytes.resize(pool + 1000000 * 4);
        std::memset(bytes.data() + pool, 0, 1000000 * 4);
        std::memcpy(bytes.data() + pool, original.constData(), original.size());
        Put(bytes, 0x15c, pool);
        SetCount(bytes, 10, 1000000);
        SetColumn(bytes, 0x158, 1, 0);
        CMocPreviewBuilder builder;
        QString error;
        QVERIFY2(builder.Initialize(bytes, &error), qPrintable(error));
        QByteArray output("sentinel");
        QVERIFY(!builder.Build({{"MeshA", {Changed(Row(bytes, 0))}}}, output, &error));
        QVERIFY(error.contains("超出"));
        QCOMPARE(output, QByteArray("sentinel"));
        SetColumn(bytes, 0x158, 1, 6);
        QVERIFY(builder.Initialize(bytes));
        QVERIFY2(builder.Build({{"MeshA", {Changed(Row(bytes, 0))}}}, output, &error), qPrintable(error));
        QCOMPARE(output.size(), bytes.size());
    }

    void allowedHeaderVersionsKeepReadLayout()
    {
        const auto source = Fixture();
        QVERIFY(!source.isEmpty());
        for (int version = 1; version <= 5; ++version) {
            auto bytes = source;
            bytes[4] = char(version);
            CMocPreviewBuilder builder;
            QString error;
            QVERIFY2(builder.Initialize(bytes, &error), qPrintable(error));
            QByteArray output;
            QVERIFY(builder.Build({{"MeshA", {Changed(Row(bytes, 0))}}}, output));
            QCOMPARE(output[4], char(version));
        }
    }
};

QTEST_GUILESS_MAIN(MocPreviewBuilderTests)
#include "preview_moc_builder_tests.moc"
