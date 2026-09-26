#include "live2dstruct/cmocparameterstructurebuilder.h"
#include "live2dstruct/ckeyformbindinggrid.h"
#include "live2dstruct/cmodeldocument.h"
#include "live2dstruct/csectionvalidation.h"
#include "deformer_snapshot_fixture.h"
#include <QtTest>
#include <QtEndian>
#include <algorithm>
#include <cstring>
#include <limits>

using namespace live2dstruct;
using Builder = CMocParameterStructureBuilder;

namespace {
Builder::Owner owner(int type) { return {Builder::Owner::Type(type), type == 0 ? "Mesh" : type == 1 ? "Warp" : "Root"}; }
Builder::BindingKey retained(float value, int index) { return {value, index, Builder::BindingKey::InterpolateOldAxis, -1}; }
Builder::BindingKey inserted(float value, int copy = -1)
{
    return {value, -1, copy < 0 ? Builder::BindingKey::InterpolateOldAxis : Builder::BindingKey::CopyOldSlice, copy};
}
void put(QByteArray& bytes, int offset, int value) { qToLittleEndian(qint32(value), bytes.data() + offset); }
void real(QByteArray& bytes, int offset, float value) { quint32 bits; std::memcpy(&bits, &value, 4); qToLittleEndian(bits, bytes.data() + offset); }
QByteArray words(const QVector<int>& values) { QByteArray bytes(values.size() * 4, '\0'); for (int i = 0; i < values.size(); ++i) put(bytes, i * 4, values[i]); return bytes; }
QByteArray floats(const QVector<float>& values) { QByteArray bytes(values.size() * 4, '\0'); for (int i = 0; i < values.size(); ++i) real(bytes, i * 4, values[i]); return bytes; }

bool fixture(int type, QByteArray& bytes, QString* error, int axes = 3)
{
    DeformerSnapshotData::Fixture source;
    if (!source.table.IsValid()) return false;
    source.column(source.table.sectionOffsetTable->art_meshes->keyform_binding_sources_indices, 0, -1);
    source.column(source.table.sectionOffsetTable->art_meshes->keyform_sources_counts, 0, 1);
    bytes = source.bytes;
    for (int axis = 0; axis < axes; ++axis) {
        Builder::CreateRequest request;
        request.parameter.id = QString("Axis%1").arg(axis);
        request.bindings.append({owner(type), {-1, 1}});
        Builder::Result result;
        if (!Builder::Create(bytes, request, result, error)) return false;
        bytes = result.bytes;
    }
    CModelDocument document;
    if (!document.LoadBytes(bytes, "binding-fixture.moc3", error)) return false;
    if (type == 0) {
        const auto& mesh = document.Drawables().first();
        for (int row = 0; row < mesh.keyforms.size(); ++row) {
            auto positions = mesh.keyforms[row].positions;
            for (int component = 0; component < positions.size(); ++component) positions[component] = row * 4 + component * .125f;
            if (!document.SetDrawableKeyformPositions(mesh.id, row, positions)) return false;
        }
    } else if (type == 1) {
        auto forms = document.DeformerWarpKeyforms("Warp");
        for (int row = 0; row < forms.size(); ++row) {
            forms[row].opacity = .125f + row * .0625f;
            for (int component = 0; component < forms[row].positions.size(); ++component) forms[row].positions[component] = row * 4 + component * .125f;
        }
        if (!document.SetDeformerWarpKeyforms("Warp", forms)) return false;
    } else {
        auto forms = document.DeformerRotationKeyforms("Root");
        for (int row = 0; row < forms.size(); ++row)
            forms[row] = {.125f + row * .0625f, row * 10.0f + .25f, row + .5f, -row - .75f, row - 4.0f, bool(row % 2), bool((row / 2) % 2)};
        if (!document.SetDeformerRotationKeyforms("Root", forms)) return false;
    }
    if (!document.BuildPreviewMoc(bytes, error)) return false;
    if (type == 0) {
        auto reader = CReader::FromBytes(bytes); CSectionTable table(reader);
        if (!table.IsValid()) return false;
        for (int local = 0; local < table.art_meshes[0]->keyform_sources_counts; ++local) {
            const int row = table.art_meshes[0]->keyform_sources_begin_indices + local;
            real(bytes, table.sectionOffsetTable->art_mesh_keyforms->opacities + row * 4, .125f + local * .0625f);
            real(bytes, table.sectionOffsetTable->art_mesh_keyforms->draw_orders + row * 4, 100 + local * 2.0f);
        }
    }
    return true;
}

Builder::EditBindingKeysRequest request(int type, int axis)
{
    return {owner(type), axis, QString("Axis%1").arg(axis), {-1, 1}, {retained(-1, 0), inserted(0), retained(1, 1)}};
}

QVector<float> rowValues(const CModelDocument& document, int type, int row)
{
    if (type == 0) {
        const auto& form = document.Drawables().first().keyforms[row];
        return QVector<float>{form.opacity, form.drawOrder} + form.positions;
    }
    if (type == 1) {
        const auto form = document.DeformerWarpKeyforms("Warp")[row];
        return QVector<float>{form.opacity} + form.positions;
    }
    const auto form = document.DeformerRotationKeyforms("Root")[row];
    return {form.opacity, form.angle, form.originX, form.originY, form.scale};
}
CSectionValidation::ColumnSpan column(const CSectionTable& table, const char* name)
{
    for (const auto& span : CSectionValidation::ArraySpans(*table.sectionOffsetTable, *table.count_info))
        if (QByteArray(span.name) == name) return span;
    return {nullptr, 0, 0, 0, false};
}
float cell(const QByteArray& bytes, qint64 offset)
{
    const quint32 bits = qFromLittleEndian<quint32>(bytes.constData() + offset);
    float value; std::memcpy(&value, &bits, 4); return value;
}
QString keyformGroup(int type) { return type == 0 ? "art_mesh_keyforms" : type == 1 ? "warp_deformer_keyforms" : "rotation_deformer_keyforms"; }
int firstRow(const CSectionTable& table, int type)
{
    return type == 0 ? table.art_meshes[0]->keyform_sources_begin_indices : type == 1
        ? table.warp_deformers[0]->keyform_sources_begin_indices : table.rotation_deformers[0]->keyform_sources_begin_indices;
}
QByteArray v5(const QByteArray& bytes)
{
    auto reader = CReader::FromBytes(bytes); CSectionTable table(reader);
    if (!table.IsValid()) return {};
    QByteArray result(0x3c0, '\0');
    result.replace(0, 0x40, bytes.left(0x40)); result[4] = 5;
    put(result, 0x40, 0x2c0); put(result, 0x44, 0x380);
    result.replace(0x2c0, 128, bytes.mid(table.sectionOffsetTable->count_info->count_info_offset, 128));
    result.replace(0x380, 20, bytes.mid(qFromLittleEndian<qint32>(bytes.constData() + 0x44), 20));
    for (auto span : CSectionValidation::ArraySpans(*table.sectionOffsetTable, *table.count_info)) {
        if (!span.offset || !span.count) continue;
        const QByteArray name(span.name);
        if (name == "parameters.runtime_space_0" || name == "parameters.extension_runtime_space_0") span.width = 8;
        put(result, span.slot, result.size()); result.append(bytes.mid(span.offset, span.count * span.width));
        result.append(QByteArray((64 - result.size() % 64) % 64, '\0'));
    }
    return result;
}
bool coloredBlendFixture(int type, QByteArray& bytes, QString* error)
{
    if (!fixture(type, bytes, error)) return false;
    bytes = v5(bytes);
    auto reader = CReader::FromBytes(bytes); CSectionTable table(reader);
    if (!table.IsValid()) { *error = reader.Error(); return false; }
    Builder::ColumnState state;
    constexpr int colors = 64;
    state.counts.insert(23, colors); state.counts.insert(24, colors);
    const auto set = [&](const QByteArray& name, const QByteArray& data) {
        const auto span = column(table, name.constData());
        Q_ASSERT(span.name); state.columns.insert(span.slot, data);
    };
    for (const QByteArray& pool : {QByteArray("keyform_multiply_colors"), QByteArray("keyform_screen_colors")})
        for (int component = 0; component < 3; ++component) {
            QVector<float> values;
            for (int index = 0; index < colors; ++index) values.append(index / 128.0f + component / 8.0f);
            set(pool + '.' + "rgb"[component], floats(values));
        }
    for (int kind = 0; kind < 3; ++kind) {
        const QByteArray group = kind == 0 ? "art_meshes" : kind == 1 ? "warp_deformers" : "rotation_deformers";
        const auto groupSpan = column(table, (group + ".keyform_sources_counts").constData());
        set(group + ".keyform_color_sources_begin_indices", words(QVector<int>(groupSpan.count, 0)));
        const auto forms = keyformGroup(kind).toUtf8();
        const int count = column(table, (forms + ".opacities").constData()).count;
        QVector<int> multiply, screen;
        for (int row = 0; row < count; ++row) { multiply.append((row * 3 + 2) % colors); screen.append((row * 5 + 1) % colors); }
        set(forms + ".multiply_color_sources_indices", words(multiply));
        set(forms + ".screen_color_sources_indices", words(screen));
    }
    state.counts.insert(25, 1); state.counts.insert(26, 1);
    set("blend_shape_parameter_bindings.keys_sources_begin_indices", words({0}));
    set("blend_shape_parameter_bindings.keys_sources_counts", words({2}));
    set("blend_shape_parameter_bindings.base_key_indices", words({0}));
    set("blend_shape_keyform_bindings.parameter_binding_sources_indices", words({0}));
    set("blend_shape_keyform_bindings.keyform_sources_blend_shape_indices", words({firstRow(table, type)}));
    set("blend_shape_keyform_bindings.keyform_sources_blend_shape_counts", words({2}));
    set("blend_shape_keyform_bindings.blend_shape_constraint_index_sources_begin_indices", words({0}));
    set("blend_shape_keyform_bindings.blend_shape_constraint_index_sources_counts", words({0}));
    const QByteArray target = type == 0 ? "blend_shapes_art_meshes" : type == 1 ? "blend_shapes_warp_deformers" : "blend_shapes_rotation_deformers";
    state.counts.insert(type == 0 ? 28 : type == 1 ? 27 : 33, 1);
    set(target + ".target_indices", words({0}));
    set(target + ".blend_shape_keyform_binding_sources_begin_indices", words({0}));
    set(target + ".blend_shape_keyform_binding_sources_counts", words({1}));
    for (const auto& field : {QByteArray("parameters.parameter_types"), QByteArray("parameters.blend_shape_parameter_binding_sources_begin_indices"),
                              QByteArray("parameters.blend_shape_parameter_binding_sources_counts")}) {
        const auto span = column(table, field.constData()); auto values = bytes.mid(span.offset, span.count * 4);
        put(values, 0, field.endsWith("begin_indices") ? 0 : 1); set(field, values);
    }
    QByteArray result;
    if (!Builder::BuildColumnState(bytes, state, result, error)) return false;
    bytes = result; return true;
}
}

class BindingKeysMocTests : public QObject
{
    Q_OBJECT
private slots:
    void resamplesOnlyTheSelectedTensorAxis_data()
    {
        QTest::addColumn<int>("type"); QTest::addColumn<int>("axis"); QTest::addColumn<QString>("operation");
        for (int type = 0; type < 3; ++type) for (int axis = 0; axis < 3; ++axis)
            for (const QString& operation : {QString("insert"), QString("copy"), QString("move"), QString("delete"), QString("swap"), QString("noop")})
                QTest::newRow(qPrintable(QString("%1-axis%2-%3").arg(type).arg(axis).arg(operation))) << type << axis << operation;
    }
    void resamplesOnlyTheSelectedTensorAxis()
    {
        QFETCH(int, type); QFETCH(int, axis); QFETCH(QString, operation);
        QByteArray bytes; QString error; QVERIFY2(fixture(type, bytes, &error), qPrintable(error));
        const auto immutable = bytes;
        auto edit = request(type, axis);
        QVector<QVector<QPair<int, double>>> sources{{{0, 1}}, {{0, .5}, {1, .5}}, {{1, 1}}};
        if (operation == "copy") { edit.keys[1] = inserted(0, 1); sources[1] = {{1, 1}}; }
        if (operation == "move") { edit.keys = {retained(-.75f, 0), retained(.5f, 1)}; sources = {{{0, 1}}, {{1, 1}}}; }
        if (operation == "delete") { edit.keys = {retained(1, 1)}; sources = {{{1, 1}}}; }
        if (operation == "swap") { edit.keys = {retained(-.75f, 1), retained(.5f, 0)}; sources = {{{1, 1}}, {{0, 1}}}; }
        if (operation == "noop") { edit.keys = {retained(-1, 0), retained(1, 1)}; sources = {{{0, 1}}, {{1, 1}}}; }
        Builder::Result result;
        QVERIFY2(Builder::EditBindingKeys(bytes, edit, result, &error), qPrintable(error));
        QCOMPARE(bytes, immutable);
        if (operation == "noop") QCOMPARE(result.bytes, bytes);
        CModelDocument before, after;
        QVERIFY2(before.LoadBytes(bytes, "before.moc3", &error), qPrintable(error));
        QVERIFY2(after.LoadBytes(result.bytes, "after.moc3", &error), qPrintable(error));
        auto reader = CReader::FromBytes(result.bytes); CSectionTable table(reader);
        CKeyformBindingGrid grid; QVERIFY(grid.Build(table));
        const auto axes = grid.Axes(CKeyformBindingGrid::ObjectType(type), owner(type).id);
        QCOMPARE(axes.size(), 3);
        QVector<float> nextKeys; for (const auto& key : edit.keys) nextKeys.append(key.value);
        for (int index = 0; index < 3; ++index) {
            QCOMPARE(axes[index].parameterId, QString("Axis%1").arg(index));
            QCOMPARE(axes[index].keys, index == axis ? nextKeys : QVector<float>({-1, 1}));
        }
        QCOMPARE(result.keyforms.size(), 1);
        const auto& remap = result.keyforms.first();
        QCOMPARE(remap.oldKeyformCount, 8); QCOMPARE(remap.newKeyformCount, int(edit.keys.size()) * 4);
        QCOMPARE(remap.newLocalSources.size(), remap.newKeyformCount);
        const int stride = 1 << axis;
        for (int local = 0; local < remap.newKeyformCount; ++local) {
            const int digit = (local / stride) % edit.keys.size(), inner = local % stride;
            const int outer = local / (stride * edit.keys.size());
            QVector<float> expected(rowValues(before, type, 0).size(), 0);
            QCOMPARE(remap.newLocalSources[local].size(), sources[digit].size());
            int first = -1;
            for (int term = 0; term < sources[digit].size(); ++term) {
                const auto contribution = sources[digit][term];
                const int old = inner + contribution.first * stride + outer * stride * 2;
                if (!term) first = old;
                QCOMPARE(remap.newLocalSources[local][term].oldLocal, old);
                QCOMPARE(remap.newLocalSources[local][term].weight, contribution.second);
                const auto values = rowValues(before, type, old);
                for (int component = 0; component < values.size(); ++component) expected[component] += float(values[component] * contribution.second);
            }
            QCOMPARE(remap.newLocalToOldLocal[local], sources[digit].size() == 1 ? first : -1);
            QCOMPARE(rowValues(after, type, local), expected);
            if (type == 2) {
                QCOMPARE(after.DeformerRotationKeyforms("Root")[local].reflectX, before.DeformerRotationKeyforms("Root")[first].reflectX);
                QCOMPARE(after.DeformerRotationKeyforms("Root")[local].reflectY, before.DeformerRotationKeyforms("Root")[first].reflectY);
            }
        }
        QCOMPARE(result.oldParameterToNew.size(), before.Parameters().size());
        for (int index = 0; index < result.oldParameterToNew.size(); ++index) QCOMPARE(result.oldParameterToNew[index], index);
        QCOMPARE(before.Parameters().size(), after.Parameters().size());
        for (int index = 0; index < before.Parameters().size(); ++index) {
            const auto& a = before.Parameters()[index]; const auto& b = after.Parameters()[index];
            QCOMPARE(a.id, b.id); QCOMPARE(a.minimum, b.minimum); QCOMPARE(a.maximum, b.maximum); QCOMPARE(a.defaultValue, b.defaultValue);
        }
        Builder::ColumnState state, repeated;
        QVERIFY2(Builder::CaptureColumnState(bytes, result.bytes, state, &error), qPrintable(error));
        QByteArray restored; QVERIFY2(Builder::BuildColumnState(bytes, state, restored, &error), qPrintable(error));
        QVERIFY(Builder::CaptureColumnState(bytes, restored, repeated, &error)); QVERIFY(state == repeated);
        QVERIFY(Builder::BuildColumnState(bytes, {}, restored, &error)); QCOMPARE(restored, bytes);
    }
    void singletonRequiresExplicitCopy()
    {
        QByteArray bytes; QString error; QVERIFY2(fixture(0, bytes, &error), qPrintable(error));
        auto edit = request(0, 1); edit.keys = {retained(-1, 0)};
        Builder::Result singleton; QVERIFY2(Builder::EditBindingKeys(bytes, edit, singleton, &error), qPrintable(error));
        edit.expectedKeys = {-1}; edit.keys = {retained(-1, 0), inserted(0)};
        Builder::Result result; result.bytes = "keep";
        QVERIFY(!Builder::EditBindingKeys(singleton.bytes, edit, result, &error)); QVERIFY(error.contains("CopyOldSlice")); QCOMPARE(result.bytes, QByteArray("keep"));
        edit.keys[1] = inserted(0, 0);
        QVERIFY2(Builder::EditBindingKeys(singleton.bytes, edit, result, &error), qPrintable(error));
        for (int local = 0; local < result.keyforms.first().newKeyformCount; ++local)
            QCOMPARE(result.keyforms.first().newLocalSources[local].size(), 1);
    }
    void invalidRequestsAreAtomic_data()
    {
        QTest::addColumn<int>("mutation");
        for (int index = 0; index < 16; ++index) QTest::newRow(qPrintable(QString::number(index))) << index;
    }
    void invalidRequestsAreAtomic()
    {
        QFETCH(int, mutation);
        QByteArray bytes; QString error; QVERIFY2(fixture(0, bytes, &error), qPrintable(error));
        auto edit = request(0, 1);
        switch (mutation) {
        case 0: edit.owner.id = "missing"; break;
        case 1: edit.owner.type = Builder::Owner::Rotation; break;
        case 2: edit.parameterId = "Axis0"; break;
        case 3: edit.axisIndex = -1; break;
        case 4: edit.axisIndex = 3; break;
        case 5: edit.expectedKeys[0] = -.5f; break;
        case 6: edit.keys.clear(); break;
        case 7: edit.keys[1].value = -1; break;
        case 8: edit.keys[1].value = std::numeric_limits<float>::infinity(); break;
        case 9: edit.keys[1].oldKeyIndex = 99; break;
        case 10: edit.keys[1].oldKeyIndex = 0; break;
        case 11: edit.keys[1] = inserted(0, 99); break;
        case 12: edit.keys[1].copyOldKeyIndex = 0; break;
        case 13: edit.keys[0].value = -2; break;
        case 14: edit.keys[1].source = Builder::BindingKey::NewSource(99); break;
        case 15: edit.keys.resize(250001); break;
        }
        Builder::Result result; result.bytes = "keep"; result.oldParameterToNew = {2, 1};
        result.keyforms.append({owner(2), 4, 9, 3, {2}, {}});
        QVERIFY(!Builder::EditBindingKeys(bytes, edit, result, &error)); QVERIFY(!error.isEmpty());
        QCOMPARE(result.bytes, QByteArray("keep")); QCOMPARE(result.oldParameterToNew, QVector<int>({2, 1}));
        QCOMPARE(result.keyforms.first().newLocalToOldLocal, QVector<int>{2});
    }
    void unchangedOutOfRangeSourceKeysRemainEditable()
    {
        QByteArray bytes; QString error; QVERIFY2(fixture(0, bytes, &error), qPrintable(error));
        auto reader = CReader::FromBytes(bytes); CSectionTable table(reader);
        real(bytes, table.sectionOffsetTable->parameters->min_values + 2 * 4, -.5f);
        real(bytes, table.sectionOffsetTable->parameters->max_values + 2 * 4, .5f);
        Builder::Result result;
        QVERIFY2(Builder::EditBindingKeys(bytes, request(0, 1), result, &error), qPrintable(error));
        auto moved = request(0, 1); moved.keys[0].value = -.75f;
        const auto retainedResult = result.bytes;
        QVERIFY(!Builder::EditBindingKeys(bytes, moved, result, &error)); QCOMPARE(result.bytes, retainedResult);
    }
    void colorsAndBlendSourceRowsRemainIndependent_data()
    {
        QTest::addColumn<int>("type");
        QTest::newRow("artmesh") << 0; QTest::newRow("warp") << 1; QTest::newRow("rotation") << 2;
    }
    void colorsAndBlendSourceRowsRemainIndependent()
    {
        QFETCH(int, type); QByteArray bytes; QString error;
        QVERIFY2(coloredBlendFixture(type, bytes, &error), qPrintable(error));
        auto reader = CReader::FromBytes(bytes); CSectionTable before(reader); QVERIFY(before.IsValid());
        Builder::Result result;
        QVERIFY2(Builder::EditBindingKeys(bytes, request(type, 1), result, &error), qPrintable(error));
        auto afterReader = CReader::FromBytes(result.bytes); CSectionTable after(afterReader); QVERIFY(after.IsValid());
        const auto previousSpans = CSectionValidation::ArraySpans(*before.sectionOffsetTable, *before.count_info);
        for (const auto& span : previousSpans) {
            const QByteArray name(span.name);
            if (!span.offset || (!name.startsWith("blend_") && !name.startsWith(keyformGroup(type).toUtf8() + '.') &&
                !name.startsWith("keyform_positions.") && !name.startsWith("keyform_multiply_colors.") && !name.startsWith("keyform_screen_colors."))) continue;
            const auto next = column(after, span.name);
            QCOMPARE(result.bytes.mid(next.offset, span.count * span.width), bytes.mid(span.offset, span.count * span.width));
        }
        const auto group = keyformGroup(type).toUtf8();
        for (const QByteArray& channel : {QByteArray("multiply"), QByteArray("screen")}) {
            const auto oldIndices = column(before, (group + '.' + channel + "_color_sources_indices").constData());
            const auto newIndices = column(after, (group + '.' + channel + "_color_sources_indices").constData());
            const auto& remap = result.keyforms.first();
            for (int local = 0; local < remap.newKeyformCount; ++local) {
                const int destination = qFromLittleEndian<qint32>(result.bytes.constData() + newIndices.offset + (firstRow(after, type) + local) * 4);
                for (char component : {'r', 'g', 'b'}) {
                    const auto name = "keyform_" + channel + "_colors." + component;
                    const auto oldPool = column(before, name.constData()), newPool = column(after, name.constData());
                    double expected = 0;
                    const int digit = (local / 2) % 3, base = local % 2 + (local / 6) * 4;
                    const QVector<QPair<int, double>> terms = digit == 0 ? QVector<QPair<int, double>>{{base, 1}} : digit == 2
                        ? QVector<QPair<int, double>>{{base + 2, 1}} : QVector<QPair<int, double>>{{base, .5}, {base + 2, .5}};
                    for (const auto& term : terms) {
                        const int source = qFromLittleEndian<qint32>(bytes.constData() + oldIndices.offset + (firstRow(before, type) + term.first) * 4);
                        expected += cell(bytes, oldPool.offset + source * 4) * term.second;
                    }
                    QCOMPARE(cell(result.bytes, newPool.offset + destination * 4), float(expected));
                }
            }
        }
    }
    void sharedBindingsAndKeyformsKeepOtherOwnersAndDropOnlyInactiveDisplayKeys_data()
    {
        QTest::addColumn<bool>("sharedGrid"); QTest::newRow("shared-pb-and-rows") << false; QTest::newRow("shared-grid-and-rows") << true;
    }
    void sharedBindingsAndKeyformsKeepOtherOwnersAndDropOnlyInactiveDisplayKeys()
    {
        QFETCH(bool, sharedGrid);
        QByteArray bytes; QString error; QVERIFY2(fixture(2, bytes, &error), qPrintable(error));
        for (int axis = 0; axis < 3; ++axis) {
            Builder::Result bound;
            QVERIFY2(Builder::BindExistingParameter(bytes, QString("Axis%1").arg(axis), {{{Builder::Owner::Rotation, "Child"}, {-1, 1}}}, bound, &error), qPrintable(error));
            bytes = bound.bytes;
        }
        auto reader = CReader::FromBytes(bytes); CSectionTable table(reader); QVERIFY(table.IsValid());
        put(bytes, table.sectionOffsetTable->rotation_deformers->keyform_sources_begin_indices + 4, table.rotation_deformers[0]->keyform_sources_begin_indices);
        if (sharedGrid) {
            const int binding = table.rotation_deformers[0]->keyform_binding_sources_indices;
            put(bytes, table.sectionOffsetTable->deformers->keyform_binding_sources_indices + 2 * 4, binding);
            put(bytes, table.sectionOffsetTable->rotation_deformers->keyform_binding_sources_indices + 4, binding);
        }
        Builder::ColumnState state;
        const auto keys = column(table, "keys.values");
        state.columns.insert(keys.slot, bytes.mid(keys.offset, keys.count * 4) + floats({-1, .875f, 1}));
        state.counts.insert(keys.countIndex, keys.count + 3);
        for (const char* name : {"parameters.keys_sources_begin_indices", "parameters.keys_sources_counts"}) {
            const auto span = column(table, name); auto values = bytes.mid(span.offset, span.count * 4);
            put(values, 4, QByteArray(name).endsWith("counts") ? 3 : keys.count); state.columns.insert(span.slot, values);
        }
        QByteArray cached; QVERIFY2(Builder::BuildColumnState(bytes, state, cached, &error), qPrintable(error)); bytes = cached;
        CModelDocument before; QVERIFY(before.LoadBytes(bytes, "source.moc3", &error));
        const auto sibling = before.DeformerRotationKeyforms("Child");
        auto edit = request(2, 0); edit.keys = {retained(0, 0)};
        Builder::Result first; QVERIFY2(Builder::EditBindingKeys(bytes, edit, first, &error), qPrintable(error));
        CModelDocument after; QVERIFY(after.LoadBytes(first.bytes, "first.moc3", &error));
        QCOMPARE(after.DeformerRotationKeyforms("Child"), sibling);
        QCOMPARE(after.Parameters()[1].keyValues, QVector<float>({-1, 0, .875f, 1}));
        edit.owner.id = "Child"; edit.keys = {retained(.25f, 0)};
        Builder::Result second; QVERIFY2(Builder::EditBindingKeys(first.bytes, edit, second, &error), qPrintable(error));
        CModelDocument final; QVERIFY(final.LoadBytes(second.bytes, "second.moc3", &error));
        QCOMPARE(final.Parameters()[1].keyValues, QVector<float>({0, .25f, .875f}));
        QCOMPARE(final.DeformerRotationKeyforms("Root"), after.DeformerRotationKeyforms("Root"));
    }
    void partAndGlueBindingsKeepSharedGlobalDisplayValues_data()
    {
        QTest::addColumn<bool>("glue"); QTest::newRow("part") << false; QTest::newRow("glue") << true;
    }
    void partAndGlueBindingsKeepSharedGlobalDisplayValues()
    {
        QFETCH(bool, glue);
        QByteArray bytes; QString error; QVERIFY2(fixture(2, bytes, &error), qPrintable(error));
        auto reader = CReader::FromBytes(bytes); CSectionTable table(reader); QVERIFY(table.IsValid());
        Builder::ColumnState state;
        state.counts.insert(glue ? 20 : 0, 1); state.counts.insert(glue ? 22 : 6, 8);
        for (const auto& span : CSectionValidation::ArraySpans(*table.sectionOffsetTable, *table.count_info)) {
            if (span.countIndex != (glue ? 20 : 0) && span.countIndex != (glue ? 22 : 6)) continue;
            const int count = state.counts.value(span.countIndex);
            QByteArray values(count * span.width, '\0'); const QByteArray name(span.name);
            for (int row = 0; row < count; ++row) {
                if (name.endsWith(".ids")) values.replace(row * 64, 64, QByteArray(glue ? "LiveGlue" : "LivePart").leftJustified(64, '\0'));
                else if (name.endsWith(".keyform_binding_sources_indices")) put(values, row * 4, table.rotation_deformers[0]->keyform_binding_sources_indices);
                else if (name.endsWith(".keyform_sources_counts")) put(values, row * 4, 8);
                else if (name.contains(".parent_")) put(values, row * 4, -1);
                else if (name.endsWith(".is_visible") || name.endsWith(".is_enabled")) put(values, row * 4, 1);
                else if (name.endsWith(".intensities")) real(values, row * 4, .5f);
            }
            state.columns.insert(span.slot, values);
        }
        QByteArray active; QVERIFY2(Builder::BuildColumnState(bytes, state, active, &error), qPrintable(error));
        auto edit = request(2, 0); edit.keys = {retained(.25f, 0)};
        Builder::Result result; QVERIFY2(Builder::EditBindingKeys(active, edit, result, &error), qPrintable(error));
        CModelDocument after; QVERIFY2(after.LoadBytes(result.bytes, "active.moc3", &error), qPrintable(error));
        QCOMPARE(after.Parameters()[1].keyValues, QVector<float>({-1, .25f, 1}));
    }
    void sharedSpecificAndUnknownExtensionsRejectChangesButAllowExactNoop()
    {
        QByteArray bytes; QString error; QVERIFY2(fixture(2, bytes, &error), qPrintable(error));
        auto reader = CReader::FromBytes(bytes); CSectionTable table(reader); QVERIFY(table.IsValid());
        const int binding = table.rotation_deformers[0]->keyform_binding_sources_indices;
        put(bytes, table.sectionOffsetTable->deformers->specific_sources_indices + 2 * 4, 0);
        put(bytes, table.sectionOffsetTable->deformers->keyform_binding_sources_indices + 2 * 4, binding);
        Builder::Result result; result.bytes = "keep";
        auto edit = request(2, 1);
        QVERIFY(!Builder::EditBindingKeys(bytes, edit, result, &error)); QVERIFY(error.contains(QStringLiteral("共享专用"))); QCOMPARE(result.bytes, QByteArray("keep"));
        edit.keys = {retained(-1, 0), retained(1, 1)};
        QVERIFY2(Builder::EditBindingKeys(bytes, edit, result, &error), qPrintable(error)); QCOMPARE(result.bytes, bytes);
        QVERIFY2(fixture(0, bytes, &error), qPrintable(error));
        put(bytes, 0x264, qFromLittleEndian<qint32>(bytes.constData() + 0x174));
        edit = request(0, 1); result.bytes = "keep";
        QVERIFY(!Builder::EditBindingKeys(bytes, edit, result, &error)); QVERIFY(error.contains(QStringLiteral("未解释"))); QCOMPARE(result.bytes, QByteArray("keep"));
        edit.keys = {retained(-1, 0), retained(1, 1)};
        QVERIFY2(Builder::EditBindingKeys(bytes, edit, result, &error), qPrintable(error)); QCOMPARE(result.bytes, bytes);
    }
    void outputPositionBudgetIsCheckedBeforePublishing()
    {
        QByteArray bytes; QString error; QVERIFY2(fixture(0, bytes, &error), qPrintable(error));
        auto reader = CReader::FromBytes(bytes); CSectionTable table(reader);
        const auto positions = column(table, "keyform_positions.xys");
        auto values = bytes.mid(positions.offset, positions.count * 4); values.append(QByteArray((1000000 - positions.count) * 4, '\0'));
        Builder::ColumnState state; state.counts.insert(positions.countIndex, 1000000); state.columns.insert(positions.slot, values);
        QByteArray full; QVERIFY2(Builder::BuildColumnState(bytes, state, full, &error), qPrintable(error));
        Builder::Result result; result.bytes = "keep";
        QVERIFY(!Builder::EditBindingKeys(full, request(0, 1), result, &error));
        QVERIFY(error.contains(QStringLiteral("坐标总量"))); QCOMPARE(result.bytes, QByteArray("keep"));
    }
    void constantFiniteExtremeInterpolationRemainsFinite_data()
    {
        QTest::addColumn<int>("type");
        QTest::addColumn<float>("constant");
        for (int type = 0; type < 3; ++type) {
            QTest::newRow(qPrintable(QString("type%1-maximum").arg(type))) << type << std::numeric_limits<float>::max();
            QTest::newRow(qPrintable(QString("type%1-minimum").arg(type))) << type << -std::numeric_limits<float>::max();
        }
    }
    void constantFiniteExtremeInterpolationRemainsFinite()
    {
        QFETCH(int, type); QFETCH(float, constant);
        QByteArray bytes; QString error; QVERIFY2(fixture(type, bytes, &error, 1), qPrintable(error));
        CModelDocument document; QVERIFY(document.LoadBytes(bytes, "source.moc3", &error));
        auto metadata = document.ParameterMetadata("Axis0"); metadata.maximum = 2;
        QVERIFY(document.SetParameterMetadata("Axis0", metadata));
        if (type == 0) {
            for (int row = 0; row < 2; ++row) QVERIFY(document.SetDrawableKeyformPositions("Mesh", row, QVector<float>(6, constant)));
        } else if (type == 1) {
            auto forms = document.DeformerWarpKeyforms("Warp");
            for (auto& form : forms) form.positions.fill(constant);
            QVERIFY(document.SetDeformerWarpKeyforms("Warp", forms));
        } else {
            auto forms = document.DeformerRotationKeyforms("Root");
            for (auto& form : forms) form.scale = constant;
            QVERIFY(document.SetDeformerRotationKeyforms("Root", forms));
        }
        QVERIFY(document.BuildPreviewMoc(bytes, &error));
        auto edit = request(type, 0); edit.keys = {retained(-1, 0), retained(2, 1)};
        Builder::Result moved; QVERIFY2(Builder::EditBindingKeys(bytes, edit, moved, &error), qPrintable(error));
        edit.expectedKeys = {-1, 2}; edit.keys = {retained(-1, 0), inserted(-.2485f), retained(2, 1)};
        Builder::Result result; QVERIFY2(Builder::EditBindingKeys(moved.bytes, edit, result, &error), qPrintable(error));
        CModelDocument after; QVERIFY(after.LoadBytes(result.bytes, "after.moc3", &error));
        if (type == 0) QCOMPARE(after.DrawableKeyformPositions("Mesh", 1), QVector<float>(6, constant));
        else if (type == 1) QCOMPARE(after.DeformerWarpKeyforms("Warp")[1].positions, QVector<float>(8, constant));
        else QCOMPARE(after.DeformerRotationKeyforms("Root")[1].scale, constant);
        Builder::Result deleted; QVERIFY2(Builder::Delete(moved.bytes, {"Axis0", -.2485f}, deleted, &error), qPrintable(error));
        CModelDocument frozen; QVERIFY(frozen.LoadBytes(deleted.bytes, "frozen.moc3", &error));
        if (type == 0) QCOMPARE(frozen.DrawableKeyformPositions("Mesh", 0), QVector<float>(6, constant));
        else if (type == 1) QCOMPARE(frozen.DeformerWarpKeyforms("Warp")[0].positions, QVector<float>(8, constant));
        else QCOMPARE(frozen.DeformerRotationKeyforms("Root")[0].scale, constant);
    }
    void repeatedParameterAxesAreAddressedByAxisIndex()
    {
        QByteArray bytes; QString error; QVERIFY2(fixture(2, bytes, &error), qPrintable(error));
        auto reader = CReader::FromBytes(bytes); CSectionTable table(reader);
        const int binding = table.rotation_deformers[0]->keyform_binding_sources_indices;
        const int begin = table.keyform_bindings[binding]->parameter_binding_index_sources_begin_indices;
        const int reference = table.parameter_binding_indices[begin]->binding_sources_indices;
        put(bytes, table.sectionOffsetTable->parameter_binding_indices->binding_sources_indices + (begin + 1) * 4, reference);
        CModelDocument before; QVERIFY(before.LoadBytes(bytes, "repeated.moc3", &error));
        auto edit = request(2, 1); edit.parameterId = "Axis0"; edit.keys = {retained(-.5f, 0), retained(.5f, 1)};
        Builder::Result result; QVERIFY2(Builder::EditBindingKeys(bytes, edit, result, &error), qPrintable(error));
        auto afterReader = CReader::FromBytes(result.bytes); CSectionTable afterTable(afterReader);
        CKeyformBindingGrid grid; QVERIFY(grid.Build(afterTable)); const auto axes = grid.Axes(CKeyformBindingGrid::Rotation, "Root");
        QCOMPARE(axes.size(), 3); QCOMPARE(axes[0].parameterId, QString("Axis0")); QCOMPARE(axes[1].parameterId, QString("Axis0"));
        QCOMPARE(axes[0].keys, QVector<float>({-1, 1})); QCOMPARE(axes[1].keys, QVector<float>({-.5f, .5f}));
        CModelDocument after; QVERIFY(after.LoadBytes(result.bytes, "after.moc3", &error));
        QCOMPARE(after.DeformerRotationKeyforms("Root"), before.DeformerRotationKeyforms("Root"));
        QCOMPARE(after.Parameters()[1].keyValues, QVector<float>({-1, -.5f, .5f, 1}));
    }
    void sharedKeySliceWorkIsChargedOnlyOnce_data()
    {
        QTest::addColumn<bool>("partial"); QTest::newRow("exact-shared") << false; QTest::newRow("distinct-partial-overlaps") << true;
    }
    void sharedKeySliceWorkIsChargedOnlyOnce()
    {
        QFETCH(bool, partial);
        QByteArray bytes; QString error; QVERIFY2(fixture(0, bytes, &error), qPrintable(error));
        auto reader = CReader::FromBytes(bytes); CSectionTable table(reader);
        constexpr int copies = 64, keyCount = 16384;
        const auto* parameter = table.parameters[2];
        const int insertion = parameter->parameter_binding_sources_begin_indices + parameter->parameter_binding_sources_counts;
        const auto keys = column(table, "keys.values");
        QVector<float> addedKeys;
        for (int index = 0; index < keyCount + copies; ++index) addedKeys.append(-1.0f + 2.0f * index / (keyCount + copies - 1));
        Builder::ColumnState state;
        state.counts.insert(14, keys.count + addedKeys.size());
        state.columns.insert(keys.slot, bytes.mid(keys.offset, keys.count * 4) + floats(addedKeys));
        for (const char* name : {"parameter_bindings.keys_sources_begin_indices", "parameter_bindings.keys_sources_counts"}) {
            const auto span = column(table, name); auto values = bytes.mid(span.offset, span.count * 4);
            QVector<int> added;
            for (int copy = 0; copy < copies; ++copy) added.append(QByteArray(name).endsWith("counts") ? keyCount : keys.count + (partial ? copy : 0));
            values.insert(insertion * 4, words(added)); state.columns.insert(span.slot, values);
            state.counts.insert(span.countIndex, span.count + copies);
        }
        for (const char* name : {"parameters.parameter_binding_sources_begin_indices", "parameters.parameter_binding_sources_counts"}) {
            const auto span = column(table, name); auto values = bytes.mid(span.offset, span.count * 4);
            if (QByteArray(name).endsWith("counts")) put(values, 2 * 4, parameter->parameter_binding_sources_counts + copies);
            else for (int row = 0; row < span.count; ++row) {
                const int value = qFromLittleEndian<qint32>(values.constData() + row * 4);
                if (row != 2 && value >= insertion) put(values, row * 4, value + copies);
            }
            state.columns.insert(span.slot, values);
        }
        const auto references = column(table, "parameter_binding_indices.binding_sources_indices");
        auto values = bytes.mid(references.offset, references.count * 4);
        for (int row = 0; row < references.count; ++row) {
            const int value = qFromLittleEndian<qint32>(values.constData() + row * 4);
            if (value >= insertion) put(values, row * 4, value + copies);
        }
        state.columns.insert(references.slot, values);
        QByteArray expanded; QVERIFY2(Builder::BuildColumnState(bytes, state, expanded, &error), qPrintable(error));
        Builder::Result result; result.bytes = "keep";
        const bool accepted = Builder::EditBindingKeys(expanded, request(0, 1), result, &error);
        QCOMPARE(accepted, !partial);
        if (partial) { QVERIFY(error.contains(QStringLiteral("扫描总量")) || error.contains(QStringLiteral("引用总量"))); QCOMPARE(result.bytes, QByteArray("keep")); }
        else { QVERIFY(!result.bytes.isEmpty()); QCOMPARE(result.keyforms.first().newKeyformCount, 12); }
    }
    void ordinaryCoverageControlsInterpolationAndDelete_data()
    {
        QTest::addColumn<int>("type"); QTest::addColumn<int>("axis"); QTest::addColumn<float>("value");
        for (int type = 0; type < 3; ++type) for (int axis = 0; axis < 3; ++axis)
            for (float value : {-1.5f, -1.0f, 1.0f, 1.5f})
                QTest::newRow(qPrintable(QString("type%1-axis%2-value%3").arg(type).arg(axis).arg(value))) << type << axis << value;
    }
    void ordinaryCoverageControlsInterpolationAndDelete()
    {
        QFETCH(int, type); QFETCH(int, axis); QFETCH(float, value);
        QByteArray bytes; QString error; QVERIFY2(fixture(type, bytes, &error), qPrintable(error));
        auto reader = CReader::FromBytes(bytes); CSectionTable table(reader); QVERIFY(table.IsValid());
        real(bytes, table.sectionOffsetTable->parameters->min_values + (axis + 1) * 4, -2);
        real(bytes, table.sectionOffsetTable->parameters->max_values + (axis + 1) * 4, 2);
        const auto immutable = bytes;
        const bool outside = value < -1 || value > 1;
        auto edit = request(type, axis);
        if (value < -1) edit.keys = {inserted(value), retained(-1, 0), retained(1, 1)};
        else if (value > 1) edit.keys = {retained(-1, 0), retained(1, 1), inserted(value)};
        else edit.keys[value < 0 ? 0 : 2] = inserted(value);
        Builder::Result result; result.bytes = "keep"; result.oldParameterToNew = {7};
        QCOMPARE(Builder::EditBindingKeys(bytes, edit, result, &error), !outside);
        if (outside) {
            QVERIFY(error.contains("CopyOldSlice")); QVERIFY(error.contains("[-1, 1]"));
            QCOMPARE(result.bytes, QByteArray("keep")); QCOMPARE(result.oldParameterToNew, QVector<int>{7});
            edit.keys[value < 0 ? 0 : 2] = inserted(value, value < 0 ? 0 : 1);
            QVERIFY2(Builder::EditBindingKeys(bytes, edit, result, &error), qPrintable(error));
            const int stride = 1 << axis, digit = value < 0 ? 0 : 2;
            for (int local = 0; local < result.keyforms.first().newKeyformCount; ++local) {
                if ((local / stride) % 3 != digit) continue;
                const auto& terms = result.keyforms.first().newLocalSources[local];
                QCOMPARE(terms.size(), 1); QCOMPARE(terms.first().weight, 1.0);
                QCOMPARE((terms.first().oldLocal / stride) % 2, value < 0 ? 0 : 1);
            }
        }
        Builder::DeleteImpact impact;
        QVERIFY2(Builder::AnalyzeDelete(bytes, {edit.parameterId, value}, impact, &error), qPrintable(error));
        QCOMPARE(impact.blockingReasons.isEmpty(), !outside);
        Builder::Result deleted; deleted.bytes = "keep"; deleted.oldParameterToNew = {8};
        QCOMPARE(Builder::Delete(bytes, {edit.parameterId, value}, deleted, &error), !outside);
        if (outside) {
            const QString detail = impact.blockingReasons.join('\n');
            QVERIFY(detail.contains(owner(type).id)); QVERIFY(detail.contains(edit.parameterId));
            QVERIFY(detail.contains("[-1, 1]")); QVERIFY(detail.contains(QString::number(value)));
            QVERIFY(detail.contains(QStringLiteral("历史姿态")));
            QCOMPARE(deleted.bytes, QByteArray("keep")); QCOMPARE(deleted.oldParameterToNew, QVector<int>{8});
        } else {
            CModelDocument before, after;
            QVERIFY(before.LoadBytes(bytes, "before.moc3", &error)); QVERIFY(after.LoadBytes(deleted.bytes, "after.moc3", &error));
            const int stride = 1 << axis;
            for (int local = 0; local < 4; ++local) {
                const int previous = local % stride + (value < 0 ? 0 : stride) + (local / stride) * stride * 2;
                QCOMPARE(rowValues(after, type, local), rowValues(before, type, previous));
            }
        }
        QCOMPARE(bytes, immutable);
    }
    void deleteSingletonOnlyAcceptsItsExactKey_data()
    {
        QTest::addColumn<int>("type"); QTest::addColumn<float>("value");
        for (int type = 0; type < 3; ++type) for (float value : {0.0f, .25f, .5f})
            QTest::newRow(qPrintable(QString("type%1-value%2").arg(type).arg(value))) << type << value;
    }
    void deleteSingletonOnlyAcceptsItsExactKey()
    {
        QFETCH(int, type); QFETCH(float, value);
        QByteArray bytes; QString error; QVERIFY2(fixture(type, bytes, &error), qPrintable(error));
        auto edit = request(type, 1); edit.keys = {retained(.25f, 0)};
        Builder::Result singleton; QVERIFY2(Builder::EditBindingKeys(bytes, edit, singleton, &error), qPrintable(error));
        Builder::DeleteImpact impact;
        QVERIFY2(Builder::AnalyzeDelete(singleton.bytes, {"Axis1", value}, impact, &error), qPrintable(error));
        QCOMPARE(impact.blockingReasons.isEmpty(), value == .25f);
        Builder::Result result; result.bytes = "keep";
        QCOMPARE(Builder::Delete(singleton.bytes, {"Axis1", value}, result, &error), value == .25f);
        if (value != .25f) { QCOMPARE(result.bytes, QByteArray("keep")); QVERIFY(impact.blockingReasons.join('\n').contains("[0.25, 0.25]")); }
    }
    void deleteRequiresTheIntersectionOfAllAffectedOwnerIntervals_data()
    {
        QTest::addColumn<float>("value"); QTest::addColumn<QString>("blockedOwner");
        QTest::newRow("outside-warp") << -.4f << QString("Warp");
        QTest::newRow("outside-mesh") << .6f << QString("Mesh");
        QTest::newRow("lower-common-endpoint") << -.25f << QString();
        QTest::newRow("upper-common-endpoint") << .5f << QString();
        QTest::newRow("inside-common-interval") << 0.0f << QString();
    }
    void deleteRequiresTheIntersectionOfAllAffectedOwnerIntervals()
    {
        QFETCH(float, value); QFETCH(QString, blockedOwner);
        QByteArray bytes; QString error; QVERIFY2(fixture(2, bytes, &error), qPrintable(error));
        Builder::Result bound;
        QVERIFY2(Builder::BindExistingParameter(bytes, "Axis0", {{owner(0), {-.5f, .5f}}, {owner(1), {-.25f, .75f}}}, bound, &error), qPrintable(error));
        Builder::DeleteImpact impact;
        QVERIFY2(Builder::AnalyzeDelete(bound.bytes, {"Axis0", value}, impact, &error), qPrintable(error));
        QCOMPARE(impact.targets.size(), 3); QCOMPARE(impact.blockingReasons.isEmpty(), blockedOwner.isEmpty());
        Builder::Result result; result.bytes = "keep";
        QCOMPARE(Builder::Delete(bound.bytes, {"Axis0", value}, result, &error), blockedOwner.isEmpty());
        if (!blockedOwner.isEmpty()) {
            QCOMPARE(result.bytes, QByteArray("keep"));
            const auto reasons = impact.blockingReasons.join('\n'); QVERIFY(reasons.contains(blockedOwner));
            QVERIFY(reasons.contains("Axis0")); QVERIFY(!reasons.contains("Root"));
        }
    }
    void unreferencedDeleteAndBlendConstraintRangeAreUnaffected()
    {
        QByteArray bytes; QString error; QVERIFY2(fixture(0, bytes, &error), qPrintable(error));
        auto reader = CReader::FromBytes(bytes); CSectionTable table(reader);
        real(bytes, table.sectionOffsetTable->parameters->max_values, 2);
        Builder::DeleteImpact impact; Builder::Result unreferenced;
        QVERIFY(Builder::AnalyzeDelete(bytes, {"P", 1.5f}, impact, &error)); QVERIFY(impact.targets.isEmpty()); QVERIFY(impact.blockingReasons.isEmpty());
        QVERIFY2(Builder::Delete(bytes, {"P", 1.5f}, unreferenced, &error), qPrintable(error));
        Builder::ColumnState state;
        state.counts = {{30, 1}, {31, 2}};
        state.columns = {{0x250, words({0})}, {0x254, words({0})}, {0x258, words({2})},
                         {0x25c, floats({0, .5f})}, {0x260, floats({.25f, .75f})}};
        QByteArray constrained;
        QVERIFY2(Builder::BuildColumnState(bytes, state, constrained, &error), qPrintable(error));
        QVERIFY(Builder::AnalyzeDelete(constrained, {"P", 1.5f}, impact, &error)); QVERIFY(impact.targets.isEmpty()); QVERIFY(impact.blockingReasons.isEmpty());
        Builder::Result result; QVERIFY2(Builder::Delete(constrained, {"P", 1.5f}, result, &error), qPrintable(error));
        auto afterReader = CReader::FromBytes(result.bytes); CSectionTable after(afterReader); QVERIFY(after.IsValid());
        const auto* constraint = after.blend_shape_constraints.first();
        for (int index = 0; index < constraint->blend_shape_constraint_value_sources_counts; ++index)
            QCOMPARE(after.blend_shape_constraint_values[constraint->blend_shape_constraint_value_sources_begin_indices + index]->weights, .75f);
    }
};
QTEST_GUILESS_MAIN(BindingKeysMocTests)
#include "binding_keys_moc_tests.moc"
