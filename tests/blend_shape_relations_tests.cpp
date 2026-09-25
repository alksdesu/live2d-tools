#include "live2dstruct/cblendshaperelations.h"
#include "live2dstruct/csectiontable.h"

#include <QtTest>
#include <QFile>
#include <QtEndian>
#include <cstring>
#include <limits>

using namespace live2dstruct;
using Relations = CBlendShapeRelations;

namespace {
int word(const QByteArray& bytes, int offset) { return qFromLittleEndian<qint32>(bytes.constData() + offset); }
void put(QByteArray& bytes, int offset, int value) { qToLittleEndian(qint32(value), bytes.data() + offset); }
void setCount(QByteArray& bytes, int index, int value) { put(bytes, word(bytes, 0x40) + index * 4, value); }
int bits(float value) { int result; std::memcpy(&result, &value, 4); return result; }
void setCell(QByteArray& bytes, int slot, int row, int value) { put(bytes, word(bytes, slot) + row * 4, value); }
void array(QByteArray& bytes, int slot, const QVector<int>& values)
{
    const int offset = (bytes.size() + 63) & ~63;
    bytes.append(QByteArray(offset + values.size() * 4 - bytes.size(), '\0'));
    for (int index = 0; index < values.size(); ++index) put(bytes, offset + index * 4, values[index]);
    put(bytes, slot, offset);
}
void id(QByteArray& bytes, int slot, int row, const QByteArray& value)
{
    bytes.replace(word(bytes, slot) + row * 64, 64, value.leftJustified(64, '\0'));
}
QByteArray modelBytes(const QString& model)
{
    QFile file(QString(TEST_RESOURCES) + '/' + model + '/' + model + ".moc3");
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

QByteArray fixture()
{
    QByteArray bytes = modelBytes("Mark");
    if (bytes.isEmpty()) return {};
    for (int index = 0; index < 32; ++index) setCount(bytes, index, 0);
    for (const auto& count : {std::pair{1, 4}, {2, 2}, {3, 1}, {4, 2}, {5, 4}, {7, 3}, {9, 3},
         {13, 1}, {14, 9}, {25, 2}, {26, 4}, {27, 2}, {28, 3}, {29, 3}, {30, 3}, {31, 5}})
        setCount(bytes, count.first, count.second);
    const QList<QByteArray> parameterIds{"DriverA", "Limiter", "DriverB", "Unbound"};
    for (int index = 0; index < parameterIds.size(); ++index) {
        id(bytes, 0x108, index, parameterIds[index]);
        setCell(bytes, 0x10c, index, bits(1));
        setCell(bytes, 0x110, index, bits(-1));
        setCell(bytes, 0x114, index, bits(0));
        setCell(bytes, 0x120, index, 0);
        setCell(bytes, 0x124, index, index == 1 ? 1 : 0);
    }
    array(bytes, 0x208, {1, 0, 1, 1});
    array(bytes, 0x20c, {1, 0, 0, -1});
    array(bytes, 0x210, {1, 0, 1, 0});
    array(bytes, 0x16c, {3});
    array(bytes, 0x170, {2});
    array(bytes, 0x174, {bits(17), bits(18), bits(19), bits(0), bits(1), bits(20), bits(-1), bits(0), bits(1)});
    array(bytes, 0x214, {3, 6});
    array(bytes, 0x218, {2, 3});
    array(bytes, 0x21c, {0, 1});
    array(bytes, 0x220, {0, 1, 1, 1});
    array(bytes, 0x224, {1, 0, 2, 0});
    array(bytes, 0x228, {1, 2, 1, 1});
    array(bytes, 0x22c, {1, 0, 3, 2});
    array(bytes, 0x230, {2, 1, 0, 1});
    array(bytes, 0x234, {1, 1});
    array(bytes, 0x238, {0, 1});
    array(bytes, 0x23c, {3, 1});
    array(bytes, 0x240, {0, 1, 0});
    array(bytes, 0x244, {1, 0, 3});
    array(bytes, 0x248, {2, 1, 1});
    array(bytes, 0x24c, {2, 0, 1});
    array(bytes, 0x250, {1, 2, 1});
    array(bytes, 0x254, {2, 0, 4});
    array(bytes, 0x258, {2, 2, 1});
    array(bytes, 0x25c, {bits(-1), bits(1), bits(0), bits(1), bits(0.5f)});
    array(bytes, 0x260, {bits(0.2f), bits(0.8f), bits(1), bits(0.25f), bits(0.5f)});
    const QList<QByteArray> deformerIds{"Shared", "RotationOccupiesIndexOne", "UnrelatedWarp", "OtherOwner"};
    for (int index = 0; index < deformerIds.size(); ++index) {
        id(bytes, 0x6c, index, deformerIds[index]);
        setCell(bytes, 0x84, index, index == 1 ? 1 : 0);
        setCell(bytes, 0x88, index, index == 0 || index == 3 ? 1 : 0);
    }
    id(bytes, 0xc4, 0, "Shared");
    id(bytes, 0xc4, 1, "OtherMesh");
    return bytes;
}

bool build(const QByteArray& bytes, Relations& relations, QString* error = nullptr)
{
    auto reader = CReader::FromBytes(bytes);
    CSectionTable table(reader);
    if (!table.IsValid()) { if (error) *error = reader.Error(); return false; }
    return relations.Build(table, error);
}

QStringList targets(const QVector<Relations::Target>& values)
{
    QStringList result;
    for (const auto& target : values) result.append(QString::number(int(target.type)) + ':' + target.id);
    return result;
}

bool diagnosed(const Relations& relations, const QString& code)
{
    for (const auto& diagnostic : relations.Diagnostics()) if (diagnostic.code == code) return true;
    return false;
}
}

class BlendShapeRelationsTests : public QObject
{
    Q_OBJECT
private slots:
    void suppliedModelsHaveNoNativeBlendShapeData_data()
    {
        QTest::addColumn<QString>("model");
        for (const char* model : {"Mark", "Natori", "Rice", "Wanko"}) QTest::newRow(model) << QString::fromLatin1(model);
    }

    void suppliedModelsHaveNoNativeBlendShapeData()
    {
        QFETCH(QString, model);
        const auto bytes = modelBytes(model);
        QVERIFY(!bytes.isEmpty());
        for (int index = 25; index < 32; ++index) QCOMPARE(word(bytes, word(bytes, 0x40) + index * 4), 0);
        QCOMPARE(word(bytes, 0x208), 0);
        Relations relations;
        QString error;
        QVERIFY2(build(bytes, relations, &error), qPrintable(error));
        QVERIFY(relations.BlendShapeParameterIndices().isEmpty());
        QVERIFY(relations.Shapes().isEmpty());
        QVERIFY(relations.Bindings().isEmpty());
        QVERIFY(relations.Diagnostics().isEmpty());
    }

    void rawChainSeparatesDriversFromIndirectLimitersAndExpandsAllWarpOwners()
    {
        const auto bytes = fixture();
        auto reader = CReader::FromBytes(bytes);
        CSectionTable table(reader);
        QVERIFY2(table.IsValid(), qPrintable(reader.Error()));
        QCOMPARE(table.blend_shape_constraint_indices.size(), 3);
        for (int index = 0; index < 3; ++index)
            QCOMPARE(table.blend_shape_constraint_indices[index]->blend_shape_constraint_sources_indices,
                     word(bytes, word(bytes, 0x24c) + index * 4));
        Relations relations;
        QVERIFY(relations.Build(table));
        QVERIFY(relations.Diagnostics().isEmpty());
        QCOMPARE(relations.BlendShapeParameterIndices(), (QVector<int>{0, 2, 3}));
        QCOMPARE(relations.ParameterIndex("DriverA"), 0);
        QCOMPARE(relations.ParameterIndex("Limiter"), 1);
        QCOMPARE(relations.ParameterIndex("missing-cdi-id"), -1);
        QCOMPARE(relations.Parameters()[1].type, 0);
        QVERIFY(relations.TargetsForParameter("Limiter").isEmpty());
        QVERIFY(relations.TargetsForParameter("Unbound").isEmpty());
        QCOMPARE(targets(relations.TargetsForParameter("DriverA")), (QStringList{"0:Shared", "1:Shared", "1:OtherOwner"}));
        QCOMPARE(targets(relations.TargetsForParameter("DriverB")), (QStringList{"0:OtherMesh", "1:Shared", "1:OtherOwner"}));
        QCOMPARE(relations.ParametersForTarget(Relations::ArtMesh, "Shared"), (QVector<int>{0}));
        QCOMPARE(relations.ParametersForTarget(Relations::Warp, "Shared"), (QVector<int>{0, 2}));
        QCOMPARE(relations.BindingsForTarget(Relations::ArtMesh, "Shared"), (QVector<int>{1, 2, 3}));
        QCOMPARE(relations.BindingsForTarget(Relations::Warp, "OtherOwner"), (QVector<int>{0, 1, 2}));
        QVERIFY(relations.ParametersForTarget(Relations::Warp, "RotationOccupiesIndexOne").isEmpty());
        QCOMPARE(relations.ParameterBindings()[1].keys.begin, 6);
        QCOMPARE(relations.ParameterBindings()[1].keys.count, 3);
        QCOMPARE(relations.ParameterBindings()[1].baseKeyIndex, 1);
        QCOMPARE(relations.Bindings()[1].keyforms.count, 2);
        QCOMPARE(relations.Bindings()[0].parameterIndex, 2);
        QCOMPARE(relations.Keys().mid(6, 3), (QVector<float>{-1, 0, 1}));
        const auto limiters = relations.LimitersForBinding(0);
        QCOMPARE(limiters.size(), 2);
        QCOMPARE(limiters[0].constraintIndex, 0);
        QCOMPARE(limiters[0].parameterIndex, 1);
        QCOMPARE(limiters[0].values.begin, 2);
        QCOMPARE(limiters[0].values.count, 2);
        QCOMPARE(limiters[1].constraintIndex, 1);
        QCOMPARE(limiters[1].parameterIndex, 2);
        const auto other = relations.LimitersForBinding(1);
        QCOMPARE(other.size(), 1);
        QCOMPARE(other[0].constraintIndex, 2);
        QCOMPARE(relations.ConstraintValues()[other[0].values.begin].key, 0.5f);
        QCOMPARE(relations.ConstraintValues()[other[0].values.begin].weight, 0.5f);
        QVERIFY(relations.LimitersForBinding(2).isEmpty());
        QVERIFY(relations.LimitersForBinding(-1).isEmpty());
    }

    void typeOneParametersRemainDiscoverableWithoutShapesOrConstraints()
    {
        auto bytes = fixture();
        for (int index = 25; index < 32; ++index) setCount(bytes, index, 0);
        array(bytes, 0x20c, {0, 0, -1, 0});
        array(bytes, 0x210, {0, 0, 0, 0});
        Relations relations;
        QVERIFY(build(bytes, relations));
        QCOMPARE(relations.BlendShapeParameterIndices(), (QVector<int>{0, 2, 3}));
        QVERIFY(relations.Diagnostics().isEmpty());
        QVERIFY(relations.TargetsForParameter(0).isEmpty());
        QVERIFY(relations.Bindings().isEmpty());
    }

    void malformedBranchesRetainOtherValidRelationships_data()
    {
        QTest::addColumn<int>("slot");
        QTest::addColumn<int>("row");
        QTest::addColumn<int>("value");
        QTest::addColumn<QString>("code");
        const int large = std::numeric_limits<int>::max();
        QTest::newRow("negative-owner-count") << 0x210 << 0 << -1 << QString("bs_parameter_slice");
        QTest::newRow("overflow-owner-span") << 0x20c << 0 << large << QString("bs_parameter_slice");
        QTest::newRow("bad-binding-owner") << 0x220 << 1 << large << QString("bs_keyform_owner");
        QTest::newRow("bad-target") << 0x240 << 0 << large << QString("bs_shape");
        QTest::newRow("bad-shape-slice") << 0x244 << 0 << large << QString("bs_shape");
        QTest::newRow("bad-keyform-span") << 0x224 << 2 << large << QString("bs_keyform_slice");
        QTest::newRow("bad-indirection") << 0x24c << 1 << large << QString("bs_constraint_reference");
        QTest::newRow("bad-limiter-parameter") << 0x250 << 0 << -1 << QString("bs_constraint");
        QTest::newRow("bad-limiter-slice") << 0x254 << 0 << large << QString("bs_constraint");
        QTest::newRow("bad-constraint-reference-slice") << 0x22c << 0 << large << QString("bs_constraint_slice");
        QTest::newRow("nonfinite-curve") << 0x260 << 2 << bits(std::numeric_limits<float>::infinity()) << QString("bs_constraint");
        QTest::newRow("nonfinite-binding-key") << 0x174 << 6 << bits(std::numeric_limits<float>::quiet_NaN()) << QString("bs_keys");
    }

    void malformedBranchesRetainOtherValidRelationships()
    {
        QFETCH(int, slot);
        QFETCH(int, row);
        QFETCH(int, value);
        QFETCH(QString, code);
        auto bytes = fixture();
        setCell(bytes, slot, row, value);
        Relations relations;
        QString error;
        QVERIFY2(build(bytes, relations, &error), qPrintable(error));
        QVERIFY2(diagnosed(relations, code), qPrintable(code));
        QCOMPARE(relations.ParametersForTarget(Relations::ArtMesh, "OtherMesh"), (QVector<int>{2}));
        QCOMPARE(relations.BlendShapeParameterIndices(), (QVector<int>{0, 2, 3}));
        QVERIFY(!relations.TargetsForParameter("missing-cdi-id").size());
    }

    void overlappingOwnersAndDuplicateIdsAreNeverResolvedByFirstMatch()
    {
        auto bytes = fixture();
        setCell(bytes, 0x20c, 0, 0);
        setCell(bytes, 0x210, 0, 2);
        Relations relations;
        QVERIFY(build(bytes, relations));
        QVERIFY(diagnosed(relations, "bs_binding_owner"));
        QVERIFY(relations.TargetsForParameter("DriverB").isEmpty());
        QVERIFY(!relations.TargetsForParameter("DriverA").isEmpty());
        bytes = fixture();
        id(bytes, 0x108, 2, "DriverA");
        QVERIFY(build(bytes, relations));
        QCOMPARE(relations.ParameterIndex("DriverA"), -1);
        QVERIFY(!relations.Parameters()[0].selectable && !relations.Parameters()[2].selectable);
        QVERIFY(relations.TargetsForParameter(0).isEmpty());
        QVERIFY(diagnosed(relations, "bs_parameter_id"));
        bytes = fixture();
        id(bytes, 0xc4, 1, "Shared");
        QVERIFY(build(bytes, relations));
        QVERIFY(diagnosed(relations, "bs_target_id"));
        QVERIFY(relations.ParametersForTarget(Relations::ArtMesh, "Shared").isEmpty());
        QCOMPARE(relations.ParametersForTarget(Relations::Warp, "Shared"), (QVector<int>{0, 2}));
        QCOMPARE(targets(relations.TargetsForParameter("DriverA")), (QStringList{"1:Shared", "1:OtherOwner"}));
    }

    void duplicateConstraintReferencesDoNotDuplicateControls()
    {
        auto bytes = fixture();
        setCell(bytes, 0x24c, 2, 0);
        Relations relations;
        QVERIFY(build(bytes, relations));
        const auto values = relations.LimitersForBinding(0);
        QCOMPARE(values.size(), 1);
        QCOMPARE(values[0].constraintIndex, 0);
    }

    void snapshotsSurviveReaderLifetimeAndFailedRebuild()
    {
        Relations relations;
        QVERIFY(build(fixture(), relations));
        const Relations copy = relations;
        const auto previous = targets(relations.TargetsForParameter(0));
        auto reader = CReader::FromBytes(QByteArray("invalid"));
        CSectionTable invalid(reader);
        QString error;
        QVERIFY(!relations.Build(invalid, &error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(targets(relations.TargetsForParameter(0)), previous);
        QVERIFY(build(modelBytes("Wanko"), relations));
        QVERIFY(relations.BlendShapeParameterIndices().isEmpty());
        QCOMPARE(targets(copy.TargetsForParameter(0)), previous);
        QCOMPARE(copy.LimitersForBinding(0).size(), 2);
    }

    void sharedRangesAndSpecificOwnersDoNotExpandDuringBuild()
    {
        auto bytes = fixture();
        constexpr int owners = 1000, shapes = 5000;
        setCount(bytes, 1, owners);
        setCount(bytes, 27, shapes);
        for (int slot : {0x68, 0x70, 0x74, 0x78, 0x7c, 0x80, 0x84, 0x88})
            array(bytes, slot, QVector<int>(owners, slot == 0x88 || slot == 0x74 || slot == 0x78 ? 1 :
                                                    slot == 0x70 || slot == 0x7c || slot == 0x80 ? -1 : 0));
        const int ids = (bytes.size() + 63) & ~63;
        bytes.append(QByteArray(ids + owners * 64 - bytes.size(), '\0'));
        put(bytes, 0x6c, ids);
        for (int index = 0; index < owners; ++index) id(bytes, 0x6c, index, QByteArray("Warp") + QByteArray::number(index));
        array(bytes, 0x234, QVector<int>(shapes, 1));
        array(bytes, 0x238, QVector<int>(shapes, 0));
        array(bytes, 0x23c, QVector<int>(shapes, 4));
        Relations relations;
        QVERIFY(build(bytes, relations));
        QCOMPARE(relations.Bindings().size(), 4);
        QCOMPARE(relations.Shapes().size(), shapes + 3);
        QCOMPARE(relations.Targets().size(), owners + 2);
        QCOMPARE(relations.TargetsForParameter("DriverA").size(), owners + 1);
        QCOMPARE(relations.BindingsForTarget(Relations::Warp, "Warp999"), (QVector<int>{0, 1, 2, 3}));
        QCOMPARE(relations.ParametersForTarget(Relations::Warp, "Warp999"), (QVector<int>{0, 2}));
    }
};

QTEST_APPLESS_MAIN(BlendShapeRelationsTests)
#include "blend_shape_relations_tests.moc"
