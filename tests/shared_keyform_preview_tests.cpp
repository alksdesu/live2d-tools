#include "live2dstruct/ckeyformbindinggrid.h"
#include "live2dstruct/cmocpreviewbuilder.h"
#include "live2dstruct/csectiontable.h"

#include <QtTest>
#include <QFile>
#include <QtEndian>
#include <array>
#include <cstring>

using namespace live2dstruct;

namespace {
enum Kind { Art, Warp, Rotation };
using Overrides = CMocPreviewBuilder::Overrides;
constexpr int Frames = 6;

int word(const QByteArray& bytes, int offset) { return qFromLittleEndian<qint32>(bytes.constData() + offset); }
void put(QByteArray& bytes, int offset, int value) { qToLittleEndian(qint32(value), bytes.data() + offset); }
int count(const QByteArray& bytes, int index) { return word(bytes, word(bytes, 0x40) + index * 4); }
void setCount(QByteArray& bytes, int index, int value) { put(bytes, word(bytes, 0x40) + index * 4, value); }
int column(const QByteArray& bytes, int slot, int row) { return word(bytes, word(bytes, slot) + row * 4); }
void setCell(QByteArray& bytes, int slot, int row, int value) { put(bytes, word(bytes, slot) + row * 4, value); }
int bits(float value) { int result; std::memcpy(&result, &value, 4); return result; }
float real(int value) { float result; std::memcpy(&result, &value, 4); return result; }
QByteArray cell(const QByteArray& bytes, int slot, int row) { return bytes.mid(word(bytes, slot) + row * 4, 4); }
QByteArray scalar(float value) { QByteArray result(4, '\0'); put(result, 0, bits(value)); return result; }

int append(QByteArray& bytes, const QVector<int>& values)
{
    const int offset = (bytes.size() + 63) & ~63;
    bytes.append(QByteArray(offset + values.size() * 4 - bytes.size(), '\0'));
    for (int index = 0; index < values.size(); ++index) put(bytes, offset + index * 4, values[index]);
    return offset;
}
void array(QByteArray& bytes, int slot, const QVector<int>& values) { put(bytes, slot, append(bytes, values)); }
void id(QByteArray& bytes, int slot, int row, const QString& value)
{
    bytes.replace(word(bytes, slot) + row * 64, 64, value.toUtf8().leftJustified(64, '\0'));
}
QString name(Kind kind, int owner) { return QStringLiteral("%1%2").arg(kind == Art ? "Mesh" : kind == Warp ? "Warp" : "Rotation").arg(owner); }
int generic(Kind kind, int owner) { return owner * 2 + (kind == Rotation ? 1 : 0); }
int specific(const QByteArray& bytes, Kind kind, int owner) { return kind == Art ? owner : column(bytes, 0x88, generic(kind, owner)); }
int begin(const QByteArray& bytes, Kind kind, int owner)
{
    return column(bytes, kind == Art ? 0xcc : kind == Warp ? 0x90 : 0xa8, specific(bytes, kind, owner));
}
QVector<int> rowSlots(Kind kind)
{
    return kind == Art ? QVector<int>{0x150, 0x154, 0x158} : kind == Warp ? QVector<int>{0x12c, 0x130} :
        QVector<int>{0x134, 0x138, 0x13c, 0x140, 0x144, 0x148, 0x14c};
}

QByteArray fixture(bool colors = true)
{
    QFile file(QString(TEST_RESOURCES) + "/Mark/Mark.moc3");
    if (!file.open(QIODevice::ReadOnly)) return {};
    QByteArray bytes = file.readAll();
    for (int index = 0; index < 32; ++index) setCount(bytes, index, 0);
    for (const auto& entry : {std::pair{1, 6}, {2, 3}, {3, 3}, {4, 3}, {5, 2}, {7, 18}, {8, 18},
                            {9, 18}, {10, 252}, {11, 4}, {12, 2}, {13, 2}, {14, 5}, {15, 18}, {16, 9}})
        setCount(bytes, entry.first, entry.second);
    for (int owner = 0; owner < 3; ++owner) {
        id(bytes, 0xc4, owner, name(Art, owner));
        for (const auto& field : {std::pair{0xc8, owner % 2}, {0xcc, owner * Frames}, {0xd0, Frames},
             {0xd4, 1}, {0xd8, 1}, {0xdc, -1}, {0xe0, -1}, {0xe4, 0}, {0xec, 3}, {0xf0, owner * 6},
             {0xf4, owner * 3}, {0xf8, 3}, {0xfc, 0}, {0x100, 0}}) setCell(bytes, field.first, owner, field.second);
        bytes[word(bytes, 0xe8) + owner] = 4;
        for (Kind kind : {Warp, Rotation}) {
            const int row = generic(kind, owner);
            id(bytes, 0x6c, row, name(kind, owner));
            for (const auto& field : {std::pair{0x70, owner % 2}, {0x74, 1}, {0x78, 1}, {0x7c, -1},
                                     {0x80, -1}, {0x84, kind == Warp ? 0 : 1}, {0x88, owner}})
                setCell(bytes, field.first, row, field.second);
        }
    }
    for (const auto& entry : {std::pair{0x8c, QVector<int>{1, 0, 1}}, {0x90, QVector<int>{0, 6, 12}},
        {0x94, QVector<int>{6, 6, 6}}, {0x98, QVector<int>{4, 4, 4}}, {0x9c, QVector<int>{1, 1, 1}},
        {0xa0, QVector<int>{1, 1, 1}}, {0x1d4, QVector<int>{2, -1, 0}},
        {0xa4, QVector<int>{1, 0, 1}}, {0xa8, QVector<int>{0, 6, 12}}, {0xac, QVector<int>{6, 6, 6}},
        {0xb0, QVector<int>{bits(17.25f), bits(-43.5f), bits(91.0f)}}}) array(bytes, entry.first, entry.second);
    for (Kind kind : {Art, Warp, Rotation}) {
        const auto fields = rowSlots(kind);
        for (int field = 0; field < fields.size(); ++field) {
            QVector<int> values;
            for (int row = 0; row < 18; ++row) {
                int value = bits(0.6f + row * 0.01f);
                if (kind != Rotation && field == fields.size() - 1) value = (kind == Art ? 0 : 108) + row * (kind == Art ? 6 : 8);
                else if (kind == Art && field == 1) value = bits(23.0f + row);
                else if (kind == Rotation && field > 0 && field < 5) value = bits(float(field * 5) + row * 0.125f);
                else if (kind == Rotation && field >= 5) value = row % 3 == 0 ? -1 : row % 3 == 1 ? int(0x80000000u) : 0;
                values.append(value);
            }
            array(bytes, fields[field], values);
        }
    }
    QVector<int> positions;
    for (int value = 0; value < 252; ++value) positions.append(bits(float(value) / 32));
    array(bytes, 0x15c, positions);
    QVector<int> uvs;
    for (int value = 0; value < 18; ++value) uvs.append(bits(float(value % 2)));
    array(bytes, 0x178, uvs);
    for (int value = 0; value < 9; ++value) qToLittleEndian(quint16(value % 3), bytes.data() + word(bytes, 0x17c) + value * 2);
    for (int index = 0; index < 2; ++index) {
        id(bytes, 0x108, index, QStringLiteral("P%1").arg(index));
        for (const auto& field : {std::pair{0x10c, bits(index ? 2.0f : 3.0f)},
             {0x110, bits(index ? -1.0f : -2.0f)}, {0x114, bits(index ? 0.25f : 3.0f)},
             {0x118, 0}, {0x11c, 3}, {0x120, index}, {0x124, 1}})
            setCell(bytes, field.first, index, field.second);
    }
    array(bytes, 0x160, {0, 1, 1, 0});
    array(bytes, 0x164, {0, 2});
    array(bytes, 0x168, {2, 2});
    array(bytes, 0x16c, {0, 2});
    array(bytes, 0x170, {2, 3});
    array(bytes, 0x174, {bits(-2.0f), bits(3.0f), bits(-1.0f), bits(0.25f), bits(2.0f)});
    for (int slot : {0x1e4, 0x1e8, 0x1ec}) {
        if (colors) array(bytes, slot, {0, 6, 12});
        else put(bytes, slot, 0);
    }
    if (!colors) put(bytes, 0x1d4, 0);
    if (colors) {
        setCount(bytes, 23, 18);
        setCount(bytes, 24, 18);
        for (int slot : {0x1f0, 0x1f4, 0x1f8, 0x1fc, 0x200, 0x204}) {
            QVector<int> values;
            for (int row = 0; row < 18; ++row) values.append(bits(float(slot - 0x1f0 + row) / 100));
            array(bytes, slot, values);
        }
    }
    return bytes;
}

struct Object
{
    QVector<QByteArray> metadata;
    QVector<QVector<QByteArray>> forms;
};

Object capture(const QByteArray& bytes, Kind kind, int owner)
{
    Object result;
    const int source = specific(bytes, kind, owner);
    if (kind != Art) {
        const int row = generic(kind, owner);
        result.metadata.append(bytes.mid(word(bytes, 0x6c) + row * 64, 64));
        for (int slot : {0x68, 0x70, 0x74, 0x78, 0x7c, 0x80, 0x84}) result.metadata.append(cell(bytes, slot, row));
    } else result.metadata.append(bytes.mid(word(bytes, 0xc4) + owner * 64, 64));
    const QVector<int> fields = kind == Art ? QVector<int>{0xb4, 0xb8, 0xbc, 0xc0, 0xc8, 0xd0, 0xd4, 0xd8, 0xdc, 0xe0, 0xe4,
        0xec, 0xf0, 0xf4, 0xf8, 0xfc, 0x100} : kind == Warp ? QVector<int>{0x8c, 0x94, 0x98, 0x9c, 0xa0} :
        QVector<int>{0xa4, 0xac, 0xb0};
    for (int slot : fields) result.metadata.append(cell(bytes, slot, source));
    if (kind == Art) result.metadata.append(bytes.mid(word(bytes, 0xe8) + owner, 1));
    const int colorSlot = kind == Art ? 0x1ec : kind == Warp ? 0x1e4 : 0x1e8;
    result.metadata.append(word(bytes, colorSlot) ? cell(bytes, colorSlot, source) : QByteArray("absent"));
    if (kind == Warp) result.metadata.append(word(bytes, 0x1d4) ? cell(bytes, 0x1d4, source) : QByteArray("absent"));
    const int first = begin(bytes, kind, owner);
    for (int local = 0; local < Frames; ++local) {
        QVector<QByteArray> form;
        for (int slot : rowSlots(kind)) {
            if ((kind == Art && slot == 0x158) || (kind == Warp && slot == 0x130))
                form.append(bytes.mid(word(bytes, 0x15c) + column(bytes, slot, first + local) * 4, kind == Art ? 24 : 32));
            else form.append(cell(bytes, slot, first + local));
        }
        result.forms.append(form);
    }
    return result;
}

Object change(const QByteArray& bytes, Kind kind, int owner, int local, float delta, Overrides& edits)
{
    Object expected = capture(bytes, kind, owner);
    if (kind == Rotation) {
        auto& form = expected.forms[local];
        form[1] = scalar(real(word(form[1], 0)) + delta);
        form[2] = scalar(real(word(form[2], 0)) - delta);
        QVector<CModelRotationKeyform> forms;
        for (const auto& row : expected.forms)
            forms.append({real(word(row[0], 0)), real(word(row[1], 0)), real(word(row[2], 0)), real(word(row[3], 0)),
                          real(word(row[4], 0)), word(row[5], 0) != 0, word(row[6], 0) != 0});
        edits.rotationKeyforms.insert(name(kind, owner), forms);
    } else {
        auto& position = expected.forms[local].last();
        put(position, 0, bits(real(word(position, 0)) + delta));
        const auto decode = [](const QByteArray& value) {
            QVector<float> positions;
            for (int index = 0; index < value.size(); index += 4) positions.append(real(word(value, index)));
            return positions;
        };
        if (kind == Art) {
            QVector<QVector<float>> forms(local + 1);
            forms[local] = decode(position);
            edits.artMeshPositions.insert(name(kind, owner), forms);
        } else {
            expected.forms[local][0] = scalar(0.375f);
            QVector<CModelWarpKeyform> forms;
            for (const auto& row : expected.forms) forms.append({decode(row[1]), real(word(row[0], 0))});
            edits.warpKeyforms.insert(name(kind, owner), forms);
        }
    }
    return expected;
}

void share(QByteArray& bytes, Kind kind, bool sameSpecific)
{
    if (kind != Art && sameSpecific) setCell(bytes, 0x88, generic(kind, 1), 0);
    else setCell(bytes, kind == Art ? 0xcc : kind == Warp ? 0x90 : 0xa8, 1, 4);
}

void addBlend(QByteArray& bytes, Kind kind)
{
    setCount(bytes, 26, 2);
    setCount(bytes, kind == Art ? 28 : 27, 1);
    array(bytes, 0x220, {-1, -1});
    array(bytes, 0x224, {1, 4});
    array(bytes, 0x228, {1, 1});
    array(bytes, 0x22c, {0, 0});
    array(bytes, 0x230, {0, 0});
    array(bytes, kind == Art ? 0x240 : 0x234, {0});
    array(bytes, kind == Art ? 0x244 : 0x238, {0});
    array(bytes, kind == Art ? 0x248 : 0x23c, {2});
}
}

class SharedKeyformPreviewTests : public QObject
{
    Q_OBJECT

    static void matches(const Object& actual, const Object& expected)
    {
        QCOMPARE(actual.metadata, expected.metadata);
        QCOMPARE(actual.forms, expected.forms);
    }

    static void unchangedPools(const QByteArray& actual, const QByteArray& source)
    {
        for (const auto& field : {std::pair{0x104, 2}, {0x10c, 2}, {0x110, 2}, {0x114, 2}, {0x118, 2}, {0x11c, 2},
             {0x120, 2}, {0x124, 2}, {0x160, 4}, {0x164, 2}, {0x168, 2}, {0x16c, 2}, {0x170, 2}, {0x174, 5},
             {0x178, 18}, {0x1f0, 18}, {0x1f4, 18}, {0x1f8, 18}, {0x1fc, 18}, {0x200, 18}, {0x204, 18}}) {
            if (field.first >= 0x1f0 && count(source, 23) == 0) continue;
            QCOMPARE(actual.mid(word(actual, field.first), field.second * 4), source.mid(word(source, field.first), field.second * 4));
        }
        QCOMPARE(actual.mid(word(actual, 0x108), 128), source.mid(word(source, 0x108), 128));
        QCOMPARE(actual.mid(word(actual, 0x17c), 18), source.mid(word(source, 0x17c), 18));
        QCOMPARE(count(actual, 23), count(source, 23));
        QCOMPARE(count(actual, 24), count(source, 24));
    }

    static void parsesAndKeepsPoses(const QByteArray& bytes)
    {
        auto reader = CReader::FromBytes(bytes);
        CSectionTable table(reader);
        QVERIFY2(table.IsValid(), qPrintable(reader.Error()));
        CKeyformBindingGrid grid;
        QString error;
        QVERIFY2(grid.Build(table, &error), qPrintable(error));
        const std::array<float, 2> x{-2, 3};
        const std::array<float, 3> y{-1, 0.25f, 2};
        for (Kind kind : {Art, Warp, Rotation})
            for (int owner = 0; owner < 3; ++owner)
                for (int local = 0; local < Frames; ++local) {
                    QMap<QString, float> pose;
                    QVERIFY2(grid.Pose(static_cast<CKeyformBindingGrid::ObjectType>(kind), name(kind, owner), local, pose, &error), qPrintable(error));
                    QCOMPARE(pose.value("P0"), x[owner % 2 ? local / 3 : local % 2]);
                    QCOMPARE(pose.value("P1"), y[owner % 2 ? local % 3 : local / 2]);
                }
    }

private slots:
    void independentOwnersRetainEveryOtherField_data()
    {
        QTest::addColumn<int>("type");
        QTest::addColumn<bool>("sameSpecific");
        QTest::addColumn<bool>("colors");
        for (Kind kind : {Art, Warp, Rotation})
            for (bool sameSpecific : {false, true})
                for (bool colors : {false, true}) {
                    if (kind == Art && sameSpecific) continue;
                    QTest::newRow(qPrintable(QString("type%1-specific%2-colors%3").arg(kind).arg(sameSpecific).arg(colors)))
                        << int(kind) << sameSpecific << colors;
                }
    }

    void independentOwnersRetainEveryOtherField()
    {
        QFETCH(int, type);
        QFETCH(bool, sameSpecific);
        QFETCH(bool, colors);
        const auto kind = static_cast<Kind>(type);
        QByteArray source = fixture(colors);
        QVERIFY(!source.isEmpty());
        share(source, kind, sameSpecific);
        parsesAndKeepsPoses(source);
        CMocPreviewBuilder builder;
        QString error;
        QVERIFY2(builder.Initialize(source, &error), qPrintable(error));
        Overrides unchanged;
        change(source, kind, 0, 4, 0.0f, unchanged);
        if (kind == Warp) unchanged.warpKeyforms[name(kind, 0)][4].opacity = real(word(capture(source, kind, 0).forms[4][0], 0));
        QByteArray noOp;
        QVERIFY2(builder.BuildOverrides(unchanged, noOp, &error), qPrintable(error));
        QCOMPARE(noOp, source);
        Overrides edits;
        const auto expectedA = change(source, kind, 0, 4, 0.125f, edits);
        QByteArray result;
        QVERIFY2(builder.BuildOverrides(edits, result, &error), qPrintable(error));
        for (Kind current : {Art, Warp, Rotation})
            for (int owner = 0; owner < 3; ++owner)
                matches(capture(result, current, owner), current == kind && owner == 0 ? expectedA : capture(source, current, owner));
        unchangedPools(result, source);
        parsesAndKeepsPoses(result);
        const auto expectedB = change(source, kind, 1, sameSpecific ? 4 : 0, -0.375f, edits);
        QByteArray both;
        QVERIFY2(builder.BuildOverrides(edits, both, &error), qPrintable(error));
        matches(capture(both, kind, 0), expectedA);
        matches(capture(both, kind, 1), expectedB);
        matches(capture(both, kind, 2), capture(source, kind, 2));
        unchangedPools(both, source);
        parsesAndKeepsPoses(both);
        Overrides reverse;
        change(source, kind, 1, sameSpecific ? 4 : 0, -0.375f, reverse);
        change(source, kind, 0, 4, 0.125f, reverse);
        QByteArray repeated;
        QVERIFY(builder.BuildOverrides(reverse, repeated, &error));
        QCOMPARE(repeated, both);
        QVERIFY(builder.BuildOverrides(edits, repeated, &error));
        QCOMPARE(repeated, both);
        QVERIFY(builder.BuildOverrides({}, repeated, &error));
        QCOMPARE(repeated, source);
    }

    void blendShapeReferencesKeepTheirOriginalRows_data()
    {
        QTest::addColumn<int>("type");
        QTest::newRow("artmesh") << int(Art);
        QTest::newRow("warp-specific") << int(Warp);
    }

    void commonRowAndPrivateConflictAreResolvedInOneBuild_data()
    {
        QTest::addColumn<int>("type");
        QTest::newRow("artmesh") << int(Art);
        QTest::newRow("warp") << int(Warp);
        QTest::newRow("rotation") << int(Rotation);
    }

    void commonRowAndPrivateConflictAreResolvedInOneBuild()
    {
        QFETCH(int, type);
        const auto kind = static_cast<Kind>(type);
        QByteArray source = fixture();
        share(source, kind, false);
        if (kind == Art) setCell(source, 0xcc, 2, 0);
        else setCell(source, 0x88, generic(kind, 2), 0);
        Overrides edits;
        auto expectedA = change(source, kind, 0, 4, 0.5f, edits);
        const auto expectedB = change(source, kind, 1, 0, 0.5f, edits);
        const auto expectedC = change(source, kind, 2, 4, 0.5f, edits);
        if (kind == Art) {
            auto& positions = expectedA.forms[1].last();
            put(positions, 0, bits(real(word(positions, 0)) + 0.125f));
            QVector<float> values;
            for (int index = 0; index < positions.size(); index += 4) values.append(real(word(positions, index)));
            edits.artMeshPositions[name(kind, 0)][1] = values;
        } else if (kind == Warp) {
            expectedA.forms[1][0] = scalar(0.25f);
            edits.warpKeyforms[name(kind, 0)][1].opacity = 0.25f;
        } else {
            const float angle = real(word(expectedA.forms[1][1], 0)) + 0.25f;
            expectedA.forms[1][1] = scalar(angle);
            edits.rotationKeyforms[name(kind, 0)][1].angle = angle;
        }
        CMocPreviewBuilder builder;
        QString error;
        QVERIFY2(builder.Initialize(source, &error), qPrintable(error));
        QByteArray result;
        QVERIFY2(builder.BuildOverrides(edits, result, &error), qPrintable(error));
        for (Kind current : {Art, Warp, Rotation}) {
            for (int owner = 0; owner < 3; ++owner) {
                if (current != kind) matches(capture(result, current, owner), capture(source, current, owner));
                else matches(capture(result, current, owner), owner == 0 ? expectedA : owner == 1 ? expectedB : expectedC);
            }
        }
        QCOMPARE(capture(result, kind, 2).forms[1], capture(source, kind, 2).forms[1]);
        QCOMPARE(capture(result, kind, 0).forms[4], capture(result, kind, 1).forms[0]);
        QCOMPARE(capture(result, kind, 1).forms[0], capture(result, kind, 2).forms[4]);
        unchangedPools(result, source);
        parsesAndKeepsPoses(result);
    }

    void blendShapeReferencesKeepTheirOriginalRows()
    {
        QFETCH(int, type);
        const auto kind = static_cast<Kind>(type);
        QByteArray source = fixture();
        if (kind == Warp) share(source, kind, true);
        addBlend(source, kind);
        CMocPreviewBuilder builder;
        QString error;
        QVERIFY2(builder.Initialize(source, &error), qPrintable(error));
        Overrides edits;
        const auto expected = change(source, kind, 0, 4, 0.5f, edits);
        QByteArray result;
        QVERIFY2(builder.BuildOverrides(edits, result, &error), qPrintable(error));
        matches(capture(result, kind, 0), expected);
        matches(capture(result, kind, 1), capture(source, kind, 1));
        for (int slot : rowSlots(kind)) for (int row : {1, 4}) QCOMPARE(cell(result, slot, row), cell(source, slot, row));
        for (int slot : {0x220, 0x224, 0x228, 0x22c, 0x230})
            QCOMPARE(result.mid(word(result, slot), 8), source.mid(word(source, slot), 8));
        if (kind == Warp) {
            QCOMPARE(count(result, 27), 2);
            QCOMPARE(column(result, 0x234, 0), 0);
            QCOMPARE(column(result, 0x234, 1), specific(result, Warp, 0));
            QVERIFY(specific(result, Warp, 0) != specific(result, Warp, 1));
            QCOMPARE(column(result, 0x238, 1), 0);
            QCOMPARE(column(result, 0x23c, 1), 2);
        } else {
            QCOMPARE(count(result, 28), 1);
            QCOMPARE(column(result, 0x240, 0), 0);
        }
        unchangedPools(result, source);
        auto reader = CReader::FromBytes(result);
        CSectionTable table(reader);
        QVERIFY2(table.IsValid(), qPrintable(reader.Error()));
        CKeyformBindingGrid grid;
        QVERIFY(grid.Build(table, &error));
        QMap<QString, float> pose{{"untouched", 17}};
        QVERIFY(!grid.Pose(static_cast<CKeyformBindingGrid::ObjectType>(kind), name(kind, 0), 4, pose, &error));
        QCOMPARE(pose, (QMap<QString, float>{{"untouched", 17}}));
        QVERIFY(error.contains("BlendShape"));
    }

    void physicalAliasesAndOrphanSourcesRemainUntouched_data()
    {
        QTest::addColumn<int>("kind");
        QTest::newRow("opacity-color") << 0;
        QTest::newRow("source-binding-column") << 1;
        QTest::newRow("orphan-specific") << 2;
        QTest::newRow("position-uv-pool") << 3;
    }

    void physicalAliasesAndOrphanSourcesRemainUntouched()
    {
        QFETCH(int, kind);
        QByteArray source = fixture();
        share(source, Warp, true);
        int watchedSlot = 0x1f4;
        if (kind == 0) put(source, 0x12c, word(source, watchedSlot));
        if (kind == 1) { watchedSlot = 0x160; put(source, 0x8c, word(source, watchedSlot)); }
        if (kind == 2) setCell(source, 0x90, 1, 0);
        if (kind == 3) { watchedSlot = 0x178; put(source, watchedSlot, word(source, 0x15c) + (108 + 4 * 8) * 4); }
        CMocPreviewBuilder builder;
        QString error;
        QVERIFY2(builder.Initialize(source, &error), qPrintable(error));
        Overrides edits;
        const auto expected = change(source, Warp, 0, 4, 0.25f, edits);
        QByteArray result;
        QVERIFY2(builder.BuildOverrides(edits, result, &error), qPrintable(error));
        matches(capture(result, Warp, 0), expected);
        matches(capture(result, Warp, 1), capture(source, Warp, 1));
        matches(capture(result, Warp, 2), capture(source, Warp, 2));
        unchangedPools(result, source);
        if (kind == 2) {
            QCOMPARE(column(result, 0x90, 1), 0);
            for (int slot : {0x12c, 0x130})
                QCOMPARE(result.mid(word(result, slot), Frames * 4), source.mid(word(source, slot), Frames * 4));
        }
    }

    void independentCopyCannotExceedThePositionBudget()
    {
        QByteArray source = fixture();
        share(source, Art, false);
        const auto original = source.mid(word(source, 0x15c), 252 * 4);
        QVector<int> pool(1000000, 0);
        for (int index = 0; index < 252; ++index) pool[index] = word(original, index * 4);
        array(source, 0x15c, pool);
        setCount(source, 10, 1000000);
        CMocPreviewBuilder builder;
        QString error;
        QVERIFY2(builder.Initialize(source, &error), qPrintable(error));
        Overrides edits;
        change(source, Art, 0, 4, 0.25f, edits);
        QByteArray result("keep-output");
        QVERIFY(!builder.BuildOverrides(edits, result, &error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(result, QByteArray("keep-output"));
        QVERIFY(builder.BuildOverrides({}, result, &error));
        QCOMPARE(result, source);
    }
};

QTEST_APPLESS_MAIN(SharedKeyformPreviewTests)
#include "shared_keyform_preview_tests.moc"
