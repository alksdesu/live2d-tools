#include "live2dstruct/cmocruntimebuilder.h"
#include "live2dstruct/csectiontable.h"
#include "live2dstruct/csectionvalidation.h"
#include "real_core_model_fixture.h"
#include <QtEndian>
#include <QSet>

using namespace live2dstruct;
using RuntimeBuilder = CMocRuntimeBuilder;

namespace {
void putFloat(QByteArray& bytes, int offset, float value)
{
    quint32 bits;
    std::memcpy(&bits, &value, sizeof(bits));
    qToLittleEndian(bits, bytes.data() + offset);
}
void putInt(QByteArray& bytes, int offset, int value) { qToLittleEndian(qint32(value), bytes.data() + offset); }
int drawable(csmModel* model, const QString& id)
{
    for (int index = 0; index < csmGetDrawableCount(model); ++index)
        if (QString::fromUtf8(csmGetDrawableIds(model)[index]) == id) return index;
    return -1;
}
bool validOrder(csmModel* model)
{
    const int size = csmGetDrawableCount(model);
    QVector<int> order(csmGetDrawableRenderOrders(model), csmGetDrawableRenderOrders(model) + size);
    std::sort(order.begin(), order.end());
    for (int index = 0; index < size; ++index) if (order[index] != index) return false;
    return true;
}
}

class RuntimeMocCoreTests : public QObject
{
    Q_OBJECT
private slots:
    void localOpacityUsesCoreParentAndBlendShapeEvaluation_data()
    {
        QTest::addColumn<QString>("path");
        for (const auto& model : models()) QTest::newRow(qPrintable(model.first)) << model.second;
    }
    void localOpacityUsesCoreParentAndBlendShapeEvaluation()
    {
        QFETCH(QString, path);
        const auto bytes = read(path);
        auto reader = CReader::FromBytes(bytes); CSectionTable table(reader);
        QVERIFY(table.IsValid());
        int target = 0;
        if (!table.blend_shapes_art_meshes.isEmpty()) {
            bool found = false;
            for (const auto* shape : table.blend_shapes_art_meshes) {
                for (int b = shape->blend_shape_keyform_binding_sources_begin_indices;
                     b < shape->blend_shape_keyform_binding_sources_begin_indices + shape->blend_shape_keyform_binding_sources_counts; ++b) {
                    const auto* binding = table.blend_shape_keyform_bindings[b];
                    for (int k = binding->keyform_sources_blend_shape_indices;
                         k < binding->keyform_sources_blend_shape_indices + binding->keyform_sources_blend_shape_counts; ++k)
                        if (table.art_mesh_keyforms[k]->opacities != 0) { target = shape->target_indices; found = true; break; }
                    if (found) break;
                }
                if (found) break;
            }
            QVERIFY(found);
        }
        const auto* mesh = table.art_meshes[target];
        auto expectedBytes = bytes;
        for (int row = mesh->keyform_sources_begin_indices; row < mesh->keyform_sources_begin_indices + mesh->keyform_sources_counts; ++row)
            putFloat(expectedBytes, table.sectionOffsetTable->art_mesh_keyforms->opacities + row * 4, .4f);
        RuntimeBuilder::Overrides overrides; overrides.drawableOpacity.insert(mesh->ids, .4f);
        QByteArray result; QString error;
        QVERIFY2(RuntimeBuilder::Build(bytes, overrides, result, &error), qPrintable(error));
        CoreModel expected, actual;
        QVERIFY(expected.load(expectedBytes)); QVERIFY(actual.load(result));
        for (const auto& pose : poses(expected.value)) {
            expected.pose(pose); actual.pose(pose);
            const auto a = frame(expected.value), b = frame(actual.value);
            QVERIFY2(changed(a, b).isEmpty(), qPrintable(difference(a, b)));
        }
        QVERIFY(expected.intact()); QVERIFY(actual.intact());
    }

    void parentOpacityDoesNotBecomeAFinalRendererOverride()
    {
        auto bytes = read(QString(TEST_RESOURCES) + "/Mark/Mark.moc3");
        auto reader = CReader::FromBytes(bytes); CSectionTable table(reader);
        int target = -1;
        for (int index = 0; index < table.art_meshes.size(); ++index)
            if (table.art_meshes[index]->parent_deformer_indices >= 0) { target = index; break; }
        QVERIFY(target >= 0);
        const auto* mesh = table.art_meshes[target];
        int parent = mesh->parent_deformer_indices;
        bool first = true;
        while (parent >= 0) {
            const auto* deformer = table.deformers[parent];
            const auto* warp = deformer->types == 0 ? table.warp_deformers[deformer->specific_sources_indices] : nullptr;
            const auto* rotation = deformer->types == 1 ? table.rotation_deformers[deformer->specific_sources_indices] : nullptr;
            const int begin = warp ? warp->keyform_sources_begin_indices : rotation->keyform_sources_begin_indices;
            const int count = warp ? warp->keyform_sources_counts : rotation->keyform_sources_counts;
            const int offset = warp ? table.sectionOffsetTable->warp_deformer_keyforms->opacities : table.sectionOffsetTable->rotation_deformer_keyforms->opacities;
            for (int row = begin; row < begin + count; ++row) putFloat(bytes, offset + row * 4, first ? .25f : 1.f);
            first = false; parent = deformer->parent_deformer_indices;
        }
        RuntimeBuilder::Overrides overrides; overrides.drawableOpacity.insert(mesh->ids, .4f);
        QByteArray result; QString error;
        QVERIFY2(RuntimeBuilder::Build(bytes, overrides, result, &error), qPrintable(error));
        CoreModel model; QVERIFY(model.load(result)); model.pose(poses(model.value).first());
        const int index = drawable(model.value, mesh->ids); QVERIFY(index >= 0);
        QVERIFY(std::abs(csmGetDrawableOpacities(model.value)[index] - .1f) < 1e-6f);
        QVERIFY(csmGetDrawableOpacities(model.value)[index] != .4f);
        QVERIFY(model.intact());
    }

    void topologyMatchesIndependentCoreIndices_data()
    {
        QTest::addColumn<QString>("path"); QTest::addColumn<int>("count");
        for (const auto& model : models()) for (int count : {0, 3})
            QTest::newRow(qPrintable(model.first + QString::number(count))) << model.second << count;
    }
    void topologyMatchesIndependentCoreIndices()
    {
        QFETCH(QString, path); QFETCH(int, count);
        const auto bytes = read(path);
        auto reader = CReader::FromBytes(bytes); CSectionTable table(reader);
        QVERIFY(table.IsValid()); const auto* mesh = table.art_meshes.first();
        QVERIFY(mesh->position_index_sources_counts > 3);
        QVector<quint16> indices;
        for (int index = 0; index < count; ++index) indices.append(quint16(table.position_indices->indices[mesh->position_index_sources_begin_indices + index]));
        auto expectedBytes = bytes;
        putInt(expectedBytes, table.sectionOffsetTable->art_meshes->position_index_sources_counts, count);
        RuntimeBuilder::Overrides overrides; overrides.drawableIndices.insert(mesh->ids, indices);
        QByteArray sourceBytes = bytes, result; QString error;
        if (!count) {
            result = "unchanged";
            QVERIFY(!RuntimeBuilder::Build(bytes, overrides, result, &error)); QCOMPARE(result, QByteArray("unchanged"));
            sourceBytes = expectedBytes;
            CoreModel emptySource; QVERIFY(emptySource.load(sourceBytes));
            overrides.drawableIndices.clear(); overrides.drawableOpacity.insert(mesh->ids, .4f);
            for (int row = mesh->keyform_sources_begin_indices; row < mesh->keyform_sources_begin_indices + mesh->keyform_sources_counts; ++row)
                putFloat(expectedBytes, table.sectionOffsetTable->art_mesh_keyforms->opacities + row * 4, .4f);
        }
        QVERIFY2(RuntimeBuilder::Build(sourceBytes, overrides, result, &error), qPrintable(error));
        CoreModel expected, actual; QVERIFY(expected.load(expectedBytes)); QVERIFY(actual.load(result));
        for (const auto& pose : poses(expected.value)) {
            expected.pose(pose); actual.pose(pose);
            const auto a = frame(expected.value), b = frame(actual.value);
            QVERIFY2(changed(a, b).isEmpty(), qPrintable(difference(a, b)));
        }
        QVERIFY(expected.intact()); QVERIFY(actual.intact());
    }

    void drawOrderUpdatesOnlyItsGroupAndMatchesCore_data()
    {
        QTest::addColumn<QString>("path"); QTest::addColumn<int>("order"); QTest::addColumn<int>("group");
        for (const auto& model : models()) for (int order : {0, 1000})
            QTest::newRow(qPrintable(model.first + '-' + QString::number(order))) << model.second << order << 0;
        QTest::newRow("Rice-nested-group") << QString(TEST_RESOURCES) + "/Rice/Rice.moc3" << 0 << 1;
    }
    void drawOrderUpdatesOnlyItsGroupAndMatchesCore()
    {
        QFETCH(QString, path); QFETCH(int, order); QFETCH(int, group);
        const auto bytes = read(path); auto reader = CReader::FromBytes(bytes); CSectionTable table(reader);
        QVERIFY(table.IsValid()); QVERIFY(group < table.draw_order_groups.size());
        const auto* definition = table.draw_order_groups[group];
        int target = -1;
        for (int row = definition->object_sources_begin_indices; row < definition->object_sources_begin_indices + definition->object_sources_counts; ++row)
            if (table.draw_order_group_objects[row]->types == 0) { target = table.draw_order_group_objects[row]->indices; break; }
        QVERIFY(target >= 0); const auto* mesh = table.art_meshes[target];
        auto expectedBytes = bytes;
        for (int row = mesh->keyform_sources_begin_indices; row < mesh->keyform_sources_begin_indices + mesh->keyform_sources_counts; ++row)
            putFloat(expectedBytes, table.sectionOffsetTable->art_mesh_keyforms->draw_orders + row * 4, order);
        putInt(expectedBytes, table.sectionOffsetTable->draw_order_groups->minimum_draw_orders + group * 4, std::min(order, definition->minimum_draw_orders));
        putInt(expectedBytes, table.sectionOffsetTable->draw_order_groups->maximum_draw_orders + group * 4, std::max(order, definition->maximum_draw_orders));
        RuntimeBuilder::Overrides overrides; overrides.drawableDrawOrder.insert(mesh->ids, order);
        QByteArray result; QString error;
        QVERIFY2(RuntimeBuilder::Build(bytes, overrides, result, &error), qPrintable(error));
        auto changedReader = CReader::FromBytes(result); CSectionTable after(changedReader);
        QVERIFY(after.IsValid());
        for (int index = 0; index < table.draw_order_groups.size(); ++index) if (index != group) {
            QCOMPARE(after.draw_order_groups[index]->minimum_draw_orders, table.draw_order_groups[index]->minimum_draw_orders);
            QCOMPARE(after.draw_order_groups[index]->maximum_draw_orders, table.draw_order_groups[index]->maximum_draw_orders);
        }
        CoreModel expected, actual; QVERIFY(expected.load(expectedBytes)); QVERIFY(actual.load(result));
        for (const auto& pose : poses(expected.value)) {
            expected.pose(pose); actual.pose(pose);
            QVERIFY(validOrder(actual.value));
            const auto a = frame(expected.value), b = frame(actual.value);
            QVERIFY2(changed(a, b).isEmpty(), qPrintable(difference(a, b)));
        }
        QVERIFY(expected.intact()); QVERIFY(actual.intact());
    }

    void visibilityKeepsAllMaskGeometryAndZerosBlendShapeAlpha_data() { localOpacityUsesCoreParentAndBlendShapeEvaluation_data(); }
    void visibilityKeepsAllMaskGeometryAndZerosBlendShapeAlpha()
    {
        QFETCH(QString, path);
        const auto bytes = read(path); auto reader = CReader::FromBytes(bytes); CSectionTable table(reader);
        QVERIFY(table.IsValid());
        int target = 0;
        for (const auto* shape : table.blend_shapes_art_meshes) {
            bool nonzero = false;
            for (int b = shape->blend_shape_keyform_binding_sources_begin_indices;
                 b < shape->blend_shape_keyform_binding_sources_begin_indices + shape->blend_shape_keyform_binding_sources_counts; ++b) {
                const auto* binding = table.blend_shape_keyform_bindings[b];
                for (int k = binding->keyform_sources_blend_shape_indices;
                     k < binding->keyform_sources_blend_shape_indices + binding->keyform_sources_blend_shape_counts; ++k)
                    nonzero = nonzero || table.art_mesh_keyforms[k]->opacities != 0;
            }
            if (nonzero) { target = shape->target_indices; break; }
        }
        const auto id = table.art_meshes[target]->ids;
        RuntimeBuilder::Overrides overrides; overrides.visibility.insert(QStringLiteral("artmesh|") + id, false);
        QByteArray result; QString error;
        QVERIFY2(RuntimeBuilder::Build(bytes, overrides, result, &error), qPrintable(error));
        CoreModel before, actual; QVERIFY(before.load(bytes)); QVERIFY(actual.load(result));
        const int index = drawable(before.value, id); QVERIFY(index >= 0);
        bool observedVisible = false;
        for (const auto& pose : poses(before.value)) {
            before.pose(pose); actual.pose(pose);
            auto a = frame(before.value); const auto b = frame(actual.value);
            const int opacity = a.positionFields[index];
            observedVisible = observedVisible || a.values[index][opacity] > 0;
            a.values[index][opacity] = 0;
            QVERIFY2(changed(a, b).isEmpty(), qPrintable(difference(a, b)));
            QCOMPARE(csmGetDrawableOpacities(actual.value)[index], 0.f);
        }
        QVERIFY(observedVisible); QVERIFY(before.intact()); QVERIFY(actual.intact());
    }

    void sharedOwnersRemainIndependent_data()
    {
        QTest::addColumn<QString>("path"); QTest::addColumn<bool>("blend");
        for (const auto& model : models()) {
            QTest::newRow(qPrintable(model.first + "-normal")) << model.second << false;
            auto reader = CReader::FromBytes(read(model.second)); CSectionTable table(reader);
            if (!table.blend_shapes_art_meshes.isEmpty())
                QTest::newRow(qPrintable(model.first + "-blend")) << model.second << true;
        }
    }
    void sharedOwnersRemainIndependent()
    {
        QFETCH(QString, path); QFETCH(bool, blend);
        auto bytes = read(path); auto reader = CReader::FromBytes(bytes); CSectionTable table(reader);
        QVERIFY(table.IsValid());
        int first = -1, second = -1;
        if (blend) {
            for (int a = 0; a < table.blend_shapes_art_meshes.size() && first < 0; ++a) {
                const auto* shape = table.blend_shapes_art_meshes[a];
                bool nonzero = false;
                for (int b = shape->blend_shape_keyform_binding_sources_begin_indices;
                     b < shape->blend_shape_keyform_binding_sources_begin_indices + shape->blend_shape_keyform_binding_sources_counts; ++b) {
                    const auto* binding = table.blend_shape_keyform_bindings[b];
                    for (int k = binding->keyform_sources_blend_shape_indices;
                         k < binding->keyform_sources_blend_shape_indices + binding->keyform_sources_blend_shape_counts; ++k)
                        nonzero = nonzero || table.art_mesh_keyforms[k]->opacities != 0;
                }
                if (!nonzero) continue;
                for (int b = a + 1; b < table.blend_shapes_art_meshes.size(); ++b) {
                    const auto* sibling = table.blend_shapes_art_meshes[b];
                    if (table.art_meshes[shape->target_indices]->vertex_counts != table.art_meshes[sibling->target_indices]->vertex_counts ||
                        shape->blend_shape_keyform_binding_sources_counts != sibling->blend_shape_keyform_binding_sources_counts) continue;
                    first = shape->target_indices; second = sibling->target_indices;
                    putInt(bytes, table.sectionOffsetTable->blend_shapes_art_meshes->blend_shape_keyform_binding_sources_begin_indices + b * 4,
                           shape->blend_shape_keyform_binding_sources_begin_indices);
                    break;
                }
            }
        } else {
            for (int a = 0; a < table.art_meshes.size() && first < 0; ++a)
                for (int b = a + 1; b < table.art_meshes.size(); ++b)
                    if (table.art_meshes[a]->vertex_counts == table.art_meshes[b]->vertex_counts &&
                        table.art_meshes[a]->keyform_sources_counts == table.art_meshes[b]->keyform_sources_counts) {
                        first = a; second = b;
                        putInt(bytes, table.sectionOffsetTable->art_meshes->keyform_sources_begin_indices + b * 4,
                               table.art_meshes[a]->keyform_sources_begin_indices);
                        break;
                    }
        }
        QVERIFY(first >= 0); QVERIFY(second >= 0);
        const auto id = table.art_meshes[first]->ids;
        RuntimeBuilder::Overrides overrides; overrides.visibility.insert(QStringLiteral("artmesh|") + id, false);
        QByteArray result; QString error;
        QVERIFY2(RuntimeBuilder::Build(bytes, overrides, result, &error), qPrintable(error));
        CoreModel before, actual; QVERIFY(before.load(bytes)); QVERIFY(actual.load(result));
        const int index = drawable(before.value, id); QVERIFY(index >= 0);
        for (const auto& pose : poses(before.value)) {
            before.pose(pose); actual.pose(pose);
            auto a = frame(before.value); const auto b = frame(actual.value);
            a.values[index][a.positionFields[index]] = 0;
            QVERIFY2(changed(a, b).isEmpty(), qPrintable(difference(a, b)));
        }
        QVERIFY(before.intact()); QVERIFY(actual.intact());
    }

    void typedAncestorVisibilityKeepsOtherPartsAndGeometry_data()
    {
        QTest::addColumn<QString>("path"); QTest::addColumn<QString>("type");
        for (const auto& model : models()) for (const char* type : {"part", "warp", "rotation"})
            QTest::newRow(qPrintable(model.first + '-' + type)) << model.second << QString::fromLatin1(type);
    }
    void typedAncestorVisibilityKeepsOtherPartsAndGeometry()
    {
        QFETCH(QString, path); QFETCH(QString, type);
        const auto bytes = read(path); auto reader = CReader::FromBytes(bytes); CSectionTable table(reader);
        QVERIFY(table.IsValid());
        int selected = 0;
        if (type != "part") {
            selected = -1;
            for (int index = 0; index < table.deformers.size(); ++index)
                if (table.deformers[index]->types == (type == "warp" ? 0 : 1)) { selected = index; break; }
        }
        QVERIFY(selected >= 0);
        const auto id = type == "part" ? table.parts[selected]->ids : table.deformers[selected]->ids;
        RuntimeBuilder::Overrides overrides; overrides.visibility.insert(type + '|' + id, false);
        QByteArray result; QString error;
        QVERIFY2(RuntimeBuilder::Build(bytes, overrides, result, &error), qPrintable(error));
        auto expectedBytes = bytes;
        if (type == "part") putInt(expectedBytes, table.sectionOffsetTable->parts->is_visible + selected * 4, 0);
        CoreModel expected, actual; QVERIFY(expected.load(expectedBytes)); QVERIFY(actual.load(result));
        QSet<int> children;
        if (type != "part") for (const auto* mesh : table.art_meshes) {
            int parent = mesh->parent_deformer_indices;
            for (int step = 0; parent >= 0 && step < table.deformers.size(); ++step) {
                if (parent == selected) { children.insert(drawable(expected.value, mesh->ids)); break; }
                parent = table.deformers[parent]->parent_deformer_indices;
            }
        }
        if (type != "part") { QVERIFY(!children.isEmpty()); QVERIFY(!children.contains(-1)); }
        for (const auto& pose : poses(expected.value)) {
            expected.pose(pose); actual.pose(pose);
            auto a = frame(expected.value); const auto b = frame(actual.value);
            for (int child : children) a.values[child][a.positionFields[child]] = 0;
            QVERIFY2(changed(a, b).isEmpty(), qPrintable(difference(a, b)));
        }
        QVERIFY(expected.intact()); QVERIFY(actual.intact());
    }

    void aliasedFlagColumnsAreSeparatedBeforeCoreLoading()
    {
        const auto canonical = read(QString(TEST_RESOURCES) + "/Mark/Mark.moc3");
        auto bytes = canonical;
        auto reader = CReader::FromBytes(bytes); CSectionTable table(reader);
        QVERIFY(table.IsValid());
        const auto spans = CSectionValidation::ArraySpans(*table.sectionOffsetTable, *table.count_info);
        int slot = -1;
        for (const auto& column : spans) if (QByteArray(column.name) == "art_meshes.is_visible") slot = column.slot;
        QVERIFY(slot >= 0);
        QCOMPARE(bytes.mid(table.sectionOffsetTable->art_meshes->is_visible, table.art_meshes.size() * 4),
                 bytes.mid(table.sectionOffsetTable->art_meshes->is_enabled, table.art_meshes.size() * 4));
        putInt(bytes, slot, table.sectionOffsetTable->art_meshes->is_enabled);
        const auto id = table.art_meshes.first()->ids;
        RuntimeBuilder::Overrides overrides; overrides.visibility.insert(QStringLiteral("artmesh|") + id, false);
        QByteArray result; QString error;
        QVERIFY2(RuntimeBuilder::Build(bytes, overrides, result, &error), qPrintable(error));
        auto changedReader = CReader::FromBytes(result); CSectionTable after(changedReader);
        QVERIFY(after.IsValid()); QVERIFY(after.art_meshes.first()->is_enabled);
        QVERIFY(!after.art_meshes.first()->is_visible);
        // Core rejects shared flag columns, so the equal canonical columns provide the independent oracle.
        CoreModel before, actual; QVERIFY(before.load(canonical)); QVERIFY(actual.load(result));
        const int index = drawable(before.value, id); QVERIFY(index >= 0);
        for (const auto& pose : poses(before.value)) {
            before.pose(pose); actual.pose(pose);
            auto a = frame(before.value); const auto b = frame(actual.value);
            a.values[index][a.positionFields[index]] = 0;
            QVERIFY2(changed(a, b).isEmpty(), qPrintable(difference(a, b)));
        }
        QVERIFY(before.intact()); QVERIFY(actual.intact());
    }
};

QTEST_GUILESS_MAIN(RuntimeMocCoreTests)
#include "runtime_moc_core_tests.moc"
