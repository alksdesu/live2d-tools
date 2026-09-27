#include "live2dstruct/cparameterselectionquery.h"
#include "live2dstruct/cmodeldocument.h"
#include "live2dstruct/csectiontable.h"
#include "deformer_snapshot_fixture.h"
#include <QtTest>
#include <QTemporaryDir>
#include <limits>

using namespace live2dstruct;
using Query = CParameterSelectionQuery;
namespace {
template<class T> T* record()
{
    auto reader = CReader::FromBytes(QByteArray(256, '\0'));
    const typename T::Offset offsets(reader);
    return new T(reader, offsets, 0);
}
struct Fixture {
    DeformerSnapshotData::Fixture source;
    CReader reader = CReader::FromBytes(source.bytes);
    CSectionTable table{reader};
    int ordinary(const QString& id) {
        auto* parameter = new CParameters(*table.parameters[0]);
        parameter->ids = id; parameter->parameter_types = 0;
        parameter->parameter_binding_sources_begin_indices = table.parameter_bindings.size(); parameter->parameter_binding_sources_counts = 1;
        table.parameters.append(parameter);
        table.parameter_bindings.append(new CParameter_bindings(*table.parameter_bindings[0]));
        auto* reference = new CParameter_binding_indices(*table.parameter_binding_indices[0]);
        reference->binding_sources_indices = table.parameter_bindings.size() - 1; table.parameter_binding_indices.append(reference);
        auto* binding = new CKeyform_bindings(*table.keyform_bindings[0]);
        binding->parameter_binding_index_sources_begin_indices = table.parameter_binding_indices.size() - 1;
        binding->parameter_binding_index_sources_counts = 1; table.keyform_bindings.append(binding);
        return table.keyform_bindings.size() - 1;
    }
    void blend(const QString& type) {
        auto* parameter = new CParameters(*table.parameters[0]); parameter->ids = "Driver"; parameter->parameter_types = 1;
        parameter->parameter_binding_sources_counts = 0;
        parameter->blend_shape_parameter_binding_sources_begin_indices = 0; parameter->blend_shape_parameter_binding_sources_counts = 1;
        table.parameters.append(parameter);
        auto* axis = record<CBlend_shape_parameter_bindings>(); axis->keys_sources_counts = 2; table.blend_shape_parameter_bindings.append(axis);
        auto* binding = record<CBlend_shape_keyform_bindings>(); binding->keyform_sources_blend_shape_counts = 1;
        binding->blend_shape_constraint_index_sources_counts = 1; table.blend_shape_keyform_bindings.append(binding);
        auto* constraint = record<CBlend_shape_constraints>(); constraint->parameter_indices = 0; constraint->blend_shape_constraint_value_sources_counts = 2;
        table.blend_shape_constraints.append(constraint); table.blend_shape_constraint_indices.append(record<CBlend_shape_constraint_indices>());
        for (int at = 0; at < 2; ++at) { auto* value = record<CBlend_shape_constraint_values>(); value->keys = value->weights = at; table.blend_shape_constraint_values.append(value); }
        if (type == "artmesh") { auto* shape = record<CBlend_shapes_art_meshes>(); shape->blend_shape_keyform_binding_sources_counts = 1; table.blend_shapes_art_meshes.append(shape); }
        if (type == "warp") { auto* shape = record<CBlend_shapes_warp_deformers>(); shape->blend_shape_keyform_binding_sources_counts = 1; table.blend_shapes_warp_deformers.append(shape); }
        if (type == "rotation") { auto* shape = record<CBlend_shape_targets>(); shape->blend_shape_keyform_binding_sources_counts = 1; table.blend_shapes_rotation_deformers.append(shape); }
        if (type == "part") {
            auto* part = record<CParts>(); part->ids = "Part"; part->parent_part_indices = -1; part->keyform_binding_sources_indices = -1;
            table.parts.append(part); table.part_keyforms.append(record<CPart_keyforms>());
            auto* shape = record<CBlend_shape_targets>(); shape->blend_shape_keyform_binding_sources_counts = 1; table.blend_shapes_parts.append(shape);
        }
    }
};
}

class ParameterSelectionQueryTests : public QObject
{
    Q_OBJECT
private slots:
    void completelyEmptyOrdinaryPoolsKeepUnboundPartsAndObjects()
    {
        QFile file(DeformerSnapshotData::sample("Mark")); QVERIFY(file.open(QIODevice::ReadOnly)); auto bytes = file.readAll();
        const int countOffset = DeformerSnapshotData::integer(bytes, 0x40);
        CReader originalReader = CReader::FromBytes(bytes); CSectionTable original(originalReader); QVERIFY(original.IsValid());
        const auto put = [&](int slot, int row, int value) {
            DeformerSnapshotData::put(bytes, DeformerSnapshotData::integer(bytes, slot) + row * 4, value);
        };
        for (int count : {11, 12, 13, 14}) DeformerSnapshotData::put(bytes, countOffset + count * 4, 0);
        for (int row = 0; row < original.parameters.size(); ++row) { put(0x120, row, row % 2 ? -1 : 0); put(0x124, row, 0); }
        for (int row = 0; row < original.deformers.size(); ++row) put(0x70, row, -1);
        for (int row = 0; row < original.art_meshes.size(); ++row) put(0xc8, row, -1);
        CReader reader = CReader::FromBytes(bytes); CSectionTable table(reader); QVERIFY(table.IsValid());
        QVERIFY(!table.parts.isEmpty()); QCOMPARE(table.parts[0]->keyform_binding_sources_indices, 0);
        QCOMPARE(table.parts[0]->keyform_sources_counts, 1);
        Query query; QString error; QVERIFY2(query.Build(table, &error), qPrintable(error));
        QVector<Query::Target> targets;
        for (const auto* p : table.parts) targets.append({"part", p->ids});
        for (const auto* d : table.deformers) targets.append({d->types == 0 ? "warp" : "rotation", d->ids});
        for (const auto* m : table.art_meshes) targets.append({"artmesh", m->ids});
        Query::Result result; QVERIFY(query.Query(targets, result)); QVERIFY(result.all.isEmpty()); QVERIFY(result.ordinary.isEmpty());
        CModelDocument document; QVERIFY2(document.LoadBytes(bytes, "empty.moc3", &error), qPrintable(error));
        QVERIFY2(query.Build(document, &error), qPrintable(error)); QVERIFY(query.Query(targets, result)); QVERIFY(result.all.isEmpty());
        table.parts[0]->keyform_binding_sources_indices = -1;
        QVERIFY(query.Build(table)); QVERIFY(query.Query(targets, result)); QVERIFY(result.all.isEmpty());
    }

    void bindingSentinelsAndNonemptyBoundsRemainStrict_data()
    {
        QTest::addColumn<QString>("type"); QTest::addColumn<bool>("empty"); QTest::addColumn<int>("binding");
        for (const auto& type : {"part", "artmesh", "warp", "rotation"})
            for (bool empty : {false, true}) for (int binding : {-2, -1, 0, 1})
                QTest::newRow(qPrintable(QString("%1-%2-%3").arg(type).arg(empty).arg(binding))) << QString(type) << empty << binding;
    }
    void bindingSentinelsAndNonemptyBoundsRemainStrict()
    {
        QFETCH(QString, type); QFETCH(bool, empty); QFETCH(int, binding);
        Fixture f; Query query; QVERIFY(query.Build(f.table)); Query::Result before;
        QVERIFY(query.Query({{"artmesh", "Mesh"}}, before)); QCOMPARE(before.all, (QStringList{"P"}));
        auto* part = record<CParts>(); part->ids = "Part"; part->parent_part_indices = -1;
        part->keyform_binding_sources_indices = -1; f.table.parts.append(part);
        if (empty) {
            qDeleteAll(f.table.keyform_bindings); f.table.keyform_bindings.clear();
            qDeleteAll(f.table.parameter_binding_indices); f.table.parameter_binding_indices.clear();
            qDeleteAll(f.table.parameter_bindings); f.table.parameter_bindings.clear(); f.table.keys->values.clear();
            for (auto* parameter : f.table.parameters) { parameter->parameter_binding_sources_begin_indices = -1; parameter->parameter_binding_sources_counts = 0; }
            for (auto* d : f.table.deformers) d->keyform_binding_sources_indices = -1;
            for (auto* m : f.table.art_meshes) m->keyform_binding_sources_indices = -1;
        }
        if (type == "part") part->keyform_binding_sources_indices = binding;
        else if (type == "artmesh") f.table.art_meshes[0]->keyform_binding_sources_indices = binding;
        else f.table.deformers[type == "warp" ? 1 : 0]->keyform_binding_sources_indices = binding;
        const bool valid = binding == -1 || (binding == 0 && (!empty || type == "part"));
        QString error; QCOMPARE(query.Build(f.table, &error), valid);
        Query::Result after; QVERIFY(query.Query({{"artmesh", "Mesh"}}, after));
        if (!valid) { QVERIFY(!error.isEmpty()); QCOMPARE(after.all, before.all); }
        else if (empty) QVERIFY(after.all.isEmpty());
    }

    void partialOrdinaryPoolsDoNotMakePartZeroAnUnboundSentinel_data()
    {
        QTest::addColumn<int>("pool"); for (int pool = 0; pool < 3; ++pool) QTest::newRow(qPrintable(QString::number(pool))) << pool;
    }
    void partialOrdinaryPoolsDoNotMakePartZeroAnUnboundSentinel()
    {
        QFETCH(int, pool); Fixture f;
        auto* part = record<CParts>(); part->ids = "Part"; part->parent_part_indices = -1;
        part->keyform_binding_sources_indices = 0; f.table.parts.append(part);
        qDeleteAll(f.table.keyform_bindings); f.table.keyform_bindings.clear();
        if (pool != 0) { qDeleteAll(f.table.parameter_bindings); f.table.parameter_bindings.clear(); }
        if (pool != 1) { qDeleteAll(f.table.parameter_binding_indices); f.table.parameter_binding_indices.clear(); }
        if (pool != 2) f.table.keys->values.clear();
        Query query; QString error; QVERIFY(!query.Build(f.table, &error)); QVERIFY(error.contains("部件"));
    }

    void descendantDeformerAxesAndAllAncestorsAreCollectedOnce()
    {
        Fixture f; QVERIFY(f.table.IsValid());
        const int other = f.ordinary("Other");
        f.table.art_meshes[0]->keyform_binding_sources_indices = -1;
        f.table.deformers[1]->keyform_binding_sources_indices = 0;
        f.table.deformers[2]->keyform_binding_sources_indices = other;
        Query query; QString error; QVERIFY2(query.Build(f.table, &error), qPrintable(error));
        for (const auto& target : QVector<Query::Target>{{"rotation", "Root"}, {"warp", "Warp"}, {"rotation", "Child"}, {"artmesh", "Mesh"}}) {
            Query::Result result; QVERIFY(query.Query({target, target}, result));
            QCOMPARE(result.ordinary, (QStringList{"P", "Other"})); QCOMPARE(result.all, result.ordinary);
            QVERIFY(result.blendDrivers.isEmpty()); QVERIFY(result.blendLimiters.isEmpty());
        }
    }

    void typedPartMembershipDoesNotExpandAncestorsIntoSiblings()
    {
        Fixture f; const int other = f.ordinary("Other");
        auto* part = record<CParts>(); part->ids = "Mesh"; part->parent_part_indices = -1; part->keyform_binding_sources_indices = other; f.table.parts.append(part);
        f.table.art_meshes[0]->parent_part_indices = 0;
        auto* sibling = new CDeformers(*f.table.deformers[0]); sibling->ids = "Sibling"; sibling->parent_deformer_indices = -1; sibling->parent_part_indices = -1;
        sibling->keyform_binding_sources_indices = f.ordinary("Unrelated"); f.table.deformers.append(sibling);
        Query query; QVERIFY(query.Build(f.table)); Query::Result result;
        QVERIFY(query.Query({{"part", "Mesh"}}, result)); QCOMPARE(result.all, (QStringList{"P", "Other"}));
        QVERIFY(query.Query({{"artmesh", "Mesh"}}, result)); QCOMPARE(result.all, (QStringList{"P", "Other"}));
        QVERIFY(query.Query({{"rotation", "Sibling"}}, result)); QCOMPARE(result.all, (QStringList{"Unrelated"}));
        QVERIFY(!query.Query({{"warp", "Mesh"}}, result)); QCOMPARE(result.all, (QStringList{"Unrelated"}));
    }

    void blendDriversAndLimitersAreSeparateFromOrdinaryDragAxes_data()
    {
        QTest::addColumn<QString>("type"); QTest::addColumn<QString>("id");
        QTest::newRow("mesh") << QString("artmesh") << QString("Mesh");
        QTest::newRow("warp") << QString("warp") << QString("Warp");
        QTest::newRow("rotation") << QString("rotation") << QString("Root");
        QTest::newRow("part") << QString("part") << QString("Part");
    }
    void blendDriversAndLimitersAreSeparateFromOrdinaryDragAxes()
    {
        QFETCH(QString, type); QFETCH(QString, id); Fixture f; f.blend(type);
        f.table.art_meshes[0]->keyform_binding_sources_indices = -1;
        Query query; QString error; QVERIFY2(query.Build(f.table, &error), qPrintable(error)); Query::Result result;
        QVERIFY(query.Query({{type, id}}, result)); QVERIFY(result.ordinary.isEmpty());
        QCOMPARE(result.blendDrivers, (QStringList{"Driver"})); QCOMPARE(result.blendLimiters, (QStringList{"P"}));
        QCOMPARE(result.all, (QStringList{"P", "Driver"}));
        if (type == "rotation") { QVERIFY(query.Query({{"artmesh", "Mesh"}}, result)); QCOMPARE(result.blendDrivers, (QStringList{"Driver"})); }
    }

    void effectiveParentAndMetadataUseTheLoadedSnapshot()
    {
        QTemporaryDir dir; QVERIFY(dir.isValid()); DeformerSnapshotData::Fixture fixture; fixture.BindRoot(false);
        const auto path = dir.filePath("model.moc3"); QVERIFY(DeformerSnapshotData::write(path, fixture.bytes));
        CModelDocument document; QString error; QVERIFY2(document.Load(path, &error), qPrintable(error));
        Query query; QVERIFY2(query.Build(document, &error), qPrintable(error)); Query::Result result;
        QVERIFY(query.Query({{"artmesh", "Mesh"}}, result)); QCOMPARE(result.all, (QStringList{"P"}));
        const auto* parameters = query.Parameters().constData(); QVERIFY(document.SetParameterValue("P", .5)); QVERIFY(QFile::remove(path));
        QVERIFY(query.Build(document)); QCOMPARE(query.Parameters().constData(), parameters);
        auto metadata = document.ParameterMetadata("P"); metadata.minimum = -1; metadata.maximum = 2; metadata.defaultValue = .25;
        QVERIFY(document.SetParameterMetadata("P", metadata)); QVERIFY(query.Build(document));
        QCOMPARE(query.Parameters()[0].minimum, -1.f); QCOMPARE(query.Parameters()[0].maximum, 2.f); QCOMPARE(query.Parameters()[0].defaultValue, .25f);
        QVERIFY(document.SetDeformerParentIndex("Child", -1)); QVERIFY(query.Build(document));
        QVERIFY(query.Query({{"artmesh", "Mesh"}}, result)); QVERIFY(result.all.isEmpty());
        QVERIFY(document.SetDeformerParentIndex("Child", 1)); QVERIFY(query.Build(document));
        QVERIFY(query.Query({{"artmesh", "Mesh"}}, result)); QCOMPARE(result.all, (QStringList{"P"}));
    }

    void failedBuildAndQueryKeepPriorResults()
    {
        Fixture f; Query query; QVERIFY(query.Build(f.table)); Query::Result result;
        QVERIFY(query.Query({{"artmesh", "Mesh"}}, result)); QCOMPARE(result.all, (QStringList{"P"}));
        QVERIFY(!query.Query({{"artmesh", "missing"}}, result)); QCOMPARE(result.all, (QStringList{"P"}));
        f.table.deformers[0]->parent_deformer_indices = 2;
        QVERIFY(!query.Build(f.table)); QVERIFY(query.Query({{"artmesh", "Mesh"}}, result)); QCOMPARE(result.all, (QStringList{"P"}));
        QVERIFY(query.Query({}, result)); QVERIFY(result.all.isEmpty());
    }

    void invalidOrTypeOneOrdinaryAxisIsNotSentToDrag()
    {
        Fixture f; f.table.parameters[0]->parameter_types = 1;
        Query query; QVERIFY(query.Build(f.table)); Query::Result result;
        QVERIFY(query.Query({{"artmesh", "Mesh"}}, result)); QVERIFY(result.ordinary.isEmpty()); QVERIFY(!result.diagnostics.isEmpty());
        f.table.parameters[0]->parameter_types = 0; f.table.keys->values[1] = std::numeric_limits<float>::quiet_NaN();
        QVERIFY(query.Build(f.table)); QVERIFY(query.Query({{"artmesh", "Mesh"}}, result)); QVERIFY(result.ordinary.isEmpty());
    }

    void invalidUnrelatedMetadataDoesNotHideUsableObjects()
    {
        Fixture f; const int broken = f.ordinary("Broken");
        f.table.parameters.last()->max_values = std::numeric_limits<float>::quiet_NaN();
        Query query; QVERIFY(query.Build(f.table)); Query::Result result;
        QVERIFY(query.Query({{"artmesh", "Mesh"}}, result)); QCOMPARE(result.all, (QStringList{"P"}));
        f.table.deformers[0]->keyform_binding_sources_indices = broken;
        QVERIFY(query.Build(f.table)); QVERIFY(query.Query({{"artmesh", "Mesh"}}, result));
        QCOMPARE(result.all, (QStringList{"P"})); QVERIFY(!result.diagnostics.isEmpty());
    }

    void glueIsDiagnosedWithoutExpandingOtherMeshParameters()
    {
        Fixture f; auto* glue = record<CGlue>(); glue->art_mesh_indices_a = glue->art_mesh_indices_b = 0; f.table.glue.append(glue);
        Query query; QVERIFY(query.Build(f.table)); Query::Result result; QVERIFY(query.Query({{"artmesh", "Mesh"}}, result));
        QCOMPARE(result.all, (QStringList{"P"})); QVERIFY(result.diagnostics.join('\n').contains("Glue"));
    }

    void deepSharedBindingsDoNotRequireRecursiveTraversal()
    {
        Fixture f;
        for (int at = 3; at < 12000; ++at) {
            auto* deformer = new CDeformers(*f.table.deformers[2]); deformer->ids = QString("Deep%1").arg(at);
            deformer->parent_deformer_indices = at - 1; deformer->keyform_binding_sources_indices = 0; f.table.deformers.append(deformer);
        }
        f.table.art_meshes[0]->parent_deformer_indices = f.table.deformers.size() - 1;
        Query query; QVERIFY(query.Build(f.table)); Query::Result result;
        QVERIFY(query.Query({{"rotation", "Root"}, {"artmesh", "Mesh"}}, result)); QCOMPARE(result.all, (QStringList{"P"}));
    }

    void realSamplesBuildAndQueryAllTypedObjects_data()
    {
        QTest::addColumn<QString>("name"); for (const auto& name : {"Mark", "Wanko", "Rice", "Natori"}) QTest::newRow(name) << QString(name);
    }
    void realSamplesBuildAndQueryAllTypedObjects()
    {
        QFETCH(QString, name); CModelDocument document; QString error;
        QVERIFY2(document.Load(DeformerSnapshotData::sample(name), &error), qPrintable(error)); Query query;
        QVERIFY2(query.Build(document, &error), qPrintable(error)); QVector<Query::Target> selected;
        for (const auto& d : document.Drawables()) selected.append({"artmesh", d.id});
        for (const auto& d : document.Deformers()) selected.append({d.type == CModelDeformer::Warp ? "warp" : "rotation", d.id});
        for (const auto& p : document.Parts()) selected.append({"part", p.id});
        Query::Result result; QVERIFY2(query.Query(selected, result, &error), qPrintable(error));
        QVERIFY(!result.all.isEmpty()); QCOMPARE(result.all.size(), QSet<QString>(result.all.cbegin(), result.all.cend()).size());
    }
};
QTEST_GUILESS_MAIN(ParameterSelectionQueryTests)
#include "parameter_selection_query_tests.moc"
