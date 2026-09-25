#include "live2dstruct/ckeyformmappingcontext.h"
#include "live2dstruct/cmodeldocument.h"
#include "live2dstruct/csectiontable.h"

#include <QtTest>
#include <QFile>
#include <QTemporaryDir>
#include <QtEndian>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

using namespace live2dstruct;

namespace {
QString sample(const QString& name)
{
    return QString(TEST_RESOURCES) + '/' + name + '/' + name + ".moc3";
}

void put(QByteArray& bytes, int offset, int value)
{
    Q_ASSERT(offset >= 0 && offset <= bytes.size() - 4);
    qToLittleEndian(qint32(value), bytes.data() + offset);
}

void real(QByteArray& bytes, int offset, float value)
{
    quint32 bits;
    std::memcpy(&bits, &value, 4);
    qToLittleEndian(bits, bytes.data() + offset);
}

int integer(const QByteArray& bytes, int offset)
{
    return qFromLittleEndian<qint32>(bytes.constData() + offset);
}

bool write(const QString& path, const QByteArray& bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(bytes) == bytes.size();
}

QMap<QString, float> pose(const CModelDocument& document)
{
    QMap<QString, float> values;
    for (const auto& parameter : document.Parameters()) values.insert(parameter.id, document.ParameterValue(parameter.id));
    return values;
}

bool near(const QVector<float>& actual, const QVector<float>& expected)
{
    if (actual.size() != expected.size()) return false;
    for (int index = 0; index < actual.size(); ++index)
        if (!std::isfinite(actual[index]) || std::abs(double(actual[index]) - expected[index]) > 0.00002) return false;
    return true;
}

class Fixture
{
public:
    Fixture(bool quad = true) : reader(sample("Mark")), table(reader), bytes(reader.m_byte)
    {
        if (!table.IsValid()) return;
        const auto& o = *table.sectionOffsetTable;
        for (int index = 0; index < 32; ++index) count(index, 0);
        for (const auto& entry : {std::pair{1, 3}, {2, 1}, {3, 2}, {4, 1}, {5, 1}, {7, 1},
                                  {8, 2}, {9, 2}, {10, 20}, {11, 1}, {12, 1}, {13, 1}, {14, 2}, {15, 6}, {16, 3}})
            count(entry.first, entry.second);
        for (int index = 0; index < 3; ++index) {
            id(o.deformers->ids, index, index == 0 ? "Root" : index == 1 ? "Warp" : "Child");
            column(o.deformers->keyform_binding_sources_indices, index, -1);
            column(o.deformers->is_visible, index, 1);
            column(o.deformers->is_enabled, index, 1);
            column(o.deformers->parent_part_indices, index, -1);
            column(o.deformers->parent_deformer_indices, index, index - 1);
            column(o.deformers->types, index, index == 1 ? 0 : 1);
            column(o.deformers->specific_sources_indices, index, index == 2 ? 1 : 0);
        }
        for (int index = 0; index < 2; ++index) {
            column(o.rotation_deformers->keyform_binding_sources_indices, index, -1);
            column(o.rotation_deformers->keyform_sources_begin_indices, index, index);
            column(o.rotation_deformers->keyform_sources_counts, index, 1);
            real(bytes, o.rotation_deformers->base_angles + index * 4, 0);
            real(bytes, o.rotation_deformer_keyforms->opacities + index * 4, 1);
            real(bytes, o.rotation_deformer_keyforms->angles + index * 4, index == 0 ? 90 : 0);
            real(bytes, o.rotation_deformer_keyforms->origin_x + index * 4, index == 0 ? 10 : 0.25f);
            real(bytes, o.rotation_deformer_keyforms->origin_y + index * 4, index == 0 ? -4 : 0.25f);
            real(bytes, o.rotation_deformer_keyforms->scales + index * 4, index == 0 ? 1 : 0.5f);
            column(o.rotation_deformer_keyforms->is_reflect_x, index, 0);
            column(o.rotation_deformer_keyforms->is_reflect_y, index, 0);
        }
        column(o.warp_deformers->keyform_binding_sources_indices, 0, -1);
        column(o.warp_deformers->keyform_sources_begin_indices, 0, 0);
        column(o.warp_deformers->keyform_sources_counts, 0, 1);
        column(o.warp_deformers->vertex_counts, 0, 4);
        column(o.warp_deformers->rows, 0, 1);
        column(o.warp_deformers->columns, 0, 1);
        appendColumn(0x1d4, {quad ? 1 : 0});
        real(bytes, o.warp_deformer_keyforms->opacities, 1);
        column(o.warp_deformer_keyforms->keyform_position_sources_begin_indices, 0, 12);
        id(o.art_meshes->ids, 0, "Mesh");
        column(o.art_meshes->keyform_binding_sources_indices, 0, 0);
        column(o.art_meshes->keyform_sources_begin_indices, 0, 0);
        column(o.art_meshes->keyform_sources_counts, 0, 2);
        column(o.art_meshes->is_visible, 0, 1);
        column(o.art_meshes->is_enabled, 0, 1);
        column(o.art_meshes->parent_part_indices, 0, -1);
        column(o.art_meshes->parent_deformer_indices, 0, 2);
        column(o.art_meshes->texture_nos, 0, 0);
        bytes[o.art_meshes->drawable_flags] = 4;
        column(o.art_meshes->vertex_counts, 0, 3);
        column(o.art_meshes->uv_sources_begin_indices, 0, 0);
        column(o.art_meshes->position_index_sources_begin_indices, 0, 0);
        column(o.art_meshes->position_index_sources_counts, 0, 3);
        column(o.art_meshes->drawable_mask_sources_begin_indices, 0, 0);
        column(o.art_meshes->drawable_mask_sources_counts, 0, 0);
        for (int index = 0; index < 2; ++index) {
            real(bytes, o.art_mesh_keyforms->opacities + index * 4, 1);
            real(bytes, o.art_mesh_keyforms->draw_orders + index * 4, 0);
            column(o.art_mesh_keyforms->keyform_position_sources_begin_indices, index, index * 6);
        }
        const QVector<float> positions{0, 0.5f, 1, 0, 0, 0, 0.1f, 0.7f, 1.1f, 0.2f, 0.1f, 0.2f,
                                       0, 0, 2, 0, 0, 2, 3, 3};
        for (int index = 0; index < positions.size(); ++index) real(bytes, o.keyform_positions->xys + index * 4, positions[index]);
        for (int index = 0; index < 6; ++index) real(bytes, o.uvs->uvs + index * 4, float(index % 2));
        for (int index = 0; index < 3; ++index) qToLittleEndian(quint16(index), bytes.data() + o.position_indices->indices + index * 2);
        id(o.parameters->ids, 0, "P");
        real(bytes, o.parameters->min_values, 0);
        real(bytes, o.parameters->max_values, 1);
        real(bytes, o.parameters->default_values, 0);
        column(o.parameters->is_repeat, 0, 0);
        column(o.parameters->decimal_places, 0, 3);
        column(o.parameters->parameter_binding_sources_begin_indices, 0, 0);
        column(o.parameters->parameter_binding_sources_counts, 0, 1);
        appendColumn(0x1d8, {0});
        appendColumn(0x1dc, {0});
        appendColumn(0x1e0, {2});
        appendColumn(0x208, {0});
        appendColumn(0x20c, {0});
        appendColumn(0x210, {0});
        column(o.keyform_bindings->parameter_binding_index_sources_begin_indices, 0, 0);
        column(o.keyform_bindings->parameter_binding_index_sources_counts, 0, 1);
        column(o.parameter_binding_indices->binding_sources_indices, 0, 0);
        column(o.parameter_bindings->keys_sources_begin_indices, 0, 0);
        column(o.parameter_bindings->keys_sources_counts, 0, 2);
        real(bytes, o.keys->values, 0);
        real(bytes, o.keys->values + 4, 1);
    }

    void count(int index, int value) { put(bytes, table.sectionOffsetTable->count_info->count_info_offset + index * 4, value); }
    void column(int offset, int row, int value) { put(bytes, offset + row * 4, value); }
    void id(int offset, int row, const QByteArray& value) { bytes.replace(offset + row * 64, 64, value.leftJustified(64, '\0')); }
    void appendColumn(int slot, std::initializer_list<int> values)
    {
        const int offset = bytes.size();
        bytes.append(QByteArray(values.size() * 4, '\0'));
        put(bytes, slot, offset);
        int index = 0;
        for (int value : values) put(bytes, offset + index++ * 4, value);
    }

    CReader reader;
    CSectionTable table;
    QByteArray bytes;
};
}

class KeyformMappingTests : public QObject
{
    Q_OBJECT
private slots:
    void sourceModelsHaveSupportedMeshes_data()
    {
        QTest::addColumn<QString>("name");
        for (const char* name : {"Mark", "Rice", "Natori", "Wanko"}) QTest::newRow(name) << QString::fromLatin1(name);
    }

    void sourceModelsHaveSupportedMeshes()
    {
        QFETCH(QString, name);
        CModelDocument document;
        QString error;
        QVERIFY2(document.Load(sample(name), &error), qPrintable(error));
        bool found = false;
        for (const auto& mesh : document.Drawables()) {
            QMap<QString, float> corner;
            if (!document.KeyformParameterValues(mesh.id, "artmesh", 0, corner, &error)) continue;
            auto values = pose(document);
            for (auto it = corner.cbegin(); it != corner.cend(); ++it) values.insert(it.key(), it.value());
            CKeyformMappingContext context;
            if (!context.Build(document, mesh.id, 0, values, &error)) continue;
            const auto local = document.DrawableKeyformPositions(mesh.id, 0);
            QVector<float> world, restored;
            QVERIFY(context.ToModel(local, world));
            QVERIFY(context.MatchesRuntime(world));
            QVERIFY(context.FromModel(world, restored));
            QVERIFY(near(local, restored));
            found = true;
            break;
        }
        QVERIFY2(found, qPrintable(name + ": " + error));
    }

    void nestedRotationWarpRotationUsesIndependentExpectedPoints_data()
    {
        QTest::addColumn<bool>("quad");
        QTest::newRow("bilinear") << true;
        QTest::newRow("triangles") << false;
    }

    void nestedRotationWarpRotationUsesIndependentExpectedPoints()
    {
        QFETCH(bool, quad);
        Fixture fixture(quad);
        QVERIFY(fixture.table.IsValid());
        CModelDocument document;
        QString error;
        QVERIFY2(document.LoadBytes(fixture.bytes, "nested.moc3", &error), qPrintable(error));
        CKeyformMappingContext context;
        QVERIFY2(context.Build(document, "Mesh", 0, {{"P", 0}}, &error), qPrintable(error));
        const QVector<float> expected = quad ? QVector<float>{8.875f, -3.375f, 9.3125f, -2.3125f, 9.4375f, -3.4375f}
                                             : QVector<float>{9, -3.5f, 9.5f, -2.5f, 9.5f, -3.5f};
        const auto local = document.DrawableKeyformPositions("Mesh", 0);
        for (int repeat = 0; repeat < 10; ++repeat) {
            QVector<float> world;
            QVERIFY(context.ToModel(local, world));
            QVERIFY(near(world, expected));
            QVERIFY(context.MatchesRuntime(expected));
            QVERIFY(context.FromModel(world, world));
            QVERIFY(near(world, local));
        }
        CKeyformMappingContext copied = context;
        document.Clear();
        QVERIFY(copied.MatchesRuntime(expected));
        QVector<float> localSubset;
        QVERIFY(copied.FromModel(expected.mid(0, 2), localSubset));
        QVERIFY(near(localSubset, local.mid(0, 2)));
    }

    void overridesAndDiskChangesDoNotRetargetAnExistingContext()
    {
        Fixture fixture;
        QVERIFY(fixture.table.IsValid());
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath("model.moc3");
        QVERIFY(write(path, fixture.bytes));
        CModelDocument document;
        QVERIFY(document.Load(path));
        auto local = document.DrawableKeyformPositions("Mesh", 1);
        local[0] = 0.25f;
        QVERIFY(document.SetDrawableKeyformPositions("Mesh", 1, local));
        CKeyformMappingContext context;
        QString error;
        QVERIFY(write(path, "replaced on disk"));
        QVERIFY2(context.Build(document, "Mesh", 1, {{"P", 1}}, &error), qPrintable(error));
        QVector<float> world;
        QVERIFY(context.ToModel(local, world));
        QVERIFY(context.MatchesRuntime(world));
        local[0] += 0.1f;
        QVERIFY(document.SetDrawableKeyformPositions("Mesh", 1, local));
        QVERIFY(QFile::remove(path));
        QVERIFY(context.MatchesRuntime(world));
        CKeyformMappingContext changed;
        QVERIFY2(changed.Build(document, "Mesh", 1, {{"P", 1}}, &error), qPrintable(error));
        QVERIFY(!changed.MatchesRuntime(world));
        document.Clear();
        QVERIFY(context.MatchesRuntime(world));
    }

    void unsupportedChainsAndInvalidBuildsPreservePreviousContext_data()
    {
        QTest::addColumn<QString>("invalid");
        for (const char* value : {"wrong-corner", "missing-pose", "extra-pose", "nan-pose", "out-of-range", "missing-mesh",
                                  "bad-keyform", "cycle", "missing-parent", "rotation-collapse", "folded-warp", "type1-target",
                                  "type1-parent", "blend-mesh", "blend-parent", "glue", "parent-override", "warp-override", "rotation-override"})
            QTest::newRow(value) << QString::fromLatin1(value);
    }

    void unsupportedChainsAndInvalidBuildsPreservePreviousContext()
    {
        QFETCH(QString, invalid);
        Fixture fixture;
        QVERIFY(fixture.table.IsValid());
        CModelDocument baseline;
        QVERIFY(baseline.LoadBytes(fixture.bytes, "baseline"));
        CKeyformMappingContext context;
        QString error;
        QVERIFY2(context.Build(baseline, "Mesh", 0, {{"P", 0}}, &error), qPrintable(error));
        QVector<float> expected;
        QVERIFY(context.ToModel(baseline.DrawableKeyformPositions("Mesh", 0), expected));
        const auto& o = *fixture.table.sectionOffsetTable;
        auto values = pose(baseline);
        QString id = "Mesh";
        int keyform = 0;
        if (invalid == "wrong-corner") values["P"] = 0.5f;
        if (invalid == "missing-pose") values.clear();
        if (invalid == "extra-pose") values.insert("unknown", 0);
        if (invalid == "nan-pose") values["P"] = std::numeric_limits<float>::quiet_NaN();
        if (invalid == "out-of-range") values["P"] = 2;
        if (invalid == "missing-mesh") id = "missing";
        if (invalid == "bad-keyform") keyform = 2;
        if (invalid == "cycle") fixture.column(o.deformers->parent_deformer_indices, 0, 2);
        if (invalid == "missing-parent") fixture.column(o.deformers->parent_deformer_indices, 0, 3);
        if (invalid == "rotation-collapse") real(fixture.bytes, o.rotation_deformer_keyforms->scales, 0);
        if (invalid == "folded-warp") {
            const QVector<float> folded{0, 0, 2, 2, 0, 2, 2, 0};
            for (int i = 0; i < folded.size(); ++i) real(fixture.bytes, o.keyform_positions->xys + (12 + i) * 4, folded[i]);
            real(fixture.bytes, o.keyform_positions->xys, 0.5f);
            real(fixture.bytes, o.keyform_positions->xys + 4, 0);
        }
        if (invalid == "type1-target" || invalid == "type1-parent") {
            put(fixture.bytes, integer(fixture.bytes, 0x208), 1);
            if (invalid == "type1-parent") {
                fixture.column(o.art_meshes->keyform_binding_sources_indices, 0, -1);
                fixture.column(o.art_meshes->keyform_sources_counts, 0, 1);
                fixture.column(o.deformers->keyform_binding_sources_indices, 0, 0);
                fixture.column(o.rotation_deformers->keyform_sources_counts, 0, 2);
            }
        }
        if (invalid == "blend-mesh" || invalid == "blend-parent") {
            const bool mesh = invalid == "blend-mesh";
            fixture.count(mesh ? 28 : 27, 1);
            fixture.appendColumn(mesh ? 0x240 : 0x234, {0});
            fixture.appendColumn(mesh ? 0x244 : 0x238, {0});
            fixture.appendColumn(mesh ? 0x248 : 0x23c, {0});
        }
        if (invalid == "glue") {
            fixture.count(20, 1);
            for (int slot : {0x1a4, 0x1ac, 0x1b0, 0x1b4, 0x1b8, 0x1bc, 0x1c0, 0x1c4})
                fixture.appendColumn(slot, {slot == 0x1ac ? -1 : 0});
            const int offset = fixture.bytes.size();
            fixture.bytes.append(QByteArray("Glue").leftJustified(64, '\0'));
            put(fixture.bytes, 0x1a8, offset);
        }
        CModelDocument invalidDocument;
        QVERIFY2(invalidDocument.LoadBytes(fixture.bytes, "invalid-candidate", &error), qPrintable(error));
        if (invalid == "parent-override") QVERIFY(invalidDocument.SetDeformerParentIndex("Child", -1));
        if (invalid == "warp-override") {
            auto forms = invalidDocument.DeformerWarpKeyforms("Warp");
            QVERIFY(!forms.isEmpty());
            forms[0].positions[0] += 0.1f;
            QVERIFY(invalidDocument.SetDeformerWarpKeyforms("Warp", forms));
        }
        if (invalid == "rotation-override") {
            auto forms = invalidDocument.DeformerRotationKeyforms("Root");
            QVERIFY(!forms.isEmpty());
            forms[0].angle += 1;
            QVERIFY(invalidDocument.SetDeformerRotationKeyforms("Root", forms));
        }
        QVERIFY2(!context.Build(invalidDocument, id, keyform, values, &error), qPrintable(invalid));
        QVERIFY(!error.isEmpty());
        QVERIFY(context.IsValid());
        QVERIFY(context.MatchesRuntime(expected));
        QVector<float> local;
        QVERIFY(context.FromModel(expected, local));
        QVERIFY(near(local, baseline.DrawableKeyformPositions("Mesh", 0)));
    }

    void noOpParentAndKeyformOverridesRemainSupported()
    {
        Fixture fixture;
        QVERIFY(fixture.table.IsValid());
        CModelDocument document;
        QVERIFY(document.LoadBytes(fixture.bytes, "unchanged-overrides"));
        QVERIFY(document.SetDeformerParentIndex("Child", 1));
        QVERIFY(document.SetDeformerRotationKeyforms("Root", document.DeformerRotationKeyforms("Root")));
        QVERIFY(document.SetDeformerWarpKeyforms("Warp", document.DeformerWarpKeyforms("Warp")));
        CKeyformMappingContext context;
        QString error;
        QVERIFY2(context.Build(document, "Mesh", 0, {{"P", 0}}, &error), qPrintable(error));
    }

    void unrelatedBlendShapeDoesNotDisableTheTargetChain()
    {
        Fixture fixture;
        QVERIFY(fixture.table.IsValid());
        const auto& o = *fixture.table.sectionOffsetTable;
        fixture.count(4, 2);
        fixture.id(o.art_meshes->ids, 1, "Other");
        for (const auto& entry : {std::pair{o.art_meshes->keyform_binding_sources_indices, -1},
                                  {o.art_meshes->keyform_sources_begin_indices, 0},
                                  {o.art_meshes->keyform_sources_counts, 1},
                                  {o.art_meshes->parent_part_indices, -1},
                                  {o.art_meshes->parent_deformer_indices, -1},
                                  {o.art_meshes->vertex_counts, 3},
                                  {o.art_meshes->uv_sources_begin_indices, 0},
                                  {o.art_meshes->position_index_sources_begin_indices, 0},
                                  {o.art_meshes->position_index_sources_counts, 3},
                                  {o.art_meshes->drawable_mask_sources_begin_indices, 0},
                                  {o.art_meshes->drawable_mask_sources_counts, 0}})
            fixture.column(entry.first, 1, entry.second);
        fixture.count(28, 1);
        fixture.appendColumn(0x240, {1});
        fixture.appendColumn(0x244, {0});
        fixture.appendColumn(0x248, {0});
        CModelDocument document;
        QString error;
        QVERIFY2(document.LoadBytes(fixture.bytes, "unrelated-blend", &error), qPrintable(error));
        CKeyformMappingContext context;
        QVERIFY2(context.Build(document, "Mesh", 0, {{"P", 0}}, &error), qPrintable(error));
        QVERIFY(!context.Build(document, "Other", 0, {{"P", 0}}, &error));
        QVERIFY(error.contains("BlendShape"));
    }

    void comparisonToleranceAndInvalidMappingsDoNotChangeOutputs()
    {
        CKeyformMappingContext context;
        QVector<float> output{7, 9};
        QVERIFY(!context.ToModel({0, 0}, output));
        QCOMPARE(output, (QVector<float>{7, 9}));
        QVERIFY(!context.FromModel({0, 0}, output));
        QCOMPARE(output, (QVector<float>{7, 9}));
        Fixture fixture;
        CModelDocument document;
        QVERIFY(document.LoadBytes(fixture.bytes, "tolerance"));
        QVERIFY(context.Build(document, "Mesh", 0, {{"P", 0}}));
        QVector<float> expected;
        QVERIFY(context.ToModel(document.DrawableKeyformPositions("Mesh", 0), expected));
        auto close = expected;
        close[0] += float(CKeyformMappingContext::AbsoluteTolerance * 0.25);
        QVERIFY(context.MatchesRuntime(close));
        auto shifted = expected;
        shifted[0] += 0.001f;
        QVERIFY(!context.MatchesRuntime(shifted));
        auto relative = expected;
        relative[0] += float((CKeyformMappingContext::AbsoluteTolerance +
                              CKeyformMappingContext::RelativeTolerance * std::abs(expected[0])) * 2);
        QVERIFY(!context.MatchesRuntime(relative));
        QVERIFY(!context.MatchesRuntime(expected.mid(0, 2)));
        auto nonFinite = expected;
        nonFinite[0] = std::numeric_limits<float>::quiet_NaN();
        QVERIFY(!context.MatchesRuntime(nonFinite));
        for (const QVector<float>& input : {QVector<float>{0}, QVector<float>{std::numeric_limits<float>::quiet_NaN(), 0},
                                           QVector<float>{0, std::numeric_limits<float>::infinity()}}) {
            output = {7, 9};
            QVERIFY(!context.ToModel(input, output));
            QCOMPARE(output, (QVector<float>{7, 9}));
            QVERIFY(!context.FromModel(input, output));
            QCOMPARE(output, (QVector<float>{7, 9}));
        }
    }
};

QTEST_GUILESS_MAIN(KeyformMappingTests)
#include "keyform_mapping_tests.moc"
