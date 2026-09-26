#include "live2dstruct/cmocparameterstructurebuilder.h"
#include "live2dstruct/csectiontable.h"
#include "live2dstruct/csectionvalidation.h"
#include "live2dstruct/ckeyformbindinggrid.h"
#include "parameter_delete_fixture.h"
#include <QtTest>
#include <QFile>
#include <QtEndian>
#include <algorithm>
#include <cstring>
#include <limits>

using namespace live2dstruct;
using Builder = CMocParameterStructureBuilder;

namespace {
QByteArray source(const QString& name) { QFile f(QString(TEST_RESOURCES) + '/' + name + '/' + name + ".moc3"); return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray(); }
void put(QByteArray& bytes, int offset, int value) { qToLittleEndian(qint32(value), bytes.data() + offset); }
QByteArray words(std::initializer_list<int> values) { QByteArray b(values.size() * 4, '\0'); int i = 0; for (int v : values) put(b, i++ * 4, v); return b; }
QByteArray floats(std::initializer_list<float> values) { QByteArray b(values.size() * 4, '\0'); int i = 0; for (float v : values) { quint32 bits; std::memcpy(&bits, &v, 4); qToLittleEndian(bits, b.data() + i++ * 4); } return b; }
}

class ParameterDeleteTests : public QObject
{
    Q_OBJECT
private slots:
    void deletesBoundParameterAndKeepsObjectDirectory_data()
    {
        QTest::addColumn<QString>("model"); QTest::addColumn<QString>("parameter"); QTest::addColumn<float>("freeze");
        for (const char* model : {"Mark", "Rice", "Natori", "Wanko"}) for (float freeze : {0.0f, 7.5f})
            QTest::newRow(qPrintable(QStringLiteral("%1-%2").arg(model).arg(freeze))) << QString::fromLatin1(model)
                << (QString::fromLatin1(model) == QLatin1String("Wanko") ? QStringLiteral("PARAM_ANGLE_X") : QStringLiteral("ParamAngleX")) << freeze;
        for (const char* parameter : {"ParamArmAL04", "ParamArmAR04", "ParamArmCR03"})
            QTest::newRow(parameter) << QStringLiteral("Natori") << QString::fromLatin1(parameter) << 0.0f;
    }

    void deletesBoundParameterAndKeepsObjectDirectory()
    {
        QFETCH(QString, model); QFETCH(QString, parameter); QFETCH(float, freeze);
        const auto bytes = source(model);
        auto reader = CReader::FromBytes(bytes); CSectionTable before(reader);
        QVERIFY(before.IsValid());
        Builder::DeleteImpact impact;
        Builder::Result result;
        QString error;
        QVERIFY2(Builder::AnalyzeDelete(bytes, {parameter, freeze}, impact, &error), qPrintable(error));
        QVERIFY2(impact.blockingReasons.isEmpty(), qPrintable(impact.blockingReasons.join('\n')));
        QVERIFY(!impact.targets.isEmpty());
        QVERIFY2(Builder::Delete(bytes, {parameter, freeze}, result, &error), qPrintable(error));
        QVERIFY(result.keyforms.isEmpty());
        QCOMPARE(result.affectedTargets.size(), impact.targets.size());
        auto updatedReader = CReader::FromBytes(result.bytes); CSectionTable after(updatedReader);
        QVERIFY(after.IsValid());
        QCOMPARE(after.parameters.size(), before.parameters.size() - 1);
        int removed = -1;
        for (int index = 0; index < before.parameters.size(); ++index) {
            if (before.parameters[index]->ids == parameter) { removed = index; QCOMPARE(result.oldParameterToNew[index], -1); continue; }
            const int next = result.oldParameterToNew[index];
            QVERIFY(next >= 0 && next < after.parameters.size());
            QCOMPARE(after.parameters[next]->ids, before.parameters[index]->ids);
            QCOMPARE(after.parameters[next]->min_values, before.parameters[index]->min_values);
            QCOMPARE(after.parameters[next]->max_values, before.parameters[index]->max_values);
            QCOMPARE(after.parameters[next]->default_values, before.parameters[index]->default_values);
        }
        QVERIFY(removed >= 0);
        const auto oldSpans = CSectionValidation::ArraySpans(*before.sectionOffsetTable, *before.count_info);
        const auto newSpans = CSectionValidation::ArraySpans(*after.sectionOffsetTable, *after.count_info);
        for (int index = 0; index < oldSpans.size(); ++index) {
            const auto& old = oldSpans[index]; const auto& updated = newSpans[index]; const QByteArray name(old.name);
            const bool directory = name.endsWith(".ids") && !name.startsWith("parameters.");
            const bool unchanged = directory || name.startsWith("uvs.") || name.startsWith("position_indices.") ||
                name.startsWith("glue_info.") || name == "glue.art_mesh_indices_a" || name == "glue.art_mesh_indices_b" ||
                name.contains("parent_part_indices") || name.contains("parent_deformer_indices");
            if (unchanged) {
                QCOMPARE(updated.count, old.count);
                QCOMPARE(result.bytes.mid(updated.offset, updated.count * updated.width), bytes.mid(old.offset, old.count * old.width));
            }
        }
        for (const auto& target : impact.targets) {
            QVERIFY(target.newKeyformCount <= target.oldKeyformCount);
            if (parameter.startsWith("ParamArmA")) QVERIFY(std::any_of(impact.targets.cbegin(), impact.targets.cend(), [](const auto& t) { return t.type == Builder::DeleteTarget::Part; }));
            if (parameter == QLatin1String("ParamArmCR03")) QVERIFY(std::any_of(impact.targets.cbegin(), impact.targets.cend(), [](const auto& t) { return t.type == Builder::DeleteTarget::Glue; }));
        }
        CKeyformBindingGrid grid; QVERIFY(grid.Build(after));
        for (const auto& target : impact.targets) {
            if (target.type > Builder::DeleteTarget::Rotation || target.id.isEmpty()) continue;
            for (const auto& axis : grid.Axes(CKeyformBindingGrid::ObjectType(target.type), target.id)) QVERIFY(axis.parameterId != parameter);
        }
        Builder::ColumnState state, repeated;
        QVERIFY(Builder::CaptureColumnState(bytes, result.bytes, state, &error));
        QByteArray rebuilt;
        QVERIFY(Builder::BuildColumnState(bytes, state, rebuilt, &error));
        QVERIFY(Builder::CaptureColumnState(bytes, rebuilt, repeated, &error));
        QVERIFY(state == repeated);
        QByteArray restored;
        QVERIFY(Builder::BuildColumnState(bytes, {}, restored, &error));
        QCOMPARE(restored, bytes);
        QCOMPARE(source(model), bytes);
    }

    void rejectsInvalidInputsAtomically()
    {
        const auto bytes = source("Mark");
        Builder::DeleteImpact impact;
        impact.blockingReasons = {QStringLiteral("sentinel")};
        Builder::Result result; result.bytes = "sentinel";
        QString error;
        for (const auto& request : {Builder::DeleteRequest{QStringLiteral("missing"), 0},
                                   Builder::DeleteRequest{QStringLiteral("ParamAngleX"), std::numeric_limits<float>::quiet_NaN()},
                                   Builder::DeleteRequest{QStringLiteral("ParamAngleX"), 31}}) {
            QVERIFY(!Builder::AnalyzeDelete(bytes, request, impact, &error));
            QCOMPARE(impact.blockingReasons, QStringList{QStringLiteral("sentinel")});
            QVERIFY(!Builder::Delete(bytes, request, result, &error));
            QCOMPARE(result.bytes, QByteArray("sentinel"));
        }
        auto unknown = bytes;
        put(unknown, 0x264, qFromLittleEndian<qint32>(unknown.constData() + 0x174));
        QVERIFY(!Builder::Delete(unknown, {QStringLiteral("ParamAngleX"), 0}, result, &error));
        QCOMPARE(result.bytes, QByteArray("sentinel"));
        auto duplicate = bytes;
        const int ids = qFromLittleEndian<qint32>(duplicate.constData() + 0x108);
        duplicate.replace(ids + 64, 64, duplicate.mid(ids, 64));
        QVERIFY(!Builder::AnalyzeDelete(duplicate, {QStringLiteral("ParamAngleX"), 0}, impact, &error));
        QVERIFY(error.contains(QStringLiteral("重复")));
        QCOMPARE(impact.blockingReasons, QStringList{QStringLiteral("sentinel")});
        QVERIFY(!Builder::Delete(duplicate, {QStringLiteral("ParamAngleX"), 0}, result, &error));
        QCOMPARE(result.bytes, QByteArray("sentinel"));
    }

    void repeatedParameterAxesContractAllDimensions()
    {
        auto bytes = source("Mark");
        auto reader = CReader::FromBytes(bytes); CSectionTable table(reader);
        CKeyformBindingGrid grid; QVERIFY(grid.Build(table));
        int selected = -1;
        QVector<CKeyformBindingGrid::Axis> axes;
        for (int index = 0; index < table.art_meshes.size(); ++index) {
            const auto candidate = grid.Axes(CKeyformBindingGrid::ArtMesh, table.art_meshes[index]->ids);
            if (candidate.size() == 2 && candidate[0].keys == candidate[1].keys && candidate[0].keys.size() == 3) { selected = index; axes = candidate; break; }
        }
        QVERIFY(selected >= 0);
        const auto* mesh = table.art_meshes[selected];
        const auto* binding = table.keyform_bindings[mesh->keyform_binding_sources_indices];
        put(bytes, table.sectionOffsetTable->parameter_binding_indices->binding_sources_indices +
            (binding->parameter_binding_index_sources_begin_indices + 1) * 4, axes[0].bindingIndex);
        const float value = (axes[0].keys[1] + axes[0].keys[2]) * .5f;
        Builder::Result result; QString error;
        QVERIFY2(Builder::Delete(bytes, {axes[0].parameterId, value}, result, &error), qPrintable(error));
        auto updatedReader = CReader::FromBytes(result.bytes); CSectionTable updated(updatedReader);
        const auto* current = updated.art_meshes[selected];
        QCOMPARE(current->keyform_sources_counts, 1);
        const auto* empty = updated.keyform_bindings[current->keyform_binding_sources_indices];
        QCOMPARE(empty->parameter_binding_index_sources_counts, 0);
        const auto* output = updated.art_mesh_keyforms[current->keyform_sources_begin_indices];
        for (int coordinate = 0; coordinate < mesh->vertex_counts * 2; ++coordinate) {
            double expected = 0;
            for (int local : {4, 5, 7, 8}) {
                const auto* input = table.art_mesh_keyforms[mesh->keyform_sources_begin_indices + local];
                expected += table.keyform_positions->GetData(input->keyform_position_sources_begin_indices + coordinate) * .25;
            }
            const float actual = updated.keyform_positions->GetData(output->keyform_position_sources_begin_indices + coordinate);
            QVERIFY(std::abs(double(actual) - expected) <= 1e-5 + 1e-6 * std::abs(expected));
        }
    }

    void reflectionUsesLowerCorner_data()
    {
        QTest::addColumn<int>("pattern"); QTest::addColumn<bool>("fast");
        for (int pattern : {0, 1}) for (bool fast : {false, true})
            QTest::newRow(qPrintable(QStringLiteral("%1-%2").arg(pattern).arg(fast))) << pattern << fast;
    }

    void reflectionUsesLowerCorner()
    {
        QFETCH(int, pattern); QFETCH(bool, fast);
        ParameterDeleteFixture::Reflection fixture;
        QString error;
        QVERIFY(ParameterDeleteFixture::MakeReflection(source("Mark"), pattern, fixture, &error));
        Builder::Result result;
        QVERIFY2(Builder::Delete(fixture.bytes, {fast ? fixture.fastParameter : fixture.parameter, fast ? 3.7f : .25f}, result, &error), qPrintable(error));
        auto beforeReader = CReader::FromBytes(fixture.bytes), afterReader = CReader::FromBytes(result.bytes);
        CSectionTable before(beforeReader), after(afterReader);
        QVERIFY(before.IsValid() && after.IsValid());
        int selected = -1;
        for (int index = 0; index < before.deformers.size(); ++index) if (before.deformers[index]->ids == fixture.owner) selected = index;
        QVERIFY(selected >= 0);
        const auto* previous = before.rotation_deformers[before.deformers[selected]->specific_sources_indices];
        const auto* current = after.rotation_deformers[after.deformers[selected]->specific_sources_indices];
        QCOMPARE(current->keyform_sources_counts, 3);
        for (int index = 0; index < 3; ++index) {
            const auto* a = before.rotation_deformer_keyforms[previous->keyform_sources_begin_indices + (fast ? 1 + index * 3 : 3 + index)];
            const auto* b = after.rotation_deformer_keyforms[current->keyform_sources_begin_indices + index];
            QCOMPARE(b->is_reflect_x, a->is_reflect_x);
            QCOMPARE(b->is_reflect_y, a->is_reflect_y);
        }
    }

    void unsupportedReflectionAndOffKeySingletonPreserveOutput()
    {
        QString error;
        ParameterDeleteFixture::Reflection reflected;
        QVERIFY(ParameterDeleteFixture::MakeReflection(source("Mark"), 2, reflected, &error));
        QByteArray singleton;
        QVERIFY(ParameterDeleteFixture::MakeSingleton(source("Mark"), QStringLiteral("ParamAngleX"), 0, singleton, &error));
        for (const auto& test : {qMakePair(reflected.bytes, Builder::DeleteRequest{reflected.parameter, .25f}),
                                qMakePair(singleton, Builder::DeleteRequest{QStringLiteral("ParamAngleX"), 7.5f})}) {
            Builder::DeleteImpact impact;
            QVERIFY(Builder::AnalyzeDelete(test.first, test.second, impact, &error));
            QVERIFY(!impact.blockingReasons.isEmpty());
            Builder::Result result; result.bytes = "sentinel";
            QVERIFY(!Builder::Delete(test.first, test.second, result, &error));
            QCOMPARE(result.bytes, QByteArray("sentinel"));
        }
    }

    void freezesLimiterAsConstantWithoutChangingOtherParameters()
    {
        auto bytes = source("Mark");
        Builder::ColumnState state;
        state.counts = {{30, 1}, {31, 2}};
        state.columns = {{0x250, words({0})}, {0x254, words({0})}, {0x258, words({2})},
                         {0x25c, floats({-30, 30})}, {0x260, floats({0, 1})}};
        QString error;
        QByteArray fixture;
        QVERIFY2(Builder::BuildColumnState(bytes, state, fixture, &error), qPrintable(error));
        Builder::DeleteImpact impact;
        Builder::Result result;
        QVERIFY(Builder::AnalyzeDelete(fixture, {QStringLiteral("ParamAngleX"), 0}, impact, &error));
        QVERIFY(impact.blockingReasons.isEmpty());
        QVERIFY(std::any_of(impact.references.cbegin(), impact.references.cend(), [](const auto& r) { return r.kind == QLatin1String("constantLimiter"); }));
        QVERIFY2(Builder::Delete(fixture, {QStringLiteral("ParamAngleX"), 0}, result, &error), qPrintable(error));
        auto reader = CReader::FromBytes(result.bytes); CSectionTable table(reader);
        QVERIFY(table.IsValid());
        const auto* constraint = table.blend_shape_constraints.first();
        QCOMPARE(table.parameters[constraint->parameter_indices]->ids, QStringLiteral("ParamAngleY"));
        QCOMPARE(constraint->blend_shape_constraint_value_sources_counts, 2);
        for (int index = 0; index < 2; ++index)
            QCOMPARE(table.blend_shape_constraint_values[constraint->blend_shape_constraint_value_sources_begin_indices + index]->weights, .5f);
    }
};

QTEST_APPLESS_MAIN(ParameterDeleteTests)
#include "parameter_delete_tests.moc"
