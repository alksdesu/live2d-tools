#include "live2dstruct/cmocpreviewbuilder.h"
#include "live2dstruct/csectiontable.h"

#include <QCryptographicHash>
#include <QFile>
#include <QtEndian>
#include <QtTest>
#include <array>
#include <cstring>
#include <limits>
#include <utility>

using namespace live2dstruct;

namespace {
using Overrides = CMocPreviewBuilder::Overrides;

QByteArray sourceBytes(const QString& name)
{
    QFile file(QString(TEST_RESOURCES) + '/' + name + '/' + name + ".moc3");
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

int word(const QByteArray& bytes, int address)
{
    Q_ASSERT(address >= 0 && address <= bytes.size() - 4);
    return qFromLittleEndian<qint32>(bytes.constData() + address);
}

void putWord(QByteArray& bytes, int address, int value)
{
    Q_ASSERT(address >= 0 && address <= bytes.size() - 4);
    qToLittleEndian(qint32(value), bytes.data() + address);
}

int count(const QByteArray& bytes, int index) { return word(bytes, word(bytes, 0x40) + index * 4); }
int column(const QByteArray& bytes, int slot, int row) { return word(bytes, word(bytes, slot) + row * 4); }
void setCount(QByteArray& bytes, int index, int value) { putWord(bytes, word(bytes, 0x40) + index * 4, value); }
void setColumn(QByteArray& bytes, int slot, int row, int value) { putWord(bytes, word(bytes, slot) + row * 4, value); }

void putFloat(QByteArray& bytes, int address, float value)
{
    qint32 bits;
    std::memcpy(&bits, &value, 4);
    putWord(bytes, address, bits);
}

QString idAt(const QByteArray& bytes, int mesh)
{
    return QString::fromUtf8(bytes.mid(word(bytes, 0xc4) + mesh * 64, 64).split('\0').first());
}

QVector<int> masksAt(const QByteArray& bytes, int mesh)
{
    QVector<int> result;
    const int first = column(bytes, 0xfc, mesh), length = column(bytes, 0x100, mesh);
    for (int index = 0; index < length; ++index) result.append(word(bytes, word(bytes, 0x180) + (first + index) * 4));
    return result;
}

int appendWords(QByteArray& bytes, const QVector<int>& values)
{
    const int address = int((bytes.size() + 63) & ~qsizetype(63));
    bytes.append(QByteArray(address + values.size() * 4 - bytes.size(), '\0'));
    for (int index = 0; index < values.size(); ++index) putWord(bytes, address + index * 4, values[index]);
    return address;
}

QVector<float> positionsAt(const QByteArray& bytes, int mesh)
{
    const int row = column(bytes, 0xcc, mesh);
    const int first = column(bytes, 0x158, row), length = column(bytes, 0xec, mesh) * 2;
    QVector<float> values;
    for (int index = 0; index < length; ++index) {
        const int bits = word(bytes, word(bytes, 0x15c) + (first + index) * 4);
        float value;
        std::memcpy(&value, &bits, 4);
        values.append(value);
    }
    return values;
}

QByteArray fixture()
{
    QByteArray bytes = sourceBytes(QStringLiteral("Mark"));
    if (bytes.isEmpty()) return {};
    for (int index = 0; index < 32; ++index) setCount(bytes, index, 0);
    for (const auto& entry : {std::pair{4, 4}, {9, 4}, {10, 24}, {15, 24}, {16, 12}, {17, 4}})
        setCount(bytes, entry.first, entry.second);
    const std::array<int, 4> begins{0, 2, 3, 3}, lengths{2, 1, 0, 1};
    const std::array<quint8, 4> flags{0x80, 0x07, 0xa8, 0xff};
    for (int mesh = 0; mesh < 4; ++mesh) {
        const auto id = QStringLiteral("Mesh%1").arg(mesh).toUtf8();
        bytes.replace(word(bytes, 0xc4) + mesh * 64, 64, id.leftJustified(64, '\0'));
        for (const auto& entry : {std::pair{0xc8, -1}, {0xcc, mesh}, {0xd0, 1}, {0xd4, 1}, {0xd8, 1},
                                  {0xdc, -1}, {0xe0, -1}, {0xe4, 0}, {0xec, 3}, {0xf0, mesh * 6},
                                  {0xf4, mesh * 3}, {0xf8, 3}, {0xfc, begins[mesh]}, {0x100, lengths[mesh]},
                                  {0x158, mesh * 6}})
            setColumn(bytes, entry.first, mesh, entry.second);
        bytes[word(bytes, 0xe8) + mesh] = char(flags[mesh]);
        putFloat(bytes, word(bytes, 0x150) + mesh * 4, 1.0f);
        putFloat(bytes, word(bytes, 0x154) + mesh * 4, float(mesh));
    }
    for (int index = 0; index < 24; ++index) {
        putFloat(bytes, word(bytes, 0x15c) + index * 4, float(index) / 8.0f);
        putFloat(bytes, word(bytes, 0x178) + index * 4, float(index % 3) / 2.0f);
    }
    for (int index = 0; index < 12; ++index)
        qToLittleEndian(quint16(index % 3), bytes.data() + word(bytes, 0x17c) + index * 2);
    const QVector<int> pool{1, 2, 2, 0};
    for (int index = 0; index < pool.size(); ++index) putWord(bytes, word(bytes, 0x180) + index * 4, pool[index]);
    return bytes;
}

bool parses(const QByteArray& bytes, QString* error)
{
    auto reader = CReader::FromBytes(bytes);
    CSectionTable table(reader);
    if (error) *error = reader.Error();
    return table.IsValid();
}
}

class MaskMocPreviewTests : public QObject
{
    Q_OBJECT
private slots:
    void realModels_data()
    {
        QTest::addColumn<QString>("model");
        for (const char* name : {"Mark", "Natori", "Rice", "Wanko"}) QTest::newRow(name) << QString::fromLatin1(name);
    }

    void realModels()
    {
        QFETCH(QString, model);
        const QByteArray source = sourceBytes(model);
        QVERIFY(!source.isEmpty());
        const int meshCount = count(source, 4);
        Overrides edits, unchanged;
        QByteArray expected = source;
        for (int mesh = 0; mesh < meshCount; ++mesh) {
            const auto id = idAt(source, mesh);
            const auto original = masksAt(source, mesh);
            QVector<int> changed;
            for (int index = 0; index < original.size(); ++index) changed.append((mesh + index + 1) % meshCount);
            edits.drawableMasks.insert(id, changed);
            unchanged.drawableMasks.insert(id, original);
            const int flags = quint8(source[word(source, 0xe8) + mesh]);
            edits.drawableMaskInverted.insert(id, (flags & 8) == 0);
            unchanged.drawableMaskInverted.insert(id, (flags & 8) != 0);
            expected[word(source, 0xe8) + mesh] = char(flags ^ 8);
            const int begin = column(source, 0xfc, mesh);
            for (int index = 0; index < changed.size(); ++index)
                putWord(expected, word(source, 0x180) + (begin + index) * 4, changed[index]);
        }
        CMocPreviewBuilder builder;
        QString error;
        QVERIFY2(builder.Initialize(source, &error), qPrintable(error));
        QCOMPARE(builder.SourceDigest(), QCryptographicHash::hash(source, QCryptographicHash::Sha256));
        QByteArray output;
        QVERIFY2(builder.BuildOverrides(edits, output, &error), qPrintable(error));
        QCOMPARE(output, expected);
        CModelDocument document;
        QVERIFY2(document.LoadBytes(output, "memory.moc3", &error), qPrintable(error));
        for (int mesh = 0; mesh < meshCount; ++mesh) {
            const auto id = idAt(source, mesh);
            QCOMPARE(document.DrawableMaskIndices(id), edits.drawableMasks.value(id));
            QCOMPARE(document.DrawableMaskInverted(id), edits.drawableMaskInverted.value(id));
        }
        QVERIFY(builder.BuildOverrides(unchanged, output, &error));
        QCOMPARE(output, source);
        QVERIFY(builder.Build({}, output, &error));
        QCOMPARE(output, source);
    }

    void allFlagBytesPreserveOtherBits()
    {
        for (int flags = 0; flags <= 255; ++flags) {
            QByteArray source = fixture();
            source[word(source, 0xe8)] = char(flags);
            CMocPreviewBuilder builder;
            QVERIFY(builder.Initialize(source));
            Overrides edits;
            edits.drawableMaskInverted.insert("Mesh0", (flags & 8) == 0);
            QByteArray output;
            QVERIFY(builder.BuildOverrides(edits, output));
            QByteArray expected = source;
            expected[word(source, 0xe8)] = char(flags ^ 8);
            QCOMPARE(output, expected);
            edits.drawableMaskInverted["Mesh0"] = (flags & 8) != 0;
            QVERIFY(builder.BuildOverrides(edits, output));
            QCOMPARE(output, source);
        }
    }

    void growShrinkClearAndEmptyPool()
    {
        const QByteArray source = fixture();
        CMocPreviewBuilder builder;
        QVERIFY(builder.Initialize(source));
        Overrides edits;
        edits.drawableMasks = {{"Mesh0", {3}}, {"Mesh1", {}}, {"Mesh2", {0, 1, 3}}};
        edits.drawableMaskInverted = {{"Mesh2", false}};
        QByteArray output;
        QString error;
        QVERIFY2(builder.BuildOverrides(edits, output, &error), qPrintable(error));
        QCOMPARE(masksAt(output, 0), QVector<int>{3});
        QVERIFY(masksAt(output, 1).isEmpty());
        QCOMPARE(masksAt(output, 2), (QVector<int>{0, 1, 3}));
        QCOMPARE(masksAt(output, 3), masksAt(source, 3));
        QCOMPARE(column(output, 0xfc, 1), column(source, 0xfc, 1));
        QCOMPARE(count(output, 17), count(source, 17) + 4);
        QCOMPARE(word(output, 0x180) % 64, 0);
        QCOMPARE(output.mid(word(output, 0x180), count(source, 17) * 4), source.mid(word(source, 0x180), count(source, 17) * 4));
        QByteArray repeated;
        QVERIFY(builder.BuildOverrides(edits, repeated));
        QCOMPARE(repeated, output);
        QVERIFY(builder.BuildOverrides({}, repeated));
        QCOMPARE(repeated, source);

        edits = {};
        edits.drawableMasks.insert("Mesh0", {});
        QVERIFY(builder.BuildOverrides(edits, output));
        QByteArray expected = source;
        setColumn(expected, 0x100, 0, 0);
        QCOMPARE(output, expected);

        const QByteArray wanko = sourceBytes("Wanko");
        QVERIFY(builder.Initialize(wanko));
        QCOMPARE(count(wanko, 17), 0);
        edits = {};
        edits.drawableMasks.insert(idAt(wanko, 0), {1});
        QVERIFY2(builder.BuildOverrides(edits, output, &error), qPrintable(error));
        QCOMPARE(count(output, 17), 1);
        QCOMPARE(masksAt(output, 0), QVector<int>{1});
        QVERIFY2(parses(output, &error), qPrintable(error));
    }

    void sharedSlices_data()
    {
        QTest::addColumn<int>("begin");
        QTest::addColumn<int>("length");
        QTest::newRow("exact") << 0 << 2;
        QTest::newRow("partial") << 1 << 1;
    }

    void sharedSlices()
    {
        QFETCH(int, begin);
        QFETCH(int, length);
        QByteArray source = fixture();
        setColumn(source, 0xfc, 3, begin);
        setColumn(source, 0x100, 3, length);
        CMocPreviewBuilder builder;
        QVERIFY(builder.Initialize(source));
        Overrides edits;
        edits.drawableMasks.insert("Mesh0", {2, 3});
        QByteArray output;
        QVERIFY(builder.BuildOverrides(edits, output));
        QCOMPARE(masksAt(output, 0), (QVector<int>{2, 3}));
        QCOMPARE(masksAt(output, 3), masksAt(source, 3));
        QVERIFY(word(output, 0x180) >= source.size());
        QCOMPARE(column(output, 0xfc, 0), count(source, 17));
        QCOMPARE(output.mid(word(output, 0x180), count(source, 17) * 4), source.mid(word(source, 0x180), count(source, 17) * 4));
        QCOMPARE(output.mid(word(source, 0x180), count(source, 17) * 4), source.mid(word(source, 0x180), count(source, 17) * 4));
    }

    void physicalAliases_data()
    {
        QTest::addColumn<int>("slot");
        QTest::addColumn<int>("delta");
        QTest::newRow("flag-int-exact") << 0xe8 << 0;
        QTest::newRow("flag-int-partial") << 0xe8 << 1;
        QTest::newRow("begin-int-exact") << 0xfc << 0;
        QTest::newRow("count-int-exact") << 0x100 << 0;
        QTest::newRow("pool-int-exact") << 0x180 << 0;
        QTest::newRow("pool-int-partial") << 0x180 << 4;
    }

    void physicalAliases()
    {
        QFETCH(int, slot);
        QFETCH(int, delta);
        QByteArray source = fixture();
        const int storage = appendWords(source, {0, 0, 0, 0, 0});
        putWord(source, 0xb4, storage);
        const int width = slot == 0xe8 ? 1 : 4;
        source.replace(storage + delta, 4 * width, source.mid(word(source, slot), 4 * width));
        putWord(source, slot, storage + delta);
        const QByteArray sentinel = source.mid(storage, 20);
        CMocPreviewBuilder builder;
        QVERIFY(builder.Initialize(source));
        Overrides edits;
        if (slot == 0xe8) edits.drawableMaskInverted.insert("Mesh0", true);
        else if (slot == 0x100) edits.drawableMasks.insert("Mesh0", {});
        else if (slot == 0xfc) edits.drawableMasks.insert("Mesh2", {0});
        else edits.drawableMasks.insert("Mesh0", {2, 3});
        QByteArray output;
        QString error;
        QVERIFY2(builder.BuildOverrides(edits, output, &error), qPrintable(error));
        QCOMPARE(output.mid(storage, 20), sentinel);
        QVERIFY(word(output, slot) >= source.size());
        QCOMPARE(word(output, slot) % 64, 0);
        if (slot == 0xe8) {
            QCOMPARE(quint8(output[word(output, slot)]), quint8(0x88));
            QCOMPARE(output.mid(word(output, slot) + 1, 3), source.mid(storage + delta + 1, 3));
        } else if (slot == 0x100) QVERIFY(masksAt(output, 0).isEmpty());
        else if (slot == 0xfc) QCOMPARE(masksAt(output, 2), QVector<int>{0});
        else QCOMPARE(masksAt(output, 0), (QVector<int>{2, 3}));
        QVERIFY2(parses(output, &error), qPrintable(error));
    }

    void maskPositionAndFlagAliasesRemainIndependent()
    {
        QByteArray source = fixture();
        const int positions = word(source, 0x15c);
        putWord(source, 0x180, positions);
        for (int index = 0; index < 4; ++index) putWord(source, positions + index * 4, QVector<int>{1, 2, 2, 0}[index]);
        putWord(source, 0xe8, positions + 4);
        const auto originalPosition = positionsAt(source, 0);
        auto changedPosition = originalPosition;
        changedPosition[0] = 0.75f;
        Overrides edits;
        edits.artMeshPositions.insert("Mesh0", {changedPosition});
        edits.drawableMasks.insert("Mesh0", {2, 3});
        edits.drawableMaskInverted.insert("Mesh0", true);
        CMocPreviewBuilder builder;
        QVERIFY(builder.Initialize(source));
        QByteArray output;
        QString error;
        QVERIFY2(builder.BuildOverrides(edits, output, &error), qPrintable(error));
        QCOMPARE(positionsAt(output, 0), changedPosition);
        QCOMPARE(positionsAt(output, 1), positionsAt(source, 1));
        QCOMPARE(masksAt(output, 0), (QVector<int>{2, 3}));
        QCOMPARE(masksAt(output, 1), masksAt(source, 1));
        QCOMPARE(quint8(output[word(output, 0xe8)]), quint8(quint8(source[word(source, 0xe8)]) | 8));
        QCOMPARE(output.mid(positions, 24 * 4), source.mid(positions, 24 * 4));
        QVERIFY(word(output, 0x15c) >= source.size());
        QVERIFY(word(output, 0x180) >= source.size());
        QVERIFY(word(output, 0xe8) >= source.size());
        QVERIFY2(parses(output, &error), qPrintable(error));
    }

    void overlappingBeginAndCountPreserveSharedCells()
    {
        QByteArray source = fixture();
        const int storage = appendWords(source, {0, 2, 1, 3, 1});
        putWord(source, 0xfc, storage);
        putWord(source, 0x100, storage + 4);
        CMocPreviewBuilder builder;
        QVERIFY(builder.Initialize(source));
        Overrides edits;
        edits.drawableMasks.insert("Mesh0", {1, 2, 3});
        QByteArray output;
        QVERIFY(builder.BuildOverrides(edits, output));
        QCOMPARE(masksAt(output, 0), (QVector<int>{1, 2, 3}));
        for (int mesh = 1; mesh < 4; ++mesh) QCOMPARE(masksAt(output, mesh), masksAt(source, mesh));
        QCOMPARE(word(output, 0xfc), storage);
        QCOMPARE(column(output, 0xfc, 0), count(source, 17));
        QCOMPARE(output.mid(storage + 4, 16), source.mid(storage + 4, 16));
        QVERIFY(word(output, 0x100) >= source.size());
        QCOMPARE(column(output, 0x100, 0), 3);
        QCOMPARE(output.mid(word(output, 0x100) + 4, 12), source.mid(word(source, 0x100) + 4, 12));
        QCOMPARE(output.mid(word(output, 0x180), count(source, 17) * 4),
                 source.mid(word(source, 0x180), count(source, 17) * 4));
        QCOMPARE(count(output, 17), count(source, 17) + 3);
        QByteArray expectedPrefix = source;
        putWord(expectedPrefix, storage, count(source, 17));
        putWord(expectedPrefix, 0x100, word(output, 0x100));
        putWord(expectedPrefix, 0x180, word(output, 0x180));
        setCount(expectedPrefix, 17, count(source, 17) + 3);
        QCOMPARE(output.left(source.size()), expectedPrefix);
    }

    void stableOrderAndMaskCycles()
    {
        const QByteArray source = fixture();
        Overrides first, second;
        first.drawableMasks.insert("Mesh0", {1});
        first.drawableMasks.insert("Mesh1", {0, 2, 3});
        second.drawableMasks.insert("Mesh1", {0, 2, 3});
        second.drawableMasks.insert("Mesh0", {1});
        first.drawableMaskInverted.insert("Mesh0", true);
        first.drawableMaskInverted.insert("Mesh3", false);
        second.drawableMaskInverted.insert("Mesh3", false);
        second.drawableMaskInverted.insert("Mesh0", true);
        CMocPreviewBuilder builder;
        QVERIFY(builder.Initialize(source));
        QByteArray a, b;
        QVERIFY(builder.BuildOverrides(first, a));
        QVERIFY(builder.BuildOverrides(second, b));
        QCOMPARE(a, b);
        QCOMPARE(masksAt(a, 0), QVector<int>{1});
        QCOMPARE(masksAt(a, 1), (QVector<int>{0, 2, 3}));
    }

    void allOverrideKindsShareOneCandidate()
    {
        const QByteArray source = sourceBytes("Mark");
        CModelDocument document;
        QString error;
        QVERIFY2(document.LoadBytes(source, "memory.moc3", &error), qPrintable(error));
        Overrides edits;
        const QString meshId = idAt(source, 0);
        auto position = document.DrawableKeyformPositions(meshId, 0);
        QVERIFY(!position.isEmpty());
        position[0] += 0.125f;
        edits.artMeshPositions.insert(meshId, {position});
        edits.drawableMasks.insert(meshId, {1, 2});
        edits.drawableMaskInverted.insert(meshId, true);
        for (const auto& deformer : document.Deformers()) {
            if (deformer.type == CModelDeformer::Warp && edits.warpKeyforms.isEmpty()) {
                auto forms = document.DeformerWarpKeyforms(deformer.id);
                QVERIFY(!forms.isEmpty());
                QVERIFY(!forms[0].positions.isEmpty());
                forms[0].positions[0] += 0.25f;
                forms[0].opacity = 0.625f;
                edits.warpKeyforms.insert(deformer.id, forms);
            }
            if (deformer.type == CModelDeformer::Rotation && edits.rotationKeyforms.isEmpty()) {
                auto forms = document.DeformerRotationKeyforms(deformer.id);
                QVERIFY(!forms.isEmpty());
                forms[0].angle += 0.5f;
                forms[0].reflectY = !forms[0].reflectY;
                edits.rotationKeyforms.insert(deformer.id, forms);
            }
            if (deformer.parentDeformerIndex != -1 && edits.deformerParents.isEmpty())
                edits.deformerParents.insert(deformer.id, -1);
        }
        QVERIFY(!edits.warpKeyforms.isEmpty());
        QVERIFY(!edits.rotationKeyforms.isEmpty());
        QVERIFY(!edits.deformerParents.isEmpty());
        CMocPreviewBuilder builder;
        QVERIFY(builder.Initialize(source));
        QByteArray output;
        QVERIFY2(builder.BuildOverrides(edits, output, &error), qPrintable(error));
        CModelDocument loaded;
        QVERIFY2(loaded.LoadBytes(output, "edited.moc3", &error), qPrintable(error));
        QCOMPARE(loaded.DrawableKeyformPositions(meshId, 0), position);
        QCOMPARE(masksAt(output, 0), (QVector<int>{1, 2}));
        QVERIFY(loaded.DrawableMaskInverted(meshId));
        for (auto it = edits.warpKeyforms.cbegin(); it != edits.warpKeyforms.cend(); ++it)
            QCOMPARE(loaded.DeformerWarpKeyforms(it.key()), it.value());
        for (auto it = edits.rotationKeyforms.cbegin(); it != edits.rotationKeyforms.cend(); ++it)
            QCOMPARE(loaded.DeformerRotationKeyforms(it.key()), it.value());
        for (auto it = edits.deformerParents.cbegin(); it != edits.deformerParents.cend(); ++it)
            QCOMPARE(loaded.DeformerParentIndex(it.key()), it.value());
    }

    void unchangedSourceListsAreNotReinterpreted()
    {
        QByteArray source = fixture();
        putWord(source, word(source, 0x180), 0);
        putWord(source, word(source, 0x180) + 4, 0);
        putWord(source, word(source, 0x180) + 8, 100);
        CMocPreviewBuilder builder;
        QVERIFY(builder.Initialize(source));
        Overrides edits;
        edits.drawableMasks.insert("Mesh0", {0, 0});
        edits.drawableMasks.insert("Mesh1", {100});
        QByteArray output;
        QVERIFY(builder.BuildOverrides(edits, output));
        QCOMPARE(output, source);
        edits.drawableMasks["Mesh0"] = {1, 2};
        QVERIFY(builder.BuildOverrides(edits, output));
        QCOMPARE(masksAt(output, 0), (QVector<int>{1, 2}));
        QCOMPARE(masksAt(output, 1), QVector<int>{100});
    }

    void invalidOverridesPreserveOutput_data()
    {
        QTest::addColumn<QString>("id");
        QTest::addColumn<QVector<int>>("masks");
        QTest::newRow("unknown") << QStringLiteral("missing") << QVector<int>{1};
        QTest::newRow("self") << QStringLiteral("Mesh0") << QVector<int>{0};
        QTest::newRow("duplicate") << QStringLiteral("Mesh0") << QVector<int>{1, 1};
        QTest::newRow("negative") << QStringLiteral("Mesh0") << QVector<int>{-1};
        QTest::newRow("end") << QStringLiteral("Mesh0") << QVector<int>{4};
        QTest::newRow("maximum") << QStringLiteral("Mesh0") << QVector<int>{std::numeric_limits<int>::max()};
    }

    void invalidOverridesPreserveOutput()
    {
        QFETCH(QString, id);
        QFETCH(QVector<int>, masks);
        CMocPreviewBuilder builder;
        QVERIFY(builder.Initialize(fixture()));
        Overrides edits;
        edits.drawableMasks.insert(id, masks);
        edits.drawableMaskInverted.insert("Mesh3", false);
        QByteArray output("sentinel");
        QString error;
        QVERIFY(!builder.BuildOverrides(edits, output, &error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(output, QByteArray("sentinel"));
        edits = {};
        edits.drawableMaskInverted.insert("missing", true);
        QVERIFY(!builder.BuildOverrides(edits, output, &error));
        QCOMPARE(output, QByteArray("sentinel"));
    }

    void badSlicesAndGrowthBudgetPreserveState()
    {
        const QByteArray source = fixture();
        CMocPreviewBuilder builder;
        QVERIFY(builder.Initialize(source));
        for (int begin : {-1, 4, std::numeric_limits<int>::max()}) {
            QByteArray bad = source;
            setColumn(bad, 0xfc, 0, begin);
            QVERIFY(!builder.Initialize(bad));
            QByteArray output;
            QVERIFY(builder.BuildOverrides({}, output));
            QCOMPARE(output, source);
        }
        QByteArray emptyIgnored = source;
        setColumn(emptyIgnored, 0xfc, 2, std::numeric_limits<int>::max());
        QVERIFY(builder.Initialize(emptyIgnored));
        QByteArray output;
        QVERIFY(builder.BuildOverrides({}, output));
        QCOMPARE(output, emptyIgnored);

        QByteArray full = source;
        QVector<int> pool(1000000, 0);
        pool[0] = 1; pool[1] = 2; pool[2] = 2;
        putWord(full, 0x180, appendWords(full, pool));
        setCount(full, 17, pool.size());
        QVERIFY(builder.Initialize(full));
        Overrides edits;
        edits.drawableMasks.insert("Mesh2", {0});
        output = "sentinel";
        QString error;
        QVERIFY(!builder.BuildOverrides(edits, output, &error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(output, QByteArray("sentinel"));
    }
};

QTEST_APPLESS_MAIN(MaskMocPreviewTests)
#include "mask_moc_preview_tests.moc"
