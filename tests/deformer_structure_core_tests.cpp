#include "live2dstruct/cmocdeformerstructurebuilder.h"
#include "live2dstruct/cmocdeformerorder.h"
#include "live2dstruct/cblendshaperelations.h"
#include "deformer_order_fixture.h"
#include "real_core_model_fixture.h"

using namespace live2dstruct;
using Builder = CMocDeformerStructureBuilder;
using Codec = CMocParameterStructureBuilder;
namespace Fixture = DeformerOrderFixture;
namespace {
Builder::InsertRequest neutral(bool rotation, int part = -1)
{
    Builder::InsertRequest request;
    request.id = "NewDeformer"; request.partIndex = part;
    if (rotation) request.shape = Builder::Rotation{};
    else request.shape = Builder::Warp{2, 3, false, {}};
    return request;
}
QByteArray columnBytes(const QByteArray& bytes, const CSectionValidation::ColumnSpan& column)
{
    return column.offset ? bytes.mid(column.offset, column.count * CSectionValidation::StorageWidth(column)) : QByteArray();
}
}

class DeformerStructureCoreTests : public QObject
{
    Q_OBJECT
private slots:
    void warpInterpolationModeChangesRealCoreGeometry_data()
    {
        QTest::addColumn<QString>("path"); QTest::addColumn<bool>("quad"); QTest::addColumn<bool>("version2");
        for (const auto& model : models()) for (bool quad : {false, true})
            QTest::newRow(qPrintable(model.first + (quad ? "-quad" : "-triangles"))) << model.second << quad << false;
        for (bool quad : {false, true})
            QTest::newRow(quad ? "Mark-v2-quad" : "Mark-v2-triangles") << QString(TEST_RESOURCES) + "/Mark/Mark.moc3" << quad << true;
    }
    void warpInterpolationModeChangesRealCoreGeometry()
    {
        QFETCH(QString, path); QFETCH(bool, quad); QFETCH(bool, version2);
        const auto original = read(path);
        auto modelBytes = original;
        if (version2) {
            auto sourceReader = CReader::FromBytes(original); CSectionTable source(sourceReader); QVERIFY(source.IsValid());
            QCOMPARE(source.sectionOffsetTable->mocVersion, 3);
            for (int slot = 0x1d8; slot < CSectionValidation::KnownOffsetEnd(3); slot += 4) QCOMPARE(Fixture::word(original, slot), 0);
            for (const auto* warp : source.warp_deformers) QVERIFY(!warp->is_quad_source);
            modelBytes[4] = 2;
            CoreModel expected, downgraded; QVERIFY(expected.load(original)); QVERIFY(downgraded.load(modelBytes));
            const int count = csmGetParameterCount(expected.value);
            const auto* values = csmGetParameterDefaultValues(expected.value);
            const QVector<float> pose(values, values + count);
            expected.pose(pose); downgraded.pose(pose);
            QVERIFY2(difference(frame(expected.value), frame(downgraded.value)).isEmpty(),
                     qPrintable(difference(frame(expected.value), frame(downgraded.value))));
            QVERIFY(expected.intact()); QVERIFY(downgraded.intact());
        }
        auto reader = CReader::FromBytes(modelBytes); CSectionTable table(reader); QVERIFY(table.IsValid());
        QSet<int> excluded;
        for (const auto* glue : table.glue) { excluded.insert(glue->art_mesh_indices_a); excluded.insert(glue->art_mesh_indices_b); }
        for (const auto* blend : table.blend_shapes_art_meshes) excluded.insert(blend->target_indices);
        int witness = -1;
        for (int index = 0; index < table.art_meshes.size(); ++index)
            if (!excluded.contains(index) && table.art_meshes[index]->vertex_counts > 0 && table.art_meshes[index]->keyform_sources_counts > 0) { witness = index; break; }
        QVERIFY(witness >= 0);
        const auto* mesh = table.art_meshes[witness];
        auto base = modelBytes;
        Fixture::put(base, table.sectionOffsetTable->art_meshes->parent_deformer_indices + witness * 4, -1);
        const float coordinate = .25f; quint32 bits; std::memcpy(&bits, &coordinate, 4);
        for (int index = 0; index < mesh->keyform_sources_counts; ++index) {
            const auto* form = table.art_mesh_keyforms[mesh->keyform_sources_begin_indices + index];
            for (int component = 0; component < mesh->vertex_counts * 2; ++component)
                Fixture::put(base, table.sectionOffsetTable->keyform_positions->xys + (form->keyform_position_sources_begin_indices + component) * 4, bits);
        }
        Builder::Warp shape{2, 2, quad, {}};
        for (int row = 0; row <= 2; ++row) for (int column = 0; column <= 2; ++column)
            shape.positions << float(column) / 2 << float(row) / 2;
        shape.positions[8] += .4f;
        auto request = neutral(false, mesh->parent_part_indices);
        request.shape = shape;
        request.children.append({Builder::Owner::ArtMesh, mesh->ids});
        Builder::Result inserted; QString error;
        if (quad && table.sectionOffsetTable->mocVersion == 1) {
            inserted.bytes = "unchanged"; inserted.state.columns[0x68] = "sentinel"; inserted.impact.id = "old";
            const auto state = inserted.state; const auto source = base;
            QVERIFY(!Builder::Insert(base, request, inserted, &error)); QVERIFY(!error.isEmpty());
            QCOMPARE(inserted.bytes, QByteArray("unchanged")); QVERIFY(inserted.state == state);
            QCOMPARE(inserted.impact.id, QString("old")); QCOMPARE(base, source);
            Builder::Impact impact; impact.id = "before";
            QVERIFY(!Builder::AnalyzeInsert(base, request, impact, &error)); QCOMPARE(impact.id, QString("before"));
            QCOMPARE(read(path), original);
            return;
        }
        QVERIFY2(Builder::Insert(base, request, inserted, &error), qPrintable(error));
        auto insertedReader = CReader::FromBytes(inserted.bytes); CSectionTable added(insertedReader); QVERIFY(added.IsValid());
        QVERIFY(added.sectionOffsetTable->warp_deformers->is_quad_source || !quad);
        QCOMPARE(added.warp_deformers.back()->is_quad_source, quad);
        QByteArray normalized;
        QVERIFY2(CMocDeformerOrder::Normalize(inserted.bytes, normalized, &error), qPrintable(error));
        CoreModel core; QVERIFY(core.load(normalized));
        QCOMPARE(QString::fromUtf8(csmGetDrawableIds(core.value)[witness]), mesh->ids);
        const int count = csmGetParameterCount(core.value);
        for (const float* values : {csmGetParameterDefaultValues(core.value), csmGetParameterMinimumValues(core.value), csmGetParameterMaximumValues(core.value)}) {
            core.pose(QVector<float>(values, values + count));
            const auto* points = csmGetDrawableVertexPositions(core.value)[witness];
            QCOMPARE(csmGetDrawableVertexCounts(core.value)[witness], mesh->vertex_counts);
            const float expectedX = quad ? .35f : .25f;
            for (int vertex = 0; vertex < mesh->vertex_counts; ++vertex) {
                QVERIFY2(std::abs(points[vertex].X - expectedX) < .00001f,
                         qPrintable(QString("vertex %1 x=%2 expected %3").arg(vertex).arg(points[vertex].X, 0, 'g', 9).arg(expectedX, 0, 'g', 9)));
                QVERIFY2(std::abs(points[vertex].Y + .25f) < .00001f,
                         qPrintable(QString("vertex %1 y=%2 expected -0.25").arg(vertex).arg(points[vertex].Y, 0, 'g', 9)));
            }
        }
        QVERIFY(core.intact()); QCOMPARE(read(path), original);
    }
    void unusedNodeInsertRemovePreservesAllDrawables_data()
    {
        QTest::addColumn<QString>("path");
        QTest::addColumn<bool>("rotation");
        QTest::addColumn<int>("part");
        for (const auto& model : models()) for (bool rotation : {false, true}) for (int part : {-1, 0})
            QTest::newRow(qPrintable(model.first + (rotation ? "-rotation" : "-warp") + QString::number(part))) << model.second << rotation << part;
    }
    void unusedNodeInsertRemovePreservesAllDrawables()
    {
        QFETCH(QString, path); QFETCH(bool, rotation); QFETCH(int, part);
        const auto original = read(path);
        const auto request = neutral(rotation, part);
        Builder::Result inserted, removed; QString error;
        QVERIFY2(Builder::Insert(original, request, inserted, &error), qPrintable(error));
        QVERIFY2(Builder::Remove(inserted.bytes, {request.id}, removed, &error), qPrintable(error));
        CoreModel source, appended, deleted;
        QVERIFY(source.load(original)); QVERIFY(appended.load(inserted.bytes)); QVERIFY(deleted.load(removed.bytes));
        for (const auto& pose : poses(source.value)) {
            source.pose(pose); appended.pose(pose); deleted.pose(pose);
            const auto expected = frame(source.value), added = frame(appended.value), final = frame(deleted.value);
            QCOMPARE(added.values.size(), expected.values.size()); QCOMPARE(final.values.size(), expected.values.size());
            QVERIFY2(difference(expected, added).isEmpty(), qPrintable(difference(expected, added)));
            QVERIFY2(difference(expected, final).isEmpty(), qPrintable(difference(expected, final)));
        }
        QVERIFY(source.intact()); QVERIFY(appended.intact()); QVERIFY(deleted.intact());
        for (const auto* bytes : {&inserted.bytes, &removed.bytes}) {
            Codec::ColumnState state; QByteArray replay;
            QVERIFY2(Codec::CaptureColumnState(original, *bytes, state, &error), qPrintable(error));
            QVERIFY2(Codec::BuildColumnState(original, state, replay, &error), qPrintable(error));
            CoreModel restored; QVERIFY(restored.load(replay));
            for (const auto& pose : poses(source.value)) {
                source.pose(pose); restored.pose(pose);
                QVERIFY2(difference(frame(source.value), frame(restored.value)).isEmpty(), qPrintable(difference(frame(source.value), frame(restored.value))));
            }
            QVERIFY(restored.intact());
        }
        QCOMPARE(read(path), original);
    }

    void insertedNodeReparentThenRemoveRestoresSource_data()
    {
        QTest::addColumn<QString>("path"); QTest::addColumn<bool>("rotation");
        for (const auto& model : models()) for (bool rotation : {false, true})
            QTest::newRow(qPrintable(model.first + (rotation ? "-rotation" : "-warp"))) << model.second << rotation;
    }
    void insertedNodeReparentThenRemoveRestoresSource()
    {
        QFETCH(QString, path); QFETCH(bool, rotation);
        const auto original = read(path);
        auto reader = CReader::FromBytes(original); CSectionTable table(reader); QVERIFY(table.IsValid());
        auto request = neutral(rotation);
        const CArt_meshes* mesh = nullptr;
        for (const auto* candidate : table.art_meshes)
            if (candidate->parent_deformer_indices < 0 || table.deformers[candidate->parent_deformer_indices]->parent_part_indices == candidate->parent_part_indices) { mesh = candidate; break; }
        QVERIFY(mesh);
        request.partIndex = mesh->parent_part_indices;
        if (mesh->parent_deformer_indices >= 0) request.parentId = table.deformers[mesh->parent_deformer_indices]->ids;
        request.children.append({Builder::Owner::ArtMesh, mesh->ids});
        Builder::Result inserted, removed; QString error;
        QVERIFY2(Builder::Insert(original, request, inserted, &error), qPrintable(error));
        QByteArray ordered;
        QVERIFY2(CMocDeformerOrder::Normalize(inserted.bytes, ordered, &error), qPrintable(error));
        CoreModel withParent; QVERIFY(withParent.load(ordered)); QVERIFY(withParent.intact());
        QVERIFY2(Builder::Remove(inserted.bytes, {request.id}, removed, &error), qPrintable(error));
        CoreModel source, restored; QVERIFY(source.load(original)); QVERIFY(restored.load(removed.bytes));
        for (const auto& pose : poses(source.value)) {
            source.pose(pose); restored.pose(pose);
            QVERIFY2(difference(frame(source.value), frame(restored.value)).isEmpty(), qPrintable(difference(frame(source.value), frame(restored.value))));
        }
        QVERIFY(source.intact()); QVERIFY(restored.intact());
    }

    void deletingSharedSpecificOwnerPreservesRemainingOwner_data()
    {
        QTest::addColumn<QString>("path"); QTest::addColumn<bool>("rotation");
        for (const auto& model : models()) for (bool rotation : {false, true})
            QTest::newRow(qPrintable(model.first + (rotation ? "-rotation" : "-warp"))) << model.second << rotation;
    }
    void deletingSharedSpecificOwnerPreservesRemainingOwner()
    {
        QFETCH(QString, path); QFETCH(bool, rotation);
        const auto original = read(path);
        auto reader = CReader::FromBytes(original); CSectionTable table(reader); QVERIFY(table.IsValid());
        int owner = -1, preferred = -1;
        if (rotation && !table.blend_shapes_rotation_deformers.isEmpty()) preferred = table.blend_shapes_rotation_deformers.front()->target_indices;
        if (!rotation && !table.blend_shapes_warp_deformers.isEmpty()) preferred = table.blend_shapes_warp_deformers.front()->target_indices;
        for (int index = 0; index < table.deformers.size(); ++index)
            if (table.deformers[index]->types == int(rotation) && (preferred < 0 || table.deformers[index]->specific_sources_indices == preferred)) { owner = index; break; }
        QVERIFY(owner >= 0);
        const auto request = neutral(rotation);
        Builder::Result inserted, removed; QString error;
        QVERIFY2(Builder::Insert(original, request, inserted, &error), qPrintable(error));
        auto nextReader = CReader::FromBytes(inserted.bytes); CSectionTable next(nextReader); QVERIFY(next.IsValid());
        auto shared = inserted.bytes;
        Fixture::put(shared, next.sectionOffsetTable->deformers->specific_sources_indices + table.deformers.size() * 4, table.deformers[owner]->specific_sources_indices);
        Fixture::put(shared, next.sectionOffsetTable->deformers->keyform_binding_sources_indices + table.deformers.size() * 4, table.deformers[owner]->keyform_binding_sources_indices);
        CoreModel originalCore, sharedCore;
        QVERIFY(originalCore.load(original)); QVERIFY(sharedCore.load(shared));
        QVERIFY2(Builder::Remove(shared, {request.id}, removed, &error), qPrintable(error));
        CoreModel removedCore; QVERIFY(removedCore.load(removed.bytes));
        auto finalReader = CReader::FromBytes(removed.bytes); CSectionTable final(finalReader); QVERIFY(final.IsValid());
        QCOMPARE(final.deformers[owner]->specific_sources_indices, table.deformers[owner]->specific_sources_indices);
        const auto oldColumns = CSectionValidation::ArraySpans(*next.sectionOffsetTable, *next.count_info);
        const auto newColumns = CSectionValidation::ArraySpans(*final.sectionOffsetTable, *final.count_info);
        for (int index = 0; index < oldColumns.size(); ++index)
            if (!QByteArray(oldColumns[index].name).startsWith("deformers."))
                QCOMPARE(columnBytes(removed.bytes, newColumns[index]), columnBytes(shared, oldColumns[index]));
        for (const auto& pose : poses(originalCore.value)) {
            originalCore.pose(pose); sharedCore.pose(pose); removedCore.pose(pose);
            const auto expected = frame(originalCore.value);
            QVERIFY2(difference(expected, frame(sharedCore.value)).isEmpty(), qPrintable(difference(expected, frame(sharedCore.value))));
            QVERIFY2(difference(expected, frame(removedCore.value)).isEmpty(), qPrintable(difference(expected, frame(removedCore.value))));
        }
        QVERIFY(originalCore.intact()); QVERIFY(sharedCore.intact()); QVERIFY(removedCore.intact());
    }

    void removeLastBlendShapeOwnerKeepsSpecificReferencesValid()
    {
        const auto path = qEnvironmentVariable("LIVE2D_REAL_CORE_EXTRA_MOC");
        if (path.isEmpty()) QSKIP("Set LIVE2D_REAL_CORE_EXTRA_MOC to a MOC with Warp and Rotation Blend Shapes");
        const auto original = read(path);
        auto reader = CReader::FromBytes(original); CSectionTable table(reader); QVERIFY(table.IsValid());
        QVERIFY(!table.blend_shapes_warp_deformers.isEmpty()); QVERIFY(!table.blend_shapes_rotation_deformers.isEmpty());
        for (bool rotation : {false, true}) {
            const int specific = rotation ? table.blend_shapes_rotation_deformers.front()->target_indices : table.blend_shapes_warp_deformers.front()->target_indices;
            int owner = -1, ownerCount = 0;
            for (int index = 0; index < table.deformers.size(); ++index)
                if (table.deformers[index]->types == int(rotation) && table.deformers[index]->specific_sources_indices == specific) { owner = index; ++ownerCount; }
            QCOMPARE(ownerCount, 1);
            Builder::Result removed; QString error;
            QVERIFY2(Builder::Remove(original, {table.deformers[owner]->ids}, removed, &error), qPrintable(error));
            auto nextReader = CReader::FromBytes(removed.bytes); CSectionTable next(nextReader); QVERIFY(next.IsValid());
            CBlendShapeRelations beforeRelations, afterRelations;
            QVERIFY2(beforeRelations.Build(table, &error), qPrintable(error));
            QVERIFY2(afterRelations.Build(next, &error), qPrintable(error));
            QCOMPARE(beforeRelations.BlendShapeParameterIndices(), afterRelations.BlendShapeParameterIndices());
            const auto oldColumns = CSectionValidation::ArraySpans(*table.sectionOffsetTable, *table.count_info);
            const auto newColumns = CSectionValidation::ArraySpans(*next.sectionOffsetTable, *next.count_info);
            for (int index = 0; index < oldColumns.size(); ++index) {
                const QByteArray name(oldColumns[index].name);
                if (!name.startsWith("deformers.") && name != "art_meshes.parent_deformer_indices")
                    QCOMPARE(columnBytes(removed.bytes, newColumns[index]), columnBytes(original, oldColumns[index]));
            }
            CoreModel candidate; QVERIFY(candidate.load(removed.bytes));
            for (const auto& pose : poses(candidate.value)) {
                candidate.pose(pose);
                for (const auto& drawable : frame(candidate.value).values) for (float value : drawable) QVERIFY(std::isfinite(value));
            }
            QVERIFY(candidate.intact());
        }
        QCOMPARE(read(path), original);
    }
};

QTEST_GUILESS_MAIN(DeformerStructureCoreTests)
#include "deformer_structure_core_tests.moc"
