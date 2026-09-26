#include "live2dstruct/cmocdeformerstructurebuilder.h"
#include "deformer_order_fixture.h"
#include <QtTest>
#include <limits>

using namespace live2dstruct;
using Builder = CMocDeformerStructureBuilder;
using Codec = CMocParameterStructureBuilder;
namespace Fixture = DeformerOrderFixture;
namespace {
QByteArray payload(const QByteArray& bytes, const CSectionValidation::ColumnSpan& span)
{
    return span.offset ? bytes.mid(span.offset, span.count * CSectionValidation::StorageWidth(span)) : QByteArray();
}
Builder::InsertRequest request(bool rotation)
{
    Builder::InsertRequest value;
    value.id = "InsertedDeformer";
    if (rotation) value.shape = Builder::Rotation{};
    else value.shape = Builder::Warp{2, 3, false, {}};
    return value;
}
Builder::Owner owner(const CDeformers& node)
{
    return {node.types == 0 ? Builder::Owner::Warp : Builder::Owner::Rotation, node.ids};
}
QVector<int> identity(int count)
{
    QVector<int> values(count);
    std::iota(values.begin(), values.end(), 0);
    return values;
}
}

class DeformerStructureBuilderTests : public QObject
{
    Q_OBJECT
private slots:
    void appendPreservesEveryExistingColumn_data()
    {
        QTest::addColumn<QString>("path");
        QTest::addColumn<bool>("rotation");
        QTest::addColumn<int>("part");
        QVector<QPair<QString, QString>> paths;
        for (const QString& name : {"Mark", "Rice", "Natori", "Wanko"})
            paths.append({name, QString(TEST_RESOURCES) + '/' + name + '/' + name + ".moc3"});
        const auto extra = qEnvironmentVariable("LIVE2D_REAL_CORE_EXTRA_MOC");
        if (!extra.isEmpty()) paths.append({"extra", extra});
        for (const auto& item : paths) for (bool rotation : {false, true}) for (int part : {-1, 0})
            QTest::newRow(qPrintable(item.first + (rotation ? "-rotation" : "-warp") + QString::number(part))) << item.second << rotation << part;
    }
    void appendPreservesEveryExistingColumn()
    {
        QFETCH(QString, path); QFETCH(bool, rotation); QFETCH(int, part);
        const auto source = Fixture::read(path);
        auto reader = CReader::FromBytes(source); CSectionTable before(reader); QVERIFY(before.IsValid());
        auto insertion = request(rotation); insertion.partIndex = part;
        Builder::Result result; QString error;
        QVERIFY2(Builder::Insert(source, insertion, result, &error), qPrintable(error));
        auto nextReader = CReader::FromBytes(result.bytes); CSectionTable after(nextReader); QVERIFY(after.IsValid());
        QCOMPARE(after.deformers.size(), before.deformers.size() + 1);
        QCOMPARE(result.impact.insertedIndex, before.deformers.size());
        QCOMPARE(result.impact.oldDeformerToNew, identity(before.deformers.size()));
        QVERIFY(result.impact.reparented.isEmpty());
        const auto* added = after.deformers.back();
        QCOMPARE(added->ids, insertion.id); QCOMPARE(added->types, int(rotation));
        QCOMPARE(added->parent_part_indices, part); QCOMPARE(added->parent_deformer_indices, -1);
        QVERIFY(added->is_visible); QVERIFY(added->is_enabled);
        QVERIFY(added->keyform_binding_sources_indices >= 0);
        QCOMPARE(after.keyform_bindings[added->keyform_binding_sources_indices]->parameter_binding_index_sources_counts, 0);
        const auto oldColumns = CSectionValidation::ArraySpans(*before.sectionOffsetTable, *before.count_info);
        const auto newColumns = CSectionValidation::ArraySpans(*after.sectionOffsetTable, *after.count_info);
        QCOMPARE(oldColumns.size(), newColumns.size());
        for (int index = 0; index < oldColumns.size(); ++index) {
            const auto& old = oldColumns[index]; const auto& current = newColumns[index];
            QCOMPARE(QString(old.name), QString(current.name));
            const auto prior = payload(source, old), next = payload(result.bytes, current);
            QVERIFY2(next.startsWith(prior), old.name);
            if (old.count == current.count && old.offset) QCOMPARE(next, prior);
        }
        QCOMPARE(result.state.columns.value(0x68).size(), after.deformers.size() * 8);
        if (rotation) {
            const auto* specific = after.rotation_deformers[added->specific_sources_indices];
            QCOMPARE(specific->keyform_binding_sources_indices, added->keyform_binding_sources_indices);
            QCOMPARE(specific->keyform_sources_counts, 1); QCOMPARE(specific->base_angles, 0.f);
            const auto* form = after.rotation_deformer_keyforms[specific->keyform_sources_begin_indices];
            QCOMPARE(form->opacities, 1.f); QCOMPARE(form->angles, 0.f); QCOMPARE(form->scales, 1.f);
            QCOMPARE(form->origin_x, 0.f); QCOMPARE(form->origin_y, 0.f);
            QVERIFY(!form->is_reflect_x); QVERIFY(!form->is_reflect_y);
        } else {
            const auto* specific = after.warp_deformers[added->specific_sources_indices];
            QCOMPARE(specific->keyform_binding_sources_indices, added->keyform_binding_sources_indices);
            QCOMPARE(specific->keyform_sources_counts, 1); QCOMPARE(specific->vertex_counts, 12);
            QCOMPARE(specific->rows, 2); QCOMPARE(specific->columns, 3); QCOMPARE(specific->is_quad_source, std::get<Builder::Warp>(insertion.shape).quad);
            const auto* form = after.warp_deformer_keyforms[specific->keyform_sources_begin_indices];
            QCOMPARE(form->opacities, 1.f);
            for (int row = 0; row <= 2; ++row) for (int column = 0; column <= 3; ++column) {
                const int offset = after.sectionOffsetTable->keyform_positions->xys + (form->keyform_position_sources_begin_indices + 2 * (row * 4 + column)) * 4;
                const auto x = qFromLittleEndian<quint32>(result.bytes.constData() + offset), y = qFromLittleEndian<quint32>(result.bytes.constData() + offset + 4);
                float px, py; std::memcpy(&px, &x, 4); std::memcpy(&py, &y, 4);
                QCOMPARE(px, float(column) / 3); QCOMPARE(py, float(row) / 2);
            }
        }
        QByteArray replay;
        QVERIFY2(Codec::BuildColumnState(source, result.state, replay, &error), qPrintable(error));
        QCOMPARE(replay, result.bytes);
        Codec::ColumnState captured;
        QVERIFY2(Codec::CaptureColumnState(source, result.bytes, captured, &error), qPrintable(error));
        QVERIFY2(Codec::BuildColumnState(source, captured, replay, &error), qPrintable(error));
        QCOMPARE(replay, result.bytes);
        Builder::Impact analyzed;
        QVERIFY2(Builder::AnalyzeInsert(source, insertion, analyzed, &error), qPrintable(error));
        QCOMPARE(analyzed.insertedIndex, result.impact.insertedIndex);
        QCOMPARE(analyzed.oldDeformerToNew, result.impact.oldDeformerToNew);
        QCOMPARE(Fixture::read(path), source);
    }

    void removeRemapsEveryParent_data()
    {
        QTest::addColumn<int>("removed");
        for (int index : {0, 5, 14, 27}) QTest::newRow(qPrintable(QString::number(index))) << index;
    }
    void removeRemapsEveryParent()
    {
        QFETCH(int, removed);
        const auto source = Fixture::read(QString(TEST_RESOURCES) + "/Mark/Mark.moc3");
        auto reader = CReader::FromBytes(source); CSectionTable before(reader); QVERIFY(before.IsValid());
        QVERIFY(removed < before.deformers.size());
        Builder::Result result; QString error;
        QVERIFY2(Builder::Remove(source, {before.deformers[removed]->ids}, result, &error), qPrintable(error));
        auto nextReader = CReader::FromBytes(result.bytes); CSectionTable after(nextReader); QVERIFY(after.IsValid());
        QCOMPARE(after.deformers.size(), before.deformers.size() - 1);
        QVector<int> remap;
        for (int index = 0; index < before.deformers.size(); ++index) remap.append(index == removed ? -1 : index - int(index > removed));
        QCOMPARE(result.impact.oldDeformerToNew, remap);
        const auto parent = [&](int prior) {
            if (prior == removed) prior = before.deformers[removed]->parent_deformer_indices;
            return prior < 0 ? -1 : remap[prior];
        };
        QVector<Builder::Owner> children;
        for (int index = 0; index < before.deformers.size(); ++index) {
            if (index == removed) continue;
            const auto* prior = before.deformers[index]; const auto* next = after.deformers[remap[index]];
            QCOMPARE(next->ids, prior->ids); QCOMPARE(next->specific_sources_indices, prior->specific_sources_indices);
            QCOMPARE(next->parent_deformer_indices, parent(prior->parent_deformer_indices));
            if (prior->parent_deformer_indices == removed) children.append(owner(*prior));
        }
        for (int index = 0; index < before.art_meshes.size(); ++index) {
            const auto* prior = before.art_meshes[index];
            QCOMPARE(after.art_meshes[index]->parent_deformer_indices, parent(prior->parent_deformer_indices));
            if (prior->parent_deformer_indices == removed) children.append({Builder::Owner::ArtMesh, prior->ids});
        }
        QVERIFY(result.impact.reparented == children);
        const auto oldColumns = CSectionValidation::ArraySpans(*before.sectionOffsetTable, *before.count_info);
        const auto newColumns = CSectionValidation::ArraySpans(*after.sectionOffsetTable, *after.count_info);
        for (int index = 0; index < oldColumns.size(); ++index)
            if (!QByteArray(oldColumns[index].name).startsWith("deformers.") && QByteArray(oldColumns[index].name) != "art_meshes.parent_deformer_indices")
                QCOMPARE(payload(result.bytes, newColumns[index]), payload(source, oldColumns[index]));
        Builder::Impact analyzed;
        QVERIFY2(Builder::AnalyzeRemove(source, {before.deformers[removed]->ids}, analyzed, &error), qPrintable(error));
        QCOMPARE(analyzed.oldDeformerToNew, remap); QVERIFY(analyzed.reparented == children);
        QByteArray replay;
        QVERIFY2(Codec::BuildColumnState(source, result.state, replay, &error), qPrintable(error));
        QCOMPARE(replay, result.bytes);
    }

    void selectedSiblingsMoveUnderInsertedNode()
    {
        const auto source = Fixture::read(QString(TEST_RESOURCES) + "/Mark/Mark.moc3");
        auto reader = CReader::FromBytes(source); CSectionTable table(reader); QVERIFY(table.IsValid());
        auto insertion = request(true);
        const auto* first = table.art_meshes.front();
        insertion.partIndex = first->parent_part_indices;
        const int parent = first->parent_deformer_indices;
        if (parent >= 0) insertion.parentId = table.deformers[parent]->ids;
        for (const auto* mesh : table.art_meshes)
            if (mesh->parent_part_indices == insertion.partIndex && mesh->parent_deformer_indices == parent)
                insertion.children.append({Builder::Owner::ArtMesh, mesh->ids});
        for (const auto* node : table.deformers)
            if (node->parent_part_indices == insertion.partIndex && node->parent_deformer_indices == parent)
                insertion.children.append(owner(*node));
        QVERIFY(!insertion.children.isEmpty());
        Builder::Result result; QString error;
        QVERIFY2(Builder::Insert(source, insertion, result, &error), qPrintable(error));
        auto nextReader = CReader::FromBytes(result.bytes); CSectionTable next(nextReader); QVERIFY(next.IsValid());
        QVERIFY(result.impact.reparented == insertion.children);
        QCOMPARE(next.deformers.back()->parent_deformer_indices, parent);
        for (int index = 0; index < table.deformers.size(); ++index) {
            const bool selected = insertion.children.contains(owner(*table.deformers[index]));
            QCOMPARE(next.deformers[index]->parent_deformer_indices, selected ? table.deformers.size() : table.deformers[index]->parent_deformer_indices);
        }
        for (int index = 0; index < table.art_meshes.size(); ++index) {
            const bool selected = insertion.children.contains({Builder::Owner::ArtMesh, table.art_meshes[index]->ids});
            QCOMPARE(next.art_meshes[index]->parent_deformer_indices, selected ? table.deformers.size() : table.art_meshes[index]->parent_deformer_indices);
        }
        Builder::Result restored;
        QVERIFY2(Builder::Remove(result.bytes, {insertion.id}, restored, &error), qPrintable(error));
        auto restoredReader = CReader::FromBytes(restored.bytes); CSectionTable back(restoredReader); QVERIFY(back.IsValid());
        for (int index = 0; index < table.deformers.size(); ++index) QCOMPARE(back.deformers[index]->parent_deformer_indices, table.deformers[index]->parent_deformer_indices);
        for (int index = 0; index < table.art_meshes.size(); ++index) QCOMPARE(back.art_meshes[index]->parent_deformer_indices, table.art_meshes[index]->parent_deformer_indices);
    }

    void emptyBindingIsReusedOrAppended_data()
    {
        QTest::addColumn<bool>("missing"); QTest::newRow("reuse") << false; QTest::newRow("append") << true;
    }
    void emptyBindingIsReusedOrAppended()
    {
        QFETCH(bool, missing);
        auto source = Fixture::read(QString(TEST_RESOURCES) + "/Mark/Mark.moc3");
        auto reader = CReader::FromBytes(source); CSectionTable table(reader); QVERIFY(table.IsValid());
        int empty = -1;
        for (int index = 0; index < table.keyform_bindings.size(); ++index) if (!table.keyform_bindings[index]->parameter_binding_index_sources_counts) {
            if (empty < 0) empty = index;
            if (missing) Fixture::put(source, table.sectionOffsetTable->keyform_bindings->parameter_binding_index_sources_counts + index * 4, 1);
        }
        QVERIFY(empty >= 0);
        Builder::Result result; QString error;
        QVERIFY2(Builder::Insert(source, request(true), result, &error), qPrintable(error));
        auto nextReader = CReader::FromBytes(result.bytes); CSectionTable next(nextReader); QVERIFY(next.IsValid());
        QCOMPARE(next.keyform_bindings.size(), table.keyform_bindings.size() + int(missing));
        QCOMPARE(next.deformers.back()->keyform_binding_sources_indices, missing ? table.keyform_bindings.size() : empty);
        const auto* binding = next.keyform_bindings[next.deformers.back()->keyform_binding_sources_indices];
        QCOMPARE(binding->parameter_binding_index_sources_counts, 0);
        if (missing) QCOMPARE(binding->parameter_binding_index_sources_begin_indices, 0);
    }

    void sharedSpecificRowsRemainAfterRemoval()
    {
        auto source = Fixture::read(QString(TEST_RESOURCES) + "/Mark/Mark.moc3");
        auto reader = CReader::FromBytes(source); CSectionTable table(reader); QVERIFY(table.IsValid());
        int first = -1, second = -1;
        for (int index = 0; index < table.deformers.size(); ++index) if (table.deformers[index]->types == 0) {
            if (first < 0) first = index; else { second = index; break; }
        }
        QVERIFY(second >= 0);
        Fixture::put(source, table.sectionOffsetTable->deformers->specific_sources_indices + second * 4, table.deformers[first]->specific_sources_indices);
        Fixture::put(source, table.sectionOffsetTable->deformers->keyform_binding_sources_indices + second * 4, table.deformers[first]->keyform_binding_sources_indices);
        Builder::Result result; QString error;
        QVERIFY2(Builder::Remove(source, {table.deformers[second]->ids}, result, &error), qPrintable(error));
        auto nextReader = CReader::FromBytes(result.bytes); CSectionTable next(nextReader); QVERIFY(next.IsValid());
        QCOMPARE(next.deformers[first]->specific_sources_indices, table.deformers[first]->specific_sources_indices);
        QCOMPARE(next.warp_deformers.size(), table.warp_deformers.size());
        QCOMPARE(next.warp_deformer_keyforms.size(), table.warp_deformer_keyforms.size());
        const auto before = CSectionValidation::ArraySpans(*table.sectionOffsetTable, *table.count_info);
        const auto after = CSectionValidation::ArraySpans(*next.sectionOffsetTable, *next.count_info);
        for (int index = 0; index < before.size(); ++index) if (QByteArray(before[index].name).startsWith("warp_deformer"))
            QCOMPARE(payload(result.bytes, after[index]), payload(source, before[index]));
    }

    void invalidRequestsLeaveOutputUntouched_data()
    {
        QTest::addColumn<int>("fault");
        const char* names[]{"empty-id", "duplicate-id", "long-id", "embedded-null", "invalid-part", "missing-parent", "wrong-parent-part", "missing-child", "wrong-child-kind", "duplicate-child", "wrong-child-parent", "zero-rows", "huge-grid", "short-points", "nan-point", "infinite-rotation", "missing-remove", "self-parent", "bad-mesh-parent", "bad-specific"};
        for (int index = 0; index < int(std::size(names)); ++index) QTest::newRow(names[index]) << index;
    }
    void invalidRequestsLeaveOutputUntouched()
    {
        QFETCH(int, fault);
        auto source = Fixture::read(QString(TEST_RESOURCES) + "/Mark/Mark.moc3");
        auto reader = CReader::FromBytes(source); CSectionTable table(reader); QVERIFY(table.IsValid());
        auto insertion = request(false);
        if (fault == 0) insertion.id.clear();
        if (fault == 1) insertion.id = table.deformers.front()->ids;
        if (fault == 2) insertion.id = QString(64, 'a');
        if (fault == 3) insertion.id = QString("a") + QChar(0) + 'b';
        if (fault == 4) insertion.partIndex = table.parts.size();
        if (fault == 5) insertion.parentId = "Missing";
        if (fault == 6) { insertion.parentId = table.deformers.front()->ids; insertion.partIndex = -1; }
        if (fault == 7) insertion.children = {{Builder::Owner::ArtMesh, "Missing"}};
        if (fault == 8) { const auto* node = table.deformers.front(); insertion.children = {{node->types == 0 ? Builder::Owner::Rotation : Builder::Owner::Warp, node->ids}}; }
        if (fault == 9) { const auto* node = table.deformers.front(); insertion.partIndex = node->parent_part_indices; if (node->parent_deformer_indices >= 0) insertion.parentId = table.deformers[node->parent_deformer_indices]->ids; insertion.children = {owner(*node), owner(*node)}; }
        if (fault == 10) { const auto* mesh = table.art_meshes.front(); QVERIFY(mesh->parent_deformer_indices >= 0); insertion.partIndex = mesh->parent_part_indices; insertion.children = {{Builder::Owner::ArtMesh, mesh->ids}}; }
        if (fault == 11) std::get<Builder::Warp>(insertion.shape).rows = 0;
        if (fault == 12) std::get<Builder::Warp>(insertion.shape).columns = std::numeric_limits<int>::max();
        if (fault == 13) std::get<Builder::Warp>(insertion.shape).positions = {0, 0};
        if (fault == 14) { auto& warp = std::get<Builder::Warp>(insertion.shape); warp.positions.fill(0, 24); warp.positions[5] = std::numeric_limits<float>::quiet_NaN(); }
        if (fault == 15) { Builder::Rotation rotation; rotation.angle = std::numeric_limits<float>::infinity(); insertion.shape = rotation; }
        if (fault == 17) Fixture::put(source, table.sectionOffsetTable->deformers->parent_deformer_indices, 0);
        if (fault == 18) Fixture::put(source, table.sectionOffsetTable->art_meshes->parent_deformer_indices, table.deformers.size());
        if (fault == 19) Fixture::put(source, table.sectionOffsetTable->deformers->specific_sources_indices, 1000000);
        Builder::Result result; result.bytes = "keep"; result.state.columns[77] = "sentinel"; result.state.counts[1] = 123; result.impact.id = "old";
        const auto prior = source; const auto state = result.state; QString error;
        const bool accepted = fault == 16 ? Builder::Remove(source, {"Missing"}, result, &error) : Builder::Insert(source, insertion, result, &error);
        QVERIFY(!accepted); QVERIFY(!error.isEmpty()); QCOMPARE(source, prior); QCOMPARE(result.bytes, QByteArray("keep")); QVERIFY(result.state == state); QCOMPARE(result.impact.id, QString("old"));
        Builder::Impact impact; impact.id = "unchanged";
        const bool analyzed = fault == 16 ? Builder::AnalyzeRemove(source, {"Missing"}, impact, &error) : Builder::AnalyzeInsert(source, insertion, impact, &error);
        QVERIFY(!analyzed); QCOMPARE(impact.id, QString("unchanged"));
    }
};

QTEST_GUILESS_MAIN(DeformerStructureBuilderTests)
#include "deformer_structure_builder_tests.moc"
