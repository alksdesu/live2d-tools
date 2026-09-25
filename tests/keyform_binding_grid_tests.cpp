#include "live2dstruct/ckeyformbindinggrid.h"
#include "live2dstruct/csectiontable.h"

#include <QtTest>
#include <QTemporaryDir>
#include <QtEndian>
#include <algorithm>
#include <cstring>
#include <functional>
#include <limits>
#include <vector>

using namespace live2dstruct;

namespace {
QString sourcePath(const QString& model)
{
    return QString(TEST_RESOURCES) + '/' + model + '/' + model + ".moc3";
}

qint32 integer(const QByteArray& bytes, int offset)
{
    Q_ASSERT(offset >= 0 && offset <= bytes.size() - 4);
    return qFromLittleEndian<qint32>(bytes.constData() + offset);
}

qint32 column(const QByteArray& bytes, int slot, int row)
{
    return integer(bytes, integer(bytes, 0x40 + slot * 4) + row * 4);
}

float real(const QByteArray& bytes, int slot, int row)
{
    const quint32 bits = static_cast<quint32>(column(bytes, slot, row));
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

QString identifier(const QByteArray& bytes, int slot, int row)
{
    const QByteArray text = bytes.mid(integer(bytes, 0x40 + slot * 4) + row * 64, 64);
    const auto end = std::find(text.cbegin(), text.cend(), '\0');
    return QString::fromUtf8(text.constData(), end - text.cbegin());
}

int appendBlendRow(CReader& reader, int target)
{
    const int begin = static_cast<int>(reader.m_byte.size());
    for (qint32 value : {target, 0, 1}) {
        const qint32 littleEndian = qToLittleEndian(value);
        reader.m_byte.append(reinterpret_cast<const char*>(&littleEndian), sizeof(littleEndian));
    }
    return begin;
}

template<typename T>
void growRows(QVector<T*>& rows, int count)
{
    while (rows.size() < count) rows.append(new T(*rows.front()));
}

class Fixture
{
public:
    Fixture() : reader(sourcePath("Mark")), table(reader)
    {
        if (!table.IsValid()) return;
        for (qsizetype index = 0; index < table.parameters.size(); ++index) {
            auto* parameter = table.parameters[index];
            parameter->ids = QStringLiteral("P%1").arg(index);
            parameter->parameter_types = 0;
            parameter->min_values = -100;
            parameter->max_values = 100;
            parameter->default_values = 7;
            parameter->parameter_binding_sources_begin_indices = 0;
            parameter->parameter_binding_sources_counts = 0;
        }
        mesh = table.art_meshes.front();
        mesh->ids = "target";
        mesh->keyform_sources_begin_indices = 0;
        auto* unaffected = table.art_meshes.back();
        unaffected->ids = "unaffected";
        unaffected->keyform_binding_sources_indices = -1;
        unaffected->keyform_sources_begin_indices = 0;
        unaffected->keyform_sources_counts = 1;
    }

    void Binding(int bindingIndex, int parameterIndex, const QVector<float>& keys)
    {
        auto* binding = table.parameter_bindings[bindingIndex];
        binding->keys_sources_begin_indices = static_cast<int>(table.keys->values.size());
        binding->keys_sources_counts = static_cast<int>(keys.size());
        table.keys->values.insert(table.keys->values.end(), keys.cbegin(), keys.cend());
        auto* parameter = table.parameters[parameterIndex];
        parameter->parameter_binding_sources_begin_indices = bindingIndex;
        parameter->parameter_binding_sources_counts = 1;
    }

    void Grid(const QVector<int>& axes, int forms)
    {
        mesh->keyform_binding_sources_indices = 0;
        mesh->keyform_sources_counts = forms;
        auto* grid = table.keyform_bindings.front();
        grid->parameter_binding_index_sources_begin_indices = 0;
        grid->parameter_binding_index_sources_counts = static_cast<int>(axes.size());
        while (table.parameter_binding_indices.size() < axes.size())
            table.parameter_binding_indices.append(new CParameter_binding_indices(*table.parameter_binding_indices.front()));
        for (qsizetype index = 0; index < axes.size(); ++index)
            table.parameter_binding_indices[index]->binding_sources_indices = axes[index];
    }

    CReader reader;
    CSectionTable table;
    CArt_meshes* mesh = nullptr;
};
}

class KeyformBindingGridTests : public QObject
{
    Q_OBJECT

    void checkRawObject(const QByteArray& bytes, const CKeyformBindingGrid& grid,
                        CKeyformBindingGrid::ObjectType type, const QString& id, int binding, int frameCount)
    {
        struct RawAxis { int binding; QString id; int type; QVector<float> keys; };
        QVector<RawAxis> expected;
        const int parameterCount = integer(bytes, integer(bytes, 0x40) + 5 * 4);
        if (binding >= 0) {
            const int first = column(bytes, 73, binding), count = column(bytes, 74, binding);
            for (int axis = 0; axis < count; ++axis) {
                const int source = column(bytes, 72, first + axis);
                int owner = -1;
                for (int parameter = 0; parameter < parameterCount; ++parameter) {
                    const int begin = column(bytes, 56, parameter), length = column(bytes, 57, parameter);
                    if (source >= begin && source - begin < length) {
                        QCOMPARE(owner, -1);
                        owner = parameter;
                    }
                }
                QVERIFY(owner >= 0);
                const int typeOffset = integer(bytes, 0x208);
                RawAxis definition{source, identifier(bytes, 50, owner),
                                   typeOffset ? integer(bytes, typeOffset + owner * 4) : 0, {}};
                const int start = column(bytes, 75, source), length = column(bytes, 76, source);
                for (int key = 0; key < length; ++key) definition.keys.append(real(bytes, 77, start + key));
                expected.append(definition);
            }
        }
        const auto actual = grid.Axes(type, id);
        QCOMPARE(actual.size(), expected.size());
        QVERIFY2(grid.Diagnostic(type, id).isEmpty(), qPrintable(id + ": " + grid.Diagnostic(type, id)));
        for (qsizetype axis = 0; axis < actual.size(); ++axis) {
            QCOMPARE(actual[axis].bindingIndex, expected[axis].binding);
            QCOMPARE(actual[axis].parameterId, expected[axis].id);
            QCOMPARE(actual[axis].parameterType, expected[axis].type);
            QCOMPARE(actual[axis].keys, expected[axis].keys);
        }
        QVector<int> cursor(expected.size(), 0);
        for (int frame = 0; frame < frameCount; ++frame) {
            QMap<QString, float> expectedValues;
            for (qsizetype axis = 0; axis < expected.size(); ++axis)
                expectedValues.insert(expected[axis].id, expected[axis].keys[cursor[axis]]);
            QMap<QString, float> values{{"old", 777}};
            QString error = "previous";
            QVERIFY2(grid.Pose(type, id, frame, values, &error), qPrintable(id + ": " + error));
            QCOMPARE(values, expectedValues);
            QVERIFY(error.isEmpty());
            for (qsizetype axis = 0; axis < cursor.size(); ++axis) {
                ++cursor[axis];
                if (cursor[axis] < expected[axis].keys.size()) break;
                cursor[axis] = 0;
            }
        }
        for (int index : cursor) QCOMPARE(index, 0);
        QMap<QString, float> preserved{{"old", 777}};
        const auto before = preserved;
        QVERIFY(!grid.Pose(type, id, frameCount, preserved));
        QCOMPARE(preserved, before);
    }

private slots:
    void originalObjectsDecodeEveryKeyformAgainstFileBytes_data()
    {
        QTest::addColumn<QString>("model");
        for (const char* model : {"Mark", "Natori", "Rice", "Wanko"})
            QTest::newRow(model) << QString::fromLatin1(model);
    }

    void originalObjectsDecodeEveryKeyformAgainstFileBytes()
    {
        QFETCH(QString, model);
        CReader reader(sourcePath(model));
        CSectionTable table(reader);
        QVERIFY2(table.IsValid(), qPrintable(reader.Error()));
        CKeyformBindingGrid grid;
        QString error;
        QVERIFY2(grid.Build(table, &error), qPrintable(error));
        const QByteArray& bytes = reader.m_byte;
        const int countOffset = integer(bytes, 0x40);
        const int meshCount = integer(bytes, countOffset + 4 * 4);
        for (int index = 0; index < meshCount; ++index)
            checkRawObject(bytes, grid, CKeyformBindingGrid::ArtMesh, identifier(bytes, 33, index),
                           column(bytes, 34, index), column(bytes, 36, index));
        const int deformerCount = integer(bytes, countOffset + 4);
        for (int index = 0; index < deformerCount; ++index) {
            const bool warp = column(bytes, 17, index) == 0;
            const int specific = column(bytes, 18, index);
            checkRawObject(bytes, grid, warp ? CKeyformBindingGrid::Warp : CKeyformBindingGrid::Rotation,
                           identifier(bytes, 11, index), column(bytes, 12, index),
                           column(bytes, warp ? 21 : 27, specific));
        }
    }

    void threeAxesAndTwoMouthBindingsKeepTheirOriginalOrder()
    {
        CReader natoriReader(sourcePath("Natori"));
        CSectionTable natori(natoriReader);
        QVERIFY(natori.IsValid());
        CKeyformBindingGrid grid;
        QVERIFY(grid.Build(natori));
        const auto eyes = grid.Axes(CKeyformBindingGrid::ArtMesh, "ArtMesh24");
        QCOMPARE(eyes.size(), 3);
        QCOMPARE(eyes[0].bindingIndex, 7);
        QCOMPARE(eyes[1].bindingIndex, 9);
        QCOMPARE(eyes[2].bindingIndex, 10);
        QMap<QString, float> values;
        QVERIFY(grid.Pose(CKeyformBindingGrid::ArtMesh, "ArtMesh24", 1, values));
        QCOMPARE(values, (QMap<QString, float>{{"ParamEyeROpen", 1}, {"ParamEyeRSmile", 0}, {"ParamEyeRForm", -2}}));
        QVERIFY(grid.Pose(CKeyformBindingGrid::ArtMesh, "ArtMesh24", 47, values));
        QCOMPARE(values, (QMap<QString, float>{{"ParamEyeROpen", 1.3f}, {"ParamEyeRSmile", 1}, {"ParamEyeRForm", 3}}));

        CReader wankoReader(sourcePath("Wanko"));
        CSectionTable wanko(wankoReader);
        QVERIFY(wanko.IsValid());
        QVERIFY(grid.Build(wanko));
        const auto mouth = grid.Axes(CKeyformBindingGrid::ArtMesh, "D_MOUTH_02");
        const auto face = grid.Axes(CKeyformBindingGrid::ArtMesh, "D_FACE_01");
        QCOMPARE(mouth.size(), 1);
        QCOMPARE(face.size(), 1);
        QCOMPARE(mouth[0].parameterId, face[0].parameterId);
        QCOMPARE(mouth[0].bindingIndex, 5);
        QCOMPARE(face[0].bindingIndex, 6);
        QCOMPARE(mouth[0].keys, (QVector<float>{0, 0.2f, 0.5f, 1}));
        QCOMPARE(face[0].keys, (QVector<float>{0, 1}));
        QVERIFY(grid.Pose(CKeyformBindingGrid::ArtMesh, "D_MOUTH_02", 1, values));
        QCOMPARE(values.value("PARAM_MOUTH_OPEN_Y"), 0.2f);
        QVERIFY(grid.Pose(CKeyformBindingGrid::ArtMesh, "D_FACE_01", 1, values));
        QCOMPARE(values.value("PARAM_MOUTH_OPEN_Y"), 1.0f);
        QVERIFY(grid.Pose(CKeyformBindingGrid::ArtMesh, "D_EFFECT_05", 5, values));
        QCOMPARE(values, (QMap<QString, float>{{"PARAM_YUGE_02", 1}, {"PARAM_YUGE_01", 0}, {"PARAM_EFFECT", 1}}));
    }

    void defaultValuesDoNotShiftSingletonOrUnboundGrids()
    {
        Fixture fixture;
        QVERIFY(fixture.table.IsValid());
        fixture.Binding(0, 0, {0.2f, 0.7f, 1.2f});
        fixture.table.parameters[0]->default_values = 0.7f;
        fixture.Binding(1, 1, {7});
        fixture.Grid({0, 1}, 3);
        CKeyformBindingGrid grid;
        QVERIFY(grid.Build(fixture.table));
        QMap<QString, float> values;
        QVERIFY(grid.Pose(CKeyformBindingGrid::ArtMesh, "target", 0, values));
        QCOMPARE(values, (QMap<QString, float>{{"P0", 0.2f}, {"P1", 7}}));
        QVERIFY(grid.Pose(CKeyformBindingGrid::ArtMesh, "target", 2, values));
        QCOMPARE(values, (QMap<QString, float>{{"P0", 1.2f}, {"P1", 7}}));
        fixture.Grid({}, 1);
        QVERIFY(grid.Build(fixture.table));
        QVERIFY(grid.Pose(CKeyformBindingGrid::ArtMesh, "target", 0, values));
        QVERIFY(values.isEmpty());
        fixture.mesh->keyform_binding_sources_indices = -1;
        QVERIFY(grid.Build(fixture.table));
        QVERIFY(grid.Pose(CKeyformBindingGrid::ArtMesh, "target", 0, values));
        QVERIFY(values.isEmpty());
        fixture.mesh->keyform_sources_counts = 2;
        QVERIFY(grid.Build(fixture.table));
        QVERIFY(!grid.Pose(CKeyformBindingGrid::ArtMesh, "target", 0, values));
    }

    void duplicateParameterAxesArePreservedAndConflictingCellsFail()
    {
        Fixture fixture;
        QVERIFY(fixture.table.IsValid());
        fixture.Binding(0, 0, {0, 1});
        fixture.Binding(1, 0, {0, 1});
        fixture.table.parameters[0]->parameter_binding_sources_begin_indices = 0;
        fixture.table.parameters[0]->parameter_binding_sources_counts = 2;
        fixture.Grid({0, 1}, 4);
        CKeyformBindingGrid grid;
        QVERIFY(grid.Build(fixture.table));
        const auto axes = grid.Axes(CKeyformBindingGrid::ArtMesh, "target");
        QCOMPARE(axes.size(), 2);
        QCOMPARE(axes[0].bindingIndex, 0);
        QCOMPARE(axes[1].bindingIndex, 1);
        QCOMPARE(axes[0].parameterId, axes[1].parameterId);
        QMap<QString, float> values;
        QVERIFY(grid.Pose(CKeyformBindingGrid::ArtMesh, "target", 0, values));
        QCOMPARE(values, (QMap<QString, float>{{"P0", 0}}));
        for (int frame : {1, 2}) {
            const auto before = values;
            QString error;
            QVERIFY(!grid.Pose(CKeyformBindingGrid::ArtMesh, "target", frame, values, &error));
            QCOMPARE(values, before);
            QVERIFY(error.contains("不同关键值"));
        }
        QVERIFY(grid.Pose(CKeyformBindingGrid::ArtMesh, "target", 3, values));
        QCOMPARE(values, (QMap<QString, float>{{"P0", 1}}));
    }

    void malformedObjectsDoNotInvalidateUnrelatedGrids()
    {
        const std::vector<std::function<void(Fixture&)>> invalid{
            [](Fixture& f) { f.table.parameters[0]->parameter_binding_sources_counts = 0; },
            [](Fixture& f) { f.table.parameters[1]->parameter_binding_sources_begin_indices = 0; f.table.parameters[1]->parameter_binding_sources_counts = 1; },
            [](Fixture& f) { f.table.parameters[0]->parameter_binding_sources_counts = std::numeric_limits<int>::max(); },
            [](Fixture& f) { f.table.parameters[0]->ids.clear(); },
            [](Fixture& f) { f.table.parameters[1]->ids = f.table.parameters[0]->ids; },
            [](Fixture& f) { f.table.parameters[0]->min_values = 200; },
            [](Fixture& f) { f.table.parameters[0]->max_values = std::numeric_limits<float>::infinity(); },
            [](Fixture& f) { f.table.parameters[0]->min_values = std::numeric_limits<float>::quiet_NaN(); },
            [](Fixture& f) { f.table.parameters[0]->min_values = 0.5f; },
            [](Fixture& f) { f.table.parameter_bindings[0]->keys_sources_counts = 0; },
            [](Fixture& f) { f.table.parameter_bindings[0]->keys_sources_begin_indices = -1; },
            [](Fixture& f) { f.table.parameter_bindings[0]->keys_sources_counts = std::numeric_limits<int>::max(); },
            [](Fixture& f) { f.table.keys->values.back() = 0; },
            [](Fixture& f) { f.table.keys->values.back() = -1; },
            [](Fixture& f) { f.table.keys->values.back() = std::numeric_limits<float>::quiet_NaN(); },
            [](Fixture& f) { f.table.keys->values.back() = std::numeric_limits<float>::infinity(); },
            [](Fixture& f) { f.mesh->keyform_binding_sources_indices = -2; },
            [](Fixture& f) { f.mesh->keyform_binding_sources_indices = std::numeric_limits<int>::max(); },
            [](Fixture& f) { f.table.keyform_bindings[0]->parameter_binding_index_sources_begin_indices = -1; },
            [](Fixture& f) { f.table.keyform_bindings[0]->parameter_binding_index_sources_counts = std::numeric_limits<int>::max(); },
            [](Fixture& f) { f.table.parameter_binding_indices[0]->binding_sources_indices = -1; },
            [](Fixture& f) { f.table.parameter_binding_indices[0]->binding_sources_indices = std::numeric_limits<int>::max(); },
            [](Fixture& f) { f.mesh->keyform_sources_counts = 0; },
            [](Fixture& f) { f.mesh->keyform_sources_counts = 3; },
            [](Fixture& f) { f.mesh->keyform_sources_begin_indices = std::numeric_limits<int>::max(); },
            [](Fixture& f) { f.mesh->keyform_sources_counts = std::numeric_limits<int>::max(); },
            [](Fixture& f) { f.table.art_meshes[1]->ids = "target"; },
            [](Fixture& f) { f.Grid(QVector<int>(64, 0), 64); }
        };
        for (size_t index = 0; index < invalid.size(); ++index) {
            Fixture fixture;
            QVERIFY(fixture.table.IsValid());
            fixture.Binding(0, 0, {0, 1});
            fixture.Grid({0}, 2);
            invalid[index](fixture);
            CKeyformBindingGrid grid;
            QString error = "old";
            QVERIFY2(grid.Build(fixture.table, &error), qPrintable(QString::number(index) + ": " + error));
            QVERIFY(error.isEmpty());
            QMap<QString, float> values{{"retained", 9}};
            const auto before = values;
            QVERIFY2(!grid.Pose(CKeyformBindingGrid::ArtMesh, "target", 0, values, &error), qPrintable(QString::number(index)));
            QCOMPARE(values, before);
            QVERIFY(!error.isEmpty());
            QCOMPARE(error, grid.Diagnostic(CKeyformBindingGrid::ArtMesh, "target"));
            QVERIFY(grid.Axes(CKeyformBindingGrid::ArtMesh, "target").isEmpty());
            QVERIFY(grid.Pose(CKeyformBindingGrid::ArtMesh, "unaffected", 0, values));
            QVERIFY(values.isEmpty());
        }
    }

    void unsupportedParameterTypesRemainReadableButCannotProduceAPose()
    {
        Fixture fixture;
        QVERIFY(fixture.table.IsValid());
        fixture.Binding(0, 0, {0, 1});
        fixture.Grid({0}, 2);
        for (int type : {1, -1, 99}) {
            fixture.table.parameters[0]->parameter_types = type;
            CKeyformBindingGrid grid;
            QVERIFY(grid.Build(fixture.table));
            const auto axes = grid.Axes(CKeyformBindingGrid::ArtMesh, "target");
            QCOMPARE(axes.size(), 1);
            QCOMPARE(axes[0].parameterType, type);
            QCOMPARE(axes[0].keys, (QVector<float>{0, 1}));
            QMap<QString, float> values{{"retained", 9}};
            const auto before = values;
            QVERIFY(!grid.Pose(CKeyformBindingGrid::ArtMesh, "target", 0, values));
            QCOMPARE(values, before);
            QVERIFY(grid.Pose(CKeyformBindingGrid::ArtMesh, "unaffected", 0, values));
        }
    }

    void blendShapeTargetsCannotMasqueradeAsUnboundNormalObjects()
    {
        Fixture fixture;
        QVERIFY(fixture.table.IsValid());
        fixture.Grid({}, 1);
        const auto warp = std::find_if(fixture.table.deformers.begin(), fixture.table.deformers.end(),
            [](const CDeformers* source) { return source->types == 0; });
        QVERIFY(warp != fixture.table.deformers.end());
        (*warp)->keyform_binding_sources_indices = -1;
        auto* warpSource = fixture.table.warp_deformers[(*warp)->specific_sources_indices];
        warpSource->keyform_sources_begin_indices = 0;
        warpSource->keyform_sources_counts = 1;
        CKeyformBindingGrid grid;
        QVERIFY(grid.Build(fixture.table));
        QMap<QString, float> values;
        QVERIFY(grid.Pose(CKeyformBindingGrid::ArtMesh, "target", 0, values));
        QVERIFY(grid.Pose(CKeyformBindingGrid::Warp, (*warp)->ids, 0, values));

        const int meshOffset = appendBlendRow(fixture.reader, 0);
        auto meshColumns = *fixture.table.sectionOffsetTable->blend_shapes_art_meshes;
        meshColumns.target_indices = meshOffset;
        meshColumns.blend_shape_keyform_binding_sources_begin_indices = meshOffset + 4;
        meshColumns.blend_shape_keyform_binding_sources_counts = meshOffset + 8;
        fixture.table.blend_shapes_art_meshes.append(new CBlend_shapes_art_meshes(fixture.reader, meshColumns, 0));
        const int warpOffset = appendBlendRow(fixture.reader, (*warp)->specific_sources_indices);
        auto warpColumns = *fixture.table.sectionOffsetTable->blend_shapes_warp_deformers;
        warpColumns.target_indices = warpOffset;
        warpColumns.blend_shape_keyform_binding_sources_begin_indices = warpOffset + 4;
        warpColumns.blend_shape_keyform_binding_sources_counts = warpOffset + 8;
        fixture.table.blend_shapes_warp_deformers.append(new CBlend_shapes_warp_deformers(fixture.reader, warpColumns, 0));
        QVERIFY(fixture.reader.IsValid());
        QVERIFY(grid.Build(fixture.table));
        values = {{"retained", 9}};
        const auto before = values;
        QVERIFY(!grid.Pose(CKeyformBindingGrid::ArtMesh, "target", 0, values));
        QCOMPARE(values, before);
        QVERIFY(!grid.Pose(CKeyformBindingGrid::Warp, (*warp)->ids, 0, values));
        QCOMPARE(values, before);
        QVERIFY(grid.Diagnostic(CKeyformBindingGrid::ArtMesh, "target").contains("BlendShape"));
        QVERIFY(grid.Diagnostic(CKeyformBindingGrid::Warp, (*warp)->ids).contains("BlendShape"));
        QVERIFY(grid.Pose(CKeyformBindingGrid::ArtMesh, "unaffected", 0, values));
    }

    void invalidDeformerReferencesAreReportedPerObject()
    {
        Fixture fixture;
        QVERIFY(fixture.table.IsValid());
        fixture.Grid({}, 1);
        for (int type : {0, 1}) {
            const auto source = std::find_if(fixture.table.deformers.begin(), fixture.table.deformers.end(),
                [type](const CDeformers* value) { return value->types == type; });
            QVERIFY(source != fixture.table.deformers.end());
            (*source)->specific_sources_indices = std::numeric_limits<int>::max();
            CKeyformBindingGrid grid;
            QVERIFY(grid.Build(fixture.table));
            QMap<QString, float> values{{"retained", 9}};
            const auto before = values;
            const auto objectType = type == 0 ? CKeyformBindingGrid::Warp : CKeyformBindingGrid::Rotation;
            QVERIFY(!grid.Pose(objectType, (*source)->ids, 0, values));
            QCOMPARE(values, before);
            QVERIFY(!grid.Diagnostic(objectType, (*source)->ids).isEmpty());
            QVERIFY(grid.Pose(CKeyformBindingGrid::ArtMesh, "unaffected", 0, values));
        }
    }

    void valuesSurviveSourceLifetimeAndInvalidBuilds()
    {
        CKeyformBindingGrid grid;
        {
            Fixture fixture;
            QVERIFY(fixture.table.IsValid());
            fixture.Binding(0, 0, {2, 5});
            fixture.Grid({0}, 2);
            QVERIFY(grid.Build(fixture.table));
            fixture.table.keys->values.back() = 90;
        }
        QMap<QString, float> values;
        QVERIFY(grid.Pose(CKeyformBindingGrid::ArtMesh, "target", 1, values));
        QCOMPARE(values, (QMap<QString, float>{{"P0", 5}}));
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        CReader missing(temporary.filePath("missing.moc3"));
        CSectionTable invalid(missing);
        QString error;
        QVERIFY(!grid.Build(invalid, &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(grid.Pose(CKeyformBindingGrid::ArtMesh, "target", 1, values));
        QCOMPARE(values, (QMap<QString, float>{{"P0", 5}}));
        const auto before = values;
        for (int frame : {-1, 2, std::numeric_limits<int>::max()}) {
            QVERIFY(!grid.Pose(CKeyformBindingGrid::ArtMesh, "target", frame, values));
            QCOMPARE(values, before);
        }
        QVERIFY(!grid.Pose(CKeyformBindingGrid::Warp, "target", 0, values));
        QVERIFY(!grid.Pose(CKeyformBindingGrid::ArtMesh, "unknown", 0, values));
        QVERIFY(!grid.Pose(static_cast<CKeyformBindingGrid::ObjectType>(99), "target", 0, values));
        QCOMPARE(values, before);
        QVERIFY(grid.Axes(CKeyformBindingGrid::ArtMesh, "unknown").isEmpty());
        QVERIFY(!grid.Diagnostic(CKeyformBindingGrid::ArtMesh, "unknown").isEmpty());
    }

    void unusedBindingsReuseOneLargeKeyPool()
    {
        constexpr int count = 10000;
        Fixture fixture;
        QVERIFY(fixture.table.IsValid());
        growRows(fixture.table.parameter_bindings, count);
        growRows(fixture.table.art_mesh_keyforms, count);
        fixture.table.keys->values.resize(count);
        for (int index = 0; index < count; ++index) {
            fixture.table.keys->values[index] = static_cast<float>(index);
            fixture.table.parameter_bindings[index]->keys_sources_begin_indices = 0;
            fixture.table.parameter_bindings[index]->keys_sources_counts = count;
        }
        auto* owner = fixture.table.parameters.front();
        owner->parameter_binding_sources_begin_indices = 0;
        owner->parameter_binding_sources_counts = count;
        owner->min_values = 0;
        owner->max_values = count;
        fixture.Grid({count - 1}, count);
        CKeyformBindingGrid grid;
        QVERIFY(grid.Build(fixture.table));
        fixture.table.keys->values.back() = -999;
        QMap<QString, float> values;
        QVERIFY(grid.Pose(CKeyformBindingGrid::ArtMesh, "target", count - 1, values));
        QCOMPARE(values, (QMap<QString, float>{{"P0", count - 1}}));
        const auto axes = grid.Axes(CKeyformBindingGrid::ArtMesh, "target");
        QCOMPARE(axes.size(), 1);
        QCOMPARE(axes[0].bindingIndex, count - 1);
        QCOMPARE(axes[0].keys.size(), count);
        QCOMPARE(axes[0].keys.back(), float(count - 1));
        QVERIFY(grid.Pose(CKeyformBindingGrid::ArtMesh, "unaffected", 0, values));
    }

    void manyObjectsShareASingletonAxisRange()
    {
        constexpr int count = 4000;
        Fixture fixture;
        QVERIFY(fixture.table.IsValid());
        fixture.Binding(0, 0, {7});
        fixture.Grid(QVector<int>(count, 0), 1);
        growRows(fixture.table.art_meshes, count);
        for (int index = 0; index < count; ++index) {
            auto* mesh = fixture.table.art_meshes[index];
            mesh->ids = QStringLiteral("shared%1").arg(index);
            mesh->keyform_binding_sources_indices = 0;
            mesh->keyform_sources_begin_indices = 0;
            mesh->keyform_sources_counts = 1;
        }
        CKeyformBindingGrid grid;
        QVERIFY(grid.Build(fixture.table));
        for (int index : {0, count / 2, count - 1}) {
            QMap<QString, float> values;
            QVERIFY(grid.Pose(CKeyformBindingGrid::ArtMesh, QStringLiteral("shared%1").arg(index), 0, values));
            QCOMPARE(values, (QMap<QString, float>{{"P0", 7}}));
        }
        const auto axes = grid.Axes(CKeyformBindingGrid::ArtMesh, QStringLiteral("shared%1").arg(count - 1));
        QCOMPARE(axes.size(), count);
        const float* sharedKeys = axes.front().keys.constData();
        for (const auto& axis : axes) {
            QCOMPARE(axis.bindingIndex, 0);
            QCOMPARE(axis.keys, (QVector<float>{7}));
            QVERIFY(axis.keys.constData() == sharedKeys);
        }
    }

    void overlappingKeyPrefixesUseIndependentSliceValidation()
    {
        constexpr int count = 6000;
        Fixture fixture;
        QVERIFY(fixture.table.IsValid());
        growRows(fixture.table.parameter_bindings, count);
        growRows(fixture.table.parameter_binding_indices, count);
        growRows(fixture.table.keyform_bindings, count);
        growRows(fixture.table.art_mesh_keyforms, count);
        growRows(fixture.table.art_meshes, count);
        fixture.table.keys->values.resize(count);
        for (int index = 0; index < count; ++index) {
            fixture.table.keys->values[index] = float(index);
            auto* binding = fixture.table.parameter_bindings[index];
            binding->keys_sources_begin_indices = 0;
            binding->keys_sources_counts = index + 1;
            fixture.table.parameter_binding_indices[index]->binding_sources_indices = index;
            auto* keyformBinding = fixture.table.keyform_bindings[index];
            keyformBinding->parameter_binding_index_sources_begin_indices = index;
            keyformBinding->parameter_binding_index_sources_counts = 1;
            auto* mesh = fixture.table.art_meshes[index];
            mesh->ids = QStringLiteral("prefix%1").arg(index);
            mesh->keyform_binding_sources_indices = index;
            mesh->keyform_sources_begin_indices = 0;
            mesh->keyform_sources_counts = index + 1;
        }
        auto* owner = fixture.table.parameters.front();
        owner->parameter_binding_sources_begin_indices = 0;
        owner->parameter_binding_sources_counts = count;
        owner->min_values = 0;
        owner->max_values = count;
        CKeyformBindingGrid grid;
        QVERIFY(grid.Build(fixture.table));
        for (int index : {0, count / 2, count - 1}) {
            QMap<QString, float> values;
            QVERIFY(grid.Pose(CKeyformBindingGrid::ArtMesh, QStringLiteral("prefix%1").arg(index), index, values));
            QCOMPARE(values, (QMap<QString, float>{{"P0", float(index)}}));
        }
        fixture.table.keys->values[count / 2] = std::numeric_limits<float>::quiet_NaN();
        QVERIFY(grid.Build(fixture.table));
        QMap<QString, float> values;
        QVERIFY(grid.Pose(CKeyformBindingGrid::ArtMesh, "prefix0", 0, values));
        const auto before = values;
        const QString invalid = QStringLiteral("prefix%1").arg(count - 1);
        QVERIFY(!grid.Pose(CKeyformBindingGrid::ArtMesh, invalid, count - 1, values));
        QCOMPARE(values, before);
        QVERIFY(grid.Axes(CKeyformBindingGrid::ArtMesh, invalid).isEmpty());
    }

    void overlappingSingletonPrefixesRemainLazy()
    {
        constexpr int count = 4000;
        Fixture fixture;
        QVERIFY(fixture.table.IsValid());
        fixture.Binding(0, 0, {7});
        fixture.Grid(QVector<int>(count, 0), 1);
        growRows(fixture.table.keyform_bindings, count);
        growRows(fixture.table.art_meshes, count);
        for (int index = 0; index < count; ++index) {
            auto* binding = fixture.table.keyform_bindings[index];
            binding->parameter_binding_index_sources_begin_indices = 0;
            binding->parameter_binding_index_sources_counts = index + 1;
            auto* mesh = fixture.table.art_meshes[index];
            mesh->ids = QStringLiteral("prefix%1").arg(index);
            mesh->keyform_binding_sources_indices = index;
            mesh->keyform_sources_begin_indices = 0;
            mesh->keyform_sources_counts = 1;
        }
        CKeyformBindingGrid grid;
        QVERIFY(grid.Build(fixture.table));
        for (int index : {0, count / 2, count - 1}) {
            QMap<QString, float> values;
            const QString id = QStringLiteral("prefix%1").arg(index);
            QVERIFY(grid.Pose(CKeyformBindingGrid::ArtMesh, id, 0, values));
            QCOMPARE(values, (QMap<QString, float>{{"P0", 7}}));
            QCOMPARE(grid.Axes(CKeyformBindingGrid::ArtMesh, id).size(), index + 1);
        }
    }

    void overlappingOwnerRangesRequireExactlyOneOwner()
    {
        constexpr int bindings = 4096, parameters = 4097;
        Fixture fixture;
        QVERIFY(fixture.table.IsValid());
        growRows(fixture.table.parameter_bindings, bindings);
        growRows(fixture.table.parameters, parameters);
        fixture.table.keys->values = {0};
        for (auto* binding : fixture.table.parameter_bindings) {
            binding->keys_sources_begin_indices = 0;
            binding->keys_sources_counts = 1;
        }
        for (int index = 0; index < parameters; ++index) {
            auto* parameter = fixture.table.parameters[index];
            parameter->ids = QStringLiteral("owner%1").arg(index);
            parameter->min_values = 0;
            parameter->max_values = 1;
            parameter->parameter_binding_sources_begin_indices = 0;
            parameter->parameter_binding_sources_counts = bindings;
        }
        fixture.Grid({bindings - 1}, 1);
        CKeyformBindingGrid grid;
        QVERIFY(grid.Build(fixture.table));
        QMap<QString, float> values{{"retained", 9}};
        const auto before = values;
        QVERIFY(!grid.Pose(CKeyformBindingGrid::ArtMesh, "target", 0, values));
        QCOMPARE(values, before);
        QVERIFY(grid.Axes(CKeyformBindingGrid::ArtMesh, "target").isEmpty());
        QVERIFY(grid.Pose(CKeyformBindingGrid::ArtMesh, "unaffected", 0, values));
        for (int index = 0; index < parameters - 1; ++index)
            fixture.table.parameters[index]->parameter_binding_sources_counts = 0;
        QVERIFY(grid.Build(fixture.table));
        QVERIFY(grid.Pose(CKeyformBindingGrid::ArtMesh, "target", 0, values));
        QCOMPARE(values, (QMap<QString, float>{{QStringLiteral("owner%1").arg(parameters - 1), 0}}));
    }

    void sliceBoundariesExcludeNeighbouringKeyAndReferenceErrors()
    {
        Fixture fixture;
        QVERIFY(fixture.table.IsValid());
        fixture.Binding(0, 0, {0, 1});
        fixture.Binding(1, 1, {7});
        fixture.Binding(2, 2, {9});
        const int begin = fixture.table.parameter_bindings[0]->keys_sources_begin_indices;
        QVERIFY(begin > 0);
        fixture.table.keys->values[begin - 1] = std::numeric_limits<float>::quiet_NaN();
        fixture.table.parameters[2]->parameter_types = 1;
        fixture.Grid({2, 0, 1}, 2);
        fixture.table.keyform_bindings[0]->parameter_binding_index_sources_begin_indices = 1;
        fixture.table.keyform_bindings[0]->parameter_binding_index_sources_counts = 2;
        CKeyformBindingGrid grid;
        for (int neighbour : {2, -1}) {
            fixture.table.parameter_binding_indices[0]->binding_sources_indices = neighbour;
            QVERIFY(grid.Build(fixture.table));
            QMap<QString, float> values;
            QVERIFY(grid.Pose(CKeyformBindingGrid::ArtMesh, "target", 1, values));
            QCOMPARE(values, (QMap<QString, float>{{"P0", 1}, {"P1", 7}}));
            const auto axes = grid.Axes(CKeyformBindingGrid::ArtMesh, "target");
            QCOMPARE(axes.size(), 2);
            QCOMPARE(axes[0].bindingIndex, 0);
            QCOMPARE(axes[1].bindingIndex, 1);
        }
    }
};

QTEST_APPLESS_MAIN(KeyformBindingGridTests)
#include "keyform_binding_grid_tests.moc"
