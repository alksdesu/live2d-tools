#include "live2dstruct/cmodeldocument.h"
#include "live2dstruct/cmocpreviewbuilder.h"
#include "live2dstruct/csectiontable.h"
#include "moc_column_oracle.h"

#include <QtTest>
#include <QFile>
#include <QtEndian>
#include <cstring>
#include <limits>

using namespace live2dstruct;

namespace {
using WarpOverrides = QHash<QString, QVector<CModelWarpKeyform>>;
using RotationOverrides = QHash<QString, QVector<CModelRotationKeyform>>;
using ParentOverrides = QHash<QString, int>;

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

void putWord(QByteArray& bytes, int address, quint32 value)
{
    Q_ASSERT(address >= 0 && address <= bytes.size() - 4);
    qToLittleEndian(value, bytes.data() + address);
}

float number(const QByteArray& bytes, int address)
{
    const quint32 bits = quint32(word(bytes, address));
    float value;
    std::memcpy(&value, &bits, 4);
    return value;
}

void putNumber(QByteArray& bytes, int address, float value)
{
    quint32 bits;
    std::memcpy(&bits, &value, 4);
    putWord(bytes, address, bits);
}

int count(const QByteArray& bytes, int index) { return word(bytes, word(bytes, 0x40) + index * 4); }
int column(const QByteArray& bytes, int slot, int index) { return word(bytes, word(bytes, slot) + index * 4); }
float columnNumber(const QByteArray& bytes, int slot, int index) { return number(bytes, word(bytes, slot) + index * 4); }
void setCount(QByteArray& bytes, int index, int value) { putWord(bytes, word(bytes, 0x40) + index * 4, quint32(value)); }
void setColumn(QByteArray& bytes, int slot, int index, int value) { putWord(bytes, word(bytes, slot) + index * 4, quint32(value)); }
void setNumberColumn(QByteArray& bytes, int slot, int index, float value) { putNumber(bytes, word(bytes, slot) + index * 4, value); }

QString idAt(const QByteArray& bytes, int slot, int index)
{
    return QString::fromUtf8(bytes.mid(word(bytes, slot) + index * 64, 64).split('\0').first());
}

void setId(QByteArray& bytes, int slot, int index, const QByteArray& id)
{
    bytes.replace(word(bytes, slot) + index * 64, 64, id.leftJustified(64, '\0'));
}

int appendWords(QByteArray& bytes, std::initializer_list<int> values)
{
    const int offset = int((bytes.size() + 63) & ~qsizetype(63));
    bytes.append(QByteArray(offset + int(values.size()) * 4 - bytes.size(), '\0'));
    int index = 0;
    for (int value : values) putWord(bytes, offset + index++ * 4, quint32(value));
    MocColumnOracle::Pad(bytes);
    return offset;
}

QVector<float> positions(const QByteArray& bytes, int beginSlot, int row, int floats)
{
    QVector<float> result;
    const int address = word(bytes, 0x15c) + column(bytes, beginSlot, row) * 4;
    for (int index = 0; index < floats; ++index) result.append(number(bytes, address + index * 4));
    return result;
}

QVector<CModelWarpKeyform> warpForms(const QByteArray& bytes, int specific)
{
    QVector<CModelWarpKeyform> result;
    const int begin = column(bytes, 0x90, specific), forms = column(bytes, 0x94, specific);
    const int floats = column(bytes, 0x98, specific) * 2;
    for (int local = 0; local < forms; ++local)
        result.append({positions(bytes, 0x130, begin + local, floats), columnNumber(bytes, 0x12c, begin + local)});
    return result;
}

QVector<CModelRotationKeyform> rotationForms(const QByteArray& bytes, int specific)
{
    QVector<CModelRotationKeyform> result;
    const int begin = column(bytes, 0xa8, specific), forms = column(bytes, 0xac, specific);
    for (int local = 0; local < forms; ++local) {
        const int row = begin + local;
        result.append({columnNumber(bytes, 0x134, row), columnNumber(bytes, 0x138, row),
            columnNumber(bytes, 0x13c, row), columnNumber(bytes, 0x140, row), columnNumber(bytes, 0x144, row),
            column(bytes, 0x148, row) != 0, column(bytes, 0x14c, row) != 0});
    }
    return result;
}

bool parses(const QByteArray& bytes, QString* error)
{
    auto reader = CReader::FromBytes(bytes);
    CSectionTable table(reader);
    if (error) *error = reader.Error();
    return table.IsValid();
}

QByteArray fixture()
{
    QByteArray bytes = sourceBytes(QStringLiteral("Mark"));
    if (bytes.isEmpty()) return {};
    for (int index = 0; index < 32; ++index) setCount(bytes, index, 0);
    for (const auto& entry : {std::pair{0, 2}, {1, 4}, {2, 2}, {3, 2}, {4, 1},
                              {7, 2}, {8, 2}, {9, 1}, {10, 32}, {15, 8}, {16, 6}})
        setCount(bytes, entry.first, entry.second);
    for (int part = 0; part < 2; ++part) {
        setId(bytes, 0x4c, part, part ? "PartB" : "PartA");
        setColumn(bytes, 0x50, part, -1);
        setColumn(bytes, 0x54, part, 0);
        setColumn(bytes, 0x58, part, 0);
        setColumn(bytes, 0x5c, part, 1);
        setColumn(bytes, 0x60, part, 1);
        setColumn(bytes, 0x64, part, -1);
    }
    const QList<QByteArray> ids{"WarpA", "RotationA", "WarpB", "RotationB"};
    for (int index = 0; index < 4; ++index) {
        setId(bytes, 0x6c, index, ids[index]);
        setColumn(bytes, 0x70, index, -1);
        setColumn(bytes, 0x74, index, 1);
        setColumn(bytes, 0x78, index, 1);
        setColumn(bytes, 0x7c, index, 0);
        setColumn(bytes, 0x80, index, -1);
        setColumn(bytes, 0x84, index, index % 2);
        setColumn(bytes, 0x88, index, index < 2 ? 1 : 0);
    }
    for (int specific = 0; specific < 2; ++specific) {
        setColumn(bytes, 0x8c, specific, -1);
        setColumn(bytes, 0x90, specific, specific);
        setColumn(bytes, 0x94, specific, 1);
        setColumn(bytes, 0x98, specific, 4);
        setColumn(bytes, 0x9c, specific, 1);
        setColumn(bytes, 0xa0, specific, 1);
        setColumn(bytes, 0x1d4, specific, specific);
        setNumberColumn(bytes, 0x12c, specific, 1.0f);
        setColumn(bytes, 0x130, specific, specific * 8);
        setColumn(bytes, 0xa4, specific, -1);
        setColumn(bytes, 0xa8, specific, specific);
        setColumn(bytes, 0xac, specific, 1);
        setNumberColumn(bytes, 0xb0, specific, specific ? 37.5f : -135.0f);
        setNumberColumn(bytes, 0x134, specific, 1.0f);
        setNumberColumn(bytes, 0x138, specific, specific ? 22.5f : 11.5f);
        setNumberColumn(bytes, 0x13c, specific, specific ? 0.4f : -0.2f);
        setNumberColumn(bytes, 0x140, specific, specific ? -0.5f : 0.3f);
        setNumberColumn(bytes, 0x144, specific, specific ? 2.0f : 1.25f);
        setColumn(bytes, 0x148, specific, 0);
        setColumn(bytes, 0x14c, specific, 0);
    }
    setId(bytes, 0xc4, 0, "Mesh");
    for (const auto& entry : {std::pair{0xc8, -1}, {0xcc, 0}, {0xd0, 1}, {0xd4, 1}, {0xd8, 1},
                              {0xdc, 0}, {0xe0, -1}, {0xe4, 0}, {0xec, 4}, {0xf0, 0},
                              {0xf4, 0}, {0xf8, 6}, {0xfc, 0}, {0x100, 0}, {0x158, 16}})
        setColumn(bytes, entry.first, 0, entry.second);
    bytes[word(bytes, 0xe8)] = 4;
    setNumberColumn(bytes, 0x150, 0, 1.0f);
    setNumberColumn(bytes, 0x154, 0, 0.0f);
    for (int index = 0; index < 32; ++index)
        putNumber(bytes, word(bytes, 0x15c) + index * 4, float(index) / 16.0f - 0.5f);
    for (int index = 0; index < 8; ++index)
        putNumber(bytes, word(bytes, 0x178) + index * 4, float(index % 2));
    const quint16 indices[]{0, 1, 2, 0, 2, 3};
    for (int index = 0; index < 6; ++index)
        qToLittleEndian(indices[index], bytes.data() + word(bytes, 0x17c) + index * 2);
    return bytes;
}

void addWarpBlendShape(QByteArray& bytes, int specific, int row)
{
    setCount(bytes, 26, 1);
    setCount(bytes, 27, 1);
    for (const auto& entry : {std::pair{0x220, -1}, {0x224, row}, {0x228, 1}, {0x22c, 0},
                              {0x230, 0}, {0x234, specific}, {0x238, 0}, {0x23c, 1}})
        putWord(bytes, entry.first, quint32(appendWords(bytes, {entry.second})));
}
}

class DeformerMocPreviewTests : public QObject
{
    Q_OBJECT
private slots:
    void fourModelsPreserveEveryUneditedByte_data()
    {
        QTest::addColumn<QString>("model");
        for (const char* name : {"Mark", "Natori", "Rice", "Wanko"}) QTest::newRow(name) << QString::fromLatin1(name);
    }

    void fourModelsPreserveEveryUneditedByte()
    {
        QFETCH(QString, model);
        const QByteArray source = sourceBytes(model);
        QVERIFY(!source.isEmpty());
        auto reader = CReader::FromBytes(source);
        CSectionTable table(reader);
        QVERIFY2(table.IsValid(), qPrintable(reader.Error()));
        QCOMPARE(word(source, 0x80), table.sectionOffsetTable->deformers->parent_deformer_indices);
        QCOMPARE(word(source, 0x130), table.sectionOffsetTable->warp_deformer_keyforms->keyform_position_sources_begin_indices);
        QCOMPARE(word(source, 0x148), table.sectionOffsetTable->rotation_deformer_keyforms->is_reflect_x);
        WarpOverrides warps, unchangedWarps;
        RotationOverrides rotations, unchangedRotations;
        QHash<QString, int> unchangedParents;
        QByteArray expected = source;
        for (int generic = 0; generic < count(source, 1); ++generic) {
            const QString id = idAt(source, 0x6c, generic);
            const int specific = column(source, 0x88, generic);
            unchangedParents.insert(id, column(source, 0x80, generic));
            if (column(source, 0x84, generic) == 0) {
                auto forms = warpForms(source, specific);
                unchangedWarps.insert(id, forms);
                const int first = column(source, 0x90, specific);
                for (int local = 0; local < forms.size(); ++local) {
                    forms[local].opacity = 0.375f;
                    const int row = first + local;
                    setNumberColumn(expected, 0x12c, row, forms[local].opacity);
                    const int address = word(source, 0x15c) + column(source, 0x130, row) * 4;
                    for (int component = 0; component < forms[local].positions.size(); ++component) {
                        forms[local].positions[component] += component % 2 ? -0.03125f : 0.0625f;
                        putNumber(expected, address + component * 4, forms[local].positions[component]);
                    }
                }
                warps.insert(id, forms);
            } else {
                auto forms = rotationForms(source, specific);
                unchangedRotations.insert(id, forms);
                const int first = column(source, 0xa8, specific);
                for (int local = 0; local < forms.size(); ++local) {
                    auto& form = forms[local];
                    form.opacity = 0.625f;
                    form.angle += 1.25f;
                    form.originX += 0.125f;
                    form.originY -= 0.25f;
                    form.scale += 0.25f;
                    form.reflectX = true;
                    form.reflectY = local % 2 != 0;
                    const int row = first + local;
                    setNumberColumn(expected, 0x134, row, form.opacity);
                    setNumberColumn(expected, 0x138, row, form.angle);
                    setNumberColumn(expected, 0x13c, row, form.originX);
                    setNumberColumn(expected, 0x140, row, form.originY);
                    setNumberColumn(expected, 0x144, row, form.scale);
                    setColumn(expected, 0x148, row, 1);
                    setColumn(expected, 0x14c, row, form.reflectY ? 1 : 0);
                }
                rotations.insert(id, forms);
            }
        }
        CMocPreviewBuilder builder;
        QString error;
        QVERIFY2(builder.Initialize(source, &error), qPrintable(error));
        QByteArray result("sentinel");
        QVERIFY2(builder.Build({}, warps, rotations, {}, result, &error), qPrintable(error));
        QCOMPARE(result, expected);
        QVERIFY2(parses(result, &error), qPrintable(error));
        CModelDocument document;
        QVERIFY2(document.LoadBytes(result, "preview.moc3", &error), qPrintable(error));
        for (auto it = warps.cbegin(); it != warps.cend(); ++it) QCOMPARE(document.DeformerWarpKeyforms(it.key()), it.value());
        for (auto it = rotations.cbegin(); it != rotations.cend(); ++it) QCOMPARE(document.DeformerRotationKeyforms(it.key()), it.value());
        QByteArray repeated;
        QVERIFY(builder.Build({}, warps, rotations, {}, repeated, &error));
        QCOMPARE(repeated, result);
        QVERIFY2(builder.Build({}, unchangedWarps, unchangedRotations, unchangedParents, repeated, &error), qPrintable(error));
        QCOMPARE(repeated, source);
        QVERIFY(builder.Build({}, repeated, &error));
        QCOMPARE(repeated, source);
    }

    void reorderedSpecificIndicesIdentifyTheCorrectRecords()
    {
        const QByteArray source = fixture();
        QString error;
        QVERIFY2(parses(source, &error), qPrintable(error));
        CMocPreviewBuilder builder;
        QVERIFY2(builder.Initialize(source, &error), qPrintable(error));
        auto warp = warpForms(source, 1);
        warp[0].positions[0] = -7.25f;
        warp[0].opacity = 0.25f;
        auto rotation = rotationForms(source, 1);
        rotation[0].angle = 721.5f;
        rotation[0].originX = -3.5f;
        rotation[0].reflectY = true;
        QByteArray expected = source;
        putNumber(expected, word(source, 0x15c) + 8 * 4, -7.25f);
        setNumberColumn(expected, 0x12c, 1, 0.25f);
        setNumberColumn(expected, 0x138, 1, 721.5f);
        setNumberColumn(expected, 0x13c, 1, -3.5f);
        setColumn(expected, 0x14c, 1, 1);
        QByteArray result;
        QVERIFY2(builder.Build({}, {{"WarpA", warp}}, {{"RotationA", rotation}}, {}, result, &error), qPrintable(error));
        QCOMPARE(result, expected);
        QCOMPARE(columnNumber(result, 0xb0, 1), 37.5f);
        QCOMPARE(warpForms(result, 0), warpForms(source, 0));
        QCOMPARE(rotationForms(result, 0), rotationForms(source, 0));
    }

    void untouchedReflectionWordsRetainTheirBits_data()
    {
        QTest::addColumn<quint32>("bits");
        QTest::newRow("two") << quint32(2);
        QTest::newRow("minus-one") << quint32(0xffffffffu);
        QTest::newRow("high-bit") << quint32(0x80000000u);
    }

    void untouchedReflectionWordsRetainTheirBits()
    {
        QFETCH(quint32, bits);
        QByteArray source = fixture();
        putWord(source, word(source, 0x148) + 4, bits);
        putWord(source, word(source, 0x14c) + 4, bits);
        auto forms = rotationForms(source, 1);
        QVERIFY(forms[0].reflectX && forms[0].reflectY);
        CMocPreviewBuilder builder;
        QString error;
        QVERIFY2(builder.Initialize(source, &error), qPrintable(error));
        QByteArray result;
        QVERIFY(builder.Build({}, {}, {{"RotationA", forms}}, {}, result, &error));
        QCOMPARE(result, source);
        forms[0].angle = 12.75f;
        QVERIFY(builder.Build({}, {}, {{"RotationA", forms}}, {}, result, &error));
        QByteArray expected = source;
        setNumberColumn(expected, 0x138, 1, 12.75f);
        QCOMPARE(result, expected);
        QCOMPARE(quint32(column(result, 0x148, 1)), bits);
        QCOMPARE(quint32(column(result, 0x14c, 1)), bits);
        forms[0].reflectX = false;
        QVERIFY(builder.Build({}, {}, {{"RotationA", forms}}, {}, result, &error));
        setColumn(expected, 0x148, 1, 0);
        QCOMPARE(result, expected);
        QCOMPARE(quint32(column(result, 0x14c, 1)), bits);
    }

    void overlappingScalarColumnsAreDetached_data()
    {
        QTest::addColumn<int>("shift");
        QTest::newRow("same-address") << 0;
        QTest::newRow("one-row-shift") << 4;
    }

    void overlappingScalarColumnsAreDetached()
    {
        QFETCH(int, shift);
        QByteArray source = fixture();
        const int pool = appendWords(source, {0, 0, 0});
        putNumber(source, pool, 0.25f);
        putNumber(source, pool + 4, 0.375f);
        putNumber(source, pool + 8, 0.5f);
        putWord(source, 0x134, pool);
        putWord(source, 0x138, pool + shift);
        auto forms = rotationForms(source, 0);
        forms[0].angle = 17.5f;
        CMocPreviewBuilder builder;
        QString error;
        QVERIFY2(builder.Initialize(source, &error), qPrintable(error));
        QByteArray result;
        QVERIFY2(builder.Build({}, {}, {{"RotationB", forms}}, {}, result, &error), qPrintable(error));
        QVERIFY(word(result, 0x138) != word(result, 0x134));
        QCOMPARE(columnNumber(result, 0x138, 0), 17.5f);
        QCOMPARE(columnNumber(result, 0x138, 1), columnNumber(source, 0x138, 1));
        QCOMPARE(result.mid(word(result, 0x134), 8), source.mid(word(source, 0x134), 8));
        QCOMPARE(columnNumber(result, 0x134, 0), 0.25f);
        QCOMPARE(columnNumber(result, 0x134, 1), 0.375f);
        const QString preserved = MocColumnOracle::Preserved(source, result, {0x138});
        QVERIFY2(preserved.isEmpty(), qPrintable(preserved));
        QVERIFY2(parses(result, &error), qPrintable(error));
        QVERIFY(builder.Build({}, {}, {}, {}, result, &error));
        QCOMPARE(result, source);
    }

    void sharedArtWarpCoordinatesAreDetached_data()
    {
        QTest::addColumn<int>("begin");
        QTest::newRow("identical-slice") << 16;
        QTest::newRow("partial-slice") << 18;
    }

    void sharedArtWarpCoordinatesAreDetached()
    {
        QFETCH(int, begin);
        QByteArray source = fixture();
        setColumn(source, 0x130, 1, begin);
        auto forms = warpForms(source, 1);
        forms[0].positions[0] += 0.125f;
        CMocPreviewBuilder builder;
        QString error;
        QVERIFY2(builder.Initialize(source, &error), qPrintable(error));
        QByteArray result;
        QVERIFY2(builder.Build({}, {{"WarpA", forms}}, {}, {}, result, &error), qPrintable(error));
        QCOMPARE(warpForms(result, 1), forms);
        QCOMPARE(warpForms(result, 0), warpForms(source, 0));
        QCOMPARE(positions(result, 0x158, 0, 8), positions(source, 0x158, 0, 8));
        QCOMPARE(result.mid(word(result, 0x15c), count(source, 10) * 4),
                 source.mid(word(source, 0x15c), count(source, 10) * 4));
        QCOMPARE(count(result, 10), count(source, 10) + forms[0].positions.size());
        const QString preserved = MocColumnOracle::Preserved(source, result, {0x130, 0x15c}, {10});
        QVERIFY2(preserved.isEmpty(), qPrintable(preserved));
        QVERIFY2(parses(result, &error), qPrintable(error));
    }

    void sharedOwnersCanSubmitIndependentRecords_data()
    {
        QTest::addColumn<bool>("rotation");
        QTest::addColumn<bool>("sameSpecific");
        for (bool rotation : {false, true})
            for (bool sameSpecific : {false, true})
                QTest::newRow(qPrintable(QStringLiteral("%1-%2").arg(rotation ? "rotation" : "warp")
                                        .arg(sameSpecific ? "specific" : "keyform-row"))) << rotation << sameSpecific;
    }

    void sharedOwnersCanSubmitIndependentRecords()
    {
        QFETCH(bool, rotation);
        QFETCH(bool, sameSpecific);
        QByteArray source = fixture();
        const QString first = rotation ? QStringLiteral("RotationA") : QStringLiteral("WarpA");
        const QString second = rotation ? QStringLiteral("RotationB") : QStringLiteral("WarpB");
        if (sameSpecific) setColumn(source, 0x88, rotation ? 3 : 2, 1);
        else setColumn(source, rotation ? 0xa8 : 0x90, 0, 1);
        CMocPreviewBuilder builder;
        QString error;
        QVERIFY2(builder.Initialize(source, &error), qPrintable(error));
        WarpOverrides warps;
        RotationOverrides rotations;
        if (rotation) { auto forms = rotationForms(source, 1); forms[0].angle += 0.25f; rotations.insert(first, forms); }
        else { auto forms = warpForms(source, 1); forms[0].opacity = 0.25f; warps.insert(first, forms); }
        QByteArray result("sentinel");
        QVERIFY2(builder.Build({}, warps, rotations, {}, result, &error), qPrintable(error));
        const int firstGeneric = rotation ? 1 : 0, secondGeneric = rotation ? 3 : 2;
        if (rotation) {
            QCOMPARE(rotationForms(result, column(result, 0x88, firstGeneric)), rotations.value(first));
            QCOMPARE(rotationForms(result, column(result, 0x88, secondGeneric)), rotationForms(source, 1));
        } else {
            QCOMPARE(warpForms(result, column(result, 0x88, firstGeneric)), warps.value(first));
            QCOMPARE(warpForms(result, column(result, 0x88, secondGeneric)), warpForms(source, 1));
        }
        if (rotation) rotations.insert(second, rotations.value(first));
        else warps.insert(second, warps.value(first));
        QVERIFY2(builder.Build({}, warps, rotations, {}, result, &error), qPrintable(error));
        CModelDocument document;
        QVERIFY2(document.LoadBytes(result, "shared.moc3", &error), qPrintable(error));
        if (rotation) {
            QCOMPARE(document.DeformerRotationKeyforms(first), rotations.value(first));
            QCOMPARE(document.DeformerRotationKeyforms(second), rotations.value(first));
            rotations[second][0].angle += 1.0f;
        } else {
            QCOMPARE(document.DeformerWarpKeyforms(first), warps.value(first));
            QCOMPARE(document.DeformerWarpKeyforms(second), warps.value(first));
            warps[second][0].opacity = 0.75f;
        }
        result = "sentinel";
        QVERIFY2(builder.Build({}, warps, rotations, {}, result, &error), qPrintable(error));
        if (rotation) {
            QCOMPARE(rotationForms(result, column(result, 0x88, firstGeneric)), rotations.value(first));
            QCOMPARE(rotationForms(result, column(result, 0x88, secondGeneric)), rotations.value(second));
        } else {
            QCOMPARE(warpForms(result, column(result, 0x88, firstGeneric)), warps.value(first));
            QCOMPARE(warpForms(result, column(result, 0x88, secondGeneric)), warps.value(second));
        }
    }

    void blendShapeWarpRowsRemainProtected()
    {
        QByteArray source = fixture();
        addWarpBlendShape(source, 1, 1);
        CMocPreviewBuilder builder;
        QString error;
        QVERIFY2(builder.Initialize(source, &error), qPrintable(error));
        auto forms = warpForms(source, 1);
        QByteArray result;
        QVERIFY(builder.Build({}, {{"WarpA", forms}}, {}, {}, result, &error));
        QCOMPARE(result, source);
        forms[0].opacity = 0.5f;
        result = "sentinel";
        QVERIFY2(builder.Build({}, {{"WarpA", forms}}, {}, {}, result, &error), qPrintable(error));
        QCOMPARE(warpForms(result, column(result, 0x88, 0)), forms);
        QCOMPARE(columnNumber(result, 0x12c, 1), columnNumber(source, 0x12c, 1));
        forms = warpForms(source, 1);
        forms[0].positions[0] += 0.5f;
        QVERIFY2(builder.Build({}, {{"WarpA", forms}}, {}, {}, result, &error), qPrintable(error));
        QCOMPARE(warpForms(result, column(result, 0x88, 0)), forms);
        QCOMPARE(positions(result, 0x130, 1, 8), positions(source, 0x130, 1, 8));
        forms = warpForms(source, 0);
        forms[0].opacity = 0.75f;
        QVERIFY2(builder.Build({}, {{"WarpB", forms}}, {}, {}, result, &error), qPrintable(error));
        QCOMPARE(warpForms(result, 1), warpForms(source, 1));
        QCOMPARE(column(result, 0x234, 0), 1);
        QCOMPARE(column(result, 0x224, 0), 1);
    }

    void parentsPreserveSourceCrossPartEdgesAndAllowForwardAcyclicLinks()
    {
        QByteArray source = fixture();
        setColumn(source, 0x7c, 2, 1);
        setColumn(source, 0x80, 0, 2);
        CMocPreviewBuilder builder;
        QString error;
        QVERIFY2(builder.Initialize(source, &error), qPrintable(error));
        QByteArray result;
        QVERIFY2(builder.Build({}, {}, {}, {{"WarpA", 2}}, result, &error), qPrintable(error));
        QCOMPARE(result, source);
        QVERIFY2(builder.Build({}, {}, {}, {{"WarpA", -1}}, result, &error), qPrintable(error));
        QByteArray expected = source;
        setColumn(expected, 0x80, 0, -1);
        QCOMPARE(result, expected);
        QVERIFY(builder.Build({}, {}, {}, {{"WarpA", 2}}, result, &error));
        QCOMPARE(result, source);
        QVERIFY2(builder.Build({}, {}, {}, {{"WarpA", 3}}, result, &error), qPrintable(error));
        setColumn(expected, 0x80, 0, 3);
        QCOMPARE(result, expected);
        QCOMPARE(warpForms(result, 1), warpForms(source, 1));
    }

    void invalidParentGraphsDoNotPublish_data()
    {
        QTest::addColumn<ParentOverrides>("parents");
        QTest::newRow("self") << QHash<QString, int>{{"WarpA", 0}};
        QTest::newRow("below-root") << QHash<QString, int>{{"WarpA", -2}};
        QTest::newRow("past-end") << QHash<QString, int>{{"WarpA", 4}};
        QTest::newRow("unknown") << QHash<QString, int>{{"Absent", -1}};
        QTest::newRow("cycle") << QHash<QString, int>{{"WarpA", 3}, {"RotationB", 0}};
        QTest::newRow("new-cross-part") << QHash<QString, int>{{"WarpA", 2}};
    }

    void invalidParentGraphsDoNotPublish()
    {
        QFETCH(ParentOverrides, parents);
        QByteArray source = fixture();
        setColumn(source, 0x7c, 2, 1);
        CMocPreviewBuilder builder;
        QString error;
        QVERIFY2(builder.Initialize(source, &error), qPrintable(error));
        QByteArray result("sentinel");
        QVERIFY(!builder.Build({}, {}, {}, parents, result, &error));
        QCOMPARE(result, QByteArray("sentinel"));
        QVERIFY(!error.isEmpty());
    }

    void aliasedParentColumnDoesNotOverwriteOtherMetadata()
    {
        QByteArray source = fixture();
        putWord(source, 0x68, word(source, 0x80));
        CMocPreviewBuilder builder;
        QString error;
        QVERIFY2(builder.Initialize(source, &error), qPrintable(error));
        QByteArray result;
        QVERIFY2(builder.Build({}, {}, {}, {{"WarpA", 2}}, result, &error), qPrintable(error));
        QVERIFY(word(result, 0x80) != word(result, 0x68));
        QCOMPARE(column(result, 0x80, 0), 2);
        for (int index = 0; index < 4; ++index) QCOMPARE(column(result, 0x68, index), -1);
        for (int index = 1; index < 4; ++index) QCOMPARE(column(result, 0x80, index), column(source, 0x80, index));
        const QString preserved = MocColumnOracle::Preserved(source, result, {0x80});
        QVERIFY2(preserved.isEmpty(), qPrintable(preserved));
        QVERIFY2(parses(result, &error), qPrintable(error));
    }

    void overrideOrderAndRepeatedCallsAreStable()
    {
        QByteArray source = fixture();
        setColumn(source, 0x130, 0, 16);
        setColumn(source, 0x130, 1, 18);
        putWord(source, 0x138, word(source, 0x134));
        CMocPreviewBuilder builder;
        QString error;
        QVERIFY(builder.Initialize(source, &error));
        WarpOverrides warps;
        RotationOverrides rotations;
        for (const auto& entry : {qMakePair(QStringLiteral("WarpA"), 1), qMakePair(QStringLiteral("WarpB"), 0)}) {
            auto forms = warpForms(source, entry.second);
            forms[0].opacity = 0.625f;
            forms[0].positions[0] += 0.125f;
            warps.insert(entry.first, forms);
        }
        for (const auto& entry : {qMakePair(QStringLiteral("RotationA"), 1), qMakePair(QStringLiteral("RotationB"), 0)}) {
            auto forms = rotationForms(source, entry.second); forms[0].angle += 2.5f; rotations.insert(entry.first, forms);
        }
        const QHash<QString, int> parents{{"WarpA", 3}, {"RotationA", 2}};
        QByteArray first, second;
        QVERIFY(builder.Build({}, warps, rotations, parents, first, &error));
        const QString preserved = MocColumnOracle::Preserved(source, first, {0x80, 0x12c, 0x130, 0x138, 0x15c}, {10});
        QVERIFY2(preserved.isEmpty(), qPrintable(preserved));
        QCOMPARE(count(first, 10), count(source, 10) + 16);
        QCOMPARE(warpForms(first, 0), warps.value("WarpB"));
        QCOMPARE(warpForms(first, 1), warps.value("WarpA"));
        QCOMPARE(rotationForms(first, 0), rotations.value("RotationB"));
        QCOMPARE(rotationForms(first, 1), rotations.value("RotationA"));
        WarpOverrides reversedWarps{{"WarpB", warps.value("WarpB")}, {"WarpA", warps.value("WarpA")}};
        RotationOverrides reversedRotations{{"RotationB", rotations.value("RotationB")}, {"RotationA", rotations.value("RotationA")}};
        const QHash<QString, int> reversedParents{{"RotationA", 2}, {"WarpA", 3}};
        QVERIFY(builder.Build({}, reversedWarps, reversedRotations, reversedParents, second, &error));
        QCOMPARE(second, first);
        QVERIFY(builder.Build({}, warps, rotations, parents, second, &error));
        QCOMPARE(second, first);
        QVERIFY(builder.Build({}, {}, {}, {}, second, &error));
        QCOMPARE(second, source);
    }

    void invalidDeformerOverridesPreserveOutput_data()
    {
        QTest::addColumn<int>("mutation");
        const QStringList names{QStringLiteral("warp-position-count"), QStringLiteral("warp-position-nan"),
            QStringLiteral("warp-opacity-nan"), QStringLiteral("warp-opacity-range"), QStringLiteral("rotation-angle-inf"),
            QStringLiteral("rotation-origin-nan"), QStringLiteral("rotation-negative-scale"),
            QStringLiteral("unknown-warp"), QStringLiteral("wrong-object-type"), QStringLiteral("extra-keyform")};
        for (int index = 0; index < names.size(); ++index) QTest::newRow(qPrintable(names[index])) << index;
    }

    void invalidDeformerOverridesPreserveOutput()
    {
        QFETCH(int, mutation);
        const QByteArray source = fixture();
        CMocPreviewBuilder builder;
        QString error;
        QVERIFY2(builder.Initialize(source, &error), qPrintable(error));
        WarpOverrides warps{{"WarpA", warpForms(source, 1)}};
        RotationOverrides rotations{{"RotationA", rotationForms(source, 1)}};
        const float nan = std::numeric_limits<float>::quiet_NaN();
        switch (mutation) {
        case 0: warps["WarpA"][0].positions.removeLast(); break;
        case 1: warps["WarpA"][0].positions[0] = nan; break;
        case 2: warps["WarpA"][0].opacity = nan; break;
        case 3: warps["WarpA"][0].opacity = 1.5f; break;
        case 4: rotations["RotationA"][0].angle = std::numeric_limits<float>::infinity(); break;
        case 5: rotations["RotationA"][0].originX = nan; break;
        case 6: rotations["RotationA"][0].scale = -1; break;
        case 7: warps.insert("Absent", warps.take("WarpA")); break;
        case 8: warps.insert("RotationA", warps.take("WarpA")); break;
        case 9: rotations["RotationA"].append(rotations["RotationA"].first()); break;
        }
        QByteArray result("sentinel");
        QVERIFY(!builder.Build({}, warps, rotations, {}, result, &error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(result, QByteArray("sentinel"));
    }
};

QTEST_GUILESS_MAIN(DeformerMocPreviewTests)
#include "deformer_moc_preview_tests.moc"
